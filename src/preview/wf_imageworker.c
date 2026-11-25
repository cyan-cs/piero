#include "wf_imageworker.h"

#include <stdlib.h>
#include <string.h>

#include "../archive/wf_archive.h"
#include "../archive/wf_vpath.h"
#include "../async/wf_workcore.h"
#include "../path/wf_path.h"
#include "../render/wf_sixel.h"
#include "../base/wf_mem.h"

#define CACHE_SLOTS 32u
#define CACHE_BYTES_DEFAULT (64u << 20)
#define TIMEOUT_DEFAULT_MS 12000u
#define GRACE_DEFAULT_MS 150u
#define PASSWORD_MAX 256u

typedef struct
{
	uint32_t gen;
	wchar_t *path;
	WfDecodeKind kind;
	uint32_t box_w, box_h, page;
	uint64_t session;
	wchar_t password[PASSWORD_MAX + 1];
	bool prefetch;
	uint32_t view_w, view_h, pan_x, pan_y, bg_rgb;
	int quality;
	unsigned max_colors;
	bool sixel;
	uint64_t serial;
} Req;

/* What identifies the bytes a cached picture was made from. */
typedef struct
{
	uint64_t volume;
	unsigned char id[16];
	uint64_t size, written;
	uint64_t
	        entry; /* inside an archive: a hash of the entry's name; else 0 */
} FileKey;

typedef struct
{
	bool used;
	uint64_t stamp;
	size_t bytes;
	FileKey key;
	WfDecodeKind kind;
	uint32_t page, box_w, box_h;
	uint64_t session;
	WfPixmap *pm;
	WfDecodeInfo info;
} Slot;

#define ENCODE_SLOTS 4u
#define ENCODE_CACHE_BYTES (16u << 20)
typedef struct
{
	WfPixmap *
	        crop; /* exact input comparison: no hash collision or stale identity */
	WfBuf data;
	uint32_t bg_rgb;
	bool pdf;
	uint64_t session;
	int quality;
	unsigned max_colors;
	uint64_t stamp;
} Encoded;

struct WfImageWorker
{
	WfWorkCore core; /* its stats are the worker half of `stats` */
	HANDLE abandon;
	Req fg, pf;
	bool has_fg, has_pf;
	uint64_t serial; /* of the newest foreground request or cancel */
	uint64_t active_session;
	bool purge_pdf;
	WfImageResult result;
	bool has_result;
	uint64_t result_serial;
	WfImageWorkerStats stats;
	WfImageWorkerOptions opt;
	/* only the worker thread touches these */
	WfDecHost *host;
	bool pdf_bound;
	FileKey pdf_key;
	uint64_t pdf_session;
	wchar_t staged[MAX_PATH * 2];
	Slot cache[CACHE_SLOTS];
	size_t cache_used;
	uint64_t clock;
	Encoded encoded[ENCODE_SLOTS];
	size_t encoded_used;
};

static void pixels_free(WfPixmap *p, bool sensitive)
{
	if (p && sensitive) {
		SecureZeroMemory(p->px, wf_pixmap_bytes(p->w, p->h));
	}
	wf_pixmap_free(p);
}

static void bytes_free(WfBuf *b, bool sensitive)
{
	if (b->data && sensitive) {
		SecureZeroMemory(b->data, b->cap);
	}
	wf_buf_free(b);
}

static void bump(uint64_t *c)
{
	if (UINT64_MAX != *c) {
		(*c)++;
	}
}

static void req_free(Req *r)
{
	free(r->path);
	SecureZeroMemory(r->password, sizeof r->password);
	memset(r, 0, sizeof *r);
}

static void worker_free(void *owner)
{
	WfImageWorker *w = owner;

	wf_dechost_stop(w->host);
	for (size_t i = 0; i < CACHE_SLOTS; i++) {
		pixels_free(w->cache[i].pm, w->cache[i].kind == WF_DKIND_PDF);
	}
	for (size_t i = 0; i < ENCODE_SLOTS; i++) {
		pixels_free(w->encoded[i].crop, w->encoded[i].pdf);
		bytes_free(&w->encoded[i].data, w->encoded[i].pdf);
	}
	req_free(&w->fg);
	req_free(&w->pf);
	pixels_free(w->result.pm, w->result.sensitive);
	w->result.pm = NULL;
	bytes_free(&w->result.sixel, w->result.sensitive);
	CloseHandle(w->abandon);
	free(w);
}

static void worker_release(WfImageWorker *w)
{
	wf_wc_release(&w->core, worker_free, w);
}

#define UNSAFE_ATTRS                                                           \
	(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |               \
	        FILE_ATTRIBUTE_RECALL_ON_OPEN |                                \
	        FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS |                         \
	        FILE_ATTRIBUTE_DIRECTORY)

typedef enum
{
	OPEN_OK,
	OPEN_REFUSED, /* policy: a link, a placeholder, a remote or moved file */
	OPEN_FAILED   /* the system said no; os_error says why */
} OpenResult;

/* Drive types a picture may be read from: what the text preview allows. */
static bool local_drive(const wchar_t *extended)
{
	wchar_t root[4];
	UINT type;

	if (wcslen(extended) < 7 || L':' != extended[5] ||
	        L'\\' != extended[6]) {
		return false;
	}
	root[0] = extended[4];
	root[1] = L':';
	root[2] = L'\\';
	root[3] = L'\0';
	type = GetDriveTypeW(root);
	return DRIVE_FIXED == type || DRIVE_REMOVABLE == type ||
	        DRIVE_RAMDISK == type;
}

#define MAX_PINS 64

/* Requires a successful lookup with a volume-GUID prefix. */
static OpenResult check_guid_path(HANDLE file, DWORD *os_error)
{
	wchar_t *path;
	DWORD got,
	        need = GetFinalPathNameByHandleW(
	                file, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
	OpenResult result;

	if (!need || need > 32768) {
		*os_error = need ? ERROR_FILENAME_EXCED_RANGE : GetLastError();
		return OPEN_FAILED;
	}
	path = wf_malloc_array((size_t)need, sizeof *path);
	if (!path) {
		*os_error = ERROR_NOT_ENOUGH_MEMORY;
		return OPEN_FAILED;
	}
	got = GetFinalPathNameByHandleW(
	        file, path, need, FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
	if (!got || got >= need) {
		*os_error = got ? ERROR_FILENAME_EXCED_RANGE : GetLastError();
		result = OPEN_FAILED;
	} else {
		result = wcsncmp(path, L"\\\\?\\Volume{", 11) ? OPEN_REFUSED
		                                              : OPEN_OK;
	}
	free(path);
	return result;
}

/*
 * The same policy as the text preview, in one place. Every component of the
 * path, from the drive root to the file, is opened first and kept open: none
 * may be a reparse point, placeholder or the wrong kind of object, and while
 * they are held none can be renamed or deleted under us. The data handle is
 * then opened and must be the very object the last pin names (so a file that
 * was swapped in between is not the one read), on a volume that has a GUID
 * path (not a network or substituted-away drive).
 */
static OpenResult open_input(
        const wchar_t *path, HANDLE *out, FileKey *key, DWORD *os_error)
{
	wchar_t *ext = NULL;
	HANDLE pins[MAX_PINS], h = INVALID_HANDLE_VALUE;
	size_t npins = 0, n;
	FILE_ATTRIBUTE_TAG_INFO tag;
	FILE_ID_INFO id, pinned;
	FILE_BASIC_INFO basic;
	LARGE_INTEGER size;
	OpenResult result = OPEN_FAILED;

	*out = INVALID_HANDLE_VALUE;
	*os_error = 0;
	if (WF_OK != wf_path_to_extended(path, &ext)) {
		*os_error = ERROR_INVALID_NAME;
		return OPEN_FAILED;
	}
	if (!local_drive(ext)) {
		result = OPEN_REFUSED;
		goto done;
	}
	n = wcslen(ext);
	for (size_t i = 7; i <= n; i++) {
		wchar_t saved;
		HANDLE pin;
		bool is_file = i == n;

		if (7 != i && !is_file && L'\\' != ext[i]) {
			continue;
		}
		if (MAX_PINS == npins) {
			result = OPEN_REFUSED;
			goto done;
		}
		saved = ext[i];
		ext[i] = L'\0';
		pin = CreateFileW(ext, FILE_READ_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        FILE_FLAG_OPEN_REPARSE_POINT |
		                FILE_FLAG_BACKUP_SEMANTICS |
		                FILE_FLAG_OPEN_NO_RECALL,
		        NULL);
		ext[i] = saved;
		if (INVALID_HANDLE_VALUE == pin) {
			*os_error = GetLastError();
			goto done;
		}
		pins[npins++] = pin;
		if (!GetFileInformationByHandleEx(
		            pin, FileAttributeTagInfo, &tag, sizeof tag)) {
			*os_error = GetLastError();
			goto done;
		}
		if ((tag.FileAttributes &
		            (UNSAFE_ATTRS & ~FILE_ATTRIBUTE_DIRECTORY)) ||
		        (!is_file &&
		                !(tag.FileAttributes &
		                        FILE_ATTRIBUTE_DIRECTORY)) ||
		        (is_file &&
		                (tag.FileAttributes &
		                        FILE_ATTRIBUTE_DIRECTORY))) {
			result = OPEN_REFUSED;
			goto done;
		}
	}
	h = CreateFileW(ext, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
	        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL |
	                FILE_FLAG_SEQUENTIAL_SCAN,
	        NULL);
	if (INVALID_HANDLE_VALUE == h) {
		*os_error = GetLastError();
		goto done;
	}
	if (FILE_TYPE_DISK != GetFileType(h) ||
	        !GetFileInformationByHandleEx(
	                h, FileAttributeTagInfo, &tag, sizeof tag) ||
	        (tag.FileAttributes & UNSAFE_ATTRS)) {
		result = OPEN_REFUSED;
		goto done;
	}
	if (!GetFileInformationByHandleEx(
	            pins[npins - 1], FileIdInfo, &pinned, sizeof pinned) ||
	        !GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id)) {
		*os_error = GetLastError();
		goto done;
	}
	if (pinned.VolumeSerialNumber != id.VolumeSerialNumber ||
	        0 !=
	                memcmp(pinned.FileId.Identifier, id.FileId.Identifier,
	                        16)) {
		result =
		        OPEN_REFUSED; /* not the object the path named a moment ago */
		goto done;
	}
	result = check_guid_path(h, os_error);
	if (OPEN_OK != result) {
		goto done;
	}
	result = OPEN_FAILED;
	if (!GetFileInformationByHandleEx(
	            h, FileBasicInfo, &basic, sizeof basic) ||
	        !GetFileSizeEx(h, &size)) {
		*os_error = GetLastError();
		goto done;
	}
	key->volume = id.VolumeSerialNumber;
	memcpy(key->id, id.FileId.Identifier, sizeof key->id);
	key->size = (uint64_t)size.QuadPart;
	key->written = (uint64_t)basic.LastWriteTime.QuadPart;
	*out = h;
	h = INVALID_HANDLE_VALUE;
	result = OPEN_OK;
done:
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	while (npins) {
		CloseHandle(pins[--npins]);
	}
	free(ext);
	return result;
}

static bool key_equal(const FileKey *a, const FileKey *b)
{
	return a->volume == b->volume && a->size == b->size &&
	        a->written == b->written && a->entry == b->entry &&
	        0 == memcmp(a->id, b->id, 16);
}

static Slot *cache_find(WfImageWorker *w,
        const FileKey *key,
        WfDecodeKind kind,
        uint32_t page,
        uint32_t box_w,
        uint32_t box_h,
        uint64_t session)
{
	for (size_t i = 0; i < CACHE_SLOTS; i++) {
		Slot *s = &w->cache[i];

		if (s->used && s->kind == kind && s->page == page &&
		        s->box_w == box_w && s->box_h == box_h &&
		        (kind != WF_DKIND_PDF || s->session == session) &&
		        key_equal(&s->key, key)) {
			s->stamp = ++w->clock;
			return s;
		}
	}
	return NULL;
}

static void slot_clear(WfImageWorker *w, Slot *s)
{
	if (s->used) {
		w->cache_used -= s->bytes;
	}
	pixels_free(s->pm, s->kind == WF_DKIND_PDF);
	memset(s, 0, sizeof *s);
}

static WfPixmap *pixmap_copy(const WfPixmap *src);

/* Keeps a copy of the pixmap (the lookup makes one more for the caller). The
 * copy is made only when it will be kept: a picture larger than the whole cache,
 * or one nobody wants any more, is not copied to be thrown away. The oldest
 * entries go until the new one fits the byte budget and a slot is free. */
static void cache_store(WfImageWorker *w,
        const FileKey *key,
        const Req *r,
        const WfDecodeInfo *info,
        const WfPixmap *src)
{
	size_t bytes = wf_pixmap_bytes(src->w, src->h),
	       budget = w->opt.cache_bytes;
	Slot *slot = NULL;
	WfPixmap *pm;

	AcquireSRWLockShared(&w->core.lock);
	bool obsolete = r->kind == WF_DKIND_PDF &&
	        (w->core.stop || w->active_session != r->session ||
	                (!r->prefetch && w->serial != r->serial));
	ReleaseSRWLockShared(&w->core.lock);
	if (bytes > budget || obsolete) {
		return; /* one picture larger than the whole cache is not kept */
	}
	pm = pixmap_copy(src);
	if (!pm) {
		return;
	}
	for (;;) {
		Slot *oldest = NULL;

		slot = NULL;
		for (size_t i = 0; i < CACHE_SLOTS; i++) {
			if (!w->cache[i].used) {
				slot = &w->cache[i];
			} else if (!oldest ||
			        w->cache[i].stamp < oldest->stamp) {
				oldest = &w->cache[i];
			}
		}
		if (slot && w->cache_used + bytes <= budget) {
			break;
		}
		slot_clear(w, oldest);
	}
	slot->used = true;
	slot->stamp = ++w->clock;
	slot->bytes = bytes;
	slot->key = *key;
	slot->kind = r->kind;
	slot->session = r->session;
	slot->page = r->page;
	slot->box_w = r->box_w;
	slot->box_h = r->box_h;
	slot->pm = pm;
	slot->info = *info;
	w->cache_used += bytes;
}

static WfPixmap *pixmap_copy(const WfPixmap *src)
{
	WfPixmap *p;

	if (WF_OK != wf_pixmap_new(src->w, src->h, &p)) {
		return NULL;
	}
	memcpy(p->px, src->px, wf_pixmap_bytes(src->w, src->h));
	return p;
}

typedef struct
{
	WfImageWorker *worker;
	uint64_t serial, deadline;
} EncodeCancel;

static bool encode_cancelled(void *ctx)
{
	EncodeCancel *c = ctx;
	bool stop;
	AcquireSRWLockShared(&c->worker->core.lock);
	stop = c->worker->core.stop || c->serial != c->worker->serial;
	ReleaseSRWLockShared(&c->worker->core.lock);
	return stop || GetTickCount64() > c->deadline;
}

static void encoded_clear(WfImageWorker *w, Encoded *e)
{
	if (e->crop) {
		w->encoded_used -=
		        wf_pixmap_bytes(e->crop->w, e->crop->h) + e->data.cap;
	}
	pixels_free(e->crop, e->pdf);
	bytes_free(&e->data, e->pdf);
	memset(e, 0, sizeof *e);
}

/* Worker-confined cache invalidation; cancel requests it under the lock and
 * wakes this thread even without another decode. Logical access ends at cancel. */
static void pdf_cache_clear(WfImageWorker *w)
{
	for (size_t i = 0; i < CACHE_SLOTS; i++) {
		if (w->cache[i].used && w->cache[i].kind == WF_DKIND_PDF) {
			slot_clear(w, &w->cache[i]);
		}
	}
	for (size_t i = 0; i < ENCODE_SLOTS; i++) {
		if (w->encoded[i].crop && w->encoded[i].pdf) {
			encoded_clear(w, &w->encoded[i]);
		}
	}
	w->pdf_bound = false;
	w->pdf_session = 0;
	SecureZeroMemory(&w->pdf_key, sizeof w->pdf_key);
}

/* Cache owns a copy of pixels and the encoded bytes. Position isn't encoded;
 * exact viewport pixels, dimensions, background and quality must all match. */
static void encoded_store(
        WfImageWorker *w, const Req *r, WfPixmap *crop, const WfBuf *data)
{
	size_t need = wf_pixmap_bytes(crop->w, crop->h) + data->len + 1;
	Encoded *slot = NULL;
	if (need > ENCODE_CACHE_BYTES) {
		pixels_free(crop, r->kind == WF_DKIND_PDF);
		return;
	}
	for (;;) {
		Encoded *oldest = NULL;
		for (size_t i = 0; i < ENCODE_SLOTS; i++) {
			Encoded *e = &w->encoded[i];
			if (!e->crop) {
				slot = e;
			} else if (!oldest || e->stamp < oldest->stamp) {
				oldest = e;
			}
		}
		if (slot && w->encoded_used + need <= ENCODE_CACHE_BYTES) {
			break;
		}
		encoded_clear(w, oldest);
	}
	/* Exact allocation: cache accounting includes the actual byte capacity. */
	slot->data.data = malloc(data->len + 1);
	if (!slot->data.data) {
		pixels_free(crop, r->kind == WF_DKIND_PDF);
		return;
	}
	memcpy(slot->data.data, data->data, data->len + 1);
	slot->data.len = data->len;
	slot->data.cap = data->len + 1;
	slot->crop = crop;
	slot->pdf = r->kind == WF_DKIND_PDF;
	slot->session = r->session;
	slot->bg_rgb = r->bg_rgb;
	slot->quality = r->quality;
	slot->max_colors = r->max_colors;
	slot->stamp = ++w->clock;
	w->encoded_used += need;
}

static void encode_view(WfImageWorker *w, const Req *r, WfImageResult *res)
{
	WfPixmap *crop = NULL;
	uint32_t width, height, px, py;
	unsigned colors = r->quality == 2 ? 64u : r->quality == 1 ? 128u : 256u;
	if (r->max_colors >= 2 && r->max_colors < colors) {
		colors = r->max_colors;
	}
	EncodeCancel cancel_state = {w, r->serial, GetTickCount64() + 1000};
	if (!r->sixel || r->prefetch || !res->pm ||
	        encode_cancelled(&cancel_state)) {
		return;
	}
	width = res->pm->w < r->view_w ? res->pm->w : r->view_w;
	height = res->pm->h < r->view_h ? res->pm->h : r->view_h;
	px = r->pan_x < res->pm->w - width ? r->pan_x : res->pm->w - width;
	py = r->pan_y < res->pm->h - height ? r->pan_y : res->pm->h - height;
	if ((uint64_t)width * height > WF_SIXEL_MAX_PIXELS ||
	        WF_OK != wf_pixmap_new(width, height, &crop)) {
		return;
	}
	for (uint32_t y = 0; y < height; y++) {
		memcpy(crop->px + (size_t)y * width,
		        res->pm->px + (size_t)(py + y) * res->pm->w + px,
		        (size_t)width * 4);
	}
	for (size_t i = 0; i < ENCODE_SLOTS; i++) {
		Encoded *e = &w->encoded[i];
		if (e->crop && e->pdf == (r->kind == WF_DKIND_PDF) &&
		        (!e->pdf || e->session == r->session) &&
		        e->crop->w == width && e->crop->h == height &&
		        e->bg_rgb == r->bg_rgb && e->quality == r->quality &&
		        e->max_colors == r->max_colors &&
		        !memcmp(e->crop->px, crop->px,
		                wf_pixmap_bytes(width, height))) {
			if (WF_OK ==
			        wf_buf_append(&res->sixel, e->data.data,
			                e->data.len)) {
				res->sixel_w = width;
				res->sixel_h = height;
				e->stamp = ++w->clock;
			}
			pixels_free(crop, r->kind == WF_DKIND_PDF);
			return;
		}
	}
	if (WF_OK ==
	        wf_sixel_encode_ex(crop, r->bg_rgb, colors, r->quality == 0,
	                encode_cancelled, &cancel_state, &res->sixel)) {
		res->sixel_w = width;
		res->sixel_h = height;
		encoded_store(w, r, crop, &res->sixel);
		crop = NULL;
	}
	pixels_free(crop, r->kind == WF_DKIND_PDF);
}

static void publish(WfImageWorker *w, const Req *r, WfImageResult *res)
{
	WfImageResult retired = {0};
	encode_view(w, r, res);
	AcquireSRWLockExclusive(&w->core.lock);
	if (!w->core.stop && !r->prefetch && r->serial == w->serial) {
		if (w->has_result) {
			retired = w->result;
			bump(&w->core.stats.discarded_results);
		}
		w->result = *res;
		w->result_serial = r->serial;
		w->has_result = true;
		res = NULL;
		SetEvent(w->core.ready);
	} else if (!r->prefetch) {
		bump(&w->core.stats.discarded_results);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	pixels_free(retired.pm, retired.sensitive);
	bytes_free(&retired.sixel, retired.sensitive);
	if (res) {
		pixels_free(res->pm, res->sensitive);
		bytes_free(&res->sixel, res->sensitive);
	}
}

static bool ensure_host(WfImageWorker *w, WfImageResult *res)
{
	WfDecHostOptions o;
	DWORD err = 0;

	if (wf_dechost_alive(w->host)) {
		return true;
	}
	if (w->host) {
		wf_dechost_stop(w->host);
		w->host = NULL;
		AcquireSRWLockExclusive(&w->core.lock);
		bump(&w->stats.host_kills);
		ReleaseSRWLockExclusive(&w->core.lock);
	}
	memset(&o, 0, sizeof o);
	o.sandbox = w->opt.sandbox;
	o.exe = w->opt.host_exe;
	if (!o.exe && w->opt.sandbox) {
		if (!w->staged[0] &&
		        WF_OK !=
		                wf_dechost_stage(NULL, w->staged,
		                        ARRAYSIZE(w->staged), &err)) {
			goto fail;
		}
		o.exe = w->staged;
	}
	if (WF_OK != wf_dechost_start(&o, &w->host, &err)) {
		goto fail;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	bump(&w->stats.host_starts);
	ReleaseSRWLockExclusive(&w->core.lock);
	return true;
fail:
	res->status = WF_DSTAT_HOST_LOST;
	res->unavailable = true;
	res->os_error = err;
	return false;
}

static uint64_t hash_wide(const wchar_t *s)
{
	uint64_t h = 1469598103934665603ull; /* FNV-1a over the UTF-16 units */

	for (; *s; s++) {
		h = (h ^ (uint64_t)(unsigned)*s) * 1099511628211ull;
	}
	return h;
}

static bool archive_cancelled(void *ctx)
{
	WfImageWorker *w = ctx;

	return WAIT_OBJECT_0 == WaitForSingleObject(w->abandon, 0);
}

/* The bytes of one entry of an archive, whole, or the reason they are not. */
static WfDecodeStatus read_entry(WfImageWorker *w,
        const wchar_t *vpath,
        WfArchiveData *data,
        bool *refused)
{
	WfArchiveCtl ctl;
	WfStatus st;

	ctl.cancelled = archive_cancelled;
	ctl.ctx = w;
	*refused = false;
	st = wf_vpath_read(vpath, WF_ARCHIVE_ENTRY_MAX, &ctl, data);
	switch (st) {
	case WF_OK:
		if (data->truncated) {
			wf_archive_data_free(data);
			return WF_DSTAT_TOO_LARGE; /* a picture must be whole */
		}
		return WF_DSTAT_OK;
	case WF_E_CANCELLED: return WF_DSTAT_ABORTED;
	case WF_E_RANGE:     return WF_DSTAT_TOO_LARGE;
	case WF_E_NOMEM:     return WF_DSTAT_INTERNAL;
	case WF_E_ENCRYPTED:
	case WF_E_FORMAT:
	case WF_E_INVAL:     return WF_DSTAT_UNSUPPORTED;
	case WF_E_ACCESS:
		*refused = true; /* a link or a special entry: never followed */
		return WF_DSTAT_UNSUPPORTED;
	default: return WF_DSTAT_CORRUPT;
	}
}

static void process(WfImageWorker *w, Req *r)
{
	WfImageResult res;
	HANDLE file = INVALID_HANDLE_VALUE;
	FileKey key;
	OpenResult opened;
	DWORD os_error = 0;
	Slot *hit;
	WfPixmap *pm = NULL;
	WfDecodeInfo info;
	ULONGLONG t0;
	wchar_t *outer = NULL;
	const wchar_t *inner = NULL;
	bool in_archive = wf_vpath_is(r->path);

	memset(&res, 0, sizeof res);
	memset(&info, 0, sizeof info);
	memset(&key, 0, sizeof key);
	res.gen = r->gen;
	res.page = r->page;
	res.sensitive = r->kind == WF_DKIND_PDF;
	res.box_w = r->box_w;
	res.box_h = r->box_h;
	if (WaitForSingleObject(w->abandon, 0) == WAIT_OBJECT_0) {
		res.status = WF_DSTAT_ABORTED;
		publish(w, r, &res);
		return;
	}
	if (in_archive && !wf_vpath_split(r->path, &outer, &inner)) {
		res.status = WF_DSTAT_CORRUPT;
		publish(w, r, &res);
		return;
	}
	/* the file that is opened and pinned is the archive itself; the entry is
	 * named by the cache key */
	opened = open_input(
	        in_archive ? outer : r->path, &file, &key, &os_error);
	if (in_archive) {
		key.entry = hash_wide(inner);
		free(outer);
	}
	if (OPEN_OK != opened) {
		res.status = OPEN_REFUSED == opened ? WF_DSTAT_UNSUPPORTED
		                                    : WF_DSTAT_CORRUPT;
		res.os_error = os_error;
		res.refused = OPEN_REFUSED == opened;
		publish(w, r, &res);
		return;
	}
	if (w->pdf_bound &&
	        (w->pdf_session != r->session ||
	                !key_equal(&w->pdf_key, &key))) {
		bool replaced = r->kind == WF_DKIND_PDF &&
		        w->pdf_session == r->session && r->password[0];
		pdf_cache_clear(w);
		if (replaced) {
			CloseHandle(file);
			res.status = WF_DSTAT_PASSWORD_REQUIRED;
			publish(w, r, &res);
			return;
		}
	}
	if (r->kind == WF_DKIND_PDF) {
		w->pdf_key = key;
		w->pdf_session = r->session;
		w->pdf_bound = true;
	}
	hit = cache_find(
	        w, &key, r->kind, r->page, r->box_w, r->box_h, r->session);
	if (hit) {
		res.pm = pixmap_copy(hit->pm);
		if (res.pm) {
			res.status = WF_DSTAT_OK;
			res.info = hit->info;
			res.from_cache = true;
			AcquireSRWLockExclusive(&w->core.lock);
			bump(&w->stats.cache_hits);
			ReleaseSRWLockExclusive(&w->core.lock);
			CloseHandle(file);
			publish(w, r, &res);
			return;
		}
	}
	AcquireSRWLockExclusive(&w->core.lock);
	bump(&w->stats.cache_misses);
	ReleaseSRWLockExclusive(&w->core.lock);
	if (!ensure_host(w, &res)) {
		CloseHandle(file);
		publish(w, r, &res);
		return;
	}
	/* Profile creation/startup cannot itself be interrupted. If selection
	 * changed during it, do not send the obsolete request to the child. */
	if (WaitForSingleObject(w->abandon, 0) == WAIT_OBJECT_0) {
		CloseHandle(file);
		res.status = WF_DSTAT_ABORTED;
		publish(w, r, &res);
		return;
	}
	t0 = GetTickCount64();
	if (in_archive) {
		WfArchiveData data;
		bool refused = false;

		CloseHandle(file); /* its job was the policy and the identity */
		res.status = read_entry(w, r->path, &data, &refused);
		res.refused = refused;
		if (WF_DSTAT_OK == res.status) {
			res.status = wf_dechost_decode_bytes(w->host, r->kind,
			        data.bytes, data.n, r->box_w, r->box_h, r->page,
			        r->password[0] ? r->password : NULL,
			        w->opt.timeout_ms, w->abandon, w->opt.grace_ms,
			        &pm, &info);
			wf_archive_data_free(&data);
		}
	} else {
		res.status = wf_dechost_decode(w->host, r->kind, file, r->box_w,
		        r->box_h, r->page, r->password[0] ? r->password : NULL,
		        w->opt.timeout_ms, w->abandon, w->opt.grace_ms, &pm,
		        &info);
		CloseHandle(file);
	}
	AcquireSRWLockExclusive(&w->core.lock);
	w->stats.decode_ms_total += GetTickCount64() - t0;
	ReleaseSRWLockExclusive(&w->core.lock);
	res.info = info;
	if (WF_DSTAT_OK == res.status && pm) {
		/* kept even if the answer is no longer wanted: scrolling back is
		 * a hit */
		cache_store(w, &key, r, &info, pm);
		res.pm = pm;
	} else {
		pixels_free(pm, r->kind == WF_DKIND_PDF);
	}
	publish(w, r, &res);
}

static DWORD WINAPI worker_main(LPVOID ctx)
{
	WfImageWorker *w = ctx;

	for (;;) {
		Req r;
		WfImageResult retired = {0};

		AcquireSRWLockExclusive(&w->core.lock);
		while (!w->core.stop && !w->has_fg && !w->has_pf &&
		        !w->purge_pdf) {
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, INFINITE, 0);
		}
		/* UI submission/cancellation only invalidates. The worker owns the
		 * potentially large secure erase, outside the shared lock. */
		if (w->has_result && w->result_serial != w->serial) {
			retired = w->result;
			memset(&w->result, 0, sizeof w->result);
			w->has_result = false;
			bump(&w->core.stats.discarded_results);
			ResetEvent(w->core.ready);
			ReleaseSRWLockExclusive(&w->core.lock);
			pixels_free(retired.pm, retired.sensitive);
			bytes_free(&retired.sixel, retired.sensitive);
			continue;
		}
		if (w->core.stop) {
			ReleaseSRWLockExclusive(&w->core.lock);
			break;
		}
		if (w->purge_pdf) {
			w->purge_pdf = false;
			ReleaseSRWLockExclusive(&w->core.lock);
			pdf_cache_clear(w);
			continue;
		}
		if (w->has_fg) {
			r = w->fg;
			memset(&w->fg, 0, sizeof w->fg);
			w->has_fg = false;
		} else {
			r = w->pf;
			memset(&w->pf, 0, sizeof w->pf);
			w->has_pf = false;
		}
		w->core.busy = true;
		ResetEvent(w->abandon);
		ReleaseSRWLockExclusive(&w->core.lock);

		process(w, &r);
		req_free(&r);

		AcquireSRWLockExclusive(&w->core.lock);
		w->core.busy = false;
		bump(&w->core.stats.completed_requests);
		ReleaseSRWLockExclusive(&w->core.lock);
	}
	worker_release(w);
	return 0;
}

static WfStatus copy_request(Req *dst, const WfImageRequest *src)
{
	size_t pw = src->password ? wcslen(src->password) : 0;

	memset(dst, 0, sizeof *dst);
	if (!src->path || wcslen(src->path) > 32760 ||
	        (WF_DKIND_IMAGE != src->kind && WF_DKIND_PDF != src->kind) ||
	        0 == src->box_w || 0 == src->box_h ||
	        src->box_w > WF_DWIRE_BOX_MAX ||
	        src->box_h > WF_DWIRE_BOX_MAX ||
	        (uint64_t)src->box_w * src->box_h * 4 > WF_DWIRE_RASTER_MAX ||
	        pw > PASSWORD_MAX || (pw && !src->session)) {
		return WF_E_INVAL;
	}
	dst->path = wf_path_dup(src->path);
	if (!dst->path) {
		return WF_E_NOMEM;
	}
	dst->gen = src->gen;
	dst->session = src->session;
	dst->kind = src->kind;
	dst->box_w = src->box_w;
	dst->box_h = src->box_h;
	dst->page = src->page;
	dst->prefetch = src->prefetch;
	dst->view_w = src->view_w;
	dst->view_h = src->view_h;
	dst->pan_x = src->pan_x;
	dst->pan_y = src->pan_y;
	dst->bg_rgb = src->bg_rgb;
	dst->quality = src->quality;
	dst->max_colors = src->max_colors;
	dst->sixel = src->sixel;
	if (pw) {
		memcpy(dst->password, src->password, pw * sizeof(wchar_t));
	}
	return WF_OK;
}

static WfStatus submit(void *ctx, const WfImageRequest *rq)
{
	WfImageWorker *w = ctx;
	Req r;
	WfStatus st = copy_request(&r, rq);

	if (WF_OK != st) {
		return st;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		req_free(&r);
		return WF_E_INVAL;
	}
	if (r.prefetch) {
		if (w->has_pf) {
			req_free(&w->pf);
		}
		w->pf = r;
		w->has_pf = true;
	} else {
		if (w->has_fg) {
			req_free(&w->fg);
			bump(&w->core.stats.superseded_requests);
		}
		if (w->has_result) {
			ResetEvent(w->core.ready);
		}
		if (w->active_session != r.session) {
			w->active_session = r.session;
			w->purge_pdf = true;
		}
		r.serial = ++w->serial;
		w->fg = r;
		w->has_fg = true;
		if (w->core.busy) {
			SetEvent(w->abandon);
		}
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

static void cancel(void *ctx)
{
	WfImageWorker *w = ctx;

	AcquireSRWLockExclusive(&w->core.lock);
	w->serial++;
	w->active_session = 0;
	w->purge_pdf = true;
	if (w->has_fg) {
		req_free(&w->fg);
		w->has_fg = false;
		bump(&w->core.stats.superseded_requests);
	}
	if (w->has_pf) {
		req_free(&w->pf);
		w->has_pf = false;
	}
	/* Serial invalidates the result; only the worker erases its payload. */
	ResetEvent(w->core.ready);
	if (w->core.busy) {
		SetEvent(w->abandon);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
}

WfStatus wf_imageworker_start(
        WfImageWorker **out, const WfImageWorkerOptions *options)
{
	WfImageWorker *w;

	if (!out || !options) {
		return WF_E_INVAL;
	}
	*out = NULL;
	w = calloc(1, sizeof *w);
	if (!w) {
		return WF_E_NOMEM;
	}
	w->opt = *options;
	if (0 == w->opt.cache_bytes) {
		w->opt.cache_bytes = CACHE_BYTES_DEFAULT;
	}
	if (0 == w->opt.timeout_ms) {
		w->opt.timeout_ms = TIMEOUT_DEFAULT_MS;
	}
	if (0 == w->opt.grace_ms) {
		w->opt.grace_ms = GRACE_DEFAULT_MS;
	}
	w->abandon = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!w->abandon) {
		free(w);
		return WF_E_OS;
	}
	if (WF_OK != wf_wc_start(&w->core, worker_main, w)) {
		CloseHandle(w->abandon);
		free(w);
		return WF_E_OS;
	}
	*out = w;
	return WF_OK;
}

WfWorkerStopReport wf_imageworker_stop(WfImageWorker *w)
{
	WfWorkerStopReport none;

	memset(&none, 0, sizeof none);
	if (!w) {
		return none;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	w->core.stop = true;
	SetEvent(w->abandon); /* an answer in flight is not wanted */
	ReleaseSRWLockExclusive(&w->core.lock);
	/* the host's grace is short, but a stuck decode ends only when the
	 * request's own deadline does: wait a bounded time, then hand the rest
	 * to the thread (it releases the object itself) */
	return wf_wc_stop(&w->core, 3000, false, worker_free, w);
}

WfImageWorkerStats wf_imageworker_stats(WfImageWorker *w)
{
	WfImageWorkerStats s;

	AcquireSRWLockShared(&w->core.lock);
	s = w->stats;
	s.worker = w->core.stats;
	ReleaseSRWLockShared(&w->core.lock);
	return s;
}

bool wf_imageworker_idle(WfImageWorker *w)
{
	bool idle;

	if (!w) {
		return true;
	}
	AcquireSRWLockShared(&w->core.lock);
	/* an answer that is not the newest is never delivered, so it does not
	 * keep the worker */
	idle = !w->core.busy && !w->has_fg && !w->has_pf &&
	        !(w->has_result && w->result_serial == w->serial);
	ReleaseSRWLockShared(&w->core.lock);
	return idle;
}

HANDLE wf_imageworker_event(const WfImageWorker *w)
{
	return w->core.ready;
}

bool wf_imageworker_poll(WfImageWorker *w, WfImageResult *out)
{
	bool got;

	AcquireSRWLockExclusive(&w->core.lock);
	got = w->has_result && w->result_serial == w->serial;
	if (got) {
		*out = w->result;
		memset(&w->result, 0, sizeof w->result);
		w->has_result = false;
		ResetEvent(w->core.ready);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	return got;
}

WfImagePort wf_imageworker_port(WfImageWorker *w)
{
	WfImagePort p = {w, submit, cancel};

	return p;
}

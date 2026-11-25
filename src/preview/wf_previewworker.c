#include "wf_previewworker.h"
#include "wf_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../archive/wf_archive.h"
#include "../async/wf_workcore.h"
#include "../path/wf_path.h"
#include "../base/wf_macro.h"
#include "../base/wf_mem.h"

struct WfPreviewWorker
{
	WfWorkCore core; /* its lock guards all mutable fields; the worker alone
	                    owns active I/O */
	bool explicit_load;
	uint64_t serial;
	uint32_t gen;
	wchar_t *path;          /* one queued request; newest replaces it */
	WfPreviewResult result; /* one immutable result; newest replaces it */
	size_t max_bytes;       /* immutable after start */
	UINT fallback_codepage;
};

static void worker_free(void *owner)
{
	WfPreviewWorker *w = owner;

	wf_preview_free(w->result.preview);
	free(w->path);
	free(w);
}

static void try_fallback(WfPreviewWorker *w,
        const BYTE *bytes,
        size_t n,
        bool truncated,
        bool keep_tabs,
        WfPreview **p)
{
	wchar_t *wide;
	int units, converted;
	size_t keep;
	WfPreview *decoded = NULL;
	if (!*p || WF_PREVIEW_INVALID != (*p)->kind || !w->fallback_codepage ||
	        CP_UTF8 == w->fallback_codepage || !n ||
	        (n >= 2 &&
	                ((0xff == bytes[0] && 0xfe == bytes[1]) ||
	                        (0xfe == bytes[0] && 0xff == bytes[1]))) ||
	        (n >= 3 && !memcmp(bytes, "\xef\xbb\xbf", 3))) {
		return;
	}
	units = MultiByteToWideChar(w->fallback_codepage, MB_ERR_INVALID_CHARS,
	        (const char *)bytes, (int)n, NULL, 0);
	/* A read that stopped at its limit may have cut a character in two: the
	 * incomplete end (up to 3 bytes, as in GB18030) is left out, and nothing
	 * else is forgiven. A file that ends whole and is invalid stays invalid. */
	for (size_t cut = 0; units <= 0 && truncated && cut < 3 && n > 1;
	        cut++) {
		n--;
		units = MultiByteToWideChar(w->fallback_codepage,
		        MB_ERR_INVALID_CHARS, (const char *)bytes, (int)n, NULL,
		        0);
	}
	if (units <= 0) {
		return;
	}
	wide = wf_malloc_array(((size_t)units + 1), sizeof *wide);
	if (!wide) {
		return;
	}
	wide[0] = 0xfeff;
	converted =
	        MultiByteToWideChar(w->fallback_codepage, MB_ERR_INVALID_CHARS,
	                (const char *)bytes, (int)n, wide + 1, units);
	keep = (size_t)units;
	if (keep > WF_PREVIEW_HARD_BYTES / 2u - 1u) {
		keep = WF_PREVIEW_HARD_BYTES / 2u - 1u;
		truncated = 1;
	}
	if (converted == units &&
	        wf_preview_decode_opt(wide, (keep + 1) * sizeof *wide,
	                truncated, keep_tabs, &decoded) == WF_OK) {
		if (WF_PREVIEW_TEXT == decoded->kind) {
			snprintf(decoded->detail, sizeof decoded->detail,
			        "CP%u fallback", w->fallback_codepage);
			wf_preview_free(*p);
			*p = decoded;
		} else {
			wf_preview_free(decoded);
		}
	}
	free(wide);
}

static bool obsolete(WfPreviewWorker *w, uint64_t serial)
{
	int stale;
	AcquireSRWLockShared(&w->core.lock);
	stale = w->core.stop || serial != w->serial;
	ReleaseSRWLockShared(&w->core.lock);
	return stale;
}

#define UNSAFE_ATTRS                                                           \
	(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |               \
	        FILE_ATTRIBUTE_RECALL_ON_OPEN |                                \
	        FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_DEVICE)

/* Why a preview was not produced. `reason` stays the generic text unless a
 * step knows better; `error` is the Win32 code to show, 0 for policy verdicts. */
typedef struct
{
	const char *reason;
	DWORD error;
} Refusal;

#define DEFAULT_REFUSAL                                                        \
	"Cannot safely open preview (busy, inaccessible, or changed)"

static bool refuse(Refusal *r, const char *reason, DWORD error)
{
	if (reason) {
		r->reason = reason;
	}
	r->error = error;
	return false;
}

/* The checked object. The pins are metadata handles on the root and on every
 * component down to the file; they stay open (without DELETE sharing) until
 * safe_file_close(), so a checked component cannot be renamed or replaced
 * while the data handle is read. Depth is bounded. */
typedef struct
{
	wchar_t *extended;
	HANDLE pins[128];
	size_t npins;
	HANDLE file;
} SafeFile;

static void safe_file_init(SafeFile *sf)
{
	memset(sf, 0, sizeof *sf);
	sf->file = INVALID_HANDLE_VALUE;
}

static void safe_file_close(SafeFile *sf)
{
	if (INVALID_HANDLE_VALUE != sf->file) {
		CloseHandle(sf->file);
	}
	while (sf->npins) {
		CloseHandle(sf->pins[--sf->npins]);
	}
	free(sf->extended);
	safe_file_init(sf);
}

/* Only the drive form of an extended path is understood below (UNC would
 * index the wrong drive), and only local volume types are read. */
static bool require_local_drive(const wchar_t *extended, Refusal *r)
{
	size_t n = wcslen(extended);
	wchar_t root[4];
	UINT drive;

	if (n < 7 || L':' != extended[5] || L'\\' != extended[6] ||
	        !((extended[4] >= L'A' && extended[4] <= L'Z') ||
	                (extended[4] >= L'a' && extended[4] <= L'z'))) {
		return refuse(r, NULL, 0);
	}
	root[0] = extended[4];
	root[1] = L':';
	root[2] = L'\\';
	root[3] = 0;
	drive = GetDriveTypeW(root);
	if (DRIVE_FIXED != drive && DRIVE_REMOVABLE != drive &&
	        DRIVE_RAMDISK != drive) {
		return refuse(r, "Remote or unknown volume refused", 0);
	}
	return true;
}

/* Pins the root and each complete component, including the file. Never
 * dereferences a reparse point, in an ancestor or in the file. */
static bool pin_ancestors(
        WfPreviewWorker *w, uint64_t serial, SafeFile *sf, Refusal *r)
{
	size_t n = wcslen(sf->extended), i;
	FILE_ATTRIBUTE_TAG_INFO info;

	for (i = 7; i <= n; i++) {
		wchar_t saved;
		HANDLE pin;

		if (7 != i && i != n && L'\\' != sf->extended[i]) {
			continue;
		}
		if (obsolete(w, serial)) {
			return refuse(r, NULL, 0);
		}
		if (sf->npins == WF_ARRAY_LEN(sf->pins)) {
			return refuse(
			        r, "Preview ancestor depth limit reached", 0);
		}
		saved = sf->extended[i];
		sf->extended[i] = 0;
		pin = CreateFileW(sf->extended, FILE_READ_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        FILE_FLAG_OPEN_REPARSE_POINT |
		                FILE_FLAG_BACKUP_SEMANTICS |
		                FILE_FLAG_OPEN_NO_RECALL,
		        NULL);
		sf->extended[i] = saved;
		if (INVALID_HANDLE_VALUE == pin) {
			return refuse(r, NULL, GetLastError());
		}
		sf->pins[sf->npins++] = pin;
		if (!GetFileInformationByHandleEx(
		            pin, FileAttributeTagInfo, &info, sizeof info)) {
			return refuse(r, NULL, GetLastError());
		}
		if (info.FileAttributes & UNSAFE_ATTRS) {
			return refuse(r,
			        "Reparse/cloud/offline preview refused (no "
			        "content read)",
			        0);
		}
		if (i < n &&
		        !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
			return refuse(r, NULL, 0);
		}
		if (i == n &&
		        (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
			return refuse(r, "Directory (metadata only)", 0);
		}
	}
	return true;
}

/* A GUID final path is mandatory: mapped/redirected remote storage must not
 * sneak through an apparently local drive letter. */
static bool require_guid_volume(HANDLE file, Refusal *r)
{
	wchar_t *final_path;
	DWORD capacity;
	DWORD got = GetFinalPathNameByHandleW(
	        file, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
	bool ok;

	if (!got) {
		return refuse(r, NULL, GetLastError());
	}
	if (got > 32768) {
		return refuse(r, NULL, ERROR_FILENAME_EXCED_RANGE);
	}
	final_path = wf_malloc_array((size_t)got, sizeof *final_path);
	if (!final_path) {
		return refuse(r, "Out of memory reading preview",
		        ERROR_NOT_ENOUGH_MEMORY);
	}
	capacity = got;
	got = GetFinalPathNameByHandleW(file, final_path, capacity,
	        FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
	if (!got || got >= capacity) {
		DWORD error = got ? ERROR_FILENAME_EXCED_RANGE : GetLastError();

		free(final_path);
		return refuse(r, NULL, error);
	}
	ok = !wcsncmp(final_path, L"\\\\?\\Volume{", 11);
	free(final_path);
	return ok ? true : refuse(r, NULL, 0);
}

/* Opens the data handle and proves it is the object the pins describe. */
static bool open_data_file(SafeFile *sf, Refusal *r)
{
	FILE_ATTRIBUTE_TAG_INFO info;
	FILE_ID_INFO pinned_id, read_id;

	sf->file = CreateFileW(sf->extended, GENERIC_READ, FILE_SHARE_READ,
	        NULL, OPEN_EXISTING,
	        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL |
	                FILE_FLAG_SEQUENTIAL_SCAN,
	        NULL);
	if (INVALID_HANDLE_VALUE == sf->file) {
		return refuse(r, NULL, GetLastError());
	}
	if (FILE_TYPE_DISK != GetFileType(sf->file)) {
		return refuse(r, NULL, 0);
	}
	if (!GetFileInformationByHandleEx(
	            sf->file, FileAttributeTagInfo, &info, sizeof info)) {
		return refuse(r, NULL, GetLastError());
	}
	if (info.FileAttributes & (UNSAFE_ATTRS | FILE_ATTRIBUTE_DIRECTORY)) {
		return refuse(r, NULL, 0);
	}
	if (!sf->npins) {
		return refuse(r, NULL, 0);
	}
	if (!GetFileInformationByHandleEx(sf->pins[sf->npins - 1], FileIdInfo,
	            &pinned_id, sizeof pinned_id)) {
		return refuse(r, NULL, GetLastError());
	}
	if (!GetFileInformationByHandleEx(
	            sf->file, FileIdInfo, &read_id, sizeof read_id)) {
		return refuse(r, NULL, GetLastError());
	}
	if (pinned_id.VolumeSerialNumber != read_id.VolumeSerialNumber ||
	        memcmp(pinned_id.FileId.Identifier, read_id.FileId.Identifier,
	                16)) {
		return refuse(r, "Preview identity changed", 0);
	}
	return require_guid_volume(sf->file, r);
}

/* Validates the local volume, ancestors and data handle. On success the pins and
 * the data handle in `sf` stay valid until safe_file_close(). */
static bool acquire_safe_file(WfPreviewWorker *w,
        uint64_t serial,
        const wchar_t *path,
        SafeFile *sf,
        Refusal *r)
{
	if (WF_OK != wf_path_to_extended(path, &sf->extended)) {
		return refuse(r, NULL, 0);
	}
	if (!require_local_drive(sf->extended, r)) {
		return false;
	}
	if (!pin_ancestors(w, serial, sf, r)) {
		return false;
	}
	if (obsolete(w, serial)) {
		return refuse(r, NULL, 0);
	}
	return open_data_file(sf, r);
}

/* Reads at most max_bytes + 1 bytes (the extra byte tells that the
 * file is longer), in slices so a superseded request stops early. */
static bool read_bounded(WfPreviewWorker *w,
        uint64_t serial,
        HANDLE file,
        BYTE **out,
        size_t *length,
        bool *truncated,
        Refusal *r)
{
	size_t got_total = 0;
	BYTE *bytes = malloc(w->max_bytes + 1u);

	if (!bytes) {
		return refuse(r, "Out of memory reading preview",
		        ERROR_NOT_ENOUGH_MEMORY);
	}
	while (got_total < w->max_bytes + 1u) {
		DWORD want = (DWORD)(w->max_bytes + 1u - got_total), got;

		if (want > 16384) {
			want = 16384;
		}
		if (obsolete(w, serial)) {
			free(bytes);
			return refuse(r, NULL, 0);
		}
		if (!ReadFile(file, bytes + got_total, want, &got, NULL)) {
			DWORD error = GetLastError();

			free(bytes);
			return refuse(r, "Cannot read preview", error);
		}
		if (!got) {
			break;
		}
		got_total += got;
	}
	*truncated = got_total > w->max_bytes;
	*length = *truncated ? w->max_bytes : got_total;
	*out = bytes;
	return true;
}

/* Strict decoding first, then the configured legacy
 * code page. */
static WfPreview *decode_preview(WfPreviewWorker *w,
        const wchar_t *name,
        const BYTE *bytes,
        size_t length,
        bool truncated,
        Refusal *r)
{
	WfPreview *p = NULL;
	char delim = wf_preview_table_delim(name); /* a CSV or TSV: columns */
	bool tabs = '\t' == delim;

	if (WF_OK !=
	        wf_preview_decode_opt(bytes, length, truncated, tabs, &p)) {
		r->error = ERROR_NOT_ENOUGH_MEMORY;
		r->reason = "Out of memory decoding preview";
		return NULL;
	}
	try_fallback(w, bytes, length, truncated, tabs, &p);
	if (delim && p && WF_PREVIEW_TEXT == p->kind) {
		/* no table (or no memory for it) leaves the text as it is */
		(void)wf_preview_tableize(p, delim);
	}
	return p;
}

typedef struct
{
	WfPreviewWorker *w;
	uint64_t serial;
} ArchiveCancel;

static bool archive_cancelled(void *ctx)
{
	ArchiveCancel *c = ctx;

	return obsolete(c->w, c->serial);
}

/* Why the first bytes of a file in an archive could not be shown, in the
 * words the catalogue translates. */
static const char *archive_reason(WfStatus st)
{
	switch (st) {
	case WF_E_ENCRYPTED:
		return "Encrypted archive entry (no password support)";
	case WF_E_ACCESS:
		return "Link or special entry in an archive (not followed)";
	case WF_E_RANGE:   return "Archive too large to read here";
	case WF_E_CHANGED: return "The archive changed while it was being read";
	case WF_E_CORRUPT: return "Invalid archive (damaged or incomplete)";
	case WF_E_FORMAT:  return "Unsupported archive format";
	case WF_E_NOMEM:   return "Out of memory reading preview";
	default:           return "Preview unavailable";
	}
}

/* A file inside an archive: its first bytes, decoded like any other file's.
 * Nothing is extracted; the archive reader is private to this call. */
static WfPreview *read_archive_text(
        WfPreviewWorker *w, uint64_t serial, const wchar_t *path)
{
	ArchiveCancel cc = {w, serial};
	WfArchiveCtl ctl = {archive_cancelled, &cc};
	WfArchiveData data;
	WfPreview *p = NULL;
	Refusal refusal = {DEFAULT_REFUSAL, 0};
	WfStatus st;

	if (!wf_preview_auto_allowed(path, 0)) {
		return wf_preview_notice(WF_PREVIEW_REFUSED,
		        "Remote preview unavailable: safe local-file reader "
		        "only");
	}
	if (wf_preview_is_image(path)) {
		return wf_preview_notice(WF_PREVIEW_IMAGE_BLOCKED,
		        "Image preview disabled: P0-A03 blocked");
	}
	st = wf_vpath_read(path, w->max_bytes, &ctl, &data);
	if (WF_OK == st) {
		p = decode_preview(
		        w, path, data.bytes, data.n, data.truncated, &refusal);
		if (p && WF_PREVIEW_TEXT == p->kind && !p->detail[0]) {
			snprintf(p->detail, sizeof p->detail, "%s",
			        WF_AF_SINGLE == data.format
			                ? (data.truncated ? "Compressed file "
			                                    "(partial, not "
			                                    "verified)"
			                                  : "Compressed file")
			                : (data.truncated ? "Archive entry "
			                                    "(partial, not "
			                                    "verified)"
			                                  : "Archive entry"));
		}
	} else if (WF_E_INVAL == st) {
		p = wf_preview_notice(
		        WF_PREVIEW_REFUSED, "Directory (metadata only)");
	} else {
		p = wf_preview_notice(WF_PREVIEW_REFUSED, archive_reason(st));
	}
	wf_archive_data_free(&data);
	return p;
}

static WfPreview *read_text(WfPreviewWorker *w,
        uint64_t serial,
        const wchar_t *path,
        bool explicit_load)
{
	SafeFile sf;
	BYTE *bytes = NULL;
	size_t length = 0;
	bool truncated = false;
	WfPreview *p = NULL;
	Refusal refusal = {DEFAULT_REFUSAL, 0};

	(void)explicit_load; /* remote/cloud hydration awaits a verified read
	                        policy */
	if (wf_vpath_is(path)) {
		return read_archive_text(w, serial, path);
	}
	if (!wf_preview_auto_allowed(path, 0)) {
		refusal.reason = "Remote preview unavailable: safe local-file "
		                 "reader only";
		goto done;
	}
	if (wf_preview_is_image(path)) {
		return wf_preview_notice(WF_PREVIEW_IMAGE_BLOCKED,
		        "Image preview disabled: P0-A03 blocked");
	}
	safe_file_init(&sf);
	if (acquire_safe_file(w, serial, path, &sf, &refusal) &&
	        read_bounded(w, serial, sf.file, &bytes, &length, &truncated,
	                &refusal)) {
		p = decode_preview(w, path, bytes, length, truncated, &refusal);
	}
	safe_file_close(&sf);
	free(bytes);
done:
	if (!p) {
		p = wf_preview_notice(WF_PREVIEW_REFUSED, refusal.reason);
	}
	if (p) {
		p->system_error = refusal.error;
	}
	return p;
}

static DWORD WINAPI worker_main(LPVOID ctx)
{
	WfPreviewWorker *w = ctx;
	for (;;) {
		wchar_t *path;
		uint64_t serial;
		uint32_t gen;
		bool explicit_load;
		WfPreview *p;
		AcquireSRWLockExclusive(&w->core.lock);
		while (!w->core.stop && !w->path) {
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, INFINITE, 0);
		}
		if (w->core.stop) {
			ReleaseSRWLockExclusive(&w->core.lock);
			break;
		}
		path = w->path;
		w->path = NULL;
		serial = w->serial;
		gen = w->gen;
		explicit_load = w->explicit_load;
		w->core.busy = true;
		ReleaseSRWLockExclusive(&w->core.lock);
		p = read_text(w, serial, path, explicit_load);
		free(path);
		AcquireSRWLockExclusive(&w->core.lock);
		w->core.busy = false;
		if (UINT64_MAX != w->core.stats.completed_requests) {
			w->core.stats.completed_requests++;
		}
		if (!w->core.stop && serial == w->serial) {
			if (w->result.preview &&
			        UINT64_MAX != w->core.stats.discarded_results) {
				w->core.stats.discarded_results++;
			}
			wf_preview_free(w->result.preview);
			w->result.gen = gen;
			w->result.preview = p;
			p = NULL;
			SetEvent(w->core.ready);
		} else if (UINT64_MAX != w->core.stats.discarded_results) {
			w->core.stats.discarded_results++;
		}
		ReleaseSRWLockExclusive(&w->core.lock);
		wf_preview_free(p);
	}
	wf_wc_release(&w->core, worker_free, w);
	return 0;
}

WfStatus wf_previewworker_start(
        WfPreviewWorker **out, size_t max_bytes, UINT fallback_codepage)
{
	WfPreviewWorker *w;
	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	if (!max_bytes || max_bytes > WF_PREVIEW_HARD_BYTES ||
	        (fallback_codepage && !IsValidCodePage(fallback_codepage))) {
		return WF_E_INVAL;
	}
	w = calloc(1, sizeof *w);
	if (!w) {
		return WF_E_NOMEM;
	}
	w->max_bytes = max_bytes;
	w->fallback_codepage = fallback_codepage;
	if (WF_OK != wf_wc_start(&w->core, worker_main, w)) {
		free(w);
		return WF_E_OS;
	}
	*out = w;
	return WF_OK;
}

static void cancel(void *ctx)
{
	WfPreviewWorker *w = ctx;
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->path && UINT64_MAX != w->core.stats.superseded_requests) {
		w->core.stats.superseded_requests++;
	}
	if (w->result.preview &&
	        UINT64_MAX != w->core.stats.discarded_results) {
		w->core.stats.discarded_results++;
	}
	w->serial++;
	free(w->path);
	w->path = NULL;
	wf_preview_free(w->result.preview);
	w->result.preview = NULL;
	ResetEvent(w->core.ready);
	if (w->core.busy) {
		CancelSynchronousIo(w->core.thread);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
}

WfWorkerStopReport wf_previewworker_stop(WfPreviewWorker *w)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	return wf_wc_stop(&w->core, 1000, true, worker_free, w);
}

WfWorkerStats wf_previewworker_stats(WfPreviewWorker *w)
{
	WfWorkerStats stats;
	AcquireSRWLockShared(&w->core.lock);
	stats = w->core.stats;
	ReleaseSRWLockShared(&w->core.lock);
	return stats;
}

HANDLE wf_previewworker_event(const WfPreviewWorker *w)
{
	return w->core.ready;
}
bool wf_previewworker_poll(WfPreviewWorker *w, WfPreviewResult *out)
{
	bool got;
	AcquireSRWLockExclusive(&w->core.lock);
	got = WAIT_OBJECT_0 == WaitForSingleObject(w->core.ready, 0);
	if (got) {
		*out = w->result;
		w->result.preview = NULL;
		ResetEvent(w->core.ready);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	return got;
}
static WfStatus submit(
        void *ctx, uint32_t gen, const wchar_t *path, bool explicit_load)
{
	WfPreviewWorker *w = ctx;
	wchar_t *copy;
	if (!path || wcslen(path) > 32760) {
		return WF_E_INVAL;
	}
	copy = wf_path_dup(path);
	if (!copy) {
		return WF_E_NOMEM;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		free(copy);
		return WF_E_INVAL;
	}
	if (w->path && UINT64_MAX != w->core.stats.superseded_requests) {
		w->core.stats.superseded_requests++;
	}
	if (w->result.preview &&
	        UINT64_MAX != w->core.stats.discarded_results) {
		w->core.stats.discarded_results++;
	}
	w->serial++;
	w->gen = gen;
	w->explicit_load = explicit_load;
	free(w->path);
	w->path = copy;
	wf_preview_free(w->result.preview);
	w->result.preview = NULL;
	ResetEvent(w->core.ready);
	if (w->core.busy) {
		CancelSynchronousIo(w->core.thread);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}
WfPreviewPort wf_previewworker_port(WfPreviewWorker *w)
{
	WfPreviewPort p = {w, submit, cancel};
	return p;
}

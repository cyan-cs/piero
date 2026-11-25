#include "wf_archive.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../path/wf_path.h"
#include "../base/wf_macro.h"
#include "../base/wf_mem.h"

#define MAX_ENTRIES_DEFAULT 500000u /* nodes, folders included */
#define MAX_COMPONENT 255u          /* UTF-16 units in one name */
#define MAX_PATH_UNITS 4096u /* UTF-16 units in a whole path in the archive */
#define MAX_DEPTH 64u        /* names in one path */
#define MAX_POOL_UNITS (32u << 20)    /* names in total (64 MiB of UTF-16) */
#define MAX_SCAN_DEFAULT (8ull << 30) /* decompressed bytes passed to index */
#define READ_BLOCK (64u * 1024u)
#define CACHE_SLOTS 8u
#define CACHE_BYTES_DEFAULT ((size_t)256u << 20)

enum
{
	N_DIR = 1u << 0,
	N_LINK = 1u
	        << 1, /* symbolic / hard link or a device: listed, never read */
	N_ENC = 1u << 2, /* needs a password */
	N_NOSIZE = 1u << 3,
	N_IMPLICIT = 1u
	        << 4 /* a folder no entry names, only implied by paths */
};

typedef struct
{
	uint32_t parent;
	uint32_t name;    /* offset into the pool, in wchar_t units */
	uint32_t ordinal; /* position in reading order; UINT32_MAX: none */
	uint16_t len;
	uint8_t flags;
	uint8_t pad;
	uint64_t size;
	uint64_t mtime; /* FILETIME */
} Node;

typedef struct
{
	FILE_ID_INFO id;
	bool id_valid;
	uint64_t size;
	uint64_t mtime;
} Stamp;

struct WfArchive
{
	LONG refs;
	Stamp stamp;
	WfArchiveInfo info;
	Node *nodes; /* nodes[0] is the top */
	size_t n, cap;
	UINT hdr_cp; /* code page of names that are not UTF-8; 0: all were */
	wchar_t *pool;
	size_t pool_n, pool_cap;
	uint32_t *hash; /* node index + 1; 0 is empty */
	size_t hash_cap;
	uint32_t *child_off; /* n + 1 offsets into child[] */
	uint32_t *child;
};

static volatile LONG g_codepage;
static volatile LONG64 g_cache_limit;
static volatile LONG64 g_max_entries = MAX_ENTRIES_DEFAULT;
static volatile LONG64 g_max_scan = (LONG64)MAX_SCAN_DEFAULT;

void wf_archive_set_limits(size_t max_entries, uint64_t max_scan_bytes)
{
	InterlockedExchange64(&g_max_entries,
	        max_entries ? (LONG64)max_entries
	                    : (LONG64)MAX_ENTRIES_DEFAULT);
	InterlockedExchange64(&g_max_scan,
	        max_scan_bytes ? (LONG64)max_scan_bytes
	                       : (LONG64)MAX_SCAN_DEFAULT);
}

void wf_archive_set_codepage(UINT codepage)
{
	InterlockedExchange(&g_codepage, (LONG)codepage);
}

void wf_archive_set_cache_limit(size_t bytes)
{
	InterlockedExchange64(&g_cache_limit, (LONG64)bytes);
}

static void index_free(WfArchive *a)
{
	if (!a) {
		return;
	}
	free(a->nodes);
	free(a->pool);
	free(a->hash);
	free(a->child_off);
	free(a->child);
	free(a);
}

static uint32_t name_hash(uint32_t parent, const wchar_t *s, size_t n)
{
	uint32_t h = 2166136261u ^ (parent * 16777619u);
	size_t i;

	for (i = 0; i < n; i++) {
		h = (h ^ (uint32_t)s[i]) * 16777619u;
	}
	return h;
}

static const wchar_t *node_name(const WfArchive *a, const Node *nd)
{
	return a->pool + nd->name;
}

/* The node `parent`/`s` or 0 (the top is never anybody's child). */
static uint32_t find_child(
        const WfArchive *a, uint32_t parent, const wchar_t *s, size_t n)
{
	size_t mask, i;

	if (!a->hash_cap) {
		return 0;
	}
	mask = a->hash_cap - 1;
	for (i = name_hash(parent, s, n) & mask;; i = (i + 1) & mask) {
		uint32_t v = a->hash[i];
		const Node *nd;

		if (!v) {
			return 0;
		}
		nd = &a->nodes[v - 1];
		if (nd->parent == parent && nd->len == n &&
		        !memcmp(node_name(a, nd), s, n * sizeof *s)) {
			return v - 1;
		}
	}
}

static void hash_put(WfArchive *a, uint32_t index)
{
	const Node *nd = &a->nodes[index];
	size_t mask = a->hash_cap - 1, i;

	for (i = name_hash(nd->parent, node_name(a, nd), nd->len) & mask;
	        a->hash[i]; i = (i + 1) & mask) {
	}
	a->hash[i] = index + 1;
}

static bool hash_grow(WfArchive *a)
{
	size_t cap = a->hash_cap ? a->hash_cap * 2 : 1024, i;
	uint32_t *h = calloc(cap, sizeof *h);

	if (!h) {
		return false;
	}
	free(a->hash);
	a->hash = h;
	a->hash_cap = cap;
	for (i = 1; i < a->n; i++) {
		hash_put(a, (uint32_t)i);
	}
	return true;
}

typedef enum
{
	ADD_OK,
	ADD_SKIP, /* this entry cannot be placed; the rest goes on */
	ADD_LIMIT,
	ADD_NOMEM
} AddResult;

/* A new node under `parent`. */
static AddResult new_node(WfArchive *a,
        uint32_t parent,
        const wchar_t *s,
        size_t n,
        uint8_t flags,
        uint32_t *out)
{
	Node *nd;

	if (a->n >= (size_t)g_max_entries || a->pool_n + n > MAX_POOL_UNITS) {
		return ADD_LIMIT;
	}
	if (a->n == a->cap) {
		size_t cap = a->cap ? a->cap * 2 : 256;
		Node *p = realloc(a->nodes, cap * sizeof *p);

		if (!p) {
			return ADD_NOMEM;
		}
		a->nodes = p;
		a->cap = cap;
	}
	if (a->pool_n + n > a->pool_cap) {
		size_t cap = a->pool_cap ? a->pool_cap * 2 : 4096;
		wchar_t *p;

		while (cap < a->pool_n + n) {
			cap *= 2;
		}
		p = realloc(a->pool, cap * sizeof *p);
		if (!p) {
			return ADD_NOMEM;
		}
		a->pool = p;
		a->pool_cap = cap;
	}
	if ((a->n + 1) * 2 > a->hash_cap && !hash_grow(a)) {
		return ADD_NOMEM;
	}
	if (n) {
		memcpy(a->pool + a->pool_n, s, n * sizeof *s);
	}
	nd = &a->nodes[a->n];
	memset(nd, 0, sizeof *nd);
	nd->parent = parent;
	nd->name = (uint32_t)a->pool_n;
	nd->len = (uint16_t)n;
	nd->flags = flags;
	nd->ordinal = UINT32_MAX;
	a->pool_n += n;
	*out = (uint32_t)a->n++;
	if (*out) { /* nobody looks the top up by name */
		hash_put(a, *out);
	}
	return ADD_OK;
}

typedef struct
{
	bool dir;
	uint8_t flags; /* N_LINK, N_ENC, N_NOSIZE */
	uint64_t size;
	uint64_t mtime;
	uint32_t ordinal;
} Meta;

static bool bad_char(wchar_t c)
{
	return c < 0x20 || 0x7F == c || NULL != wcschr(L"<>:\"|?*", c);
}

/*
 * Puts one entry of the archive at its place in the index. The path is cut at
 * '/' and '\'; empty and "." components are dropped; a ".." component makes
 * the entry unplaceable (it is skipped, never resolved upwards); characters a
 * Windows name cannot hold, and control characters, become U+FFFD so that no
 * component can contain '\', '|' or an escape sequence. A name already taken
 * by the other kind (file vs folder) skips the entry; two files of the same
 * name: the later one wins.
 */
static AddResult add_entry(
        WfArchive *a, const wchar_t *p, size_t n, const Meta *m)
{
	struct
	{
		size_t from, len;
	} comp[MAX_DEPTH];
	size_t nc = 0, i = 0, c;
	wchar_t name[MAX_COMPONENT];
	uint32_t cur = 0;

	if (n > MAX_PATH_UNITS) {
		return ADD_SKIP;
	}
	while (i < n) {
		size_t from = i;

		while (i < n && L'/' != p[i] && L'\\' != p[i]) {
			i++;
		}
		if (i - from == 2 && L'.' == p[from] && L'.' == p[from + 1]) {
			return ADD_SKIP;
		}
		if (i > from && !(i - from == 1 && L'.' == p[from])) {
			if (i - from > MAX_COMPONENT || MAX_DEPTH == nc) {
				return ADD_SKIP;
			}
			comp[nc].from = from;
			comp[nc].len = i - from;
			nc++;
		}
		i++;
	}
	if (!nc) {
		return ADD_SKIP; /* "./", "/": the top itself */
	}
	for (c = 0; c < nc; c++) {
		bool last = c + 1 == nc;
		uint32_t at;
		size_t k;

		for (k = 0; k < comp[c].len; k++) {
			wchar_t ch = p[comp[c].from + k];

			name[k] = bad_char(ch) ? (wchar_t)0xFFFD : ch;
		}
		at = find_child(a, cur, name, comp[c].len);
		if (!at) {
			uint8_t f = last
			        ? (uint8_t)((m->dir ? N_DIR : 0) | m->flags)
			        : (uint8_t)(N_DIR | N_IMPLICIT);
			AddResult r =
			        new_node(a, cur, name, comp[c].len, f, &at);

			if (ADD_OK != r) {
				return r;
			}
		} else if (!last) {
			if (!(a->nodes[at].flags & N_DIR)) {
				return ADD_SKIP; /* a file used as a folder */
			}
		} else if (!!(a->nodes[at].flags & N_DIR) != m->dir) {
			return ADD_SKIP; /* a file where a folder is, or back */
		} else if (m->dir) {
			a->nodes[at].flags =
			        (uint8_t)(a->nodes[at].flags & ~N_IMPLICIT);
		} else {
			a->nodes[at].flags = m->flags;
		}
		if (last) {
			Node *nd = &a->nodes[at];

			nd->size = m->size;
			nd->mtime = m->mtime;
			nd->ordinal = m->ordinal;
		}
		cur = at;
	}
	return ADD_OK;
}

/* The children of every node, in one array; called once, after the last
 * entry. */
static bool index_finish(WfArchive *a)
{
	size_t i;
	uint32_t *fill;

	a->child_off = calloc(a->n + 1, sizeof *a->child_off);
	a->child = wf_malloc_array((a->n ? a->n : 1), sizeof *a->child);
	fill = wf_malloc_array((a->n ? a->n : 1), sizeof *fill);
	if (!a->child_off || !a->child || !fill) {
		free(fill);
		return false;
	}
	for (i = 1; i < a->n; i++) {
		a->child_off[a->nodes[i].parent + 1]++;
	}
	for (i = 0; i < a->n; i++) {
		a->child_off[i + 1] += a->child_off[i];
	}
	memcpy(fill, a->child_off, a->n * sizeof *fill);
	for (i = 1; i < a->n; i++) {
		a->child[fill[a->nodes[i].parent]++] = (uint32_t)i;
	}
	free(fill);
	a->info.entries = a->n - 1;
	a->info.bytes = sizeof *a + a->cap * sizeof *a->nodes +
	        a->pool_cap * sizeof *a->pool + a->hash_cap * sizeof *a->hash +
	        (a->n + 1 + a->n) * sizeof *a->child;
	return true;
}

/* The node named by `inner` ("" is the top), or UINT32_MAX. */
static uint32_t resolve(const WfArchive *a, const wchar_t *inner)
{
	uint32_t cur = 0;
	const wchar_t *p = inner;

	while (*p) {
		const wchar_t *e = p;
		uint32_t at;

		while (*e && L'\\' != *e) {
			e++;
		}
		if (e == p) {
			return UINT32_MAX; /* "\\" doubled */
		}
		if ((size_t)(e - p) > MAX_COMPONENT) {
			return UINT32_MAX;
		}
		if (!(a->nodes[cur].flags & N_DIR)) {
			return UINT32_MAX;
		}
		at = find_child(a, cur, p, (size_t)(e - p));
		if (!at) {
			return UINT32_MAX;
		}
		cur = at;
		p = *e ? e + 1 : e;
	}
	return cur;
}

typedef struct
{
	HANDLE h;
	const WfArchiveCtl *ctl;
	bool stopped; /* the caller cancelled */
	DWORD io_error;
	uint64_t size;
	uint8_t buf[READ_BLOCK];
} Source;

static bool is_cancelled(Source *s)
{
	if (s->stopped) {
		return true;
	}
	if (s->ctl && s->ctl->cancelled && s->ctl->cancelled(s->ctl->ctx)) {
		s->stopped = true;
	}
	return s->stopped;
}

static la_ssize_t src_read(struct archive *ar, void *ctx, const void **buf)
{
	Source *s = ctx;
	DWORD got = 0;

	if (is_cancelled(s)) {
		archive_set_error(ar, ECANCELED, "cancelled");
		return -1;
	}
	if (!ReadFile(s->h, s->buf, READ_BLOCK, &got, NULL)) {
		s->io_error = GetLastError();
		if (!s->io_error) {
			s->io_error = ERROR_READ_FAULT;
		}
		archive_set_error(ar, EIO, "read error %lu", s->io_error);
		return -1;
	}
	*buf = s->buf;
	return (la_ssize_t)got;
}

static la_int64_t src_seek(
        struct archive *ar, void *ctx, la_int64_t off, int whence)
{
	Source *s = ctx;
	LARGE_INTEGER to, now;
	DWORD method = SEEK_SET == whence ? FILE_BEGIN
	        : SEEK_CUR == whence      ? FILE_CURRENT
	                                  : FILE_END;

	to.QuadPart = off;
	if (is_cancelled(s)) {
		archive_set_error(ar, ECANCELED, "cancelled");
		return ARCHIVE_FATAL;
	}
	if (!SetFilePointerEx(s->h, to, &now, method)) {
		s->io_error = GetLastError();
		if (!s->io_error) {
			s->io_error = ERROR_SEEK;
		}
		archive_set_error(ar, EIO, "seek error %lu", s->io_error);
		return ARCHIVE_FATAL;
	}
	return now.QuadPart;
}

static la_int64_t src_skip(struct archive *ar, void *ctx, la_int64_t request)
{
	Source *s = ctx;
	LARGE_INTEGER to, was, now;

	(void)ar;
	to.QuadPart = 0;
	if (request <= 0 || !SetFilePointerEx(s->h, to, &was, FILE_CURRENT)) {
		return 0;
	}
	to.QuadPart = request;
	if (!SetFilePointerEx(s->h, to, &now, FILE_CURRENT)) {
		return 0;
	}
	return now.QuadPart - was.QuadPart;
}

static int src_close(struct archive *ar, void *ctx)
{
	(void)ar;
	(void)ctx;
	return ARCHIVE_OK; /* the handle is the caller's */
}

static bool stamp_of(HANDLE h, Stamp *s)
{
	FILE_STANDARD_INFO st;
	FILE_BASIC_INFO b;

	memset(s, 0, sizeof *s);
	if (!GetFileInformationByHandleEx(
	            h, FileStandardInfo, &st, sizeof st) ||
	        !GetFileInformationByHandleEx(h, FileBasicInfo, &b, sizeof b)) {
		return false;
	}
	s->size = (uint64_t)st.EndOfFile.QuadPart;
	s->mtime = (uint64_t)b.LastWriteTime.QuadPart;
	s->id_valid = GetFileInformationByHandleEx(
	                      h, FileIdInfo, &s->id, sizeof s->id) != 0;
	return true;
}

static bool same_stamp(const Stamp *a, const Stamp *b)
{
	return a->size == b->size && a->mtime == b->mtime &&
	        a->id_valid == b->id_valid &&
	        (!a->id_valid ||
	                (a->id.VolumeSerialNumber == b->id.VolumeSerialNumber &&
	                        !memcmp(a->id.FileId.Identifier,
	                                b->id.FileId.Identifier, 16)));
}

static WfStatus win_status(DWORD e)
{
	switch (e) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_NAME:
	case ERROR_BAD_NETPATH:
	case ERROR_BAD_PATHNAME:      return WF_E_NOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:    return WF_E_ACCESS;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:       return WF_E_NOMEM;
	default:                      return WF_E_OS;
	}
}

/* Opens the archive file for reading. Not locked against writers: a change
 * while it is read is found by comparing the stamp afterwards. */
static WfStatus open_file(const wchar_t *file, HANDLE *h, Stamp *stamp)
{
	wchar_t *x = NULL;
	WfStatus st = wf_path_to_extended(file, &x);
	FILE_ATTRIBUTE_TAG_INFO tag;

	*h = INVALID_HANDLE_VALUE;
	if (WF_OK != st) {
		return WF_E_NOMEM == st ? st : WF_E_NOENT;
	}
	*h = CreateFileW(x, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_NO_RECALL |
	                FILE_FLAG_BACKUP_SEMANTICS,
	        NULL);
	DWORD open_error =
	        *h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
	free(x);
	if (INVALID_HANDLE_VALUE == *h) {
		return win_status(open_error);
	}
	if (FILE_TYPE_DISK != GetFileType(*h) ||
	        !GetFileInformationByHandleEx(
	                *h, FileAttributeTagInfo, &tag, sizeof tag) ||
	        (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
	        !stamp_of(*h, stamp)) {
		CloseHandle(*h);
		*h = INVALID_HANDLE_VALUE;
		return WF_E_NOENT;
	}
	return WF_OK;
}

static WfStatus archive_failure(struct archive *ar, const Source *src);

static struct archive *reader_new(Source *src, UINT hdr_cp, WfStatus *why)
{
	struct archive *ar = archive_read_new();
	char charset[32];

	snprintf(charset, sizeof charset,
	        hdr_cp ? "hdrcharset=CP%u" : "hdrcharset=UTF-8", hdr_cp);

	if (!ar) {
		*why = WF_E_NOMEM;
		return NULL;
	}
	/* the filters first, then the formats; "raw" last, it accepts anything
	 * and is only believed when a filter was undone */
	if (ARCHIVE_OK != archive_read_support_filter_gzip(ar) ||
	        ARCHIVE_OK != archive_read_support_filter_xz(ar) ||
	        ARCHIVE_OK != archive_read_support_filter_lzma(ar) ||
	        ARCHIVE_OK != archive_read_support_filter_zstd(ar) ||
	        ARCHIVE_OK != archive_read_support_format_zip(ar) ||
	        ARCHIVE_OK != archive_read_support_format_7zip(ar) ||
	        ARCHIVE_OK != archive_read_support_format_tar(ar) ||
	        ARCHIVE_OK != archive_read_support_format_raw(ar) ||
	        ARCHIVE_OK != archive_read_set_seek_callback(ar, src_seek) ||
	        /* Names are UTF-8 or they are not. A name that is not (older
		 * Windows tools write the system code page) cannot be converted
		 * by libarchive from "the locale", which it believes to be
		 * something single-byte; it is told the code page instead, but
		 * only after a first look found a name that was not UTF-8. */
	        archive_read_set_options(ar, charset) < ARCHIVE_WARN ||
	        archive_read_open2(ar, src, NULL, src_read, src_skip,
	                src_close) != ARCHIVE_OK) {
		*why = archive_failure(ar, src);
		archive_read_free(ar);
		return NULL;
	}
	return ar;
}

static bool message_has(const char *m, const char *needle)
{
	return m && NULL != strstr(m, needle);
}

/* Preserve allocation/cancellation errors; distinguish format and password
 * failures using libarchive's diagnostic text. */
static WfStatus archive_failure(struct archive *ar, const Source *src)
{
	const char *m = archive_error_string(ar);

	if (src->stopped) {
		return WF_E_CANCELLED;
	}
	if (src->io_error) {
		return win_status(src->io_error);
	}
	if (ENOMEM == archive_errno(ar)) {
		return WF_E_NOMEM;
	}
	if (message_has(m, "ncrypt") || message_has(m, "assphrase") ||
	        message_has(m, "assword")) {
		return WF_E_ENCRYPTED;
	}
	if (message_has(m, "Unrecognized") || message_has(m, "Unsupported") ||
	        message_has(m, "unsupported") || message_has(m, "Unknown")) {
		return WF_E_FORMAT;
	}
	return WF_E_CORRUPT;
}

/* UTF-8 (strict) or, failing that, the configured code page, to UTF-16. */
static wchar_t *to_wide(const char *s, size_t *n)
{
	UINT cps[2] = {CP_UTF8, (UINT)g_codepage};
	int units, k;
	wchar_t *w;

	if (!cps[1]) {
		cps[1] = CP_ACP;
	}
	for (k = 0; k < 2; k++) {
		units = MultiByteToWideChar(
		        cps[k], MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
		if (units <= 0) {
			continue;
		}
		w = wf_malloc_array((size_t)units, sizeof *w);
		if (!w) {
			return NULL;
		}
		if (MultiByteToWideChar(cps[k], MB_ERR_INVALID_CHARS, s, -1, w,
		            units) <= 0) {
			free(w);
			continue;
		}
		*n = (size_t)units - 1;
		return w;
	}
	/* neither: take what the code page can make of it */
	units = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
	if (units <= 0) {
		return NULL;
	}
	w = wf_malloc_array((size_t)units, sizeof *w);
	if (!w) {
		return NULL;
	}
	if (MultiByteToWideChar(CP_ACP, 0, s, -1, w, units) <= 0) {
		free(w);
		return NULL;
	}
	*n = (size_t)units - 1;
	return w;
}

static uint64_t to_filetime(struct archive_entry *e)
{
	int64_t t;
	int64_t ticks;
	long nanos;

	if (!archive_entry_mtime_is_set(e)) {
		return 0;
	}
	t = (int64_t)archive_entry_mtime(e);
	if (t < -11644473600ll || t > 910692730085ll) {
		return 0;
	}
	nanos = archive_entry_mtime_nsec(e);
	if (nanos < 0 || nanos >= 1000000000) {
		return 0;
	}
	ticks = (t + 11644473600ll) * 10000000ll;
	if ((int64_t)(nanos / 100) > INT64_MAX - ticks) {
		return 0;
	}
	ticks += (int64_t)(nanos / 100);
	return (uint64_t)ticks;
}

static uint8_t entry_flags(struct archive_entry *e, unsigned type)
{
	uint8_t f = 0;

	if (AE_IFREG != type && AE_IFDIR != type) {
		f |= N_LINK;
	}
	if (archive_entry_symlink(e) || archive_entry_hardlink(e)) {
		f |= N_LINK;
	}
	if (archive_entry_is_encrypted(e) ||
	        archive_entry_is_data_encrypted(e) ||
	        archive_entry_is_metadata_encrypted(e)) {
		f |= N_ENC;
	}
	if (!archive_entry_size_is_set(e)) {
		f |= N_NOSIZE;
	}
	return f;
}

static WfArchiveFormat format_of(struct archive *ar, bool *seekable)
{
	int f = archive_format(ar) & ARCHIVE_FORMAT_BASE_MASK;

	*seekable = false;
	if (ARCHIVE_FORMAT_ZIP == f) {
		*seekable = NULL != strstr(archive_format_name(ar), "seekable");
		return WF_AF_ZIP;
	}
	if (ARCHIVE_FORMAT_7ZIP == f) {
		return WF_AF_7Z;
	}
	if (ARCHIVE_FORMAT_RAW == f) {
		return WF_AF_SINGLE;
	}
	return WF_AF_TAR;
}

static void filters_text(struct archive *ar, char *out, size_t cap)
{
	int n = archive_filter_count(ar), i;
	size_t len = 0;

	out[0] = '\0';
	for (i = 0; i < n; i++) {
		const char *name;
		int w;

		if (ARCHIVE_FILTER_NONE == archive_filter_code(ar, i)) {
			continue;
		}
		name = archive_filter_name(ar, i);
		w = snprintf(out + len, cap - len, "%s%s", len ? "+" : "",
		        name ? name : "?");
		if (w < 0 || (size_t)w >= cap - len) {
			break;
		}
		len += (size_t)w;
	}
}

/* The name a single compressed file shows as: its own name without the
 * compression suffix. */
static size_t single_name(const wchar_t *file, wchar_t *out, size_t cap)
{
	const wchar_t *leaf;
	size_t len, i;
	static const wchar_t *const ext[] = {L".gz", L".xz", L".zst", L".lzma"};

	wf_path_leaf(file, &leaf, &len);
	for (i = 0; i < WF_ARRAY_LEN(ext); i++) {
		size_t e = wcslen(ext[i]);

		if (len > e && !_wcsnicmp(leaf + len - e, ext[i], e)) {
			len -= e;
			break;
		}
	}
	if (len >= cap) {
		len = cap - 1;
	}
	memcpy(out, leaf, len * sizeof *out);
	out[len] = L'\0';
	return len;
}

/* Empty headers and padding cost stream bytes even when declared file sizes are
 * zero. Keep both counters bounded, without overflowing the declared total. */
static bool scan_limit_exceeded(
        struct archive *ar, uint64_t *scanned, uint64_t declared)
{
	uint64_t limit = (uint64_t)g_max_scan;
	la_int64_t consumed = archive_filter_bytes(ar, 0);

	if (*scanned > limit || declared > limit - *scanned || consumed < 0 ||
	        (uint64_t)consumed > limit) {
		return true;
	}
	*scanned += declared;
	return false;
}

static WfStatus build_pass(const wchar_t *file,
        HANDLE h,
        const Stamp *stamp,
        const WfArchiveCtl *ctl,
        UINT hdr_cp,
        WfArchive **out,
        size_t *name_failures)
{
	Source *src = calloc(1, sizeof *src);
	struct archive *ar;
	struct archive_entry *e;
	WfArchive *a = calloc(1, sizeof *a);
	WfStatus result = WF_E_CORRUPT;
	uint64_t scanned = 0;
	uint32_t ordinal = 0;
	bool first = true, seekable = false;
	int r;
	Stamp after;

	*out = NULL;
	if (!src || !a) {
		free(src);
		free(a);
		return WF_E_NOMEM;
	}
	src->h = h;
	src->ctl = ctl;
	a->refs = 1;
	a->stamp = *stamp;
	a->hdr_cp = hdr_cp;
	/* node 0 is the top */
	{
		uint32_t top;
		AddResult ar0 = new_node(a, 0, L"", 0, N_DIR, &top);

		if (ADD_OK != ar0) {
			result = WF_E_NOMEM;
			goto fail_nofree;
		}
	}
	ar = reader_new(src, hdr_cp, &result);
	if (!ar) {
		goto fail_nofree;
	}
	for (;;) {
		wchar_t *w;
		wchar_t single[MAX_COMPONENT + 1];
		size_t wn = 0;
		const char *raw;
		unsigned type;
		Meta m;
		AddResult add;

		if (is_cancelled(src)) {
			result = WF_E_CANCELLED;
			goto fail;
		}
		r = archive_read_next_header(ar, &e);
		if (ARCHIVE_EOF == r) {
			if (!first &&
			        (WF_AF_ZIP != a->info.format || !seekable) &&
			        scan_limit_exceeded(ar, &scanned, 0)) {
				result = WF_E_RANGE;
				goto fail;
			}
			break;
		}
		if (ARCHIVE_OK != r && ARCHIVE_WARN != r) {
			result = archive_failure(ar, src);
			goto fail;
		}
		if (ordinal == UINT32_MAX ||
		        (uint64_t)ordinal >= (uint64_t)g_max_entries) {
			result = WF_E_RANGE;
			goto fail;
		}
		if (first) {
			a->info.format = format_of(ar, &seekable);
			a->info.seekable = seekable;
			filters_text(
			        ar, a->info.filters, sizeof a->info.filters);
			if (WF_AF_SINGLE == a->info.format &&
			        !a->info.filters[0]) {
				/* "raw" takes anything; only a stream that was
				 * decompressed is a single compressed file */
				result = WF_E_FORMAT;
				goto fail;
			}
			first = false;
		}
		type = (unsigned)archive_entry_filetype(e);
		memset(&m, 0, sizeof m);
		m.dir = AE_IFDIR == type;
		m.flags = entry_flags(e, type);
		m.size = (!m.dir && archive_entry_size_is_set(e) &&
		                 archive_entry_size(e) > 0)
		        ? (uint64_t)archive_entry_size(e)
		        : 0;
		m.mtime = to_filetime(e);
		m.ordinal = ordinal++;
		/* a stream that has to be decompressed to get past its entries
		 * is only followed so far (a zip bomb is a few kilobytes) */
		if (WF_AF_ZIP != a->info.format || !seekable) {
			if (scan_limit_exceeded(ar, &scanned, m.size)) {
				result = WF_E_RANGE;
				goto fail;
			}
		}
		if (WF_AF_SINGLE == a->info.format) {
			wn = single_name(file, single, MAX_COMPONENT + 1);
			if (!wn) {
				wcscpy_s(single, MAX_COMPONENT + 1, L"data");
				wn = 4;
			}
			add = add_entry(a, single, wn, &m);
		} else {
			raw = archive_entry_pathname_utf8(e);
			if (!raw) {
				raw = archive_entry_pathname(e);
			}
			if (!raw || !*raw) {
				a->info.skipped++;
				(*name_failures)++;
				continue;
			}
			w = to_wide(raw, &wn);
			if (!w) {
				result = WF_E_NOMEM;
				goto fail;
			}
			add = add_entry(a, w, wn, &m);
			free(w);
		}
		if (ADD_SKIP == add) {
			a->info.skipped++;
		} else if (ADD_LIMIT == add) {
			result = WF_E_RANGE;
			goto fail;
		} else if (ADD_NOMEM == add) {
			result = WF_E_NOMEM;
			goto fail;
		}
		if (WF_AF_SINGLE == a->info.format) {
			break; /* one stream, one file */
		}
	}
	if (first) {
		/* no entry at all: an empty ZIP / tar is real, but "raw" and
		 * "nothing recognised" are not */
		int f = archive_format(ar) & ARCHIVE_FORMAT_BASE_MASK;

		if (0 == f || ARCHIVE_FORMAT_RAW == f ||
		        ARCHIVE_FORMAT_EMPTY == f) {
			result = WF_E_FORMAT;
			goto fail;
		}
		a->info.format = format_of(ar, &seekable);
		a->info.seekable = seekable;
		filters_text(ar, a->info.filters, sizeof a->info.filters);
	}
	archive_read_free(ar);
	ar = NULL;
	/* the file must be the same one at the end as at the start */
	if (!stamp_of(h, &after) || !same_stamp(stamp, &after)) {
		result = WF_E_CHANGED;
		goto fail_nofree;
	}
	if (!index_finish(a)) {
		result = WF_E_NOMEM;
		goto fail_nofree;
	}
	free(src);
	*out = a;
	return WF_OK;
fail:
	archive_read_free(ar);
fail_nofree:
	free(src);
	index_free(a);
	return result;
}

/* Builds the index. Names are read as UTF-8; if some are not, the archive is
 * read again with the configured code page for those. */
static WfStatus build_index(const wchar_t *file,
        HANDLE h,
        const Stamp *stamp,
        const WfArchiveCtl *ctl,
        WfArchive **out)
{
	size_t failures = 0;
	UINT cp = (UINT)g_codepage ? (UINT)g_codepage : GetACP();
	WfStatus st = build_pass(file, h, stamp, ctl, 0, out, &failures);

	if (WF_OK == st && failures && CP_UTF8 != cp) {
		WfArchive *again = NULL;
		size_t ignored = 0;

		st = build_pass(file, h, stamp, ctl, cp, &again, &ignored);
		if (WF_OK == st) {
			index_free(*out);
			*out = again;
		} else if (WF_E_FORMAT == st || WF_E_CORRUPT == st ||
		        WF_E_ENCRYPTED == st) {
			st = WF_OK; /* the first look stands: its names that
			               worked, the rest skipped */
		} else {
			index_free(*out);
			*out = NULL;
		}
	}
	return st;
}

typedef struct
{
	wchar_t *path; /* NULL: free */
	Stamp stamp;
	WfArchive *a; /* NULL while building or after a failure */
	WfStatus status;
	bool building;
	uint64_t used;
} Slot;

static struct
{
	SRWLOCK lock;
	CONDITION_VARIABLE cv;
	Slot slots[CACHE_SLOTS];
	uint64_t tick;
	uint64_t used_ms; /* GetTickCount64 of the last open or touch */
} g_cache = {SRWLOCK_INIT, CONDITION_VARIABLE_INIT, {{0}}, 0, 0};

void wf_archive_release(WfArchive *a)
{
	if (a && 0 == InterlockedDecrement(&a->refs)) {
		index_free(a);
	}
}

WfArchiveInfo wf_archive_info(const WfArchive *a)
{
	return a->info;
}

static void slot_clear(Slot *s)
{
	wf_archive_release(s->a);
	free(s->path);
	memset(s, 0, sizeof *s);
}

static size_t cache_limit(void)
{
	LONG64 v = g_cache_limit;

	return v > 0 ? (size_t)v : CACHE_BYTES_DEFAULT;
}

/* Caller holds the lock. Drops least recently used indexes (never one that
 * is being built, never `keep`) until the rest fits. */
static void cache_trim(const Slot *keep)
{
	for (;;) {
		size_t total = 0, i;
		Slot *victim = NULL;

		for (i = 0; i < CACHE_SLOTS; i++) {
			Slot *s = &g_cache.slots[i];

			if (s->a) {
				total += s->a->info.bytes;
			}
			if (s->path && !s->building && s != keep &&
			        (!victim || s->used < victim->used)) {
				victim = s;
			}
		}
		if (total <= cache_limit() || !victim) {
			return;
		}
		slot_clear(victim);
	}
}

void wf_archive_cache_clear(void)
{
	size_t i;

	AcquireSRWLockExclusive(&g_cache.lock);
	for (i = 0; i < CACHE_SLOTS; i++) {
		if (g_cache.slots[i].path && !g_cache.slots[i].building) {
			slot_clear(&g_cache.slots[i]);
		}
	}
	ReleaseSRWLockExclusive(&g_cache.lock);
}

void wf_archive_cache_touch(uint64_t now_ms)
{
	AcquireSRWLockExclusive(&g_cache.lock);
	g_cache.used_ms = now_ms;
	ReleaseSRWLockExclusive(&g_cache.lock);
}

uint64_t wf_archive_cache_idle_wait(uint64_t now_ms, uint64_t idle_ms)
{
	bool kept = false, building = false;
	uint64_t gone;
	size_t i;

	AcquireSRWLockShared(&g_cache.lock);
	gone = now_ms > g_cache.used_ms ? now_ms - g_cache.used_ms : 0;
	for (i = 0; i < CACHE_SLOTS; i++) {
		kept |= NULL != g_cache.slots[i].path;
		building |= g_cache.slots[i].building;
	}
	ReleaseSRWLockShared(&g_cache.lock);
	if (!kept) {
		return UINT64_MAX;
	}
	/* a build in progress is use: look again after a whole period */
	return gone >= idle_ms ? (building ? idle_ms : 0) : idle_ms - gone;
}

static bool same_path(const wchar_t *a, const wchar_t *b)
{
	return CSTR_EQUAL == CompareStringOrdinal(a, -1, b, -1, TRUE);
}

static bool ctl_cancelled(const WfArchiveCtl *ctl)
{
	return ctl && ctl->cancelled && ctl->cancelled(ctl->ctx);
}

WfStatus wf_archive_open(
        const wchar_t *file, const WfArchiveCtl *ctl, WfArchive **out)
{
	HANDLE h;
	Stamp stamp;
	WfStatus st;
	Slot *mine = NULL;
	WfArchive *built = NULL;
	size_t i;

	if (!file || !out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	st = open_file(file, &h, &stamp);
	if (WF_OK != st) {
		return st;
	}
	AcquireSRWLockExclusive(&g_cache.lock);
	while (!mine) {
		Slot *hit = NULL, *free_slot = NULL, *lru = NULL;

		for (i = 0; i < CACHE_SLOTS; i++) {
			Slot *s = &g_cache.slots[i];

			if (!s->path) {
				free_slot = free_slot ? free_slot : s;
			} else if (same_path(s->path, file)) {
				hit = s;
			} else if (!s->building &&
			        (!lru || s->used < lru->used)) {
				lru = s;
			}
		}
		if (hit && hit->building) {
			if (ctl_cancelled(ctl)) {
				ReleaseSRWLockExclusive(&g_cache.lock);
				CloseHandle(h);
				return WF_E_CANCELLED;
			}
			SleepConditionVariableSRW(
			        &g_cache.cv, &g_cache.lock, 30, 0);
			continue;
		}
		if (hit && same_stamp(&hit->stamp, &stamp)) {
			hit->used = ++g_cache.tick;
			g_cache.used_ms = GetTickCount64();
			if (hit->a) {
				InterlockedIncrement(&hit->a->refs);
				*out = hit->a;
				st = WF_OK;
			} else {
				st = hit->status; /* remembered failure */
			}
			ReleaseSRWLockExclusive(&g_cache.lock);
			CloseHandle(h);
			return st;
		}
		if (hit) {
			slot_clear(hit); /* the file is not the one indexed */
			free_slot = hit;
		}
		if (!free_slot) {
			if (!lru) { /* every slot is busy: wait for one */
				if (ctl_cancelled(ctl)) {
					ReleaseSRWLockExclusive(&g_cache.lock);
					CloseHandle(h);
					return WF_E_CANCELLED;
				}
				SleepConditionVariableSRW(
				        &g_cache.cv, &g_cache.lock, 30, 0);
				continue;
			}
			slot_clear(lru);
			free_slot = lru;
		}
		free_slot->path = wf_path_dup(file);
		if (!free_slot->path) {
			ReleaseSRWLockExclusive(&g_cache.lock);
			CloseHandle(h);
			return WF_E_NOMEM;
		}
		free_slot->stamp = stamp;
		free_slot->building = true;
		free_slot->used = ++g_cache.tick;
		mine = free_slot;
	}
	ReleaseSRWLockExclusive(&g_cache.lock);

	st = build_index(file, h, &stamp, ctl, &built);
	CloseHandle(h);

	AcquireSRWLockExclusive(&g_cache.lock);
	mine->building = false;
	g_cache.used_ms = GetTickCount64();
	if (WF_OK == st) {
		mine->a = built;
		InterlockedIncrement(&built->refs);
		mine->status = WF_OK;
		cache_trim(mine);
		*out = built;
	} else if (WF_E_CANCELLED == st || WF_E_NOMEM == st ||
	        WF_E_CHANGED == st || WF_E_OS == st || WF_E_ACCESS == st) {
		slot_clear(mine); /* not about this file: try again next time */
	} else {
		mine->status = st; /* about this file: remembered */
	}
	ReleaseSRWLockExclusive(&g_cache.lock);
	WakeAllConditionVariable(&g_cache.cv);
	return st;
}

/* The cached index of `file` is not to be used again. */
static void cache_forget(const wchar_t *file)
{
	size_t i;

	AcquireSRWLockExclusive(&g_cache.lock);
	for (i = 0; i < CACHE_SLOTS; i++) {
		if (g_cache.slots[i].path && !g_cache.slots[i].building &&
		        same_path(g_cache.slots[i].path, file)) {
			slot_clear(&g_cache.slots[i]);
		}
	}
	ReleaseSRWLockExclusive(&g_cache.lock);
}

WfStatus wf_archive_list(
        const WfArchive *a, const wchar_t *inner, size_t limit, WfDir **out)
{
	uint32_t at;
	WfDir *d;
	size_t k;

	if (!a || !inner || !out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	at = resolve(a, inner);
	if (UINT32_MAX == at || !(a->nodes[at].flags & N_DIR)) {
		return WF_E_NOENT;
	}
	d = wf_dir_new();
	if (!d) {
		return WF_E_NOMEM;
	}
	d->limit = limit;
	for (k = a->child_off[at]; k < a->child_off[at + 1]; k++) {
		const Node *nd = &a->nodes[a->child[k]];
		uint32_t flags = (nd->flags & N_DIR) ? WF_ENT_DIR : 0;
		WfStatus st;

		if (nd->flags & N_LINK) {
			flags |= WF_ENT_LINK;
		}
		st = wf_dir_add(d, node_name(a, nd), nd->len, flags,
		        (nd->flags & N_DIR) ? 0 : nd->size, nd->mtime);
		if (WF_OK != st) {
			wf_dir_free(d);
			return st;
		}
		/* a name inside an archive is never entered as another one */
		d->e[d->n - 1].flags &= ~(uint32_t)WF_ENT_ARCHIVE;
	}
	*out = d;
	return WF_OK;
}

WfStatus wf_vpath_list(const wchar_t *vpath,
        size_t limit,
        const WfArchiveCtl *ctl,
        WfDir **out)
{
	wchar_t *file;
	const wchar_t *inner;
	WfArchive *a;
	WfStatus st;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	if (!wf_vpath_split(vpath, &file, &inner)) {
		return WF_E_INVAL;
	}
	st = wf_archive_open(file, ctl, &a);
	free(file);
	if (WF_OK != st) {
		return st;
	}
	st = wf_archive_list(a, inner, limit, out);
	wf_archive_release(a);
	return st;
}

void wf_archive_data_free(WfArchiveData *d)
{
	if (!d) {
		return;
	}
	free(d->bytes);
	memset(d, 0, sizeof *d);
}

WfStatus wf_vpath_read(const wchar_t *vpath,
        size_t max_bytes,
        const WfArchiveCtl *ctl,
        WfArchiveData *out)
{
	wchar_t *file;
	const wchar_t *inner;
	WfArchive *a = NULL;
	Source *src = NULL;
	struct archive *ar = NULL;
	struct archive_entry *e;
	HANDLE h = INVALID_HANDLE_VALUE;
	Stamp stamp;
	WfStatus st;
	uint32_t at;
	Node node;
	uint32_t ordinal = 0;
	uint64_t scanned = 0;
	int r;
	size_t got = 0;
	uint8_t *buf = NULL;

	if (!out) {
		return WF_E_INVAL;
	}
	memset(out, 0, sizeof *out);
	if (!max_bytes || max_bytes > WF_ARCHIVE_ENTRY_MAX ||
	        !wf_vpath_split(vpath, &file, &inner)) {
		return WF_E_INVAL;
	}
	st = wf_archive_open(file, ctl, &a);
	if (WF_OK != st) {
		free(file);
		return st;
	}
	at = resolve(a, inner);
	if (UINT32_MAX == at) {
		st = WF_E_NOENT;
		goto done;
	}
	node = a->nodes[at];
	if (node.flags & N_DIR) {
		st = WF_E_INVAL;
		goto done;
	}
	if (node.flags & N_LINK) {
		st = WF_E_ACCESS;
		goto done;
	}
	if (node.flags & N_ENC) {
		st = WF_E_ENCRYPTED;
		goto done;
	}
	st = open_file(file, &h, &stamp);
	if (WF_OK != st) {
		goto done;
	}
	if (!same_stamp(&stamp, &a->stamp)) {
		cache_forget(file);
		st = WF_E_CHANGED;
		goto done;
	}
	src = calloc(1, sizeof *src);
	if (!src) {
		st = WF_E_NOMEM;
		goto done;
	}
	src->h = h;
	src->ctl = ctl;
	ar = reader_new(src, a->hdr_cp, &st);
	if (!ar) {
		goto done;
	}
	for (;;) {
		if (is_cancelled(src)) {
			st = WF_E_CANCELLED;
			goto done;
		}
		r = archive_read_next_header(ar, &e);
		if (ARCHIVE_EOF == r) {
			st = WF_E_CHANGED; /* the index named an entry that is not there */
			goto done;
		}
		if (ARCHIVE_OK != r && ARCHIVE_WARN != r) {
			st = archive_failure(ar, src);
			goto done;
		}
		if (WF_AF_ZIP != a->info.format || !a->info.seekable) {
			uint64_t declared = archive_entry_size_is_set(e) &&
			                archive_entry_size(e) > 0
			        ? (uint64_t)archive_entry_size(e)
			        : 0;

			if (scan_limit_exceeded(ar, &scanned, declared)) {
				st = WF_E_RANGE;
				goto done;
			}
		}
		if (ordinal++ == node.ordinal) {
			break;
		}
	}
	/* the entry must be what the index recorded */
	if (!!(AE_IFDIR == archive_entry_filetype(e)) !=
	                !!(node.flags & N_DIR) ||
	        (archive_entry_size_is_set(e) && !(node.flags & N_NOSIZE) &&
	                (uint64_t)archive_entry_size(e) != node.size)) {
		st = WF_E_CHANGED;
		goto done;
	}
	if (archive_entry_is_encrypted(e) ||
	        archive_entry_is_data_encrypted(e)) {
		st = WF_E_ENCRYPTED;
		goto done;
	}
	out->size = node.size;
	out->size_known = !(node.flags & N_NOSIZE);
	out->format = a->info.format;
	buf = malloc(max_bytes + 1u);
	if (!buf) {
		st = WF_E_NOMEM;
		goto done;
	}
	for (;;) {
		la_ssize_t k;

		if (is_cancelled(src)) {
			st = WF_E_CANCELLED;
			goto done;
		}
		k = archive_read_data(ar, buf + got, max_bytes + 1u - got);
		if (k < 0) {
			st = archive_failure(ar, src);
			goto done;
		}
		if (0 == k) {
			break;
		}
		got += (size_t)k;
		if (got > max_bytes) {
			break;
		}
	}
	out->truncated = got > max_bytes;
	out->n = out->truncated ? max_bytes : got;
	out->verified = !out->truncated;
	out->bytes = buf;
	buf = NULL;
	if (!stamp_of(h, &stamp) || !same_stamp(&stamp, &a->stamp)) {
		wf_archive_data_free(out);
		st = WF_E_CHANGED;
		goto done;
	}
	st = WF_OK;
done:
	free(buf);
	if (ar) {
		archive_read_free(ar);
	}
	free(src);
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	wf_archive_release(a);
	free(file);
	if (WF_OK != st) {
		wf_archive_data_free(out);
	}
	return st;
}

#define EX_BLOCK (256u * 1024u)
#define EX_PATH_MAX 32000u /* UTF-16 units: what the file system can take */

WfExtractLimits wf_extract_default_limits(void)
{
	WfExtractLimits l;

	l.max_file_bytes = 16ull << 30;
	l.max_total_bytes = 64ull << 30;
	l.max_files = 200000;
	l.max_name_bytes = (size_t)64 << 20;
	return l;
}

/* Windows device names, with or without an extension, in any letter case. */
static bool reserved_device(const wchar_t *s, size_t n)
{
	static const wchar_t *const plain[] = {
	        L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
	wchar_t up[8];
	size_t stem = 0, k;

	while (stem < n && L'.' != s[stem]) {
		stem++;
	}
	while (stem && L' ' == s[stem - 1]) {
		stem--; /* "NUL .txt" is the device too */
	}
	if (!stem || stem > 6) {
		return false;
	}
	for (k = 0; k < stem; k++) {
		up[k] = s[k];
	}
	up[stem] = 0;
	CharUpperBuffW(up, (DWORD)stem);
	for (k = 0; k < WF_ARRAY_LEN(plain); k++) {
		if (!wcscmp(up, plain[k])) {
			return true;
		}
	}
	return 4 == stem &&
	        (!wcsncmp(up, L"COM", 3) || !wcsncmp(up, L"LPT", 3)) &&
	        up[3] >= L'1' && up[3] <= L'9';
}

/*
 * Whether a name of the archive may become a path below the destination as it
 * is. `p` is the raw name; its separators are '/' and '\' (as the index reads
 * them). Components that are empty or "." are dropped, as in the index; ".."
 * is refused (the index never places such an entry, this is the same rule
 * stated again where the data is written).
 */
static bool name_is_safe(const wchar_t *p, size_t n)
{
	size_t i = 0, comps = 0;

	if (!n || n > EX_PATH_MAX || L'/' == p[0] || L'\\' == p[0]) {
		return false; /* absolute, or UNC */
	}
	while (i < n) {
		size_t from = i, k;

		while (i < n && L'/' != p[i] && L'\\' != p[i]) {
			i++;
		}
		if (i > from && !(1 == i - from && L'.' == p[from])) {
			if (2 == i - from && !wmemcmp(p + from, L"..", 2)) {
				return false; /* the same rule as add_entry's */
			}
			if (i - from > MAX_COMPONENT || L'.' == p[i - 1] ||
			        L' ' == p[i - 1] ||
			        reserved_device(p + from, i - from)) {
				return false;
			}
			for (k = from; k < i; k++) {
				if (bad_char(p[k]) || 0xFFFD == p[k]) {
					return false; /* ':' also covers a drive and a stream */
				}
			}
			comps++;
		}
		i++;
	}
	return comps > 0 && comps <= MAX_DEPTH;
}

typedef struct
{
	wchar_t *key;   /* upper-cased */
	wchar_t *exact; /* as it was written */
} CaseSlot;

typedef struct
{
	CaseSlot *v;
	size_t cap, n;
	size_t bytes, max_bytes;
} CaseSet;

static uint64_t key_hash(const wchar_t *s)
{
	uint64_t h = 1469598103934665603ull;

	for (; *s; s++) {
		h = (h ^ (uint64_t)*s) * 1099511628211ull;
	}
	return h;
}

static void caseset_free(CaseSet *c)
{
	for (size_t i = 0; i < c->cap; i++) {
		free(c->v[i].key);
		free(c->v[i].exact);
	}
	free(c->v);
	memset(c, 0, sizeof *c);
}

static bool caseset_grow(CaseSet *c)
{
	size_t cap = c->cap ? c->cap * 2 : 1024;
	CaseSlot *v = calloc(cap, sizeof *v);

	if (!v) {
		return false;
	}
	for (size_t i = 0; i < c->cap; i++) {
		size_t k;

		if (!c->v[i].key) {
			continue;
		}
		for (k = key_hash(c->v[i].key) & (cap - 1); v[k].key;
		        k = (k + 1) & (cap - 1)) {
		}
		v[k] = c->v[i];
	}
	free(c->v);
	c->v = v;
	c->cap = cap;
	return true;
}

/* 1: new, or the same spelling as before; 0: another spelling of a name that
 * is taken; -1: no memory, or over the budget. */
static int caseset_put(CaseSet *c, const wchar_t *s, size_t n)
{
	wchar_t *key = wf_malloc_array(n + 1, sizeof *key);
	size_t k;

	if (!key) {
		return -1;
	}
	memcpy(key, s, n * sizeof *key);
	key[n] = 0;
	CharUpperBuffW(key, (DWORD)n);
	if ((c->n + 1) * 2 > c->cap && !caseset_grow(c)) {
		free(key);
		return -1;
	}
	for (k = key_hash(key) & (c->cap - 1); c->v[k].key;
	        k = (k + 1) & (c->cap - 1)) {
		if (!wcscmp(c->v[k].key, key)) {
			bool same = wcslen(c->v[k].exact) == n &&
			        !wcsncmp(c->v[k].exact, s, n);

			free(key);
			return same ? 1 : 0;
		}
	}
	c->bytes += (n + 1) * 2 * sizeof *key + 2 * sizeof(CaseSlot);
	if (c->bytes > c->max_bytes) {
		free(key);
		return -1;
	}
	c->v[k].exact = wf_malloc_array(n + 1, sizeof *c->v[k].exact);
	if (!c->v[k].exact) {
		free(key);
		return -1;
	}
	memcpy(c->v[k].exact, s, n * sizeof *s);
	c->v[k].exact[n] = 0;
	c->v[k].key = key;
	c->n++;
	return 1;
}

/* Every prefix of `rel` (cut at '\') must be new or spelled as before. */
static int caseset_put_path(CaseSet *c, const wchar_t *rel, size_t n)
{
	for (size_t i = 0; i <= n; i++) {
		if (i == n || L'\\' == rel[i]) {
			int r = caseset_put(c, rel, i);

			if (r <= 0) {
				return r;
			}
		}
	}
	return 1;
}

typedef struct
{
	uint32_t *owner;  /* per node: the item it belongs to, or UINT32_MAX */
	uint32_t *by_ord; /* per entry in reading order: its node */
	size_t n_ord;
	uint32_t *roots; /* the items, no item inside another */
	size_t n_roots;
} Plan;

static void plan_free(Plan *p)
{
	free(p->owner);
	free(p->by_ord);
	free(p->roots);
	memset(p, 0, sizeof *p);
}

static bool is_below(const WfArchive *a, uint32_t node, const uint8_t *chosen)
{
	for (uint32_t at = node; at; at = a->nodes[at].parent) {
		if (at != node && chosen[at]) {
			return true;
		}
	}
	return false;
}

static WfStatus plan_build(const WfArchive *a,
        const wchar_t *const *inner,
        size_t n_inner,
        Plan *p,
        WfExtractStats *st)
{
	uint8_t *chosen = calloc(a->n ? a->n : 1, 1);
	uint32_t *stack = wf_malloc_array(a->n ? a->n : 1, sizeof *stack);
	WfStatus res = WF_OK;
	size_t i, sp;
	uint32_t maxord = 0;
	bool any_ord = false;

	memset(p, 0, sizeof *p);
	p->owner = wf_malloc_array(a->n ? a->n : 1, sizeof *p->owner);
	p->roots = wf_malloc_array(a->n ? a->n : 1, sizeof *p->roots);
	if (!chosen || !stack || !p->owner || !p->roots) {
		res = WF_E_NOMEM;
		goto done;
	}
	for (i = 0; i < a->n; i++) {
		p->owner[i] = UINT32_MAX;
	}
	for (i = 0; i < n_inner; i++) {
		uint32_t at = resolve(a, inner[i]);

		if (UINT32_MAX == at) {
			res = WF_E_NOENT;
			goto done;
		}
		if (0 == at) { /* the whole archive: its top-level items */
			for (uint32_t k = a->child_off[0]; k < a->child_off[1];
			        k++) {
				chosen[a->child[k]] = 1;
			}
		} else {
			chosen[at] = 1;
		}
	}
	for (i = 1; i < a->n; i++) {
		if (chosen[i] && !is_below(a, (uint32_t)i, chosen)) {
			p->roots[p->n_roots++] = (uint32_t)i;
		}
	}
	/* every node below an item belongs to it */
	for (i = 0; i < p->n_roots; i++) {
		sp = 0;
		stack[sp++] = p->roots[i];
		while (sp) {
			uint32_t at = stack[--sp];
			const Node *nd = &a->nodes[at];

			p->owner[at] = p->roots[i];
			if (nd->flags & N_DIR) {
				st->declared_dirs++;
				for (uint32_t k = a->child_off[at];
				        k < a->child_off[at + 1]; k++) {
					stack[sp++] = a->child[k];
				}
			} else {
				st->declared_files++;
				st->declared_bytes += nd->size;
			}
			if (UINT32_MAX != nd->ordinal) {
				any_ord = true;
				if (nd->ordinal > maxord) {
					maxord = nd->ordinal;
				}
			}
		}
	}
	/* the entries in reading order, to the node each one became */
	p->n_ord = any_ord ? (size_t)maxord + 1 : 0;
	p->by_ord = wf_malloc_array(p->n_ord ? p->n_ord : 1, sizeof *p->by_ord);
	if (!p->by_ord) {
		res = WF_E_NOMEM;
		goto done;
	}
	for (i = 0; i < p->n_ord; i++) {
		p->by_ord[i] = UINT32_MAX;
	}
	for (i = 1; i < a->n; i++) {
		uint32_t o = a->nodes[i].ordinal;

		if (UINT32_MAX != p->owner[i] && UINT32_MAX != o &&
		        o < p->n_ord) {
			p->by_ord[o] = (uint32_t)i;
		}
	}
done:
	free(chosen);
	free(stack);
	if (WF_OK != res) {
		plan_free(p);
	}
	return res;
}

/* The path of `node` below the destination: from its item down, '\' between
 * names. Returns the length, 0 if it would not fit. */
static size_t rel_of(const WfArchive *a,
        const Plan *p,
        uint32_t node,
        wchar_t *out,
        size_t cap)
{
	uint32_t chain[MAX_DEPTH + 1];
	size_t n = 0, len = 0;

	for (uint32_t at = node;; at = a->nodes[at].parent) {
		if (n == MAX_DEPTH + 1) {
			return 0;
		}
		chain[n++] = at;
		if (at == p->owner[node]) {
			break;
		}
	}
	while (n) {
		const Node *nd = &a->nodes[chain[--n]];

		if (len + nd->len + 2 > cap) {
			return 0;
		}
		if (len) {
			out[len++] = L'\\';
		}
		memcpy(out + len, node_name(a, nd), nd->len * sizeof *out);
		len += nd->len;
	}
	out[len] = 0;
	return len;
}

static WfStatus extract_open_index(
        const wchar_t *file, const WfArchiveCtl *ctl, WfArchive **a)
{
	WfStatus st = wf_archive_open(file, ctl, a);

	if (WF_OK != st) {
		return st;
	}
	if (WF_AF_ZIP != (*a)->info.format) {
		wf_archive_release(*a);
		*a = NULL;
		return WF_E_FORMAT;
	}
	return WF_OK;
}

WfStatus wf_archive_extract_measure(const wchar_t *file,
        const wchar_t *const *inner,
        size_t n_inner,
        const WfArchiveCtl *ctl,
        WfExtractStats *out)
{
	WfArchive *a = NULL;
	Plan p;
	WfStatus st;

	if (!file || !out || (n_inner && !inner)) {
		return WF_E_INVAL;
	}
	memset(out, 0, sizeof *out);
	st = extract_open_index(file, ctl, &a);
	if (WF_OK != st) {
		return st;
	}
	st = plan_build(a, inner, n_inner, &p, out);
	if (WF_OK == st) {
		plan_free(&p);
		out->unplaced = a->info.skipped;
	}
	wf_archive_release(a);
	return st;
}

static void raw_name_of(struct archive_entry *e, wchar_t **w, size_t *wn)
{
	const char *raw = archive_entry_pathname_utf8(e);

	if (!raw) {
		raw = archive_entry_pathname(e);
	}
	*w = (raw && *raw) ? to_wide(raw, wn) : NULL;
}

typedef struct
{
	const WfExtractSink *sink;
	WfExtractStats *st;
} ExOut;

static void ex_issue(ExOut *o, const wchar_t *rel, WfExtractIssue kind)
{
	o->st->issues++;
	if (o->sink->issue) {
		o->sink->issue(o->sink->ctx, rel, kind);
	}
}

/* Reads the data of the current entry into the sink. Returns the act that
 * ends it; *kind says why when it is not CONTINUE. */
static WfExtractAct ex_stream(struct archive *ar,
        Source *src,
        const WfExtractSink *sink,
        const WfExtractLimits *lim,
        WfExtractStats *st,
        uint8_t *buf,
        uint64_t declared,
        bool size_known,
        WfExtractIssue *kind,
        WfStatus *stop)
{
	uint64_t got = 0;

	for (;;) {
		la_ssize_t k;

		if (is_cancelled(src) ||
		        (sink->cancelled && sink->cancelled(sink->ctx))) {
			*stop = WF_E_CANCELLED;
			return WF_EX_ABORT;
		}
		k = archive_read_data(ar, buf, EX_BLOCK);
		if (k < 0) {
			const char *m = archive_error_string(ar);

			*kind = message_has(m, "CRC") ? WF_XI_CHECKSUM
			        : (message_has(m, "ncrypt") ||
			                  message_has(m, "assword"))
			        ? WF_XI_ENCRYPTED
			        : WF_XI_READ;
			return WF_EX_SKIP;
		}
		if (0 == k) {
			break;
		}
		got += (uint64_t)k;
		/* the bytes that really come out are what is counted, whatever the
		 * entry says it holds */
		if (got > lim->max_file_bytes) {
			*kind = WF_XI_TOO_BIG;
			return WF_EX_SKIP;
		}
		if (st->bytes + (uint64_t)k > lim->max_total_bytes) {
			*stop = WF_E_RANGE;
			return WF_EX_ABORT;
		}
		if (size_known && got > declared) {
			*kind = WF_XI_SIZE;
			return WF_EX_SKIP;
		}
		{
			WfExtractAct act =
			        sink->file_data(sink->ctx, buf, (size_t)k);

			if (WF_EX_ABORT == act) {
				*stop = WF_E_OS;
				return WF_EX_ABORT;
			}
			if (WF_EX_SKIP == act) {
				*kind = WF_XI_WRITE;
				return WF_EX_SKIP;
			}
		}
		st->bytes += (uint64_t)k;
	}
	if (size_known && got != declared) {
		*kind = WF_XI_SIZE;
		return WF_EX_SKIP;
	}
	return WF_EX_CONTINUE;
}

WfStatus wf_archive_extract(const wchar_t *file,
        const wchar_t *const *inner,
        size_t n_inner,
        const WfExtractLimits *limits,
        const WfExtractSink *sink,
        const WfArchiveCtl *ctl,
        WfExtractStats *out)
{
	WfExtractLimits lim = limits ? *limits : wf_extract_default_limits();
	WfArchive *a = NULL;
	Plan p;
	CaseSet names;
	Source *src = NULL;
	struct archive *ar = NULL;
	struct archive_entry *e;
	HANDLE h = INVALID_HANDLE_VALUE;
	Stamp stamp;
	WfStatus st;
	ExOut o;
	uint8_t *buf = NULL;
	wchar_t *rel = NULL;
	uint32_t ordinal = 0;
	size_t pending = 0;
	int r;

	if (!file || !out || !sink || !sink->file_begin || !sink->file_data ||
	        !sink->file_end || !sink->dir || (n_inner && !inner)) {
		return WF_E_INVAL;
	}
	memset(out, 0, sizeof *out);
	memset(&names, 0, sizeof names);
	memset(&p, 0, sizeof p);
	names.max_bytes = lim.max_name_bytes;
	o.sink = sink;
	o.st = out;
	st = extract_open_index(file, ctl, &a);
	if (WF_OK != st) {
		return st;
	}
	st = plan_build(a, inner, n_inner, &p, out);
	if (WF_OK != st) {
		goto done;
	}
	out->unplaced = a->info.skipped;
	/* refuse early on what the entries declare; the real count is kept below */
	if (out->declared_files + out->declared_dirs > lim.max_files ||
	        out->declared_bytes > lim.max_total_bytes) {
		st = WF_E_RANGE;
		goto done;
	}
	for (size_t i = 0; i < p.n_ord; i++) {
		pending += UINT32_MAX != p.by_ord[i];
	}
	st = open_file(file, &h, &stamp);
	if (WF_OK != st) {
		goto done;
	}
	if (!same_stamp(&stamp, &a->stamp)) {
		cache_forget(file);
		st = WF_E_CHANGED;
		goto done;
	}
	src = calloc(1, sizeof *src);
	buf = malloc(EX_BLOCK);
	rel = wf_malloc_array(EX_PATH_MAX + 2, sizeof *rel);
	if (!src || !buf || !rel) {
		st = WF_E_NOMEM;
		goto done;
	}
	src->h = h;
	src->ctl = ctl;
	ar = reader_new(src, a->hdr_cp, &st);
	if (!ar) {
		goto done;
	}
	while (pending) {
		uint32_t node;
		const Node *nd;
		wchar_t *raw;
		size_t rawn = 0, reln;
		bool dir;
		int put;

		if (is_cancelled(src) ||
		        (sink->cancelled && sink->cancelled(sink->ctx))) {
			st = WF_E_CANCELLED;
			goto done;
		}
		r = archive_read_next_header(ar, &e);
		if (ARCHIVE_EOF == r) {
			st = WF_E_CHANGED; /* the index named entries that are not there */
			goto done;
		}
		if (ARCHIVE_OK != r && ARCHIVE_WARN != r) {
			st = archive_failure(ar, src);
			goto done;
		}
		if (ordinal >= p.n_ord || UINT32_MAX == p.by_ord[ordinal]) {
			ordinal++;
			continue; /* not selected, or not the entry the index kept */
		}
		node = p.by_ord[ordinal++];
		pending--;
		nd = &a->nodes[node];
		dir = 0 != (nd->flags & N_DIR);
		/* the entry must be what the index recorded */
		if (!!(AE_IFDIR == archive_entry_filetype(e)) != dir ||
		        (archive_entry_size_is_set(e) &&
		                !(nd->flags & N_NOSIZE) && !dir &&
		                (uint64_t)archive_entry_size(e) != nd->size)) {
			st = WF_E_CHANGED;
			goto done;
		}
		reln = rel_of(a, &p, node, rel, EX_PATH_MAX);
		if (!reln) {
			ex_issue(&o, node_name(a, nd), WF_XI_NAME);
			continue;
		}
		raw_name_of(e, &raw, &rawn);
		if (!raw) {
			st = WF_E_NOMEM;
			goto done;
		}
		/* the name as the archive wrote it, not as the index made it fit */
		if (!name_is_safe(raw, rawn)) {
			free(raw);
			ex_issue(&o, rel, WF_XI_NAME);
			continue;
		}
		free(raw);
		put = caseset_put_path(&names, rel, reln);
		if (put < 0) {
			st = WF_E_RANGE;
			goto done;
		}
		if (!put) {
			ex_issue(&o, rel, WF_XI_COLLISION);
			continue;
		}
		if (out->files + out->dirs >= lim.max_files) {
			st = WF_E_RANGE;
			goto done;
		}
		if (dir) {
			WfExtractAct act = sink->dir(sink->ctx, rel, nd->mtime);

			if (WF_EX_ABORT == act) {
				st = WF_E_OS;
				goto done;
			}
			if (WF_EX_SKIP == act) {
				ex_issue(&o, rel, WF_XI_WRITE);
			} else {
				out->dirs++;
			}
			continue;
		}
		if (nd->flags & N_LINK) {
			ex_issue(&o, rel, WF_XI_LINK);
			continue;
		}
		if (nd->flags & N_ENC) {
			ex_issue(&o, rel, WF_XI_ENCRYPTED);
			continue;
		}
		if (!(nd->flags & N_NOSIZE) && nd->size > lim.max_file_bytes) {
			ex_issue(&o, rel, WF_XI_TOO_BIG);
			continue;
		}
		{
			WfExtractAct act = sink->file_begin(sink->ctx, rel,
			        nd->size, !(nd->flags & N_NOSIZE), nd->mtime);
			WfExtractIssue kind = WF_XI_READ;
			uint64_t before = out->bytes;
			WfStatus stop = WF_OK;

			if (WF_EX_ABORT == act) {
				st = WF_E_OS;
				goto done;
			}
			if (WF_EX_SKIP == act) {
				ex_issue(&o, rel, WF_XI_WRITE);
				continue;
			}
			act = ex_stream(ar, src, sink, &lim, out, buf, nd->size,
			        !(nd->flags & N_NOSIZE), &kind, &stop);
			if (WF_EX_CONTINUE == act) {
				sink->file_end(sink->ctx, true);
				out->files++;
				continue;
			}
			sink->file_end(sink->ctx, false);
			out->bytes =
			        before; /* what was thrown away is not counted */
			if (WF_EX_ABORT == act) {
				st = stop;
				goto done;
			}
			ex_issue(&o, rel, kind);
		}
	}
	/* the archive must be the same one at the end as at the start */
	if (!stamp_of(h, &stamp) || !same_stamp(&stamp, &a->stamp)) {
		st = WF_E_CHANGED;
		goto done;
	}
	st = WF_OK;
done:
	if (ar) {
		archive_read_free(ar);
	}
	free(src);
	free(buf);
	free(rel);
	caseset_free(&names);
	plan_free(&p);
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	wf_archive_release(a);
	return st;
}

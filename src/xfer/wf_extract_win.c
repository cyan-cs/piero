#include "wf_extract_win.h"

#include <bcrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../archive/wf_archive.h"
#include "../base/wf_macro.h"
#include "../base/wf_mem.h"
#include "../fs/owned_delete.h"
#include "../path/wf_path.h"

#define STAGE_PATH_MAX 32000u /* UTF-16 units a path may have */
#define REL_MAX 4200u         /* a name from the archive: at most 4096 units */
#define TIME_LOOK_BACK 256u   /* how far back a folder's entry is looked for */
#define SPACE_MARGIN (64ull << 20)

typedef struct
{
	wchar_t *path_x; /* extended path */
	FILE_ID_INFO id;
	bool dir;
	HANDLE h; /* a folder is held from the moment it is made until the end, so
	             that it cannot be swapped for a link while files go below it */
	uint64_t
	        mtime; /* a folder the archive named: its time, set at the end */
} Made;

typedef struct
{
	wchar_t *rel;
	WfCopyFailKind kind;
	DWORD error;
} Issue;

/* The folders below the staging folder that this job has made, upper-cased: a
 * name looked up here is a folder that is held, not looked at again. */
typedef struct
{
	wchar_t **key;
	size_t cap, n;
} PathSet;

typedef struct
{
	const WfCopyControl *ctl;
	HANDLE dest_h; /* the destination, held and proven before anything is made */
	wchar_t *stage, *stage_x;
	size_t prefix; /* length of stage_x plus the separator */
	HANDLE stage_h;
	FILE_ID_INFO stage_id;
	Made *made;
	size_t n_made, cap_made;
	Issue *issue;
	size_t n_issue, cap_issue;
	PathSet folders;
	HANDLE file; /* the file being written, or invalid */
	FILE_ID_INFO file_id;
	uint64_t file_mtime;
	DWORD write_error;
	wchar_t cur[64]; /* the file being written, for the progress line */
	size_t files;
	uint64_t bytes;
	bool oom;
	bool unclean; /* something that is not whole is, or may be, in the staging
	                 folder: nothing is published from it */
} X;

static bool made_add(
        X *x, wchar_t *path_x, const FILE_ID_INFO *id, bool dir, HANDLE held)
{
	if (x->n_made == x->cap_made) {
		size_t cap = x->cap_made ? x->cap_made * 2 : 256;
		Made *g = wf_realloc_array(x->made, cap, sizeof *g);

		if (!g) {
			return false;
		}
		x->made = g;
		x->cap_made = cap;
	}
	x->made[x->n_made].path_x = path_x;
	x->made[x->n_made].id = *id;
	x->made[x->n_made].dir = dir;
	x->made[x->n_made].h = held;
	x->made[x->n_made].mtime = 0;
	x->n_made++;
	return true;
}

static uint64_t key_hash(const wchar_t *s)
{
	uint64_t h = 1469598103934665603ull;

	for (; *s; s++) {
		h = (h ^ (uint64_t)*s) * 1099511628211ull;
	}
	return h;
}

static wchar_t *upper_copy(const wchar_t *s, size_t n)
{
	wchar_t *k = wf_malloc_array(n + 1, sizeof *k);

	if (k) {
		memcpy(k, s, n * sizeof *k);
		k[n] = 0;
		CharUpperBuffW(k, (DWORD)n);
	}
	return k;
}

static bool pset_has(const PathSet *p, const wchar_t *key)
{
	if (!p->cap) {
		return false;
	}
	for (size_t k = key_hash(key) & (p->cap - 1); p->key[k];
	        k = (k + 1) & (p->cap - 1)) {
		if (!wcscmp(p->key[k], key)) {
			return true;
		}
	}
	return false;
}

/* Takes `key` (malloc'd). False when there is no memory; key is freed then. */
static bool pset_add(PathSet *p, wchar_t *key)
{
	size_t k;

	if ((p->n + 1) * 2 > p->cap) {
		size_t cap = p->cap ? p->cap * 2 : 256;
		wchar_t **v = calloc(cap, sizeof *v);

		if (!v) {
			free(key);
			return false;
		}
		for (size_t i = 0; i < p->cap; i++) {
			if (p->key[i]) {
				size_t j = key_hash(p->key[i]) & (cap - 1);

				while (v[j]) {
					j = (j + 1) & (cap - 1);
				}
				v[j] = p->key[i];
			}
		}
		free(p->key);
		p->key = v;
		p->cap = cap;
	}
	for (k = key_hash(key) & (p->cap - 1); p->key[k];
	        k = (k + 1) & (p->cap - 1)) {
	}
	p->key[k] = key;
	p->n++;
	return true;
}

static void pset_free(PathSet *p)
{
	for (size_t i = 0; i < p->cap; i++) {
		free(p->key[i]);
	}
	free(p->key);
	memset(p, 0, sizeof *p);
}

static bool issue_add(
        X *x, const wchar_t *rel, WfCopyFailKind kind, DWORD error)
{
	Issue *g;

	if (x->n_issue == x->cap_issue) {
		size_t cap = x->cap_issue ? x->cap_issue * 2 : 16;

		g = wf_realloc_array(x->issue, cap, sizeof *g);
		if (!g) {
			x->oom = true;
			return false;
		}
		x->issue = g;
		x->cap_issue = cap;
	}
	x->issue[x->n_issue].rel = wf_path_dup(rel);
	if (!x->issue[x->n_issue].rel) {
		x->oom = true;
		return false;
	}
	x->issue[x->n_issue].kind = kind;
	x->issue[x->n_issue].error = error;
	x->n_issue++;
	return true;
}

static bool id_of_handle(HANDLE h, FILE_ID_INFO *id)
{
	return 0 != GetFileInformationByHandleEx(h, FileIdInfo, id, sizeof *id);
}

static bool stage_make(X *x, const wchar_t *dest, DWORD *error)
{
	unsigned char rnd[8] = {0};
	unsigned long long v = 0;
	wchar_t leaf[40];
	BY_HANDLE_FILE_INFORMATION bi;

	(void)BCryptGenRandom(
	        NULL, rnd, sizeof rnd, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	for (size_t i = 0; i < sizeof rnd; i++) {
		v = v << 8 | rnd[i];
	}
	swprintf(leaf, WF_ARRAY_LEN(leaf), L".wfextract-%016llx.dir", v);
	if (WF_OK != wf_path_join(dest, leaf, &x->stage) ||
	        WF_OK != wf_path_to_extended(x->stage, &x->stage_x)) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return false;
	}
	if (!CreateDirectoryW(x->stage_x, NULL)) {
		*error = GetLastError();
		return false;
	}
	/* held from the moment it exists: nothing can be put in its place while
	 * the archive is read into it */
	x->stage_h = CreateFileW(x->stage_x,
	        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES |
	                FILE_WRITE_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == x->stage_h) {
		*error = GetLastError();
		return false;
	}
	if (!id_of_handle(x->stage_h, &x->stage_id) ||
	        !GetFileInformationByHandle(x->stage_h, &bi) ||
	        !(bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
	        (bi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
		*error = ERROR_INVALID_DATA;
		return false;
	}
	(void)SetFileAttributesW(x->stage_x, FILE_ATTRIBUTE_HIDDEN);
	x->prefix = wcslen(x->stage_x) + 1;
	return true;
}

static wchar_t *stage_path(const X *x, const wchar_t *rel)
{
	size_t n = x->prefix - 1, m = wcslen(rel);
	wchar_t *p;

	if (n + 1 + m > STAGE_PATH_MAX) {
		return NULL;
	}
	p = wf_malloc_array(n + m + 2, sizeof *p);
	if (!p) {
		return NULL;
	}
	memcpy(p, x->stage_x, n * sizeof *p);
	p[n] = L'\\';
	memcpy(p + n + 1, rel, (m + 1) * sizeof *p);
	return p;
}

/* Every folder on the way to `rel`'s parent exists below the staging folder
 * and is held. Each is made here by this job (one that is there and is not in
 * the set is not ours and is refused) and opened at once without delete
 * sharing, so that nothing can be put in its place while files go below it. */
static bool ensure_parents(X *x, const wchar_t *rel, DWORD *error)
{
	size_t last = 0;

	for (size_t i = 0; rel[i]; i++) {
		if (L'\\' == rel[i]) {
			last = i;
		}
	}
	if (!last) {
		return true;
	}
	for (size_t i = 0; i <= last; i++) {
		wchar_t *part, *p, *key;
		HANDLE h;
		FILE_ID_INFO id;
		BY_HANDLE_FILE_INFORMATION bi;

		if (i != last && L'\\' != rel[i]) {
			continue;
		}
		key = upper_copy(rel, i);
		if (!key) {
			x->oom = true;
			*error = ERROR_NOT_ENOUGH_MEMORY;
			return false;
		}
		if (pset_has(&x->folders, key)) {
			free(key);
			continue;
		}
		part = wf_path_dup_n(rel, i);
		p = part ? stage_path(x, part) : NULL;
		free(part);
		if (!p) {
			free(key);
			*error = ERROR_FILENAME_EXCED_RANGE;
			return false;
		}
		if (!CreateDirectoryW(p, NULL)) {
			*error = GetLastError();
			free(key);
			free(p);
			return false;
		}
		h = CreateFileW(p, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        FILE_FLAG_BACKUP_SEMANTICS |
		                FILE_FLAG_OPEN_REPARSE_POINT,
		        NULL);
		if (INVALID_HANDLE_VALUE == h || !id_of_handle(h, &id) ||
		        !GetFileInformationByHandle(h, &bi) ||
		        !(bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
		        (bi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
			*error = INVALID_HANDLE_VALUE == h ? GetLastError()
			                                   : ERROR_INVALID_DATA;
			if (INVALID_HANDLE_VALUE != h) {
				CloseHandle(h);
			}
			x->unclean =
			        true; /* a folder we made and cannot vouch for */
			free(key);
			free(p);
			return false;
		}
		if (!made_add(x, p, &id, true, h)) {
			CloseHandle(h);
			x->unclean = true;
			free(key);
			free(p);
			x->oom = true;
			*error = ERROR_NOT_ENOUGH_MEMORY;
			return false;
		}
		if (!pset_add(&x->folders, key)) {
			x->oom = true;
			*error = ERROR_NOT_ENOUGH_MEMORY;
			return false; /* the folder is in the manifest: removed with it */
		}
	}
	return true;
}

bool (*wf_extract_test_fail_write)(void);
void (*wf_extract_test_before_discard)(const wchar_t *path_x);

static bool x_cancelled(void *ctx)
{
	X *x = ctx;

	return x->ctl && x->ctl->checkpoint && !x->ctl->checkpoint(x->ctl->ctx);
}

static void x_progress(X *x, const wchar_t *rel)
{
	if (rel) {
		const wchar_t *leaf = wcsrchr(rel, L'\\');

		wcsncpy_s(x->cur, WF_ARRAY_LEN(x->cur), leaf ? leaf + 1 : rel,
		        _TRUNCATE);
	}
	if (x->ctl && x->ctl->progress) {
		x->ctl->progress(x->ctl->ctx, x->files, x->bytes, x->cur);
	}
}

static WfExtractAct x_dir(void *ctx, const wchar_t *rel, uint64_t mtime)
{
	X *x = ctx;
	wchar_t *p;
	DWORD error = 0;
	wchar_t holder[REL_MAX + 2];
	size_t n = wcslen(rel);

	/* the folder itself: its parents, then itself (as a parent of a name) */
	if (n + 2 > WF_ARRAY_LEN(holder)) {
		x->write_error = ERROR_FILENAME_EXCED_RANGE;
		return WF_EX_SKIP;
	}
	memcpy(holder, rel, n * sizeof *holder);
	holder[n] = L'\\';
	holder[n + 1] = 0;
	if (!ensure_parents(x, holder, &error)) {
		x->write_error = error;
		return x->oom ? WF_EX_ABORT : WF_EX_SKIP;
	}
	/* ensure_parents() made the folder but remembers no time for it */
	p = stage_path(x, rel);
	if (p) {
		/* a folder the archive names comes right after the ones it needed;
		 * one made earlier, by a file below it, is looked for only a little
		 * way back (its time is a nicety, never worth a scan of everything) */
		size_t from = x->n_made,
		       stop = from > TIME_LOOK_BACK ? from - TIME_LOOK_BACK : 0;

		for (size_t i = from; i-- > stop;) {
			if (!_wcsicmp(x->made[i].path_x, p)) {
				x->made[i].mtime = mtime;
				break;
			}
		}
		free(p);
	}
	return WF_EX_CONTINUE;
}

static WfExtractAct x_begin(void *ctx,
        const wchar_t *rel,
        uint64_t size,
        bool known,
        uint64_t mtime)
{
	X *x = ctx;
	wchar_t *p;
	DWORD error = 0;
	FILE_ID_INFO id;

	(void)size;
	(void)known;
	if (!ensure_parents(x, rel, &error)) {
		x->write_error = error;
		return x->oom ? WF_EX_ABORT : WF_EX_SKIP;
	}
	p = stage_path(x, rel);
	if (!p) {
		x->write_error = ERROR_FILENAME_EXCED_RANGE;
		return WF_EX_SKIP;
	}
	/* a new file, never an existing one, held while it is written */
	x->file = CreateFileW(p, GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
	        0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (INVALID_HANDLE_VALUE == x->file) {
		x->write_error = GetLastError();
		free(p);
		return WF_EX_SKIP;
	}
	if (!id_of_handle(x->file, &id)) {
		/* a file that cannot be identified is not removed by name: it is
		 * left, and nothing is published from this staging folder */
		x->write_error = GetLastError();
		x->unclean = true;
		CloseHandle(x->file);
		x->file = INVALID_HANDLE_VALUE;
		free(p);
		return WF_EX_SKIP;
	}
	x->file_id = id;
	x->file_mtime = mtime;
	if (!made_add(x, p, &id, false, INVALID_HANDLE_VALUE)) {
		free(p);
		x->oom = true;
		x->unclean = true;
		CloseHandle(x->file);
		x->file = INVALID_HANDLE_VALUE;
		return WF_EX_ABORT;
	}
	x_progress(x, rel);
	return WF_EX_CONTINUE;
}

static WfExtractAct x_data(void *ctx, const void *buf, size_t n)
{
	X *x = ctx;
	const unsigned char *p = buf;

	while (n) {
		DWORD put = 0, want = n > 1u << 20 ? 1u << 20 : (DWORD)n;

		if (wf_extract_test_fail_write &&
		        wf_extract_test_fail_write()) {
			x->write_error = ERROR_DISK_FULL;
			return WF_EX_SKIP;
		}
		if (!WriteFile(x->file, p, want, &put, NULL) || !put) {
			x->write_error = GetLastError();
			return WF_EX_SKIP;
		}
		p += put;
		n -= put;
		x->bytes += put;
	}
	x_progress(x, NULL);
	return WF_EX_CONTINUE;
}

static void x_end(void *ctx, bool complete)
{
	X *x = ctx;
	Made *m;

	if (INVALID_HANDLE_VALUE == x->file) {
		return;
	}
	m = &x->made[x->n_made - 1];
	if (complete) {
		if (x->file_mtime) {
			FILETIME ft;

			ft.dwLowDateTime = (DWORD)(x->file_mtime & 0xFFFFFFFFu);
			ft.dwHighDateTime = (DWORD)(x->file_mtime >> 32);
			(void)SetFileTime(x->file, NULL, NULL, &ft);
		}
		CloseHandle(x->file);
		x->file = INVALID_HANDLE_VALUE;
		x->files++;
		return;
	}
	/* thrown away: removed on the identity it was made with, and forgotten */
	CloseHandle(x->file);
	x->file = INVALID_HANDLE_VALUE;
	{
		WfOwnedResult r;

		if (wf_extract_test_before_discard) {
			wf_extract_test_before_discard(m->path_x);
		}
		r = wf_owned_delete_save_temp(m->path_x, &m->id, NULL);
		if (WF_OWNED_DELETED == r || WF_OWNED_ABSENT == r) {
			free(m->path_x);
			x->n_made--;
		} else {
			/* a file that is not whole and could not be removed must not be
			 * taken for part of the result: the copy is not made at all */
			x->unclean = true;
		}
	}
}

static void x_issue(void *ctx, const wchar_t *rel, WfExtractIssue kind)
{
	X *x = ctx;
	WfCopyFailKind k = WF_CF_WRITE_FAILED;
	DWORD error = 0;

	switch (kind) {
	case WF_XI_NAME:      k = WF_CF_ARCHIVE_NAME; break;
	case WF_XI_COLLISION: k = WF_CF_ARCHIVE_COLLISION; break;
	case WF_XI_LINK:
	case WF_XI_ENCRYPTED: k = WF_CF_ARCHIVE_UNSUPPORTED; break;
	case WF_XI_TOO_BIG:   k = WF_CF_ARCHIVE_LIMIT; break;
	case WF_XI_CHECKSUM:
	case WF_XI_SIZE:
	case WF_XI_READ:      k = WF_CF_ARCHIVE_DATA; break;
	case WF_XI_WRITE:
		error = x->write_error;
		k = ERROR_DISK_FULL == error ? WF_CF_NO_SPACE
		                             : WF_CF_WRITE_FAILED;
		break;
	}
	(void)issue_add(x, rel, k, error);
}

static void made_free(X *x)
{
	for (size_t i = 0; i < x->n_made; i++) {
		if (INVALID_HANDLE_VALUE != x->made[i].h) {
			CloseHandle(x->made[i].h);
		}
		free(x->made[i].path_x);
	}
	pset_free(&x->folders);
	free(x->made);
	x->made = NULL;
	x->n_made = x->cap_made = 0;
}

/* Everything that was made, last first, each on the identity it was made with;
 * then the staging folder if it is empty. False if anything is left. */
static bool stage_remove(X *x)
{
	bool clean = true;

	if (x->file != INVALID_HANDLE_VALUE) {
		CloseHandle(x->file);
		x->file = INVALID_HANDLE_VALUE;
	}
	for (size_t i = 0; i < x->n_made; i++) {
		if (INVALID_HANDLE_VALUE != x->made[i].h) {
			CloseHandle(x->made[i]
			                .h); /* a held folder cannot be removed */
			x->made[i].h = INVALID_HANDLE_VALUE;
		}
	}
	for (size_t i = x->n_made; i-- > 0;) {
		WfOwnedResult r = x->made[i].dir
		        ? wf_owned_delete_empty_dir(
		                  x->made[i].path_x, &x->made[i].id, NULL)
		        : wf_owned_delete_save_temp(
		                  x->made[i].path_x, &x->made[i].id, NULL);

		if (WF_OWNED_DELETED != r && WF_OWNED_ABSENT != r) {
			clean = false;
		}
	}
	if (INVALID_HANDLE_VALUE != x->stage_h) {
		CloseHandle(x->stage_h);
		x->stage_h = INVALID_HANDLE_VALUE;
	}
	if (x->stage_x) {
		WfOwnedResult r = wf_owned_delete_empty_dir(
		        x->stage_x, &x->stage_id, NULL);

		if (WF_OWNED_DELETED != r && WF_OWNED_ABSENT != r) {
			clean = false;
		}
	}
	return clean;
}

/* A folder the archive named takes the archive's time, children first. */
static void stage_times(X *x)
{
	for (size_t i = x->n_made; i-- > 0;) {
		HANDLE h;
		FILETIME ft;

		if (!x->made[i].dir || !x->made[i].mtime) {
			continue;
		}
		ft.dwLowDateTime = (DWORD)(x->made[i].mtime & 0xFFFFFFFFu);
		ft.dwHighDateTime = (DWORD)(x->made[i].mtime >> 32);
		h = CreateFileW(x->made[i].path_x, FILE_WRITE_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		        NULL, OPEN_EXISTING,
		        FILE_FLAG_BACKUP_SEMANTICS |
		                FILE_FLAG_OPEN_REPARSE_POINT,
		        NULL);
		if (INVALID_HANDLE_VALUE != h) {
			(void)SetFileTime(h, NULL, NULL, &ft);
			CloseHandle(h);
		}
	}
}

static void note_issue(
        WfCopyResult *res, const wchar_t *archive, const Issue *is)
{
	if (res->failure_total < WF_CJ_FAILURES) {
		WfCopyFailure *f = &res->failures[res->failure_total];

		wcsncpy_s(f->name, WF_ARRAY_LEN(f->name), is->rel, _TRUNCATE);
		f->kind = is->kind;
		f->error = is->error;
	}
	if (res->fail_all_n < WF_CJ_FAIL_ALL_MAX) {
		size_t n = wcslen(archive) + 3 + wcslen(is->rel);
		wchar_t *full = wf_malloc_array(n, sizeof *full);

		if (full) {
			if (0 == (res->fail_all_n & 31u)) {
				WfCopyFailFull *g = realloc(res->fail_all,
				        (res->fail_all_n + 32) *
				                sizeof *res->fail_all);

				if (!g) {
					free(full);
					full = NULL;
				} else {
					res->fail_all = g;
				}
			}
			if (full) {
				WfCopyFailFull *f =
				        &res->fail_all[res->fail_all_n++];

				swprintf(full, n, L"%ls|\\%ls", archive,
				        is->rel);
				f->path = full;
				f->kind = is->kind;
				f->error = is->error;
			}
		}
	}
	res->failure_total++;
	res->files_failed++;
}

static void note_all(WfCopyResult *res, const wchar_t *archive, const X *x)
{
	for (size_t i = 0; i < x->n_issue; i++) {
		note_issue(res, archive, &x->issue[i]);
	}
}

static void issues_free(X *x)
{
	for (size_t i = 0; i < x->n_issue; i++) {
		free(x->issue[i].rel);
	}
	free(x->issue);
	x->issue = NULL;
	x->n_issue = x->cap_issue = 0;
}

/* A staging folder an earlier run left (a crash while the archive was read) is
 * said, by name, as a failure of this job: it is not ours to remove without
 * its identity, and it holds files the user may want to look at. */
static void note_leftovers(X *x, const wchar_t *dest)
{
	wchar_t *pattern = NULL, *dest_x = NULL;
	WIN32_FIND_DATAW fd;
	HANDLE f;

	if (WF_OK != wf_path_to_extended(dest, &dest_x) ||
	        WF_OK != wf_path_join(dest_x, L".wfextract-*", &pattern)) {
		free(dest_x);
		return;
	}
	f = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE != f) {
		do {
			(void)issue_add(x, fd.cFileName, WF_CF_STAGING_LEFT, 0);
		} while (FindNextFileW(f, &fd));
		FindClose(f);
	}
	free(pattern);
	free(dest_x);
}

static WfCopyFailKind kind_of_status(WfStatus st)
{
	switch (st) {
	case WF_E_RANGE:     return WF_CF_ARCHIVE_LIMIT;
	case WF_E_CHANGED:
	case WF_E_NOENT:     return WF_CF_ARCHIVE_CHANGED;
	case WF_E_ACCESS:    return WF_CF_SOURCE_UNREADABLE;
	case WF_E_ENCRYPTED:
	case WF_E_FORMAT:    return WF_CF_ARCHIVE_UNSUPPORTED;
	case WF_E_NOMEM:     return WF_CF_WRITE_FAILED;
	case WF_E_OS:        return WF_CF_WRITE_FAILED;
	default:             return WF_CF_ARCHIVE_DATA;
	}
}

/* The staged top-level items as a transfer slot of the staging folder. */
static WfStatus slot_of_stage(const X *x, WfTransferSlot *slot)
{
	size_t n = 0, tops = 0;

	wf_slot_init(slot);
	slot->mode = WF_XFER_COPY;
	slot->dir = wf_path_dup(x->stage);
	slot->dir_id.kind = WF_ID_STRONG;
	slot->dir_id.volume = x->stage_id.VolumeSerialNumber;
	memcpy(slot->dir_id.id, x->stage_id.FileId.Identifier,
	        sizeof slot->dir_id.id);
	/* only the items at the top are in the slot: they are counted first, and
	 * what is below them is not given a place in the array */
	for (size_t i = 0; i < x->n_made; i++) {
		tops += !wcschr(x->made[i].path_x + x->prefix, L'\\');
	}
	slot->src = calloc(tops ? tops : 1, sizeof *slot->src);
	if (!slot->dir || !slot->src) {
		wf_slot_free(slot);
		return WF_E_NOMEM;
	}
	for (size_t i = 0; i < x->n_made; i++) {
		const wchar_t *rel = x->made[i].path_x + x->prefix;
		HANDLE h;
		BY_HANDLE_FILE_INFORMATION bi;
		WfNodeInfo *nd;

		if (wcschr(rel, L'\\')) {
			continue; /* below an item: it goes with it */
		}
		h = CreateFileW(x->made[i].path_x, FILE_READ_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		        NULL, OPEN_EXISTING,
		        FILE_FLAG_BACKUP_SEMANTICS |
		                FILE_FLAG_OPEN_REPARSE_POINT,
		        NULL);
		if (INVALID_HANDLE_VALUE == h ||
		        !GetFileInformationByHandle(h, &bi)) {
			if (INVALID_HANDLE_VALUE != h) {
				CloseHandle(h);
			}
			wf_slot_free(slot);
			return WF_E_OS;
		}
		CloseHandle(h);
		slot->src[n].name = wf_path_dup(rel);
		if (!slot->src[n].name) {
			wf_slot_free(slot);
			return WF_E_NOMEM;
		}
		nd = &slot->src[n].snap;
		nd->id.kind = WF_ID_STRONG;
		nd->id.volume = x->made[i].id.VolumeSerialNumber;
		memcpy(nd->id.id, x->made[i].id.FileId.Identifier,
		        sizeof nd->id.id);
		nd->flags = x->made[i].dir ? WF_ENT_DIR : 0;
		nd->size = ((uint64_t)bi.nFileSizeHigh << 32) | bi.nFileSizeLow;
		nd->mtime =
		        ((uint64_t)bi.ftLastWriteTime.dwHighDateTime << 32) |
		        bi.ftLastWriteTime.dwLowDateTime;
		nd->nlinks = bi.nNumberOfLinks;
		n++;
		slot->count = n;
	}
	return WF_OK;
}

static void finish(WfCopyResult *out, const WfCopySpec *spec)
{
	out->kind = WF_CJ_COPY;
	out->extracted = true;
	out->verify = spec->verify;
	out->dest_id = spec->dest_id;
}

/* The destination is opened, shown to be the folder that was chosen (its
 * FileId, a plain folder and not a link) and held, before anything is made in
 * it: nothing can be put in its place while the job runs. */
static bool pin_dest(
        X *x, const wchar_t *dest_x, const WfFileId *want, DWORD *error)
{
	BY_HANDLE_FILE_INFORMATION bi;
	FILE_ID_INFO id;

	x->dest_h = CreateFileW(dest_x,
	        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == x->dest_h) {
		*error = GetLastError();
		return false;
	}
	if (!id_of_handle(x->dest_h, &id) ||
	        !GetFileInformationByHandle(x->dest_h, &bi) ||
	        !(bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
	        (bi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
	        !(want->kind == WF_ID_STRONG) ||
	        want->volume != id.VolumeSerialNumber ||
	        memcmp(want->id, id.FileId.Identifier, sizeof want->id)) {
		*error = ERROR_INVALID_DATA;
		return false;
	}
	return true;
}

WfStatus wf_extract_run(const WfCopySpec *spec,
        const WfCopyControl *control,
        WfCopyJournal *journal,
        WfCopyResult *out)
{
	X x;
	WfExtractStats stats, declared;
	WfExtractLimits lim = wf_extract_default_limits();
	WfExtractSink sink;
	WfStatus st;
	WfCopyResult engine;
	WfCopySpec es;
	ULARGE_INTEGER avail;
	wchar_t *dest_x = NULL;
	DWORD error = 0;
	bool left = false;
	WfStatus ret = WF_OK;

	if (!spec || !out || !spec->archive || !spec->dest ||
	        WF_CJ_EXTRACT != spec->kind ||
	        (spec->n_inner && !spec->inner)) {
		return WF_E_INVAL;
	}
	memset(out, 0, sizeof *out);
	out->kind = WF_CJ_COPY;
	out->extracted = true;
	out->verify = spec->verify;
	out->dest_id = spec->dest_id;
	out->state = WF_CS_FAILED;
	if (!wf_copyjob_allowed(WF_CJ_EXTRACT, spec->collide)) {
		return WF_E_ACCESS;
	}
	memset(&x, 0, sizeof x);
	x.ctl = control;
	x.stage_h = INVALID_HANDLE_VALUE;
	x.file = INVALID_HANDLE_VALUE;
	x.dest_h = INVALID_HANDLE_VALUE;
	memset(&sink, 0, sizeof sink);
	sink.ctx = &x;
	sink.dir = x_dir;
	sink.file_begin = x_begin;
	sink.file_data = x_data;
	sink.file_end = x_end;
	sink.issue = x_issue;
	sink.cancelled = x_cancelled;

	/* what the selection holds, from the index; the disk must take it twice */
	st = wf_archive_extract_measure(spec->archive,
	        (const wchar_t *const *)spec->inner, spec->n_inner, NULL,
	        &declared);
	if (WF_OK != st) {
		Issue is = {L"", kind_of_status(st), 0};

		note_issue(out, spec->archive, &is);
		goto end;
	}
	if (WF_OK != wf_path_to_extended(spec->dest, &dest_x)) {
		ret = WF_E_NOMEM;
		goto end;
	}
	if (!pin_dest(&x, dest_x, &spec->dest_id, &error)) {
		Issue is = {L"", WF_CF_DEST_UNSAFE, error};

		free(dest_x);
		note_issue(out, spec->archive, &is);
		goto end;
	}
	if (GetDiskFreeSpaceExW(dest_x, &avail, NULL, NULL) &&
	        avail.QuadPart < declared.declared_bytes * 2 + SPACE_MARGIN) {
		Issue is = {L"", WF_CF_NO_SPACE, ERROR_DISK_FULL};

		free(dest_x);
		note_issue(out, spec->archive, &is);
		goto end;
	}
	free(dest_x);
	note_leftovers(&x, spec->dest);
	if (!stage_make(&x, spec->dest, &error)) {
		Issue is = {L"", WF_CF_WRITE_FAILED, error};

		note_issue(out, spec->archive, &is);
		if (x.stage_x && !stage_remove(&x)) {
			Issue left_is = {
			        x.stage ? x.stage : L"", WF_CF_STAGING_LEFT, 0};

			note_issue(out, spec->archive, &left_is);
		}
		goto end;
	}
	st = wf_archive_extract(spec->archive,
	        (const wchar_t *const *)spec->inner, spec->n_inner, &lim, &sink,
	        NULL, &stats);
	if (WF_OK != st) {
		/* partial: nothing of it is offered, and nothing of the destination
		 * was touched */
		bool cancelled = WF_E_CANCELLED == st;

		left = !stage_remove(&x);
		if (!cancelled) {
			Issue is = {L"", kind_of_status(st), 0};

			note_issue(out, spec->archive, &is);
		}
		note_all(out, spec->archive, &x);
		if (left) {
			Issue left_is = {
			        x.stage ? x.stage : L"", WF_CF_STAGING_LEFT, 0};

			note_issue(out, spec->archive, &left_is);
		}
		out->state = cancelled ? WF_CS_CANCELLED : WF_CS_FAILED;
		goto end;
	}
	if (x.unclean) {
		/* something that is not whole may be in the staging folder (a thrown
		 * away file that could not be removed, a folder or file that could not
		 * be vouched for): nothing is published from it */
		Issue is = {L"", WF_CF_WRITE_FAILED, 0};

		left = !stage_remove(&x);
		note_issue(out, spec->archive, &is);
		note_all(out, spec->archive, &x);
		if (left) {
			Issue left_is = {x.stage, WF_CF_STAGING_LEFT, 0};

			note_issue(out, spec->archive, &left_is);
		}
		out->state = WF_CS_FAILED;
		goto end;
	}
	stage_times(&x);
	if (!x.n_made) {
		/* everything was refused: nothing to put in place */
		note_all(out, spec->archive, &x);
		left = !stage_remove(&x);
		if (left) {
			Issue left_is = {x.stage, WF_CF_STAGING_LEFT, 0};

			note_issue(out, spec->archive, &left_is);
		}
		out->state = WF_CS_DONE;
		goto end;
	}
	/* the staged items are now put in place by the copy engine */
	memset(&es, 0, sizeof es);
	wf_slot_init(&es.slot);
	if (WF_OK != slot_of_stage(&x, &es.slot)) {
		Issue is = {L"", WF_CF_WRITE_FAILED, ERROR_NOT_ENOUGH_MEMORY};

		(void)stage_remove(&x);
		note_issue(out, spec->archive, &is);
		goto end;
	}
	es.kind = WF_CJ_COPY;
	es.dest = spec->dest;
	es.dest_id = spec->dest_id;
	es.verify = spec->verify;
	es.collide = spec->collide;
	es.merge_folders = true;
	es.total_files = stats.files;
	es.total_bytes = stats.bytes;
	memset(&engine, 0, sizeof engine);
	st = wf_copyengine_run(&es, control, journal, &engine);
	es.dest = NULL; /* the spec's own */
	wf_slot_free(&es.slot);
	if (WF_OK != st) {
		/* the engine could not run: the original error is returned, after
		 * everything this job holds has been given back */
		wf_copyresult_free(&engine);
		(void)stage_remove(&x);
		ret = st;
		goto end;
	}
	left = !stage_remove(&x);
	*out = engine;
	finish(out, spec);
	note_all(out, spec->archive, &x);
	if (left) {
		Issue left_is = {x.stage, WF_CF_STAGING_LEFT, 0};

		note_issue(out, spec->archive, &left_is);
	}
end:
	made_free(&x);
	issues_free(&x);
	if (INVALID_HANDLE_VALUE != x.dest_h) {
		CloseHandle(x.dest_h);
	}
	free(x.stage);
	free(x.stage_x);
	return ret;
}

#include "wf_wslops_win.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../base/wf_macro.h"
#include "../path/wf_name.h"
#include "../path/wf_path.h"
#include "wf_wslrm_win.h"

#include <stdio.h>
#include <wchar.h>

#define CHUNK (1u << 20)
#define WF_MAX_PATH_CHARS 2048

static volatile LONG64 progress_bytes;

static volatile LONG cancel_flag;

void wf_wslops_progress_reset(void)
{
	InterlockedExchange64(&progress_bytes, 0);
	InterlockedExchange(&cancel_flag, 0);
}

void wf_wslops_request_cancel(void)
{
	InterlockedExchange(&cancel_flag, 1);
}

static bool cancelled(void)
{
	return 0 != InterlockedCompareExchange(&cancel_flag, 0, 0);
}

/* What one copy of a folder has used, checked as it goes: the limits are the
 * operation's, not only a measurement taken before it. */
typedef struct
{
	size_t entries;
	unsigned long long bytes;
} Budget;

unsigned long long wf_wslops_progress(void)
{
	return (unsigned long long)InterlockedCompareExchange64(
	        &progress_bytes, 0, 0);
}

typedef enum
{
	K_MISSING,
	K_FILE,
	K_DIR,
	K_LINK,
	K_ERROR
} Kind;

static WfStatus fail(WfWslReport *r, DWORD error, const char *why)
{
	r->error = error;
	r->reason = why;
	if (ERROR_FILE_NOT_FOUND == error || ERROR_PATH_NOT_FOUND == error) {
		return WF_E_NOENT;
	}
	if (ERROR_ACCESS_DENIED == error) {
		return WF_E_ACCESS;
	}
	return 0 == error ? WF_E_INVAL : WF_E_OS;
}

static void begin(WfWslReport *r)
{
	memset(r, 0, sizeof *r);
}

bool wf_wslops_name_ok(const wchar_t *name)
{
	size_t n = name ? wcslen(name) : 0;

	if (0 == n || n > 255 || !wcscmp(name, L".") || !wcscmp(name, L"..") ||
	        L'.' == name[n - 1] || L' ' == name[n - 1]) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (name[i] < 0x20 || wcschr(L"\\/:*?\"<>|", name[i])) {
			return false;
		}
	}
	return true;
}

static wchar_t *join(const wchar_t *dir, const wchar_t *name)
{
	size_t d = wcslen(dir), n = wcslen(name);
	wchar_t *out = malloc((d + n + 2) * sizeof *out);

	if (!out) {
		return NULL;
	}
	memcpy(out, dir, d * sizeof *out);
	if (d && L'\\' != dir[d - 1]) {
		out[d++] = L'\\';
	}
	memcpy(out + d, name, (n + 1) * sizeof *name);
	return out;
}

static HANDLE open_path(const wchar_t *path,
        DWORD access,
        DWORD share,
        DWORD disposition,
        DWORD flags,
        DWORD *error)
{
	wchar_t *ext = NULL;
	HANDLE h;

	if (WF_OK != wf_path_to_extended(path, &ext)) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return INVALID_HANDLE_VALUE;
	}
	h = CreateFileW(ext, access, share, NULL, disposition,
	        flags | FILE_FLAG_OPEN_REPARSE_POINT |
	                FILE_FLAG_BACKUP_SEMANTICS,
	        NULL);
	*error = INVALID_HANDLE_VALUE == h ? GetLastError() : 0;
	free(ext);
	return h;
}

static Kind kind_of(const wchar_t *path, DWORD *attrs, DWORD *error)
{
	FILE_BASIC_INFO b;
	HANDLE h = open_path(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	        OPEN_EXISTING, 0, error);
	bool got;

	*attrs = 0;
	if (INVALID_HANDLE_VALUE == h) {
		return ERROR_FILE_NOT_FOUND == *error ||
		                ERROR_PATH_NOT_FOUND == *error
		        ? K_MISSING
		        : K_ERROR;
	}
	got = 0 != GetFileInformationByHandleEx(h, FileBasicInfo, &b, sizeof b);
	if (!got) {
		*error = GetLastError();
	}
	CloseHandle(h);
	if (!got) {
		return K_ERROR;
	}
	*attrs = b.FileAttributes;
	if (b.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		return K_LINK;
	}
	return b.FileAttributes & FILE_ATTRIBUTE_DIRECTORY ? K_DIR : K_FILE;
}

WfStatus wf_wslops_create(
        const wchar_t *dir, const wchar_t *name, bool folder, WfWslReport *r)
{
	wchar_t *target;
	wchar_t *ext = NULL;
	WfStatus st = WF_OK;
	DWORD error;

	if (!r) {
		return WF_E_INVAL;
	}
	begin(r);
	if (!dir || !wf_wslops_name_ok(name)) {
		return fail(r, 0, "that is not a usable name");
	}
	target = join(dir, name);
	if (!target) {
		return WF_E_NOMEM;
	}
	if (WF_OK != wf_path_to_extended(target, &ext)) {
		free(target);
		return WF_E_NOMEM;
	}
	if (folder) {
		if (!CreateDirectoryW(ext, NULL)) {
			error = GetLastError();
			st = fail(r, error,
			        ERROR_ALREADY_EXISTS == error
			                ? "that name is taken"
			                : "the folder cannot be created");
		}
	} else {
		HANDLE h = CreateFileW(ext, GENERIC_WRITE, 0, NULL, CREATE_NEW,
		        FILE_ATTRIBUTE_NORMAL, NULL);

		if (INVALID_HANDLE_VALUE == h) {
			error = GetLastError();
			st = fail(r, error,
			        ERROR_FILE_EXISTS == error
			                ? "that name is taken"
			                : "the file cannot be created");
		} else {
			CloseHandle(h);
		}
	}
	r->done = WF_OK == st;
	free(ext);
	free(target);
	return st;
}

/* "a/b/c": a folder on the way is made when it is missing and entered when it
 * is a folder; a file or a link of that name is never entered, so nothing is
 * created through a link. Folders made before a failure stay. */
WfStatus wf_wslops_create_path(
        const wchar_t *dir, const wchar_t *path, bool folder, WfWslReport *r)
{
	wchar_t name[256];
	wchar_t *cur;
	WfStatus st = WF_OK;
	size_t depth = 0;

	if (!r) {
		return WF_E_INVAL;
	}
	begin(r);
	if (!dir || !path) {
		return fail(r, 0, "that is not a usable name");
	}
	cur = _wcsdup(dir);
	if (!cur) {
		return WF_E_NOMEM;
	}
	for (;;) {
		const wchar_t *after;
		size_t n = wf_name_split_first(path, &after);
		wchar_t *next;
		DWORD attrs, error;
		Kind k;

		if (n >= WF_ARRAY_LEN(name) || ++depth > WF_NAME_PATH_DEPTH) {
			st = fail(r, 0, "that is not a usable name");
			break;
		}
		memcpy(name, path, n * sizeof *name);
		name[n] = L'\0';
		if (!after) {
			st = wf_wslops_create(cur, name, folder, r);
			break;
		}
		if (!wf_wslops_name_ok(name)) {
			st = fail(r, 0, "that is not a usable name");
			break;
		}
		next = join(cur, name);
		if (!next) {
			st = WF_E_NOMEM;
			break;
		}
		k = kind_of(next, &attrs, &error);
		if (K_MISSING == k) {
			st = wf_wslops_create(cur, name, true, r);
		} else if (K_ERROR == k) {
			st = fail(r, error, "the folder cannot be read");
		} else if (K_DIR != k) {
			st = fail(
			        r, ERROR_ALREADY_EXISTS, "that name is taken");
		}
		free(cur);
		cur = next;
		if (WF_OK != st) {
			break;
		}
		path = after;
	}
	free(cur);
	return st;
}

WfStatus wf_wslops_rename(const wchar_t *from_dir,
        const wchar_t *from,
        const wchar_t *to_dir,
        const wchar_t *to,
        WfWslReport *r)
{
	wchar_t *a = NULL, *b = NULL, *ea = NULL, *eb = NULL;
	WfStatus st = WF_OK;
	DWORD attrs, error;
	Kind k;

	if (!r) {
		return WF_E_INVAL;
	}
	begin(r);
	if (!from_dir || !to_dir || !wf_wslops_name_ok(from) ||
	        !wf_wslops_name_ok(to)) {
		return fail(r, 0, "that is not a usable name");
	}
	a = join(from_dir, from);
	b = join(to_dir, to);
	if (!a || !b) {
		st = WF_E_NOMEM;
		goto done;
	}
	k = kind_of(a, &attrs, &error);
	if (K_MISSING == k || K_ERROR == k) {
		st = fail(r, error, "the item is gone");
		goto done;
	}
	k = kind_of(b, &attrs, &error);
	if (K_MISSING != k) {
		st = fail(r, K_ERROR == k ? error : 0, "that name is taken");
		goto done;
	}
	if (WF_OK != wf_path_to_extended(a, &ea) ||
	        WF_OK != wf_path_to_extended(b, &eb)) {
		st = WF_E_NOMEM;
		goto done;
	}
	/* flags 0: no replace and no copy fallback; the name must be free and
	 * the item must not be copied and removed instead */
	if (!MoveFileExW(ea, eb, 0)) {
		st = fail(r, GetLastError(), "the item cannot be moved there");
		goto done;
	}
	r->done = 1;
done:
	free(a);
	free(b);
	free(ea);
	free(eb);
	return st;
}

static bool remove_entry(const wchar_t *path, DWORD attrs, DWORD *error)
{
	wchar_t *ext = NULL;
	bool ok;

	if (WF_OK != wf_path_to_extended(path, &ext)) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return false;
	}
	ok = wf_wslrm_entry(ext, attrs, error);
	free(ext);
	return ok;
}

/* Visits the children of `path`. With `del` they are removed (links as links,
 * never descended into); without it only counted. */
static WfStatus walk(const wchar_t *path,
        unsigned depth,
        bool del,
        size_t *entries,
        WfWslReport *r)
{
	wchar_t *pattern = join(path, L"*"), *ext = NULL;
	WIN32_FIND_DATAW fd;
	HANDLE find;
	WfStatus st = WF_OK;

	if (!pattern) {
		return WF_E_NOMEM;
	}
	if (depth > WF_WSLOPS_MAX_DEPTH) {
		free(pattern);
		return fail(r, 0, "the folder is nested too deeply");
	}
	if (WF_OK != wf_path_to_extended(pattern, &ext)) {
		free(pattern);
		return WF_E_NOMEM;
	}
	find = FindFirstFileExW(ext, FindExInfoBasic, &fd,
	        FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
	free(ext);
	free(pattern);
	if (INVALID_HANDLE_VALUE == find) {
		DWORD e = GetLastError();

		return ERROR_FILE_NOT_FOUND == e
		        ? WF_OK
		        : fail(r, e, "the folder cannot be listed");
	}
	do {
		wchar_t *child;
		bool is_dir, is_link;
		DWORD error = 0;

		if (!wcscmp(fd.cFileName, L".") ||
		        !wcscmp(fd.cFileName, L"..")) {
			continue;
		}
		if (++*entries > WF_WSLOPS_MAX_ENTRIES) {
			st = fail(r, 0, "too many items for one operation");
			break;
		}
		is_link = 0 !=
		        (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
		is_dir = 0 != (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
		child = join(path, fd.cFileName);
		if (!child) {
			st = WF_E_NOMEM;
			break;
		}
		if (!is_link && is_dir) {
			st = walk(child, depth + 1, del, entries, r);
			if (WF_OK == st && del) {
				if (remove_entry(child, fd.dwFileAttributes,
				            &error)) {
					r->done++;
				} else {
					st = fail(r, error,
					        "a folder cannot be removed");
				}
			}
		} else if (del) {
			if (!is_link &&
			        (fd.dwFileAttributes &
			                FILE_ATTRIBUTE_READONLY)) {
				st = fail(
				        r, 0, "a read-only file is in the way");
			} else if (!remove_entry(child, fd.dwFileAttributes,
			                   &error)) {
				st = fail(r, error, "a file cannot be removed");
			} else {
				r->done++;
			}
		}
		free(child);
		if (WF_OK == st && cancelled()) {
			st = fail(r, ERROR_OPERATION_ABORTED, "cancelled");
		}
	} while (WF_OK == st && FindNextFileW(find, &fd));
	if (WF_OK == st && ERROR_NO_MORE_FILES != GetLastError()) {
		st = fail(r, GetLastError(),
		        "the folder listing stopped before its end");
	}
	FindClose(find);
	return st;
}

WfStatus wf_wslops_delete(const wchar_t *path, bool tree, WfWslReport *r)
{
	DWORD attrs, error = 0;
	size_t entries = 0;
	WfStatus st;
	Kind k;

	if (!r) {
		return WF_E_INVAL;
	}
	begin(r);
	if (!path || !*path) {
		return WF_E_INVAL;
	}
	k = kind_of(path, &attrs, &error);
	if (K_MISSING == k || K_ERROR == k) {
		return fail(r, error, "the item is gone");
	}
	if (K_FILE == k && (attrs & FILE_ATTRIBUTE_READONLY)) {
		return fail(r, 0, "the file is read-only");
	}
	if (K_DIR == k) {
		if (tree) {
			st = walk(
			        path, 0, false, &entries, r); /* bound first */
			if (WF_OK != st) {
				return st;
			}
			entries = 0;
			st = walk(path, 0, true, &entries, r);
			if (WF_OK != st) {
				return st;
			}
		}
	}
	if (!remove_entry(path, attrs, &error)) {
		return fail(r, error,
		        K_DIR == k && !tree ? "the folder is not empty"
		                            : "the item cannot be removed");
	}
	r->done++;
	return WF_OK;
}

static bool read_full(HANDLE h, uint8_t *buf, DWORD want, DWORD *got)
{
	*got = 0;
	while (*got < want) {
		DWORD n = 0;

		if (!ReadFile(h, buf + *got, want - *got, &n, NULL)) {
			return false;
		}
		if (0 == n) {
			break;
		}
		*got += n;
	}
	return true;
}

static bool write_full(HANDLE h, const uint8_t *buf, DWORD n)
{
	DWORD at = 0;

	while (at < n) {
		DWORD put = 0;

		if (!WriteFile(h, buf + at, n - at, &put, NULL) || 0 == put) {
			return false;
		}
		at += put;
	}
	return true;
}

static void temp_in(const wchar_t *dir, wchar_t *out, size_t cap)
{
	static LONG counter;

	swprintf(out, cap, L"%ls\\.piero-%lu-%lu-%ld.tmp", dir,
	        (unsigned long)GetCurrentProcessId(),
	        (unsigned long)GetTickCount(), InterlockedIncrement(&counter));
}

static WfStatus compare_back(HANDLE a, HANDLE b, uint8_t *x, uint8_t *y)
{
	LARGE_INTEGER zero = {0};

	if (!SetFilePointerEx(a, zero, NULL, FILE_BEGIN) ||
	        !SetFilePointerEx(b, zero, NULL, FILE_BEGIN)) {
		return WF_E_OS;
	}
	for (;;) {
		DWORD ga, gb;

		if (cancelled()) {
			return WF_E_CANCELLED;
		}
		if (!read_full(a, x, CHUNK, &ga) ||
		        !read_full(b, y, CHUNK, &gb)) {
			return WF_E_OS;
		}
		if (ga != gb || memcmp(x, y, ga)) {
			return WF_E_CHANGED;
		}
		if (0 == ga) {
			return WF_OK;
		}
	}
}

/* The modification time of `from` onto `to` (best effort: a copy that is
 * right in every byte is not undone because a time could not be set). */
static bool copy_times(const wchar_t *from, const wchar_t *to)
{
	DWORD error = 0;
	FILETIME written;
	bool ok = false;
	HANDLE a = open_path(from, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	        OPEN_EXISTING, 0, &error);
	HANDLE b;

	if (INVALID_HANDLE_VALUE == a) {
		return false;
	}
	if (GetFileTime(a, NULL, NULL, &written)) {
		b = open_path(to, FILE_WRITE_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		        OPEN_EXISTING, 0, &error);
		if (INVALID_HANDLE_VALUE != b) {
			ok = 0 != SetFileTime(b, NULL, NULL, &written);
			CloseHandle(b);
		}
	}
	CloseHandle(a);
	return ok;
}

/* The bytes are right whether or not the time could be kept; the user is told
 * the second thing separately. */
static void keep_time(const wchar_t *from, const wchar_t *to, WfWslReport *r)
{
	if (!copy_times(from, to)) {
		r->warnings++;
		r->warning = "the modification time could not be kept";
	}
}

static WfStatus copy_file(const wchar_t *src,
        const wchar_t *dst_dir,
        const wchar_t *name,
        WfWslReport *r,
        Budget *bud)
{
	wchar_t tmp[WF_MAX_PATH_CHARS], *final = join(dst_dir, name);
	wchar_t *etmp = NULL, *efinal = NULL;
	uint8_t *x = malloc(CHUNK), *y = malloc(CHUNK);
	HANDLE hs, ht = INVALID_HANDLE_VALUE;
	FILE_BASIC_INFO b;
	DWORD error = 0;
	WfStatus st = WF_OK;
	bool made = false;

	if (!final || !x || !y) {
		st = WF_E_NOMEM;
		goto done;
	}
	if (wcslen(dst_dir) + 48 >= WF_MAX_PATH_CHARS) {
		st = fail(r, 0, "the path is too long");
		goto done;
	}
	hs = open_path(src, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
	        FILE_FLAG_SEQUENTIAL_SCAN, &error);
	if (INVALID_HANDLE_VALUE == hs) {
		st = fail(r, error, "the source cannot be opened");
		goto done;
	}
	if (!GetFileInformationByHandleEx(hs, FileBasicInfo, &b, sizeof b) ||
	        (b.FileAttributes &
	                (FILE_ATTRIBUTE_REPARSE_POINT |
	                        FILE_ATTRIBUTE_DIRECTORY))) {
		CloseHandle(hs);
		st = fail(r, 0, "the source is not a plain file");
		goto done;
	}
	temp_in(dst_dir, tmp, WF_MAX_PATH_CHARS);
	if (WF_OK != wf_path_to_extended(tmp, &etmp) ||
	        WF_OK != wf_path_to_extended(final, &efinal)) {
		CloseHandle(hs);
		st = WF_E_NOMEM;
		goto done;
	}
	ht = CreateFileW(etmp, GENERIC_READ | GENERIC_WRITE, 0, NULL,
	        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == ht) {
		DWORD create_error = GetLastError(); /* before CloseHandle */

		CloseHandle(hs);
		st = fail(r, create_error, "the copy cannot be created");
		goto done;
	}
	made = true;
	for (;;) {
		DWORD got;

		if (!read_full(hs, x, CHUNK, &got)) {
			st = fail(r, GetLastError(), "reading failed");
			break;
		}
		if (0 == got) {
			break;
		}
		if (!write_full(ht, x, got)) {
			st = fail(r, GetLastError(), "writing failed");
			break;
		}
		InterlockedAdd64(&progress_bytes, (LONG64)got);
		if (bud) {
			bud->bytes += got;
			if (bud->bytes > WF_WSLOPS_MAX_BYTES) {
				st = fail(r, 0,
				        "too much data for one operation");
				break;
			}
		}
		if (cancelled()) {
			st = fail(r, ERROR_OPERATION_ABORTED, "cancelled");
			break;
		}
	}
	if (WF_OK == st && !FlushFileBuffers(ht)) {
		st = fail(r, GetLastError(), "flushing failed");
	}
	if (WF_OK == st) {
		WfStatus cmp = compare_back(hs, ht, x, y);

		if (WF_E_CANCELLED == cmp) {
			st = fail(r, ERROR_OPERATION_ABORTED, "cancelled");
		} else if (WF_OK != cmp) {
			st = fail(r, 0,
			        WF_E_CHANGED == cmp
			                ? "the copy differs from the source"
			                : "the copy could not be read back");
		}
	}
	CloseHandle(hs);
	CloseHandle(ht);
	ht = INVALID_HANDLE_VALUE;
	if (WF_OK == st && cancelled()) { /* not given its name after a stop */
		st = fail(r, ERROR_OPERATION_ABORTED, "cancelled");
	}
	if (WF_OK == st && !MoveFileExW(etmp, efinal, 0)) {
		DWORD e = GetLastError();

		st = fail(r, e,
		        ERROR_ALREADY_EXISTS == e || ERROR_FILE_EXISTS == e
		                ? "that name is taken"
		                : "the copy cannot be given its name");
	}
	if (WF_OK == st) {
		made = false; /* it has its name now */
		keep_time(src, final, r);
		r->done++;
	}
done:
	if (INVALID_HANDLE_VALUE != ht) {
		CloseHandle(ht);
	}
	if (made && etmp) {
		(void)wf_wslrm_created_file(
		        etmp); /* our own CREATE_NEW temporary */
	}
	free(etmp);
	free(efinal);
	free(final);
	free(x);
	free(y);
	return st;
}

typedef struct
{
	size_t entries;
	unsigned long long bytes;
} Measure;

/* Counts what a copy would touch and refuses anything it must not follow. */
static WfStatus measure(
        const wchar_t *path, unsigned depth, Measure *m, WfWslReport *r)
{
	WIN32_FIND_DATAW fd;
	wchar_t *pattern, *ext = NULL;
	HANDLE find;
	WfStatus st = WF_OK;

	if (depth > WF_WSLOPS_MAX_DEPTH) {
		return fail(r, 0, "the folder is nested too deeply");
	}
	pattern = join(path, L"*");
	if (!pattern) {
		return WF_E_NOMEM;
	}
	if (WF_OK != wf_path_to_extended(pattern, &ext)) {
		free(pattern);
		return WF_E_NOMEM;
	}
	find = FindFirstFileExW(ext, FindExInfoBasic, &fd,
	        FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
	free(ext);
	free(pattern);
	if (INVALID_HANDLE_VALUE == find) {
		return ERROR_FILE_NOT_FOUND == GetLastError()
		        ? WF_OK
		        : fail(r, GetLastError(),
		                  "the folder cannot be listed");
	}
	do {
		wchar_t *child;

		if (!wcscmp(fd.cFileName, L".") ||
		        !wcscmp(fd.cFileName, L"..")) {
			continue;
		}
		if (++m->entries > WF_WSLOPS_MAX_ENTRIES) {
			st = fail(r, 0, "too many items for one operation");
			break;
		}
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
			st = fail(
			        r, 0, "a link is inside: links are not copied");
			break;
		}
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			child = join(path, fd.cFileName);
			st = child ? measure(child, depth + 1, m, r)
			           : WF_E_NOMEM;
			free(child);
		} else {
			m->bytes +=
			        ((unsigned long long)fd.nFileSizeHigh << 32) |
			        fd.nFileSizeLow;
			if (m->bytes > WF_WSLOPS_MAX_BYTES) {
				st = fail(r, 0,
				        "too much data for one operation");
			}
		}
	} while (WF_OK == st && FindNextFileW(find, &fd));
	if (WF_OK == st && ERROR_NO_MORE_FILES != GetLastError()) {
		st = fail(r, GetLastError(),
		        "the folder listing stopped before its end");
	}
	FindClose(find);
	return st;
}

static WfStatus copy_tree(const wchar_t *src,
        const wchar_t *dst_dir,
        const wchar_t *name,
        unsigned depth,
        WfWslReport *r,
        Budget *b)
{
	DWORD attrs, error = 0, find_error = 0;
	Kind k = kind_of(src, &attrs, &error);
	wchar_t *made, *pattern, *ext = NULL;
	WIN32_FIND_DATAW fd;
	HANDLE find;
	WfStatus st = WF_OK;

	if (K_FILE == k) {
		/* one file is bounded by the disk, a folder by its budget */
		return copy_file(src, dst_dir, name, r, depth ? b : NULL);
	}
	if (K_DIR != k) {
		return fail(
		        r, error, "the source is not a plain file or folder");
	}
	if (depth > WF_WSLOPS_MAX_DEPTH) {
		return fail(r, 0, "the folder is nested too deeply");
	}
	made = join(dst_dir, name);
	if (!made) {
		return WF_E_NOMEM;
	}
	if (WF_OK != wf_path_to_extended(made, &ext)) {
		free(made);
		return WF_E_NOMEM;
	}
	if (!CreateDirectoryW(ext, NULL)) {
		error = GetLastError();
		free(ext);
		free(made);
		return fail(r, error,
		        ERROR_ALREADY_EXISTS == error
		                ? "that name is taken"
		                : "the folder cannot be created");
	}
	free(ext);
	ext = NULL;
	pattern = join(src, L"*");
	if (!pattern || WF_OK != wf_path_to_extended(pattern, &ext)) {
		free(pattern);
		free(made);
		return WF_E_NOMEM;
	}
	find = FindFirstFileExW(ext, FindExInfoBasic, &fd,
	        FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
	find_error = GetLastError();
	free(ext);
	free(pattern);
	if (INVALID_HANDLE_VALUE != find) {
		do {
			wchar_t *child;

			if (!wcscmp(fd.cFileName, L".") ||
			        !wcscmp(fd.cFileName, L"..")) {
				continue;
			}
			if (b && ++b->entries > WF_WSLOPS_MAX_ENTRIES) {
				st = fail(r, 0,
				        "too many items for one operation");
				break;
			}
			if (cancelled()) {
				st = fail(r, ERROR_OPERATION_ABORTED,
				        "cancelled");
				break;
			}
			child = join(src, fd.cFileName);
			st = child ? copy_tree(child, made, fd.cFileName,
			                     depth + 1, r, b)
			           : WF_E_NOMEM;
			free(child);
		} while (WF_OK == st && FindNextFileW(find, &fd));
		if (WF_OK == st && ERROR_NO_MORE_FILES != GetLastError()) {
			/* an I/O error part-way is not the end of the folder */
			st = fail(r, GetLastError(),
			        "the folder listing stopped before its end");
		}
		FindClose(find);
	} else if (ERROR_FILE_NOT_FOUND != find_error) {
		st = fail(r, find_error, "the folder cannot be listed");
	}
	if (WF_OK == st) {
		wchar_t *whole = join(dst_dir, name);

		if (whole) {
			keep_time(src, whole, r);
			free(whole);
		}
	}
	free(made);
	if (WF_OK == st) {
		r->done++;
	}
	return st;
}

/* Whether `dst` is the folder `src[0..n)` or lies inside it. The distribution
 * (\\wsl.localhost\<distro>) is a Windows name and is compared without regard
 * to case; what comes after it is the Linux path, where foo and Foo are two
 * folders. A path that is not of that form is a Windows path. */
bool wf_wslops_is_inside(const wchar_t *dst, const wchar_t *src, size_t n)
{
	size_t windows_part = n, bars = 0;

	if (L'\\' == src[0] && L'\\' == src[1]) {
		for (size_t i = 2; i < n; i++) {
			bars += L'\\' == src[i];
			if (2 ==
			        bars) { /* \\host\distro\ ends the Windows part */
				windows_part = i;
				break;
			}
		}
	}
	return 0 == _wcsnicmp(dst, src, windows_part) &&
	        0 ==
	        wcsncmp(dst + windows_part, src + windows_part,
	                n - windows_part) &&
	        (L'\0' == dst[n] || L'\\' == dst[n]);
}

WfStatus wf_wslops_copy(const wchar_t *src,
        const wchar_t *dst_dir,
        const wchar_t *name,
        WfWslReport *r)
{
	Measure m = {0};
	Budget budget = {0};
	DWORD attrs, error = 0;
	Kind k;
	WfStatus st;

	if (!r) {
		return WF_E_INVAL;
	}
	begin(r);
	if (!src || !dst_dir || !wf_wslops_name_ok(name)) {
		return fail(r, 0, "that is not a usable name");
	}
	k = kind_of(src, &attrs, &error);
	if (K_MISSING == k || K_ERROR == k) {
		return fail(r, error, "the source is gone");
	}
	if (K_LINK == k) {
		return fail(r, 0, "a link is not copied");
	}
	if (K_DIR == k) {
		size_t n = wcslen(src);

		while (n && L'\\' == src[n - 1]) {
			n--;
		}
		/* into itself would never end */
		if (wf_wslops_is_inside(dst_dir, src, n)) {
			return fail(
			        r, 0, "a folder cannot be copied into itself");
		}
		st = measure(src, 0, &m, r);
		if (WF_OK != st) {
			return st;
		}
	}
	return copy_tree(src, dst_dir, name, 0, r, &budget);
}

WfStatus wf_wslops_copy_in(const wchar_t *src_dir,
        const wchar_t *name,
        const wchar_t *dst_dir,
        const wchar_t *dst_name,
        WfWslReport *r)
{
	wchar_t *src;
	WfStatus st;

	if (!r) {
		return WF_E_INVAL;
	}
	begin(r);
	if (!src_dir || !wf_wslops_name_ok(name)) {
		return fail(r, 0, "that is not a usable name");
	}
	src = join(src_dir, name);
	if (!src) {
		return WF_E_NOMEM;
	}
	st = wf_wslops_copy(src, dst_dir, dst_name, r);
	free(src);
	return st;
}

WfStatus wf_wslops_delete_in(
        const wchar_t *dir, const wchar_t *name, bool tree, WfWslReport *r)
{
	wchar_t *path;
	WfStatus st;

	if (!r) {
		return WF_E_INVAL;
	}
	begin(r);
	if (!dir || !wf_wslops_name_ok(name)) {
		return fail(r, 0, "that is not a usable name");
	}
	path = join(dir, name);
	if (!path) {
		return WF_E_NOMEM;
	}
	st = wf_wslops_delete(path, tree, r);
	free(path);
	return st;
}

WfStatus wf_wslops_remove_if_empty(
        const wchar_t *dir, const wchar_t *name, WfWslReport *r)
{
	wchar_t *path, *ext = NULL;
	DWORD attrs, error = 0;
	WfStatus st = WF_OK;
	Kind k;

	if (!r) {
		return WF_E_INVAL;
	}
	begin(r);
	if (!dir || !wf_wslops_name_ok(name)) {
		return fail(r, 0, "that is not a usable name");
	}
	path = join(dir, name);
	if (!path) {
		return WF_E_NOMEM;
	}
	k = kind_of(path, &attrs, &error);
	if (K_FILE == k) {
		HANDLE h = open_path(path, FILE_READ_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, 0,
		        &error);
		FILE_STANDARD_INFO std;
		bool empty = false;

		if (INVALID_HANDLE_VALUE != h) {
			empty = GetFileInformationByHandleEx(h,
			                FileStandardInfo, &std, sizeof std) &&
			        0 == std.EndOfFile.QuadPart;
			CloseHandle(h);
		}
		if (!empty) {
			st = fail(r, 0, "the file is no longer empty");
		}
	} else if (K_DIR != k) {
		st = fail(r, error, "the item is gone or not a plain file");
	}
	if (WF_OK == st && WF_OK != wf_path_to_extended(path, &ext)) {
		st = WF_E_NOMEM;
	}
	if (WF_OK == st && !wf_wslrm_entry(ext, attrs, &error)) {
		st = fail(r, error,
		        K_DIR == k ? "the folder is no longer empty"
		                   : "the file cannot be removed");
	}
	if (WF_OK == st) {
		r->done = 1;
	}
	free(ext);
	free(path);
	return st;
}

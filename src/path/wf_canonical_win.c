#include "wf_canonical_win.h"

#include <stdlib.h>
#include <string.h>
#include "base/wf_mem.h" /* via -I src: this file is also built from a copy */
#include "fs/wf_weakid_win.h"

#define GUID_ROOT_UNITS 49
#define MAX_PATH_UNITS 32767

static bool hex_digit(wchar_t c)
{
	return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
	        (c >= L'A' && c <= L'F');
}

/* Whether `path` is one that can be normalized (a volume-GUID path with sound
 * components); `*units` is its length. Nothing is allocated. */
static bool canonical_check(const wchar_t *path, size_t *units)
{
	size_t n, i, start;

	if (!path) {
		return 0;
	}
	n = wcsnlen(path, MAX_PATH_UNITS + 1);
	if (n < GUID_ROOT_UNITS || n > MAX_PATH_UNITS ||
	        CSTR_EQUAL !=
	                CompareStringOrdinal(
	                        path, 11, L"\\\\?\\Volume{", 11, TRUE) ||
	        L'}' != path[47] || L'\\' != path[48]) {
		return 0;
	}
	for (i = 0; i < 36; i++) {
		int dash = 8 == i || 13 == i || 18 == i || 23 == i;
		if (dash ? L'-' != path[11 + i] : !hex_digit(path[11 + i])) {
			return 0;
		}
	}
	start = GUID_ROOT_UNITS;
	for (i = start; i <= n; i++) {
		if (i == n || L'\\' == path[i]) {
			size_t len = i - start;
			if (!len && GUID_ROOT_UNITS == n) {
				break;
			}
			if (!len || L'.' == path[i - 1] ||
			        L' ' == path[i - 1]) {
				return 0;
			}
			start = i + 1;
		} else if (path[i] < 32 || wcschr(L"/:*?\"<>|", path[i])) {
			return 0;
		}
	}
	*units = n;
	return 1;
}

bool wf_canonical_normalize(const wchar_t *path, wchar_t **out)
{
	size_t n, i;
	wchar_t *p;

	if (!out) {
		return 0;
	}
	*out = NULL;
	if (!canonical_check(path, &n)) {
		return 0;
	}
	p = wf_malloc_array((n + 1), sizeof *p);
	if (!p) {
		return 0;
	}
	memcpy(p, path, (n + 1) * sizeof *p);
	memcpy(p, L"\\\\?\\Volume{", 11 * sizeof *p);
	for (i = 11; i < 47; i++) {
		if (p[i] >= L'A' && p[i] <= L'F') {
			p[i] += L'a' - L'A';
		}
	}
	*out = p;
	return 1;
}

/* `weak_ok`: on a FAT volume, which has no FileId, the volume serial still says
 * which volume the path is on (wf_weakid_win.h). Every caller that is not
 * handling a new name keeps the refusal. */
static bool canonical_from_handle(
        HANDLE handle, WfCanonicalPath *out, DWORD *error, bool weak_ok)
{
	WfFileId id;
	DWORD n, got;
	wchar_t *raw;

	if (!out || !error) {
		return 0;
	}
	memset(out, 0, sizeof *out);
	*error = 0;
	if (!wf_handle_identity_any(handle, &id, error)) {
		return 0;
	}
	if (wf_fileid_is_weak(&id) && !weak_ok) {
		*error = ERROR_NOT_SUPPORTED;
		return 0;
	}
	n = GetFinalPathNameByHandleW(
	        handle, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
	if (!n) {
		*error = GetLastError();
		return 0;
	}
	if (n > MAX_PATH_UNITS + 1) {
		*error = ERROR_FILENAME_EXCED_RANGE;
		return 0;
	}
	raw = wf_malloc_array((size_t)n, sizeof *raw);
	if (!raw) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	got = GetFinalPathNameByHandleW(
	        handle, raw, n, FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
	if (!got) {
		*error = GetLastError();
	} else if (got >= n || !wf_canonical_normalize(raw, &out->path)) {
		*error = ERROR_INVALID_NAME;
	}
	free(raw);
	if (!out->path) {
		return 0;
	}
	out->volume = id.volume;
	return 1;
}

bool wf_canonical_from_handle(HANDLE handle, WfCanonicalPath *out, DWORD *error)
{
	return canonical_from_handle(handle, out, error, false);
}

bool wf_canonical_from_handle_weak_ok(
        HANDLE handle, WfCanonicalPath *out, DWORD *error)
{
	return canonical_from_handle(handle, out, error, true);
}

/* The same as the two normalized spellings compared, without making them: the
 * first 47 units (the prefix and the GUID) fold ASCII case, which is all that
 * normalizing changes, and the rest is exact. */
WfStatus wf_canonical_compare(
        const WfCanonicalPath *a, const WfCanonicalPath *b, bool *equal)
{
	size_t na, nb;

	if (equal) {
		*equal = false;
	}
	if (!a || !b || !equal || !canonical_check(a->path, &na) ||
	        !canonical_check(b->path, &nb)) {
		return WF_E_INVAL; /* one of them is not a path that can be compared */
	}
	if (a->volume != b->volume || na != nb) {
		return WF_OK; /* different: that is an answer */
	}
	for (size_t i = 0; i < na; i++) {
		wchar_t x = a->path[i], y = b->path[i];

		if (i < 47) {
			x = x >= L'A' && x <= L'Z' ? x + (L'a' - L'A') : x;
			y = y >= L'A' && y <= L'Z' ? y + (L'a' - L'A') : y;
		}
		if (x != y) {
			return WF_OK;
		}
	}
	*equal = true;
	return WF_OK;
}

bool wf_canonical_equal(const WfCanonicalPath *a, const WfCanonicalPath *b)
{
	bool equal;

	return WF_OK == wf_canonical_compare(a, b, &equal) && equal;
}

void wf_canonical_free(WfCanonicalPath *path)
{
	if (!path) {
		return;
	}
	free(path->path);
	memset(path, 0, sizeof *path);
}

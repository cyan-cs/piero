#include "wf_vpath.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../path/wf_path.h"
#include "../base/wf_macro.h"

bool wf_vpath_is(const wchar_t *path)
{
	return path && NULL != wcschr(path, WF_VPATH_MARK);
}

size_t wf_vpath_archive_len(const wchar_t *path)
{
	const wchar_t *m = path ? wcschr(path, WF_VPATH_MARK) : NULL;

	return m ? (size_t)(m - path) : 0;
}

bool wf_vpath_split(
        const wchar_t *path, wchar_t **archive, const wchar_t **inner)
{
	const wchar_t *m;
	wchar_t *a;

	if (!path || !archive || !inner) {
		return false;
	}
	m = wcschr(path, WF_VPATH_MARK);
	if (!m || m == path) {
		return false;
	}
	a = wf_path_dup_n(path, (size_t)(m - path));
	if (!a) {
		return false;
	}
	*archive = a;
	m++;
	while (L'\\' == *m) {
		m++;
	}
	*inner = m;
	return true;
}

WfStatus wf_vpath_root(const wchar_t *file, wchar_t **out)
{
	size_t n;
	wchar_t *r;

	if (!file || !out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	n = wcslen(file);
	if (!n || wcschr(file, WF_VPATH_MARK)) {
		return WF_E_INVAL;
	}
	r = wf_path_dup_n(file, n + 1);
	if (!r) {
		return WF_E_NOMEM;
	}
	r[n] = WF_VPATH_MARK;
	r[n + 1] = L'\0';
	*out = r;
	return WF_OK;
}

void wf_vpath_leaf(const wchar_t *path, const wchar_t **leaf, size_t *len)
{
	size_t n;

	wf_path_leaf(path, leaf, len);
	n = *len;
	/* "...\p.zip|" : the file's name, not the mark */
	if (n && WF_VPATH_MARK == (*leaf)[n - 1]) {
		*len = n - 1;
	}
}

static bool ends_with_nocase(const wchar_t *s, size_t n, const wchar_t *ext)
{
	size_t e = wcslen(ext), i;

	if (n <= e) {
		return false;
	}
	for (i = 0; i < e; i++) {
		wchar_t c = s[n - e + i];

		if (c >= L'A' && c <= L'Z') {
			c = (wchar_t)(c - L'A' + L'a');
		}
		if (c != ext[i]) {
			return false;
		}
	}
	return true;
}

bool wf_archive_name_recognized(const wchar_t *name)
{
	static const wchar_t *const exts[] = {L".zip", L".7z", L".tar", L".tgz",
	        L".txz", L".tzst", L".gz", L".xz", L".zst"};
	size_t n, i;

	if (!name) {
		return false;
	}
	n = wcslen(name);
	for (i = 0; i < WF_ARRAY_LEN(exts); i++) {
		if (ends_with_nocase(name, n, exts[i])) {
			return true;
		}
	}
	return false;
}

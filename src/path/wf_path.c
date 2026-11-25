#include "wf_path.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SEP L'\\'

static bool has_prefix(const wchar_t *p, const wchar_t *prefix)
{
	return 0 == wcsncmp(p, prefix, wcslen(prefix));
}

/* Index just past "srv\share" and its trailing '\' if any. `off` is where
 * the server name starts. A missing share makes the whole string the root. */
static size_t unc_root(const wchar_t *p, size_t n, size_t off)
{
	size_t i = off;

	while (i < n && SEP != p[i]) {
		i++;
	}
	if (i >= n) {
		return n;
	}
	i++; /* '\' after server */
	while (i < n && SEP != p[i]) {
		i++;
	}
	return i < n ? i + 1 : n;
}

/* Length of the root prefix, or 0 if the form is not recognised. */
static size_t root_len(const wchar_t *p, size_t n)
{
	size_t off = 0;

	if (has_prefix(p, L"\\\\?\\UNC\\")) {
		return unc_root(p, n, 8);
	}
	if (has_prefix(p, L"\\\\?\\")) {
		off = 4;
	} else if (has_prefix(p, L"\\\\")) {
		return unc_root(p, n, 2);
	}

	if (n >= off + 3 && L':' == p[off + 1] && SEP == p[off + 2]) {
		return off + 3;
	}
	return 0;
}

size_t wf_path_parent_len(const wchar_t *p)
{
	size_t n = wcslen(p), root = root_len(p, n), e = n, i;

	while (e > root && SEP == p[e - 1]) {
		e--;
	}
	if (e <= root) {
		return 0;
	}
	for (i = e; i > root; i--) {
		if (SEP == p[i - 1]) {
			return i - 1 > root ? i - 1 : root;
		}
	}
	return root;
}

void wf_path_leaf(const wchar_t *p, const wchar_t **leaf, size_t *len)
{
	size_t n = wcslen(p), root = root_len(p, n), e = n, s;

	while (e > root && SEP == p[e - 1]) {
		e--;
	}
	s = e;
	while (s > root && SEP != p[s - 1]) {
		s--;
	}
	*leaf = p + s;
	*len = e - s;
}

wchar_t *wf_path_dup_n(const wchar_t *p, size_t n)
{
	wchar_t *c;

	if (!p && n) {
		return NULL;
	}
	if (n > SIZE_MAX / sizeof *c - 1) {
		return NULL;
	}
	c = malloc((n + 1) * sizeof *c);
	if (!c) {
		return NULL;
	}
	if (n) {
		memcpy(c, p, n * sizeof *c);
	}
	c[n] = L'\0';
	return c;
}

wchar_t *wf_path_dup(const wchar_t *p)
{
	return p ? wf_path_dup_n(p, wcslen(p)) : NULL;
}

bool wf_pcroot_is(const wchar_t *p)
{
	size_t n = wcslen(WF_PCROOT);

	return p && 0 == _wcsnicmp(p, WF_PCROOT, n) &&
	        (!p[n] || (SEP == p[n] && !p[n + 1]));
}

bool wf_path_host_invalid(const wchar_t *p)
{
	static const wchar_t tld[] = L".invalid";
	size_t host = 2, end, n = sizeof tld / sizeof *tld - 1;

	if (!p || SEP != p[0] || SEP != p[1] || L'?' == p[2] || L'.' == p[2]) {
		return false;
	}
	for (end = host; p[end] && SEP != p[end]; end++) {
	}
	return end - host > n && 0 == _wcsnicmp(p + end - n, tld, n);
}

/* A WSL distribution name that can be a path component: what Windows would
 * not rewrite, and what the registry list in wf_wsl_win.h already returns. */
static bool distro_name_ok(const wchar_t *name, size_t n)
{
	if (0 == n || n > 128 || L'.' == name[n - 1] || L' ' == name[n - 1] ||
	        (1 == n && L'.' == name[0]) ||
	        (2 == n && L'.' == name[0] && L'.' == name[1])) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (name[i] < 0x20 || wcschr(L"\\/:*?\"<>|", name[i])) {
			return false;
		}
	}
	return true;
}

#define WSL_PREFIX L"\\\\wsl.localhost\\"

/* The only children of "This PC": the drive "C:" is "C:\", and "wsl:Ubuntu" is
 * \\wsl.localhost\Ubuntu. Nothing else is made from a name here. */
static WfStatus pcroot_child(const wchar_t *name, wchar_t **out)
{
	size_t n = wcslen(name), prefix = wcslen(WSL_PREFIX);
	wchar_t *r;

	if (2 == n && L':' == name[1] &&
	        ((name[0] >= L'A' && name[0] <= L'Z') ||
	                (name[0] >= L'a' && name[0] <= L'z'))) {
		r = malloc(4 * sizeof *r);
		if (!r) {
			return WF_E_NOMEM;
		}
		r[0] = name[0];
		r[1] = L':';
		r[2] = SEP;
		r[3] = L'\0';
	} else if (n > 4 && 0 == wcsncmp(name, L"wsl:", 4) &&
	        distro_name_ok(name + 4, n - 4)) {
		r = malloc((prefix + n - 4 + 1) * sizeof *r);
		if (!r) {
			return WF_E_NOMEM;
		}
		memcpy(r, WSL_PREFIX, prefix * sizeof *r);
		memcpy(r + prefix, name + 4, (n - 4 + 1) * sizeof *r);
	} else {
		return WF_E_INVAL;
	}
	*out = r;
	return WF_OK;
}

wchar_t *wf_pcroot_leaf(const wchar_t *p)
{
	size_t n = p ? wcslen(p) : 0, prefix = wcslen(WSL_PREFIX);
	wchar_t *leaf;

	if (n >= 2 && n <= 3 && L':' == p[1] && (2 == n || SEP == p[2]) &&
	        ((p[0] >= L'A' && p[0] <= L'Z') ||
	                (p[0] >= L'a' && p[0] <= L'z'))) {
		leaf = malloc(3 * sizeof *leaf);
		if (leaf) {
			leaf[0] = (wchar_t)(p[0] &
			        ~0x20); /* "c:" is the drive C: */
			leaf[1] = L':';
			leaf[2] = L'\0';
		}
		return leaf;
	}
	if (n > prefix && 0 == _wcsnicmp(p, WSL_PREFIX, prefix)) {
		size_t d = n - prefix;

		if (SEP == p[n - 1]) {
			d--;
		}
		if (distro_name_ok(p + prefix, d)) {
			leaf = malloc((4 + d + 1) * sizeof *leaf);
			if (leaf) {
				memcpy(leaf, L"wsl:", 4 * sizeof *leaf);
				memcpy(leaf + 4, p + prefix, d * sizeof *leaf);
				leaf[4 + d] = L'\0';
			}
			return leaf;
		}
	}
	return NULL;
}

WfStatus wf_path_join(const wchar_t *dir, const wchar_t *name, wchar_t **out)
{
	size_t dn, nn, total;
	bool need_sep;
	wchar_t *r;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	if (!dir || !name) {
		return WF_E_INVAL;
	}
	if (wf_pcroot_is(dir)) {
		return pcroot_child(name, out);
	}
	dn = wcslen(dir);
	nn = wcslen(name);
	need_sep = dn > 0 && SEP != dir[dn - 1];
	if (0 == nn || wcschr(name, L'\\') || wcschr(name, L'/')) {
		return WF_E_INVAL;
	}
	if (dn > SIZE_MAX / sizeof *r - nn - 2) {
		return WF_E_RANGE;
	}
	total = dn + (need_sep ? 1 : 0) + nn;
	r = malloc((total + 1) * sizeof *r);
	if (!r) {
		return WF_E_NOMEM;
	}
	memcpy(r, dir, dn * sizeof *r);
	if (need_sep) {
		r[dn] = SEP;
	}
	memcpy(r + dn + (need_sep ? 1 : 0), name, nn * sizeof *r);
	r[total] = L'\0';
	*out = r;
	return WF_OK;
}

/* Rejects '/' anywhere and "." / ".." components after the root. */
static bool components_ok(const wchar_t *p, size_t n, size_t root)
{
	size_t i = root, start;

	if (wcschr(p, L'/')) {
		return 0;
	}
	while (i < n) {
		start = i;
		while (i < n && SEP != p[i]) {
			i++;
		}
		if ((i - start == 1 && L'.' == p[start]) ||
		        (i - start == 2 && L'.' == p[start] &&
		                L'.' == p[start + 1])) {
			return 0;
		}
		i++;
	}
	return 1;
}

WfStatus wf_path_to_extended(const wchar_t *abs, wchar_t **out)
{
	size_t n, root;
	const wchar_t *prefix, *body;
	size_t pn, bn;
	wchar_t *r;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	if (!abs) {
		return WF_E_INVAL;
	}
	n = wcslen(abs);
	root = root_len(abs, n);
	if (0 == root || !components_ok(abs, n, root)) {
		return WF_E_INVAL;
	}

	if (has_prefix(abs, L"\\\\?\\")) {
		prefix = L"";
		body = abs;
	} else if (has_prefix(abs, L"\\\\")) {
		prefix = L"\\\\?\\UNC\\";
		body = abs + 2;
	} else {
		prefix = L"\\\\?\\";
		body = abs;
	}
	pn = wcslen(prefix);
	bn = wcslen(body);
	if (bn > SIZE_MAX / sizeof *r - pn - 1) {
		return WF_E_RANGE;
	}
	r = malloc((pn + bn + 1) * sizeof *r);
	if (!r) {
		return WF_E_NOMEM;
	}
	memcpy(r, prefix, pn * sizeof *r);
	memcpy(r + pn, body, (bn + 1) * sizeof *r);
	*out = r;
	return WF_OK;
}

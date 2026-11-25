#include "wf_loc.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

uint32_t wf_caps_default(WfBackendKind kind)
{
	switch (kind) {
	case WF_BK_LOCAL:
		return WF_CAP_LIST | WF_CAP_READ | WF_CAP_READ_RANGE |
		        WF_CAP_NEW_FILE | WF_CAP_SAVE_OVER | WF_CAP_RENAME |
		        WF_CAP_FSYNC | WF_CAP_IDENTITY;
	case WF_BK_WSL:
	case WF_BK_SFTP: return WF_CAP_LIST | WF_CAP_READ | WF_CAP_READ_RANGE;
	}
	return 0; /* an unknown kind can do nothing */
}

uint32_t wf_caps_grant_save(
        WfBackendKind kind, uint32_t caps, const WfSaveProof *p)
{
	if (WF_BK_LOCAL == kind || !p) {
		return caps; /* Windows has its own proofs; nothing to add */
	}
	/* A new file needs a sibling nobody else can take over, a publish that
	 * fails if the name appeared, durable data and a read-back. */
	if (p->staging_in_same_directory && p->noclobber_publish &&
	        p->data_durable_before_commit && p->readback_verified) {
		caps |= WF_CAP_NEW_FILE;
		/* Replacing also needs the old name never to be absent and the
		 * swapped object to be the one that was read. */
		if (p->atomic_replace && p->identity_checked_at_commit) {
			caps |= WF_CAP_SAVE_OVER | WF_CAP_RENAME | WF_CAP_FSYNC;
		}
	}
	return caps;
}

static bool id_chars_ok(const char *s, size_t max)
{
	size_t n = s ? strlen(s) : 0;

	if (n < 1 || n > max || '-' == s[0]) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
		            (c >= 'A' && c <= 'Z') || '.' == c || '_' == c ||
		            '-' == c || '+' == c)) {
			return false;
		}
	}
	return true;
}

WfStatus wf_locid_local(WfLocId *out)
{
	if (!out) {
		return WF_E_INVAL;
	}
	out->kind = WF_BK_LOCAL;
	out->key[0] = 0;
	return WF_OK;
}

WfStatus wf_locid_wsl(WfLocId *out, const char *distro)
{
	if (!out || !id_chars_ok(distro, 128)) {
		return WF_E_INVAL;
	}
	out->kind = WF_BK_WSL;
	memcpy(out->key, distro, strlen(distro) + 1);
	return WF_OK;
}

WfStatus wf_locid_sftp(WfLocId *out, const char *alias, unsigned port)
{
	int n;

	if (!out || !id_chars_ok(alias, 100) || port > 65535u) {
		return WF_E_INVAL;
	}
	n = snprintf(
	        out->key, sizeof out->key, "%s:%u", alias, port ? port : 22u);
	if (n < 0 || (size_t)n >= sizeof out->key) {
		return WF_E_RANGE;
	}
	out->kind = WF_BK_SFTP;
	return WF_OK;
}

bool wf_locid_equal(const WfLocId *a, const WfLocId *b)
{
	return a && b && a->kind == b->kind && !strcmp(a->key, b->key);
}

WfStatus wf_loc_memory_key(const WfLocId *loc,
        const uint8_t *path,
        size_t path_n,
        uint8_t *out,
        size_t cap,
        size_t *out_n)
{
	size_t klen;
	size_t total;

	if (!loc || !out || !out_n || (path_n && !path)) {
		return WF_E_INVAL;
	}
	klen = strlen(loc->key);
	/* kind byte, key, 0, path: the key never contains a NUL, so the split
	 * is unambiguous and two places cannot produce the same bytes */
	if (klen > SIZE_MAX - 3 || path_n > SIZE_MAX - klen - 3) {
		return WF_E_RANGE;
	}
	total = 1 + klen + 1 + path_n;
	if (total + 1 > cap) {
		return WF_E_RANGE;
	}
	out[0] = (uint8_t)('0' + (int)loc->kind);
	memcpy(out + 1, loc->key, klen);
	out[1 + klen] = 0;
	if (path_n) {
		memcpy(out + 2 + klen, path, path_n);
	}
	out[total] = 0;
	*out_n = total;
	return WF_OK;
}

bool wf_posix_name_ok(const uint8_t *name, size_t n)
{
	if (!name || n < 1 || n > WF_POSIX_NAME_MAX) {
		return false;
	}
	if ((1 == n && '.' == name[0]) ||
	        (2 == n && '.' == name[0] && '.' == name[1])) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (0 == name[i] || '/' == name[i]) {
			return false;
		}
	}
	return true;
}

bool wf_posix_path_ok(const uint8_t *path, size_t n)
{
	size_t i = 1;

	if (!path || n < 1 || n > WF_POSIX_PATH_MAX || '/' != path[0]) {
		return false;
	}
	while (i < n) {
		size_t start = i;

		while (i < n && '/' != path[i]) {
			i++;
		}
		if (!wf_posix_name_ok(path + start, i - start)) {
			return false; /* empty, ".", "..", NUL or too long */
		}
		if (i < n) {
			i++; /* the separator; a trailing one leaves an empty
			        component and is caught on the next lap only if
			        something follows, so check it here */
			if (i == n) {
				return false;
			}
		}
	}
	return true;
}

WfStatus wf_posix_join(const uint8_t *dir,
        size_t dir_n,
        const uint8_t *name,
        size_t name_n,
        uint8_t *out,
        size_t cap,
        size_t *out_n)
{
	size_t pos;
	bool root;

	if (!out || !out_n || !wf_posix_path_ok(dir, dir_n) ||
	        !wf_posix_name_ok(name, name_n)) {
		return WF_E_INVAL;
	}
	root = 1 == dir_n; /* "/" */
	pos = dir_n + (root ? 0 : 1);
	if (pos + name_n > WF_POSIX_PATH_MAX || pos + name_n + 1 > cap) {
		return WF_E_RANGE;
	}
	memcpy(out, dir, dir_n);
	if (!root) {
		out[dir_n] = '/';
	}
	memcpy(out + pos, name, name_n);
	out[pos + name_n] = 0;
	*out_n = pos + name_n;
	return WF_OK;
}

/* One strict UTF-8 sequence at s[0..n): its length and scalar, or 0. */
static size_t utf8_one(const uint8_t *s, size_t n, uint32_t *cp)
{
	uint8_t c = s[0];

	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	if (c >= 0xC2 && c <= 0xDF && n >= 2 && (s[1] & 0xC0) == 0x80) {
		*cp = (uint32_t)(c & 0x1F) << 6 | (s[1] & 0x3F);
		return 2;
	}
	if (c >= 0xE0 && c <= 0xEF && n >= 3 && (s[1] & 0xC0) == 0x80 &&
	        (s[2] & 0xC0) == 0x80) {
		uint32_t v = (uint32_t)(c & 0x0F) << 12 |
		        (uint32_t)(s[1] & 0x3F) << 6 | (s[2] & 0x3F);

		if (v >= 0x800 && !(v >= 0xD800 && v <= 0xDFFF)) {
			*cp = v;
			return 3;
		}
		return 0;
	}
	if (c >= 0xF0 && c <= 0xF4 && n >= 4 && (s[1] & 0xC0) == 0x80 &&
	        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
		uint32_t v = (uint32_t)(c & 0x07) << 18 |
		        (uint32_t)(s[1] & 0x3F) << 12 |
		        (uint32_t)(s[2] & 0x3F) << 6 | (s[3] & 0x3F);

		if (v >= 0x10000 && v <= 0x10FFFF) {
			*cp = v;
			return 4;
		}
	}
	return 0;
}

static bool scalar_needs_escape(uint32_t c)
{
	return c < 0x20 || (c >= 0x7F && c <= 0x9F) || '\\' == c ||
	        0x061C == c || 0x200E == c || 0x200F == c || 0x2028 == c ||
	        0x2029 == c || (c >= 0x202A && c <= 0x202E) ||
	        (c >= 0x2066 && c <= 0x2069);
}

size_t wf_name_display(
        const uint8_t *raw, size_t n, char *out, size_t cap, bool *truncated)
{
	size_t at = 0, o = 0;
	bool cut = false;

	if (truncated) {
		*truncated = false;
	}
	if (!out || 0 == cap) {
		return 0;
	}
	while (at < n && !cut) {
		char piece[16];
		size_t len, adv;
		uint32_t cp = 0;
		size_t seq = utf8_one(raw + at, n - at, &cp);

		if (seq && !scalar_needs_escape(cp)) {
			memcpy(piece, raw + at, seq);
			len = seq;
			adv = seq;
		} else if (seq && '\\' == cp) {
			memcpy(piece, "\\\\", 2);
			len = 2;
			adv = 1;
		} else if (seq && cp >= 0x80) {
			len = (size_t)snprintf(
			        piece, sizeof piece, "\\u{%04X}", (unsigned)cp);
			adv = seq;
		} else {
			len = (size_t)snprintf(piece, sizeof piece, "\\x%02X",
			        (unsigned)raw[at]);
			adv = 1;
		}
		if (o + len + 1 > cap) {
			cut = true; /* whole piece or nothing: a character boundary */
			break;
		}
		memcpy(out + o, piece, len);
		o += len;
		at += adv;
	}
	out[o] = 0;
	if (truncated) {
		*truncated = cut || at < n;
	}
	return o;
}

static bool starts_with_ci(const wchar_t *s, const wchar_t *prefix, size_t *len)
{
	size_t n = wcslen(prefix);

	if (_wcsnicmp(s, prefix, n)) {
		return false;
	}
	*len = n;
	return true;
}

bool wf_wsl_unc_is(const wchar_t *path)
{
	static const wchar_t *const roots[] = {L"\\\\wsl.localhost",
	        L"\\\\wsl$", L"\\\\?\\UNC\\wsl.localhost", L"\\\\?\\UNC\\wsl$"};
	size_t len;

	if (!path) {
		return false;
	}
	for (size_t i = 0; i < sizeof roots / sizeof *roots; i++) {
		/* the server name ends at a separator or at the end */
		if (starts_with_ci(path, roots[i], &len) &&
		        (L'\\' == path[len] || L'/' == path[len] ||
		                !path[len])) {
			return true;
		}
	}
	return false;
}

WfNewEol wf_new_file_eol(WfBackendKind kind)
{
	return WF_BK_LOCAL == kind ? WF_NEW_EOL_CRLF : WF_NEW_EOL_LF;
}

/* The distribution name of a WSL path, or false. */
static bool wsl_distro_of(const wchar_t *path, const wchar_t **at, size_t *n)
{
	const wchar_t *p;

	if (!wf_wsl_unc_is(path)) {
		return false;
	}
	p = path;
	if (!_wcsnicmp(p, L"\\\\?\\UNC\\", 8)) {
		p += 8;
	} else {
		p += 2;
	}
	while (*p && L'\\' != *p && L'/' != *p) { /* the server name */
		p++;
	}
	while (L'\\' == *p || L'/' == *p) {
		p++;
	}
	*at = p;
	*n = 0;
	while ((*at)[*n] && L'\\' != (*at)[*n] && L'/' != (*at)[*n]) {
		(*n)++;
	}
	return *n > 0;
}

bool wf_wsl_same_distro(const wchar_t *a, const wchar_t *b)
{
	const wchar_t *da, *db;
	size_t na = 0, nb = 0;

	return wsl_distro_of(a, &da, &na) && wsl_distro_of(b, &db, &nb) &&
	        na == nb && 0 == wcsncmp(da, db, na);
}

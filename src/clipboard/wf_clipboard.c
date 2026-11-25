#include "wf_clipboard.h"

#include <stdlib.h>
#include <string.h>

#include "../session/wf_session.h"

void wf_clipfiles_free(WfClipFiles *f)
{
	for (size_t i = 0; f->path && i < f->n; i++) {
		free(f->path[i]);
	}
	free(f->path);
	memset(f, 0, sizeof *f);
}

bool wf_clip_effect_is_cut(DWORD effect)
{
	return (effect & 2u) && !(effect & 1u); /* DROPEFFECT_MOVE, _COPY */
}

DWORD wf_clip_parse_effect(const void *data, size_t bytes)
{
	DWORD v = 0;

	if (!data || bytes < sizeof v) {
		return 0;
	}
	memcpy(&v, data, sizeof v);
	return v;
}

/* Valid UTF-16: every surrogate is half of a pair. */
static bool utf16_ok(const wchar_t *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (s[i] >= 0xD800 && s[i] <= 0xDBFF) {
			if (i + 1 >= n || s[i + 1] < 0xDC00 ||
			        s[i + 1] > 0xDFFF) {
				return false;
			}
			i++;
		} else if (s[i] >= 0xDC00 && s[i] <= 0xDFFF) {
			return false;
		}
	}
	return true;
}

/* Adds one path (a copy of s[0..n)): OK, or why it could not be (too many
 * files, a name that is not acceptable, memory). */
static WfClipReason add_path(WfClipFiles *out, const wchar_t *s, size_t n)
{
	wchar_t *copy;

	if (out->n >= WF_CLIPFILES_MAX) {
		return WF_CLIP_TOO_MANY;
	}
	if (n > WF_CLIPFILES_PATH || !utf16_ok(s, n)) {
		return WF_CLIP_BAD;
	}
	copy = malloc((n + 1) * sizeof *copy);
	if (!copy) {
		return WF_CLIP_FAILED;
	}
	memcpy(copy, s, n * sizeof *copy);
	copy[n] = L'\0';
	if (!wf_session_path_ok(copy)) { /* absolute, no . or .. component */
		free(copy);
		return WF_CLIP_BAD;
	}
	if (!out->path) {
		/* the limit is small: one allocation holds every path that may come */
		out->path = malloc(WF_CLIPFILES_MAX * sizeof *out->path);
		if (!out->path) {
			free(copy);
			return WF_CLIP_FAILED;
		}
	}
	out->path[out->n++] = copy;
	return WF_CLIP_OK;
}

/* The DROPFILES header: DWORD pFiles; POINT pt; BOOL fNC; BOOL fWide. */
#define HEADER_BYTES 20u

WfClipReason wf_clip_parse_hdrop(
        const void *data, size_t bytes, WfClipFiles *out)
{
	const unsigned char *b = data;
	DWORD offset, wide;
	size_t rest;
	WfClipReason why = WF_CLIP_OK;

	memset(out, 0, sizeof *out);
	if (!b || bytes < HEADER_BYTES) {
		return WF_CLIP_BAD;
	}
	memcpy(&offset, b, sizeof offset);
	memcpy(&wide, b + 16, sizeof wide);
	if (offset < HEADER_BYTES || offset > bytes) {
		return WF_CLIP_BAD;
	}
	rest = bytes - offset;
	if (wide) {
		const wchar_t *w = (const wchar_t *)(const void *)(b + offset);
		size_t units = rest / sizeof(wchar_t), at = 0;
		wchar_t *aligned = NULL;

		if ((uintptr_t)w %
		        sizeof *w) { /* only a list at an odd offset: copied */
			aligned = malloc(
			        units * sizeof *aligned + sizeof *aligned);
			if (!aligned) {
				return WF_CLIP_FAILED;
			}
			memcpy(aligned, b + offset, units * sizeof *aligned);
			w = aligned;
		}
		for (;;) {
			size_t len = 0;

			while (at + len < units && w[at + len]) {
				len++;
			}
			if (at + len >= units) {
				why = WF_CLIP_BAD; /* no terminator before the end */
				break;
			}
			if (!len) {
				break; /* the empty name that ends the list */
			}
			why = add_path(out, w + at, len);
			if (WF_CLIP_OK != why) {
				break;
			}
			at += len + 1;
		}
		free(aligned);
	} else {
		const char *a = (const char *)b + offset;
		size_t at = 0;

		for (;;) {
			size_t len = 0;
			int units;
			wchar_t *w;

			while (at + len < rest && a[at + len]) {
				len++;
			}
			if (at + len >= rest) {
				why = WF_CLIP_BAD;
				break;
			}
			if (!len) {
				break;
			}
			units = MultiByteToWideChar(CP_ACP,
			        MB_ERR_INVALID_CHARS, a + at, (int)len, NULL,
			        0);
			if (units <= 0 || (size_t)units > WF_CLIPFILES_PATH) {
				why = WF_CLIP_BAD;
				break;
			}
			w = malloc((size_t)units * sizeof *w);
			if (!w ||
			        units !=
			                MultiByteToWideChar(CP_ACP,
			                        MB_ERR_INVALID_CHARS, a + at,
			                        (int)len, w, units)) {
				free(w);
				why = WF_CLIP_BAD;
				break;
			}
			why = add_path(out, w, (size_t)units);
			free(w);
			if (WF_CLIP_OK != why) {
				break;
			}
			at += len + 1;
		}
	}
	if (WF_CLIP_OK == why && !out->n) {
		why = WF_CLIP_NONE; /* a list with no names */
	}
	if (WF_CLIP_OK != why) {
		wf_clipfiles_free(out);
	}
	return why;
}

WfStatus wf_clip_build_hdrop(
        const wchar_t *const *path, size_t n, void **block, size_t *bytes)
{
	size_t total =
	        HEADER_BYTES + sizeof(wchar_t); /* the final empty name */
	unsigned char *b;
	DWORD header[5] = {
	        HEADER_BYTES, 0, 0, 0, 1}; /* pFiles, pt, fNC, fWide */
	size_t at = HEADER_BYTES;

	*block = NULL;
	*bytes = 0;
	if (!n || n > WF_CLIPFILES_MAX) {
		return WF_E_RANGE;
	}
	for (size_t i = 0; i < n; i++) {
		size_t len = path[i] ? wcslen(path[i]) : 0;

		if (len > WF_CLIPFILES_PATH || !wf_session_path_ok(path[i]) ||
		        !utf16_ok(path[i], len)) {
			return WF_E_RANGE; /* only what this program would read back */
		}
		total += (len + 1) * sizeof(wchar_t);
	}
	b = calloc(1, total);
	if (!b) {
		return WF_E_NOMEM;
	}
	memcpy(b, header, sizeof header);
	for (size_t i = 0; i < n; i++) {
		size_t len = (wcslen(path[i]) + 1) * sizeof(wchar_t);

		memcpy(b + at, path[i], len);
		at += len;
	}
	*block = b;
	*bytes = total;
	return WF_OK;
}

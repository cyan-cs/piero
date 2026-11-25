#include "wf_preview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../text/wf_text.h"
#include "../base/wf_macro.h"

WfPreview *wf_preview_notice(WfPreviewKind kind, const char *detail)
{
	WfPreview *p;
	WfBuf clean;
	size_t n;

	if (!detail) {
		return NULL;
	}
	p = calloc(1, sizeof *p);
	if (!p) {
		return NULL;
	}
	p->kind = kind;
	wf_buf_init(&p->text);
	wf_buf_init(&clean);
	if (WF_OK != wf_sanitize_utf8(detail, strlen(detail), &clean)) {
		wf_preview_free(p);
		wf_buf_free(&clean);
		return NULL;
	}
	n = clean.len < sizeof p->detail - 1 ? clean.len : sizeof p->detail - 1;
	while (n && n < clean.len &&
	        0x80 == ((unsigned char)clean.data[n] & 0xc0)) {
		n--;
	}
	memcpy(p->detail, wf_buf_cstr(&clean), n);
	p->detail[n] = '\0';
	wf_buf_free(&clean);
	return p;
}

typedef enum
{
	ENC_UTF8,
	ENC_UTF16LE,
	ENC_UTF16BE
} Encoding;
typedef enum
{
	SCALAR_BAD,
	SCALAR_OK,
	SCALAR_INCOMPLETE
} ScalarResult;

/* BAD for malformed input, INCOMPLETE for an incomplete final scalar in a
 * deliberately bounded prefix. BOM byte order is handled without casts. */
static ScalarResult next_scalar(const unsigned char *b,
        size_t n,
        size_t *i,
        Encoding encoding,
        bool truncated,
        uint32_t *cp)
{
	size_t k, remaining = n - *i;
	uint32_t a, lo;

	if (ENC_UTF8 == encoding) {
		k = wf_utf8_decode((const char *)b + *i, remaining, cp);
		if (1 == k && b[*i] >= 0x80) {
			unsigned first = b[*i];
			size_t need = first >= 0xc2 && first <= 0xdf ? 2
			        : first >= 0xe0 && first <= 0xef     ? 3
			        : first >= 0xf0 && first <= 0xf4     ? 4
			                                             : 0;
			if (truncated && need && remaining < need) {
				for (k = 1; k < remaining; k++) {
					if (0x80 != (b[*i + k] & 0xc0)) {
						return SCALAR_BAD;
					}
				}
				if (remaining > 1 &&
				        ((0xe0 == first && b[*i + 1] < 0xa0) ||
				                (0xed == first &&
				                        b[*i + 1] >= 0xa0) ||
				                (0xf0 == first &&
				                        b[*i + 1] < 0x90) ||
				                (0xf4 == first &&
				                        b[*i + 1] >= 0x90))) {
					return SCALAR_BAD;
				}
				return SCALAR_INCOMPLETE;
			}
			return SCALAR_BAD;
		}
		*i += k;
		return SCALAR_OK;
	}
	if (remaining < 2) {
		return truncated ? SCALAR_INCOMPLETE : SCALAR_BAD;
	}
	a = ENC_UTF16LE == encoding
	        ? (uint32_t)b[*i] | ((uint32_t)b[*i + 1] << 8)
	        : ((uint32_t)b[*i] << 8) | b[*i + 1];
	*i += 2;
	if (a >= 0xd800 && a <= 0xdbff) {
		if (n - *i < 2) {
			return truncated ? SCALAR_INCOMPLETE : SCALAR_BAD;
		}
		lo = ENC_UTF16LE == encoding
		        ? (uint32_t)b[*i] | ((uint32_t)b[*i + 1] << 8)
		        : ((uint32_t)b[*i] << 8) | b[*i + 1];
		if (lo < 0xdc00 || lo > 0xdfff) {
			return SCALAR_BAD;
		}
		*i += 2;
		a = 0x10000 + ((a - 0xd800) << 10) + lo - 0xdc00;
	} else if (a >= 0xdc00 && a <= 0xdfff) {
		return SCALAR_BAD;
	}
	*cp = a;
	return SCALAR_OK;
}

/* BOM wins; without one the input is taken as UTF-8. *start is the first
 * byte after the BOM. */
static Encoding detect_encoding(const unsigned char *b, size_t n, size_t *start)
{
	*start = 0;
	if (n >= 3 && !memcmp(b, "\xef\xbb\xbf", 3)) {
		*start = 3;
	} else if (n >= 2 && 0xff == b[0] && 0xfe == b[1]) {
		*start = 2;
		return ENC_UTF16LE;
	} else if (n >= 2 && 0xfe == b[0] && 0xff == b[1]) {
		*start = 2;
		return ENC_UTF16BE;
	}
	return ENC_UTF8;
}

/* Judges the WHOLE bounded input, including what the line limits will hide:
 * malformed text is INVALID, mostly-control or NUL-bearing data is BINARY,
 * anything else is TEXT. Does not build anything. */
static WfPreviewKind classify_input(const unsigned char *b,
        size_t n,
        size_t start,
        Encoding encoding,
        bool truncated)
{
	size_t i, scalars = 0, controls = 0;

	for (i = start; i < n;) {
		uint32_t cp;
		ScalarResult got =
		        next_scalar(b, n, &i, encoding, truncated, &cp);

		if (SCALAR_INCOMPLETE == got) {
			break;
		}
		if (SCALAR_BAD == got) {
			return WF_PREVIEW_INVALID;
		}
		scalars++;
		if (0 == cp) {
			return WF_PREVIEW_BINARY;
		}
		if (cp < 32 && '\t' != cp && '\r' != cp && '\n' != cp &&
		        27 != cp) {
			controls++;
		}
	}
	return scalars && controls > scalars / 10 ? WF_PREVIEW_BINARY
	                                          : WF_PREVIEW_TEXT;
}

/* Fills p->text / p->lines with the sanitized, bounded display lines. Marks
 * the preview truncated when a line or the line count hit their limit. */
static WfStatus build_lines(WfPreview *p,
        const unsigned char *b,
        size_t n,
        size_t start,
        Encoding encoding,
        bool truncated,
        bool keep_tabs)
{
	size_t i, line_bytes = 0;
	bool last_cr = false;
	WfStatus st = WF_OK;
	WfBuf clean; /* one buffer for every scalar: cleared, not made again */

	wf_buf_init(&clean);
	p->lines[p->nlines++] = 0;
	for (i = start; i < n;) {
		uint32_t cp;
		char utf8[4];
		size_t len;
		ScalarResult got =
		        next_scalar(b, n, &i, encoding, truncated, &cp);
		if (SCALAR_OK != got) {
			break;
		}
		if ('\r' == cp || '\n' == cp) {
			if ('\n' == cp && last_cr) {
				last_cr = false;
				continue;
			}
			last_cr = '\r' == cp;
			if (WF_PREVIEW_LINES == p->nlines) {
				p->truncated = true;
				break;
			}
			if (WF_OK != (st = wf_buf_putc(&p->text, '\0'))) {
				goto done;
			}
			p->lines[p->nlines++] = p->text.len;
			line_bytes = 0;
			continue;
		}
		last_cr = false;
		if (line_bytes >= WF_PREVIEW_LINE_BYTES) {
			p->truncated = true;
			continue;
		}
		len = wf_utf8_encode(cp, utf8);
		wf_buf_clear(&clean);
		if (keep_tabs && '\t' == cp) {
			st = wf_buf_putc(&clean, '\t'); /* a TSV's delimiter */
		} else {
			st = wf_sanitize_utf8(utf8, len, &clean);
		}
		if (WF_OK == st &&
		        clean.len <= WF_PREVIEW_LINE_BYTES - line_bytes &&
		        p->text.len + clean.len < 2u * WF_PREVIEW_HARD_BYTES) {
			st = wf_buf_append(
			        &p->text, wf_buf_cstr(&clean), clean.len);
			line_bytes += clean.len;
		} else if (WF_OK == st) {
			p->truncated = true;
			line_bytes = WF_PREVIEW_LINE_BYTES;
		}
		if (WF_OK != st) {
			goto done;
		}
	}
	st = wf_buf_putc(&p->text, '\0');
done:
	wf_buf_free(&clean);
	return st;
}

/* Widest line in display cells, for horizontal scrolling. */
static void measure_widest(WfPreview *p)
{
	size_t i;

	for (i = 0; i < p->nlines; i++) {
		const char *line = p->text.data + p->lines[i];
		int cells = wf_utf8_cells(line, strlen(line));

		if (cells > 0 && (size_t)cells > p->max_cells) {
			p->max_cells = (size_t)cells;
		}
	}
}

WfStatus wf_preview_decode(
        const void *bytes, size_t n, bool truncated, WfPreview **out)
{
	return wf_preview_decode_opt(bytes, n, truncated, false, out);
}

WfStatus wf_preview_decode_opt(const void *bytes,
        size_t n,
        bool truncated,
        bool keep_tabs,
        WfPreview **out)
{
	const unsigned char *b = bytes;
	WfPreview *p;
	size_t start;
	Encoding encoding;
	WfPreviewKind kind;
	WfStatus st;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	if ((!bytes && n) || n > WF_PREVIEW_HARD_BYTES) {
		return WF_E_INVAL;
	}
	encoding = detect_encoding(b, n, &start);
	p = wf_preview_notice(WF_PREVIEW_TEXT,
	        ENC_UTF16LE == encoding           ? "UTF-16 LE"
	                : ENC_UTF16BE == encoding ? "UTF-16 BE"
	                                          : "UTF-8");
	if (!p) {
		return WF_E_NOMEM;
	}
	p->truncated = truncated;
	kind = classify_input(b, n, start, encoding, truncated);
	if (WF_PREVIEW_TEXT != kind) {
		p->kind = kind;
		snprintf(p->detail, sizeof p->detail, "%s",
		        WF_PREVIEW_INVALID == kind
		                ? "Invalid encoding; only strict UTF-8 / BOM "
		                  "UTF-16 supported"
		                : "Binary content (no automatic text display)");
		*out = p;
		return WF_OK;
	}
	st = build_lines(p, b, n, start, encoding, truncated, keep_tabs);
	if (WF_OK != st) {
		wf_preview_free(p);
		return st;
	}
	measure_widest(p);
	*out = p;
	return WF_OK;
}
void wf_preview_free(WfPreview *p)
{
	if (!p) {
		return;
	}
	wf_buf_free(&p->text);
	free(p);
}
/* Only absolute local drive paths, including extended drive paths. */
static bool local_drive_path(const wchar_t *path)
{
	size_t n;

	if (!path) {
		return false;
	}
	n = wcslen(path);
	if (n >= 7 && !wcsncmp(path, L"\\\\?\\", 4)) {
		path += 4;
		n -= 4;
	}
	return n >= 3 &&
	        ((path[0] >= L'A' && path[0] <= L'Z') ||
	                (path[0] >= L'a' && path[0] <= L'z')) &&
	        L':' == path[1] && L'\\' == path[2];
}

bool wf_preview_auto_allowed(const wchar_t *path, uint32_t flags)
{
	return !(flags & (WF_ENT_DIR | WF_ENT_CLOUD | WF_ENT_LINK)) &&
	        local_drive_path(path);
}

bool wf_preview_dir_auto_allowed(const wchar_t *path, uint32_t flags)
{
	return !(flags & (WF_ENT_CLOUD | WF_ENT_LINK)) &&
	        local_drive_path(path);
}

bool wf_preview_is_pdf(const wchar_t *name)
{
	const wchar_t *dot = name ? wcsrchr(name, L'.') : NULL;

	return dot && 0 == _wcsicmp(dot, L".pdf");
}

bool wf_preview_is_image(const wchar_t *name)
{
	/* Candidate formats only; the decoder determines codec availability. */
	static const wchar_t *const ext[] = {L".png", L".jpg", L".jpeg",
	        L".jpe", L".jfif", L".bmp", L".gif", L".tif", L".tiff", L".ico",
	        L".webp", L".heic", L".heif", L".avif", L".jxl", L".wdp",
	        L".jxr"};
	const wchar_t *dot;
	size_t i;
	if (!name) {
		return false;
	}
	dot = wcsrchr(name, L'.');
	if (!dot) {
		return false;
	}
	for (i = 0; i < WF_ARRAY_LEN(ext); i++) {
		if (!_wcsicmp(dot, ext[i])) {
			return true;
		}
	}
	return false;
}

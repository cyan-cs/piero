#include <string.h>

#include "wf_text.h"
#include "../base/wf_macro.h"

typedef struct
{
	uint32_t cp;
	const char *label;
} Label;

/* Bidi controls enable filename spoofing (RLO); zero-width controls hide
 * content. Both become visible text. */
static const Label labels[] = {
        {0x061C, "<ALM>"},
        {0x200B, "<ZWSP>"},
        {0x200C, "<ZWNJ>"},
        {0x200D, "<ZWJ>"},
        {0x200E, "<LRM>"},
        {0x200F, "<RLM>"},
        {0x2028, "<LS>"},
        {0x2029, "<PS>"},
        {0x202A, "<LRE>"},
        {0x202B, "<RLE>"},
        {0x202C, "<PDF>"},
        {0x202D, "<LRO>"},
        {0x202E, "<RLO>"},
        {0x2060, "<WJ>"},
        {0x2066, "<LRI>"},
        {0x2067, "<RLI>"},
        {0x2068, "<FSI>"},
        {0x2069, "<PDI>"},
        {0xFEFF, "<BOM>"},
};

static WfStatus emit_scalar(uint32_t cp, WfBuf *out)
{
	static const char hex[] = "0123456789ABCDEF";
	char tmp[4];
	size_t i;

	if ('\t' == cp) {
		return wf_buf_append(out, "    ", 4);
	}
	if (cp < 0x20) {
		tmp[0] = '^';
		tmp[1] = (char)(cp + 0x40); /* ESC -> "^[" */
		return wf_buf_append(out, tmp, 2);
	}
	if (0x7F == cp) {
		return wf_buf_puts(out, "^?");
	}
	if (cp >= 0x80 && cp <= 0x9F) {
		char c1[] = {'<', 'U', '+', '0', '0', hex[cp >> 4],
		        hex[cp & 15], '>'};

		return wf_buf_append(out, c1, sizeof c1);
	}
	for (i = 0; i < WF_ARRAY_LEN(labels); i++) {
		if (labels[i].cp == cp) {
			return wf_buf_puts(out, labels[i].label);
		}
	}

	return wf_buf_append(out, tmp, wf_utf8_encode(cp, tmp));
}

size_t wf_sanitized_cells(uint32_t cp)
{
	if ('\t' == cp) {
		return 4;
	}
	if (cp < 0x20 || 0x7F == cp) {
		return 2; /* "^A", "^?" */
	}
	if (cp >= 0x80 && cp <= 0x9F) {
		return 8; /* "<U+0085>" */
	}
	for (size_t i = 0; i < WF_ARRAY_LEN(labels); i++) {
		if (labels[i].cp == cp) {
			return strlen(labels[i].label);
		}
	}
	return (size_t)wf_cp_width(cp);
}

WfStatus wf_sanitize_utf8(const char *s, size_t n, WfBuf *out)
{
	if (!out || (n && !s)) {
		return WF_E_INVAL;
	}
	while (n > 0) {
		uint32_t cp;
		size_t k = wf_utf8_decode(s, n, &cp);
		WfStatus st = emit_scalar(cp, out);

		if (WF_OK != st) {
			return st;
		}
		if (0 == k) {
			return WF_E_INVAL; /* cannot happen for n > 0; never
			                      spin */
		}
		s += k;
		n -= k;
	}
	return WF_OK;
}

WfStatus wf_sanitize_utf16(const wchar_t *s, size_t n, WfBuf *out)
{
	size_t i = 0;

	if (!out || (n && !s)) {
		return WF_E_INVAL;
	}
	while (i < n) {
		WfStatus st = emit_scalar(wf_utf16_next(s, n, &i), out);

		if (WF_OK != st) {
			return st;
		}
	}
	return WF_OK;
}

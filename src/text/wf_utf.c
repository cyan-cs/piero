#include "wf_text.h"

size_t wf_utf8_prefix(const char *s, size_t n, size_t budget)
{
	size_t at = 0;
	while (at < n && at < budget) {
		uint32_t cp;
		size_t k = wf_utf8_decode(s + at, n - at, &cp);
		if (!k || k > budget - at) {
			break;
		}
		at += k;
	}
	return at;
}

_Static_assert(sizeof(wchar_t) == 2, "wfile assumes UTF-16 wchar_t (Windows)");

size_t wf_utf8_decode(const char *s, size_t n, uint32_t *cp)
{
	const unsigned char *p = (const unsigned char *)s;
	uint32_t c;
	uint32_t min;
	size_t need, i;

	if (!cp) {
		return 0;
	}
	if (!s || 0 == n) {
		*cp = WF_CP_REPLACEMENT;
		return 0;
	}
	c = p[0];
	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	if (c >= 0xC2 && c <= 0xDF) {
		need = 1;
		c &= 0x1F;
		min = 0x80;
	} else if (c >= 0xE0 && c <= 0xEF) {
		need = 2;
		c &= 0x0F;
		min = 0x800;
	} else if (c >= 0xF0 && c <= 0xF4) {
		need = 3;
		c &= 0x07;
		min = 0x10000;
	} else {
		goto bad;
	}

	if (n < need + 1) {
		goto bad;
	}
	for (i = 1; i <= need; i++) {
		if (0x80 != (p[i] & 0xC0)) {
			goto bad;
		}
		c = (c << 6) | (p[i] & 0x3F);
	}
	/* overlong, surrogate, or beyond Unicode */
	if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
		goto bad;
	}

	*cp = c;
	return need + 1;
bad:
	*cp = WF_CP_REPLACEMENT;
	return 1;
}

size_t wf_utf8_encode(uint32_t cp, char out[4])
{
	if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
		cp = WF_CP_REPLACEMENT;
	}

	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

uint32_t wf_utf16_next(const wchar_t *s, size_t n, size_t *i)
{
	uint32_t u;

	if (!s || !i || *i >= n) {
		return WF_CP_REPLACEMENT;
	}
	u = (uint16_t)s[(*i)++];
	if (u >= 0xD800 && u <= 0xDBFF) {
		if (*i < n) {
			uint32_t lo = (uint16_t)s[*i];

			if (lo >= 0xDC00 && lo <= 0xDFFF) {
				(*i)++;
				return 0x10000 + ((u - 0xD800) << 10) +
				        (lo - 0xDC00);
			}
		}
		return WF_CP_REPLACEMENT;
	}
	if (u >= 0xDC00 && u <= 0xDFFF) {
		return WF_CP_REPLACEMENT;
	}
	return u;
}

size_t wf_utf16_prev_boundary(const wchar_t *s, size_t pos)
{
	if (!pos) {
		return 0;
	}
	pos--;
	if ((uint16_t)s[pos] >= 0xDC00 && (uint16_t)s[pos] <= 0xDFFF && pos &&
	        (uint16_t)s[pos - 1] >= 0xD800 &&
	        (uint16_t)s[pos - 1] <= 0xDBFF) {
		pos--;
	}
	return pos;
}

size_t wf_utf16_next_boundary(const wchar_t *s, size_t len, size_t pos)
{
	if (pos >= len) {
		return len;
	}
	(void)wf_utf16_next(s, len, &pos);
	return pos;
}

size_t wf_utf16_chunk_end(const wchar_t *w, size_t n, size_t want)
{
	size_t i;

	if (n <= want) {
		return n;
	}
	if (!w || want < 2) {
		return 1; /* contract wants >= 2; still make progress, never
		             read w[-1] */
	}
	for (i = want; i > 0; i--) {
		if (0x1B == w[i]) {
			return i;
		}
	}
	/* no ESC in the window: plain text, only the surrogate rule applies */
	if ((uint16_t)w[want - 1] >= 0xD800 &&
	        (uint16_t)w[want - 1] <= 0xDBFF) {
		return want - 1;
	}
	return want;
}

WfStatus wf_utf8_to_utf16(
        const char *s, size_t n, wchar_t *out, size_t out_cap, size_t *used)
{
	size_t u = 0;

	if (!used) {
		return WF_E_INVAL;
	}
	*used = 0;
	if (n && (!s || !out)) {
		return WF_E_INVAL;
	}
	while (n > 0) {
		uint32_t cp;
		size_t k = wf_utf8_decode(s, n, &cp);
		size_t units;

		if (0 == k) {
			return WF_E_INVAL;
		}
		units = cp >= 0x10000 ? 2 : 1;
		if (out_cap - u < units) {
			return WF_E_RANGE;
		}
		s += k;
		n -= k;
		if (2 == units) {
			cp -= 0x10000;
			out[u++] = (wchar_t)(0xD800 + (cp >> 10));
			out[u++] = (wchar_t)(0xDC00 + (cp & 0x3FF));
		} else {
			out[u++] = (wchar_t)cp;
		}
	}
	*used = u;
	return WF_OK;
}

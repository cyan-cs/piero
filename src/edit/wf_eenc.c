#include <windows.h>

#include "wf_eenc.h"
#include "../base/wf_macro.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Length of the valid scalar at s[0..n), or 0 if it is not valid. */
static size_t strict_scalar(const uint8_t *s, size_t n, uint32_t *cp)
{
	uint8_t c = s[0];

	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	if (c >= 0xC2 && c <= 0xDF && n >= 2 && 0x80 == (s[1] & 0xC0)) {
		*cp = (uint32_t)(c & 0x1F) << 6 | (s[1] & 0x3F);
		return 2;
	}
	if (c >= 0xE0 && c <= 0xEF && n >= 3 && 0x80 == (s[1] & 0xC0) &&
	        0x80 == (s[2] & 0xC0)) {
		uint32_t v = (uint32_t)(c & 0x0F) << 12 |
		        (uint32_t)(s[1] & 0x3F) << 6 | (s[2] & 0x3F);

		if (v < 0x800 || (v >= 0xD800 && v <= 0xDFFF)) {
			return 0;
		}
		*cp = v;
		return 3;
	}
	if (c >= 0xF0 && c <= 0xF4 && n >= 4 && 0x80 == (s[1] & 0xC0) &&
	        0x80 == (s[2] & 0xC0) && 0x80 == (s[3] & 0xC0)) {
		uint32_t v = (uint32_t)(c & 0x07) << 18 |
		        (uint32_t)(s[1] & 0x3F) << 12 |
		        (uint32_t)(s[2] & 0x3F) << 6 | (s[3] & 0x3F);

		if (v < 0x10000 || v > 0x10FFFF) {
			return 0;
		}
		*cp = v;
		return 4;
	}
	return 0;
}

bool wf_utf8_strict(const char *s, size_t n, size_t *bad)
{
	const uint8_t *u = (const uint8_t *)s;
	size_t i = 0;

	while (i < n) {
		uint32_t cp;
		size_t len = strict_scalar(u + i, n - i, &cp);

		if (!len || 0 == cp) {
			if (bad) {
				*bad = i;
			}
			return false;
		}
		i += len;
	}
	return true;
}

static uint16_t unit_at(const uint8_t *b, size_t i, bool big)
{
	return big ? (uint16_t)(b[i] << 8 | b[i + 1])
	           : (uint16_t)(b[i + 1] << 8 | b[i]);
}

static size_t put_utf8(uint32_t cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | cp >> 6);
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | cp >> 12);
		out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | cp >> 18);
	out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
	out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

static WfStatus utf16_to_utf8(const uint8_t *b,
        size_t n,
        bool big,
        char **out,
        size_t *out_n,
        size_t *bad)
{
	size_t units = n / 2, i, len = 0;
	char *buf;

	if (n % 2) {
		if (bad) {
			*bad = n - 1;
		}
		return WF_E_INVAL;
	}
	if (units > (SIZE_MAX - 1) / 3) {
		return WF_E_RANGE;
	}
	buf = malloc(units * 3 + 1);
	if (!buf) {
		return WF_E_NOMEM;
	}
	for (i = 0; i < units; i++) {
		uint32_t cp = unit_at(b, i * 2, big);

		if (cp >= 0xD800 && cp <= 0xDBFF) {
			uint32_t lo;

			if (i + 1 >= units ||
			        (lo = unit_at(b, (i + 1) * 2, big)) < 0xDC00 ||
			        lo > 0xDFFF) {
				goto invalid;
			}
			cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
			i++;
		} else if ((cp >= 0xDC00 && cp <= 0xDFFF) || 0 == cp) {
			goto invalid;
		}
		len += put_utf8(cp, buf + len);
	}
	buf[len] = '\0';
	*out = buf;
	*out_n = len;
	return WF_OK;
invalid:
	if (bad) {
		*bad = i * 2;
	}
	free(buf);
	return WF_E_INVAL;
}

static WfStatus utf8_to_utf16(const char *s,
        size_t n,
        bool big,
        bool bom,
        uint8_t **out,
        size_t *out_n,
        size_t *bad)
{
	const uint8_t *u = (const uint8_t *)s;
	size_t i = 0, at = 0;
	uint8_t *buf;

	if (n > (SIZE_MAX - 4) / 4) {
		return WF_E_RANGE;
	}
	buf = malloc(n * 4 + 4);
	if (!buf) {
		return WF_E_NOMEM;
	}
#define PUT_UNIT(v)                                                            \
	do {                                                                   \
		uint16_t unit_ = (uint16_t)(v);                                \
		buf[at++] = big ? (uint8_t)(unit_ >> 8) : (uint8_t)unit_;      \
		buf[at++] = big ? (uint8_t)unit_ : (uint8_t)(unit_ >> 8);      \
	} while (0)
	if (bom) {
		PUT_UNIT(0xFEFF);
	}
	while (i < n) {
		uint32_t cp;
		size_t len = strict_scalar(u + i, n - i, &cp);

		if (!len) {
			if (bad) {
				*bad = i;
			}
			free(buf);
			return WF_E_INVAL;
		}
		if (cp >= 0x10000) {
			cp -= 0x10000;
			PUT_UNIT(0xD800 + (cp >> 10));
			PUT_UNIT(0xDC00 + (cp & 0x3FF));
		} else {
			PUT_UNIT(cp);
		}
		i += len;
	}
#undef PUT_UNIT
	*out = buf;
	*out_n = at;
	return WF_OK;
}

static WfStatus cp_to_utf8(unsigned cp,
        const uint8_t *b,
        size_t n,
        char **out,
        size_t *out_n,
        size_t *bad)
{
	int units;
	wchar_t *w;
	uint8_t *wb;
	WfStatus st;
	int i;

	if (n > INT_MAX / 4) {
		return WF_E_RANGE;
	}
	units = MultiByteToWideChar(
	        cp, MB_ERR_INVALID_CHARS, (const char *)b, (int)n, NULL, 0);
	if (units <= 0 && n) {
		if (bad) {
			*bad = 0;
		}
		return WF_E_INVAL;
	}
	w = malloc(((size_t)units + 1) * sizeof *w);
	wb = malloc(((size_t)units + 1) * 2);
	if (!w || !wb) {
		free(w);
		free(wb);
		return WF_E_NOMEM;
	}
	if (n &&
	        MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, (const char *)b,
	                (int)n, w, units) != units) {
		free(w);
		free(wb);
		return WF_E_INVAL;
	}
	for (i = 0; i < units;
	        i++) { /* little-endian bytes for the shared path */
		wb[2 * i] = (uint8_t)(w[i] & 0xFF);
		wb[2 * i + 1] = (uint8_t)(w[i] >> 8);
	}
	st = utf16_to_utf8(wb, (size_t)units * 2, false, out, out_n, bad);
	if (WF_E_INVAL == st && bad) {
		*bad = 0; /* the byte offset in the source is not recoverable */
	}
	free(w);
	free(wb);
	return st;
}

static WfStatus cp_from_utf8(unsigned cp,
        const char *s,
        size_t n,
        uint8_t **out,
        size_t *out_n,
        size_t *bad)
{
	uint8_t *u16;
	size_t u16_n, units, i;
	wchar_t *w;
	int need;
	BOOL used_default = FALSE;
	uint8_t *buf;
	WfStatus st = utf8_to_utf16(s, n, false, false, &u16, &u16_n, bad);

	if (WF_OK != st) {
		return st;
	}
	units = u16_n / 2;
	if (units > INT_MAX / 4) {
		free(u16);
		return WF_E_RANGE;
	}
	w = malloc((units + 1) * sizeof *w);
	if (!w) {
		free(u16);
		return WF_E_NOMEM;
	}
	for (i = 0; i < units; i++) {
		w[i] = (wchar_t)(u16[2 * i] | u16[2 * i + 1] << 8);
	}
	free(u16);
	need = units ? WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, w,
	                       (int)units, NULL, 0, NULL, &used_default)
	             : 0;
	if ((units && need <= 0) || used_default) {
		/* find the first scalar that has no mapping */
		size_t at = 0;

		if (bad) {
			*bad = 0;
			while (at < n) {
				uint32_t c;
				size_t len = strict_scalar(
				        (const uint8_t *)s + at, n - at, &c);
				wchar_t pair[2];
				int wn = c >= 0x10000 ? 2 : 1;
				BOOL def = FALSE;
				char tmp[8];

				if (c >= 0x10000) {
					pair[0] = (wchar_t)(0xD800 +
					        ((c - 0x10000) >> 10));
					pair[1] = (wchar_t)(0xDC00 +
					        ((c - 0x10000) & 0x3FF));
				} else {
					pair[0] = (wchar_t)c;
				}
				if (WideCharToMultiByte(cp,
				            WC_NO_BEST_FIT_CHARS, pair, wn, tmp,
				            sizeof tmp, NULL, &def) <= 0 ||
				        def) {
					*bad = at;
					break;
				}
				at += len ? len : 1;
			}
		}
		free(w);
		return WF_E_INVAL;
	}
	buf = malloc((size_t)need + 1);
	if (!buf) {
		free(w);
		return WF_E_NOMEM;
	}
	if (need &&
	        WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, w, (int)units,
	                (char *)buf, need, NULL, &used_default) != need) {
		free(w);
		free(buf);
		return WF_E_OS;
	}
	free(w);
	*out = buf;
	*out_n = (size_t)need;
	return WF_OK;
}

static bool has_bom8(const uint8_t *b, size_t n)
{
	return n >= 3 && 0xEF == b[0] && 0xBB == b[1] && 0xBF == b[2];
}

WfStatus wf_enc_decode(const uint8_t *bytes,
        size_t n,
        WfEncoding enc,
        char **utf8,
        size_t *utf8_n,
        size_t *bad)
{
	WfStatus st;

	if (!utf8 || !utf8_n || (n && !bytes)) {
		return WF_E_INVAL;
	}
	*utf8 = NULL;
	*utf8_n = 0;
	if (!bytes) {
		bytes = (const uint8_t *)"";
	}
	switch (enc.kind) {
	case WF_ENC_UTF8:
	case WF_ENC_UTF8_BOM: {
		size_t skip = WF_ENC_UTF8_BOM == enc.kind ? 3u : 0u;
		char *copy;

		if (n == SIZE_MAX) {
			return WF_E_RANGE;
		}
		if (skip && !has_bom8(bytes, n)) {
			return WF_E_INVAL;
		}
		if (!wf_utf8_strict(
		            (const char *)bytes + skip, n - skip, bad)) {
			if (bad) {
				*bad += skip;
			}
			return WF_E_INVAL;
		}
		copy = malloc(n - skip + 1);
		if (!copy) {
			return WF_E_NOMEM;
		}
		memcpy(copy, bytes + skip, n - skip);
		copy[n - skip] = '\0';
		*utf8 = copy;
		*utf8_n = n - skip;
		return WF_OK;
	}
	case WF_ENC_UTF16LE:
	case WF_ENC_UTF16BE: {
		bool big = WF_ENC_UTF16BE == enc.kind;

		if (n < 2 || 0xFEFF != unit_at(bytes, 0, big)) {
			if (bad) {
				*bad = 0;
			}
			return WF_E_INVAL;
		}
		st = utf16_to_utf8(bytes + 2, n - 2, big, utf8, utf8_n, bad);
		if (WF_E_INVAL == st && bad) {
			*bad += 2;
		}
		return st;
	}
	case WF_ENC_UTF16LE_RAW:
	case WF_ENC_UTF16BE_RAW:
		return utf16_to_utf8(bytes, n, WF_ENC_UTF16BE_RAW == enc.kind,
		        utf8, utf8_n, bad);
	case WF_ENC_CODEPAGE: {
		uint8_t *again;
		size_t again_n;

		if (!enc.codepage) {
			return WF_E_INVAL;
		}
		st = cp_to_utf8(enc.codepage, bytes, n, utf8, utf8_n, bad);
		if (WF_OK != st) {
			return st;
		}
		/* it opens only if saving it unchanged gives the same bytes */
		st = cp_from_utf8(
		        enc.codepage, *utf8, *utf8_n, &again, &again_n, bad);
		if (WF_OK != st || again_n != n || memcmp(again, bytes, n)) {
			if (WF_OK == st) {
				free(again);
			}
			free(*utf8);
			*utf8 = NULL;
			*utf8_n = 0;
			if (WF_OK != st) {
				return st;
			}
			if (bad) {
				*bad = 0;
			}
			return WF_E_INVAL;
		}
		free(again);
		return WF_OK;
	}
	}
	return WF_E_INVAL;
}

WfStatus wf_enc_encode(const char *utf8,
        size_t n,
        WfEncoding enc,
        uint8_t **bytes,
        size_t *bytes_n,
        size_t *bad)
{
	uint8_t *buf;

	if (!bytes || !bytes_n || (n && !utf8)) {
		return WF_E_INVAL;
	}
	*bytes = NULL;
	*bytes_n = 0;
	if (!utf8) {
		utf8 = "";
	}
	switch (enc.kind) {
	case WF_ENC_UTF8:
	case WF_ENC_UTF8_BOM: {
		size_t skip = WF_ENC_UTF8_BOM == enc.kind ? 3u : 0u;

		if (n > SIZE_MAX - skip - 1) {
			return WF_E_RANGE;
		}
		if (!wf_utf8_strict(utf8, n, bad)) {
			return WF_E_INVAL;
		}
		buf = malloc(n + skip + 1);
		if (!buf) {
			return WF_E_NOMEM;
		}
		if (skip) {
			memcpy(buf, "\xEF\xBB\xBF", 3);
		}
		memcpy(buf + skip, utf8, n);
		*bytes = buf;
		*bytes_n = n + skip;
		return WF_OK;
	}
	case WF_ENC_UTF16LE:
	case WF_ENC_UTF16BE:
		return utf8_to_utf16(utf8, n, WF_ENC_UTF16BE == enc.kind, true,
		        bytes, bytes_n, bad);
	case WF_ENC_UTF16LE_RAW:
	case WF_ENC_UTF16BE_RAW:
		return utf8_to_utf16(utf8, n, WF_ENC_UTF16BE_RAW == enc.kind,
		        false, bytes, bytes_n, bad);
	case WF_ENC_CODEPAGE:
		if (!enc.codepage) {
			return WF_E_INVAL;
		}
		return cp_from_utf8(enc.codepage, utf8, n, bytes, bytes_n, bad);
	}
	return WF_E_INVAL;
}

void wf_enc_name(WfEncoding enc, char *out, size_t cap)
{
	switch (enc.kind) {
	case WF_ENC_UTF8:     snprintf(out, cap, "UTF-8"); break;
	case WF_ENC_UTF8_BOM: snprintf(out, cap, "UTF-8 BOM"); break;
	case WF_ENC_UTF16LE:  snprintf(out, cap, "UTF-16 LE"); break;
	case WF_ENC_UTF16BE:  snprintf(out, cap, "UTF-16 BE"); break;
	case WF_ENC_UTF16LE_RAW:
		snprintf(out, cap, "UTF-16 LE (no BOM)");
		break;
	case WF_ENC_UTF16BE_RAW:
		snprintf(out, cap, "UTF-16 BE (no BOM)");
		break;
	case WF_ENC_CODEPAGE: snprintf(out, cap, "CP%u", enc.codepage); break;
	}
}

static const struct
{
	const char *magic;
	size_t len;
	const char *what;
} MAGIC[] = {
        {"\x89PNG", 4, "PNG image"},
        {"\xFF\xD8\xFF", 3, "JPEG image"},
        {"GIF8", 4, "GIF image"},
        {"%PDF", 4, "PDF document"},
        {"PK\x03\x04", 4, "ZIP archive"},
        {"7z\xBC\xAF", 4, "7-Zip archive"},
        {"Rar!", 4, "RAR archive"},
        {"\x1F\x8B", 2, "gzip data"},
        {"\x7F"
         "ELF",
                4, "executable"},
        {"\0asm", 4, "WebAssembly module"},
        {"SQLite format 3", 15, "SQLite database"},
        {"\xD0\xCF\x11\xE0", 4, "OLE document"},
        {"MZ\x90\0", 4, "executable"},
        {"\0\0\x01\0", 4, "icon"},
        {"II*\0", 4, "TIFF image"},
        {"MM\0*", 4, "TIFF image"},
        {"OggS", 4, "audio/video"},
        {"fLaC", 4, "audio"},
        {"RIFF", 4, "media container"},
        {"\0\0\xFE\xFF", 4, "UTF-32 text (not supported)"},
        {"\xFF\xFE\0\0", 4, "UTF-32 text (not supported)"},
};

static WfDetect result(WfDetectKind k,
        WfEncKind enc,
        unsigned cp,
        bool candidate_ok,
        const char *reason)
{
	WfDetect d;

	d.kind = k;
	d.enc.kind = enc;
	d.enc.codepage = cp;
	d.candidate_ok = candidate_ok;
	d.reason = reason;
	return d;
}

WfDetect wf_enc_detect(
        const uint8_t *bytes, size_t n, unsigned fallback_codepage)
{
	size_t i, nul_even = 0, nul_odd = 0, controls = 0, pairs;

	if (0 == n) {
		return result(WF_DETECT_TEXT, WF_ENC_UTF8, 0, true, NULL);
	}
	for (i = 0; i < WF_ARRAY_LEN(MAGIC); i++) {
		if (n >= MAGIC[i].len &&
		        !memcmp(bytes, MAGIC[i].magic, MAGIC[i].len)) {
			return result(WF_DETECT_BINARY, WF_ENC_UTF8, 0, false,
			        MAGIC[i].what);
		}
	}
	if (has_bom8(bytes, n)) {
		return result(WF_DETECT_TEXT, WF_ENC_UTF8_BOM, 0, true, NULL);
	}
	if (n >= 2 && 0xFF == bytes[0] && 0xFE == bytes[1]) {
		return result(WF_DETECT_TEXT, WF_ENC_UTF16LE, 0, true, NULL);
	}
	if (n >= 2 && 0xFE == bytes[0] && 0xFF == bytes[1]) {
		return result(WF_DETECT_TEXT, WF_ENC_UTF16BE, 0, true, NULL);
	}

	for (i = 0; i < n; i++) {
		uint8_t c = bytes[i];

		if (0 == c) {
			if (i % 2) {
				nul_odd++;
			} else {
				nul_even++;
			}
		} else if ((c < 0x20 && '\t' != c && '\n' != c && '\r' != c &&
		                   '\f' != c && 0x1B != c) ||
		        0x7F == c) {
			controls++;
		}
	}
	if (nul_even + nul_odd) {
		pairs = n / 2;
		if (n >= 4 && nul_odd * 10 >= pairs * 3 &&
		        nul_even * 50 < pairs) {
			return result(WF_DETECT_AMBIGUOUS, WF_ENC_UTF16LE_RAW,
			        0, true, "UTF-16 without a byte order mark");
		}
		if (n >= 4 && nul_even * 10 >= pairs * 3 &&
		        nul_odd * 50 < pairs) {
			return result(WF_DETECT_AMBIGUOUS, WF_ENC_UTF16BE_RAW,
			        0, true, "UTF-16 without a byte order mark");
		}
		return result(WF_DETECT_BINARY, WF_ENC_UTF8, 0, false,
		        "contains NUL bytes");
	}
	if (controls && (controls >= 16 || controls * 100 > n)) {
		return result(WF_DETECT_BINARY, WF_ENC_UTF8, 0, false,
		        "contains many control characters");
	}
	if (wf_utf8_strict((const char *)bytes, n, NULL)) {
		return result(WF_DETECT_TEXT, WF_ENC_UTF8, 0, true, NULL);
	}
	if (fallback_codepage) {
		return result(WF_DETECT_AMBIGUOUS, WF_ENC_CODEPAGE,
		        fallback_codepage, true, "not valid UTF-8");
	}
	return result(WF_DETECT_AMBIGUOUS, WF_ENC_UTF8, 0, false,
	        "not valid UTF-8 and no other encoding is allowed");
}

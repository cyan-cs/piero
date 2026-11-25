#include "wf_session.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../base/wf_buf.h"

void wf_session_free(WfSession *s)
{
	if (!s) {
		return;
	}
	for (size_t i = 0; i < s->n_selected; i++) {
		free(s->selected[i]);
	}
	free(s->selected);
	free(s->dir);
	free(s->entry);
	free(s->edit.path);
	memset(s, 0, sizeof *s);
}

bool wf_session_path_ok(const wchar_t *p)
{
	size_t n = p ? wcslen(p) : 0, start;

	if (n < 3 || n > WF_SESSION_MAX_PATH) {
		return false;
	}
	if (p[0] < 0x80 && iswalpha(p[0]) && L':' == p[1] && L'\\' == p[2]) {
		start = 3;
	} else if (L'\\' == p[0] && L'\\' == p[1] && L'?' != p[2] &&
	        L'.' != p[2] && L'\\' != p[2]) {
		const wchar_t *share = wcschr(p + 2, L'\\');

		if (!share || !share[1] || L'\\' == share[1]) {
			return false;
		}
		start = 2;
	} else {
		return false;
	}
	/* no "." or ".." component, no control character, no '/' */
	for (size_t i = start; i <= n;) {
		size_t j = i;

		while (j < n && L'\\' != p[j]) {
			if (p[j] < 0x20 || L'/' == p[j]) {
				return false;
			}
			j++;
		}
		if ((1 == j - i && L'.' == p[i]) ||
		        (2 == j - i && L'.' == p[i] && L'.' == p[i + 1])) {
			return false;
		}
		i = j + 1;
	}
	return true;
}

/* One name inside a folder: not empty, no separator or control character. */
static bool name_ok(const wchar_t *s)
{
	size_t n = s ? wcslen(s) : 0;

	if (!n || n > WF_SESSION_MAX_NAME) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (s[i] < 0x20 || L'\\' == s[i] || L'/' == s[i] ||
		        L':' == s[i]) {
			return false;
		}
	}
	return true;
}

/* Appends UTF-8 with line-safe escapes; invalid UTF-16 is distinct from OOM. */
static WfStatus put_string(WfBuf *b, const wchar_t *w)
{
	char utf8[WF_SESSION_MAX_PATH * 3 + 4];
	int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, utf8,
	        (int)sizeof utf8, NULL, NULL);

	if (n <= 0) {
		return WF_E_INVAL;
	}
	for (int i = 0; i < n - 1; i++) {
		unsigned char c = (unsigned char)utf8[i];
		char esc[8];

		if ('\\' == c) {
			esc[0] = '\\';
			esc[1] = '\\';
			esc[2] = '\0';
		} else if (c < 0x20 || 0x7F == c) {
			snprintf(esc, sizeof esc, "\\x%02X", c);
		} else {
			esc[0] = (char)c;
			esc[1] = '\0';
		}
		WfStatus st = wf_buf_puts(b, esc);

		if (WF_OK != st) {
			return st;
		}
	}
	return WF_OK;
}

static int hex_value(char c)
{
	return c >= '0' && c <= '9'    ? c - '0'
	        : c >= 'a' && c <= 'f' ? c - 'a' + 10
	        : c >= 'A' && c <= 'F' ? c - 'A' + 10
	                               : -1;
}

/* Decodes a bounded escaped string. On failure *out is NULL. */
static WfStatus get_string(
        const char *v, size_t n, size_t max_units, wchar_t **out)
{
	char *raw = malloc(n + 1);
	size_t m = 0;
	wchar_t *w = NULL;
	int units;

	*out = NULL;
	if (!raw) {
		return WF_E_NOMEM;
	}
	for (size_t i = 0; i < n; i++) {
		if ('\\' != v[i]) {
			raw[m++] = v[i];
		} else if (i + 1 < n && '\\' == v[i + 1]) {
			raw[m++] = '\\';
			i++;
		} else if (i + 3 < n && 'x' == v[i + 1] &&
		        hex_value(v[i + 2]) >= 0 && hex_value(v[i + 3]) >= 0) {
			raw[m++] = (char)(hex_value(v[i + 2]) * 16 +
			        hex_value(v[i + 3]));
			i += 3;
		} else {
			free(raw);
			return WF_E_CORRUPT;
		}
	}
	/* a NUL that an escape wrote is as much a NUL as a raw one: every check
	 * after this reads the string up to its first NUL, so what follows one would
	 * never be looked at */
	if (memchr(raw, 0, m)) {
		free(raw);
		return WF_E_CORRUPT;
	}
	units = MultiByteToWideChar(
	        CP_UTF8, MB_ERR_INVALID_CHARS, raw, (int)m, NULL, 0);
	if (units <= 0 || (size_t)units > max_units) {
		free(raw);
		return WF_E_CORRUPT;
	}
	w = malloc(((size_t)units + 1) * sizeof *w);
	if (!w) {
		free(raw);
		return WF_E_NOMEM;
	}
	if (units !=
	        MultiByteToWideChar(
	                CP_UTF8, MB_ERR_INVALID_CHARS, raw, (int)m, w, units)) {
		free(w);
		free(raw);
		return WF_E_CORRUPT;
	}
	w[units] = L'\0';
	free(raw);
	*out = w;
	return WF_OK;
}

static const char *pane_word(WfSessionPane p)
{
	return WF_SESSION_PANE_PREVIEW == p ? "pane"
	        : WF_SESSION_PANE_FULL == p ? "full"
	                                    : "list";
}

/* Rolls back a partial line on failure, preserving the error cause. */
static WfStatus put_named(WfBuf *b, const char *key, const wchar_t *name)
{
	size_t keep = b->len;
	WfStatus st = wf_buf_puts(b, key);

	if (WF_OK == st) {
		st = wf_buf_putc(b, ' ');
	}
	if (WF_OK == st) {
		st = put_string(b, name);
	}
	if (WF_OK == st) {
		st = wf_buf_putc(b, '\n');
	}
	if (WF_OK != st && b->data) {
		b->len = keep;
		b->data[keep] = '\0';
	}
	return st;
}

WfStatus wf_session_encode(const WfSession *s, char **text, size_t *n)
{
	WfBuf b;
	char num[96];
	WfStatus st;

	*text = NULL;
	*n = 0;
	wf_buf_init(&b);
	if (!s->dir || !wf_session_path_ok(s->dir)) {
		return WF_E_INVAL;
	}
	st = put_named(&b, "dir", s->dir);
	if (WF_OK != st) {
		goto done;
	}
	if (s->entry && name_ok(s->entry)) {
		st = put_named(&b, "entry", s->entry);
		if (WF_E_INVAL == st) {
			st = WF_OK; /* an unencodable optional name is omitted */
		} else if (WF_OK != st) {
			goto done;
		}
	}
	snprintf(num, sizeof num, "cursor %zu %zu\npane %s\npv %zu %zu\n",
	        s->cursor, s->top, pane_word(s->pane), s->pv_top, s->pv_left);
	st = wf_buf_puts(&b, num);
	if (WF_OK == st && s->has_edit && s->edit.path &&
	        wf_session_path_ok(s->edit.path)) {
		snprintf(num, sizeof num, "edit %zu %zu %zu %zu %d ",
		        s->edit.line, s->edit.col, s->edit.top, s->edit.left,
		        s->edit.modified ? 1 : 0);
		size_t keep = b.len;

		st = wf_buf_puts(&b, num);
		if (WF_OK == st) {
			st = put_string(&b, s->edit.path);
		}
		if (WF_OK == st) {
			st = wf_buf_putc(&b, '\n');
		}
		if (WF_OK != st) {
			b.len = keep;
			b.data[keep] = '\0';
			if (WF_E_INVAL == st) {
				st = WF_OK; /* omit only an unencodable editor path */
			}
		}
	}
	/* Invalid names are omitted; resource failures reject the snapshot. */
	for (size_t i = 0;
	        WF_OK == st && i < s->n_selected && i < WF_SESSION_MAX_SELECTED;
	        i++) {
		if (name_ok(s->selected[i])) {
			st = put_named(&b, "sel", s->selected[i]);
			if (WF_E_INVAL == st) {
				st = WF_OK;
			}
		}
	}
done:
	if (WF_OK == st && b.len > WF_SESSION_MAX_BYTES) {
		st = WF_E_RANGE;
	}
	if (WF_OK != st) {
		wf_buf_free(&b);
		return st;
	}
	*text = b.data;
	*n = b.len;
	return WF_OK;
}

/* The unsigned decimal at `*p` (up to `end`), then `*p` is past it. */
static bool take_number(const char **p, const char *end, size_t *out)
{
	const char *q = *p;
	unsigned long long v = 0;

	if (q >= end || !isdigit((unsigned char)*q)) {
		return false;
	}
	while (q < end && isdigit((unsigned char)*q)) {
		v = v * 10 + (unsigned)(*q - '0');
		if (v > 1000000000ull) { /* far more rows than any folder holds */
			return false;
		}
		q++;
	}
	*out = (size_t)v;
	*p = q;
	return true;
}

/* Two numbers separated by one space and nothing after them. */
static bool take_pair(const char *v, const char *end, size_t *a, size_t *b)
{
	if (!take_number(&v, end, a) || v >= end || ' ' != *v) {
		return false;
	}
	v++;
	return take_number(&v, end, b) && v == end;
}

static WfStatus add_selected(WfSession *s, wchar_t *name)
{
	if (s->n_selected >= WF_SESSION_MAX_SELECTED) {
		return WF_E_CORRUPT;
	}
	if (!s->selected) {
		/* the limit is small: one allocation holds every name that may come */
		s->selected =
		        malloc(WF_SESSION_MAX_SELECTED * sizeof *s->selected);
		if (!s->selected) {
			return WF_E_NOMEM;
		}
	}
	s->selected[s->n_selected++] = name;
	return WF_OK;
}

/* "<line> <col> <top> <left> <0|1> <escaped path>" */
static WfStatus take_edit(WfSession *s, const char *v, const char *end)
{
	size_t n[5];
	wchar_t *w;
	WfStatus st;

	for (int i = 0; i < 5; i++) {
		if (!take_number(&v, end, &n[i]) || v >= end || ' ' != *v) {
			return WF_E_CORRUPT;
		}
		v++;
	}
	if (n[4] > 1) {
		return WF_E_CORRUPT; /* modified is 0 or 1 */
	}
	st = get_string(v, (size_t)(end - v), WF_SESSION_MAX_PATH, &w);
	if (WF_OK != st) {
		return st;
	}
	if (!wf_session_path_ok(w)) {
		free(w);
		return WF_E_CORRUPT;
	}
	s->has_edit = true;
	s->edit.path = w;
	s->edit.line = n[0];
	s->edit.col = n[1];
	s->edit.top = n[2];
	s->edit.left = n[3];
	s->edit.modified = 1 == n[4];
	return WF_OK;
}

enum
{
	SEEN_DIR = 1u << 0,
	SEEN_ENTRY = 1u << 1,
	SEEN_CURSOR = 1u << 2,
	SEEN_PANE = 1u << 3,
	SEEN_PV = 1u << 4,
	SEEN_EDIT = 1u << 5
};

static bool word_is(const char *key, size_t klen, const char *word)
{
	return strlen(word) == klen && !memcmp(key, word, klen);
}

/* One record. `key`/`klen` is the word, `v`/`vlen` what follows the space. */
static WfStatus take_record(WfSession *s,
        unsigned *seen,
        const char *key,
        size_t klen,
        const char *v,
        size_t vlen)
{
	const char *end = v + vlen;
	unsigned bit = word_is(key, klen, "dir") ? SEEN_DIR
	        : word_is(key, klen, "entry")    ? SEEN_ENTRY
	        : word_is(key, klen, "cursor")   ? SEEN_CURSOR
	        : word_is(key, klen, "pane")     ? SEEN_PANE
	        : word_is(key, klen, "pv")       ? SEEN_PV
	        : word_is(key, klen, "edit")     ? SEEN_EDIT
	                                         : 0;
	wchar_t *w;
	WfStatus st;

	if (*seen & bit) {
		return WF_E_CORRUPT; /* a record that may be there once, twice */
	}
	*seen |= bit;
	if (SEEN_DIR == bit) {
		st = get_string(v, vlen, WF_SESSION_MAX_PATH, &w);
		if (WF_OK != st) {
			return st;
		}
		if (!wf_session_path_ok(w)) {
			free(w);
			return WF_E_CORRUPT;
		}
		s->dir = w;
	} else if (SEEN_ENTRY == bit) {
		st = get_string(v, vlen, WF_SESSION_MAX_NAME, &w);
		if (WF_OK != st) {
			return st;
		}
		if (!name_ok(w)) {
			free(w);
			return WF_E_CORRUPT;
		}
		s->entry = w;
	} else if (SEEN_CURSOR == bit) {
		return take_pair(v, end, &s->cursor, &s->top) ? WF_OK
		                                              : WF_E_CORRUPT;
	} else if (SEEN_PV == bit) {
		return take_pair(v, end, &s->pv_top, &s->pv_left)
		        ? WF_OK
		        : WF_E_CORRUPT;
	} else if (SEEN_EDIT == bit) {
		return take_edit(s, v, end);
	} else if (SEEN_PANE == bit) {
		if (word_is(v, vlen, "list")) {
			s->pane = WF_SESSION_PANE_LIST;
		} else if (word_is(v, vlen, "pane")) {
			s->pane = WF_SESSION_PANE_PREVIEW;
		} else if (word_is(v, vlen, "full")) {
			s->pane = WF_SESSION_PANE_FULL;
		} else {
			return WF_E_CORRUPT;
		}
	} else if (word_is(key, klen, "sel")) {
		st = get_string(v, vlen, WF_SESSION_MAX_NAME, &w);
		if (WF_OK != st) {
			return st;
		}
		st = name_ok(w) ? add_selected(s, w) : WF_E_CORRUPT;
		if (WF_OK != st) {
			free(w);
			return st;
		}
	} else {
		return WF_E_CORRUPT; /* a record this reader does not know */
	}
	return WF_OK;
}

WfStatus wf_session_decode(const char *text, size_t n, WfSession *out)
{
	const char *p, *end;
	unsigned seen = 0;
	WfStatus st = WF_OK;

	memset(out, 0, sizeof *out);
	if (!text || n > WF_SESSION_MAX_BYTES || (n && '\n' != text[n - 1])) {
		return WF_E_CORRUPT;
	}
	/* the encoder escapes every control character: a raw NUL is damage (or a
	 * file that is not ours), never part of a name */
	if (n && memchr(text, 0, n)) {
		return WF_E_CORRUPT;
	}
	p = text;
	end = text + n;
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		const char *sp = memchr(p, ' ', (size_t)(nl - p));

		if (!sp) {
			st = WF_E_CORRUPT;
			break;
		}
		st = take_record(out, &seen, p, (size_t)(sp - p), sp + 1,
		        (size_t)(nl - sp - 1));
		if (WF_OK != st) {
			break;
		}
		p = nl + 1;
	}
	if (WF_OK == st && !(seen & SEEN_DIR)) {
		st = WF_E_CORRUPT; /* no folder: nothing to go back to */
	}
	if (WF_OK != st) {
		wf_session_free(out);
	}
	return st;
}

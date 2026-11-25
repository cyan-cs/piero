#include "wf_stroke.h"

#include <string.h>

#include "../text/wf_text.h"
#include "../base/wf_macro.h"

static const char *const NAMES[] = {"", "Up", "Down", "Left", "Right", "Enter",
        "Esc", "Backspace", "Tab", "Delete", "Home", "End", "PageUp",
        "PageDown", "Insert", "F2", "F3"};
_Static_assert(WF_ARRAY_LEN(NAMES) == WF_NK_COUNT,
        "a named key has no name (or the other way round)");

/* What the console layer reports, and what each report means. */
static const struct
{
	WfKey key;
	uint8_t nk, mods;
} KEYS[] = {{WF_KEY_UP, WF_NK_UP, 0}, {WF_KEY_DOWN, WF_NK_DOWN, 0},
        {WF_KEY_LEFT, WF_NK_LEFT, 0}, {WF_KEY_RIGHT, WF_NK_RIGHT, 0},
        {WF_KEY_ENTER, WF_NK_ENTER, 0}, {WF_KEY_ESC, WF_NK_ESC, 0},
        {WF_KEY_BACKSPACE, WF_NK_BACKSPACE, 0}, {WF_KEY_TAB, WF_NK_TAB, 0},
        {WF_KEY_F2, WF_NK_F2, 0}, {WF_KEY_F3, WF_NK_F3, 0},
        {WF_KEY_DELETE, WF_NK_DELETE, 0}, {WF_KEY_HOME, WF_NK_HOME, 0},
        {WF_KEY_END, WF_NK_END, 0}, {WF_KEY_PGUP, WF_NK_PGUP, 0},
        {WF_KEY_PGDN, WF_NK_PGDN, 0}, {WF_KEY_INSERT, WF_NK_INSERT, 0},
        {WF_KEY_CTRL_UP, WF_NK_UP, WF_MOD_CTRL},
        {WF_KEY_CTRL_DOWN, WF_NK_DOWN, WF_MOD_CTRL},
        {WF_KEY_CTRL_HOME, WF_NK_HOME, WF_MOD_CTRL},
        {WF_KEY_CTRL_END, WF_NK_END, WF_MOD_CTRL},
        {WF_KEY_CTRL_LEFT, WF_NK_LEFT, WF_MOD_CTRL},
        {WF_KEY_CTRL_RIGHT, WF_NK_RIGHT, WF_MOD_CTRL},
        {WF_KEY_CTRL_BACKSPACE, WF_NK_BACKSPACE, WF_MOD_CTRL},
        {WF_KEY_CTRL_DELETE, WF_NK_DELETE, WF_MOD_CTRL},
        {WF_KEY_CTRL_INSERT, WF_NK_INSERT, WF_MOD_CTRL},
        {WF_KEY_SHIFT_TAB, WF_NK_TAB, WF_MOD_SHIFT},
        {WF_KEY_SHIFT_UP, WF_NK_UP, WF_MOD_SHIFT},
        {WF_KEY_SHIFT_DOWN, WF_NK_DOWN, WF_MOD_SHIFT},
        {WF_KEY_SHIFT_LEFT, WF_NK_LEFT, WF_MOD_SHIFT},
        {WF_KEY_SHIFT_RIGHT, WF_NK_RIGHT, WF_MOD_SHIFT},
        {WF_KEY_SHIFT_HOME, WF_NK_HOME, WF_MOD_SHIFT},
        {WF_KEY_SHIFT_END, WF_NK_END, WF_MOD_SHIFT},
        {WF_KEY_SHIFT_PGUP, WF_NK_PGUP, WF_MOD_SHIFT},
        {WF_KEY_SHIFT_PGDN, WF_NK_PGDN, WF_MOD_SHIFT},
        {WF_KEY_SHIFT_INSERT, WF_NK_INSERT, WF_MOD_SHIFT},
        {WF_KEY_SHIFT_DELETE, WF_NK_DELETE, WF_MOD_SHIFT},
        {WF_KEY_CTRL_SHIFT_LEFT, WF_NK_LEFT, WF_MOD_CTRL | WF_MOD_SHIFT},
        {WF_KEY_CTRL_SHIFT_RIGHT, WF_NK_RIGHT, WF_MOD_CTRL | WF_MOD_SHIFT},
        {WF_KEY_CTRL_SHIFT_HOME, WF_NK_HOME, WF_MOD_CTRL | WF_MOD_SHIFT},
        {WF_KEY_CTRL_SHIFT_END, WF_NK_END, WF_MOD_CTRL | WF_MOD_SHIFT}};

bool wf_stroke_equal(WfStroke a, WfStroke b)
{
	return a.code == b.code && a.mods == b.mods && a.named == b.named;
}

bool wf_seq_equal(const WfSeq *a, const WfSeq *b)
{
	uint8_t i;

	if (a->n != b->n) {
		return false;
	}
	for (i = 0; i < a->n; i++) {
		if (!wf_stroke_equal(a->s[i], b->s[i])) {
			return false;
		}
	}
	return true;
}

bool wf_seq_is_prefix(const WfSeq *a, const WfSeq *b)
{
	uint8_t i;

	if (a->n >= b->n) {
		return false;
	}
	for (i = 0; i < a->n; i++) {
		if (!wf_stroke_equal(a->s[i], b->s[i])) {
			return false;
		}
	}
	return true;
}

static WfStroke character(uint32_t cp, uint8_t mods)
{
	WfStroke s = {cp, mods, false};

	return s;
}

static WfStroke named(WfNamedKey k, uint8_t mods)
{
	WfStroke s = {(uint32_t)k, mods, true};

	return s;
}

bool wf_stroke_from_event(const WfEvent *ev, WfStroke *out)
{
	size_t i;

	switch (ev->type) {
	case WF_EV_CHAR:
		if (ev->ch < 0x20 || 0x7F == ev->ch) {
			return false;
		}
		*out = character(ev->ch, 0);
		return true;
	case WF_EV_CTRL: *out = character(ev->ch, WF_MOD_CTRL); return true;
	case WF_EV_CTRL_SHIFT:
		*out = character(ev->ch, WF_MOD_CTRL | WF_MOD_SHIFT);
		return true;
	case WF_EV_ALT: *out = character(ev->ch, WF_MOD_ALT); return true;
	case WF_EV_KEY:
		if (WF_KEY_CTRL_L == ev->key) {
			*out = character('l', WF_MOD_CTRL);
			return true;
		}
		for (i = 0; i < WF_ARRAY_LEN(KEYS); i++) {
			if (KEYS[i].key == ev->key) {
				*out = named(
				        (WfNamedKey)KEYS[i].nk, KEYS[i].mods);
				return true;
			}
		}
		return false;
	case WF_EV_RELEASE:
	case WF_EV_RESIZE:
	case WF_EV_MOUSE:   break;
	}
	return false;
}

static bool is_ascii_letter(uint32_t c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool wf_stroke_bindable(WfStroke s, bool interrupt_is_signal, const char **why)
{
	const char *dummy;
	size_t i;

	if (!why) {
		why = &dummy;
	}
	*why = NULL;
	if (s.named) {
		if (0 == s.code || s.code >= WF_NK_COUNT) {
			*why = "not a key";
			return false;
		}
		for (i = 0; i < WF_ARRAY_LEN(KEYS); i++) {
			if (KEYS[i].nk == s.code && KEYS[i].mods == s.mods) {
				return true;
			}
		}
		*why = "the terminal does not report this key";
		return false;
	}
	if (s.code < 0x20 || 0x7F == s.code || s.code > 0x10FFFF ||
	        (s.code >= 0xD800 && s.code <= 0xDFFF)) {
		*why = "not a printable character";
		return false;
	}
	if (s.mods & WF_MOD_CTRL) {
		if (s.mods & WF_MOD_ALT) {
			*why = "Ctrl+Alt is AltGr on many layouts";
			return false;
		}
		if (!((s.code >= 'a' && s.code <= 'z') ||
		            (s.code >= '0' && s.code <= '9') || '_' == s.code ||
		            '\\' == s.code)) {
			*why = "the terminal reports Ctrl only with a letter, "
			       "a digit, _ or backslash";
			return false;
		}
		if ((s.mods & WF_MOD_SHIFT) &&
		        !(s.code >= 'a' && s.code <= 'z')) {
			*why = "Ctrl+Shift is reported only with a letter";
			return false;
		}
		if (interrupt_is_signal && 'c' == s.code &&
		        !(s.mods & WF_MOD_SHIFT)) {
			*why = "Ctrl+C is the interrupt of the program";
			return false;
		}
		if (!(s.mods & WF_MOD_SHIFT) &&
		        ('i' == s.code || 'm' == s.code)) {
			*why = "terminals send Ctrl+I as Tab and Ctrl+M as "
			       "Enter";
			return false;
		}
		return true;
	}
	if (s.mods & WF_MOD_ALT) {
		if (s.mods & WF_MOD_SHIFT) {
			*why = "Alt+Shift is not reported apart from Alt";
			return false;
		}
		if (is_ascii_letter(s.code) && s.code < 'a') {
			*why = "Alt is reported with the lower-case letter";
			return false;
		}
		return true;
	}
	if (s.mods & WF_MOD_SHIFT) {
		*why = "Shift is part of the character itself";
		return false;
	}
	return true;
}

static size_t put(char *out, size_t cap, size_t at, const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (at + i + 1 < cap) {
			out[at + i] = s[i];
		}
	}
	return at + n;
}

size_t wf_stroke_format(WfStroke s, char *out, size_t cap)
{
	size_t at = 0;
	char buf[8];
	const char *text;
	size_t n;

	if (s.mods & WF_MOD_CTRL) {
		at = put(out, cap, at, "Ctrl+", 5);
	}
	if (s.mods & WF_MOD_ALT) {
		at = put(out, cap, at, "Alt+", 4);
	}
	if (s.mods & WF_MOD_SHIFT) {
		at = put(out, cap, at, "Shift+", 6);
	}
	if (s.named) {
		text = s.code < WF_NK_COUNT ? NAMES[s.code] : "?";
		n = strlen(text);
	} else if (' ' == s.code) {
		text = "Space";
		n = 5;
	} else if (',' == s.code) {
		text = "Comma";
		n = 5;
	} else if ('+' == s.code) {
		text = "Plus";
		n = 4;
	} else {
		uint32_t c = s.code;

		if ((s.mods & (WF_MOD_CTRL | WF_MOD_ALT)) && c >= 'a' &&
		        c <= 'z') {
			c -= 32;
		}
		n = wf_utf8_encode(c, buf);
		text = buf;
	}
	at = put(out, cap, at, text, n);
	if (cap) {
		out[at < cap ? at : cap - 1] = '\0';
	}
	return cap ? (at < cap ? at : cap - 1) : 0;
}

static bool ieq(const char *a, size_t n, const char *b)
{
	size_t i;

	if (strlen(b) != n) {
		return false;
	}
	for (i = 0; i < n; i++) {
		char x = a[i], y = b[i];

		if (x >= 'A' && x <= 'Z') {
			x = (char)(x + 32);
		}
		if (y >= 'A' && y <= 'Z') {
			y = (char)(y + 32);
		}
		if (x != y) {
			return false;
		}
	}
	return true;
}

static int modifier(const char *s, size_t n)
{
	if (ieq(s, n, "ctrl") || ieq(s, n, "control")) {
		return WF_MOD_CTRL;
	}
	if (ieq(s, n, "alt")) {
		return WF_MOD_ALT;
	}
	if (ieq(s, n, "shift")) {
		return WF_MOD_SHIFT;
	}
	return 0;
}

static bool key_token(const char *s, size_t n, WfStroke *out)
{
	static const struct
	{
		const char *name;
		uint8_t nk;
	} extra[] = {{"escape", WF_NK_ESC}, {"del", WF_NK_DELETE},
	        {"pgup", WF_NK_PGUP}, {"pgdn", WF_NK_PGDN},
	        {"return", WF_NK_ENTER}};
	size_t i;
	uint32_t cp;
	size_t used;

	for (i = 1; i < WF_NK_COUNT; i++) {
		if (ieq(s, n, NAMES[i])) {
			*out = named((WfNamedKey)i, 0);
			return true;
		}
	}
	for (i = 0; i < WF_ARRAY_LEN(extra); i++) {
		if (ieq(s, n, extra[i].name)) {
			*out = named((WfNamedKey)extra[i].nk, 0);
			return true;
		}
	}
	if (ieq(s, n, "space")) {
		*out = character(' ', 0);
		return true;
	}
	if (ieq(s, n, "comma")) {
		*out = character(',', 0);
		return true;
	}
	if (ieq(s, n, "plus")) {
		*out = character('+', 0);
		return true;
	}
	if (!n) {
		return false;
	}
	used = wf_utf8_decode(s, n, &cp);
	if (used != n || cp < 0x20 || 0x7F == cp || WF_CP_REPLACEMENT == cp) {
		return false;
	}
	*out = character(cp, 0);
	return true;
}

/* yazi's way of writing a key: <C-s>, <A-x>, <S-Tab>, <Enter>, <F2>. It is the
 * same key as "Ctrl+s", "Alt+x", "Shift+Tab", "Enter", "F2". */
static bool angle_key(const char *text, size_t n, WfStroke *out)
{
	char plain[48];
	size_t at = 0;
	const char *p = text + 1;
	size_t len = n - 2;

	while (len >= 3 && '-' == p[1] &&
	        ('C' == p[0] || 'A' == p[0] || 'S' == p[0] || 'c' == p[0] ||
	                'a' == p[0] || 's' == p[0])) {
		const char *name = 'C' == p[0] || 'c' == p[0] ? "Ctrl+"
		        : 'A' == p[0] || 'a' == p[0]          ? "Alt+"
		                                              : "Shift+";
		size_t k = strlen(name);

		if (at + k >= sizeof plain) {
			return false;
		}
		memcpy(plain + at, name, k);
		at += k;
		p += 2;
		len -= 2;
	}
	if (!len || at + len >= sizeof plain) {
		return false;
	}
	memcpy(plain + at, p, len);
	at += len;
	plain[at] = '\0';
	if ('<' == plain[0]) { /* no nesting */
		return false;
	}
	return wf_stroke_parse(plain, at, out);
}

bool wf_stroke_parse(const char *text, size_t n, WfStroke *out)
{
	uint8_t mods = 0;
	WfStroke k;

	while (n && ' ' == *text) {
		text++;
		n--;
	}
	while (n && ' ' == text[n - 1]) {
		n--;
	}
	if (n >= 3 && '<' == text[0] && '>' == text[n - 1]) {
		return angle_key(text, n, out);
	}
	for (;;) {
		size_t i;
		int m;

		for (i = 1; i < n && '+' != text[i]; i++) {
		}
		if (i >= n || !(m = modifier(text, i))) {
			break;
		}
		mods = (uint8_t)(mods | m);
		text += i + 1;
		n -= i + 1;
	}
	if (!key_token(text, n, &k)) {
		return false;
	}
	if (!k.named) {
		if (mods & (WF_MOD_CTRL | WF_MOD_ALT)) {
			if (k.code >= 'A' && k.code <= 'Z') {
				k.code += 32;
			}
		} else if (mods & WF_MOD_SHIFT) {
			if (!(k.code >= 'a' && k.code <= 'z')) {
				return false;
			}
			k.code -= 32;
			mods = (uint8_t)(mods & ~WF_MOD_SHIFT);
		}
	}
	k.mods = mods;
	*out = k;
	return true;
}

static bool plain_run(const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if ((unsigned char)s[i] <= 0x20 ||
		        (unsigned char)s[i] >= 0x7F) {
			return false;
		}
	}
	return n > 0;
}

bool wf_seq_parse(const char *text, size_t n, WfSeq *out)
{
	size_t i = 0;

	out->n = 0;
	while (i < n) {
		size_t from;
		WfStroke k;

		while (i < n && ' ' == text[i]) {
			i++;
		}
		from = i;
		while (i < n && ' ' != text[i]) {
			i++;
		}
		if (i == from) {
			break;
		}
		if (wf_stroke_parse(text + from, i - from, &k)) {
			if (WF_SEQ_MAX == out->n) {
				return false;
			}
			out->s[out->n++] = k;
		} else if (plain_run(text + from, i - from)) {
			size_t j;

			for (j = from; j < i; j++) {
				if (WF_SEQ_MAX == out->n) {
					return false;
				}
				out->s[out->n++] =
				        character((uint8_t)text[j], 0);
			}
		} else {
			return false;
		}
	}
	return out->n > 0;
}

static bool compact_char(WfStroke s)
{
	return !s.named && !s.mods && s.code > 0x20 && s.code < 0x7F &&
	        ',' != s.code && '+' != s.code;
}

size_t wf_seq_format(const WfSeq *q, char *out, size_t cap)
{
	size_t at = 0;
	uint8_t i;

	for (i = 0; i < q->n; i++) {
		char one[24];
		size_t n = wf_stroke_format(q->s[i], one, sizeof one);

		if (i &&
		        !(compact_char(q->s[i]) && compact_char(q->s[i - 1]))) {
			at = put(out, cap, at, " ", 1);
		}
		at = put(out, cap, at, one, n);
	}
	if (cap) {
		out[at < cap ? at : cap - 1] = '\0';
	}
	return cap ? (at < cap ? at : cap - 1) : 0;
}

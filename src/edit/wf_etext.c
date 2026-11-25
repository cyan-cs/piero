#include "wf_etext.h"

#include <stdlib.h>
#include <string.h>

#include "../text/wf_text.h"

#define LINE_MIN_CAP 16u

/* The model's allocations go through these so a test can make memory run out
 * at the Nth one and stay out, and prove that a failed edit leaves the text,
 * the cursor and the history exactly as they were (a rollback that needs
 * memory fails then too). Off (-1) in the product. */
static volatile long fail_countdown = -1;

void wf_et_fail_nth_alloc(long n)
{
	fail_countdown = n;
}

static bool alloc_refused(void)
{
	if (fail_countdown < 0) {
		return false;
	}
	if (0 == fail_countdown) {
		return true; /* and every one after it */
	}
	fail_countdown--;
	return false;
}

void *wf_et_malloc(size_t n)
{
	return alloc_refused() ? NULL : malloc(n);
}

void *wf_et_calloc(size_t count, size_t size)
{
	return alloc_refused() ? NULL : calloc(count, size);
}

void *wf_et_realloc(void *p, size_t n)
{
	return alloc_refused() ? NULL : realloc(p, n);
}

size_t wf_eol_len(WfEol e)
{
	return WF_EOL_CRLF == e ? 2u : WF_EOL_NONE == e ? 0u : 1u;
}

const char *wf_eol_bytes(WfEol e)
{
	return WF_EOL_CRLF == e  ? "\r\n"
	        : WF_EOL_CR == e ? "\r"
	        : WF_EOL_LF == e ? "\n"
	                         : "";
}

static WfStatus line_reserve(WfELine *l, size_t need)
{
	size_t cap = l->cap ? l->cap : LINE_MIN_CAP;
	char *p;

	if (need <= l->cap) {
		return WF_OK;
	}
	while (cap < need) {
		if (cap > SIZE_MAX / 2) {
			return WF_E_RANGE;
		}
		cap *= 2;
	}
	p = wf_et_realloc(l->s, cap);
	if (!p) {
		return WF_E_NOMEM;
	}
	l->s = p;
	l->cap = cap;
	return WF_OK;
}

static WfStatus lines_reserve(WfEText *t, size_t need)
{
	size_t cap = t->cap ? t->cap : 64;
	WfELine *p;

	if (need <= t->cap) {
		return WF_OK;
	}
	while (cap < need) {
		if (cap > SIZE_MAX / 2 / sizeof *p) {
			return WF_E_RANGE;
		}
		cap *= 2;
	}
	p = wf_et_realloc(t->lines, cap * sizeof *p);
	if (!p) {
		return WF_E_NOMEM;
	}
	t->lines = p;
	t->cap = cap;
	return WF_OK;
}

static void line_free(WfELine *l)
{
	free(l->s);
	memset(l, 0, sizeof *l);
}

/* Whether `added` more bytes fit under the limit. */
static bool fits(const WfEText *t, size_t added)
{
	if (added > SIZE_MAX - t->bytes) {
		return false;
	}
	return !t->max_bytes || t->bytes + added <= t->max_bytes;
}

static void touch(WfEText *t)
{
	t->gen++;
}

WfStatus wf_et_init(WfEText *t, WfEol default_eol, size_t max_bytes)
{
	if (!t || WF_EOL_NONE == default_eol) {
		return WF_E_INVAL;
	}
	memset(t, 0, sizeof *t);
	t->default_eol = default_eol;
	t->max_bytes = max_bytes;
	if (WF_OK != lines_reserve(t, 1)) {
		return WF_E_NOMEM;
	}
	t->n = 1;
	memset(&t->lines[0], 0, sizeof t->lines[0]);
	return WF_OK;
}

void wf_et_free(WfEText *t)
{
	size_t i;

	if (!t) {
		return;
	}
	for (i = 0; i < t->n; i++) {
		line_free(&t->lines[i]);
	}
	free(t->lines);
	memset(t, 0, sizeof *t);
}

/* Length and kind of the break starting at s[i], 0 if there is none. */
static size_t break_at(const char *s, size_t n, size_t i, WfEol *kind)
{
	if ('\r' == s[i]) {
		if (i + 1 < n && '\n' == s[i + 1]) {
			*kind = WF_EOL_CRLF;
			return 2;
		}
		*kind = WF_EOL_CR;
		return 1;
	}
	if ('\n' == s[i]) {
		*kind = WF_EOL_LF;
		return 1;
	}
	return 0;
}

WfStatus wf_et_load(WfEText *t, const char *s, size_t n)
{
	WfEText fresh;
	size_t i = 0, start = 0;
	WfStatus st;

	if (!t || (n && !s)) {
		return WF_E_INVAL;
	}
	fresh = *t;
	st = wf_et_init(&fresh, t->default_eol, t->max_bytes);
	if (WF_OK != st) {
		return st;
	}
	if (t->max_bytes && n > t->max_bytes) {
		wf_et_free(&fresh);
		return WF_E_RANGE;
	}
	fresh.gen = t->gen + 1;
	fresh.shape_gen = t->shape_gen + 1;
	while (i <= n) {
		WfEol kind = WF_EOL_NONE;
		size_t blen = i < n ? break_at(s, n, i, &kind) : 0;
		WfELine *l;

		if (i < n && !blen) {
			i++;
			continue;
		}
		/* a line ends here: at a break or at the end of the input */
		if (fresh.n == fresh.cap || NULL == fresh.lines) {
			st = lines_reserve(&fresh, fresh.n + 1);
			if (WF_OK != st) {
				break;
			}
		}
		l = &fresh.lines[fresh.n - 1];
		st = line_reserve(l, i - start ? i - start : 1);
		if (WF_OK != st) {
			break;
		}
		memcpy(l->s, s + start, i - start);
		l->len = i - start;
		l->eol = kind;
		if (i == n) {
			break;
		}
		/* open the next line */
		st = lines_reserve(&fresh, fresh.n + 1);
		if (WF_OK != st) {
			break;
		}
		memset(&fresh.lines[fresh.n], 0, sizeof fresh.lines[0]);
		fresh.n++;
		i += blen;
		start = i;
	}
	if (WF_OK != st) {
		wf_et_free(&fresh);
		return st;
	}
	fresh.bytes = n;
	wf_et_free(t);
	*t = fresh;
	return WF_OK;
}

int wf_pos_cmp(WfPos a, WfPos b)
{
	if (a.line != b.line) {
		return a.line < b.line ? -1 : 1;
	}
	if (a.col != b.col) {
		return a.col < b.col ? -1 : 1;
	}
	return 0;
}

WfPos wf_et_end(const WfEText *t)
{
	WfPos p = {t->n - 1, t->lines[t->n - 1].len};

	return p;
}

bool wf_et_valid(const WfEText *t, WfPos p)
{
	return p.line < t->n && p.col <= t->lines[p.line].len;
}

typedef struct
{
	size_t breaks; /* line breaks in the text */
	size_t added;  /* bytes the text adds, terminators as they will be */
} InsertShape;

static InsertShape shape_of(const char *s, size_t n, bool normalize, WfEol ctx)
{
	InsertShape sh = {0, 0};
	size_t i = 0;

	while (i < n) {
		WfEol kind;
		size_t blen = break_at(s, n, i, &kind);

		if (!blen) {
			sh.added++;
			i++;
			continue;
		}
		sh.breaks++;
		sh.added += wf_eol_len(normalize ? ctx : kind);
		i += blen;
	}
	return sh;
}

WfStatus wf_et_insert(WfEText *t,
        WfPos at,
        const char *s,
        size_t n,
        bool normalize,
        WfPos *end)
{
	WfELine *l;
	WfEol ctx, tail_eol;
	InsertShape sh;
	WfELine *fresh = NULL;
	char *tail = NULL;
	size_t tail_len, i, piece_start = 0, k = 0;
	WfStatus st;

	if (!t || !wf_et_valid(t, at) || (n && !s)) {
		return WF_E_INVAL;
	}
	if (!n) {
		if (end) {
			*end = at;
		}
		return WF_OK;
	}
	l = &t->lines[at.line];
	ctx = WF_EOL_NONE != l->eol ? l->eol : t->default_eol;
	sh = shape_of(s, n, normalize, ctx);
	if (!fits(t, sh.added)) {
		return WF_E_RANGE;
	}

	if (0 == sh.breaks) {
		st = line_reserve(l, l->len + n);
		if (WF_OK != st) {
			return st;
		}
		memmove(l->s + at.col + n, l->s + at.col, l->len - at.col);
		memcpy(l->s + at.col, s, n);
		l->len += n;
		t->bytes += n;
		touch(t);
		if (end) {
			end->line = at.line;
			end->col = at.col + n;
		}
		return WF_OK;
	}

	/* Line breaks: prepare everything that can fail, then commit. */
	if (sh.breaks > SIZE_MAX - t->n) {
		return WF_E_RANGE;
	}
	st = lines_reserve(t, t->n + sh.breaks);
	if (WF_OK != st) {
		return st;
	}
	l = &t->lines[at.line]; /* the array may have moved */
	tail_len = l->len - at.col;
	tail_eol = l->eol;
	fresh = wf_et_calloc(sh.breaks, sizeof *fresh);
	if (!fresh) {
		return WF_E_NOMEM;
	}
	if (tail_len) {
		tail = wf_et_malloc(tail_len);
		if (!tail) {
			free(fresh);
			return WF_E_NOMEM;
		}
		memcpy(tail, l->s + at.col, tail_len);
	}

	/* the pieces between breaks, in order; piece 0 joins the head */
	{
		WfEol kinds
		        [1]; /* only the latest break kind is needed at a time */
		size_t first_len = 0;
		WfEol first_eol = WF_EOL_NONE;

		(void)kinds;
		i = 0;
		while (i <= n) {
			WfEol kind = WF_EOL_NONE;
			size_t blen = i < n ? break_at(s, n, i, &kind) : 0;
			size_t plen;
			WfEol eol;

			if (i < n && !blen) {
				i++;
				continue;
			}
			plen = i - piece_start;
			eol = i == n ? tail_eol : normalize ? ctx : kind;
			if (0 == k) {
				first_len = plen;
				first_eol = eol;
			} else {
				WfELine *nl = &fresh[k - 1];
				size_t extra = i == n ? tail_len : 0;

				nl->s = wf_et_malloc(
				        plen + extra ? plen + extra : 1);
				if (!nl->s) {
					st = WF_E_NOMEM;
					goto fail;
				}
				nl->cap = plen + extra ? plen + extra : 1;
				memcpy(nl->s, s + piece_start, plen);
				if (extra) {
					memcpy(nl->s + plen, tail, extra);
				}
				nl->len = plen + extra;
				nl->eol = eol;
			}
			k++;
			if (i == n) {
				break;
			}
			piece_start = i + blen;
			i += blen;
		}
		st = line_reserve(l, at.col + first_len);
		if (WF_OK != st) {
			goto fail;
		}
		memcpy(l->s + at.col, s, first_len);
		l->len = at.col + first_len;
		l->eol = first_eol;
	}
	memmove(&t->lines[at.line + 1 + sh.breaks], &t->lines[at.line + 1],
	        (t->n - at.line - 1) * sizeof *t->lines);
	memcpy(&t->lines[at.line + 1], fresh, sh.breaks * sizeof *fresh);
	t->n += sh.breaks;
	t->bytes += sh.added;
	if (sh.breaks) {
		t->shape_gen++;
	}
	touch(t);
	if (end) {
		end->line = at.line + sh.breaks;
		end->col = t->lines[end->line].len - tail_len;
	}
	free(fresh);
	free(tail);
	return WF_OK;
fail:
	for (i = 0; i < sh.breaks; i++) {
		free(fresh[i].s);
	}
	free(fresh);
	free(tail);
	return st;
}

size_t wf_et_size(const WfEText *t, WfPos from, WfPos to)
{
	size_t total = 0, i;

	if (wf_pos_cmp(from, to) >= 0) {
		return 0;
	}
	if (from.line == to.line) {
		return to.col - from.col;
	}
	total = t->lines[from.line].len - from.col +
	        wf_eol_len(t->lines[from.line].eol);
	for (i = from.line + 1; i < to.line; i++) {
		total += t->lines[i].len + wf_eol_len(t->lines[i].eol);
	}
	return total + to.col;
}

WfStatus wf_et_read(
        const WfEText *t, WfPos from, WfPos to, char **out, size_t *out_n)
{
	size_t size, at = 0, i;
	char *buf;

	if (!t || !out || !wf_et_valid(t, from) || !wf_et_valid(t, to) ||
	        wf_pos_cmp(from, to) > 0) {
		return WF_E_INVAL;
	}
	size = wf_et_size(t, from, to);
	buf = wf_et_malloc(size ? size : 1);
	if (!buf) {
		return WF_E_NOMEM;
	}
	if (from.line == to.line) {
		memcpy(buf, t->lines[from.line].s + from.col, size);
	} else {
		const WfELine *l = &t->lines[from.line];
		size_t e;

		memcpy(buf, l->s + from.col, l->len - from.col);
		at = l->len - from.col;
		e = wf_eol_len(l->eol);
		memcpy(buf + at, wf_eol_bytes(l->eol), e);
		at += e;
		for (i = from.line + 1; i < to.line; i++) {
			l = &t->lines[i];
			memcpy(buf + at, l->s, l->len);
			at += l->len;
			e = wf_eol_len(l->eol);
			memcpy(buf + at, wf_eol_bytes(l->eol), e);
			at += e;
		}
		memcpy(buf + at, t->lines[to.line].s, to.col);
	}
	*out = buf;
	if (out_n) {
		*out_n = size;
	}
	return WF_OK;
}

WfStatus wf_et_delete(
        WfEText *t, WfPos from, WfPos to, char **out, size_t *out_n)
{
	WfStatus st;
	size_t size, i;
	WfELine *l;

	if (!t || !wf_et_valid(t, from) || !wf_et_valid(t, to) ||
	        wf_pos_cmp(from, to) > 0) {
		return WF_E_INVAL;
	}
	if (out) {
		*out = NULL;
	}
	if (out_n) {
		*out_n = 0;
	}
	if (0 == wf_pos_cmp(from, to)) {
		return WF_OK;
	}
	size = wf_et_size(t, from, to);
	if (out) {
		st = wf_et_read(t, from, to, out, out_n);
		if (WF_OK != st) {
			return st;
		}
	}
	l = &t->lines[from.line];
	if (from.line == to.line) {
		memmove(l->s + from.col, l->s + to.col, l->len - to.col);
		l->len -= to.col - from.col;
	} else {
		WfELine *last = &t->lines[to.line];
		size_t tail_len = last->len - to.col;

		st = line_reserve(l, from.col + tail_len);
		if (WF_OK != st) {
			if (out) {
				free(*out);
				*out = NULL;
			}
			return st;
		}
		memcpy(l->s + from.col, last->s + to.col, tail_len);
		l->len = from.col + tail_len;
		l->eol = last->eol;
		for (i = from.line + 1; i <= to.line; i++) {
			line_free(&t->lines[i]);
		}
		memmove(&t->lines[from.line + 1], &t->lines[to.line + 1],
		        (t->n - to.line - 1) * sizeof *t->lines);
		t->n -= to.line - from.line;
		t->shape_gen++;
	}
	t->bytes -= size;
	touch(t);
	return WF_OK;
}

static size_t line_bytes(const WfELine *l)
{
	return l->len + wf_eol_len(l->eol);
}

WfStatus wf_et_snap_take(
        const WfEText *t, size_t first, size_t last, WfETSnap *out)
{
	size_t i, n;

	memset(out, 0, sizeof *out);
	if (!t || first > last || last >= t->n) {
		return WF_E_INVAL;
	}
	n = last - first + 1;
	out->lines = wf_et_calloc(n, sizeof *out->lines);
	if (!out->lines) {
		return WF_E_NOMEM;
	}
	for (i = 0; i < n; i++) {
		const WfELine *from = &t->lines[first + i];
		WfELine *to = &out->lines[i];

		to->len = from->len;
		to->eol = from->eol;
		if (from->len) {
			to->s = wf_et_malloc(from->len);
			if (!to->s) {
				out->n = i;
				wf_et_snap_free(out);
				return WF_E_NOMEM;
			}
			memcpy(to->s, from->s, from->len);
			to->cap = from->len;
		}
		out->bytes += line_bytes(from);
	}
	out->first = first;
	out->n = n;
	return WF_OK;
}

void wf_et_snap_free(WfETSnap *snap)
{
	size_t i;

	if (!snap || !snap->lines) {
		return;
	}
	for (i = 0; i < snap->n; i++) {
		free(snap->lines[i].s);
	}
	free(snap->lines);
	memset(snap, 0, sizeof *snap);
}

void wf_et_snap_restore(WfEText *t, WfETSnap *snap, size_t cur_last)
{
	size_t i, now = cur_last - snap->first + 1, gone = 0;

	for (i = snap->first; i <= cur_last; i++) {
		gone += line_bytes(&t->lines[i]);
		line_free(&t->lines[i]);
	}
	memmove(&t->lines[snap->first + snap->n], &t->lines[cur_last + 1],
	        (t->n - cur_last - 1) * sizeof *t->lines);
	memcpy(&t->lines[snap->first], snap->lines,
	        snap->n * sizeof *snap->lines);
	t->n = t->n - now + snap->n;
	t->bytes = t->bytes - gone + snap->bytes;
	free(snap->lines);
	memset(snap, 0, sizeof *snap);
	t->shape_gen++;
	touch(t);
}

static size_t scalar_at(const WfELine *l, size_t col, uint32_t *cp)
{
	size_t n = wf_utf8_decode(l->s + col, l->len - col, cp);

	return n ? n : 1;
}

/* Scalars that stay with the one before them in a cursor step: combining
 * marks and variation selectors (width 0) and the skin-tone modifiers. The
 * zero width joiner is not one of them on purpose: it is shown as a visible
 * <ZWJ> label (a hidden joiner can disguise a name), so it is a step of its
 * own, and what the cursor does matches what is on screen. */
static bool stays_with_previous(uint32_t cp)
{
	return 0 == wf_cp_width(cp) || (cp >= 0x1F3FBu && cp <= 0x1F3FFu);
}

/* The start of the scalar that ends at `col` (col > 0). */
static size_t scalar_start_before(const WfELine *l, size_t col)
{
	col--;
	while (col > 0 && 0x80 == ((unsigned char)l->s[col] & 0xC0)) {
		col--;
	}
	return col;
}

size_t wf_et_col_next(const WfELine *l, size_t col)
{
	uint32_t cp;

	if (col >= l->len) {
		return l->len;
	}
	col += scalar_at(l, col, &cp);
	while (col < l->len) {
		size_t n = scalar_at(l, col, &cp);

		if (!stays_with_previous(cp)) {
			break;
		}
		col += n;
	}
	return col;
}

size_t wf_et_col_prev(const WfELine *l, size_t col)
{
	uint32_t cp;

	if (0 == col) {
		return 0;
	}
	if (col > l->len) {
		col = l->len;
	}
	do {
		col = scalar_start_before(l, col);
		(void)scalar_at(l, col, &cp);
	} while (col > 0 && stays_with_previous(cp));
	return col;
}

bool wf_et_special_scalar(uint32_t cp)
{
	return cp < 0x20 || (cp >= 0x7F && cp < 0xA0) ||
	        (cp >= 0x200B && cp <= 0x200F) ||
	        (cp >= 0x202A && cp <= 0x202E) ||
	        (cp >= 0x2060 && cp <= 0x2064) ||
	        (cp >= 0x2066 && cp <= 0x2069) || 0xFEFF == cp || 0xAD == cp;
}

/* The cells a scalar takes where the view draws it: a special one as the text
 * that replaces it, so that positions, wrapping and the cursor agree with the
 * screen. */
static size_t scalar_cells(uint32_t cp, size_t at, unsigned tab)
{
	if ('\t' == cp) {
		return tab ? tab - at % tab : 1;
	}
	if (wf_et_special_scalar(cp)) {
		return wf_sanitized_cells(cp);
	}
	return (size_t)wf_cp_width(cp);
}

size_t wf_et_cells(const WfELine *l, size_t col, unsigned tab)
{
	size_t at = 0, i = 0;

	if (col > l->len) {
		col = l->len;
	}
	while (i < col) {
		uint32_t cp;
		size_t n;
		unsigned char c = (unsigned char)l->s[i];

		if (c >= 0x20 &&
		        c < 0x7F) { /* the common case, without decoding */
			at++;
			i++;
			continue;
		}
		n = scalar_at(l, i, &cp);
		at += scalar_cells(cp, at, tab);
		i += n;
	}
	return at;
}

size_t wf_et_col_at_cells(const WfELine *l, size_t cells, unsigned tab)
{
	size_t at = 0, i = 0;

	while (i < l->len) {
		uint32_t cp;
		size_t n, w;
		unsigned char c = (unsigned char)l->s[i];

		if (c >= 0x20 && c < 0x7F) {
			if (at + 1 > cells) {
				break;
			}
			at++;
			i++;
			continue;
		}
		n = scalar_at(l, i, &cp);
		w = scalar_cells(cp, at, tab);
		if (at + w > cells && w > 0) {
			break;
		}
		at += w;
		i += n;
	}
	return i;
}

size_t wf_et_wrap(const WfELine *l, size_t width, unsigned tab, size_t **starts)
{
	size_t *rows = malloc(sizeof *rows * 4), cap = 4, n = 1, at = 0, i = 0;

	if (!rows) {
		return 0;
	}
	rows[0] = 0;
	if (0 == width) {
		width = 1;
	}
	while (i < l->len) {
		uint32_t cp;
		size_t len = scalar_at(l, i, &cp);
		size_t w = scalar_cells(cp, at, tab);

		if (at + w > width && at > 0 && w > 0) {
			if (n == cap) {
				size_t *grown =
				        realloc(rows, sizeof *rows * cap * 2);

				if (!grown) {
					free(rows);
					return 0;
				}
				rows = grown;
				cap *= 2;
			}
			rows[n++] = i;
			at = 0;
			w = scalar_cells(cp, 0, tab);
		}
		at += w;
		i += len;
	}
	/* An insertion cursor at a full row's end belongs to the next display row. */
	if (l->len && at >= width) {
		if (n == cap) {
			size_t *grown = realloc(rows, sizeof *rows * (cap + 1));
			if (!grown) {
				free(rows);
				return 0;
			}
			rows = grown;
		}
		rows[n++] = l->len;
	}
	*starts = rows;
	return n;
}

bool wf_et_check(const WfEText *t)
{
	size_t i, total = 0;

	if (!t || t->n < 1 || t->n > t->cap) {
		return false;
	}
	for (i = 0; i < t->n; i++) {
		const WfELine *l = &t->lines[i];
		size_t j = 0;

		if (l->len > l->cap || (l->len && !l->s)) {
			return false;
		}
		if (i + 1 < t->n && WF_EOL_NONE == l->eol) {
			return false;
		}
		if (i + 1 == t->n && WF_EOL_NONE != l->eol) {
			return false;
		}
		while (j < l->len) {
			uint32_t cp;
			size_t n = wf_utf8_decode(l->s + j, l->len - j, &cp);

			if (!n || 0 == cp ||
			        (WF_CP_REPLACEMENT == cp && 1 == n)) {
				return false;
			}
			j += n;
		}
		total += l->len + wf_eol_len(l->eol);
	}
	return total == t->bytes;
}

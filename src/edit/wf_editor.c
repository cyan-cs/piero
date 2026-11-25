#include "wf_editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../i18n/wf_i18n.h"
#include "../text/wf_text.h"

#define PASTE_MAX (8u << 20)
#define BRACKET_SCAN_MAX (200000u)
#define COMPLETE_SCAN_LINES 20000u

void wf_ed_options_default(WfEditorOptions *o)
{
	o->tab_width = 8;
	o->use_spaces = false;
	o->auto_indent = true;
	o->case_sensitive = false;
	o->max_bytes = 32u << 20;
	o->undo_bytes = 16u << 20;
	o->new_eol = WF_EOL_CRLF; /* Windows: see wf_new_file_eol */
}

WfStatus wf_ed_init(WfEditor *e, const WfEditorOptions *o)
{
	WfStatus st;

	memset(e, 0, sizeof *e);
	if (o) {
		e->opt = *o;
	} else {
		wf_ed_options_default(&e->opt);
	}
	if (e->opt.tab_width < 1 || e->opt.tab_width > 16) {
		e->opt.tab_width = 8;
	}
	if (WF_EOL_LF != e->opt.new_eol && WF_EOL_CRLF != e->opt.new_eol) {
		e->opt.new_eol = WF_EOL_CRLF;
	}
	st = wf_et_init(&e->text, e->opt.new_eol, e->opt.max_bytes);
	if (WF_OK != st) {
		return st;
	}
	wf_un_init(&e->undo, e->opt.undo_bytes);
	e->enc.kind = WF_ENC_UTF8;
	return WF_OK;
}

void wf_ed_free(WfEditor *e)
{
	wf_et_free(&e->text);
	wf_un_free(&e->undo);
	free(e->clip);
	free(e->comp_prefix);
	memset(e, 0, sizeof *e);
}

static void say(WfEditor *e, const char *text)
{
	snprintf(e->msg, sizeof e->msg, "%s", text);
}

static WfEol dominant_eol(const WfEText *t)
{
	size_t lf = 0, crlf = 0, cr = 0, i;

	for (i = 0; i + 1 < t->n; i++) {
		WfEol k = t->lines[i].eol;

		if (WF_EOL_LF == k) {
			lf++;
		} else if (WF_EOL_CRLF == k) {
			crlf++;
		} else if (WF_EOL_CR == k) {
			cr++;
		}
	}
	if (!lf && !crlf && !cr) {
		return WF_EOL_LF;
	}
	if (crlf >= lf && crlf >= cr) {
		return WF_EOL_CRLF;
	}
	return lf >= cr ? WF_EOL_LF : WF_EOL_CR;
}

WfStatus wf_ed_set_text(WfEditor *e, const char *utf8, size_t n, WfEncoding enc)
{
	WfStatus st = wf_et_load(&e->text, utf8, n);

	if (WF_OK != st) {
		return st;
	}
	e->text.default_eol = dominant_eol(&e->text);
	e->enc = enc;
	wf_un_free(&e->undo);
	wf_un_init(&e->undo, e->opt.undo_bytes);
	e->cur.line = e->cur.col = 0;
	e->anchor = e->cur;
	e->has_anchor = e->mark = false;
	e->want_cells = 0;
	e->want_valid = false;
	e->dirty_from = 0;
	return WF_OK;
}

bool wf_ed_modified(const WfEditor *e)
{
	return wf_un_modified(&e->undo);
}

void wf_ed_mark_saved(WfEditor *e)
{
	wf_un_mark_clean(&e->undo);
}

bool wf_ed_has_selection(const WfEditor *e)
{
	return e->has_anchor && 0 != wf_pos_cmp(e->anchor, e->cur);
}

void wf_ed_selection(const WfEditor *e, WfPos *from, WfPos *to)
{
	if (wf_pos_cmp(e->anchor, e->cur) <= 0) {
		*from = e->anchor;
		*to = e->cur;
	} else {
		*from = e->cur;
		*to = e->anchor;
	}
}

void wf_ed_clear_selection(WfEditor *e)
{
	e->has_anchor = false;
	e->mark = false;
}

void wf_ed_select_all(WfEditor *e)
{
	e->anchor.line = e->anchor.col = 0;
	e->has_anchor = true;
	e->mark = false;
	e->cur = wf_et_end(&e->text);
}

void wf_ed_toggle_mark(WfEditor *e)
{
	if (e->mark) {
		wf_ed_clear_selection(e);
		say(e, wf_msg(MSG_ED_MARK_UNSET));
		return;
	}
	e->anchor = e->cur;
	e->has_anchor = true;
	e->mark = true;
	say(e, wf_msg(MSG_ED_MARK_SET));
}

static bool has_break(const char *s, size_t n)
{
	return memchr(s, '\n', n) || memchr(s, '\r', n);
}

/*
 * Taking a failed step back must not need memory, or "the text is as it was"
 * is a promise made by luck. Two cases:
 *  - inside one line, with no line break in the new text: deleting only moves
 *    bytes, and putting the old bytes back fits the room the line already
 *    has, so the plain re-insert below cannot fail;
 *  - anything else (several lines, or breaks in the new text) works on a copy
 *    of the lines it touches, taken before the first change, and a rollback
 *    puts that copy back without allocating.
 */
typedef struct
{
	bool copied;
	WfETSnap snap;
} Rollback;

static WfStatus rollback_begin(Rollback *rb,
        const WfEditor *e,
        WfPos from,
        WfPos to,
        const char *text,
        size_t n)
{
	memset(rb, 0, sizeof *rb);
	if (from.line == to.line && !has_break(text, n)) {
		return WF_OK;
	}
	rb->copied = true;
	return wf_et_snap_take(&e->text, from.line, to.line, &rb->snap);
}

/* Puts the text back; `cur_last` is the last line the edit's result covers. */
static void rollback_apply(Rollback *rb,
        WfEditor *e,
        WfPos from,
        WfPos inserted_end,
        size_t cur_last,
        const char *del,
        size_t del_n)
{
	if (rb->copied) {
		wf_et_snap_restore(&e->text, &rb->snap, cur_last);
		return;
	}
	(void)wf_et_delete(&e->text, from, inserted_end, NULL, NULL);
	if (del_n) {
		(void)wf_et_insert(&e->text, from, del, del_n, false, NULL);
	}
}

static void rollback_end(Rollback *rb)
{
	if (rb->copied) {
		wf_et_snap_free(&rb->snap);
	}
}

/*
 * Replaces [from, to) by `text` and records it. On any failure the text is as
 * it was and nothing is recorded. The cursor ends after the inserted text.
 */
static WfStatus replace(WfEditor *e,
        WfPos from,
        WfPos to,
        const char *text,
        size_t n,
        bool normalize,
        WfUKind kind)
{
	WfURec r;
	WfPos end = from;
	WfStatus st;
	Rollback rb;

	if (e->read_only) {
		return WF_E_ACCESS;
	}
	if (!wf_et_valid(&e->text, from) || !wf_et_valid(&e->text, to) ||
	        wf_pos_cmp(from, to) > 0) {
		return WF_E_INVAL;
	}
	if (0 == wf_pos_cmp(from, to) && 0 == n) {
		return WF_OK;
	}
	st = rollback_begin(&rb, e, from, to, text, n);
	if (WF_OK != st) {
		return st;
	}
	memset(&r, 0, sizeof r);
	r.at = from;
	r.before = e->cur;
	r.kind = kind;
	st = wf_et_delete(&e->text, from, to, &r.del, &r.del_n);
	if (WF_OK != st) {
		rollback_end(&rb);
		return st;
	}
	if (n) {
		st = wf_et_insert(&e->text, from, text, n, normalize, &end);
		if (WF_OK != st) {
			/* the insert changes nothing when it fails */
			rollback_apply(
			        &rb, e, from, from, from.line, r.del, r.del_n);
			rollback_end(&rb);
			free(r.del);
			return st;
		}
		if (normalize && has_break(text, n)) {
			st = wf_et_read(&e->text, from, end, &r.ins, &r.ins_n);
		} else {
			r.ins = wf_et_malloc(n);
			if (r.ins) {
				memcpy(r.ins, text, n);
				r.ins_n = n;
				st = WF_OK;
			} else {
				st = WF_E_NOMEM;
			}
		}
		if (WF_OK != st) {
			rollback_apply(
			        &rb, e, from, end, end.line, r.del, r.del_n);
			rollback_end(&rb);
			free(r.del);
			return st;
		}
	}
	r.after = end;
	{
		/* the push owns the buffers; keep what a rollback needs */
		char *del_copy = NULL;
		size_t del_n = r.del_n;

		if (r.del_n && !rb.copied) {
			del_copy = wf_et_malloc(r.del_n);
			if (!del_copy) {
				rollback_apply(&rb, e, from, end, end.line,
				        r.del, r.del_n);
				free(r.del);
				free(r.ins);
				return WF_E_NOMEM;
			}
			memcpy(del_copy, r.del, r.del_n);
		}
		st = wf_un_push(&e->undo, &r);
		if (WF_OK != st) {
			rollback_apply(
			        &rb, e, from, end, end.line, del_copy, del_n);
			rollback_end(&rb);
			free(del_copy);
			return st;
		}
		free(del_copy);
	}
	rollback_end(&rb);
	e->cur = end;
	if (from.line < e->dirty_from) {
		e->dirty_from = from.line;
	}
	return WF_OK;
}

/* Replacing the selection (if any) by `text`. */
static WfStatus replace_selection(
        WfEditor *e, const char *text, size_t n, bool normalize, WfUKind kind)
{
	WfPos a, b;
	WfStatus st;

	if (wf_ed_has_selection(e)) {
		wf_ed_selection(e, &a, &b);
		kind = WF_UK_OTHER;
	} else {
		a = b = e->cur;
	}
	st = replace(e, a, b, text, n, normalize, kind);
	if (WF_OK == st) {
		wf_ed_clear_selection(e);
		e->want_valid = false;
	}
	return st;
}

static bool is_word_cp(uint32_t cp)
{
	return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') ||
	        (cp >= 'A' && cp <= 'Z') || '_' == cp || cp >= 0x80;
}

/* 0 space, 1 word, 2 punctuation */
static int cls(uint32_t cp)
{
	if (' ' == cp || '\t' == cp) {
		return 0;
	}
	return is_word_cp(cp) ? 1 : 2;
}

static uint32_t cp_at(const WfELine *l, size_t col)
{
	uint32_t cp = 0;

	if (col < l->len) {
		(void)wf_utf8_decode(l->s + col, l->len - col, &cp);
	}
	return cp;
}

static uint32_t cp_before(const WfELine *l, size_t col)
{
	size_t p;

	if (0 == col) {
		return 0;
	}
	p = col - 1;
	while (p > 0 && 0x80 == ((unsigned char)l->s[p] & 0xC0)) {
		p--;
	}
	return cp_at(l, p);
}

static void word_left(WfEditor *e)
{
	const WfELine *l = &e->text.lines[e->cur.line];

	if (0 == e->cur.col) {
		if (e->cur.line > 0) {
			e->cur.line--;
			e->cur.col = e->text.lines[e->cur.line].len;
		}
		return;
	}
	while (e->cur.col > 0 && 0 == cls(cp_before(l, e->cur.col))) {
		e->cur.col = wf_et_col_prev(l, e->cur.col);
	}
	if (e->cur.col > 0) {
		int k = cls(cp_before(l, e->cur.col));

		while (e->cur.col > 0 && cls(cp_before(l, e->cur.col)) == k) {
			e->cur.col = wf_et_col_prev(l, e->cur.col);
		}
	}
}

static void word_right(WfEditor *e)
{
	const WfELine *l = &e->text.lines[e->cur.line];

	if (e->cur.col >= l->len) {
		if (e->cur.line + 1 < e->text.n) {
			e->cur.line++;
			e->cur.col = 0;
		}
		return;
	}
	{
		int k = cls(cp_at(l, e->cur.col));

		while (e->cur.col < l->len && cls(cp_at(l, e->cur.col)) == k &&
		        0 != k) {
			e->cur.col = wf_et_col_next(l, e->cur.col);
		}
		while (e->cur.col < l->len && 0 == cls(cp_at(l, e->cur.col))) {
			e->cur.col = wf_et_col_next(l, e->cur.col);
		}
	}
}

static size_t first_nonblank(const WfELine *l)
{
	size_t i = 0;

	while (i < l->len && (' ' == l->s[i] || '\t' == l->s[i])) {
		i++;
	}
	return i;
}

/* Cell offset of `col` inside the display row of line `l` that holds it, the
 * row's start and the row count. Without wrapping the whole line is one row. */
typedef struct
{
	size_t *starts;
	size_t rows, row; /* the row holding the column */
} Rows;

static bool rows_of(const WfEditor *e, const WfELine *l, size_t col, Rows *r)
{
	size_t i;

	r->starts = NULL;
	r->rows = 1;
	r->row = 0;
	if (!e->wrap_cells) {
		return true;
	}
	r->rows = wf_et_wrap(l, e->wrap_cells, e->opt.tab_width, &r->starts);
	if (!r->rows) {
		return false;
	}
	for (i = 0; i < r->rows; i++) {
		if (r->starts[i] <= col) {
			r->row = i;
		}
	}
	return true;
}

static size_t row_start(const Rows *r, size_t row)
{
	return r->starts ? r->starts[row] : 0;
}

static size_t row_end(const Rows *r, size_t row, const WfELine *l)
{
	return r->starts && row + 1 < r->rows ? r->starts[row + 1] : l->len;
}

/* Cells from the row start to `col`. */
static size_t row_cells(const WfEditor *e,
        const WfELine *l,
        const Rows *r,
        size_t row,
        size_t col)
{
	WfELine sub;

	sub.s = l->s + row_start(r, row);
	sub.len = col - row_start(r, row);
	sub.cap = sub.len;
	sub.eol = WF_EOL_NONE;
	return wf_et_cells(&sub, sub.len, e->opt.tab_width);
}

static size_t col_in_row(const WfEditor *e,
        const WfELine *l,
        const Rows *r,
        size_t row,
        size_t cells)
{
	WfELine sub;
	size_t start = row_start(r, row), end = row_end(r, row, l), c;

	sub.s = l->s + start;
	sub.len = end - start;
	sub.cap = sub.len;
	sub.eol = WF_EOL_NONE;
	c = wf_et_col_at_cells(&sub, cells, e->opt.tab_width);
	/* a wrapped row must not hand the cursor to the next row's first column */
	if (r->starts && row + 1 < r->rows && c >= sub.len) {
		c = wf_et_col_prev(&sub, sub.len);
	}
	return start + c;
}

/* The display column vertical moves aim for; worked out when first needed,
 * so typing and horizontal moves never pay for measuring a long line. */
static size_t want_cells(WfEditor *e)
{
	if (!e->want_valid) {
		const WfELine *l = &e->text.lines[e->cur.line];
		Rows r;

		e->want_cells = 0;
		if (rows_of(e, l, e->cur.col, &r)) {
			e->want_cells = row_cells(e, l, &r, r.row, e->cur.col);
			free(r.starts);
		}
		e->want_valid = true;
	}
	return e->want_cells;
}

/* One visual row up or down. */
static void move_vertical(WfEditor *e, bool down)
{
	size_t want = want_cells(e);

	const WfELine *l = &e->text.lines[e->cur.line];
	WfPos to = e->cur; /* e->cur changes only when the new place is known */
	Rows r;

	if (!rows_of(e, l, e->cur.col, &r)) {
		return;
	}
	if (!down) {
		if (r.row > 0) {
			to.col = col_in_row(e, l, &r, r.row - 1, want);
		} else if (e->cur.line > 0) {
			Rows p;
			const WfELine *pl = &e->text.lines[e->cur.line - 1];

			if (rows_of(e, pl, pl->len, &p)) {
				to.line--;
				to.col =
				        col_in_row(e, pl, &p, p.rows - 1, want);
				free(p.starts);
			}
		} else {
			to.col = 0;
		}
	} else {
		if (r.row + 1 < r.rows) {
			to.col = col_in_row(e, l, &r, r.row + 1, want);
		} else if (e->cur.line + 1 < e->text.n) {
			Rows p;
			const WfELine *nl = &e->text.lines[e->cur.line + 1];

			if (rows_of(e, nl, 0, &p)) {
				to.line++;
				to.col = col_in_row(e, nl, &p, 0, want);
				free(p.starts);
			}
		} else {
			to.col = l->len;
		}
	}
	e->cur = to;
	free(r.starts);
}

void wf_ed_move(WfEditor *e, WfMove m, bool extend, size_t page)
{
	WfPos from, to;
	bool vertical = WF_MV_UP == m || WF_MV_DOWN == m ||
	        WF_MV_PAGE_UP == m || WF_MV_PAGE_DOWN == m;
	const WfELine *l;

	extend = extend || e->mark;
	if (!extend && wf_ed_has_selection(e)) {
		wf_ed_selection(e, &from, &to);
		if (WF_MV_LEFT == m) {
			e->cur = from;
			wf_ed_clear_selection(e);
			e->want_valid = false;
			return;
		}
		if (WF_MV_RIGHT == m) {
			e->cur = to;
			wf_ed_clear_selection(e);
			e->want_valid = false;
			return;
		}
	}
	if (extend && !e->has_anchor) {
		e->anchor = e->cur;
		e->has_anchor = true;
	}
	if (!extend) {
		wf_ed_clear_selection(e);
	}

	l = &e->text.lines[e->cur.line];
	switch (m) {
	case WF_MV_LEFT:
		if (e->cur.col > 0) {
			e->cur.col = wf_et_col_prev(l, e->cur.col);
		} else if (e->cur.line > 0) {
			e->cur.line--;
			e->cur.col = e->text.lines[e->cur.line].len;
		}
		break;
	case WF_MV_RIGHT:
		if (e->cur.col < l->len) {
			e->cur.col = wf_et_col_next(l, e->cur.col);
		} else if (e->cur.line + 1 < e->text.n) {
			e->cur.line++;
			e->cur.col = 0;
		}
		break;
	case WF_MV_UP:        move_vertical(e, false); break;
	case WF_MV_DOWN:      move_vertical(e, true); break;
	case WF_MV_PAGE_UP:
	case WF_MV_PAGE_DOWN: {
		size_t k = page ? page : 1, i;

		for (i = 0; i < k; i++) {
			move_vertical(e, WF_MV_PAGE_DOWN == m);
		}
		break;
	}
	case WF_MV_LINE_START: {
		size_t first = first_nonblank(l);

		/* a second press goes to the true start */
		e->cur.col = e->cur.col == first || 0 == e->cur.col ? 0 : first;
		if (0 == e->cur.col && first && e->cur.col != first &&
		        l->len == first) {
			e->cur.col = 0;
		}
		break;
	}
	case WF_MV_LINE_END:   e->cur.col = l->len; break;
	case WF_MV_DOC_START:  e->cur.line = e->cur.col = 0; break;
	case WF_MV_DOC_END:    e->cur = wf_et_end(&e->text); break;
	case WF_MV_WORD_LEFT:  word_left(e); break;
	case WF_MV_WORD_RIGHT: word_right(e); break;
	}
	if (!vertical) {
		e->want_valid = false;
	}
	if (e->has_anchor && !e->mark && 0 == wf_pos_cmp(e->anchor, e->cur)) {
		e->has_anchor = false;
	}
}

void wf_ed_goto(WfEditor *e, size_t line, size_t column)
{
	const WfELine *l;
	size_t c = 0, i = 1;

	if (line < 1) {
		line = 1;
	}
	if (line > e->text.n) {
		line = e->text.n;
	}
	e->cur.line = line - 1;
	l = &e->text.lines[e->cur.line];
	if (column < 1) {
		column = 1;
	}
	while (i < column && c < l->len) {
		c = wf_et_col_next(l, c);
		i++;
	}
	e->cur.col = c;
	wf_ed_clear_selection(e);
	e->want_valid = false;
}

size_t wf_ed_cursor_col(const WfEditor *e)
{
	return wf_et_cells(&e->text.lines[e->cur.line], e->cur.col,
	               e->opt.tab_width) +
	        1;
}

WfStatus wf_ed_type(WfEditor *e, uint32_t cp)
{
	char buf[4];
	size_t n;

	if (cp < 0x20 || 0x7F == cp || (cp >= 0x80 && cp < 0xA0) ||
	        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
		return WF_E_INVAL;
	}
	n = wf_utf8_encode(cp, buf);
	return replace_selection(e, buf, n, false, WF_UK_TYPE);
}

/* Control characters never enter the buffer from a paste. */
static WfStatus clean_text(
        const char *s, size_t n, char **out, size_t *out_n, size_t *dropped)
{
	char *buf = malloc(n ? n : 1);
	size_t i = 0, k = 0;

	if (!buf) {
		return WF_E_NOMEM;
	}
	*dropped = 0;
	while (i < n) {
		uint32_t cp;
		size_t len = wf_utf8_decode(s + i, n - i, &cp);

		if (!len) {
			break;
		}
		if (WF_CP_REPLACEMENT == cp && 1 == len) { /* malformed */
			(*dropped)++;
		} else if ((cp < 0x20 && '\t' != cp && '\n' != cp &&
		                   '\r' != cp) ||
		        0x7F == cp || (cp >= 0x80 && cp < 0xA0)) {
			(*dropped)++;
		} else {
			memcpy(buf + k, s + i, len);
			k += len;
		}
		i += len;
	}
	*out = buf;
	*out_n = k;
	return WF_OK;
}

WfStatus wf_ed_insert_text(WfEditor *e, const char *utf8, size_t n)
{
	char *clean;
	size_t clean_n, dropped;
	WfStatus st;

	if (e->read_only) {
		return WF_E_ACCESS;
	}
	if (n > PASTE_MAX) {
		return WF_E_RANGE;
	}
	st = clean_text(utf8, n, &clean, &clean_n, &dropped);
	if (WF_OK != st) {
		return st;
	}
	st = replace_selection(e, clean, clean_n, true, WF_UK_OTHER);
	free(clean);
	if (WF_OK == st && dropped) {
		(void)wf_msgf(e->msg, sizeof e->msg,
		        wf_msg_count(MSG_ED_CONTROL_REMOVED_ONE,
		                MSG_ED_CONTROL_REMOVED_OTHER,
		                (long long)dropped),
		        &WF_ARG_N(dropped), 1);
	}
	return st;
}

WfStatus wf_ed_restore_text(WfEditor *e, const char *utf8, size_t n)
{
	wf_ed_select_all(e);
	return replace_selection(e, utf8, n, false, WF_UK_OTHER);
}

static void indent_text(const WfEditor *e, char *out, size_t cap, size_t *n)
{
	if (e->opt.use_spaces) {
		size_t k = e->opt.tab_width < cap ? e->opt.tab_width : cap;

		memset(out, ' ', k);
		*n = k;
	} else {
		out[0] = '\t';
		*n = 1;
	}
}

WfStatus wf_ed_newline(WfEditor *e)
{
	const WfELine *l;
	char *text;
	size_t n = 1, ind = 0, cap, extra_n = 0;
	char extra[16];
	bool brace = false, close_follows = false;
	WfStatus st;
	WfPos from, to;

	if (e->read_only) {
		return WF_E_ACCESS;
	}
	from = to = e->cur;
	if (wf_ed_has_selection(e)) {
		wf_ed_selection(e, &from, &to);
	}
	l = &e->text.lines[from.line];
	if (e->opt.auto_indent) {
		ind = first_nonblank(l);
		if (ind > from.col) {
			ind = from.col;
		}
		{
			size_t p = from.col;

			while (p > 0 &&
			        (' ' == l->s[p - 1] || '\t' == l->s[p - 1])) {
				p--;
			}
			brace = p > 0 && '{' == l->s[p - 1];
			close_follows = from.col < l->len &&
			        '}' == l->s[from.col] && brace;
		}
	}
	indent_text(e, extra, sizeof extra, &extra_n);
	cap = 2 + ind + extra_n + 2 + ind + 1;
	text = malloc(cap);
	if (!text) {
		return WF_E_NOMEM;
	}
	text[0] = '\n';
	memcpy(text + 1, l->s, ind);
	n = 1 + ind;
	if (brace) {
		memcpy(text + n, extra, extra_n);
		n += extra_n;
	}
	if (close_follows) {
		size_t keep = n;

		text[n++] = '\n';
		memcpy(text + n, l->s, ind);
		n += ind;
		st = replace_selection(e, text, n, true, WF_UK_OTHER);
		if (WF_OK == st) { /* the cursor goes to the middle line */
			e->cur.line--;
			e->cur.col = keep - 1;
		}
		free(text);
		return st;
	}
	st = replace_selection(e, text, n, true, WF_UK_OTHER);
	free(text);
	return st;
}

WfStatus wf_ed_backspace(WfEditor *e)
{
	WfPos from, to = e->cur;
	WfStatus st;

	if (e->read_only) {
		return WF_E_ACCESS;
	}
	if (wf_ed_has_selection(e)) {
		wf_ed_selection(e, &from, &to);
		st = replace(e, from, to, NULL, 0, false, WF_UK_OTHER);
	} else if (e->cur.col > 0) {
		const WfELine *l = &e->text.lines[e->cur.line];

		from.line = e->cur.line;
		from.col = wf_et_col_prev(l, e->cur.col);
		st = replace(e, from, to, NULL, 0, false, WF_UK_BACK);
	} else if (e->cur.line > 0) {
		from.line = e->cur.line - 1;
		from.col = e->text.lines[from.line].len;
		st = replace(e, from, to, NULL, 0, false, WF_UK_BACK);
	} else {
		return WF_OK;
	}
	if (WF_OK == st) {
		wf_ed_clear_selection(e);
		e->want_valid = false;
	}
	return st;
}

WfStatus wf_ed_delete(WfEditor *e)
{
	WfPos from = e->cur, to;
	const WfELine *l = &e->text.lines[e->cur.line];
	WfStatus st;

	if (e->read_only) {
		return WF_E_ACCESS;
	}
	if (wf_ed_has_selection(e)) {
		wf_ed_selection(e, &from, &to);
		st = replace(e, from, to, NULL, 0, false, WF_UK_OTHER);
	} else if (e->cur.col < l->len) {
		to.line = e->cur.line;
		to.col = wf_et_col_next(l, e->cur.col);
		st = replace(e, from, to, NULL, 0, false, WF_UK_DEL);
	} else if (e->cur.line + 1 < e->text.n) {
		to.line = e->cur.line + 1;
		to.col = 0;
		st = replace(e, from, to, NULL, 0, false, WF_UK_DEL);
	} else {
		return WF_OK;
	}
	if (WF_OK == st) {
		wf_ed_clear_selection(e);
	}
	return st;
}

/* The lines an operation covers: the selection's, without a last line that the
 * selection only touches at its very start. */
static void line_span(const WfEditor *e, size_t *first, size_t *last)
{
	WfPos a, b;

	if (wf_ed_has_selection(e)) {
		wf_ed_selection(e, &a, &b);
		*first = a.line;
		*last = b.line;
		if (0 == b.col && b.line > a.line) {
			(*last)--;
		}
	} else {
		*first = *last = e->cur.line;
	}
}

WfStatus wf_ed_delete_line(WfEditor *e)
{
	size_t a, b;
	WfPos from, to;
	WfStatus st;

	if (e->read_only) {
		return WF_E_ACCESS;
	}
	line_span(e, &a, &b);
	from.line = a;
	from.col = 0;
	if (b + 1 < e->text.n) {
		to.line = b + 1;
		to.col = 0;
	} else if (0 == e->text.lines[b].len && a == b) {
		return WF_OK; /* the empty line at the end of the file */
	} else {
		to.line = b;
		to.col = e->text.lines[b].len;
		if (a > 0) { /* take the break before it, keeping the ending */
			from.line = a - 1;
			from.col = e->text.lines[a - 1].len;
		}
	}
	st = replace(e, from, to, NULL, 0, false, WF_UK_OTHER);
	if (WF_OK == st) {
		const WfELine *l;

		wf_ed_clear_selection(e);
		l = &e->text.lines[e->cur.line];
		e->cur.col = e->cur.col > l->len ? l->len : e->cur.col;
	}
	return st;
}

/* Shifts the cursor and anchor columns on `line` by `delta`. */
static void shift_cols(WfEditor *e, size_t line, long delta)
{
	WfPos *p[2] = {&e->cur, &e->anchor};
	int i;

	for (i = 0; i < 2; i++) {
		if (p[i]->line != line) {
			continue;
		}
		if (delta < 0 && (size_t)-delta > p[i]->col) {
			p[i]->col = 0;
		} else {
			p[i]->col = (size_t)((long)p[i]->col + delta);
		}
	}
}

WfStatus wf_ed_tab(WfEditor *e, bool back)
{
	char ind[16];
	size_t ind_n, a, b, i;
	WfStatus st = WF_OK;

	if (e->read_only) {
		return WF_E_ACCESS;
	}
	indent_text(e, ind, sizeof ind, &ind_n);
	if (!back &&
	        !(wf_ed_has_selection(e) && e->cur.line != e->anchor.line)) {
		/* a tab stop, or the spaces up to it */
		char buf[16];
		size_t n = 1;

		if (e->opt.use_spaces) {
			WfPos from, to;
			size_t at;

			wf_ed_selection(e, &from, &to);
			if (!wf_ed_has_selection(e)) {
				from = e->cur;
			}
			at = wf_et_cells(&e->text.lines[from.line], from.col,
			        e->opt.tab_width);
			n = e->opt.tab_width - at % e->opt.tab_width;
			memset(buf, ' ', n);
		} else {
			buf[0] = '\t';
		}
		return replace_selection(e, buf, n, false, WF_UK_OTHER);
	}
	line_span(e, &a, &b);
	wf_un_group_begin(&e->undo);
	for (i = a; i <= b && WF_OK == st; i++) {
		const WfELine *l = &e->text.lines[i];
		WfPos at = {i, 0}, to = {i, 0};
		WfPos before = e->cur;

		if (!back) {
			if (0 == l->len) {
				continue;
			}
			st = replace(e, at, to, ind, ind_n, false, WF_UK_OTHER);
			if (WF_OK == st) {
				e->cur = before;
				shift_cols(e, i, (long)ind_n);
			}
		} else {
			size_t k = 0;

			if (l->len && '\t' == l->s[0]) {
				k = 1;
			} else {
				while (k < l->len && k < e->opt.tab_width &&
				        ' ' == l->s[k]) {
					k++;
				}
			}
			if (!k) {
				continue;
			}
			to.col = k;
			st = replace(e, at, to, NULL, 0, false, WF_UK_OTHER);
			if (WF_OK == st) {
				e->cur = before;
				shift_cols(e, i, -(long)k);
			}
		}
	}
	wf_un_group_end(&e->undo);
	return st;
}

WfStatus wf_ed_toggle_comment(WfEditor *e, const char *prefix)
{
	size_t a, b, i, plen, indent = (size_t)-1;
	bool all_commented = true, any = false;
	WfStatus st = WF_OK;

	if (e->read_only) {
		return WF_E_ACCESS;
	}
	if (!prefix || !*prefix) {
		return WF_E_INVAL;
	}
	plen = strlen(prefix);
	line_span(e, &a, &b);
	for (i = a; i <= b; i++) {
		const WfELine *l = &e->text.lines[i];
		size_t ind = first_nonblank(l);

		if (ind == l->len) {
			continue; /* blank lines are left alone */
		}
		any = true;
		if (ind < indent) {
			indent = ind;
		}
		if (l->len - ind < plen || memcmp(l->s + ind, prefix, plen)) {
			all_commented = false;
		}
	}
	if (!any) {
		return WF_OK;
	}
	wf_un_group_begin(&e->undo);
	for (i = a; i <= b && WF_OK == st; i++) {
		const WfELine *l = &e->text.lines[i];
		size_t ind = first_nonblank(l);
		WfPos before = e->cur;

		if (ind == l->len) {
			continue;
		}
		if (all_commented) {
			WfPos from = {i, ind}, to = {i, ind + plen};
			size_t removed = plen;

			if (to.col < l->len && ' ' == l->s[to.col]) {
				to.col++;
				removed++;
			}
			st = replace(e, from, to, NULL, 0, false, WF_UK_OTHER);
			if (WF_OK == st) {
				e->cur = before;
				shift_cols(e, i, -(long)removed);
			}
		} else {
			char *t = malloc(plen + 1);
			WfPos at = {i, indent};

			if (!t) {
				st = WF_E_NOMEM;
				break;
			}
			memcpy(t, prefix, plen);
			t[plen] = ' ';
			st = replace(
			        e, at, at, t, plen + 1, false, WF_UK_OTHER);
			free(t);
			if (WF_OK == st) {
				e->cur = before;
				shift_cols(e, i, (long)plen + 1);
			}
		}
	}
	wf_un_group_end(&e->undo);
	return st;
}

WfStatus wf_ed_set_clip(WfEditor *e, const char *utf8, size_t n, bool lines)
{
	char *copy = malloc(n ? n : 1);

	if (!copy) {
		return WF_E_NOMEM;
	}
	memcpy(copy, utf8, n);
	free(e->clip);
	e->clip = copy;
	e->clip_n = n;
	e->clip_line = lines;
	return WF_OK;
}

/* The text a copy takes: the selection, or the current line with its ending. */
static WfStatus take(WfEditor *e, bool *whole_line, WfPos *from, WfPos *to)
{
	if (wf_ed_has_selection(e)) {
		wf_ed_selection(e, from, to);
		*whole_line = false;
		return WF_OK;
	}
	from->line = e->cur.line;
	from->col = 0;
	if (e->cur.line + 1 < e->text.n) {
		to->line = e->cur.line + 1;
		to->col = 0;
	} else {
		to->line = e->cur.line;
		to->col = e->text.lines[e->cur.line].len;
		if (0 == to->col) {
			return WF_E_NOENT; /* nothing on the empty last line */
		}
	}
	*whole_line = true;
	return WF_OK;
}

WfStatus wf_ed_copy(WfEditor *e)
{
	WfPos from, to;
	bool whole;
	char *text;
	size_t n;
	WfStatus st = take(e, &whole, &from, &to);

	if (WF_OK != st) {
		say(e, wf_msg(MSG_ED_NOTHING_TO_COPY));
		return WF_E_NOENT == st ? WF_OK : st;
	}
	st = wf_et_read(&e->text, from, to, &text, &n);
	if (WF_OK != st) {
		return st;
	}
	if (whole && to.line == from.line) { /* no ending: give it one */
		const char *eol = wf_eol_bytes(e->text.default_eol);
		char *grown = realloc(text, n + 2);

		if (!grown) {
			free(text);
			return WF_E_NOMEM;
		}
		text = grown;
		memcpy(text + n, eol, wf_eol_len(e->text.default_eol));
		n += wf_eol_len(e->text.default_eol);
	}
	st = wf_ed_set_clip(e, text, n, whole);
	free(text);
	if (WF_OK == st) {
		say(e, wf_msg(whole ? MSG_ED_COPIED_LINE : MSG_ED_COPIED));
	}
	return st;
}

WfStatus wf_ed_cut(WfEditor *e)
{
	WfStatus st;

	if (e->read_only) {
		return WF_E_ACCESS;
	}
	st = wf_ed_copy(e);
	if (WF_OK != st) {
		return st;
	}
	if (wf_ed_has_selection(e)) {
		return replace_selection(e, NULL, 0, false, WF_UK_OTHER);
	}
	return wf_ed_delete_line(e);
}

WfStatus wf_ed_paste(WfEditor *e)
{
	WfStatus st;

	if (e->read_only) {
		return WF_E_ACCESS;
	}
	if (!e->clip_n) {
		say(e, wf_msg(MSG_ED_CLIPBOARD_EMPTY));
		return WF_OK;
	}
	if (e->clip_line && !wf_ed_has_selection(e)) {
		WfPos at = {e->cur.line, 0};
		size_t keep_col = e->cur.col, breaks = 0, i;

		for (i = 0; i < e->clip_n; i++) {
			if ('\n' == e->clip[i] ||
			        ('\r' == e->clip[i] &&
			                !(i + 1 < e->clip_n &&
			                        '\n' == e->clip[i + 1]))) {
				breaks++;
			}
		}
		st = replace(e, at, at, e->clip, e->clip_n, true, WF_UK_OTHER);
		if (WF_OK == st) {
			e->cur.line = at.line + breaks;
			e->cur.col = keep_col;
			if (e->cur.col > e->text.lines[e->cur.line].len) {
				e->cur.col = e->text.lines[e->cur.line].len;
			}
		}
		return st;
	}
	return wf_ed_insert_text(e, e->clip, e->clip_n);
}

static WfStatus apply_record(WfEditor *e, const WfURec *r, bool forward)
{
	const char *gone = forward ? r->del : r->ins;
	size_t gone_n = forward ? r->del_n : r->ins_n;
	const char *put = forward ? r->ins : r->del;
	size_t put_n = forward ? r->ins_n : r->del_n;
	WfPos end = wf_et_advance(r->at, gone, gone_n);
	char *removed = NULL;
	WfStatus st;
	Rollback rb;

	/* the copy is taken before the first change: putting the removed text back
	 * after a failed insert must not need memory */
	st = rollback_begin(&rb, e, r->at, end, put, put_n);
	if (WF_OK != st) {
		return st;
	}
	st = wf_et_delete(&e->text, r->at, end, &removed, NULL);
	if (WF_OK != st) {
		rollback_end(&rb);
		return st;
	}
	if (put_n) {
		st = wf_et_insert(&e->text, r->at, put, put_n, false, NULL);
		if (WF_OK != st) {
			/* the insert changes nothing when it fails */
			rollback_apply(&rb, e, r->at, r->at, r->at.line,
			        removed, gone_n);
			rollback_end(&rb);
			free(removed);
			return st;
		}
	}
	rollback_end(&rb);
	free(removed);
	if (r->at.line < e->dirty_from) {
		e->dirty_from = r->at.line;
	}
	return WF_OK;
}

WfStatus wf_ed_undo(WfEditor *e)
{
	size_t first, last, i;
	WfStatus st = WF_OK;

	if (e->read_only) {
		return WF_E_ACCESS;
	}
	if (!wf_un_undo_span(&e->undo, &first, &last)) {
		say(e, wf_msg(MSG_ED_NOTHING_TO_UNDO));
		return WF_OK;
	}
	for (i = last + 1; i-- > first;) {
		st = apply_record(e, &e->undo.rec[i], false);
		if (WF_OK != st) {
			break;
		}
		e->undo.pos = i;
		e->cur = e->undo.rec[i].before;
	}
	wf_ed_clear_selection(e);
	if (wf_et_valid(&e->text, e->cur)) {
		e->want_valid = false;
	}
	if (WF_OK == st) {
		say(e, wf_msg(MSG_ED_UNDONE));
	}
	return st;
}

WfStatus wf_ed_redo(WfEditor *e)
{
	size_t first, last, i;
	WfStatus st = WF_OK;

	if (e->read_only) {
		return WF_E_ACCESS;
	}
	if (!wf_un_redo_span(&e->undo, &first, &last)) {
		say(e, wf_msg(MSG_ED_NOTHING_TO_REDO));
		return WF_OK;
	}
	for (i = first; i <= last; i++) {
		st = apply_record(e, &e->undo.rec[i], true);
		if (WF_OK != st) {
			break;
		}
		e->undo.pos = i + 1;
		e->cur = e->undo.rec[i].after;
	}
	wf_ed_clear_selection(e);
	if (wf_et_valid(&e->text, e->cur)) {
		e->want_valid = false;
	}
	if (WF_OK == st) {
		say(e, wf_msg(MSG_ED_REDONE));
	}
	return st;
}

bool wf_ed_find(WfEditor *e, const WfSearch *s, bool forward, bool *wrapped)
{
	WfMatch m;
	WfPos from = e->cur;

	if (wf_ed_has_selection(e)) {
		WfPos a, b;

		wf_ed_selection(e, &a, &b);
		from = forward ? b : a;
	}
	if (!wf_es_find(&e->text, s, from, forward, true, &m, wrapped)) {
		return false;
	}
	e->anchor = m.start;
	e->cur = m.end;
	e->has_anchor = true;
	e->mark = false;
	e->want_valid = false;
	return true;
}

WfStatus wf_ed_replace_selection(
        WfEditor *e, const WfSearch *s, const char *repl, size_t n)
{
	WfMatch m;
	WfPos a, b;
	WfStatus st;

	if (e->read_only) {
		return WF_E_ACCESS;
	}
	if (!wf_ed_has_selection(e)) {
		return WF_E_NOENT;
	}
	wf_ed_selection(e, &a, &b);
	if (!wf_es_find(&e->text, s, a, true, false, &m, NULL) ||
	        0 != wf_pos_cmp(m.start, a) || 0 != wf_pos_cmp(m.end, b)) {
		return WF_E_NOENT;
	}
	st = replace(e, a, b, repl, n, false, WF_UK_OTHER);
	if (WF_OK == st) {
		wf_ed_clear_selection(e);
	}
	return st;
}

/* The byte that stands before the text after a match at `at` once the match has
 * been replaced: the last byte of the replacement, or, if that is empty, the
 * byte that was before the match (`left` when the match began where the walk
 * started, `col`). */
static int left_after(const WfELine *l,
        size_t col,
        size_t at,
        int left,
        const char *repl,
        size_t n)
{
	if (n) {
		return (unsigned char)repl[n - 1];
	}
	return at > col ? (unsigned char)l->s[at - 1] : left;
}

/*
 * Replace all: the matches in the original line are the ones that would be found
 * one after the other in the changing text, so a line that has any is made again
 * in one piece, and moved once, not once for every match. A whole-word test looks
 * at the byte before the match, which by then is the one the replacement put
 * there, and at the byte after it, which is still the original (`left`). The
 * undo record is one for the line. A replacement with line breaks makes the
 * lines it makes; the walk goes on after them.
 */
static WfStatus replace_all_by_line(WfEditor *e,
        const WfSearch *s,
        const char *repl,
        size_t n,
        size_t *count)
{
	WfStatus st = WF_OK;

	for (size_t line = 0; line < e->text.n && WF_OK == st; line++) {
		const WfELine *l = &e->text.lines[line];
		size_t at, col, hits = 0, len = l->len, size, from = 0, put = 0,
		                first = 0;
		int left = -1;
		char *out;

		for (col = 0; wf_es_next_match_after(l, s, col, left, &at);
		        col = at + s->pat_n) {
			first = hits ? first : at;
			hits++;
			left = left_after(l, col, at, left, repl, n);
		}
		if (!hits) {
			continue;
		}
		if (1 ==
		        hits) { /* the usual line: the one match, nothing to rebuild */
			e->cur = (WfPos){line, first};
			st = replace(e, (WfPos){line, first},
			        (WfPos){line, first + s->pat_n}, repl, n, false,
			        WF_UK_OTHER);
			if (WF_OK == st) {
				(*count)++;
				line = e->cur.line; /* the rest of that line has no match */
			}
			continue;
		}
		if (n > s->pat_n && hits > (SIZE_MAX - len) / (n - s->pat_n)) {
			return WF_E_RANGE;
		}
		size = len - hits * s->pat_n + hits * n;
		out = malloc(size ? size : 1);
		if (!out) {
			return WF_E_NOMEM;
		}
		left = -1;
		for (col = 0; wf_es_next_match_after(l, s, col, left, &at);
		        col = at + s->pat_n) {
			left = left_after(l, col, at, left, repl, n);
			memcpy(out + put, l->s + from, at - from);
			put += at - from;
			memcpy(out + put, repl, n);
			put += n;
			from = at + s->pat_n;
		}
		memcpy(out + put, l->s + from, len - from);
		e->cur = (WfPos){line, 0};
		st = replace(e, (WfPos){line, 0}, (WfPos){line, len}, out, size,
		        false, WF_UK_OTHER);
		free(out);
		if (WF_OK == st) {
			*count += hits;
			line = e->cur.line; /* just after the text made; the next line is next */
		}
	}
	return st;
}

WfStatus wf_ed_replace_all(WfEditor *e,
        const WfSearch *s,
        const char *repl,
        size_t n,
        size_t *count)
{
	WfPos keep = e->cur;
	WfStatus st;

	*count = 0;
	if (e->read_only) {
		return WF_E_ACCESS;
	}
	if (!wf_es_ok(s)) {
		return WF_E_INVAL;
	}
	wf_un_group_begin(&e->undo);
	st = replace_all_by_line(e, s, repl, n, count);
	wf_un_group_end(&e->undo);
	wf_ed_clear_selection(e);
	if (wf_et_valid(&e->text, keep)) {
		/* the bytes before it may now be a longer character: stay at its start */
		const WfELine *l = &e->text.lines[keep.line];

		while (keep.col > 0 && keep.col < l->len &&
		        0x80 == ((unsigned char)l->s[keep.col] & 0xC0)) {
			keep.col--;
		}
		e->cur = keep;
	}
	return st;
}

bool wf_ed_match_bracket(const WfEditor *e, WfPos *open, WfPos *close)
{
	static const char PAIRS[] = "()[]{}";
	const WfELine *l = &e->text.lines[e->cur.line];
	size_t col = e->cur.col, k, scanned = 0;
	char c = 0;
	WfPos here;
	bool forward;
	long depth = 0;

	if (col < l->len && strchr(PAIRS, l->s[col]) && l->s[col]) {
		c = l->s[col];
	} else if (col > 0 && strchr(PAIRS, l->s[col - 1]) && l->s[col - 1]) {
		c = l->s[col - 1];
		col--;
	}
	if (!c) {
		return false;
	}
	here.line = e->cur.line;
	here.col = col;
	k = (size_t)(strchr(PAIRS, c) - PAIRS);
	forward = k % 2 == 0;
	{
		char other = PAIRS[forward ? k + 1 : k - 1];
		size_t line = here.line, i = forward ? col : col + 1;

		for (;;) {
			const WfELine *cl = &e->text.lines[line];

			if (forward) {
				for (; i < cl->len; i++) {
					if (++scanned > BRACKET_SCAN_MAX) {
						return false;
					}
					if (cl->s[i] == c) {
						depth++;
					} else if (cl->s[i] == other &&
					        --depth == 0) {
						goto found;
					}
				}
				if (++line >= e->text.n ||
				        ++scanned > BRACKET_SCAN_MAX) {
					return false; /* a line break costs a step: empty lines too */
				}
				i = 0;
			} else {
				while (i > 0) {
					i--;
					if (++scanned > BRACKET_SCAN_MAX) {
						return false;
					}
					if (cl->s[i] == c) {
						depth++;
					} else if (cl->s[i] == other &&
					        --depth == 0) {
						goto found;
					}
				}
				if (0 == line || ++scanned > BRACKET_SCAN_MAX) {
					return false;
				}
				line--;
				i = e->text.lines[line].len;
			}
			continue;
		found:
			if (forward) {
				*open = here;
				close->line = line;
				close->col = i;
			} else {
				open->line = line;
				open->col = i;
				*close = here;
			}
			return true;
		}
	}
}

/* A word of the text seen during one scan. `w` is borrowed from the line it was
 * found in: it is valid only until the text changes, which it cannot do while
 * nth_candidate runs. Only the word that is handed out is copied. */
typedef struct
{
	const char *w;
	size_t n;
} Word;

static bool word_byte(unsigned char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
	        (c >= 'A' && c <= 'Z') || '_' == c || c >= 0x80;
}

static bool listed_before(
        const Word *list, size_t listed, const char *w, size_t n)
{
	for (size_t q = 0; q < listed; q++) {
		if (list[q].n == n && !memcmp(list[q].w, w, n)) {
			return true;
		}
	}
	return false;
}

/* The k-th distinct word starting with `prefix` (nearest first, backward then
 * forward from the cursor) as a copy the caller frees. WF_OK: found; WF_E_NOENT:
 * there are fewer words; WF_E_NOMEM: no memory (nothing is left allocated). The
 * scan stops as soon as that word is found: the later words are not looked at. */
static WfStatus nth_candidate(const WfEditor *e,
        const char *prefix,
        size_t plen,
        size_t k,
        char **out,
        size_t *out_n)
{
	Word *list = NULL; /* the one owner of the scan's memory */
	size_t listed = 0, cap = 0, step;
	WfStatus st = WF_E_NOENT;

	for (step = 0; step < 2 * COMPLETE_SCAN_LINES && WF_E_NOENT == st;
	        step++) {
		long delta =
		        step % 2 ? (long)(step / 2 + 1) : -(long)(step / 2);
		long line = (long)e->cur.line + delta;
		const WfELine *l;
		size_t i = 0;

		if (line < 0 || (size_t)line >= e->text.n) {
			continue;
		}
		l = &e->text.lines[line];
		while (i < l->len && WF_E_NOENT == st) {
			size_t s = i, j;

			if (!word_byte((unsigned char)l->s[i])) {
				i++;
				continue;
			}
			while (i < l->len &&
			        word_byte((unsigned char)l->s[i])) {
				i++;
			}
			j = i - s;
			/* skip the word being completed itself: it starts where the
			 * completion started and ends at the cursor, whatever it has
			 * been completed to so far */
			if ((size_t)line == e->cur.line && s == e->comp_start &&
			        s + j == e->cur.col) {
				continue;
			}
			if (j <= plen || memcmp(l->s + s, prefix, plen) ||
			        listed_before(list, listed, l->s + s, j)) {
				continue;
			}
			if (listed == cap) {
				size_t nc = cap ? cap * 2 : 8;
				/* a failed realloc leaves `list` as it was */
				Word *nl = wf_et_realloc(list, nc * sizeof *nl);

				if (!nl) {
					st = WF_E_NOMEM;
					goto out_free_list;
				}
				list = nl;
				cap = nc;
			}
			list[listed].w = l->s + s;
			list[listed].n = j;
			if (listed++ == k) {
				*out = wf_et_malloc(j);
				if (!*out) {
					st = WF_E_NOMEM;
					goto out_free_list;
				}
				memcpy(*out, l->s + s, j);
				*out_n = j;
				st = WF_OK;
			}
		}
	}
out_free_list:
	free(list);
	return st;
}

WfStatus wf_ed_complete_word(WfEditor *e)
{
	const WfELine *l = &e->text.lines[e->cur.line];
	char *cand = NULL;
	size_t cand_n = 0, start;
	WfPos from, to;
	WfStatus st;
	bool continuing = e->comp_prefix && e->comp_gen == e->text.gen &&
	        0 == wf_pos_cmp(e->cur, e->comp_end);

	if (e->read_only) {
		return WF_E_ACCESS;
	}
	if (continuing) {
		start = e->comp_start;
		e->comp_index++;
	} else {
		start = e->cur.col;
		while (start > 0 && word_byte((unsigned char)l->s[start - 1])) {
			start--;
		}
		if (start == e->cur.col) {
			say(e, wf_msg(MSG_ED_NO_WORD));
			return WF_OK;
		}
		free(e->comp_prefix);
		e->comp_len = e->cur.col - start;
		e->comp_prefix = malloc(e->comp_len);
		if (!e->comp_prefix) {
			return WF_E_NOMEM;
		}
		memcpy(e->comp_prefix, l->s + start, e->comp_len);
		e->comp_index = 0;
		e->comp_start = start;
	}
	st = nth_candidate(
	        e, e->comp_prefix, e->comp_len, e->comp_index, &cand, &cand_n);
	if (WF_E_NOENT == st &&
	        e->comp_index) { /* past the last: round to the first */
		st = nth_candidate(
		        e, e->comp_prefix, e->comp_len, 0, &cand, &cand_n);
		e->comp_index = 0;
	}
	if (WF_E_NOENT == st) {
		say(e, wf_msg(MSG_ED_NO_COMPLETION));
		return WF_OK;
	}
	if (WF_OK != st) {
		return st;
	}
	from.line = to.line = e->cur.line;
	from.col = start;
	to.col = e->cur.col;
	st = replace(e, from, to, cand, cand_n, false, WF_UK_OTHER);
	free(cand);
	if (WF_OK == st) {
		wf_ed_clear_selection(e);
		e->comp_end = e->cur;
		e->comp_gen = e->text.gen;
	}
	return st;
}

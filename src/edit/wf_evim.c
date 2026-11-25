/*
 * Vim's Normal mode beyond single keys. See wf_evim.h.
 *
 * An operator waits for a motion; the motion is measured by moving the real
 * cursor and putting it back, then the range goes through the ordinary Copy
 * and Cut of the session, so undo, the clipboard and the read-only checks are
 * the ones every other edit has.
 */
#include <stdlib.h>
#include <string.h>

#include "../text/wf_text.h"
#include "wf_esession_internal.h"
#include "wf_evim.h"

void wf_vim_init(WfVim *v)
{
	memset(v, 0, sizeof *v);
	v->reg = -1;
	v->recording = -1;
}

void wf_vim_free(WfVim *v)
{
	for (int i = 0; i < WF_VIM_REGS; i++) {
		free(v->regs[i].text);
		free(v->macro[i].ev);
	}
	free(v->dot);
	free(v->cand);
	wf_vim_init(v);
}

bool wf_vim_extends(const WfVim *v)
{
	return v->visual;
}

void wf_vim_cancel(WfVim *v)
{
	v->wait = 0;
	v->op = 0;
	v->count = v->count2 = 0;
	v->visual = v->vline = false;
	v->reg = -1;
	v->append = v->reg_fresh = false;
}

bool wf_vim_is_cmd(WfEdCmd cmd)
{
	return cmd >= WF_CMD_VIM_OP_DELETE && cmd <= WF_CMD_VIM_DOT;
}

static size_t last_line(const WfEditor *ed)
{
	return ed->text.n ? ed->text.n - 1 : 0;
}

static bool pos_before(WfPos a, WfPos b)
{
	return a.line < b.line || (a.line == b.line && a.col < b.col);
}

/* Where the cursor lands after `n` steps of `mv`; the cursor itself is put back. */
static WfPos motion_target(WfEditor *ed, WfMove mv, unsigned n)
{
	WfPos cur = ed->cur, anchor = ed->anchor, t;
	bool has = ed->has_anchor, mark = ed->mark, want = ed->want_valid;
	size_t cells = ed->want_cells;

	for (unsigned i = 0; i < n; i++) {
		wf_ed_move(ed, mv, false, 0);
	}
	t = ed->cur;
	ed->cur = cur;
	ed->anchor = anchor;
	ed->has_anchor = has;
	ed->mark = mark;
	ed->want_valid = want;
	ed->want_cells = cells;
	return t;
}

/* The lines l1..l2 as the clip holds them: each one with its break. */
static char *lines_text(const WfEditor *ed, size_t l1, size_t l2, size_t *n)
{
	size_t total = 0, at = 0;
	char *out;

	for (size_t i = l1; i <= l2; i++) {
		total += ed->text.lines[i].len + 1;
	}
	out = malloc(total ? total : 1);
	if (!out) {
		*n = 0;
		return NULL;
	}
	for (size_t i = l1; i <= l2; i++) {
		memcpy(out + at, ed->text.lines[i].s, ed->text.lines[i].len);
		at += ed->text.lines[i].len;
		out[at++] = '\n';
	}
	*n = total;
	return out;
}

static void keep_register(WfEditSession *s, const WfEditor *ed)
{
	WfVim *v = &s->vim;

	if (v->reg >= 0 && ed->clip) {
		size_t old = v->append && v->regs[v->reg].text
		        ? v->regs[v->reg].n
		        : 0;
		char *copy = malloc(old + ed->clip_n ? old + ed->clip_n : 1);

		if (copy) {
			if (old) { /* "A: the new text follows what the register holds */
				memcpy(copy, v->regs[v->reg].text, old);
			}
			memcpy(copy + old, ed->clip, ed->clip_n);
			free(v->regs[v->reg].text);
			v->regs[v->reg].text = copy;
			v->regs[v->reg].n = old + ed->clip_n;
			v->regs[v->reg].lines =
			        ed->clip_line || (old && v->regs[v->reg].lines);
		}
	}
	v->reg = -1;
	v->append = false;
}

bool wf_vim_paste_register(WfEditSession *s)
{
	WfVim *v = &s->vim;
	WfBuffer *b = wf_es_buffer(s);
	bool ok = true;

	if (b && v->reg >= 0 && v->regs[v->reg].text) {
		WfStatus st = wf_ed_set_clip(&b->ed, v->regs[v->reg].text,
		        v->regs[v->reg].n, v->regs[v->reg].lines);

		if (WF_OK == st) {
			wf_es_clip_out(s);
		} else {
			/* the clip still holds something else: it must not be
			 * pasted, nor sent to the system as this register */
			wf_es_fail_status(s, st);
			ok = false;
		}
	}
	v->reg = -1;
	v->append = false;
	return ok;
}

/* Applies `op` to the characters from..to (to excluded). */
static WfEsResult chars_op(WfEditSession *s, char op, WfPos from, WfPos to)
{
	WfBuffer *b = wf_es_buffer(s);
	WfEditor *ed = &b->ed;
	WfEsResult res;
	uint64_t gen = ed->text.gen;

	if (pos_before(to, from)) {
		WfPos t = from;

		from = to;
		to = t;
	}
	if (!pos_before(from, to)) {
		if ('c' == op) {
			s->vim_insert = true;
		}
		return WF_ES_REDRAW;
	}
	ed->anchor = from;
	ed->cur = to;
	ed->has_anchor = true;
	ed->mark = false;
	ed->want_valid = false;
	if ('y' == op) {
		res = wf_es_run(s, WF_CMD_COPY, false);
		wf_ed_clear_selection(ed);
		ed->cur = from;
		ed->want_valid = false;
	} else {
		res = wf_es_run(s, WF_CMD_CUT, false);
		if (ed->text.gen ==
		        gen) { /* read-only, or too big: nothing was cut */
			s->vim.reg = -1;
			return res;
		}
		if ('c' == op) {
			s->vim_insert = true;
		}
	}
	keep_register(s, ed);
	return res;
}

/* Applies `op` to whole lines l1..l2. */
static WfEsResult lines_op(WfEditSession *s, char op, size_t l1, size_t l2)
{
	WfBuffer *b = wf_es_buffer(s);
	WfEditor *ed = &b->ed;
	WfPos home = ed->cur;
	WfEsResult res;
	size_t n = 0, last = last_line(ed);
	uint64_t gen = ed->text.gen;
	char *text;

	if (l2 > last) {
		l2 = last;
	}
	if (l1 > l2) {
		l1 = l2;
	}
	text = lines_text(ed, l1, l2, &n);
	if (!text) {
		return WF_ES_REDRAW;
	}
	if ('y' == op) {
		ed->anchor = (WfPos){l1, 0};
		ed->cur = (WfPos){l2, ed->text.lines[l2].len};
		ed->has_anchor = true;
		res = wf_es_run(s, WF_CMD_COPY, false);
		wf_ed_clear_selection(ed);
		ed->cur = home;
		if (ed->cur.line > last_line(ed)) {
			ed->cur.line = last_line(ed);
		}
		ed->want_valid = false;
	} else {
		if ('c' == op) { /* the text goes, the line stays */
			ed->anchor = (WfPos){l1, 0};
			ed->cur = (WfPos){l2, ed->text.lines[l2].len};
		} else if (l2 < last) {
			ed->anchor = (WfPos){l1, 0};
			ed->cur = (WfPos){l2 + 1, 0};
		} else if (l1 >
		        0) { /* the last lines: take the break before them */
			ed->anchor =
			        (WfPos){l1 - 1, ed->text.lines[l1 - 1].len};
			ed->cur = (WfPos){l2, ed->text.lines[l2].len};
		} else {
			ed->anchor = (WfPos){0, 0};
			ed->cur = (WfPos){l2, ed->text.lines[l2].len};
		}
		ed->has_anchor = true;
		ed->mark = false;
		res = wf_es_run(s, WF_CMD_CUT, false);
		if (ed->text.gen == gen) { /* nothing was cut */
			wf_ed_clear_selection(ed);
			ed->cur = home;
			s->vim.reg = -1;
			free(text);
			return res;
		}
		if ('c' == op) {
			s->vim_insert = true;
		} else if (ed->cur.line <= last_line(ed)) {
			ed->cur.col = 0;
		}
		ed->want_valid = false;
	}
	{
		WfStatus st = wf_ed_set_clip(ed, text, n, true);

		if (WF_OK == st) {
			wf_es_clip_out(s);
			keep_register(s, ed);
		} else {
			/* the text is out of the document and in no clip: said, and
			 * the old clip is not sent on as if it were this */
			wf_es_fail_status(s, st);
			s->vim.reg = -1;
		}
	}
	free(text);
	return res;
}

static unsigned total_count(const WfVim *v)
{
	unsigned a = v->count ? v->count : 1, b = v->count2 ? v->count2 : 1;
	unsigned long long t = (unsigned long long)a * b;

	return t > 9999u ? 9999u : (unsigned)t;
}

/* 'c' with 'w' stops at the end of the word, as Vim does: not on the blank. */
static WfPos trim_blanks(const WfEditor *ed, WfPos from, WfPos to)
{
	if (to.line == from.line) {
		const char *s = ed->text.lines[to.line].s;

		while (to.col > from.col &&
		        (' ' == s[to.col - 1] || '\t' == s[to.col - 1])) {
			to.col--;
		}
	}
	return to;
}

/* "dw" on the last word of a line stops at the end of the line: the break and
 * the next line's word stay (Vim's special case for w with an operator). */
static WfPos clamp_to_line(const WfEditor *ed, WfPos from, WfPos to)
{
	if (to.line > from.line && to.line < ed->text.n) {
		const WfELine *l = &ed->text.lines[to.line];
		size_t c = 0;

		while (c < l->len && (' ' == l->s[c] || '\t' == l->s[c])) {
			c++;
		}
		if (to.col <= c) {
			return (WfPos){
			        to.line - 1, ed->text.lines[to.line - 1].len};
		}
	}
	return to;
}

/* 0 blank, 1 word, 2 punctuation: the classes the w/b/e motions use */
static int char_class(unsigned char c)
{
	if (' ' == c || '\t' == c) {
		return 0;
	}
	return ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
	               (c >= 'A' && c <= 'Z') || '_' == c || c >= 0x80)
	        ? 1
	        : 2;
}

/* The end of the word `n` words on, excluded: "de" takes the last letter. */
static WfPos word_end(const WfEditor *ed, WfPos p, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		const WfELine *l = &ed->text.lines[p.line];

		/* the first step leaves the character we stand on; the next ones start
		 * where the last stopped, just after a word, on what comes next */
		if (0 == i && p.col < l->len) {
			p.col = wf_utf8_cluster_next(l->s, l->len, p.col);
		}
		for (;;) { /* over blanks and line breaks */
			l = &ed->text.lines[p.line];
			if (p.col < l->len &&
			        0 != char_class((unsigned char)l->s[p.col])) {
				break;
			}
			if (p.col < l->len) {
				p.col++;
			} else if (p.line + 1 < ed->text.n) {
				p.line++;
				p.col = 0;
			} else {
				return p;
			}
		}
		l = &ed->text.lines[p.line];
		{
			int k = char_class((unsigned char)l->s[p.col]);

			while (p.col < l->len &&
			        char_class((unsigned char)l->s[p.col]) == k) {
				p.col++;
			}
		}
	}
	return p;
}

/* f, t, F, T: the `n`th `cp` on this line. */
static bool find_in_line(const WfEditor *ed,
        char kind,
        uint32_t cp,
        unsigned n,
        WfPos *from,
        WfPos *to)
{
	const WfELine *l = &ed->text.lines[ed->cur.line];
	char u[4];
	size_t w = wf_utf8_encode(cp, u), at = ed->cur.col;
	bool fwd = 'f' == kind || 't' == kind;

	if (!w || !n) {
		return false;
	}
	for (;;) {
		if (fwd) {
			if (at >= l->len) {
				return false;
			}
			at = wf_utf8_cluster_next(l->s, l->len, at);
			while (at + w <= l->len &&
			        memcmp(l->s + at, u, w) != 0) {
				at = wf_utf8_cluster_next(l->s, l->len, at);
			}
			if (at + w > l->len) {
				return false;
			}
		} else {
			if (0 == at) {
				return false;
			}
			at = wf_utf8_cluster_prev(l->s, l->len, at);
			while (at + w > l->len ||
			        memcmp(l->s + at, u, w) != 0) {
				if (0 == at) {
					return false;
				}
				at = wf_utf8_cluster_prev(l->s, l->len, at);
			}
		}
		if (0 == --n) {
			break;
		}
	}
	*from = *to = ed->cur;
	switch (kind) {
	case 'f': to->col = at + w; break;
	case 't': to->col = at; break;
	case 'F': from->col = at; break;
	default:  from->col = at + w; break; /* T */
	}
	return true;
}

static bool is_blank_at(const WfELine *l, size_t c)
{
	return c < l->len && (' ' == l->s[c] || '\t' == l->s[c]);
}

static bool object_word(const WfEditor *ed, bool around, WfPos *from, WfPos *to)
{
	const WfELine *l = &ed->text.lines[ed->cur.line];
	size_t a = ed->cur.col, b;
	int k;

	if (!l->len) {
		return false;
	}
	if (a >= l->len) {
		a = l->len - 1;
	}
	k = char_class((unsigned char)l->s[a]);
	b = a;
	while (a > 0 && char_class((unsigned char)l->s[a - 1]) == k) {
		a--;
	}
	while (b < l->len && char_class((unsigned char)l->s[b]) == k) {
		b++;
	}
	if (around) {
		if (0 == k) { /* on blanks: they and the word after them */
			if (b < l->len) {
				int k2 = char_class((unsigned char)l->s[b]);

				while (b < l->len &&
				        char_class((unsigned char)l->s[b]) ==
				                k2) {
					b++;
				}
			}
		} else if (is_blank_at(l, b)) { /* the blanks after it ... */
			while (is_blank_at(l, b)) {
				b++;
			}
		} else { /* ... or, with none, the ones before it */
			while (a > 0 && is_blank_at(l, a - 1)) {
				a--;
			}
		}
	}
	*from = (WfPos){ed->cur.line, a};
	*to = (WfPos){ed->cur.line, b};
	return true;
}

/* The columns of the text between the quotes at `open` and `close`; with
 * `around`, the quotes too and the blanks on one side: after the closing quote if
 * there are any, else before the opening one. */
static void quote_span(const WfELine *l,
        size_t open,
        size_t close,
        bool around,
        size_t *a,
        size_t *b)
{
	*a = open + 1;
	*b = close;
	if (!around) {
		return;
	}
	*a = open;
	*b = close + 1;
	if (is_blank_at(l, *b)) {
		while (is_blank_at(l, *b)) {
			(*b)++;
		}
		return;
	}
	while (*a > 0 && is_blank_at(l, *a - 1)) {
		(*a)--;
	}
}

static bool object_quote(
        const WfEditor *ed, char q, bool around, WfPos *from, WfPos *to)
{
	const WfELine *l = &ed->text.lines[ed->cur.line];
	size_t open = 0, i = 0;
	bool have = false;

	while (i <
	        l->len) { /* quotes pair up from the left; a backslash escapes */
		if ('\\' == l->s[i] && i + 1 < l->len) {
			i += 2;
			continue;
		}
		if (q != l->s[i]) {
			i++;
			continue;
		}
		if (!have) {
			open = i++;
			have = true;
			continue;
		}
		if (i >=
		        ed->cur.col) { /* the first pair that reaches the cursor */
			size_t a, b;

			quote_span(l, open, i, around, &a, &b);
			*from = (WfPos){ed->cur.line, a};
			*to = (WfPos){ed->cur.line, b};
			return true;
		}
		have = false;
		i++;
	}
	return false;
}

static bool object_bracket(const WfEditor *ed,
        char open,
        char close,
        bool around,
        WfPos *from,
        WfPos *to)
{
	WfPos p = ed->cur, o, c;
	size_t depth = 0;
	const WfELine *l = &ed->text.lines[p.line];

	/* back to the opening bracket that encloses the cursor (or is under it) */
	if (p.col < l->len && open == l->s[p.col]) {
		goto found_open;
	}
	for (;;) {
		if (0 == p.col) {
			if (0 == p.line) {
				return false;
			}
			p.line--;
			p.col = ed->text.lines[p.line].len;
			continue;
		}
		p.col--;
		l = &ed->text.lines[p.line];
		if (close == l->s[p.col]) {
			depth++;
		} else if (open == l->s[p.col]) {
			if (0 == depth) {
				break;
			}
			depth--;
		}
	}
found_open:
	o = p;
	depth = 0;
	for (;;) { /* forward to its partner */
		l = &ed->text.lines[p.line];
		if (p.col >= l->len) {
			if (p.line + 1 >= ed->text.n) {
				return false;
			}
			p.line++;
			p.col = 0;
			continue;
		}
		if (open == l->s[p.col]) {
			depth++;
		} else if (close == l->s[p.col]) {
			if (0 == --depth) {
				break;
			}
		}
		p.col++;
	}
	c = p;
	if (around) {
		c.col++;
	} else {
		o.col++;
	}
	*from = o;
	*to = c;
	return true;
}

static WfEsResult run_object(WfEditSession *s, bool around, uint32_t ch)
{
	WfVim *v = &s->vim;
	WfEditor *ed = &wf_es_buffer(s)->ed;
	char op = v->op;
	WfPos from = {0, 0}, to = {0, 0};
	bool ok = false;

	v->wait = 0;
	v->op = 0;
	v->count = v->count2 = 0;
	switch (ch) {
	case 'w':  ok = object_word(ed, around, &from, &to); break;
	case '"':
	case '\'':
	case '`':  ok = object_quote(ed, (char)ch, around, &from, &to); break;
	case '(':
	case ')':
	case 'b':  ok = object_bracket(ed, '(', ')', around, &from, &to); break;
	case '[':
	case ']':  ok = object_bracket(ed, '[', ']', around, &from, &to); break;
	case '{':
	case '}':
	case 'B':  ok = object_bracket(ed, '{', '}', around, &from, &to); break;
	case '<':
	case '>':  ok = object_bracket(ed, '<', '>', around, &from, &to); break;
	default:   break;
	}
	if (!ok) {
		v->reg = -1;
		v->append = false;
		return WF_ES_REDRAW;
	}
	return chars_op(s, op, from, to);
}

/* The character after f, t, F or T. */
static WfEsResult run_find(WfEditSession *s, char kind, uint32_t ch)
{
	WfVim *v = &s->vim;
	WfEditor *ed = &wf_es_buffer(s)->ed;
	char op = v->op;
	unsigned n = total_count(v);
	WfPos from = {0, 0}, to = {0, 0};

	v->wait = 0;
	v->op = 0;
	v->count = v->count2 = 0;
	if (!find_in_line(ed, kind, ch, n, &from, &to)) {
		v->reg = -1;
		v->append = false;
		return WF_ES_REDRAW; /* not found: nothing happens, as in Vim */
	}
	return chars_op(s, op, from, to);
}

static WfEsResult run_motion(WfEditSession *s, char motion)
{
	WfVim *v = &s->vim;
	WfBuffer *b = wf_es_buffer(s);
	WfEditor *ed = &b->ed;
	unsigned n = total_count(v);
	char op = v->op;
	size_t cur = ed->cur.line, last = last_line(ed);
	WfPos t;
	bool counted = v->count || v->count2;

	v->wait = 0;
	v->op = 0;
	v->count = v->count2 = 0;
	switch (motion) {
	case 'w':
		t = clamp_to_line(
		        ed, ed->cur, motion_target(ed, WF_MV_WORD_RIGHT, n));
		if ('c' == op) {
			t = trim_blanks(ed, ed->cur, t);
		}
		return chars_op(s, op, ed->cur, t);
	case 'e': return chars_op(s, op, ed->cur, word_end(ed, ed->cur, n));
	case 'b':
		return chars_op(
		        s, op, motion_target(ed, WF_MV_WORD_LEFT, n), ed->cur);
	case '$':
		return chars_op(s, op, ed->cur,
		        (WfPos){ed->cur.line,
		                ed->text.lines[ed->cur.line].len});
	case '0': return chars_op(s, op, (WfPos){ed->cur.line, 0}, ed->cur);
	case 'h':
		return chars_op(
		        s, op, motion_target(ed, WF_MV_LEFT, n), ed->cur);
	case 'l':
		return chars_op(
		        s, op, ed->cur, motion_target(ed, WF_MV_RIGHT, n));
	case 'j': return lines_op(s, op, cur, cur + n > last ? last : cur + n);
	case 'k': return lines_op(s, op, cur > n ? cur - n : 0, cur);
	case 'G': {
		size_t to = counted ? (n ? n - 1 : 0) : last;

		if (to > last) {
			to = last;
		}
		return lines_op(
		        s, op, cur < to ? cur : to, cur < to ? to : cur);
	}
	case 'g': { /* gg */
		size_t to = counted ? (n ? n - 1 : 0) : 0;

		if (to > last) {
			to = last;
		}
		return lines_op(
		        s, op, cur < to ? cur : to, cur < to ? to : cur);
	}
	default:
		if (motion == op) { /* dd, yy, cc: n lines from this one */
			return lines_op(s, op, cur, cur + n - 1);
		}
		return WF_ES_REDRAW; /* not a motion: nothing happens */
	}
}

static WfEsResult visual_op(WfEditSession *s, char op)
{
	WfVim *v = &s->vim;
	WfBuffer *b = wf_es_buffer(s);
	WfEditor *ed = &b->ed;
	WfPos a = ed->has_anchor ? ed->anchor : ed->cur, c = ed->cur, to;
	bool line = v->vline;
	WfEsResult res;

	v->visual = v->vline = false;
	if (line) {
		size_t l1 = a.line < c.line ? a.line : c.line;
		size_t l2 = a.line < c.line ? c.line : a.line;

		wf_ed_clear_selection(ed);
		return lines_op(s, op, l1, l2);
	}
	if (pos_before(c, a)) {
		WfPos t = a;

		a = c;
		c = t;
	}
	/* Visual is inclusive: the character under the cursor goes too */
	to = c;
	if (to.col < ed->text.lines[to.line].len) {
		to.col = wf_utf8_cluster_next(ed->text.lines[to.line].s,
		        ed->text.lines[to.line].len, to.col);
	}
	res = chars_op(s, op, a, to);
	return res;
}

static void repeat_last(WfEditSession *s, unsigned times);

WfEsResult wf_vim_exec(WfEditSession *s, WfEdCmd cmd, unsigned count)
{
	WfVim *v = &s->vim;
	WfBuffer *b = wf_es_buffer(s);
	WfEditor *ed;

	if (!b) {
		return WF_ES_CLOSED;
	}
	ed = &b->ed;
	switch (cmd) {
	case WF_CMD_VIM_OP_DELETE:
	case WF_CMD_VIM_OP_YANK:
	case WF_CMD_VIM_OP_CHANGE: {
		char op = WF_CMD_VIM_OP_DELETE == cmd ? 'd'
		        : WF_CMD_VIM_OP_YANK == cmd   ? 'y'
		                                      : 'c';

		if (v->visual) {
			return visual_op(s, op);
		}
		v->wait = 'o';
		v->op = op;
		v->count = count;
		v->count2 = 0;
		return WF_ES_REDRAW;
	}
	case WF_CMD_VIM_DELETE_EOL:
	case WF_CMD_VIM_CHANGE_EOL: {
		char op = WF_CMD_VIM_DELETE_EOL == cmd ? 'd' : 'c';

		return chars_op(s, op, ed->cur,
		        (WfPos){ed->cur.line,
		                ed->text.lines[ed->cur.line].len});
	}
	case WF_CMD_VIM_VISUAL:
	case WF_CMD_VIM_VISUAL_LINE: {
		bool line = WF_CMD_VIM_VISUAL_LINE == cmd;

		if (v->visual &&
		        v->vline == line) { /* the same key again leaves it */
			v->visual = v->vline = false;
			wf_ed_clear_selection(ed);
			return WF_ES_REDRAW;
		}
		if (!v->visual) {
			ed->anchor = ed->cur;
			ed->has_anchor = true;
			ed->mark = false;
		}
		v->visual = true;
		v->vline = line;
		return WF_ES_REDRAW;
	}
	case WF_CMD_VIM_REGISTER: v->wait = '"'; return WF_ES_REDRAW;
	case WF_CMD_VIM_RECORD:
		if (v->recording >=
		        0) { /* stop; the key that stopped it is not part of it */
			if (v->macro[v->recording].n && !v->macro_over) {
				v->macro[v->recording].n--;
			}
			v->recording = -1;
		} else {
			v->wait = 'q';
		}
		return WF_ES_REDRAW;
	case WF_CMD_VIM_PLAY:
		v->wait = '@';
		v->count = count;
		return WF_ES_REDRAW;
	case WF_CMD_VIM_DOT: repeat_last(s, count); return WF_ES_REDRAW;
	default:             break;
	}
	return WF_ES_NONE;
}

static void play(WfEditSession *s, int reg, unsigned times)
{
	WfVim *v = &s->vim;
	size_t n = v->macro[reg].n;
	WfEvent *copy;

	if (v->playing || !n) {
		return;
	}
	copy = malloc(n * sizeof *copy);
	if (!copy) {
		return;
	}
	memcpy(copy, v->macro[reg].ev, n * sizeof *copy);
	v->playing = true;
	for (unsigned t = 0; t < (times ? times : 1); t++) {
		for (size_t i = 0; i < n; i++) {
			(void)wf_es_event(s, &copy[i], s->rows, s->cols);
		}
	}
	v->playing = false;
	v->dot_skip = true; /* the keys inside already made their own "." */
	free(copy);
}

bool wf_vim_follow(WfEditSession *s, const WfEvent *ev, WfEsResult *res)
{
	WfVim *v = &s->vim;
	uint32_t ch;

	if (!v->wait) {
		return false;
	}
	*res = WF_ES_REDRAW;
	if (WF_EV_CHAR != ev->type || (ev->vk & WF_VK_PASTED)) {
		v->wait =
		        0; /* Esc, an arrow, anything else: the command is dropped */
		v->op = 0;
		v->count = v->count2 = 0;
		return WF_EV_KEY == ev->type;
	}
	ch = ev->ch;
	switch (v->wait) {
	case '"':
		v->wait = 0;
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
			v->append = ch >= 'A' &&
			        ch <= 'Z'; /* "A: add to register a */
			v->reg = (int)((ch | 0x20) - 'a');
			v->reg_fresh = true;
		}
		return true;
	case 'q':
		v->wait = 0;
		if (ch >= 'a' && ch <= 'z') {
			int r = (int)(ch - 'a');

			free(v->macro[r].ev);
			v->macro[r].ev =
			        malloc(WF_VIM_MACRO_MAX * sizeof(WfEvent));
			v->macro[r].n = 0;
			if (v->macro[r].ev) {
				v->recording = r;
				v->macro_over = false;
			}
		}
		return true;
	case '@':
		v->wait = 0;
		if (ch >= 'a' && ch <= 'z') {
			unsigned times = v->count;

			v->count = 0;
			play(s, (int)(ch - 'a'), times);
		}
		return true;
	case 'g':
		if ('g' == ch) {
			v->wait = 'o';
			*res = run_motion(s, 'g');
		} else {
			v->wait = 0;
			v->op = 0;
		}
		return true;
	case 'o':
		if (ch >= '1' && ch <= '9') {
			v->count2 = v->count2 * 10 + (ch - '0');
			return true;
		}
		if ('0' == ch && v->count2) {
			v->count2 *= 10;
			return true;
		}
		if ('g' == ch) {
			v->wait = 'g';
			return true;
		}
		if ('i' == ch ||
		        'a' == ch) { /* a text object: iw, a", i( ... */
			v->wait = 'i' == ch ? 'I' : 'A';
			return true;
		}
		if ('f' == ch || 't' == ch || 'F' == ch || 'T' == ch) {
			v->wait = (char)ch;
			return true;
		}
		*res = run_motion(s, (char)(ch < 0x80 ? ch : 0));
		return true;
	case 'I':
	case 'A': *res = run_object(s, 'A' == v->wait, ch); return true;
	case 'f':
	case 't':
	case 'F':
	case 'T': *res = run_find(s, v->wait, ch); return true;
	default:  break;
	}
	v->wait = 0;
	return true;
}

void wf_vim_record(WfEditSession *s, const WfEvent *ev)
{
	WfVim *v = &s->vim;

	if (v->recording < 0 || v->playing || v->replaying) {
		return;
	}
	if (v->macro[v->recording].n < WF_VIM_MACRO_MAX) {
		v->macro[v->recording].ev[v->macro[v->recording].n++] = *ev;
	} else {
		v->macro_over =
		        true; /* this key, and the one that stops it, are lost */
	}
}

static bool idle(const WfEditSession *s)
{
	return wf_keymap_modal(s->cfg.keys) && !s->vim_insert && !s->vim.wait &&
	        !s->vim.visual && PR_NONE == s->pk &&
	        !wf_keystate_active(&s->ks);
}

void wf_vim_dot_before(WfEditSession *s, const WfEvent *ev)
{
	WfVim *v = &s->vim;
	WfBuffer *b = wf_es_buffer(s);

	if (v->replaying || !b || !wf_keymap_modal(s->cfg.keys)) {
		return;
	}
	if (idle(s)) { /* a new command starts here */
		v->cand_n = 0;
		v->cand_ok = true;
		v->cand_gen = b->ed.text.gen;
		v->dot_skip = false;
	}
	if (!v->cand_ok) {
		return;
	}
	if (!v->cand) {
		v->cand = malloc(WF_VIM_DOT_MAX * sizeof *v->cand);
	}
	if (!v->cand || v->cand_n >= WF_VIM_DOT_MAX) {
		v->cand_ok = false;
		return;
	}
	v->cand[v->cand_n++] = *ev;
}

void wf_vim_dot_after(WfEditSession *s)
{
	WfVim *v = &s->vim;
	WfBuffer *b = wf_es_buffer(s);

	if (v->replaying || !b || !wf_keymap_modal(s->cfg.keys)) {
		return;
	}
	if (PR_NONE !=
	        s->pk) { /* a question in the middle (Ex, find): not repeatable */
		v->cand_ok = false;
	}
	if (!idle(s)) {
		return;
	}
	if (v->cand_ok && v->cand_n && !v->dot_skip &&
	        b->ed.text.gen != v->cand_gen) {
		WfEvent *keep = malloc(v->cand_n * sizeof *keep);

		if (keep) {
			memcpy(keep, v->cand, v->cand_n * sizeof *keep);
			free(v->dot);
			v->dot = keep;
			v->dot_n = v->cand_n;
		}
	}
	v->cand_n = 0;
	v->cand_ok = false;
	/* a register chosen with " serves the next command only */
	if (v->reg >= 0) {
		if (v->reg_fresh) {
			v->reg_fresh = false;
		} else {
			v->reg = -1;
			v->append = false;
		}
	}
}

static void repeat_last(WfEditSession *s, unsigned times)
{
	WfVim *v = &s->vim;
	WfEvent *copy;
	size_t n = v->dot_n;

	if (v->replaying || !n) {
		return;
	}
	copy = malloc(n * sizeof *copy);
	if (!copy) {
		return;
	}
	memcpy(copy, v->dot, n * sizeof *copy);
	v->replaying = true;
	for (unsigned t = 0; t < (times ? times : 1); t++) {
		for (size_t i = 0; i < n; i++) {
			(void)wf_es_event(s, &copy[i], s->rows, s->cols);
		}
	}
	v->replaying = false;
	v->dot_skip = true; /* "." is not itself the change to repeat */
	free(copy);
}

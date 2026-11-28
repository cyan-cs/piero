/*
 * Vim's Normal mode beyond single keys. See wf_evim.h.
 *
 * Operators resolve a range before committing text and typed registers.
 * Long w/b motions and macro playback yield to the event loop; provisional
 * positions never change the document or visible selection.
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
	free(v->unnamed.text);
	free(v->dot);
	free(v->cand);
	free(v->playback.ev);
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
	size_t last = ed->text.n ? ed->text.n - 1 : 0;
	return last && !ed->text.lines[last].len ? last - 1 : last;
}

static bool pos_before(WfPos a, WfPos b)
{
	return a.line < b.line || (a.line == b.line && a.col < b.col);
}

void wf_vim_normal_cursor(WfEditSession *s)
{
	WfBuffer *b = wf_es_buffer(s);
	if (!b || s->vim_insert) {
		return;
	}
	WfEditor *ed = &b->ed;
	size_t last = last_line(ed);
	if (ed->cur.line > last) {
		ed->cur.line = last;
	}
	const WfELine *l = &ed->text.lines[ed->cur.line];
	if (ed->cur.col >= l->len) {
		ed->cur.col = l->len ? wf_et_col_prev(l, l->len) : 0;
	}
}

static char *lines_text(const WfEditor *ed, size_t l1, size_t l2, size_t *n)
{
	size_t total = 0, at = 0;
	char *out;

	for (size_t i = l1; i <= l2; i++) {
		total += ed->text.lines[i].len + 1;
	}
	out = wf_et_malloc(total ? total : 1);
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

/* Resolve and allocate all register updates before changing the document.
 * The old text, cursor, selection and registers survive every failure. */
static WfEsResult apply_range(WfEditSession *s,
        char op,
        WfPos from,
        WfPos to,
        char *text,
        size_t n,
        bool lines)
{
	WfVim *v = &s->vim;
	WfEditor *ed = &wf_es_buffer(s)->ed;
	char *named = NULL, *clip = NULL;
	size_t named_n = n;
	bool named_lines = lines;
	WfStatus st = WF_OK;
	int reg = v->reg;
	uint32_t group = ed->undo.open_group, next_group = ed->undo.next_group;
	size_t group_bytes = ed->undo.open_bytes;
	if ('y' != op && ed->read_only) {
		st = WF_E_ACCESS;
		goto fail;
	}
	if (!text) {
		st = wf_et_read(&ed->text, from, to, &text, &n);
		named_n = n;
		if (WF_OK != st) {
			goto fail;
		}
	}
	clip = wf_et_malloc(n ? n : 1);
	if (!clip) {
		st = WF_E_NOMEM;
		goto fail;
	}
	memcpy(clip, text, n);
	if (reg >= 0) {
		WfVimRegister *old = &v->regs[reg];
		size_t prefix = v->append ? old->n : 0;
		named_lines = lines || (prefix && old->lines);
		bool separator =
		        prefix && named_lines && old->text[prefix - 1] != '\n';
		bool ending = named_lines && n && text[n - 1] != '\n';
		if (prefix > SIZE_MAX - 2 || n > SIZE_MAX - 2 - prefix ||
		        (ed->opt.max_bytes &&
		                (prefix > ed->opt.max_bytes ||
		                        n > ed->opt.max_bytes - prefix))) {
			st = WF_E_RANGE;
			goto fail;
		}
		named_n = prefix + n + (separator ? 1 : 0) + (ending ? 1 : 0);
		named = wf_et_malloc(named_n ? named_n : 1);
		if (!named) {
			st = WF_E_NOMEM;
			goto fail;
		}
		if (prefix) {
			memcpy(named, old->text, prefix);
		}
		size_t at = prefix;
		if (separator) {
			named[at++] = '\n';
		}
		memcpy(named + at, text, n);
		if (ending) {
			named[at + n] = '\n';
		}
	}
	if ('y' != op) {
		if ('c' == op) {
			wf_un_group_begin(&ed->undo);
		}
		st = wf_ed_replace_range(ed, from, to, NULL, 0);
		if (WF_OK != st) {
			goto fail;
		}
	} else {
		ed->cur = from;
		wf_ed_clear_selection(ed);
		ed->want_valid = false;
	}
	free(v->unnamed.text);
	v->unnamed = (WfVimRegister){text, n, lines};
	free(ed->clip);
	ed->clip = clip;
	ed->clip_n = n;
	ed->clip_line = lines;
	if (reg >= 0) {
		free(v->regs[reg].text);
		v->regs[reg] = (WfVimRegister){named, named_n, named_lines};
	}
	if ('c' == op) {
		s->vim_insert = true;
	} else if ('d' == op) {
		wf_vim_normal_cursor(s);
		if (ed->undo.pos) {
			ed->undo.rec[ed->undo.pos - 1].after = ed->cur;
		}
	}
	v->reg = -1;
	v->append = false;
	return WF_ES_REDRAW;
fail:
	ed->undo.open_group = group;
	ed->undo.next_group = next_group;
	ed->undo.open_bytes = group_bytes;
	free(text);
	free(named);
	free(clip);
	v->reg = -1;
	v->append = false;
	wf_es_fail_status(s, st);
	return WF_ES_REDRAW;
}

static WfEsResult chars_op(WfEditSession *s, char op, WfPos from, WfPos to)
{
	if (pos_before(to, from)) {
		WfPos t = from;
		from = to;
		to = t;
	}
	if (!pos_before(from, to)) {
		if ('c' == op) {
			wf_un_group_begin(&wf_es_buffer(s)->ed.undo);
			s->vim_insert = true;
		}
		return WF_ES_REDRAW;
	}
	return apply_range(s, op, from, to, NULL, 0, false);
}

static WfEsResult lines_op(WfEditSession *s, char op, size_t l1, size_t l2)
{
	WfEditor *ed = &wf_es_buffer(s)->ed;
	size_t last = last_line(ed), n = 0;
	if (l2 > last) {
		l2 = last;
	}
	if (l1 > l2) {
		l1 = l2;
	}
	char *text = lines_text(ed, l1, l2, &n);
	if (!text) {
		wf_es_fail_status(s, WF_E_NOMEM);
		return WF_ES_REDRAW;
	}
	WfPos from = {l1, 0}, to = {l2, ed->text.lines[l2].len};
	if ('c' != op && l2 + 1 < ed->text.n) {
		to = (WfPos){l2 + 1, 0};
	} else if ('d' == op && l1 > 0) {
		from = (WfPos){l1 - 1, ed->text.lines[l1 - 1].len};
	}
	uint64_t gen = ed->text.gen;
	WfEsResult result = apply_range(s, op, from, to, text, n, true);
	if ('d' == op && gen != ed->text.gen) {
		ed->cur.line = l1 < ed->text.n ? l1 : ed->text.n - 1;
		ed->cur.col = 0;
	}
	return result;
}

WfEsResult wf_vim_paste(WfEditSession *s, bool after)
{
	WfVim *v = &s->vim;
	WfEditor *ed = &wf_es_buffer(s)->ed;
	const WfVimRegister *r = v->reg >= 0 ? &v->regs[v->reg] : &v->unnamed;
	v->reg = -1;
	v->append = false;
	if (!r->text || !r->n) {
		return WF_ES_REDRAW;
	}
	WfPos at = ed->cur;
	char *allocated = NULL;
	const char *text = r->text;
	size_t n = r->n;
	size_t line = at.line;
	if (r->lines) {
		if (after && at.line + 1 < ed->text.n) {
			at.line++;
			at.col = 0;
		} else if (after) {
			if (n && text[n - 1] == '\n') {
				n--;
			}
			if (n && text[n - 1] == '\r') {
				n--;
			}
			allocated = wf_et_malloc(n + 1);
			if (!allocated) {
				wf_es_fail_status(s, WF_E_NOMEM);
				return WF_ES_REDRAW;
			}
			allocated[0] = '\n';
			memcpy(allocated + 1, text, n);
			text = allocated;
			n++;
			at.col = ed->text.lines[at.line].len;
		} else {
			at.col = 0;
		}
		line += after ? 1 : 0;
	} else if (after && at.col < ed->text.lines[at.line].len) {
		at.col = wf_et_col_next(&ed->text.lines[at.line], at.col);
	}
	WfStatus st = wf_ed_replace_range(ed, at, at, text, n);
	free(allocated);
	if (WF_OK != st) {
		wf_es_fail_status(s, st);
		return WF_ES_REDRAW;
	}
	if (r->lines) {
		ed->cur = (WfPos){line < ed->text.n ? line : ed->text.n - 1, 0};
		const WfELine *l = &ed->text.lines[ed->cur.line];
		while (ed->cur.col < l->len &&
		        (l->s[ed->cur.col] == ' ' ||
		                l->s[ed->cur.col] == '\t')) {
			ed->cur.col++;
		}
	} else if (ed->cur.col) {
		ed->cur.col = wf_et_col_prev(
		        &ed->text.lines[ed->cur.line], ed->cur.col);
	}
	if (ed->undo.pos) {
		ed->undo.rec[ed->undo.pos - 1].after = ed->cur;
	}
	return WF_ES_REDRAW;
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

static void word_begin(
        WfEditSession *s, bool right, unsigned count, bool extend, char op)
{
	WfVim *v = &s->vim;
	WfBuffer *b = wf_es_buffer(s);
	memset(&v->motion, 0, sizeof v->motion);
	v->motion.buffer = b;
	v->motion.gen = b->ed.text.gen;
	v->motion.from = v->motion.at = b->ed.cur;
	v->motion.left = count;
	v->motion.right = right;
	v->motion.extend = extend;
	v->motion.op = op;
	v->motion.active = true;
	if (!right && !v->motion.at.col && v->motion.at.line) {
		v->motion.at.line--;
		v->motion.at.col = b->ed.text.lines[v->motion.at.line].len;
	}
	wf_ed_word_scan_begin(&v->motion.scan, v->motion.at.col, right);
}

/* One bounded scan; neither cursor nor selection is provisional. */
static bool vim_word_slice(WfEditSession *s)
{
	WfVim *v = &s->vim;
	WfBuffer *b = wf_es_buffer(s);
	if (!v->motion.active) {
		return false;
	}
	if (!b || v->motion.buffer != b || v->motion.gen != b->ed.text.gen ||
	        wf_pos_cmp(v->motion.from, b->ed.cur)) {
		wf_vim_stop(s);
		wf_es_fail_status(s, WF_E_CANCELLED);
		return true;
	}
	WfEditor *ed = &b->ed;
	size_t budget = 65536;
	/* Counts cannot turn one event into an unbounded series of scans. */
	for (unsigned step = 0; step < 16 && budget; step++) {
		const WfELine *l = &ed->text.lines[v->motion.at.line];
		if (v->motion.leading) {
			size_t before = v->motion.at.col;
			size_t end = l->len - v->motion.at.col > budget
			        ? v->motion.at.col + budget
			        : l->len;
			while (v->motion.at.col < end &&
			        char_class((
			                unsigned char)l->s[v->motion.at.col]) ==
			                0) {
				v->motion.at.col++;
			}
			budget -= v->motion.at.col - before;
			if (v->motion.at.col == end && end < l->len) {
				return false;
			}
			v->motion.leading = false;
		} else {
			size_t before = v->motion.scan.col;
			if (!wf_ed_word_scan_step(l, &v->motion.scan, budget)) {
				return false;
			}
			v->motion.at.col = v->motion.scan.col;
			size_t scanned = before > v->motion.at.col
			        ? before - v->motion.at.col
			        : v->motion.at.col - before;
			budget = scanned >= budget ? 0 : budget - scanned;
			if (v->motion.right && v->motion.at.col == l->len &&
			        v->motion.at.line < last_line(ed)) {
				v->motion.at.line++;
				v->motion.at.col = 0;
				v->motion.leading = true;
				continue;
			}
		}
		if (--v->motion.left == 0) {
			WfPos to = v->motion.at, from = v->motion.from;
			char op = v->motion.op;
			v->motion.active = false;
			if (op) {
				if (v->motion.right) {
					to = clamp_to_line(ed, from, to);
				}
				if (op == 'c') {
					to = trim_blanks(ed, from, to);
				}
				(void)chars_op(s, op, from, to);
			} else {
				wf_ed_move_to(ed, to, v->motion.extend);
				wf_vim_normal_cursor(s);
			}
			wf_vim_dot_after(s);
			return true;
		}
		if (!v->motion.right && !v->motion.at.col &&
		        v->motion.at.line) {
			v->motion.at.line--;
			v->motion.at.col =
			        ed->text.lines[v->motion.at.line].len;
		}
		wf_ed_word_scan_begin(
		        &v->motion.scan, v->motion.at.col, v->motion.right);
	}
	return false;
}

WfEsResult wf_vim_move(
        WfEditSession *s, WfEdCmd cmd, unsigned count, bool extend)
{
	WfEditor *ed = &wf_es_buffer(s)->ed;
	WfPos to = ed->cur;
	unsigned n = count ? count : 1;
	if (cmd == WF_CMD_WORD_LEFT || cmd == WF_CMD_WORD_RIGHT) {
		word_begin(s, cmd == WF_CMD_WORD_RIGHT, n, extend, 0);
		(void)vim_word_slice(s);
		return WF_ES_REDRAW;
	} else if (cmd == WF_CMD_HOME) {
		to.col = 0;
	} else if (cmd == WF_CMD_END) {
		to.line += n - 1 > last_line(ed) - to.line
		        ? last_line(ed) - to.line
		        : n - 1;
		const WfELine *l = &ed->text.lines[to.line];
		to.col = l->len ? wf_et_col_prev(l, l->len) : 0;
	} else if (cmd == WF_CMD_DOC_START || cmd == WF_CMD_DOC_END) {
		to.line = count                 ? (size_t)count - 1
		        : cmd == WF_CMD_DOC_END ? last_line(ed)
		                                : 0;
		if (to.line > last_line(ed)) {
			to.line = last_line(ed);
		}
		to.col = 0;
		const WfELine *l = &ed->text.lines[to.line];
		while (to.col < l->len &&
		        char_class((unsigned char)l->s[to.col]) == 0) {
			to.col++;
		}
	} else if (cmd == WF_CMD_LEFT || cmd == WF_CMD_RIGHT) {
		const WfELine *l = &ed->text.lines[to.line];
		for (unsigned i = 0; i < n; i++) {
			if (cmd == WF_CMD_LEFT && to.col) {
				to.col = wf_et_col_prev(l, to.col);
			} else if (cmd == WF_CMD_RIGHT && to.col < l->len) {
				size_t next = wf_et_col_next(l, to.col);
				if (next < l->len) {
					to.col = next;
				}
			}
		}
	} else {
		for (unsigned i = 0; i < n; i++) {
			wf_ed_move(ed,
			        cmd == WF_CMD_UP             ? WF_MV_UP
			                : cmd == WF_CMD_DOWN ? WF_MV_DOWN
			                : cmd == WF_CMD_PGUP ? WF_MV_PAGE_UP
			                                     : WF_MV_PAGE_DOWN,
			        extend, s->rows > 4 ? (size_t)s->rows - 4 : 1);
		}
		wf_vim_normal_cursor(s);
		return WF_ES_REDRAW;
	}
	wf_ed_move_to(ed, to, extend);
	wf_vim_normal_cursor(s);
	return WF_ES_REDRAW;
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
	unsigned count = total_count(v);

	v->wait = 0;
	v->op = 0;
	v->count = v->count2 = 0;
	switch (ch) {
	case 'w': {
		WfEditor view = *ed;
		ok = object_word(&view, around, &from, &to);
		for (unsigned i = 1; ok && i < count; i++) {
			view.cur = to;
			if (view.cur.col ==
			                view.text.lines[view.cur.line].len &&
			        view.cur.line < last_line(ed)) {
				view.cur.line++;
				view.cur.col = 0;
			}
			WfPos a, b;
			if (!object_word(&view, around, &a, &b) ||
			        wf_pos_cmp(b, to) <= 0) {
				break;
			}
			to = b;
		}
		break;
	}
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
	if (!op && v->visual) {
		ed->anchor = from;
		ed->cur = to;
		if (to.col) {
			ed->cur.col = wf_et_col_prev(
			        &ed->text.lines[to.line], to.col);
		}
		ed->has_anchor = true;
		ed->mark = false;
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
	case 'b':
		word_begin(s, motion == 'w', n, false, op);
		(void)vim_word_slice(s);
		return WF_ES_REDRAW;
	case 'e': return chars_op(s, op, ed->cur, word_end(ed, ed->cur, n));
	case '$':
		if (n > 1 && !ed->cur.col && op == 'd') {
			return lines_op(s, op, cur, cur + n - 1);
		}
		return chars_op(s, op, ed->cur,
		        (WfPos){cur + n - 1 < last ? cur + n - 1 : last,
		                ed->text.lines[cur + n - 1 < last ? cur + n - 1
		                                                  : last]
		                        .len});
	case '0': return chars_op(s, op, (WfPos){ed->cur.line, 0}, ed->cur);
	case 'h':
	case 'l': {
		const WfELine *l = &ed->text.lines[cur];
		t = ed->cur;
		for (unsigned i = 0; i < n; i++) {
			if (motion == 'h' && t.col) {
				t.col = wf_et_col_prev(l, t.col);
			}
			if (motion == 'l' && t.col < l->len) {
				t.col = wf_et_col_next(l, t.col);
			}
		}
		return chars_op(s, op, ed->cur, t);
	}
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
	case WF_CMD_VIM_YANK_LINE:
	case WF_CMD_VIM_CUT_LINE:
		return lines_op(s, cmd == WF_CMD_VIM_YANK_LINE ? 'y' : 'd',
		        ed->cur.line, ed->cur.line + (count ? count : 1) - 1);
	case WF_CMD_VIM_DELETE_CHAR: {
		if (v->visual) {
			return visual_op(s, 'd');
		}
		const WfELine *l = &ed->text.lines[ed->cur.line];
		WfPos to = ed->cur;
		for (unsigned i = 0; i < (count ? count : 1) && to.col < l->len;
		        i++) {
			to.col = wf_et_col_next(l, to.col);
		}
		return chars_op(s, 'd', ed->cur, to);
	}
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

		v->op = op;
		v->count = count;
		v->count2 = 0;
		return run_motion(s, '$');
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

static bool replay_begin(WfEditSession *s,
        const WfEvent *events,
        size_t n,
        unsigned times,
        bool dot)
{
	WfVim *v = &s->vim;
	if (!n || v->playback.ev) {
		return false;
	}
	WfEvent *copy = wf_et_malloc(n * sizeof *copy);
	if (!copy) {
		wf_es_fail_status(s, WF_E_NOMEM);
		return false;
	}
	memcpy(copy, events, n * sizeof *copy);
	v->playback.ev = copy;
	v->playback.n = n;
	v->playback.at = 0;
	v->playback.left = times ? times : 1;
	v->playback.buffer = wf_es_buffer(s);
	v->playback.gen = wf_es_buffer(s)->ed.text.gen;
	v->playing = !dot;
	v->replaying = dot;
	return true;
}

static void play(WfEditSession *s, int reg, unsigned times)
{
	if (s->vim.playing || s->vim.replaying) {
		return;
	}
	(void)replay_begin(
	        s, s->vim.macro[reg].ev, s->vim.macro[reg].n, times, false);
}

void wf_vim_stop(WfEditSession *s)
{
	WfVim *v = &s->vim;
	v->motion.active = false;
	free(v->playback.ev);
	memset(&v->playback, 0, sizeof v->playback);
	v->playing = v->replaying = false;
	v->cand_ok = false;
	wf_vim_cancel(v);
}

bool wf_vim_tick(WfEditSession *s)
{
	WfVim *v = &s->vim;
	if (v->playback.ev &&
	        (v->playback.buffer != wf_es_buffer(s) || !wf_es_buffer(s) ||
	                v->playback.gen != wf_es_buffer(s)->ed.text.gen)) {
		wf_vim_stop(s);
		wf_es_fail_status(s, WF_E_CHANGED);
		return true;
	}
	bool redraw = vim_word_slice(s);
	if (v->playback.ev) {
		v->playback.gen = wf_es_buffer(s)->ed.text.gen;
	}
	for (unsigned i = 0; v->playback.ev && !v->motion.active &&
	        !s->motion.active && i < 16;
	        i++) {
		WfEvent ev = v->playback.ev[v->playback.at++];
		s->input_draining = true;
		(void)wf_es_event(s, &ev, s->rows, s->cols);
		s->input_draining = false;
		redraw = true;
		if (!wf_es_buffer(s)) {
			wf_vim_stop(s);
			break;
		}
		v->playback.buffer = wf_es_buffer(s);
		v->playback.gen = wf_es_buffer(s)->ed.text.gen;
		if (v->playback.at == v->playback.n) {
			v->playback.at = 0;
			if (--v->playback.left == 0) {
				free(v->playback.ev);
				memset(&v->playback, 0, sizeof v->playback);
				v->playing = v->replaying = false;
				v->dot_skip = true;
			}
		}
	}
	return redraw;
}

bool wf_vim_follow(WfEditSession *s, const WfEvent *ev, WfEsResult *res)
{
	WfVim *v = &s->vim;
	uint32_t ch;

	if (!v->wait && v->visual && WF_EV_CHAR == ev->type &&
	        (ev->ch == 'i' || ev->ch == 'a') && !(ev->vk & WF_VK_PASTED)) {
		v->wait = ev->ch == 'i' ? 'I' : 'A';
		v->op = 0;
		*res = WF_ES_REDRAW;
		return true;
	}
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

			WfEvent *events =
			        wf_et_malloc(WF_VIM_MACRO_MAX * sizeof *events);
			if (events) {
				free(v->macro[r].ev);
				v->macro[r].ev = events;
				v->macro[r].n = 0;
				v->recording = r;
				v->macro_over = false;
			} else {
				wf_es_fail_status(s, WF_E_NOMEM);
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
			unsigned next = v->count2 * 10 + (ch - '0');
			v->count2 = next > 9999 ? 9999 : next;
			return true;
		}
		if ('0' == ch && v->count2) {
			unsigned next = v->count2 * 10;
			v->count2 = next > 9999 ? 9999 : next;
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
	        !s->vim.visual && !s->vim.motion.active && PR_NONE == s->pk &&
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
	if (s->vim.playing || s->vim.replaying) {
		return;
	}
	(void)replay_begin(s, s->vim.dot, s->vim.dot_n, times, true);
}

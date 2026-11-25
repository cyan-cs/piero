#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../base/wf_buf.h"
#include "../i18n/wf_i18n.h"
#include "../path/wf_path.h"
#include "../render/wf_theme.h"
#include "../text/wf_text.h"
#include "wf_esession_internal.h"
#include "../base/wf_macro.h"

/* Every colour comes from the active theme (render/wf_theme.h). */
#define THEME (wf_theme_styles())
#define BG (THEME->base.bg_rgb)
#define S_TEXT (THEME->base)
#define S_DIM (THEME->dim)
#define S_GUTTER (THEME->gutter)
#define S_GUTTER_CUR (THEME->gutter_current)
#define S_BAR (THEME->bar)
#define S_BAR_BOLD (THEME->bar_bold)
#define S_ERROR (THEME->error)
#define S_NOTE (THEME->note)
#define S_PROMPT (THEME->prompt)
#define S_PROMPT_LABEL (THEME->prompt_label)
/* syntax class -> foreground */
#define BG_SELECTION (THEME->text_selection.bg_rgb)
#define FG_SELECTION (THEME->text_selection.fg_rgb)
#define BG_CURSOR_LINE (THEME->cursor_line.bg_rgb)
#define BG_MATCH (THEME->match.bg_rgb)
#define FG_WHITESPACE (THEME->whitespace.fg_rgb)
#define FG_SPECIAL (THEME->error.bg_rgb)

#define SYNTAX_LINE_MAX WF_SYN_LINE_MAX

static int digits_of(size_t n)
{
	int d = 1;

	while (n >= 10) {
		n /= 10;
		d++;
	}
	return d;
}

static int gutter_width(const WfEditSession *s, const WfBuffer *b)
{
	return s->cfg.line_numbers ? digits_of(b->ed.text.n) + 1 : 0;
}

int wf_es_text_width(const WfEditSession *s, const WfBuffer *b)
{
	int side = wf_es_md_cols(s, b);
	int w;

	if (!side) {
		side = wf_es_git_cols(
		        s, b); /* one pane at the right, never two */
	}
	w = s->cols - side - gutter_width(s, b);

	return w > 1 ? w : 1;
}

static int text_rows(const WfEditSession *s)
{
	int th = s->rows - 3;

	return th > 1 ? th : 1;
}

static const char *eol_label(WfBuffer *b)
{
	size_t i, lf = 0, crlf = 0, cr = 0;
	const WfEText *t = &b->ed.text;

	if (b->eol_gen == t->shape_gen + 1 && b->eol_label[0]) {
		return b->eol_label;
	}
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
		snprintf(b->eol_label, sizeof b->eol_label, "%s",
		        WF_EOL_CRLF == t->default_eol         ? "CRLF"
		                : WF_EOL_CR == t->default_eol ? "CR"
		                                              : "LF");
	} else if ((0 != lf) + (0 != crlf) + (0 != cr) > 1) {
		snprintf(b->eol_label, sizeof b->eol_label, "%s",
		        wf_msg(MSG_ED_EOL_MIXED));
	} else {
		snprintf(b->eol_label, sizeof b->eol_label, "%s",
		        crlf ? "CRLF"
		                : cr ? "CR"
		                     : "LF");
	}
	b->eol_gen = t->shape_gen + 1;
	return b->eol_label;
}

static void put_right(WfFrame *f, int y, const char *text, const WfStyle *st)
{
	int w = wf_utf8_cells(text, strlen(text));

	if (w < f->cols) {
		wf_frame_put(f, f->cols - w, y, text, strlen(text), st, w);
	}
}

static void draw_header(WfEditSession *s, WfBuffer *b, WfFrame *f)
{
	WfBuf name;
	char left[300], right[96], enc[32];
	const wchar_t *leaf = L"";
	size_t len = 0;

	wf_frame_fill(f, 0, 0, f->cols, 1, ' ', &S_BAR);
	if (b->path) {
		wf_path_leaf(b->path, &leaf, &len);
	}
	wf_buf_init(&name);
	if (WF_OK != wf_sanitize_utf16(leaf, len, &name)) {
		wf_buf_puts(&name, "?");
	}
	snprintf(left, sizeof left, " %s%s%s%s", wf_buf_cstr(&name),
	        wf_ed_modified(&b->ed) ? wf_msg(MSG_ED_TAG_MODIFIED) : "",
	        b->ed.read_only ? wf_msg(MSG_ED_TAG_READONLY) : "",
	        b->on_disk ? "" : wf_msg(MSG_ED_TAG_NEW));
	wf_buf_free(&name);
	wf_enc_name(b->ed.enc, enc, sizeof enc);
	snprintf(right, sizeof right, "%s / %s / %s ", enc, eol_label(b),
	        wf_syn_lang_name(b->lang));
	put_right(f, 0, right, &S_BAR);
	wf_frame_put(f, 0, 0, left, strlen(left), &S_BAR_BOLD,
	        f->cols - wf_utf8_cells(right, strlen(right)) - 1);
	(void)s;
}

/* The name of the keys in force; for a modal profile the mode, and the part
 * of a command typed so far. */
static void mode_text(const WfEditSession *s, char *out, size_t cap)
{
	size_t n;

	if (!s->cfg.keys) {
		snprintf(out, cap, "%s", wf_ekm_preset_name(s->cfg.keymap));
		return;
	}
	if (PR_EX == s->pk) {
		snprintf(out, cap, "%s", wf_msg(MSG_MODE_CMDLINE));
	} else if (PR_FIND == s->pk || PR_FIND_BACK == s->pk) {
		snprintf(out, cap, "%s", wf_msg(MSG_ED_MODE_SEARCH));
	} else if (wf_keymap_modal(s->cfg.keys)) {
		snprintf(out, cap, "%s",
		        wf_msg(s->vim_insert ? MSG_ED_MODE_INSERT
		                        : s->vim.visual
		                        ? (s->vim.vline ? MSG_ED_MODE_VISUAL_LINE
		                                        : MSG_ED_MODE_VISUAL)
		                        : MSG_ED_MODE_NORMAL));
		if (s->vim.recording >= 0) {
			char rec[48],
			        name[2] = {(char)('a' + s->vim.recording), 0};

			(void)wf_msgf(rec, sizeof rec, MSG_ED_MODE_RECORDING,
			        &WF_ARG_S(name), 1);
			snprintf(out + strlen(out), cap - strlen(out), " %s",
			        rec);
		}
	} else {
		snprintf(out, cap, "%s",
		        WF_PROFILE_CUSTOM == wf_keymap_profile(s->cfg.keys)
		                ? "CUSTOM"
		                : "NANO");
	}
	n = strlen(out);
	if (wf_keystate_active(&s->ks) && n + 2 < cap) {
		char typed[32] = "";

		if (s->ks.count) {
			snprintf(typed, sizeof typed, "%u",
			        (unsigned)s->ks.count);
		}
		wf_seq_format(&s->ks.pending, typed + strlen(typed),
		        sizeof typed - strlen(typed));
		snprintf(out + n, cap - n, " %s", typed);
	}
}

static void draw_status(WfEditSession *s, WfBuffer *b, WfFrame *f)
{
	char left[200], buf_info[32] = "";
	int y = f->rows - 2;
	const WfStyle *msg_style = s->msg_error ? &S_ERROR : &S_NOTE;
	size_t index = 0;
	int used;

	wf_frame_fill(f, 0, y, f->cols, 1, ' ', &S_BAR);
	if (s->n > 1) {
		snprintf(buf_info, sizeof buf_info, "  [%zu/%zu]", s->cur + 1,
		        s->n);
	}
	{
		const WfEditor *e = &b->ed;
		const char *state =
		        wf_msg(wf_ed_modified(e) ? MSG_ED_STATE_MODIFIED
		                                 : MSG_ED_STATE_SAVED);
		char position[96];

		index = e->cur.line + 1;
		(void)wf_msgf(position, sizeof position, MSG_ED_POSITION,
		        (WfMsgArg[]){WF_ARG_N(index), WF_ARG_N(e->text.n),
		                WF_ARG_N(wf_ed_cursor_col(e))},
		        3);
		char mode[64];

		mode_text(s, mode, sizeof mode);
		snprintf(left, sizeof left, " %s  |  %s  |  %s%s", position,
		        mode, state, buf_info);
	}
	wf_frame_put(f, 0, y, left, strlen(left), &S_BAR, f->cols);
	used = wf_utf8_cells(left, strlen(left));
	if (s->msg[0] && used + 3 < f->cols) {
		wf_frame_put(f, used + 2, y, s->msg, strlen(s->msg), msg_style,
		        f->cols - used - 2);
	}
}

typedef struct
{
	WfEdCmd cmd;
	WfMsg label;
} Hint;

static void draw_hints(WfEditSession *s, WfFrame *f)
{
	static const Hint HINTS_NORMAL[] = {{WF_CMD_SAVE, MSG_ED_HINT_SAVE},
	        {WF_CMD_FIND, MSG_ED_HINT_FIND},
	        {WF_CMD_UNDO, MSG_ED_HINT_UNDO},
	        {WF_CMD_REDO, MSG_ED_HINT_REDO},
	        {WF_CMD_VIM_OP_DELETE, MSG_ED_HINT_CUT},
	        {WF_CMD_VIM_OP_YANK, MSG_ED_HINT_COPY},
	        {WF_CMD_VIM_PASTE_AFTER, MSG_ED_HINT_PASTE},
	        {WF_CMD_VIM_CMDLINE, MSG_ED_HINT_CMDLINE},
	        {WF_CMD_HELP, MSG_ED_HINT_HELP}};
	static const Hint HINTS_INSERT[] = {
	        {WF_CMD_VIM_NORMAL, MSG_ED_HINT_NORMAL},
	        {WF_CMD_SAVE, MSG_ED_HINT_SAVE}};
	static const Hint HINTS[] = {{WF_CMD_SAVE, MSG_ED_HINT_SAVE},
	        {WF_CMD_FIND, MSG_ED_HINT_FIND},
	        {WF_CMD_REPLACE, MSG_ED_HINT_REPLACE},
	        {WF_CMD_GOTO, MSG_ED_HINT_GOTO},
	        {WF_CMD_UNDO, MSG_ED_HINT_UNDO},
	        {WF_CMD_REDO, MSG_ED_HINT_REDO}, {WF_CMD_CUT, MSG_ED_HINT_CUT},
	        {WF_CMD_COPY, MSG_ED_HINT_COPY},
	        {WF_CMD_PASTE, MSG_ED_HINT_PASTE},
	        {WF_CMD_EXIT, MSG_ED_HINT_EXIT},
	        {WF_CMD_HELP, MSG_ED_HINT_HELP}};
	int y = f->rows - 1, x = 1;
	size_t i, count = WF_ARRAY_LEN(HINTS);
	const Hint *hints = HINTS;
	WfKeyContext ctx = WF_CTX_EDIT;

	if (s->cfg.keys) {
		ctx = wf_keymap_modal(s->cfg.keys)
		        ? (s->vim_insert ? WF_CTX_VIM_INSERT
		                         : WF_CTX_VIM_NORMAL)
		        : WF_CTX_EDIT;
		if (WF_CTX_VIM_NORMAL == ctx) {
			hints = HINTS_NORMAL;
			count = WF_ARRAY_LEN(HINTS_NORMAL);
		} else if (WF_CTX_VIM_INSERT == ctx) {
			hints = HINTS_INSERT;
			count = WF_ARRAY_LEN(HINTS_INSERT);
		}
	}
	wf_frame_fill(f, 0, y, f->cols, 1, ' ', &S_DIM);
	for (i = 0; i < count; i++) {
		const char *key;
		char item[48] = {0};
		const char *label = wf_msg(hints[i].label);
		int w, lw = wf_utf8_cells(label, strlen(label));

		if (s->cfg.keys) {
			wf_keymap_first_key(s->cfg.keys, ctx,
			        (uint16_t)hints[i].cmd, item, sizeof item);
			key = item[0] ? item : NULL;
		} else {
			key = wf_ekm_key_name(s->cfg.keymap, hints[i].cmd);
			if (WF_CMD_HELP == hints[i].cmd) {
				key = "F2";
			}
		}
		if (!key) {
			continue;
		}
		if (key != item) {
			snprintf(item, sizeof item, "%s", key);
		}
		w = wf_utf8_cells(item, strlen(item));
		if (x + w + 1 + lw + 2 > f->cols) {
			break;
		}
		wf_frame_put(f, x, y, item, strlen(item), &S_BAR_BOLD, w);
		x += w + 1;
		wf_frame_put(
		        f, x, y, label, strlen(label), &S_TEXT, f->cols - x);
		x += lw + 2;
	}
}

static void draw_prompt(WfEditSession *s, WfFrame *f)
{
	int y = f->rows - 1, x = 0;
	char label[160];
	bool find = PR_FIND == s->pk || PR_FIND_BACK == s->pk ||
	        PR_REPL_FIND == s->pk;
	bool text = PR_FIND == s->pk || PR_FIND_BACK == s->pk ||
	        PR_REPL_FIND == s->pk || PR_REPL_WITH == s->pk ||
	        PR_GOTO == s->pk || PR_SAVE_AS == s->pk || PR_OPEN == s->pk ||
	        PR_INSERT == s->pk || PR_EX == s->pk;

	wf_frame_fill(f, 0, y, f->cols, 1, ' ', &S_PROMPT);
	if (!text) {
		int used = wf_frame_put(f, 1, y, s->label, strlen(s->label),
		        &S_PROMPT_LABEL, f->cols - 1);
		bool asks = PR_QUIT == s->pk || PR_CONFLICT == s->pk ||
		        PR_GONE == s->pk || PR_ENCODING == s->pk ||
		        PR_RECOVER == s->pk || PR_DRAFT == s->pk ||
		        PR_LOCKED == s->pk;

		if (asks && s->choice.n && 1 + used + 2 < f->cols) {
			/* the answer Enter gives, so Up/Down is not a blind move */
			char letter[2] = {0, 0}, hint[96];

			letter[0] = (char)(wf_choice_selected(&s->choice) - 32);
			(void)wf_msgf(hint, sizeof hint, MSG_CHOICE_HINT,
			        &WF_ARG_S(letter), 1);
			wf_frame_put(f, 1 + used + 2, y, hint, strlen(hint),
			        &S_PROMPT_LABEL, f->cols - 1 - used - 2);
		}
		return;
	}
	if (PR_EX == s->pk) {
		snprintf(label, sizeof label, ":");
	} else {
		snprintf(label, sizeof label, " %s%s%s: ", s->label,
		        find && s->f_case ? wf_msg(MSG_ED_FIND_CASE) : "",
		        find && s->f_word ? wf_msg(MSG_ED_FIND_WORD) : "");
	}
	x = wf_frame_put(
	        f, 0, y, label, strlen(label), &S_PROMPT_LABEL, f->cols);
	{
		/* What was typed (or taken from the text by a selection) is data:
		 * it is shown the way the text is, controls made visible, never
		 * sent to the terminal as they are. */
		WfBuf before_b, cur_b, after_b;
		size_t cur_n = 0, after_from;
		uint32_t cp;
		int avail = f->cols - x - 1, shown_before;
		const char *before_s;
		size_t before_n, from = 0;
		WfStyle cur = S_PROMPT;

		wf_buf_init(&before_b);
		wf_buf_init(&cur_b);
		wf_buf_init(&after_b);
		if (s->in_cur < s->in_n) {
			cur_n = wf_utf8_decode(
			        s->in + s->in_cur, s->in_n - s->in_cur, &cp);
		}
		after_from = s->in_cur + cur_n;
		if (WF_OK != wf_sanitize_utf8(s->in, s->in_cur, &before_b) ||
		        WF_OK !=
		                wf_sanitize_utf8(
		                        s->in + s->in_cur, cur_n, &cur_b) ||
		        wf_sanitize_utf8(s->in + after_from,
		                s->in_n - after_from, &after_b) != WF_OK) {
			goto prompt_done;
		}
		before_s = wf_buf_cstr(&before_b);
		before_n = before_b.len;
		shown_before = wf_utf8_cells(before_s, before_n);
		/* keep the cursor in view: drop the start of long input */
		while (shown_before >= avail && from < before_n) {
			size_t n = wf_utf8_decode(
			        before_s + from, before_n - from, &cp);

			shown_before -= wf_cp_width(cp);
			from += n ? n : 1;
		}
		x += wf_frame_put(f, x, y, before_s + from, before_n - from,
		        &S_PROMPT, avail);
		cur.attributes |= WF_ATTR_REVERSE;
		wf_frame_set_cursor(f, x, y);
		if (cur_n) {
			int w = wf_utf8_cells(wf_buf_cstr(&cur_b), cur_b.len);

			wf_frame_put(f, x, y, wf_buf_cstr(&cur_b), cur_b.len,
			        &cur, w ? w : 1);
			x += w ? w : 1;
			wf_frame_put(f, x, y, wf_buf_cstr(&after_b),
			        after_b.len, &S_PROMPT, f->cols - x);
		} else {
			wf_frame_put(f, x, y, " ", 1, &cur, 1);
		}
	prompt_done:
		wf_buf_free(&before_b);
		wf_buf_free(&cur_b);
		wf_buf_free(&after_b);
	}
}

typedef struct
{
	WfEditSession *s;
	WfBuffer *b;
	WfFrame *f;
	size_t sel_from_line, sel_to_line;
	WfPos sel_a, sel_b;
	bool selected;
	bool bracket;
	WfPos br_open, br_close;
	WfSearch q;
	bool matches;
	unsigned tab;
	/* the matches of the line being drawn, one at a time: where the next one
	 * is (m_at) and whether there is one (m_have). The rows of a line are drawn
	 * in order, so this carries over from one segment to the next. */
	bool m_init;
	size_t m_at;
	bool m_have;
} View;

static WfStyle style_for(const View *v,
        size_t line,
        size_t col,
        uint8_t tok,
        bool in_sel,
        bool in_match,
        bool cur_line)
{
	WfStyle st = wf_theme_syntax_style(tok);

	if (cur_line) {
		st.bg_rgb = BG_CURSOR_LINE;
	}
	if (in_match) {
		st.bg_rgb = BG_MATCH;
		st.fg_rgb = BG;
	}
	if (in_sel) {
		st.bg_rgb = BG_SELECTION;
		st.fg_rgb = FG_SELECTION;
	}
	if (v->bracket &&
	        ((line == v->br_open.line && col == v->br_open.col) ||
	                (line == v->br_close.line && col == v->br_close.col))) {
		st.attributes |= WF_ATTR_UNDERLINE | WF_ATTR_BOLD;
		st.bg_rgb = BG_SELECTION;
	}
	return st;
}

/*
 * Draws bytes [from, to) of one line into row `y`, starting at column x0 with
 * `w` columns, after skipping `skip` cells at the left. `cells` counts cells
 * from the start of the segment (for tabs). Returns the cell reached.
 */
static void draw_segment(View *v,
        size_t line,
        size_t from,
        size_t to,
        int x0,
        int y,
        int w,
        size_t skip,
        const uint8_t *syn,
        bool cur_line)
{
	const WfELine *l = &v->b->ed.text.lines[line];
	size_t i = from, cells = 0;
	bool in_match = false;
	bool show_ws = v->s->cfg.show_whitespace;

	/* jump to the left edge instead of walking every scalar before it */
	if (skip && 0 == from) {
		i = wf_et_col_at_cells(l, skip, v->tab);
		cells = wf_et_cells(l, i, v->tab);
	}

	if (v->matches && !v->m_init) {
		v->m_have = wf_es_next_match(l, &v->q, 0, &v->m_at);
		v->m_init = true;
	}
	while (i < to) {
		uint32_t cp = 0;
		size_t next = wf_et_col_next(l, i),
		       n = wf_utf8_decode(l->s + i, l->len - i, &cp);
		size_t width;
		bool in_sel = false;
		WfStyle st;
		char glyph[64];
		size_t glyph_n;

		if (next <= i) {
			next = i + (n ? n : 1);
		}
		if (next > to) {
			next = to;
		}
		/* a match that began before this segment may reach into it */
		while (v->matches && v->m_have && v->m_at + v->q.pat_n <= i) {
			v->m_have = wf_es_next_match(
			        l, &v->q, v->m_at + v->q.pat_n, &v->m_at);
		}
		in_match = v->matches && v->m_have && i >= v->m_at &&
		        i < v->m_at + v->q.pat_n;
		if (v->selected) {
			WfPos here = {line, i};

			in_sel = wf_pos_cmp(here, v->sel_a) >= 0 &&
			        wf_pos_cmp(here, v->sel_b) < 0;
		}
		st = style_for(v, line, i, syn ? syn[i] : 0, in_sel, in_match,
		        cur_line);
		if ('\t' == cp) {
			size_t stop = v->tab ? v->tab - cells % v->tab : 1;

			width = stop;
			if (show_ws) {
				st.fg_rgb = FG_WHITESPACE;
				/* U+2192 and U+00B7 are ambiguous-width: with a
				 * wide setting, plain ASCII marks keep the
				 * columns where the terminal draws them */
				const bool narrow = 1 == wf_cp_width(0x2192);
				const size_t mark = narrow ? 3 : 1;

				memcpy(glyph, narrow ? "\xE2\x86\x92" : ">",
				        mark);
				glyph_n = mark;
				while (glyph_n < width + (narrow ? 2 : 0) &&
				        glyph_n < sizeof glyph - 1) {
					glyph[glyph_n++] = ' ';
				}
			} else {
				glyph_n = width < sizeof glyph
				        ? width
				        : sizeof glyph - 1;
				memset(glyph, ' ', glyph_n);
			}
		} else if (' ' == cp && show_ws) {
			width = 1;
			st.fg_rgb = FG_WHITESPACE;
			if (1 == wf_cp_width(0xB7)) {
				memcpy(glyph, "\xC2\xB7", 2); /* U+00B7 */
				glyph_n = 2;
			} else {
				glyph[0] = '.';
				glyph_n = 1;
			}
		} else if (wf_et_special_scalar(cp)) {
			WfBuf clean;

			wf_buf_init(&clean);
			(void)wf_sanitize_utf8(l->s + i, next - i, &clean);
			glyph_n = clean.len < sizeof glyph - 1
			        ? clean.len
			        : sizeof glyph - 1;
			memcpy(glyph, clean.data ? clean.data : "", glyph_n);
			wf_buf_free(&clean);
			st.fg_rgb = FG_SPECIAL;
			width = (size_t)wf_utf8_cells(glyph, glyph_n);
		} else {
			size_t cn = next - i;

			width = (size_t)wf_cp_width(cp);
			glyph_n = cn < sizeof glyph - 1 ? cn : sizeof glyph - 1;
			memcpy(glyph, l->s + i, glyph_n);
		}
		glyph[glyph_n] = '\0';
		if (cells + width > skip) {
			if (cells <
			        skip) { /* cut by the left edge: blank the visible part */
				int vis = (int)(cells + width - skip);
				char blanks[8];

				memset(blanks, ' ', sizeof blanks);
				wf_frame_put(v->f, x0, y, blanks,
				        (size_t)(vis < 8 ? vis : 8), &st, w);
			} else {
				int at = x0 + (int)(cells - skip);

				if (at >= x0 + w) {
					break;
				}
				wf_frame_put(v->f, at, y, glyph, glyph_n, &st,
				        x0 + w - at);
			}
		}
		cells += width;
		if (cells >= skip + (size_t)w) {
			break;
		}
		i = next;
	}
}

/* The soft wrap of the lines on screen is kept in the buffer (WrapCache): the
 * rows of a line are worked out again only when the text, the width or the tab
 * width has changed since they were. */
static void wrap_cache_free(WrapCache *c)
{
	for (size_t i = 0; i < c->n; i++) {
		free(c->e[i].starts);
	}
	c->n = 0;
	c->next = 0;
	c->keyed = false;
}

void wf_es_wrap_free(WfBuffer *b)
{
	wrap_cache_free(&b->wrap);
}

/* Keeps what the cache holds if it was made for this text, width and tab
 * width, and empties it if not. */
static void wrap_cache_for(
        WrapCache *c, uint64_t gen, size_t width, unsigned tab)
{
	if (c->keyed && c->gen == gen && c->width == width && c->tab == tab) {
		return;
	}
	wrap_cache_free(c);
	c->keyed = true;
	c->gen = gen;
	c->width = width;
	c->tab = tab;
}

/* The rows of `line` and where each starts; 1 and NULL when the memory for them
 * is not there (the line is then drawn as one row). */
static size_t wrap_rows(WrapCache *c,
        size_t line,
        const WfELine *l,
        size_t tw,
        unsigned tab,
        const size_t **starts)
{
	WrapEntry *w;

	for (size_t i = 0; i < c->n; i++) {
		if (c->e[i].line == line) {
			*starts = c->e[i].starts;
			return c->e[i].rows;
		}
	}
	if (c->n < WRAP_CACHE_LINES) {
		w = &c->e[c->n++];
	} else {
		w = &c->e[c->next];
		c->next = (c->next + 1) % WRAP_CACHE_LINES;
		free(w->starts);
	}
	w->line = line;
	w->starts = NULL;
	w->rows = wf_et_wrap(l, tw, tab, &w->starts);
	if (!w->rows) {
		w->rows = 1;
	}
	*starts = w->starts;
	return w->rows;
}

static void draw_text(WfEditSession *s, WfBuffer *b, WfFrame *f, WrapCache *wc)
{
	WfEditor *e = &b->ed;
	int th = text_rows(s), gw = gutter_width(s, b),
	    tw = wf_es_text_width(s, b);
	View v;
	int y = 1;
	size_t line;
	uint8_t syn_buf[SYNTAX_LINE_MAX];

	memset(&v, 0, sizeof v);
	v.s = s;
	v.b = b;
	v.f = f;
	v.tab = e->opt.tab_width;
	v.selected = wf_ed_has_selection(e);
	if (v.selected) {
		wf_ed_selection(e, &v.sel_a, &v.sel_b);
	}
	v.bracket = wf_ed_match_bracket(e, &v.br_open, &v.br_close);
	if (s->hl && s->find_n) {
		v.q.pat = s->find;
		v.q.pat_n = s->find_n;
		v.q.match_case = s->f_case;
		v.q.whole_word = s->f_word;
		v.matches = true;
	}
	wf_frame_fill(f, 0, 1, f->cols, th, ' ', &S_TEXT);
	wf_es_syntax_sync(s, b);
	if (e->dirty_from != (size_t)-1) {
		wf_syn_invalidate(&b->syn, e->dirty_from);
		e->dirty_from = (size_t)-1;
	}
	for (line = b->top; line < e->text.n && y <= th; line++) {
		const WfELine *l = &e->text.lines[line];
		bool cur_line = line == e->cur.line;
		bool colored = s->cfg.syntax && l->len <= SYNTAX_LINE_MAX &&
		        wf_syn_line(&b->syn, &e->text, line, syn_buf,
		                sizeof syn_buf);
		const size_t *starts = NULL;
		size_t rows = 1, r, first;

		v.m_init =
		        false; /* the matches of this line are looked for afresh */

		if (e->wrap_cells) {
			rows = wrap_rows(wc, line, l, (size_t)tw,
			        e->opt.tab_width, &starts);
		}
		first = line == b->top && starts ? b->top_wrap : 0;
		if (first >= rows) {
			first = rows - 1;
		}
		if (gw) {
			char num[32];

			snprintf(num, sizeof num, "%*zu ", gw - 1, line + 1);
			wf_frame_put(f, 0, y, num, strlen(num),
			        cur_line ? &S_GUTTER_CUR : &S_GUTTER, gw);
		}
		if (cur_line && !v.selected) {
			wf_frame_fill(f, gw, y, tw,
			        (int)(rows - first < (size_t)(th - y + 1)
			                        ? rows - first
			                        : (size_t)(th - y + 1)),
			        ' ',
			        &(WfStyle){S_TEXT.fg_rgb, BG_CURSOR_LINE, 0});
		}
		for (r = first; r < rows && y + (int)(r - first) <= th; r++) {
			size_t from = starts ? starts[r] : 0;
			size_t to =
			        starts && r + 1 < rows ? starts[r + 1] : l->len;
			size_t skip = starts ? 0 : b->left;

			draw_segment(&v, line, from, to, gw,
			        y + (int)(r - first), tw, skip,
			        colored ? syn_buf : NULL,
			        cur_line && !v.selected);
			/* a selected line break shows as one selected cell */
			if (v.selected && r + 1 == rows &&
			        wf_pos_cmp((WfPos){line, l->len}, v.sel_a) >=
			                0 &&
			        wf_pos_cmp((WfPos){line, l->len}, v.sel_b) <
			                0) {
				size_t used = wf_et_cells(
				        l, l->len, e->opt.tab_width);
				size_t end = starts
				        ? wf_et_cells(
				                  &(WfELine){l->s + from,
				                          to - from, to - from,
				                          WF_EOL_NONE},
				                  to - from, e->opt.tab_width)
				        : used;

				if (end >= skip && end - skip < (size_t)tw) {
					wf_frame_put(f, gw + (int)(end - skip),
					        y + (int)(r - first), " ", 1,
					        &(WfStyle){FG_SELECTION,
					                BG_SELECTION, 0},
					        1);
				}
			}
			/* the cursor */
			if (cur_line && e->cur.col >= from &&
			        (e->cur.col < to || r + 1 == rows)) {
				WfELine sub = {l->s + from, e->cur.col - from,
				        e->cur.col - from, WF_EOL_NONE};
				size_t cc = wf_et_cells(
				        &sub, sub.len, e->opt.tab_width);
				uint32_t cp = 0;
				WfStyle cs = S_TEXT;
				char glyph[8] = " ";
				size_t gn = 1;

				if (e->cur.col < l->len) {
					size_t nx =
					        wf_et_col_next(l, e->cur.col);

					(void)wf_utf8_decode(l->s + e->cur.col,
					        l->len - e->cur.col, &cp);
					if (cp >= 0x20 && 0x7F != cp &&
					        nx - e->cur.col <
					                sizeof glyph) {
						gn = nx - e->cur.col;
						memcpy(glyph, l->s + e->cur.col,
						        gn);
					}
				}
				cs.attributes |= WF_ATTR_REVERSE;
				if (cc >= skip && cc - skip < (size_t)tw) {
					wf_frame_put(f, gw + (int)(cc - skip),
					        y + (int)(r - first), glyph, gn,
					        &cs, 2);
					wf_frame_set_cursor(f,
					        gw + (int)(cc - skip),
					        y + (int)(r - first));
				}
			}
		}
		y += (int)(rows - first);
	}
}

static void ensure_visible(WfEditSession *s, WfBuffer *b, WrapCache *wc)
{
	WfEditor *e = &b->ed;
	int th = text_rows(s), tw = wf_es_text_width(s, b);

	if (b->top >= e->text.n) {
		b->top = e->text.n - 1;
	}
	if (e->cur.line < b->top) {
		b->top = e->cur.line;
		b->top_wrap = 0;
	}
	if (e->wrap_cells) {
		size_t visible = 0, first_rows = 1, cursor_row = 0;
		b->left = 0;
		for (size_t line = b->top; line <= e->cur.line; line++) {
			const size_t *starts = NULL;
			size_t n = wrap_rows(wc, line, &e->text.lines[line],
			        (size_t)tw, e->opt.tab_width, &starts);
			if (line == b->top) {
				first_rows = n;
			}
			if (line == e->cur.line) {
				for (size_t r = 0; starts && r < n; r++) {
					if (starts[r] <= e->cur.col) {
						cursor_row = r;
					}
				}
				visible += cursor_row + 1;
			} else {
				visible += n;
			}
		}
		if (b->top_wrap >= first_rows) {
			b->top_wrap = first_rows - 1;
		}
		if (b->top == e->cur.line && cursor_row < b->top_wrap) {
			b->top_wrap = cursor_row;
		}
		visible -= b->top_wrap;
		while (visible > (size_t)th) {
			size_t excess = visible - (size_t)th;
			if (excess < first_rows - b->top_wrap ||
			        b->top == e->cur.line) {
				b->top_wrap += excess;
				break;
			}
			visible -= first_rows - b->top_wrap;
			b->top++;
			b->top_wrap = 0;
			const size_t *starts = NULL;
			first_rows =
			        wrap_rows(wc, b->top, &e->text.lines[b->top],
			                (size_t)tw, e->opt.tab_width, &starts);
		}
		return;
	}
	b->top_wrap = 0;
	if (e->cur.line >= b->top + (size_t)th) {
		b->top = e->cur.line - (size_t)th + 1;
	}
	{
		size_t cc = wf_et_cells(&e->text.lines[e->cur.line], e->cur.col,
		        e->opt.tab_width);

		if (cc < b->left) {
			b->left = cc;
		} else if (cc >= b->left + (size_t)tw) {
			b->left = cc - (size_t)tw + 1;
		}
		if (b->left && cc < (size_t)tw) {
			b->left = 0;
		}
	}
}

static const WfMsg HELP_PIERO[] = {MSG_ED_HELP_STD_TITLE, WF_MSG_COUNT,
        MSG_ED_HELP_STD_SAVE, MSG_ED_HELP_STD_FIND, MSG_ED_HELP_STD_REPLACE,
        MSG_ED_HELP_STD_COPY, MSG_ED_HELP_STD_DELETE, MSG_ED_HELP_STD_COMMENT,
        MSG_ED_HELP_STD_BUFFERS, MSG_ED_HELP_STD_WORDS, MSG_ED_HELP_MD,
        WF_MSG_COUNT, MSG_ED_HELP_STD_PROMPT1, MSG_ED_HELP_STD_PROMPT2,
        WF_MSG_COUNT, MSG_ED_HELP_RETURN};

static const WfMsg HELP_NANO[] = {MSG_ED_HELP_NANO_TITLE, WF_MSG_COUNT,
        MSG_ED_HELP_NANO_SAVE, MSG_ED_HELP_NANO_FIND, MSG_ED_HELP_NANO_REPLACE,
        MSG_ED_HELP_NANO_CUT, MSG_ED_HELP_NANO_MARK, MSG_ED_HELP_NANO_MOVE,
        MSG_ED_HELP_NANO_BUFFERS, MSG_ED_HELP_MD, WF_MSG_COUNT,
        MSG_ED_HELP_NANO_PROMPT1, MSG_ED_HELP_NANO_PROMPT2, WF_MSG_COUNT,
        MSG_ED_HELP_RETURN};

typedef struct
{
	WfFrame *f;
	int first, rows;
	size_t line;
} HelpDraw;

static void help_sink(
        void *user, const char *keys, const char *desc, bool header)
{
	HelpDraw *h = user;

	if ((int)h->line >= h->first && (int)h->line < h->first + h->rows) {
		int y = 1 + (int)h->line - h->first;

		if (header) {
			wf_frame_put(h->f, 1, y, desc, strlen(desc),
			        &S_BAR_BOLD, h->f->cols - 1);
		} else {
			wf_frame_put(h->f, 2, y, keys, strlen(keys),
			        &S_BAR_BOLD, 26);
			wf_frame_put(h->f, 28, y, desc, strlen(desc), &S_TEXT,
			        h->f->cols - 28);
		}
	}
	h->line++;
}

static size_t help_keyed_contexts(const WfEditSession *s, WfKeyContext *c)
{
	if (wf_keymap_modal(s->cfg.keys)) {
		c[0] = WF_CTX_VIM_NORMAL;
		c[1] = WF_CTX_VIM_INSERT;
		return 2;
	}
	c[0] = WF_CTX_EDIT;
	return 1;
}

static void count_sink(
        void *user, const char *keys, const char *desc, bool header)
{
	(void)keys;
	(void)desc;
	(void)header;
	(*(size_t *)user)++;
}

static void draw_help_keyed(WfEditSession *s, WfFrame *f)
{
	WfKeyContext ctx[2];
	size_t n = help_keyed_contexts(s, ctx), total = 0;
	HelpDraw h;

	wf_help_walk(s->cfg.keys, ctx, n, count_sink, &total);
	if (s->help_top + 1 > total) {
		s->help_top = total ? total - 1 : 0;
	}
	wf_frame_fill(f, 0, 1, f->cols, f->rows - 3, ' ', &S_TEXT);
	h.f = f;
	h.first = (int)s->help_top;
	h.rows = f->rows - 3;
	h.line = 0;
	wf_help_walk(s->cfg.keys, ctx, n, help_sink, &h);
}

static void draw_help(WfEditSession *s, WfFrame *f)
{
	const WfMsg *lines =
	        WF_KM_NANO == s->cfg.keymap ? HELP_NANO : HELP_PIERO;
	size_t total = WF_KM_NANO == s->cfg.keymap ? WF_ARRAY_LEN(HELP_NANO)
	                                           : WF_ARRAY_LEN(HELP_PIERO);
	size_t i;
	int y;

	if (s->cfg.keys) {
		draw_help_keyed(s, f);
		return;
	}
	if (s->help_top + 1 > total) {
		s->help_top = total ? total - 1 : 0;
	}
	wf_frame_fill(f, 0, 1, f->cols, f->rows - 3, ' ', &S_TEXT);
	for (i = s->help_top, y = 1; i < total && y < f->rows - 2; i++, y++) {
		const char *text =
		        WF_MSG_COUNT == lines[i] ? "" : wf_msg(lines[i]);

		wf_frame_put(f, 1, y, text, strlen(text),
		        0 == i ? &S_BAR_BOLD : &S_TEXT, f->cols - 1);
	}
}

void wf_es_draw(WfEditSession *s, WfFrame *f)
{
	WfBuffer *b = wf_es_buffer(s);
	WrapCache *wc;

	s->rows = f->rows;
	s->cols = f->cols;
	wf_frame_fill(f, 0, 0, f->cols, f->rows, ' ', &S_TEXT);
	if (!b) { /* only a question is on screen (an unknown encoding) */
		if (PR_NONE != s->pk && f->rows > 0) {
			draw_prompt(s, f);
		}
		return;
	}
	if (f->cols < 24 || f->rows < 6) {
		wf_frame_put(f, 0, 0, wf_msg(MSG_ED_TOO_SMALL),
		        strlen(wf_msg(MSG_ED_TOO_SMALL)), &S_ERROR, f->cols);
		return;
	}
	b->ed.wrap_cells =
	        s->cfg.soft_wrap ? (size_t)wf_es_text_width(s, b) : 0;
	wc = &b->wrap;
	if (b->ed.wrap_cells) {
		wrap_cache_for(wc, b->ed.text.gen, b->ed.wrap_cells,
		        b->ed.opt.tab_width);
	} else {
		wrap_cache_free(wc); /* no wrapping: nothing to keep */
	}
	ensure_visible(s, b, wc);
	draw_header(s, b, f);
	if (PR_HELP == s->pk) {
		draw_help(s, f);
	} else {
		draw_text(s, b, f, wc);
		wf_es_md_draw(s, b, f, text_rows(s));
		wf_es_git_draw(s, b, f, text_rows(s));
	}
	draw_status(s, b, f);
	if (PR_NONE != s->pk && PR_HELP != s->pk) {
		draw_prompt(s, f);
	} else {
		draw_hints(s, f);
	}
}

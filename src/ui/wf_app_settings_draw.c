/*
 * The settings screen, drawn: the three columns, the note and the hint rows.
 * What it shows is decided in wf_app_settings.c; this file only paints it.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../text/wf_text.h"
#include "wf_app_settings_priv.h"
#include "../base/wf_macro.h"

static void draw_note(const WfApp *app, WfFrame *f, int y)
{
	const WfThemeStyles *th = wf_theme_styles();

	if (app->settings.note[0]) {
		wf_app_put_fit(f, 1, y, app->settings.note,
		        app->settings.note_is_error ? &th->warning : &th->base,
		        f->cols - 2);
	} else if (app->settings.edit_theme >= 0 &&
	        app->settings.edit_theme < app->settings.draft.custom_count) {
		WfThemePalette p;
		char text[160], normal[16], selection[16];
		wf_theme_build_custom(
		        &app->settings.draft.customs[app->settings.edit_theme]
		                .theme,
		        &p);
		snprintf(normal, sizeof normal, "%.1f",
		        wf_color_contrast(p.foreground, p.background));
		snprintf(selection, sizeof selection, "%.1f",
		        wf_color_contrast(p.selection_foreground,
		                p.selection_background));
		(void)wf_msgf(text, sizeof text, MSG_THEME_CONTRAST,
		        (WfMsgArg[]){WF_ARG_S(normal), WF_ARG_S(selection)}, 2);
		wf_app_put_fit(f, 1, y, text, &th->dim, f->cols - 2);
	}
}

static void draw_sections(const WfApp *app, WfFrame *f, int x, int w)
{
	const WfThemeStyles *th = wf_theme_styles();
	int i;

	for (i = 0; i < WF_SEC_COUNT; i++) {
		bool on = i == app->settings.section;
		bool cursor = on && WF_FOCUS_SECTION == app->settings.focus;
		const WfStyle *st = cursor ? &th->selection
		        : on               ? &th->accent
		                           : &th->base;

		wf_frame_fill(f, x, 2 + i, w, 1, ' ', st);
		if (cursor) {
			wf_app_put_str(f, x, 2 + i, ">", st, 1);
		}
		wf_app_put_fit(f, x + 1, 2 + i, wf_msg(wf_set_section_names[i]),
		        st, w - 1);
	}
}

static void draw_entries(const WfApp *app, WfFrame *f, int x, int w)
{
	const WfThemeStyles *th = wf_theme_styles();
	size_t i, n = wf_set_entry_count(app),
	          h = (size_t)wf_set_view_rows(app), top = 0;
	size_t cur = (size_t)app->settings.category;

	if (cur >= top + h) {
		top = cur + 1 - h;
	}
	for (i = top; i < n && i < top + h; i++) {
		bool on = i == cur;
		bool cursor = on && WF_FOCUS_CATEGORY == app->settings.focus;
		const WfStyle *st = cursor ? &th->selection
		        : on               ? &th->accent
		                           : &th->base;
		char title[64];
		int y = 2 + (int)(i - top);

		if (WF_SEC_SETTINGS == app->settings.section) {
			snprintf(title, sizeof title, "%s",
			        wf_msg(wf_set_category_names[i]));
		} else {
			wf_about_title(app, app->settings.section, i, title,
			        sizeof title);
		}
		wf_frame_fill(f, x, y, w, 1, ' ', st);
		if (cursor) {
			wf_app_put_str(f, x, y, ">", st, 1);
		}
		wf_app_put_fit(f, x + 1, y, title, st, w - 1);
	}
}

/* The values of the choice under the cursor, in place of the rows. The value
 * the draft has now is marked with *, the cursor with > like every list. */
static void draw_pick(const WfApp *app, WfFrame *f, int x, int w)
{
	const WfThemeStyles *th = wf_theme_styles();
	const Item *it = wf_set_item_at(app, app->settings.item);
	int n = it && K_CHOICE == it->kind ? wf_set_choice_count(it->choices)
	                                   : 0;
	int rows = wf_set_view_rows(app) - 1, top = 0, now, i;

	if (!n) {
		return;
	}
	now = wf_set_get_int(&app->settings.draft, it);
	if (app->settings.pick >= rows) {
		top = app->settings.pick - rows + 1;
	}
	wf_app_put_fit(f, x + 1, 2, wf_msg(it->label), &th->dim, w - 1);
	for (i = top; i < n && i < top + rows; i++) {
		char label[96];
		bool on = i == app->settings.pick;
		const WfStyle *st = on ? &th->selection : &th->base;
		int y = 3 + (i - top);

		wf_set_choice_label(app, it, i, label, sizeof label);
		wf_frame_fill(f, x, y, w, 1, ' ', st);
		if (on) {
			wf_app_put_str(f, x, y, ">", st, 1);
		}
		wf_app_put_fit(f, x + 1, y, label, st, w - 4);
		if (i == now) {
			wf_app_put_str(f, x + w - 2, y, "*", st, 1);
		}
	}
}

static void draw_rows(const WfApp *app, WfFrame *f, int x, int w)
{
	const WfThemeStyles *th = wf_theme_styles();
	const WfSetPage *pg = wf_set_page_of(app);
	size_t i, n = pg->count(app), h = (size_t)wf_set_view_rows(app);
	int label_w = w * 11 / 20;

	if (label_w > 36) {
		label_w = 36;
	}
	for (i = app->settings.top; i < n && i < app->settings.top + h; i++) {
		WfSetRow r;
		bool on = i == app->settings.item &&
		        WF_FOCUS_DETAILS == app->settings.focus;
		const WfStyle *st;
		int y = 2 + (int)(i - app->settings.top);
		char value[96];

		pg->row(app, i, &r);
		st = on ? &th->selection
		        : r.header ? &th->accent
		        : r.dim    ? &th->dim
		                   : &th->base;
		wf_frame_fill(f, x, y, w, 1, ' ', st);
		if (on) {
			wf_app_put_str(f, x, y, ">", st, 1);
		}
		wf_app_put_fit(f, x + 1, y, r.label, st,
		        r.header ? w - 1 : label_w - 1);
		if (r.header) {
			continue;
		}
		snprintf(value, sizeof value, "%s", r.value);
		wf_app_put_fit(f, x + label_w, y, value, st,
		        w - label_w - (r.swatch ? 6 : 0));
		if (r.swatch && w > label_w + 12) {
			WfStyle sample = {0, r.rgb, 0};

			wf_frame_fill(f, x + w - 5, y, 4, 1, ' ', &sample);
		}
	}
}

static void draw_doc(const WfApp *app, WfFrame *f, int x, int w)
{
	const WfThemeStyles *th = wf_theme_styles();
	const char *p = app->settings.doc, *end, *line;
	size_t skip = app->settings.top, len;
	int y = 2, h = wf_set_view_rows(app), width = w > 2 ? w - 2 : 1;

	if (!p) {
		return;
	}
	end = p + app->settings.doc_n;
	while (p < end && skip) {
		p = wf_set_doc_row(p, end, width, &line, &len);
		skip--;
	}
	while (p < end && y < 2 + h) {
		char text[512];

		p = wf_set_doc_row(p, end, width, &line, &len);
		if (len >= sizeof text) {
			len = sizeof text - 1;
		}
		memcpy(text, line, len);
		text[len] = '\0';
		for (size_t i = 0; i < len; i++) {
			if ('\t' == text[i] || '\r' == text[i]) {
				text[i] = ' ';
			}
		}
		wf_app_put_fit(f, x + 1, y, text, &th->base, width);
		y++;
	}
}

static void draw_columns(const WfApp *app, WfFrame *f)
{
	const WfThemeStyles *th = wf_theme_styles();
	int x[3], w[3], i;
	bool is_doc = NULL == wf_set_page_of(app);

	wf_set_layout(app, f->cols, x, w);
	for (i = 0; i < (int)WF_ARRAY_LEN(x); i++) {
		const WfMsg heads[3] = {MSG_SET_COL_SECTIONS,
		        MSG_SET_COL_ENTRIES, MSG_SET_COL_DETAILS};
		bool on = i == app->settings.focus;

		if (!w[i]) {
			continue;
		}
		wf_app_put_fit(f, x[i] + 1, 1, wf_msg(heads[i]),
		        on ? &th->accent : &th->dim, w[i] - 1);
		if (i < 2 && x[i] + w[i] < f->cols) {
			wf_app_vrule(f, x[i] + w[i], 1, f->rows - 5);
		}
	}
	if (w[0]) {
		draw_sections(app, f, x[0], w[0]);
	}
	if (w[1]) {
		draw_entries(app, f, x[1], w[1]);
	}
	if (w[2]) {
		if (is_doc) {
			draw_doc(app, f, x[2], w[2]);
		} else if (WF_SS_PICK == app->settings.sub) {
			draw_pick(app, f, x[2], w[2]);
		} else {
			draw_rows(app, f, x[2], w[2]);
		}
	}
}

static WfMsg hint_for(const WfApp *app)
{
	const WfSetPage *pg = wf_set_page_of(app);

	switch (app->settings.sub) {
	case WF_SS_TEXT:      return MSG_SET_HINT_TEXT;
	case WF_SS_PICK:      return MSG_SET_HINT_PICK;
	case WF_SS_CAPTURE:   return MSG_SET_HINT_CAPTURE;
	case WF_SS_WARN:      return MSG_SETTINGS_HINT_WARN;
	case WF_SS_DELETE:    return MSG_SET_HINT_DELETE;
	case WF_SS_RESET_ALL: return MSG_SET_HINT_RESET_ALL;
	case WF_SS_DISCARD:   return MSG_SETTINGS_HINT_DISCARD;
	case WF_SS_BROWSE:    break;
	}
	if (WF_FOCUS_DETAILS != app->settings.focus) {
		return MSG_SET_HINT_COLUMNS;
	}
	return pg ? pg->hint(app) : MSG_SET_HINT_DOC;
}

void wf_set_draw(const WfApp *app, WfFrame *f)
{
	const WfThemeStyles *th = wf_theme_styles();
	const WfSetPage *pg = wf_set_page_of(app);

	wf_frame_fill(f, 0, 0, f->cols, f->rows, ' ', &th->base);
	wf_frame_fill(f, 0, 0, f->cols, 1, ' ', &th->bar_bold);
	wf_app_put_fit(f, 1, 0, wf_msg(MSG_SETTINGS_TITLE), &th->bar_bold,
	        f->cols - 2);
	if (wf_set_is_dirty(app) && f->cols > 30) {
		wf_app_put_fit(f, f->cols - 17, 0, wf_msg(MSG_SETTINGS_UNSAVED),
		        &th->bar, 16);
	}
	if (f->rows < 8) {
		return;
	}
	draw_columns(app, f);
	if (pg && WF_FOCUS_DETAILS == app->settings.focus) {
		const char *help = pg->help(app);

		if (help && help[0]) {
			wf_app_put_fit(
			        f, 1, f->rows - 4, help, &th->dim, f->cols - 2);
		}
	}
	if (WF_SS_TEXT == app->settings.sub) {
		/* the field scrolls sideways so the cursor stays in view */
		const char *in = app->settings.input;
		size_t cur = app->settings.input_cur;
		int room =
		        f->cols - 2 - 2 - 1; /* "> ", the margins, the cursor */
		size_t from = 0;
		int cx;

		if (room < 1) {
			room = 1;
		}
		while (from < cur &&
		        wf_utf8_cells(in + from, cur - from) > room) {
			from = wf_set_char_after(in, cur, from);
		}
		wf_app_put_str(f, 1, f->rows - 3, "> ", &th->accent, 2);
		wf_app_put_fit(
		        f, 3, f->rows - 3, in + from, &th->accent, f->cols - 4);
		cx = 3 + wf_utf8_cells(in + from, cur - from);
		wf_frame_set_cursor(f, cx, f->rows - 3);
	} else {
		draw_note(app, f, f->rows - 3);
	}
	wf_frame_fill(f, 0, f->rows - 1, f->cols, 1, ' ', &th->bar);
	{
		char line[256], letter[2] = {0, 0};

		if ((WF_SS_DISCARD == app->settings.sub ||
		            WF_SS_DELETE == app->settings.sub ||
		            WF_SS_RESET_ALL == app->settings.sub) &&
		        app->settings.choice.n) {
			/* the answer Enter gives, so Up/Down is not a blind move */
			letter[0] = (char)(wf_choice_selected(
			                           &app->settings.choice) -
			        32);
			(void)wf_msgf(line, sizeof line, MSG_CHOICE_HINT,
			        &WF_ARG_S(letter), 1);
			snprintf(line + strlen(line),
			        sizeof line - strlen(line), "  %s",
			        wf_msg(hint_for(app)));
		} else {
			snprintf(
			        line, sizeof line, "%s", wf_msg(hint_for(app)));
		}
		wf_app_put_fit(f, 1, f->rows - 1, line, &th->bar, f->cols - 2);
	}
}

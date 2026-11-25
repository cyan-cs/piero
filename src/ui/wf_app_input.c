#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#include "../archive/wf_vpath.h"
#include "../fs/wf_loc.h"
#include "../fs/wf_ntfsfacts.h"
#include "../fs/wf_sftpplace_win.h"
#include "../path/wf_name.h"
#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"
#include "wf_layout.h"
#include "../base/wf_macro.h"

static void path_draw(const WfApp *app, WfFrame *f);
static WfAction path_event(WfApp *app, const WfEvent *ev);
static WfAction name_event(WfApp *app, const WfEvent *ev);
static WfAction create_event(WfApp *app, const WfEvent *ev);
static void create_draw(const WfApp *app, WfFrame *f);
static const WfMode mode_file = {"NAME", path_draw, name_event};
static const WfMode mode_folder = {"NAME", path_draw, name_event};
static const WfMode mode_rename = {"NAME", path_draw, name_event};
static const WfMode mode_create = {"CREATE", create_draw, create_event};

static bool input_scalar_valid(uint32_t cp)
{
	return cp >= 32 && 127 != cp && cp <= 0x10FFFF &&
	        !(cp >= 0xD800 && cp <= 0xDFFF);
}

static void path_delete_at(WfApp *app, size_t pos)
{
	size_t end = wf_utf16_next_boundary(
	        app->path_input, app->path_input_len, pos);

	memmove(app->path_input + pos, app->path_input + end,
	        (app->path_input_len - end + 1) * sizeof *app->path_input);
	app->path_input_len -= end - pos;
}

static size_t utf8_clip_left(const char *s, size_t len, int budget)
{
	size_t start = len;
	int cells = 0;

	while (start > 0) {
		size_t prev = start - 1;
		uint32_t cp;

		while (prev > 0 && 0x80 == ((unsigned char)s[prev] & 0xC0)) {
			prev--;
		}
		wf_utf8_decode(s + prev, start - prev, &cp);
		if (cells + wf_cp_width(cp) > budget) {
			break;
		}
		cells += wf_cp_width(cp);
		start = prev;
	}
	return start;
}

static size_t utf8_clip_right(const char *s, size_t len, int budget)
{
	size_t end = 0;
	int cells = 0;

	while (end < len) {
		uint32_t cp;
		size_t n = wf_utf8_decode(s + end, len - end, &cp);
		int width = wf_cp_width(cp);

		if (cells + width > budget) {
			break;
		}
		cells += width;
		end += n;
	}
	return end;
}

static WfAction help_event(WfApp *app, const WfEvent *ev)
{
	if (WF_EV_RESIZE == ev->type || WF_EV_RELEASE == ev->type) {
		return WF_ACT_NONE;
	}
	app->mode = &WF_MODE_NORMAL;
	return WF_ACT_REDRAW;
}

static size_t info_layout(const WfInfo *info, int cols, int rows, WfFrame *f)
{
	const WfThemeStyles *th = wf_theme_styles();
	/* Unlike the old all-or-nothing alert, every wrapped row can be reached
	 * in a small terminal. This read-only view is only for the selected file or folder. */
	size_t visual = 0;
	int width = cols > 2 ? cols - 2 : 1;
	for (int i = 0; i < info->n; i++) {
		const char *s = info->text[i];
		size_t remaining = strlen(s);
		do {
			size_t n = utf8_clip_right(s, remaining, width);
			if (!n && remaining) {
				uint32_t cp;
				n = wf_utf8_decode(s, remaining, &cp);
			}
			if (f && rows > 2 && visual >= info->scroll &&
			        visual - info->scroll < (size_t)(rows - 2)) {
				wf_frame_put(f, 1, (int)(visual - info->scroll),
				        s, n, &th->base, width);
			}
			visual++;
			s += n;
			remaining -= n;
		} while (remaining);
	}
	return visual;
}

/* How many screen rows one line of the details takes at this width. */
static size_t info_rows_of(const char *s, int cols)
{
	int width = cols > 2 ? cols - 2 : 1;
	size_t remaining = strlen(s), rows = 0;

	do {
		size_t n = utf8_clip_right(s, remaining, width);

		if (!n && remaining) {
			uint32_t cp;

			n = wf_utf8_decode(s, remaining, &cp);
		}
		rows++;
		s += n;
		remaining -= n;
	} while (remaining);
	return rows;
}

/* The line of text a screen row belongs to, and the first row of a line, at a
 * given width: a resize re-wraps the text, and what was being read stays. */
static int info_line_at(const WfInfo *info, int cols, size_t row)
{
	size_t acc = 0;

	for (int i = 0; i < info->n; i++) {
		size_t n = info_rows_of(info->text[i], cols);

		if (row < acc + n) {
			return i;
		}
		acc += n;
	}
	return info->n;
}

static size_t info_row_of_line(const WfInfo *info, int cols, int line)
{
	size_t acc = 0;

	for (int i = 0; i < line && i < info->n; i++) {
		acc += info_rows_of(info->text[i], cols);
	}
	return acc;
}

static void info_draw(const WfApp *app, WfFrame *f)
{
	const WfThemeStyles *th = wf_theme_styles();
	wf_frame_fill(f, 0, 0, f->cols, f->rows, ' ', &th->base);
	if (f->cols < 3 || f->rows < 3) {
		return;
	}
	(void)info_layout(&app->info, f->cols, f->rows, f);
	wf_app_put_fit(f, 1, f->rows - 1, wf_msg(MSG_INFO_SCROLL_HINT),
	        &th->dim, f->cols - 2);
}

static WfAction info_event(WfApp *app, const WfEvent *ev)
{
	if (ev->type == WF_EV_RESIZE) {
		int line = info_line_at(
		        &app->info, app->info_cols, app->info.scroll);
		size_t page = app->rows > 2 ? (size_t)app->rows - 2 : 1;
		size_t total =
		        info_layout(&app->info, app->cols, app->rows, NULL);
		size_t limit = total > page ? total - page : 0;

		app->info.scroll =
		        info_row_of_line(&app->info, app->cols, line);
		if (app->info.scroll > limit) {
			app->info.scroll = limit;
		}
		app->info_cols = app->cols;
		return WF_ACT_REDRAW;
	}
	if (ev->type == WF_EV_KEY) {
		size_t page = app->rows > 2 ? (size_t)app->rows - 2 : 1;
		size_t step = ev->key == WF_KEY_PGUP || ev->key == WF_KEY_PGDN
		        ? page
		        : 1;
		if (ev->key == WF_KEY_UP || ev->key == WF_KEY_PGUP) {
			app->info.scroll = app->info.scroll > step
			        ? app->info.scroll - step
			        : 0;
			return WF_ACT_REDRAW;
		}
		if (ev->key == WF_KEY_HOME) {
			app->info.scroll = 0;
			return WF_ACT_REDRAW;
		}
		if (ev->key == WF_KEY_DOWN || ev->key == WF_KEY_PGDN ||
		        ev->key == WF_KEY_END) {
			size_t total = info_layout(
			        &app->info, app->cols, app->rows, NULL);
			size_t limit = total > page ? total - page : 0;
			app->info.scroll = ev->key == WF_KEY_END ||
			                app->info.scroll + step > limit
			        ? limit
			        : app->info.scroll + step;
			return WF_ACT_REDRAW;
		}
	}
	return help_event(app, ev);
}

const WfMode WF_MODE_INFO = {"INFO", info_draw, info_event};

void wf_app_info_begin(WfApp *app)
{
	const WfEntry *entry = wf_view_current(&app->cur);
	char counted[WF_INFO_LINE_BYTES];
	WfNtfsFacts facts;
	char hash[65];

	memset(&facts, 0, sizeof facts);
	if (entry && app->cwd) {
		wchar_t *full = NULL;

		/* only a real local path is opened: an archive, an SFTP place and
		 * a WSL distribution are said not to be read */
		if (wf_vpath_is(app->cwd) || wf_sftp_place_is(app->cwd) ||
		        wf_pcroot_is(app->cwd) || wf_wsl_unc_is(app->cwd)) {
			facts.state = WF_NF_REMOTE;
		} else if (WF_OK ==
		        wf_path_join(app->cwd, entry->name, &full)) {
			wf_ntfsfacts_read(full, &facts);
			free(full);
		}
	}
	wf_info_build_full(&app->info, entry, app->cur.dir,
	        wf_sel_folder(&app->sel, app->cwd), app->local_time,
	        wf_app_dirsize_describe(app, entry, counted, sizeof counted)
	                ? counted
	                : NULL,
	        &facts,
	        wf_app_hash_line(app, entry, hash, sizeof hash) ? hash : NULL);
	app->info_cols = app->cols;
	app->mode = &WF_MODE_INFO;
}

void wf_app_safety_info(const WfApp *app, WfInfo *info)
{
	WfMsg detail[16];
	size_t n = 0;
	bool review = app->recovery.unknown || app->recovery.unresolved;
	memset(info, 0, sizeof *info);
	detail[n++] = MSG_RECOVERY_TITLE;
	detail[n++] = review ? MSG_SAFETY_REVIEW : MSG_SAFETY_NORMAL;
	if (!review) {
		detail[n++] = MSG_SAFETY_NORMAL_DETAIL;
	} else {
		detail[n++] = app->recovery.unknown
		        ? MSG_RECOVERY_CHECK_DETAIL
		        : MSG_RECOVERY_OPERATION_DETAIL;
		const unsigned flags[] = {WF_RECOVERY_NOTICE_STORE,
		        WF_RECOVERY_NOTICE_PENDING, WF_RECOVERY_NOTICE_LEGACY,
		        WF_RECOVERY_NOTICE_COPY_INVALID,
		        WF_RECOVERY_NOTICE_LIMIT,
		        WF_RECOVERY_NOTICE_OLD_FOLDER};
		const WfMsg reasons[] = {MSG_RECOVERY_STORE_DETAIL,
		        MSG_RECOVERY_OPERATION_DETAIL,
		        MSG_RECOVERY_LEGACY_DETAIL, MSG_RECOVERY_INVALID_DETAIL,
		        MSG_RECOVERY_LIMIT_DETAIL,
		        MSG_RECOVERY_OLD_FOLDER_DETAIL};
		for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
			if (app->recovery.causes & flags[i]) {
				detail[n++] = reasons[i];
			}
		}
		if (app->recovery.causes & WF_RECOVERY_NOTICE_LEGACY) {
			detail[n++] = MSG_RECOVERY_LEGACY_KIND;
			detail[n++] = MSG_RECOVERY_LEGACY_STATE;
		}
		detail[n++] = MSG_RECOVERY_KEEP_DETAIL;
		detail[n++] = MSG_RECOVERY_PATH_DETAIL;
		detail[n++] = MSG_RECOVERY_ACTION_DETAIL;
		detail[n++] = MSG_RECOVERY_NO_REPAIR;
	}
	for (size_t i = 0; i < n; i++) {
		snprintf(info->text[info->n++], WF_INFO_LINE_BYTES, "%s",
		        wf_msg(detail[i]));
	}
	if (app->recovery.unresolved) {
		(void)wf_msgf(info->text[info->n++], WF_INFO_LINE_BYTES,
		        MSG_STATUS_RECOVERY_UNRESOLVED,
		        &WF_ARG_N(app->recovery.unresolved), 1);
	}
	if (app->recovery.error) {
		(void)wf_msgf(info->text[info->n++], WF_INFO_LINE_BYTES,
		        MSG_RECOVERY_ERROR_DETAIL,
		        &WF_ARG_N(app->recovery.error), 1);
	}
}

void wf_app_set_local_time(WfApp *app, WfLocalTime local)
{
	app->local_time = local;
}

/*
 * The name is edited where it will be: a rename on the entry's own row, a new
 * file or folder on a temporary row at the end of the list. Nothing else on
 * screen moves. False when the list is not on screen (a very small terminal):
 * the caller then uses the line above the status bar.
 */
static bool inplace_draw(const WfApp *app, WfFrame *f)
{
	WfLayout lay;
	WfBuf head, tail;
	WfStyle field = wf_app_style_bar, sel = wf_app_style_bar;
	size_t top, used;
	int x, w, y, budget, cells_head, tail_budget;
	size_t head_from;

	wf_layout_compute(f->cols, f->rows, &app->ratios, &lay);
	if (lay.minimal || lay.body_h < 1 || !app->cur.dir ||
	        lay.pane[WF_PANE_CURRENT].w < 10) {
		return false;
	}
	x = lay.pane[WF_PANE_CURRENT].x;
	w = lay.pane[WF_PANE_CURRENT].w;
	top = wf_view_top(&app->cur, lay.body_h);
	used = app->cur.nvis > top ? app->cur.nvis - top : 0;
	if (app->name_form.rename) {
		if (app->cur.cursor < top ||
		        app->cur.cursor - top >= (size_t)lay.body_h) {
			return false;
		}
		y = lay.body_y + (int)(app->cur.cursor - top);
	} else {
		y = lay.body_y +
		        (used < (size_t)lay.body_h ? (int)used
		                                   : lay.body_h - 1);
		if (used >= (size_t)lay.body_h && lay.body_h > 1) {
			/* a full screen: the rows move up one so the new row
			 * takes the bottom and no entry is covered */
			for (int r = lay.body_y; r < y; r++) {
				memcpy(&f->cells[(size_t)r * (size_t)f->cols +
				               (size_t)x],
				        &f->cells[(size_t)(r + 1) *
				                        (size_t)f->cols +
				                (size_t)x],
				        (size_t)w * sizeof f->cells[0]);
			}
		}
	}
	sel.attributes |= WF_ATTR_REVERSE;
	wf_frame_fill(f, x, y, w, 1, ' ', &field);
	wf_app_put_str(f, x, y, app->name_form.rename ? ">" : "+", &field, 1);
	budget = w - 2;
	wf_buf_init(&head);
	wf_buf_init(&tail);
	wf_sanitize_utf16(app->path_input, app->path_input_cursor, &head);
	wf_sanitize_utf16(app->path_input + app->path_input_cursor,
	        app->path_input_len - app->path_input_cursor, &tail);
	head_from = utf8_clip_left(
	        wf_buf_cstr(&head), head.len, budget > 1 ? budget - 1 : 0);
	cells_head = wf_utf8_cells(
	        wf_buf_cstr(&head) + head_from, head.len - head_from);
	tail_budget = budget - 1 - cells_head; /* the mark takes a cell */
	wf_frame_put(f, x + 2, y, wf_buf_cstr(&head) + head_from,
	        head.len - head_from, app->path_input_selected ? &sel : &field,
	        cells_head);
	if (budget > 0) {
		wf_frame_put(f, x + 2 + cells_head, y, "_", 1, &field, 1);
	}
	if (tail_budget > 0) {
		size_t end = utf8_clip_right(
		        wf_buf_cstr(&tail), tail.len, tail_budget);

		wf_frame_put(f, x + 3 + cells_head, y, wf_buf_cstr(&tail), end,
		        &field, tail_budget);
	}
	if (!app->name_form.rename && 0 == app->path_input_len) {
		WfStyle dim = field;

		dim.fg_rgb = wf_theme_active()->muted_foreground;
		wf_app_put_fit(f, x + 4, y,
		        wf_msg(app->mode == &mode_folder
		                        ? MSG_OPS_NEW_FOLDER_HOLDER
		                        : MSG_OPS_NEW_FILE_HOLDER),
		        &dim, budget > 2 ? budget - 2 : 0);
	}
	wf_frame_set_cursor(f, x + 2 + cells_head, y);
	wf_buf_free(&head);
	wf_buf_free(&tail);
	return true;
}

static void path_draw(const WfApp *app, WfFrame *f)
{
	if (f->rows < 2) {
		return;
	}
	if (inplace_draw(app, f)) {
		return;
	}

	WfBuf prefix, suffix, display;
	size_t prefix_start, suffix_end;
	int budget = f->cols > 3 ? f->cols - 3 : 0;
	int prefix_cells, suffix_budget;

	wf_buf_init(&prefix);
	wf_buf_init(&suffix);
	wf_buf_init(&display);
	wf_sanitize_utf16(app->path_input, app->path_input_cursor, &prefix);
	wf_sanitize_utf16(app->path_input + app->path_input_cursor,
	        app->path_input_len - app->path_input_cursor, &suffix);
	prefix_start = utf8_clip_left(
	        wf_buf_cstr(&prefix), prefix.len, budget > 0 ? budget - 1 : 0);
	prefix_cells = wf_utf8_cells(
	        wf_buf_cstr(&prefix) + prefix_start, prefix.len - prefix_start);
	suffix_budget = budget > 0 ? budget - 1 - prefix_cells : 0;
	suffix_end = utf8_clip_right(
	        wf_buf_cstr(&suffix), suffix.len, suffix_budget);
	if (prefix_start < prefix.len) {
		wf_buf_append(&display, prefix.data + prefix_start,
		        prefix.len - prefix_start);
	}
	if (budget > 0) {
		wf_buf_putc(&display, '_');
	}
	if (suffix_end) {
		wf_buf_append(&display, suffix.data, suffix_end);
	}
	{
		/* name forms sit on the line above the status bar */
		int y = f->rows - 2;
		WfStyle text = wf_app_style_bar;

		if (app->path_input_selected) {
			text.attributes |= WF_ATTR_REVERSE;
		}
		wf_frame_fill(f, 0, y, f->cols, 1, ' ', &wf_app_style_bar);
		wf_app_put_str(f, 0, y, "n> ", &wf_app_style_bar, f->cols);
		wf_frame_put(f, 3, y, wf_buf_cstr(&display), display.len, &text,
		        budget);
		if (budget > 0) {
			wf_frame_set_cursor(f, 3 + prefix_cells, y);
		}
	}
	wf_buf_free(&prefix);
	wf_buf_free(&suffix);
	wf_buf_free(&display);
}

static WfStatus path_append(WfApp *app, uint32_t cp)
{
	size_t need = cp >= 0x10000 ? 2 : 1, count;
	wchar_t *next;
	if (!input_scalar_valid(cp)) {
		return WF_E_INVAL;
	}
	if (app->path_input_len > SIZE_MAX / sizeof *next - need - 1) {
		return WF_E_RANGE;
	}
	count = app->path_input_len + need + 1;
	if (count > app->path_input_cap) {
		size_t capacity = app->path_input_cap <= SIZE_MAX / 2
		        ? app->path_input_cap * 2
		        : count;
		if (capacity < count) {
			capacity = count;
		}
		if (capacity > SIZE_MAX / sizeof *next) {
			capacity = count;
		}
		next = realloc(app->path_input, capacity * sizeof *next);
		if (!next) {
			return WF_E_NOMEM;
		}
		app->path_input = next;
		app->path_input_cap = capacity;
	}
	memmove(app->path_input + app->path_input_cursor + need,
	        app->path_input + app->path_input_cursor,
	        (app->path_input_len - app->path_input_cursor + 1) *
	                sizeof *app->path_input);
	if (2 == need) {
		cp -= 0x10000;
		app->path_input[app->path_input_cursor++] =
		        (wchar_t)(0xD800 + (cp >> 10));
		app->path_input[app->path_input_cursor++] =
		        (wchar_t)(0xDC00 + (cp & 0x3FF));
	} else {
		app->path_input[app->path_input_cursor++] = (wchar_t)cp;
	}
	app->path_input_len += need;
	app->path_input[app->path_input_len] = 0;
	return WF_OK;
}

static void path_prompt_close(WfApp *app)
{
	free(app->path_input);
	app->path_input = NULL;
	app->path_input_len = app->path_input_cap = app->path_input_cursor = 0;
	app->path_input_selected = false;
	memset(&app->name_form, 0, sizeof app->name_form);
	app->msg[0] = 0;
	app->mode = &WF_MODE_NORMAL;
}

static void path_select_none(WfApp *app)
{
	app->path_input_selected = false;
}

/* Typing, Backspace or Delete over a selection replaces the whole text. */
static void path_take_selection(WfApp *app)
{
	if (!app->path_input_selected) {
		return;
	}
	size_t end = app->path_input_sel_end < app->path_input_len
	        ? app->path_input_sel_end
	        : app->path_input_len;

	app->path_input_selected = false;
	memmove(app->path_input, app->path_input + end,
	        (app->path_input_len - end + 1) * sizeof *app->path_input);
	app->path_input_len -= end;
	app->path_input_cursor = 0;
}

static WfAction path_event(WfApp *app, const WfEvent *ev)
{
	WfStatus st;

	if (WF_EV_RELEASE == ev->type || WF_EV_RESIZE == ev->type) {
		return WF_ACT_NONE;
	}
	if (WF_EV_CHAR == ev->type) {
		path_take_selection(app);
		st = path_append(app, ev->ch);
		if (WF_OK != st) {
			wf_app_set_msg(app, wf_msg(MSG_OPS_INPUT_ERROR_PATH));
		}
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY != ev->type) {
		return WF_ACT_NONE;
	}
	if (WF_KEY_ESC == ev->key) {
		path_prompt_close(app);
		return WF_ACT_REDRAW;
	}
	if (WF_KEY_LEFT == ev->key) {
		if (app->path_input_selected) {
			app->path_input_cursor = 0;
		} else {
			app->path_input_cursor = wf_utf16_prev_boundary(
			        app->path_input, app->path_input_cursor);
		}
		path_select_none(app);
		return WF_ACT_REDRAW;
	}
	if (WF_KEY_RIGHT == ev->key) {
		if (app->path_input_selected) {
			app->path_input_cursor = app->path_input_sel_end;
		} else {
			app->path_input_cursor = wf_utf16_next_boundary(
			        app->path_input, app->path_input_len,
			        app->path_input_cursor);
		}
		path_select_none(app);
		return WF_ACT_REDRAW;
	}
	if (WF_KEY_HOME == ev->key || WF_KEY_END == ev->key) {
		app->path_input_cursor =
		        WF_KEY_HOME == ev->key ? 0 : app->path_input_len;
		path_select_none(app);
		return WF_ACT_REDRAW;
	}
	if (WF_KEY_BACKSPACE == ev->key || WF_KEY_DELETE == ev->key) {
		if (app->path_input_selected) {
			path_take_selection(app);
		} else if (WF_KEY_BACKSPACE == ev->key) {
			if (app->path_input_cursor) {
				app->path_input_cursor =
				        wf_utf16_prev_boundary(app->path_input,
				                app->path_input_cursor);
				path_delete_at(app, app->path_input_cursor);
			}
		} else if (app->path_input_cursor < app->path_input_len) {
			path_delete_at(app, app->path_input_cursor);
		}
		return WF_ACT_REDRAW;
	}
	return WF_ACT_NONE;
}

/* Operation forms deliberately have no filesystem port. A submitted form
 * stops at the release gate, even if a transfer executor is installed. */
static void name_begin(WfApp *app, const WfMode *mode, const wchar_t *initial)
{
	size_t n = wcslen(initial);
	wchar_t *name = wf_path_dup_n(initial, n);
	if (!name) {
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	free(app->path_input);
	app->path_input = name;
	app->path_input_len = app->path_input_cursor = n;
	app->path_input_cap = n + 1;
	app->path_input_selected = false;
	app->path_input_sel_end = 0;
	memset(&app->name_form, 0, sizeof app->name_form);
	app->name_form.nav_gen = app->nav_gen;
	app->name_form.cwd_version = app->cwd_version;
	app->name_form.rename = mode == &mode_rename;
	if (app->cur.dir) {
		app->name_form.parent = app->cur.dir->identity;
	}
	if (app->name_form.rename) {
		const WfEntry *entry = wf_view_current(&app->cur);
		if (entry && n < WF_ARRAY_LEN(app->name_form.source_name)) {
			app->name_form.source = entry->identity;
			app->name_form.source_flags = entry->flags;
			memcpy(app->name_form.source_name, initial,
			        (n + 1) * sizeof *initial);
		}
	}
	/* a WSL distribution has no file identity: the listing generation and
	 * the entry's name and kind bind the request instead */
	app->name_form.bound = wf_wsl_unc_is(app->cwd) ||
	        ((app->name_form.parent.kind == WF_ID_STRONG) &&
	                (!app->name_form.rename ||
	                        ((app->name_form.source.kind == WF_ID_STRONG) &&
	                                app->name_form.source.volume ==
	                                        app->name_form.parent
	                                                .volume))) ||
	        /* a new name on a FAT volume: nothing that exists is touched */
	        (!app->name_form.rename &&
	                wf_fileid_is_weak(&app->name_form.parent));
	if (mode == &mode_rename && n) {
		/* the name is selected, the extension is not: typing replaces
		 * the name and keeps ".txt" */
		size_t stem = n;

		if (!(app->name_form.source_flags & WF_ENT_DIR)) {
			for (size_t i = n; i > 1; i--) {
				if (L'.' == initial[i - 1]) {
					stem = i - 1;
					break;
				}
			}
		}
		app->path_input_selected = true;
		app->path_input_sel_end = stem;
		app->path_input_cursor = stem;
	}
	app->mode = mode;
	if (app->edit.submit) {
		wf_app_set_msg(app,
		        wf_msg(mode == &mode_rename ? MSG_OPS_FORM_RENAME
		                        : mode == &mode_file
		                        ? MSG_OPS_FORM_NEW_FILE
		                        : MSG_OPS_FORM_NEW_FOLDER));
	} else {
		wf_app_set_msg(app,
		        wf_msg(mode == &mode_rename ? MSG_OPS_FORM_RENAME_GATED
		                        : mode == &mode_file
		                        ? MSG_OPS_FORM_NEW_FILE_GATED
		                        : MSG_OPS_FORM_NEW_FOLDER_GATED));
	}
}

static const char *name_submission_blocked(const WfApp *app)
{
	const WfEntry *entry;
	bool wsl = wf_wsl_unc_is(app->cwd);

	if (!app->name_form.bound) {
		return wf_msg(MSG_OPS_BLOCKED_IDENTITY);
	}
	if (app->want_path || app->name_form.nav_gen != app->nav_gen ||
	        app->name_form.cwd_version != app->cwd_version ||
	        !app->cur.dir ||
	        (!wsl &&
	                !wf_fileid_equal_or_weak(&app->name_form.parent,
	                        &app->cur.dir->identity))) {
		return wf_msg(MSG_OPS_BLOCKED_LISTING_CHANGED);
	}
	if (app->name_form.rename) {
		entry = wf_view_current(&app->cur);
		if (!entry ||
		        (!wsl &&
		                !wf_fileid_equal(&app->name_form.source,
		                        &entry->identity)) ||
		        app->name_form.source_flags != entry->flags ||
		        wcscmp(app->name_form.source_name, entry->name)) {
			return wf_msg(MSG_OPS_BLOCKED_LISTING_CHANGED);
		}
	}
	return NULL;
}

/* Win32 codes the receipt messages distinguish (kept numeric: this file does
 * not include <windows.h>). */
enum
{
	ERR_ACCESS_DENIED = 5,
	ERR_FILE_EXISTS = 80,
	ERR_ALREADY_EXISTS = 183
};

void wf_app_set_edit(WfApp *app, WfEditPort port)
{
	app->edit = port;
}

/* Remembers what a rename in flight would need to be taken back; it is kept
 * only if the rename succeeds (wf_app_on_edit). */
static void note_rename_for_undo(WfApp *app)
{
	WfUndoEntry *u = &app->undo_pending;

	wf_app_undo_free(u);
	app->undo_pending_set = false;
	u->kind = WF_UNDO_RENAME;
	u->folder = wf_path_dup(app->cwd);
	u->folder_id = app->name_form.parent;
	u->item = calloc(1, sizeof *u->item);
	if (u->item) {
		u->item[0].name =
		        wf_path_dup_n(app->path_input, app->path_input_len);
		u->item[0].old_name = wf_path_dup(app->name_form.source_name);
	}
	if (!u->folder || !u->item || !u->item[0].name ||
	        !u->item[0].old_name) {
		u->n = 1; /* free whatever of the single slot was made */
		wf_app_undo_free(u);
		return;
	}
	u->item[0].id = app->name_form.source;
	u->item[0].is_dir = 0 != (app->name_form.source_flags & WF_ENT_DIR);
	u->n = 1;
	app->undo_pending_set = true;
}

/* Sends the captured form to the execution port. The form stays open when
 * the name is rejected so it can be corrected. */
static WfAction name_submit(WfApp *app, bool rename, bool directory)
{
	WfEditRequest req;
	WfNameResult verdict;
	WfStatus st;
	uint32_t gen;

	if (app->edit_busy) {
		wf_app_set_msg(app, wf_msg(MSG_OPS_BUSY));
		return WF_ACT_REDRAW;
	}
	/* 255 is the NTFS component limit; the backend re-checks against the
	 * real volume. */
	verdict = rename
	        ? wf_name_validate(app->path_input, app->path_input_len, 255)
	        : wf_name_validate_path(
	                  app->path_input, app->path_input_len, 255);
	if (WF_NAME_OK != verdict) {
		char text[96];

		(void)wf_msgf(text, sizeof text, MSG_OPS_NAME_ERROR,
		        &WF_ARG_S(wf_app_name_reason_text((int)verdict)), 1);
		wf_app_set_msg(app, text);
		return WF_ACT_REDRAW;
	}
	if (rename && !wcscmp(app->path_input, app->name_form.source_name)) {
		path_prompt_close(app);
		wf_app_set_msg(app, wf_msg(MSG_OPS_RENAME_UNCHANGED));
		return WF_ACT_REDRAW;
	}
	memset(&req, 0, sizeof req);
	req.kind = rename   ? WF_EDIT_RENAME
	        : directory ? WF_EDIT_CREATE_DIR
	                    : WF_EDIT_CREATE_FILE;
	req.parent = app->cwd;
	req.parent_id = app->name_form.parent;
	req.old_name = rename ? app->name_form.source_name : NULL;
	req.source_id = app->name_form.source;
	req.new_name = app->path_input;
	gen = ++app->edit_gen;
	if (rename) {
		note_rename_for_undo(app); /* before the form is closed */
	}
	wf_app_wsl_note_form(app, rename, directory);
	st = app->edit.submit(app->edit.ctx, gen, &req);
	path_prompt_close(app);
	if (WF_OK != st) {
		wf_app_undo_free(&app->undo_pending);
		app->undo_pending_set = false;
		wf_app_set_msg(app, wf_msg(MSG_OPS_CANNOT_START));
		return WF_ACT_REDRAW;
	}
	app->edit_busy = true;
	app->edit_is_undo = false;
	/* for the row of the history, written when the answer comes */
	memset(&app->form_note, 0, sizeof app->form_note);
	app->form_note.set = true;
	app->form_note.rename = rename;
	app->form_note.dir = directory;
	if (rename) {
		WfBuf old;

		wf_buf_init(&old);
		(void)wf_sanitize_utf16(app->name_form.source_name,
		        wcslen(app->name_form.source_name), &old);
		wf_utf8_elide(app->form_note.old_name,
		        sizeof app->form_note.old_name, wf_buf_cstr(&old), 28);
		wf_buf_free(&old);
	}
	wf_app_set_msg(
	        app, wf_msg(rename ? MSG_OPS_RENAMING : MSG_OPS_CREATING));
	return WF_ACT_REDRAW;
}

/* Why a name was refused, in words. The verdict is a WfNameResult. */
const char *wf_app_name_reason_text(int verdict)
{
	switch ((WfNameResult)verdict) {
	case WF_NAME_EMPTY:    return wf_msg(MSG_REASON_NAME_EMPTY);
	case WF_NAME_TOO_LONG: return wf_msg(MSG_REASON_NAME_TOO_LONG);
	case WF_NAME_INVALID:  return wf_msg(MSG_REASON_NAME_INVALID_CHARS);
	case WF_NAME_RESERVED: return wf_msg(MSG_REASON_NAME_RESERVED);
	default:               return wf_msg(MSG_REASON_NAME_INVALID);
	}
}

/* The backend reports the reason as wf_name_reason() text; find the verdict
 * behind it to show it in the current language. */
static const char *name_reason_of(const char *reason)
{
	for (int v = WF_NAME_EMPTY; v <= WF_NAME_RESERVED; v++) {
		if (!strcmp(reason, wf_name_reason((WfNameResult)v))) {
			return wf_app_name_reason_text(v);
		}
	}
	return wf_app_name_reason_text(-1);
}

void wf_app_edit_failure_text(const WfEditOutcome *outcome,
        const char *shown_name,
        char *out,
        size_t cap)
{
	char code[16];

	snprintf(code, sizeof code, "%lu", outcome->error);
	if (outcome->name_reason) {
		(void)wf_msgf(out, cap, MSG_OPS_NAME_ERROR,
		        &WF_ARG_S(name_reason_of(outcome->name_reason)), 1);
	} else if (outcome->renamed) {
		(void)wf_msgf(out, cap, MSG_OPS_CHECK_FAILED,
		        (WfMsgArg[]){WF_ARG_S(code), WF_ARG_S(shown_name)}, 2);
	} else if (ERR_FILE_EXISTS == outcome->error ||
	        ERR_ALREADY_EXISTS == outcome->error) {
		(void)wf_msgf(
		        out, cap, MSG_OPS_EXISTS, &WF_ARG_S(shown_name), 1);
	} else if (WF_LOCK_IN_USE == wf_lock_classify(outcome->error)) {
		(void)wf_msgf(out, cap, MSG_OPS_IN_USE, &WF_ARG_S(code), 1);
	} else if (ERR_ACCESS_DENIED == outcome->error) {
		(void)wf_msgf(out, cap, MSG_OPS_ACCESS_DENIED, NULL, 0);
	} else if (WF_E_INVAL == outcome->st && !outcome->error) {
		(void)wf_msgf(out, cap, MSG_OPS_CHANGED_SINCE_LISTED, NULL, 0);
	} else {
		(void)wf_msgf(out, cap, MSG_OPS_FAILED, &WF_ARG_S(code), 1);
	}
}

/* The row of the history for the form that was answered with success. A WSL
 * distribution has an Undo of its own (one step, in that distribution), so its
 * rows do not claim the ordinary one. */
static void form_history_row(
        WfApp *app, const char *new_shown, uint32_t undo_uid)
{
	char head[128], name[64];
	bool wsl = wf_wsl_unc_is(app->cwd);

	wf_utf8_elide(name, sizeof name, new_shown, 28);
	if (app->form_note.rename) {
		(void)wf_msgf(head, sizeof head, MSG_HIST_RENAME,
		        (WfMsgArg[]){WF_ARG_S(app->form_note.old_name),
		                WF_ARG_S(name)},
		        2);
		wf_app_hist_add(app, head, wsl ? 0 : undo_uid,
		        wsl ? MSG_HIST_WHY_WSL : MSG_HIST_WHY_NOT_KEPT);
	} else {
		(void)wf_msgf(head, sizeof head,
		        app->form_note.dir ? MSG_HIST_NEW_FOLDER
		                           : MSG_HIST_NEW_FILE,
		        &WF_ARG_S(name), 1);
		wf_app_hist_add(app, head, 0,
		        wsl ? MSG_HIST_WHY_WSL : MSG_HIST_WHY_CREATE);
	}
}

void wf_app_on_edit(WfApp *app,
        uint32_t gen,
        const WfEditOutcome *outcome,
        const wchar_t *new_name)
{
	char text[96];
	WfBuf shown;
	bool created = outcome && outcome->renamed;
	uint32_t undo_uid = 0;

	if (!app || !outcome || gen != app->edit_gen || !app->edit_busy) {
		return;
	}
	wf_app_dircache_invalidate(app);
	app->edit_busy = false;
	if (wf_app_wsl_receipt(app, outcome, new_name) ||
	        wf_app_move_receipt(app, outcome, new_name)) {
		return;
	}
	wf_app_wsl_edit_done(app, outcome);
	if (WF_OK != outcome->st) {
		wf_app_lock_rename_failed(
		        app, outcome); /* before the undo is dropped */
	}
	if (app->undo_pending_set) {
		if (WF_OK == outcome->st) {
			undo_uid = wf_app_undo_push(app, &app->undo_pending);
		} else {
			wf_app_undo_free(&app->undo_pending);
		}
		app->undo_pending_set = false;
	}
	if (app->edit_is_undo) {
		app->edit_is_undo = false;
		wf_app_undo_done(app, WF_OK == outcome->st ? 1 : 0);
		snprintf(text, sizeof text, "%s",
		        wf_msg(WF_OK == outcome->st ? MSG_OPS_UNDO_DONE
		                                    : MSG_OPS_UNDO_FAILED));
		app->msg_keep_until = app->now_ms + WF_RECEIPT_KEEP_MS;
		wf_app_reload(app);
		wf_app_set_msg(app, text);
		return;
	}
	wf_buf_init(&shown);
	if (!new_name ||
	        WF_OK !=
	                wf_sanitize_utf16(new_name, wcslen(new_name), &shown)) {
		wf_buf_puts(&shown, "?");
	}
	if (WF_OK == outcome->st) {
		(void)wf_msgf(text, sizeof text, MSG_OPS_DONE,
		        &WF_ARG_S(wf_buf_cstr(&shown)), 1);
		if (app->form_note.set) {
			form_history_row(app, wf_buf_cstr(&shown), undo_uid);
		}
	} else {
		wf_app_edit_failure_text(
		        outcome, wf_buf_cstr(&shown), text, sizeof text);
	}
	wf_buf_free(&shown);
	app->form_note.set = false;
	/* Whatever happened, the listing is reloaded; the new name is selected.
	 * Reloads clear the status line (the directory watcher also reloads
	 * right after the change), so the receipt is held for a few seconds. */
	app->msg_keep_until = app->now_ms + WF_RECEIPT_KEEP_MS;
	if (created && new_name) {
		/* "a/b/c" was made inside this folder as "a": that is what is
		 * listed here and selected. */
		wchar_t first[256];
		const wchar_t *rest;
		size_t n = wf_name_split_first(new_name, &rest);

		wcsncpy_s(first, WF_ARRAY_LEN(first), new_name, n);
		(void)wf_app_navigate(app, app->cwd, first, SIZE_MAX);
	} else {
		wf_app_reload(app);
	}
	wf_app_set_msg(app, text);
}

static WfAction name_event(WfApp *app, const WfEvent *ev)
{
	if (WF_EV_CHAR == ev->type) {
		path_take_selection(app);
		if (WF_OK != path_append(app, ev->ch)) {
			wf_app_set_msg(app, wf_msg(MSG_OPS_INPUT_ERROR_NAME));
		}
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY == ev->type && WF_KEY_ENTER == ev->key) {
		bool rename = app->mode == &mode_rename;
		const char *blocked = name_submission_blocked(app);
		if (!blocked && app->edit.submit) {
			return name_submit(
			        app, rename, app->mode == &mode_folder);
		}
		path_prompt_close(app);
		wf_app_set_msg(app,
		        blocked ? blocked
		                : wf_msg(rename ? MSG_OPS_BLOCKED_RENAME_GATES
		                                : MSG_OPS_BLOCKED_CREATE_GATES));
		return WF_ACT_REDRAW;
	}
	return path_event(app, ev);
}

static void create_draw(const WfApp *app, WfFrame *f)
{
	char row[3][96];
	const char *text[3] = {wf_msg(MSG_OPS_CREATE_FILE),
	        wf_msg(MSG_OPS_CREATE_FOLDER), wf_msg(MSG_OPS_CREATE_CANCEL)};
	const char *lines[] = {
	        wf_msg(MSG_OPS_CREATE_TITLE_BLOCKED), row[0], row[1], row[2]};

	/* The arrow-chosen line is marked; Enter takes it. */
	for (int i = 0; i < 3; i++) {
		snprintf(row[i], sizeof row[i], "%s%s",
		        i == app->create_pick ? "> " : "  ", text[i]);
	}
	if (app->edit.submit) {
		lines[0] = wf_msg(MSG_OPS_CREATE_TITLE);
	}
	if (!wf_ui_alert(f, lines, 4) && f->rows > 1) {
		char bar[256];

		/* The choice comes first so a narrow screen still shows it. */
		snprintf(bar, sizeof bar, "%s  |  %s", row[app->create_pick],
		        wf_msg(MSG_OPS_CREATE_LINE));
		wf_app_put_fit(
		        f, 0, f->rows - 2, bar, &wf_app_style_bar, f->cols);
	}
}

static WfAction create_event(WfApp *app, const WfEvent *ev)
{
	if (WF_EV_KEY == ev->type && WF_KEY_ESC == ev->key) {
		app->mode = &WF_MODE_NORMAL;
		app->msg[0] = 0;
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY == ev->type &&
	        (WF_KEY_UP == ev->key || WF_KEY_LEFT == ev->key)) {
		app->create_pick = (app->create_pick + 2) % 3;
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY == ev->type &&
	        (WF_KEY_DOWN == ev->key || WF_KEY_RIGHT == ev->key)) {
		app->create_pick = (app->create_pick + 1) % 3;
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY == ev->type && WF_KEY_ENTER == ev->key) {
		if (2 == app->create_pick) {
			app->mode = &WF_MODE_NORMAL;
			app->msg[0] = 0;
			return WF_ACT_REDRAW;
		}
		name_begin(
		        app, app->create_pick ? &mode_folder : &mode_file, L"");
		return WF_ACT_REDRAW;
	}
	if (WF_EV_CHAR == ev->type && ('%' == ev->ch || 'd' == ev->ch)) {
		name_begin(app, '%' == ev->ch ? &mode_file : &mode_folder, L"");
		return WF_ACT_REDRAW;
	}
	return WF_ACT_NONE;
}

/* Recycle runs on the copy service. When that is off, say why, as the copy
 * does (the same reason, in the words of a delete). */
static WfMsg recycle_off_reason(const WfApp *app)
{
	switch (app->copy_off_set ? app->copy_off : MSG_COPY_SAFETY_BLOCKED) {
	case MSG_COPY_OFF_STATE_FOLDER: return MSG_RECYCLE_OFF_STATE_FOLDER;
	case MSG_COPY_OFF_RECORD:       return MSG_RECYCLE_OFF_RECORD;
	default:                        return MSG_RECYCLE_OFF_SAFETY;
	}
}

/* False when the operation must not start: an archive, or a listing that is
 * not current. */
static bool operation_allowed(WfApp *app)
{
	if (wf_app_archive_guard(app)) {
		return false;
	}
	if (app->want_path) {
		wf_app_set_msg(app, wf_msg(MSG_OPS_BLOCKED_LOADING));
		return false;
	}
	return true;
}

void wf_app_cmd_create(WfApp *app)
{
	if (!operation_allowed(app)) {
		return;
	}
	app->create_pick = 0;
	app->mode = &mode_create;
	wf_app_set_msg(app,
	        wf_msg(app->edit.submit ? MSG_OPS_CREATE_HINT
	                                : MSG_OPS_CREATE_HINT_GATED));
}

void wf_app_cmd_create_kind(WfApp *app, bool file)
{
	if (operation_allowed(app)) {
		name_begin(app, file ? &mode_file : &mode_folder, L"");
	}
}

void wf_app_cmd_rename(WfApp *app)
{
	const WfEntry *entry;

	if (!operation_allowed(app)) {
		return;
	}
	entry = wf_view_current(&app->cur);
	if (entry) {
		name_begin(app, &mode_rename, entry->name);
	} else {
		wf_app_set_msg(app, wf_msg(MSG_OPS_BLOCKED_NO_ENTRY));
	}
}

/* The rename that failed because the file was in use, put back at the prompt
 * (DR-DRAFT-024): the cursor goes to the item, which must still be the file
 * that failed, and the prompt opens with the name that was asked for. Enter
 * then runs the ordinary rename with its own checks. */
bool wf_app_rename_again(WfApp *app,
        const wchar_t *dir,
        const wchar_t *old_name,
        const wchar_t *new_name,
        const WfFileId *id)
{
	if (!operation_allowed(app)) {
		return false;
	}
	if (!app->cwd || _wcsicmp(app->cwd, dir)) {
		(void)wf_app_navigate(app, dir, old_name, SIZE_MAX);
		wf_app_set_msg(app, wf_msg(MSG_LOCK_RETRY_GO));
		return true;
	}
	for (size_t i = 0; i < app->cur.nvis; i++) {
		const WfEntry *e = &app->cur.dir->e[app->cur.vis[i]];

		if (wcscmp(e->name, old_name)) {
			continue;
		}
		if (!(e->identity.kind == WF_ID_STRONG) ||
		        !wf_fileid_equal(&e->identity, id)) {
			wf_app_set_msg(app, wf_msg(MSG_LOCK_RETRY_REPLACED));
			return false;
		}
		wf_view_move(&app->cur, (long)i - (long)app->cur.cursor,
		        wf_app_body_height(app));
		/* the form is bound to the item as it is named now; only the text in
		 * the prompt is the name that was asked for */
		name_begin(app, &mode_rename, old_name);
		{
			wchar_t *text = wf_path_dup(new_name);

			if (text) {
				free(app->path_input);
				app->path_input = text;
				app->path_input_len = app->path_input_cursor =
				        wcslen(text);
				app->path_input_cap = app->path_input_len + 1;
				app->path_input_selected = false;
			}
		}
		return true;
	}
	wf_app_set_msg(app, wf_msg(MSG_LOCK_RETRY_REPLACED));
	return false;
}

void wf_app_cmd_recycle(WfApp *app, const WfEvent *ev)
{
	if (!operation_allowed(app)) {
		return;
	}
	if (wf_app_wsl_delete(app, ev)) {
		return; /* a distribution has no Recycle Bin: its own delete */
	}
	if (!wf_copyjob_allowed(WF_CJ_RECYCLE, WF_COLLIDE_STOP)) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_RECYCLE_UNAVAILABLE));
		return;
	}
	if (!app->copy.submit) {
		/* no question to ask: confirming could not delete anything */
		wf_app_set_msg(app, wf_msg(recycle_off_reason(app)));
		return;
	}
	wf_app_recycle_begin(app, ev);
}

static void filter_draw(const WfApp *app, WfFrame *f)
{
	if (f->rows < 2) {
		return;
	}

	WfBuf s;

	wf_frame_fill(f, 0, f->rows - 2, f->cols, 1, ' ', &wf_app_style_bar);
	wf_buf_init(&s);
	wf_frame_put(f, 0, f->rows - 2, "/", 1, &wf_app_style_bar, f->cols);
	if (WF_OK ==
	        wf_sanitize_utf16(
	                app->cur.filter, wcslen(app->cur.filter), &s)) {
		/* what was typed last is always on screen, with the cursor */
		int cx = wf_app_put_input_tail(f, 1, f->rows - 2,
		        wf_buf_cstr(&s), s.len, f->cols - 1, &wf_app_style_bar);

		wf_frame_set_cursor(f, cx, f->rows - 2);
		if (cx < f->cols) {
			wf_frame_put(f, cx, f->rows - 2, "_", 1,
			        &wf_app_style_bar, 1);
		}
	}
	wf_buf_free(&s);
}

/* Appends one scalar as UTF-16; false if it does not fit. */
static bool filter_append(WfView *v, uint32_t cp)
{
	size_t n = wcslen(v->filter), need = cp >= 0x10000 ? 2 : 1;
	wchar_t next[WF_FILTER_MAX];

	if (!input_scalar_valid(cp) || n + need >= WF_FILTER_MAX) {
		return false;
	}
	memcpy(next, v->filter, n * sizeof *next);
	if (2 == need) {
		cp -= 0x10000;
		next[n++] = (wchar_t)(0xD800 + (cp >> 10));
		next[n++] = (wchar_t)(0xDC00 + (cp & 0x3FF));
	} else {
		next[n++] = (wchar_t)cp;
	}
	next[n] = L'\0';
	return WF_OK == wf_view_set_filter(v, next);
}

static void filter_backspace(WfView *v)
{
	size_t n = wcslen(v->filter);
	wchar_t next[WF_FILTER_MAX];

	if (0 == n) {
		return;
	}
	n--;
	if (n > 0 && (uint16_t)v->filter[n] >= 0xDC00 &&
	        (uint16_t)v->filter[n] <= 0xDFFF) {
		n--; /* drop the whole surrogate pair */
	}
	memcpy(next, v->filter, n * sizeof *next);
	next[n] = L'\0';
	wf_view_set_filter(v, next);
}

static WfAction filter_event_inner(WfApp *app, const WfEvent *ev)
{
	if (WF_EV_CHAR == ev->type) {
		filter_append(&app->cur, ev->ch);
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY != ev->type) {
		return WF_ACT_NONE;
	}
	switch (ev->key) {
	case WF_KEY_CTRL_UP:
		wf_view_move_edge(&app->cur, false, wf_app_body_height(app));
		return WF_ACT_REDRAW;
	case WF_KEY_CTRL_DOWN:
		wf_view_move_edge(&app->cur, true, wf_app_body_height(app));
		return WF_ACT_REDRAW;
	case WF_KEY_UP:
		wf_view_move(&app->cur, -1, wf_app_body_height(app));
		return WF_ACT_REDRAW;
	case WF_KEY_DOWN:
		wf_view_move(&app->cur, 1, wf_app_body_height(app));
		return WF_ACT_REDRAW;
	case WF_KEY_BACKSPACE:
		filter_backspace(&app->cur);
		return WF_ACT_REDRAW;
	case WF_KEY_TAB: /* widen it to this folder and below */
		wf_app_search_begin(app);
		return WF_ACT_REDRAW;
	case WF_KEY_ENTER: /* keep the filter active */
		app->mode = &WF_MODE_NORMAL;
		return WF_ACT_REDRAW;
	case WF_KEY_ESC: /* drop it */
		wf_view_set_filter(&app->cur, L"");
		app->mode = &WF_MODE_NORMAL;
		return WF_ACT_REDRAW;
	default: return WF_ACT_NONE;
	}
}

static WfAction filter_event(WfApp *app, const WfEvent *ev)
{
	WfAction action = filter_event_inner(app, ev);
	if (WF_ACT_REDRAW == action) {
		wf_app_preview_sync(app, false);
	}
	return action;
}

const WfMode WF_MODE_FILTER = {"FILTER", filter_draw, filter_event};

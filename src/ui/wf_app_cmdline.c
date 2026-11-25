/*
 * The command line (':' in the Vim profile): editing a line, its history, and
 * running the documented commands. (The sort menu and the help are their own
 * files: wf_app_sort.c, wf_app_help.c.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../base/wf_macro.h"
#include "../input/wf_exline.h"
#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"

/* One step over what is seen as one character: a letter with its accents
 * moves and is deleted as a whole. */
static size_t prev_boundary(const char *s, size_t n, size_t at)
{
	return wf_utf8_cluster_prev(s, n, at);
}

static size_t next_boundary(const char *s, size_t n, size_t at)
{
	return wf_utf8_cluster_next(s, n, at);
}

void wf_app_cmdline_begin(WfApp *app)
{
	app->cmdline.text[0] = '\0';
	app->cmdline.n = app->cmdline.cur = 0;
	app->cmdline.hist_at = app->cmdline.hist_n;
	app->mode = &WF_MODE_CMDLINE;
}

static void cl_set(WfApp *app, const char *text)
{
	size_t n = strlen(text);

	if (n >= sizeof app->cmdline.text) {
		n = sizeof app->cmdline.text - 1;
	}
	memcpy(app->cmdline.text, text, n);
	app->cmdline.text[n] = '\0';
	app->cmdline.n = app->cmdline.cur = n;
}

static void cl_insert(WfApp *app, uint32_t cp)
{
	char enc[4];
	size_t k, n = app->cmdline.n;

	if (cp < 0x20 || 0x7F == cp || cp > 0x10FFFF ||
	        (cp >= 0xD800 && cp <= 0xDFFF)) {
		return;
	}
	k = wf_utf8_encode(cp, enc);
	if (n + k >= sizeof app->cmdline.text) {
		/* what is typed is what runs: say that it was cut off */
		wf_app_set_msg(app, wf_msg(MSG_EX_TOO_LONG));
		return;
	}
	memmove(app->cmdline.text + app->cmdline.cur + k,
	        app->cmdline.text + app->cmdline.cur, n - app->cmdline.cur + 1);
	memcpy(app->cmdline.text + app->cmdline.cur, enc, k);
	app->cmdline.n += k;
	app->cmdline.cur += k;
}

static void cl_remove(WfApp *app, size_t from, size_t to)
{
	memmove(app->cmdline.text + from, app->cmdline.text + to,
	        app->cmdline.n - to + 1);
	app->cmdline.n -= to - from;
	app->cmdline.cur = from;
}

static void cl_remember(WfApp *app, const char *text)
{
	size_t i;

	if (!text[0] ||
	        (app->cmdline.hist_n &&
	                !strcmp(app->cmdline.hist[app->cmdline.hist_n - 1],
	                        text))) {
		return;
	}
	if (16 == app->cmdline.hist_n) {
		for (i = 1; i < 16; i++) {
			memcpy(app->cmdline.hist[i - 1], app->cmdline.hist[i],
			        sizeof app->cmdline.hist[i]);
		}
		app->cmdline.hist_n--;
	}
	snprintf(app->cmdline.hist[app->cmdline.hist_n++],
	        sizeof app->cmdline.hist[0], "%s", text);
}

static void tell_error(WfApp *app, const WfEx *ex)
{
	char text[160];

	wf_ex_error_text(ex, text, sizeof text);
	if (text[0]) {
		wf_app_set_msg(app, text);
	}
}

/* The path named by `:go`, by the rules of Smart Go. */
static void cmd_go(WfApp *app, const char *utf8)
{
	size_t n = strlen(utf8);
	wchar_t *w = malloc((n + 1) * sizeof *w);
	size_t units = 0;
	WfSmartResult r;
	WfStatus st;

	if (!w) {
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	if (WF_OK != wf_utf8_to_utf16(utf8, n, w, n + 1, &units)) {
		free(w);
		wf_app_set_msg(app, wf_msg(MSG_SMART_GO_INVALID_PATH));
		return;
	}
	memset(&r, 0, sizeof r);
	st = wf_smart_resolve(
	        &app->go_env, app->cwd, app->prev_dir, w, units, &r);
	free(w);
	if (WF_OK != st) {
		wf_app_set_error(app, st);
	} else if (WF_SG_OK != r.error || !r.target) {
		wf_app_set_msg(app, wf_smart_error_text(r.error));
	} else {
		st = wf_app_navigate(app, r.target, NULL, SIZE_MAX);
		if (WF_OK != st) {
			wf_app_set_msg(app, wf_msg(MSG_SMART_GO_INVALID_PATH));
		}
	}
	wf_smart_result_free(&r);
}

static void cmd_filter(WfApp *app, const char *utf8)
{
	wchar_t w[WF_FILTER_MAX];
	size_t units = 0;

	if (wf_utf8_to_utf16(utf8, strlen(utf8), w, WF_FILTER_MAX - 1,
	            &units) != WF_OK) {
		wf_app_set_msg(app, wf_msg(MSG_EX_TOO_LONG));
		return;
	}
	w[units] = L'\0';
	if (WF_OK != wf_view_set_filter(&app->cur, w)) {
		wf_app_set_error(
		        app, WF_E_NOMEM); /* the filter did not apply */
	}
}

/* Runs a command line. The mode has already been left, so a command may
 * open a screen of its own. */
static WfAction cmdline_run(WfApp *app, const char *text, size_t n)
{
	WfEx ex;
	char said[96];

	if (!wf_ex_parse(text, n, WF_EXS_FM, &ex)) {
		if (WF_EXE_EMPTY != ex.error) {
			tell_error(app, &ex);
		}
		return WF_ACT_REDRAW;
	}
	switch (ex.id) {
	case WF_EX_GO: cmd_go(app, ex.arg); break;
	case WF_EX_SORT:
		wf_app_set_sort(app, (WfSortKey)ex.sort_key, ex.descending);
		(void)wf_msgf(said, sizeof said, MSG_EX_SORTED,
		        (WfMsgArg[]){WF_ARG_S(wf_app_sort_name(
		                             (WfSortKey)ex.sort_key)),
		                WF_ARG_S(wf_msg(ex.descending
		                                ? MSG_SORT_DESCENDING
		                                : MSG_SORT_ASCENDING))},
		        2);
		wf_app_set_msg(app, said);
		break;
	case WF_EX_SET_HIDDEN:
		wf_app_set_hidden(app, ex.on);
		wf_app_set_msg(app,
		        wf_msg(ex.on ? MSG_EX_HIDDEN_ON : MSG_EX_HIDDEN_OFF));
		break;
	case WF_EX_FILTER: cmd_filter(app, ex.arg); break;
	case WF_EX_CLEAR_FILTER:
		(void)wf_view_set_filter(&app->cur, L"");
		break;
	case WF_EX_RELOAD:   wf_app_reload(app); break;
	case WF_EX_SETTINGS: wf_app_settings_begin(app); break;
	case WF_EX_HELP:     wf_app_help_begin(app); break;
	case WF_EX_QUIT:     return wf_app_quit_request(app);
	default:             break;
	}
	return WF_ACT_REDRAW;
}

static WfAction cmdline_event(WfApp *app, const WfEvent *ev)
{
	if (WF_EV_RESIZE == ev->type) {
		return WF_ACT_REDRAW;
	}
	if (WF_EV_CHAR == ev->type) {
		cl_insert(app, ev->ch);
		return WF_ACT_REDRAW;
	}
	if (WF_EV_CTRL == ev->type && 'u' == ev->ch) {
		cl_set(app, "");
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY != ev->type) {
		return WF_ACT_NONE;
	}
	switch (ev->key) {
	case WF_KEY_ESC:   app->mode = &WF_MODE_NORMAL; return WF_ACT_REDRAW;
	case WF_KEY_ENTER: {
		char line[sizeof app->cmdline.text];
		size_t n = app->cmdline.n;

		memcpy(line, app->cmdline.text, n + 1);
		cl_remember(app, line);
		app->mode = &WF_MODE_NORMAL;
		app->msg[0] = '\0';
		return cmdline_run(app, line, n);
	}
	case WF_KEY_LEFT:
		app->cmdline.cur = prev_boundary(
		        app->cmdline.text, app->cmdline.n, app->cmdline.cur);
		return WF_ACT_REDRAW;
	case WF_KEY_RIGHT:
		app->cmdline.cur = next_boundary(
		        app->cmdline.text, app->cmdline.n, app->cmdline.cur);
		return WF_ACT_REDRAW;
	case WF_KEY_HOME: app->cmdline.cur = 0; return WF_ACT_REDRAW;
	case WF_KEY_END:
		app->cmdline.cur = app->cmdline.n;
		return WF_ACT_REDRAW;
	case WF_KEY_BACKSPACE:
		if (!app->cmdline
		                .n) { /* as in Vim: Backspace on nothing leaves */
			app->mode = &WF_MODE_NORMAL;
			return WF_ACT_REDRAW;
		}
		if (app->cmdline.cur) {
			cl_remove(app,
			        prev_boundary(app->cmdline.text, app->cmdline.n,
			                app->cmdline.cur),
			        app->cmdline.cur);
		}
		return WF_ACT_REDRAW;
	case WF_KEY_DELETE:
		if (app->cmdline.cur < app->cmdline.n) {
			cl_remove(app, app->cmdline.cur,
			        next_boundary(app->cmdline.text, app->cmdline.n,
			                app->cmdline.cur));
		}
		return WF_ACT_REDRAW;
	case WF_KEY_UP:
		if (app->cmdline.hist_at) {
			if (app->cmdline.hist_at == app->cmdline.hist_n) {
				/* leaving the line being typed: keep it */
				memcpy(app->cmdline.draft, app->cmdline.text,
				        sizeof app->cmdline.draft);
			}
			app->cmdline.hist_at--;
			cl_set(app, app->cmdline.hist[app->cmdline.hist_at]);
		}
		return WF_ACT_REDRAW;
	case WF_KEY_DOWN:
		if (app->cmdline.hist_at < app->cmdline.hist_n) {
			app->cmdline.hist_at++;
			cl_set(app,
			        app->cmdline.hist_at < app->cmdline.hist_n
			                ? app->cmdline
			                          .hist[app->cmdline.hist_at]
			                : app->cmdline.draft);
		}
		return WF_ACT_REDRAW;
	default: return WF_ACT_NONE;
	}
}

static void cmdline_draw(const WfApp *app, WfFrame *f)
{
	WfStyle cursor = wf_app_style_bar;
	WfBuf before, at, after;
	const char *t = app->cmdline.text;
	size_t cur = app->cmdline.cur, next;
	int x = 1;

	if (f->rows < 1) {
		return;
	}
	cursor.attributes |= WF_ATTR_REVERSE;
	wf_frame_fill(f, 0, f->rows - 1, f->cols, 1, ' ', &wf_app_style_bar);
	wf_frame_put(f, 0, f->rows - 1, ":", 1, &wf_app_style_bar, f->cols);
	wf_buf_init(&before);
	wf_buf_init(&at);
	wf_buf_init(&after);
	next = next_boundary(t, app->cmdline.n, cur);
	if (WF_OK == wf_sanitize_utf8(t, cur, &before) &&
	        WF_OK == wf_sanitize_utf8(t + cur, next - cur, &at) &&
	        WF_OK ==
	                wf_sanitize_utf8(
	                        t + next, app->cmdline.n - next, &after)) {
		/* A line longer than the row scrolls sideways so the cursor stays
		 * in view: the start is dropped (marked with <) until the text
		 * before the cursor, the cursor cell and a little context after
		 * it fit. */
		const char *shown = wf_buf_cstr(&before);
		size_t shown_n = before.len;
		int wat = at.len ? wf_utf8_cells(wf_buf_cstr(&at), at.len) : 1;
		int ctx = wf_utf8_cells(wf_buf_cstr(&after), after.len);
		int room;

		if (wat < 1) {
			wat = 1;
		}
		if (ctx > 8) {
			ctx = 8;
		}
		room = f->cols - 1 - wat - ctx;
		if (room < 1) {
			room = 1;
		}
		if (wf_utf8_cells(shown, shown_n) > room) {
			wf_frame_put(f, x, f->rows - 1, "<", 1,
			        &wf_app_style_bar, 1);
			x++;
			room--;
			while (shown_n &&
			        wf_utf8_cells(shown, shown_n) > room) {
				uint32_t cp;
				size_t step =
				        wf_utf8_decode(shown, shown_n, &cp);

				if (0 == step) {
					step = 1;
				}
				shown += step;
				shown_n -= step;
			}
		}
		x += wf_frame_put(f, x, f->rows - 1, shown, shown_n,
		        &wf_app_style_bar, f->cols - x);
		wf_frame_set_cursor(f, x, f->rows - 1);
		if (at.len) {
			int w = wf_utf8_cells(wf_buf_cstr(&at), at.len);

			wf_frame_put(f, x, f->rows - 1, wf_buf_cstr(&at),
			        at.len, &cursor, w ? w : 1);
			x += w ? w : 1;
		} else {
			wf_frame_put(f, x, f->rows - 1, " ", 1, &cursor, 1);
			x++;
		}
		if (x < f->cols) {
			wf_frame_put(f, x, f->rows - 1, wf_buf_cstr(&after),
			        after.len, &wf_app_style_bar, f->cols - x);
		}
	}
	wf_buf_free(&before);
	wf_buf_free(&at);
	wf_buf_free(&after);
}

const WfMode WF_MODE_CMDLINE = {"CMDLINE", cmdline_draw, cmdline_event};

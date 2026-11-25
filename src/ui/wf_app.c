#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../edit/wf_ekeymap.h"
#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"

void wf_app_set_msg(WfApp *app, const char *text)
{
	WfBuf clean;
	size_t n;

	if (!app || !text) {
		return;
	}
	wf_buf_init(&clean);
	if (WF_OK != wf_sanitize_utf8(text, strlen(text), &clean)) {
		snprintf(app->msg, sizeof app->msg, "%s",
		        wf_msg(MSG_XFER_NOMEM));
		app->msg_keep_until = app->now_ms + WF_RECEIPT_KEEP_MS;
		wf_buf_free(&clean);
		return;
	}
	n = clean.len < sizeof app->msg - 1 ? clean.len : sizeof app->msg - 1;
	while (n > 0 && n < clean.len &&
	        0x80 == ((unsigned char)clean.data[n] & 0xC0)) {
		n--; /* never cut a UTF-8 sequence in half */
	}
	bool changed =
	        strlen(app->msg) != n || (n && memcmp(app->msg, clean.data, n));
	if (n) {
		memcpy(app->msg, clean.data, n);
	}
	app->msg[n] = '\0';
	/* A new result owns a full reading interval; repeated identical updates
	 * must not keep a transient notice alive indefinitely. */
	if (n && (changed || app->msg_keep_until <= app->now_ms)) {
		app->msg_keep_until = app->now_ms + WF_RECEIPT_KEEP_MS;
	}
	wf_buf_free(&clean);
}

void wf_app_set_xfer(WfApp *app, WfXferPort port)
{
	app->xfer = port;
}

void wf_app_set_exec(WfApp *app, WfExecPort port)
{
	app->exec = port;
}

WfStatus wf_app_init(
        WfApp *app, int cols, int rows, WfFsPort fs, const wchar_t *start_path)
{
	WfStatus st;

	if (!app || !start_path) {
		return WF_E_INVAL;
	}
	memset(app, 0, sizeof *app);
	app->mode = &WF_MODE_NORMAL;
	app->cols = cols;
	app->rows = rows;
	app->ratios = WF_RATIOS_DEFAULT;
	app->fs = fs;
	app->cwd_version = 1;
	app->confirm_unc_navigation = true;
	wf_view_init(&app->cur);
	app->cur.guard = wf_app_fuzzy_guard;
	app->cur.guard_ctx = app;
	wf_view_init(&app->parent);
	wf_view_init(&app->dirprev);
	wf_sel_init(&app->sel);
	wf_app_md_create(app); /* optional: without it Markdown shows as text */
	{
		WfKeyConfig none;

		wf_keyconfig_init(&none);
		if (WF_OK != wf_app_set_keys(app, &none, WF_KM_PIERO)) {
			wf_app_md_destroy(app);
			return WF_E_NOMEM;
		}
	}

	app->cwd = wf_path_dup(start_path);
	if (!app->cwd) {
		wf_app_keys_destroy(app);
		return WF_E_NOMEM;
	}
	st = wf_app_navigate(app, start_path, NULL, SIZE_MAX);
	if (WF_OK != st) {
		wf_app_destroy(app);
	}
	return st;
}

void wf_app_destroy(WfApp *app)
{
	wf_app_fuzzy_destroy(app);
	wf_app_batch_destroy(app);
	free(app->preview_keep.path);
	app->preview_keep.path = NULL;
	wf_app_preview_clear(app);
	wf_app_md_destroy(app);
	wf_app_dircache_destroy(app);
	wf_app_free_want(app);
	wf_app_navigation_memory_destroy(app);
	wf_view_free(&app->cur);
	wf_view_free(&app->parent);
	wf_view_free(&app->dirprev);
	wf_sel_free(&app->sel);
	wf_app_keys_destroy(app);
	wf_app_xfer_destroy(app);
	wf_app_copy_destroy(app);
	wf_app_lock_destroy(app);
	wf_app_open_destroy(app);
	wf_app_dirsize_destroy(app);
	wf_app_hash_destroy(app);
	wf_app_hits_destroy(app);
	wf_app_git_destroy(app);
	wf_app_tabs_destroy(app);
	wf_app_session_destroy(app);
	free(app->cwd);
	free(app->prev_dir);
	app->cwd = NULL;
	free(app->path_input);
	wf_app_path_state_destroy(app);
	free(app->unc_approved_root);
	wf_app_free_pending_navigation(app);
	for (size_t i = 0; i < app->history_n; i++) {
		free(app->history[i].path);
		free(app->history[i].select);
	}
	app->history_n = 0;
}

void wf_app_toggle_hidden(WfApp *app)
{
	wf_app_set_hidden(app, !app->cur.show_hidden);
}

void wf_app_set_hidden(WfApp *app, bool show)
{
	wf_app_dircache_invalidate(app);
	WfStatus a = wf_view_set_hidden(&app->cur, show);
	WfStatus b = wf_view_set_hidden(&app->parent, show);
	WfStatus c = wf_view_set_hidden(&app->dirprev, show);

	if (WF_OK != a || WF_OK != b || WF_OK != c) {
		wf_app_set_error(app, WF_OK != a ? a : WF_OK != b ? b : c);
	}
	if (app->config) {
		app->config->show_hidden = show;
	}
}

/* Toggles the cursor entry and moves down, so Space-Space-Space selects a run.
 */
void wf_app_toggle_select(WfApp *app)
{
	const WfEntry *e = wf_view_current(&app->cur);
	WfStatus st;
	bool had, was;

	if (!e) {
		return;
	}
	had = wf_app_counts_current(app);
	was = wf_nameset_has(wf_sel_folder(&app->sel, app->cwd), e->name);
	st = wf_sel_toggle(&app->sel, app->cwd, e->name);
	if (WF_OK != st) {
		wf_app_set_error(app, st);
	} else {
		wf_app_counts_toggled(app, had, was);
		wf_view_move(&app->cur, 1, wf_app_body_height(app));
	}
}

static void quit_yes(WfApp *app, void *ctx)
{
	(void)ctx;
	app->quit_now = true;
}

/* `q`: leaves at once when nothing is running; otherwise says what is going
 * on and what leaving does to it, and leaves only after a yes. */
WfAction wf_app_quit_request(WfApp *app)
{
	const char *lines[4];
	char detail[160];
	WfStatus st;
	int n = 0;

	if (!app->copy_jobs && !app->move.active && !app->edit_busy &&
	        !app->batch.busy && WF_XB_IDLE == app->xfer_busy &&
	        !app->launch_busy) {
		return WF_ACT_QUIT;
	}
	lines[n++] = wf_msg(MSG_QUIT_TITLE);
	if (app->move.active) {
		(void)wf_msgf(detail, sizeof detail, MSG_QUIT_MOVE,
		        (WfMsgArg[]){WF_ARG_N(app->move.moved),
		                WF_ARG_N(app->move.slot.count)},
		        2);
		lines[n++] = detail;
	} else if (app->batch.busy) {
		lines[n++] = wf_msg(MSG_QUIT_BATCH);
	} else if (app->copy_jobs) {
		(void)wf_msgf(detail, sizeof detail, MSG_QUIT_COPY,
		        &WF_ARG_N(app->copy_jobs), 1);
		lines[n++] = detail;
	} else {
		lines[n++] = wf_msg(MSG_QUIT_BUSY);
	}
	lines[n++] = wf_msg(MSG_QUIT_ASK);
	st = wf_app_confirm_begin(app, lines, n, quit_yes, NULL, NULL);
	if (WF_OK != st) {
		wf_app_set_error(app, st);
	}
	return WF_ACT_REDRAW;
}

static WfAction normal_event(WfApp *app, const WfEvent *ev)
{
	WfAction action;

	if (WF_EV_MOUSE == ev->type) {
		action = wf_app_mouse(app, ev);
		if (WF_ACT_REDRAW == action) {
			wf_app_preview_sync(app, false);
		}
		return action;
	}
	/* Pasted text is text, not a run of commands: the list's keys never
	 * see it. (A prompt or the editor takes it as typed.) */
	if (ev->vk & WF_VK_PASTED) {
		if (strcmp(app->msg, wf_msg(MSG_PASTE_IGNORED))) {
			wf_app_set_msg(app, wf_msg(MSG_PASTE_IGNORED));
			return WF_ACT_REDRAW;
		}
		return WF_ACT_NONE;
	}
	action = wf_app_fm_key(app, ev);
	if (WF_ACT_REDRAW == action) {
		wf_app_preview_sync(app, false);
	}
	return action;
}

const WfMode WF_MODE_NORMAL = {"NORMAL", NULL, normal_event};

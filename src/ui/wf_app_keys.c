/*
 * The file manager's keys: a key becomes a stroke, the stroke is fed to the
 * keymap of the current context, and a finished sequence names a command that
 * is carried out here by calling the code that has always done it. Nothing
 * below knows which key asked.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../edit/wf_ekeymap.h"
#include "wf_app_priv.h"
#include "wf_app_settings_priv.h"

WfStatus wf_app_set_keys(
        WfApp *app, const WfKeyConfig *cfg, int editor_legacy_keymap)
{
	WfKeymap *m = NULL;
	WfKeyIssue issue;
	WfStatus st = wf_keymap_build(cfg, editor_legacy_keymap, &m, &issue);

	memset(&app->keys_issue, 0, sizeof app->keys_issue);
	if (WF_E_INVAL == st) {
		/* the configured keys cannot be used: the profile's own are */
		app->keys_issue = issue;
		st = wf_keymap_build_defaults(cfg, editor_legacy_keymap, &m);
	}
	if (WF_OK != st) {
		return st;
	}
	wf_keymap_free(app->keymap_prev);
	app->keymap_prev = app->keymap;
	app->keymap = m;
	wf_keystate_reset(&app->keys);
	return WF_OK;
}

const WfKeymap *wf_app_keymap(const WfApp *app)
{
	return app->keymap;
}

void wf_app_keys_warn(WfApp *app)
{
	char why[112], text[200];

	if (WF_KI_NONE == app->keys_issue.kind) {
		return;
	}
	wf_key_issue_text(&app->keys_issue, why, sizeof why);
	(void)wf_msgf(text, sizeof text, MSG_KEYS_FALLBACK, &WF_ARG_S(why), 1);
	wf_app_set_msg(app, text);
}

void wf_app_keys_destroy(WfApp *app)
{
	wf_keymap_free(app->keymap);
	wf_keymap_free(app->keymap_prev);
	app->keymap = NULL;
	app->keymap_prev = NULL;
}

size_t wf_app_key_text(
        const WfApp *app, WfKeyContext ctx, uint16_t cmd, char *out, size_t cap)
{
	return wf_keymap_keys_text(app->keymap, ctx, cmd, out, cap);
}

WfKeyResult wf_app_feed(WfApp *app, WfKeyContext ctx, WfStroke stroke)
{
	if (app->keys_ctx != ctx) {
		wf_keystate_reset(&app->keys);
		app->keys_ctx = ctx;
	}
	return wf_keymap_feed(
	        app->keymap, ctx, &app->keys, stroke, app->now_ms);
}

/* Resolve every displayed chord from the same map that dispatches input. */
void wf_app_binding_hint(const WfApp *app, bool preview, char *out, size_t cap)
{
	const uint16_t fm[] = {WF_FM_HELP, WF_FM_NAV_OPEN, WF_FM_SMART_GO_PATH};
	const uint16_t pv[] = {WF_PV_CLOSE, WF_PV_DOWN, WF_PV_PAN_RIGHT};
	const WfMsg fm_labels[] = {MSG_CMD_FM_HELP, MSG_HINT_OPEN, MSG_HINT_GO};
	const WfMsg pv_labels[] = {
	        MSG_HINT_CLOSE, MSG_HINT_SCROLL, MSG_HINT_PAN};
	const uint16_t *cmds = preview ? pv : fm;
	const WfMsg *labels = preview ? pv_labels : fm_labels;
	WfKeyContext ctx = preview ? WF_CTX_PREVIEW : WF_CTX_FM;
	size_t used = 0;

	if (!cap) {
		return;
	}
	out[0] = '\0';
	for (size_t i = 0; i < 3; i++) {
		char key[32];
		int n;
		wf_keymap_first_key(app->keymap, ctx, cmds[i], key, sizeof key);
		if (!key[0]) {
			continue;
		}
		n = snprintf(out + used, cap - used, "%s%s %s",
		        used ? "  " : "", key, wf_msg(labels[i]));
		if (n < 0 || (size_t)n >= cap - used) {
			break;
		}
		used += (size_t)n;
	}
}

uint32_t wf_app_keys_timeout(const WfApp *app)
{
	if (!wf_keystate_active(&app->keys)) {
		return WF_WAIT_FOREVER;
	}
	return app->now_ms >= app->keys.deadline
	        ? 0
	        : (uint32_t)(app->keys.deadline - app->now_ms);
}

bool wf_app_keys_tick(WfApp *app)
{
	if (!wf_keystate_active(&app->keys) ||
	        app->now_ms < app->keys.deadline) {
		return false;
	}
	wf_keystate_reset(&app->keys);
	return true; /* the half-typed keys leave the status line */
}

static void say_count_refused(WfApp *app)
{
	wf_app_set_msg(app, wf_msg(MSG_KEYS_COUNT_REFUSED));
}

/* Rows moved by a command that can be given a number. */
static long rows_of(uint32_t count)
{
	return count > 0 ? (long)count : 1;
}

/* The row a count names (1-based), or the edge when no number was typed. */
static void go_to_row(WfApp *app, bool last, bool counted, uint32_t count)
{
	int h = wf_app_body_height(app);

	wf_view_move_edge(&app->cur, false, h);
	if (counted) {
		wf_view_move(&app->cur, rows_of(count) - 1, h);
	} else if (last) {
		wf_view_move_edge(&app->cur, true, h);
	}
}

static WfAction
run(WfApp *app, const WfEvent *ev, uint16_t cmd, bool counted, uint32_t count)
{
	int h = wf_app_body_height(app);
	const WfEntry *entry = wf_view_current(&app->cur);
	bool enterable =
	        entry && (entry->flags & (WF_ENT_DIR | WF_ENT_ARCHIVE));

	switch (cmd) {
	case WF_FM_NAV_UP:   wf_view_move(&app->cur, -rows_of(count), h); break;
	case WF_FM_NAV_DOWN: wf_view_move(&app->cur, rows_of(count), h); break;
	case WF_FM_NAV_FIRST:  go_to_row(app, false, counted, count); break;
	case WF_FM_NAV_LAST:   go_to_row(app, true, counted, count); break;
	case WF_FM_NAV_PARENT: wf_app_go_parent(app); break;
	case WF_FM_NAV_ENTER:  wf_app_enter_selected(app); break;
	case WF_FM_NAV_RIGHT:
		if (enterable) {
			wf_app_enter_selected(app);
		} else {
			wf_app_preview_focus(app);
		}
		break;
	case WF_FM_NAV_OPEN:
		/* the Enters that finished the first-run screens, still coming
		 * in, must not open the first file */
		if (app->now_ms < app->enter_guard_until) {
			return WF_ACT_NONE;
		}
		if (entry && !enterable) {
			wf_app_open_selected(app);
		} else {
			wf_app_enter_selected(app);
		}
		break;
	case WF_FM_HISTORY_BACK:    wf_app_history_go(app, false); break;
	case WF_FM_HISTORY_FORWARD: wf_app_history_go(app, true); break;
	case WF_FM_SMART_GO:        wf_app_path_prompt_begin(app); break;
	case WF_FM_SMART_GO_PATH:   wf_app_path_bar_begin(app); break;
	case WF_FM_FILTER:          app->mode = &WF_MODE_FILTER; break;
	case WF_FM_HIDDEN:          wf_app_toggle_hidden(app); break;
	case WF_FM_RELOAD:
		wf_app_dirsize_forget(app);
		wf_app_reload(app);
		break;
	case WF_FM_DIRSIZE:      wf_app_cmd_dirsize(app); break;
	case WF_FM_CLIP_PASTE:   wf_app_cmd_clip_paste(app); break;
	case WF_FM_LOCKS:        wf_app_cmd_locks(app); break;
	case WF_FM_CLIP_COPY:    wf_app_cmd_clip_copy(app); break;
	case WF_FM_FUZZY:        wf_app_cmd_fuzzy(app); break;
	case WF_FM_BULK_RENAME:  wf_app_cmd_bulk_rename(app); break;
	case WF_FM_HASH:         wf_app_cmd_hash(app); break;
	case WF_FM_TAB_NEW:      wf_app_cmd_tab_new(app); break;
	case WF_FM_TAB_CLOSE:    wf_app_cmd_tab_close(app); break;
	case WF_FM_TAB_PREV:     wf_app_cmd_tab_move(app, -1); break;
	case WF_FM_TAB_NEXT:     wf_app_cmd_tab_move(app, 1); break;
	case WF_FM_PLACE_SET:    wf_app_cmd_place_set(app); break;
	case WF_FM_PLACE_GO:     wf_app_cmd_place_go(app); break;
	case WF_FM_DUPES:        wf_app_dupes_begin(app); break;
	case WF_FM_TERMINAL:     wf_app_cmd_terminal(app); break;
	case WF_FM_INFO:         wf_app_info_begin(app); break;
	case WF_FM_PREVIEW_FULL: app->mode = &WF_MODE_PREVIEW; break;
	case WF_FM_PREVIEW_LOAD: wf_app_preview_sync(app, true); break;
	case WF_FM_SELECT:       wf_app_toggle_select(app); break;
	case WF_FM_CANCEL:
		/* first a running count, then the selection, then a filter that is
		 * on, then the copy / move slot: one thing is cancelled per key,
		 * in this order */
		if (wf_app_dirsize_stop(app)) {
			break;
		}
		if (wf_app_hash_stop(app)) {
			break;
		}
		if (wf_sel_count(&app->sel, app->cwd) > 0) {
			wf_sel_clear(&app->sel, app->cwd);
		} else if (app->cur.filter[0]) {
			(void)wf_view_set_filter(&app->cur, L"");
			wf_app_preview_sync(app, false);
		} else {
			wf_app_xfer_drop_slot(app);
		}
		break;
	case WF_FM_CREATE:        wf_app_cmd_create(app); break;
	case WF_FM_CREATE_FILE:   wf_app_cmd_create_kind(app, true); break;
	case WF_FM_CREATE_FOLDER: wf_app_cmd_create_kind(app, false); break;
	case WF_FM_RENAME:        wf_app_cmd_rename(app); break;
	case WF_FM_RECYCLE:       wf_app_cmd_recycle(app, ev); break;
	case WF_FM_COPY_STAGE:    wf_app_cmd_stage(app, false); break;
	case WF_FM_MOVE_STAGE:    wf_app_cmd_stage(app, true); break;
	case WF_FM_PASTE:         wf_app_cmd_paste(app); break;
	case WF_FM_QUEUE_PAUSE:   wf_app_cmd_pause(app); break;
	case WF_FM_QUEUE_CANCEL:  wf_app_cmd_cancel_jobs(app); break;
	case WF_FM_UNDO:          wf_app_cmd_undo(app); break;
	case WF_FM_JOBLOG:        wf_app_joblog_begin(app); break;
	case WF_FM_BACKUPS:       wf_app_backups_begin(app); break;
	case WF_FM_EDIT:          wf_app_edit_begin(app); break;
	case WF_FM_SORT:          wf_app_sort_begin(app); break;
	case WF_FM_SETTINGS:      wf_app_settings_begin(app); break;
	case WF_FM_HELP:          wf_app_help_begin(app); break;
	case WF_FM_QUIT:          return wf_app_quit_request(app);
	case WF_FM_CMDLINE:       wf_app_cmdline_begin(app); break;
	default:                  return WF_ACT_NONE;
	}
	return WF_ACT_REDRAW;
}

WfAction wf_app_fm_key(WfApp *app, const WfEvent *ev)
{
	WfStroke s;
	WfKeyResult r;

	if (!wf_stroke_from_event(ev, &s)) {
		return WF_ACT_NONE;
	}
	if (s.named && WF_NK_ENTER == s.code && !s.mods &&
	        app->now_ms < app->enter_guard_until) {
		return WF_ACT_NONE;
	}
	r = wf_app_feed(app, WF_CTX_FM, s);
	switch (r.verdict) {
	case WF_KR_NONE:      return WF_ACT_NONE;
	case WF_KR_PENDING:
	case WF_KR_CANCELLED: return WF_ACT_REDRAW;
	case WF_KR_REFUSED:   say_count_refused(app); return WF_ACT_REDRAW;
	case WF_KR_COMMAND:   break;
	}
	if (!s.named) {
		app->msg[0] = '\0';
	}
	return run(app, ev, r.cmd, r.counted, r.count);
}

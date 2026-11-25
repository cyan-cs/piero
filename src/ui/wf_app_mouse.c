/*
 * The pointer in the file manager, and what pasted keys may reach.
 *
 * The pointer works on what is under it: a click in the list puts the cursor
 * on that entry and a double click opens it; a click on an entry of the parent
 * column goes to it (a folder is entered, a file is shown in its own folder);
 * the wheel scrolls the list under it, or the preview under it. Everything
 * goes through the keys and moves the list already has, so every rule and
 * every refusal they have applies unchanged.
 */
#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"
#include "wf_app_priv.h"
#include "wf_layout.h"

#define WHEEL_STEP 3

/* The same thing the Enter key of the list does, without going through the
 * key map (a rebound Enter must not change what a double click means). */
static void open_current(WfApp *app)
{
	const WfEntry *entry = wf_view_current(&app->cur);
	bool enterable =
	        entry && (entry->flags & (WF_ENT_DIR | WF_ENT_ARCHIVE));

	if (entry && !enterable) {
		wf_app_open_selected(app);
	} else {
		wf_app_enter_selected(app);
	}
}

static bool inside(const WfSpan *s, int x)
{
	return s->w > 0 && x >= s->x && x < s->x + s->w;
}

/* A click on an entry of the parent column. */
static WfAction click_parent(WfApp *app, const WfLayout *lay, int y)
{
	size_t at, plen = wf_path_parent_len(app->cwd);
	const WfEntry *e;
	wchar_t *folder, *target = NULL;
	WfStatus st;

	if (!app->parent.dir || 0 == plen) {
		return WF_ACT_NONE;
	}
	at = wf_view_top(&app->parent, lay->body_h) + (size_t)(y - lay->body_y);
	if (at >= app->parent.nvis) {
		return WF_ACT_NONE; /* below the last entry */
	}
	e = &app->parent.dir->e[app->parent.vis[at]];
	folder = wf_path_dup_n(app->cwd, plen);
	if (!folder) {
		return WF_ACT_NONE;
	}
	if (e->flags & WF_ENT_DIR) {
		st = wf_path_join(folder, e->name, &target);
		if (WF_OK == st) {
			st = wf_app_navigate(app, target, NULL, SIZE_MAX);
		}
	} else {
		st = wf_app_navigate(app, folder, e->name, SIZE_MAX);
	}
	free(target);
	free(folder);
	return WF_OK == st ? WF_ACT_REDRAW : WF_ACT_NONE;
}

WfAction wf_app_mouse(WfApp *app, const WfEvent *ev)
{
	WfLayout lay;
	int x = WF_MOUSE_X(ev), y = WF_MOUSE_Y(ev);
	const WfSpan *cur;
	bool in_body;

	wf_layout_compute(app->cols, app->rows, &app->ratios, &lay);
	if (lay.minimal || app->want_path) {
		return WF_ACT_NONE;
	}
	cur = &lay.pane[WF_PANE_CURRENT];
	in_body = y >= lay.body_y && y < lay.body_y + lay.body_h;
	if (ev->vk & (WF_MOUSE_WHEEL_UP | WF_MOUSE_WHEEL_DOWN)) {
		bool up = 0 != (ev->vk & WF_MOUSE_WHEEL_UP);

		if (!in_body) {
			return WF_ACT_NONE;
		}
		if (inside(&lay.pane[WF_PANE_PREVIEW], x)) {
			/* what is under the pointer is what moves */
			wf_app_preview_scroll(
			        app, up ? -WHEEL_STEP : WHEEL_STEP);
			return WF_ACT_REDRAW;
		}
		if (!inside(cur, x)) {
			return WF_ACT_NONE;
		}
		/* the view clamps at the ends */
		wf_view_move(
		        &app->cur, up ? -WHEEL_STEP : WHEEL_STEP, lay.body_h);
		return WF_ACT_REDRAW;
	}
	if (!(ev->vk & WF_MOUSE_LEFT) || !in_body) {
		return WF_ACT_NONE;
	}
	if (inside(&lay.pane[WF_PANE_PARENT], x)) {
		return click_parent(app, &lay, y);
	}
	if (inside(cur, x)) {
		size_t at = wf_view_top(&app->cur, lay.body_h) +
		        (size_t)(y - lay.body_y);

		if (at >= app->cur.nvis) {
			return WF_ACT_NONE; /* below the last entry */
		}
		wf_view_move(&app->cur, (long)at - (long)app->cur.cursor,
		        lay.body_h);
		if (ev->vk & WF_MOUSE_DOUBLE) {
			if (app->now_ms < app->enter_guard_until) {
				return WF_ACT_REDRAW;
			}
			open_current(app);
		}
		return WF_ACT_REDRAW;
	}
	return WF_ACT_NONE;
}

static bool scrolls_by_wheel(const WfApp *app)
{
	return &WF_MODE_HELP == app->mode || &WF_MODE_JOBLOG == app->mode ||
	        &WF_MODE_REPORT == app->mode || &WF_MODE_BACKUPS == app->mode ||
	        &WF_MODE_INFO == app->mode || &WF_MODE_PREVIEW == app->mode ||
	        &WF_MODE_PREVIEW_PANE == app->mode ||
	        (&WF_MODE_SETTINGS == app->mode &&
	                WF_SS_BROWSE == app->settings.sub);
}

bool wf_app_window_wheel(WfApp *app, const WfEvent *ev, WfAction *act)
{
	WfEvent key = {WF_EV_KEY, 0, WF_KEY_NONE, 0};

	*act = WF_ACT_NONE;
	if (WF_EV_MOUSE != ev->type || !scrolls_by_wheel(app) ||
	        !(ev->vk & (WF_MOUSE_WHEEL_UP | WF_MOUSE_WHEEL_DOWN))) {
		return false;
	}
	key.key = ev->vk & WF_MOUSE_WHEEL_UP ? WF_KEY_UP : WF_KEY_DOWN;
	for (int i = 0; i < WHEEL_STEP; i++) {
		if (WF_ACT_REDRAW == app->mode->on_event(app, &key)) {
			*act = WF_ACT_REDRAW;
		}
		if (!scrolls_by_wheel(app)) {
			break; /* a key closed the window: do not press on */
		}
	}
	return true;
}

/* Modes with a line the user types: pasted characters are text there. */
static bool takes_text(const WfApp *app)
{
	return &WF_MODE_GO == app->mode || &WF_MODE_CMDLINE == app->mode ||
	        &WF_MODE_FILTER == app->mode || &WF_MODE_FUZZY == app->mode ||
	        (&WF_MODE_BATCH == app->mode &&
	                WF_BS_PATTERN == app->batch.stage) ||
	        &WF_MODE_IMAGE_PASSWORD == app->mode ||
	        !strcmp(app->mode->name, "NAME") ||
	        wf_app_settings_takes_text(app);
}

bool wf_app_paste_filter(WfApp *app, const WfEvent *ev)
{
	/* The pointer works on the file list only: in a question, a prompt, the
	 * help or a report it neither answers nor closes anything. */
	if (WF_EV_MOUSE == ev->type) {
		return &WF_MODE_NORMAL == app->mode;
	}
	if (!(ev->vk & WF_VK_PASTED)) {
		/* a key-up between pasted keys does not end the paste */
		if (WF_EV_RELEASE != ev->type) {
			app->paste_cut = false;
		}
		return true;
	}
	/* the list says so itself; the editor sorts out its own prompts */
	if (&WF_MODE_NORMAL == app->mode || &WF_MODE_EDITOR == app->mode) {
		return true;
	}
	if (!takes_text(app)) {
		return false; /* a question or a choice: pasted keys answer nothing */
	}
	if (WF_EV_KEY == ev->type) { /* Enter or Tab: never confirms */
		app->paste_cut =
		        true; /* and the lines after it are not taken */
		return false;
	}
	return !app->paste_cut;
}

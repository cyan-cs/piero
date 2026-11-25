/*
 * The sort menu (S): the order in force, the row the arrows are on, and the
 * one place that changes the sort of the three lists.
 */
#include <stdio.h>
#include <string.h>

#include "../base/wf_macro.h"
#include "wf_app_priv.h"

static void current_sort(const WfApp *app, WfSortKey *key, bool *desc)
{
	*key = app->config ? (WfSortKey)app->config->sort_by : app->sort_key;
	*desc = app->config ? 0 != app->config->sort_descending
	                    : app->sort_desc;
}

/* `[files] directories_first`; true without settings. */
static bool dirs_first(const WfApp *app)
{
	return !app->config || 0 != app->config->directories_first;
}

void wf_app_set_sort(WfApp *app, WfSortKey key, bool descending)
{
	WfStatus a = WF_OK, b = WF_OK, c = WF_OK;

	app->sort_key = key;
	app->sort_desc = descending;
	if (app->config) {
		/* for this session; the saved setting changes in Settings */
		app->config->sort_by = (WfConfigSortBy)key;
		app->config->sort_descending = descending;
	}
	if (app->cur.dir) {
		a = wf_view_resort(&app->cur, key, descending, dirs_first(app));
	}
	if (app->parent.dir) {
		b = wf_view_resort(
		        &app->parent, key, descending, dirs_first(app));
	}
	if (app->dirprev.dir) {
		c = wf_view_resort(
		        &app->dirprev, key, descending, dirs_first(app));
	}
	if (WF_OK != a || WF_OK != b || WF_OK != c) {
		wf_app_set_error(app, WF_OK != a ? a : WF_OK != b ? b : c);
	}
}

static const WfMsg SORT_NAMES[] = {
        MSG_SORT_NAME, MSG_SORT_SIZE, MSG_SORT_DATE, MSG_SORT_EXT};

const char *wf_app_sort_name(WfSortKey key)
{
	return wf_msg(SORT_NAMES[key]);
}
static const char SORT_LETTERS[] = "nsde";

void wf_app_sort_begin(WfApp *app)
{
	WfSortKey key;
	bool desc;

	current_sort(app, &key, &desc);
	app->sort_pick = (int)key; /* the arrows start on what is in force */
	app->mode = &WF_MODE_SORT;
}

static void sort_draw(const WfApp *app, WfFrame *f)
{
	char row[4][64], state[96], hint[96];
	const char *lines[8];
	WfSortKey key;
	bool desc;
	int i;

	current_sort(app, &key, &desc);
	for (i = 0; i < (int)WF_ARRAY_LEN(row); i++) {
		/* '>' is the order in force, '*' the row the arrows are on */
		snprintf(row[i], sizeof row[i], "%c%c [%c] %s",
		        (int)key == i ? '>' : ' ',
		        app->sort_pick == i ? '*' : ' ', SORT_LETTERS[i],
		        wf_msg(SORT_NAMES[i]));
	}
	(void)wf_msgf(state, sizeof state, MSG_SORT_NOW,
	        (WfMsgArg[]){WF_ARG_S(wf_msg(SORT_NAMES[key])),
	                WF_ARG_S(wf_msg(desc ? MSG_SORT_DESCENDING
	                                     : MSG_SORT_ASCENDING))},
	        2);
	snprintf(hint, sizeof hint, "%s", wf_msg(MSG_SORT_HINT));
	lines[0] = wf_msg(MSG_SORT_TITLE);
	lines[1] = "";
	for (i = 0; i < 4; i++) {
		lines[2 + i] = row[i];
	}
	lines[6] = state;
	lines[7] = hint;
	wf_ui_alert_or_text(f, lines, 8);
}

static WfAction sort_event(WfApp *app, const WfEvent *ev)
{
	WfSortKey key;
	bool desc;
	int i;

	if (WF_EV_RESIZE == ev->type) {
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY == ev->type && WF_KEY_UP == ev->key) {
		app->sort_pick = (app->sort_pick + 3) % 4;
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY == ev->type && WF_KEY_DOWN == ev->key) {
		app->sort_pick = (app->sort_pick + 1) % 4;
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY == ev->type && WF_KEY_ENTER == ev->key) {
		/* Enter applies the row the arrows are on, if it is not the one
		 * in force already, and closes */
		current_sort(app, &key, &desc);
		if ((int)key != app->sort_pick) {
			wf_app_set_sort(app, (WfSortKey)app->sort_pick, false);
		}
		app->mode = &WF_MODE_NORMAL;
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY == ev->type && WF_KEY_ESC == ev->key) {
		app->mode = &WF_MODE_NORMAL;
		return WF_ACT_REDRAW;
	}
	if (WF_EV_CHAR != ev->type) {
		return WF_ACT_NONE;
	}
	if ('q' == ev->ch) {
		app->mode = &WF_MODE_NORMAL;
		return WF_ACT_REDRAW;
	}
	for (i = 0; i < 4; i++) {
		if (ev->ch == (uint32_t)SORT_LETTERS[i] ||
		        ev->ch == (uint32_t)SORT_LETTERS[i] - 32u) {
			current_sort(app, &key, &desc);
			/* the same letter again turns the order round */
			wf_app_set_sort(app, (WfSortKey)i,
			        (int)key == i ? !desc : false);
			app->sort_pick = i;
			return WF_ACT_REDRAW;
		}
	}
	return WF_ACT_NONE;
}

const WfMode WF_MODE_SORT = {"SORT", sort_draw, sort_event};

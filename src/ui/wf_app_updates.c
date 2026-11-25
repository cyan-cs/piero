#include "wf_app_settings_priv.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define WF_UPDATE_ENTER_GUARD_MS 700u

static const WfMsg states[] = {MSG_UPDATE_IDLE, MSG_UPDATE_CHECKING,
        MSG_UPDATE_CURRENT, MSG_UPDATE_AVAILABLE, MSG_UPDATE_NO_RELEASE,
        MSG_UPDATE_LIMITED, MSG_UPDATE_UNAVAILABLE, MSG_UPDATE_UNAVAILABLE,
        MSG_UPDATE_THROTTLED};
static size_t count(const WfApp *app)
{
	(void)app;
	return 4;
}
static void row(const WfApp *app, size_t i, WfSetRow *out)
{
	static const WfMsg labels[] = {MSG_UPDATE_AUTO, MSG_UPDATE_MANUAL,
	        MSG_UPDATE_VERSION, MSG_UPDATE_RESULT};
	memset(out, 0, sizeof *out);
	if (i >= 4) {
		return;
	}
	snprintf(out->label, sizeof out->label, "%s", wf_msg(labels[i]));
	const char *value = "";
	if (i == 0) {
		value = wf_msg(app->settings.draft.update_checks
		                ? MSG_UPDATE_ON
		                : MSG_UPDATE_OFF);
		out->cycle = true;
	}
	if (i == 2) {
		value = app->version ? app->version : "?";
	}
	if (i == 3) {
		unsigned state = (unsigned)app->update_result.state;
		value = wf_msg(states[state < sizeof states / sizeof states[0]
		                ? state
		                : WF_UPDATE_UNAVAILABLE]);
		if (app->update_result.state == WF_UPDATE_AVAILABLE) {
			value = app->update_result.latest;
		}
	}
	snprintf(out->value, sizeof out->value, "%s", value);
}
static WfAction event(WfApp *app, const WfEvent *ev)
{
	if (!((ev->type == WF_EV_KEY && ev->key == WF_KEY_ENTER) ||
	            (ev->type == WF_EV_CHAR && ev->ch == ' '))) {
		return WF_ACT_NONE;
	}
	if (app->settings.item == 0) {
		app->settings.draft.update_checks =
		        !app->settings.draft.update_checks;
		app->settings.draft.update_consent = 1;
	} else if (app->settings.item == 1 && app->update_request) {
		if (app->update_request(app->update_context, true) == WF_OK) {
			app->update_result.state = WF_UPDATE_CHECKING;
		}
	}
	return WF_ACT_REDRAW;
}
static const char *help(const WfApp *app)
{
	return app->update_result.state == WF_UPDATE_AVAILABLE
	        ? WF_UPDATE_RELEASES
	        : wf_msg(MSG_UPDATE_MANUAL_ONLY);
}
static WfMsg hint(const WfApp *app)
{
	(void)app;
	return MSG_UPDATE_SETTINGS_HINT;
}
const WfSetPage WF_SETPAGE_UPDATES = {count, row, event, help, hint};

void wf_app_update_consent_begin(WfApp *app)
{
	app->update_prompted = true;
	app->update_choice = false;
	app->mode = &WF_MODE_UPDATE_CONSENT;
}
void wf_app_update_complete(WfApp *app, const WfUpdateResult *result)
{
	app->update_result = *result;
	if (result->state == WF_UPDATE_AVAILABLE) {
		wf_app_set_msg(app, wf_msg(MSG_UPDATE_AVAILABLE));
	}
}
static void draw(const WfApp *app, WfFrame *f)
{
	const WfThemeStyles *th = wf_theme_styles();
	wf_frame_fill(f, 0, 0, f->cols, f->rows, ' ', &th->base);
	const WfMsg lines[] = {MSG_UPDATE_ASK, MSG_UPDATE_PRIVACY,
	        MSG_UPDATE_MANUAL_ONLY, MSG_UPDATE_CHANGE_LATER};
	for (int i = 0; i < 4; i++) {
		if (i + 1 < f->rows) {
			wf_app_put_fit(f, 1, i + 1, wf_msg(lines[i]),
			        i ? &th->base : &th->accent, f->cols - 2);
		}
	}
	for (int i = 0; i < 2; i++) {
		if (i + 6 < f->rows) {
			wf_app_put_fit(f, 1, i + 6,
			        wf_msg(i ? MSG_UPDATE_NOT_NOW
			                 : MSG_UPDATE_ENABLE),
			        app->update_choice == (i == 0) ? &th->selection
			                                       : &th->base,
			        f->cols - 2);
		}
	}
	if (f->rows > 10) {
		wf_app_put_fit(f, 1, f->rows - 2, wf_msg(MSG_UPDATE_HINT),
		        &th->dim, f->cols - 2);
	}
}
static WfAction consent(WfApp *app, const WfEvent *ev)
{
	if (ev->type != WF_EV_KEY) {
		return WF_ACT_NONE;
	}
	if (ev->key == WF_KEY_UP || ev->key == WF_KEY_DOWN) {
		if (app->cols < 60 || app->rows < 11) {
			return WF_ACT_NONE;
		}
		app->update_choice = !app->update_choice;
		return WF_ACT_REDRAW;
	}
	if (ev->key != WF_KEY_ENTER && ev->key != WF_KEY_ESC) {
		return WF_ACT_NONE;
	}
	WfConfig *draft = app->config ? malloc(sizeof *draft) : NULL;
	if (draft) {
		*draft = *app->config;
		draft->update_consent = 1;
		draft->update_checks = ev->key != WF_KEY_ESC &&
		        app->update_choice && app->cols >= 60 &&
		        app->rows >= 11;
		char why[96];
		if (!wf_app_settings_commit(
		            app, draft, false, why, sizeof why)) {
			char text[256];
			(void)wf_msgf(text, sizeof text, MSG_SETTINGS_NOT_SAVED,
			        &WF_ARG_S(why), 1);
			wf_app_set_msg(app, text);
		}
		free(draft);
	} else {
		wf_app_set_msg(app, wf_msg(MSG_UPDATE_SAVE_FAILED));
	}
	app->mode = &WF_MODE_NORMAL;
	app->flush_input = true;
	app->enter_guard_until = app->now_ms + WF_UPDATE_ENTER_GUARD_MS;
	return WF_ACT_REDRAW;
}
const WfMode WF_MODE_UPDATE_CONSENT = {"UPDATE_CONSENT", draw, consent};

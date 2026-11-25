/* The settings screen: putting changes in force and writing them. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../edit/wf_ekeymap.h"
#include "../text/wf_text.h"
#include "wf_app_settings_priv.h"
#include "../base/wf_macro.h"

#define MAX_CHANGES 512

static void ensure_change(WfConfigChange *ch,
        size_t *n,
        size_t cap,
        const char *section,
        const char *key,
        const char *value)
{
	size_t i;

	for (i = 0; i < *n; i++) {
		if (!strcmp(ch[i].section, section) &&
		        !strcmp(ch[i].key, key)) {
			return;
		}
	}
	if (*n == cap) {
		return;
	}
	memset(&ch[*n], 0, sizeof ch[*n]);
	ch[*n].section = section;
	ch[*n].key = key;
	snprintf(ch[*n].value, sizeof ch[*n].value, "%s", value);
	(*n)++;
}

void wf_app_dirpreview_enable(WfApp *app, bool on);

void wf_set_apply_live_changes(WfApp *app, const WfConfig *before)
{
	/* the lists follow the new settings */
	if (before->show_hidden != app->config->show_hidden) {
		wf_app_set_hidden(app, 0 != app->config->show_hidden);
	}
	if (before->dir_preview != app->config->dir_preview) {
		wf_app_dirpreview_enable(app, 0 != app->config->dir_preview);
	}
	{
		WfThemePalette p;

		wf_set_palette_of(app->config, &p);
		wf_theme_activate(&p);
		wf_set_apply_language(app, app->config);
	}
	/* the keys of the saved profile are in force from now on */
	if (app->keys_stale ||
	        !wf_keyconfig_equal(&before->keys, &app->config->keys) ||
	        before->editor_keymap != app->config->editor_keymap) {
		WfStatus ks = wf_app_set_keys(app, &app->config->keys,
		        1 == app->config->editor_keymap ? WF_KM_NANO
		                                        : WF_KM_PIERO);

		app->keys_stale = WF_OK != ks;
		if (WF_OK != ks) {
			/* saved, but the keys in use are the old ones: say so; the
			 * next save tries again */
			wf_app_set_msg(app, wf_msg(MSG_KEYS_NOT_APPLIED));
		} else {
			wf_app_keys_warn(app);
		}
	}
	if (app->settings_port.apply) {
		app->settings_port.apply(
		        app->settings_port.ctx, app->config, true);
	}
	if (before->sort_by != app->config->sort_by ||
	        before->sort_descending != app->config->sort_descending ||
	        before->directories_first != app->config->directories_first) {
		wf_app_reload(app);
	}
}

/* Writes the draft's differences before applying them to live consumers.
 * Failure leaves the live configuration unchanged; disk publication may have
 * happened with an unverified baseline, which the save port must report. */
bool wf_app_settings_commit(WfApp *app,
        const WfConfig *draft,
        bool force_theme,
        char *why,
        size_t cap)
{
	WfConfigChange *ch;
	WfConfig *before;
	size_t n;
	WfStatus st;
	char *start;
	const char *theme_text;
	bool ok = false;

	if (!app->config) {
		snprintf(why, cap, "%s", wf_msg(MSG_SETTINGS_WHY_NO_CONFIG));
		return false;
	}
	if (app->config->schema_version > WF_CONFIG_SCHEMA_VERSION ||
	        draft->schema_version > WF_CONFIG_SCHEMA_VERSION) {
		snprintf(why, cap, "%s", wf_msg(MSG_SETTINGS_WHY_NEWER_SCHEMA));
		return false;
	}
	if (!app->settings_port.save) {
		snprintf(why, cap, "%s", wf_msg(MSG_SETTINGS_WHY_NO_SAVE));
		return false;
	}
	ch = malloc(MAX_CHANGES * sizeof *ch);
	before = malloc(sizeof *before);
	if (!ch || !before) {
		free(ch);
		free(before);
		snprintf(why, cap, "%s", wf_status_name(WF_E_NOMEM));
		return false;
	}
	n = wf_config_diff(app->config, draft, ch, MAX_CHANGES);
	if (force_theme &&
	        NULL !=
	                (theme_text = wf_theme_id_text(
	                         (WfThemeId)draft->theme))) {
		ensure_change(
		        ch, &n, MAX_CHANGES, "appearance", "theme", theme_text);
	}
	why[0] = '\0';
	st = app->settings_port.save(app->settings_port.ctx, ch, n, why, cap);
	if (WF_OK != st) {
		if (!why[0]) {
			(void)wf_msgf(why, cap, MSG_SETTINGS_WHY_WRITE,
			        &WF_ARG_S(wf_status_name(st)), 1);
		}
		goto done;
	}
	*before = *app->config;
	start = app->config->start_directory;
	*app->config = *draft;
	app->config->start_directory = start;
	wf_set_apply_live_changes(app, before);
	ok = true;
done:
	free(ch);
	free(before);
	return ok;
}

/* The first-run screen is finished with `id`: record that setup was done and
 * which theme was chosen. A failed write is reported, not fatal: the theme
 * still applies for this session. */
void wf_app_onboarding_done(WfApp *app, WfThemeId id)
{
	WfConfig *draft;
	char why[96];

	if (!app->config) {
		wf_theme_activate(&wf_theme_preset(id)->palette);
		return;
	}
	draft = malloc(sizeof *draft);
	if (!draft) {
		wf_theme_activate(&wf_theme_preset(id)->palette);
		return;
	}
	*draft = *app->config;
	draft->start_directory = NULL;
	draft->theme = (int)id;
	draft->lang_setting = app->picker.lang;
	if (app->picker.keys >= 0) {
		draft->keys.profile = 1 == app->picker.keys ? WF_PROFILE_VIM
		                                            : WF_PROFILE_NANO;
	}
	if (draft->schema_version <= WF_CONFIG_SCHEMA_VERSION) {
		draft->schema_version = WF_CONFIG_SCHEMA_VERSION;
	}
	draft->onboarding_completed = 1;
	if (!wf_app_settings_commit(app, draft, true, why, sizeof why)) {
		char text[96];
		WfThemePalette p;
		char *start = app->config->start_directory;

		*app->config = *draft;
		app->config->start_directory = start;
		wf_set_palette_of(app->config, &p);
		wf_theme_activate(&p);
		wf_set_apply_language(app, app->config);
		(void)wf_msgf(text, sizeof text,
		        MSG_SETTINGS_ONBOARDING_NOT_SAVED, &WF_ARG_S(why), 1);
		wf_app_set_msg(app, text);
	}
	free(draft);
}

bool wf_set_save_draft(WfApp *app)
{
	WfConfig *d = &app->settings.draft;
	char why[96], text[112];

	if (d->schema_version <= WF_CONFIG_SCHEMA_VERSION) {
		d->schema_version = WF_CONFIG_SCHEMA_VERSION;
	}
	d->onboarding_completed = 1;
	if (!wf_set_is_dirty(app)) {
		if (app->keys_stale) {
			/* saved earlier, never put in force: try again now */
			bool ok = WF_OK ==
			        wf_app_set_keys(app, &app->config->keys,
			                1 == app->config->editor_keymap
			                        ? WF_KM_NANO
			                        : WF_KM_PIERO);

			app->keys_stale = !ok;
			wf_set_note_msg(app, !ok,
			        ok ? MSG_KEYS_APPLIED : MSG_KEYS_NOT_APPLIED);
			return true;
		}
		wf_set_note_msg(app, false, MSG_SETTINGS_NOTHING_TO_SAVE);
		return true;
	}
	if (wf_app_settings_commit(app, d, false, why, sizeof why)) {
		if (app->keys_stale) { /* the file is saved; the keys are not live */
			wf_set_note_msg(app, true, MSG_KEYS_NOT_APPLIED);
		} else {
			wf_set_note_msg(app, false, MSG_SETTINGS_SAVED);
		}
		return true;
	}
	(void)wf_msgf(
	        text, sizeof text, MSG_SETTINGS_NOT_SAVED, &WF_ARG_S(why), 1);
	wf_set_note(app, true, text);
	return false;
}

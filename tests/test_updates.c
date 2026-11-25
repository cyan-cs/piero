#include "update/wf_update.h"
#include "config/wf_config.h"
#include "ui/wf_app_settings_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"
#include <stdlib.h>
#include <string.h>

static WfUpdateState response(const char *current, const char *tag)
{
	char json[256];
	int n = snprintf(json, sizeof json,
	        "{\"tag_name\":\"%s\",\"draft\":false,\"prerelease\":false}",
	        tag);
	return wf_update_response(current, 200, json, (size_t)n).state;
}
static unsigned saves, requests;
static bool fail_save;
static WfStatus
save(void *ctx, const WfConfigChange *changes, size_t n, char *why, size_t cap)
{
	(void)ctx;
	(void)changes;
	(void)n;
	(void)why;
	(void)cap;
	saves++;
	return fail_save ? WF_E_ACCESS : WF_OK;
}
static WfStatus request(void *ctx, bool manual)
{
	(void)ctx;
	CHECK(manual);
	requests++;
	return WF_OK;
}
static void key(WfApp *app, WfKey keycode)
{
	WfEvent e = {WF_EV_KEY, 0, keycode, 0};
	app->mode->on_event(app, &e);
}
static void ui(void)
{
	static WfApp app;
	FakePort fs = {0};
	WfConfig config;
	wf_config_init(&config);
	fs.app = &app;
	CHECK(wf_app_init(&app, 80, 24, fake_port(&fs), L"C:\\") == WF_OK);
	wf_app_set_settings(
	        &app, &config, (WfSettingsPort){NULL, save, NULL, NULL});
	app.update_request = request;
	wf_app_update_consent_begin(&app);
	CHECK(!config.update_checks && !config.update_consent &&
	        requests == 0 && !app.update_choice);
	key(&app, WF_KEY_ENTER);
	CHECK(config.update_consent && !config.update_checks && saves == 1 &&
	        requests == 0);
	config.update_consent = 0;
	wf_app_update_consent_begin(&app);
	key(&app, WF_KEY_UP);
	key(&app, WF_KEY_ENTER);
	CHECK(config.update_consent && config.update_checks && requests == 0);
	config.update_checks = 0;
	config.update_consent = 0;
	fail_save = true;
	wf_app_update_consent_begin(&app);
	key(&app, WF_KEY_UP);
	key(&app, WF_KEY_ENTER);
	CHECK(!config.update_consent && !config.update_checks &&
	        requests == 0 && app.update_prompted);
	fail_save = false;
	wf_app_settings_begin(&app);
	app.settings.category = WF_CAT_UPDATES;
	app.settings.item = 1;
	WfEvent enter = {WF_EV_KEY, 0, WF_KEY_ENTER, 0};
	WF_SETPAGE_UPDATES.event(&app, &enter);
	CHECK(requests == 1 &&
	        !config.update_checks); /* explicit manual check while OFF */
	app.settings.item = 0;
	WF_SETPAGE_UPDATES.event(&app, &enter);
	CHECK(app.settings.draft.update_checks &&
	        app.settings.draft.update_consent && !config.update_checks);
	char why[96];
	CHECK(wf_app_settings_commit(
	        &app, &app.settings.draft, false, why, sizeof why));
	CHECK(config.update_checks && config.update_consent);
	for (int lang = 0; lang < 2; lang++) {
		wf_i18n_set_language(lang ? WF_UILANG_JA : WF_UILANG_EN);
		wf_app_update_consent_begin(&app);
		for (int width = 1; width <= 80; width += 13) {
			WfFrame *f = wf_frame_new(width, 24, &WF_STYLE_BASE);
			CHECK(f != NULL);
			if (f) {
				app.mode->draw(&app, f);
				wf_frame_free(f);
			}
		}
	}
	wf_i18n_set_language(WF_UILANG_EN);
	wf_app_destroy(&app);
	wf_config_dispose(&config);
}
int main(void)
{
	CHECK(response("0.0.3-dev", "v0.0.3") == WF_UPDATE_AVAILABLE);
	CHECK(response("1.2.3", "v1.2.4") == WF_UPDATE_AVAILABLE);
	CHECK(response("2.0.0", "v1.9.9") == WF_UPDATE_CURRENT);
	CHECK(response("1.2.3+build", "v1.2.3") == WF_UPDATE_CURRENT);
	const char *invalid[] = {"", "1", "1.2", "01.2.3", "1.2.3-dev",
	        "https://other.invalid/a", "4294967296.0.0", "1.2.3\n"};
	for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
		CHECK(response("1.0.0", invalid[i]) == WF_UPDATE_INVALID);
	}
	const char *bad[] = {"{}", "[]",
	        "{\"tag_name\":\"1.2.3\",\"draft\":true,\"prerelease\":false}",
	        "{\"tag_name\":\"1.2.3\",\"draft\":false,\"prerelease\":true}",
	        "{\"tag_name\":\"1.2.3\",\"tag_name\":\"9.9.9\",\"draft\":"
	        "false,\"prerelease\":false}",
	        "{\"tag_name\":\"1.2.3\",\"draft\":false,\"prerelease\":false} "
	        "garbage"};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		CHECK(wf_update_response("1.0.0", 200, bad[i], strlen(bad[i]))
		                .state == WF_UPDATE_INVALID);
	}
	CHECK(wf_update_response("1.0.0", 404, NULL, 0).state ==
	        WF_UPDATE_NO_RELEASE);
	CHECK(wf_update_response("1.0.0", 429, NULL, 0).state ==
	        WF_UPDATE_LIMITED);
	CHECK(wf_update_response("1.0.0", 500, NULL, 0).state ==
	        WF_UPDATE_UNAVAILABLE);
	CHECK(wf_update_response("1.0.0", 200, "{}", WF_UPDATE_BODY_MAX + 1)
	                .state == WF_UPDATE_INVALID);
	for (int enabled = 0; enabled < 2; enabled++) {
		for (int consent = 0; consent < 2; consent++) {
			for (int valid = 0; valid < 2; valid++) {
				CHECK(wf_update_allowed(enabled != 0,
				              consent != 0, valid != 0,
				              false) ==
				        (enabled && consent && valid));
				CHECK(wf_update_allowed(enabled != 0,
				        consent != 0, valid != 0, true));
			}
		}
	}
	CHECK(!wf_update_due(true, true, true, 90000, 89999));
	CHECK(wf_update_due(true, true, true, 90000, 1));
	CHECK(!wf_update_due(true, true, true, 1, 90000));
	WfConfig c;
	wf_config_init(&c);
	WfConfigReport report;
	const char *good = "[updates]\nenabled=true\nconsent_recorded=true\n";
	CHECK(wf_config_parse(&c, good, strlen(good), NULL, NULL, &report) ==
	                WF_OK &&
	        c.update_checks && c.update_consent);
	const char *broken = "[updates]\nenabled=true\nconsent_recorded="
	                     "true\nenabled=nonsense\n";
	CHECK(wf_config_parse(&c, broken, strlen(broken), NULL, NULL,
	              &report) == WF_OK &&
	        !c.update_checks && c.updates_blocked);
	char *round = NULL;
	size_t size = 0;
	WfIniSet sets[] = {{"updates", "enabled", "true"},
	        {"updates", "consent_recorded", "true"}};
	CHECK(wf_config_apply("", 0, sets, 2, &round, &size) == WF_OK);
	if (round) {
		CHECK(wf_config_parse(&c, round, size, NULL, NULL, NULL) ==
		                WF_OK &&
		        c.update_checks && c.update_consent);
		free(round);
	}
	wf_config_dispose(&c);
	ui();
	return TEST_RESULT();
}

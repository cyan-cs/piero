/* Real bounded INI loading with ownership-proven disposable data. No user
 * settings, legacy receipt or installed translation is modified. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf_test.h"
#include "wf_fake_fs.h"
#include "ui/wf_app_priv.h"
#include "ui/wf_app_settings_priv.h"
#include "config/wf_config_save_win.h"
#include "../tools/p0/p0_lab.h"

static P0Lab lab;

static void path_of(const wchar_t *name, wchar_t path[1100])
{
	CHECK(p0_lab_path(&lab, name, path, 1100));
}

static void write_bytes(const wchar_t *name, const void *bytes, size_t size)
{
	wchar_t path[1100];
	path_of(name, path);
	if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
		CHECK(p0_area_remove(lab.area, name));
	}
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(file != INVALID_HANDLE_VALUE);
	if (file == INVALID_HANDLE_VALUE) {
		return;
	}
	DWORD put = 0;
	CHECK(size <= MAXDWORD &&
	        WriteFile(file, bytes, (DWORD)size, &put, NULL) && put == size);
	CloseHandle(file);
	CHECK(p0_area_track(lab.area, name, 0));
}

static void write_text(const wchar_t *name, const char *text)
{
	write_bytes(name, text, strlen(text));
}

static void french(const char *messages)
{
	WfBuf text = {0};
	CHECK(wf_buf_puts(&text,
	              "[language]\r\ncode=fr\r\nname=Identical\r\n[messages]"
	              "\r\n") == WF_OK);
	CHECK(wf_buf_puts(&text, messages) == WF_OK);
	write_bytes(L"user\\fr.ini", text.data, text.len);
	wf_buf_free(&text);
}

static void key(WfApp *app, WfKey code)
{
	WfEvent ev = {.type = WF_EV_KEY, .key = code};
	(void)app->mode->on_event(app, &ev);
}

static WfStatus save_settings(void *ctx,
        const WfConfigChange *changes,
        size_t n,
        char *why,
        size_t cap)
{
	WfIniSet sets[64];
	WfConfigSaveResult result = {0};
	(void)why;
	(void)cap;
	if (n > 64) {
		return WF_E_INVAL;
	}
	for (size_t i = 0; i < n; i++) {
		sets[i] = (WfIniSet){changes[i].section, changes[i].key,
		        changes[i].remove ? NULL : changes[i].value};
	}
	WfStatus status = wf_config_file_save(ctx, sets, n, &result);
	return status == WF_OK && result.verdict == WF_CS_OK ? WF_OK : WF_E_OS;
}

static void ui_selection_is_persisted(WfLangSetting setting)
{
	static WfApp app;
	FakePort fs = {.app = &app};
	WfConfig config, reloaded;
	wchar_t path[1100];
	WfConfigFile file;
	wf_config_init(&config);
	path_of(L"config.ini", path);
	CHECK(wf_config_file_open(&file, path) == WF_OK);
	CHECK(wf_app_init(&app, 120, 30, fake_port(&fs), L"C:\\Pics") == WF_OK);
	wf_app_set_settings(&app, &config,
	        (WfSettingsPort){.ctx = &file, .save = save_settings});
	wf_app_settings_begin(&app);
	key(&app, WF_KEY_ENTER); /* language, first General item */
	key(&app, WF_KEY_ENTER); /* choice list */
	key(&app, WF_KEY_HOME);
	for (int i = 0; i < setting; i++) {
		key(&app, WF_KEY_DOWN);
	}
	key(&app, WF_KEY_ENTER);
	CHECK(app.settings.draft.lang_setting == setting);
	WfEvent save = {.type = WF_EV_CTRL, .ch = 's'};
	(void)app.mode->on_event(&app, &save);
	CHECK(config.lang_setting == setting);
	CHECK(p0_area_track(lab.area, L"config.ini", 0));
	wf_config_file_close(&file);
	HANDLE input = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	char bytes[4096] = {0};
	DWORD got = 0;
	CHECK(input != INVALID_HANDLE_VALUE &&
	        ReadFile(input, bytes, sizeof bytes - 1, &got, NULL));
	if (input != INVALID_HANDLE_VALUE) {
		CloseHandle(input);
	}
	wf_config_init(&reloaded);
	CHECK(wf_config_parse(&reloaded, bytes, got, NULL, NULL, NULL) ==
	        WF_OK);
	CHECK(reloaded.lang_setting == setting);
	CHECK(!strcmp(wf_i18n_setting_id(reloaded.lang_setting), "fr"));
	key(&app, WF_KEY_ESC);
	app.recovery.unknown = true;
	CHECK(wf_app_copy_start(&app, false));
	CHECK(app.recovery.unknown && app.copy_jobs == 0 && !app.copy.submit);
	/* Untrusted labels never change the actual recovery gate. */
	const int widths[] = {32, 64, 120};
	for (size_t i = 0; i < 3; i++) {
		app.cols = widths[i];
		app.rows = 12;
		WfFrame *frame =
		        wf_frame_new(app.cols, app.rows, &WF_STYLE_BASE);
		wf_app_draw(&app, frame);
		wf_app_settings_begin(&app);
		key(&app, WF_KEY_ENTER);
		key(&app, WF_KEY_ENTER);
		key(&app, WF_KEY_END);
		key(&app, WF_KEY_ENTER);
		wf_app_draw(&app, frame);
		key(&app, WF_KEY_ESC);
		if (app.mode != &WF_MODE_NORMAL) {
			WfEvent discard = {.type = WF_EV_CHAR, .ch = 'd'};
			(void)app.mode->on_event(&app, &discard);
		}
		wf_frame_free(frame);
	}
	wf_app_destroy(&app);
	wf_config_dispose(&config);
	wf_config_dispose(&reloaded);
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--discover-executable")) {
		WfLangSetting fr;
		CHECK(wf_i18n_parse_setting("fr", &fr));
		if (wf_i18n_parse_setting("fr", &fr)) {
			wf_i18n_set_language(fr - 1);
			CHECK(!strcmp(
			        wf_msg(MSG_SETTINGS_TITLE), "User relocation"));
		}
		return TEST_RESULT();
	}
	CHECK(p0_lab_open(&lab, "language", 0));
	if (!lab.area) {
		return TEST_RESULT();
	}
	CHECK(p0_area_mkdir(lab.area, L"bundle"));
	CHECK(p0_area_mkdir(lab.area, L"user"));
	wchar_t bundle[1100], user[1100];
	path_of(L"bundle", bundle);
	path_of(L"user", user);
	if (argc > 1 && !strcmp(argv[1], "--missing")) {
		wf_i18n_initialize(bundle, user);
		WfConfig config;
		wf_config_init(&config);
		const char text[] = "[general]\nlanguage=fr\n";
		CHECK(wf_config_parse(&config, text, sizeof text - 1, NULL,
		              NULL, NULL) == WF_OK);
		CHECK(!strcmp(wf_i18n_setting_id(config.lang_setting), "fr"));
		CHECK(!wf_i18n_lang_available(config.lang_setting - 1));
		CHECK(!strcmp(
		        wf_msg_in(config.lang_setting - 1, MSG_SETTINGS_TITLE),
		        "Settings"));
		WfConfig english = config;
		english.start_directory = NULL;
		english.lang_setting = WF_LANGSET_EN;
		WfConfigChange changes[8];
		size_t n = wf_config_diff(&config, &english, changes, 8);
		CHECK(n == 1 && !strcmp(changes[0].key, "language") &&
		        !strcmp(changes[0].value, "en"));
		wf_config_dispose(&english);
		wf_config_dispose(&config);
	} else {
		write_text(L"bundle\\en.ini",
		        "[language]\ncode=en\nname=English\n[messages]\nMSG_"
		        "SETTINGS_TITLE=Bundled\n");
		write_text(L"user\\en.ini",
		        "; custom user "
		        "English\n[language]\ncode=en\nname=English\n[messages]"
		        "\nMSG_SETTINGS_TITLE=Override\nfuture.key=ignored\n");
		french("MSG_SETTINGS_TITLE=Bonjour\r\nMSG_SAFETY_REVIEW=Tout "
		       "va bien\r\n");
		wf_i18n_initialize(bundle, user);
		CHECK(WF_UILANG_COUNT == 3);
		write_text(L"bundle\\ja.ini",
		        "[language]\ncode=ja\nname=日本語\nplural=none\n["
		        "messages]\nMSG_SETTINGS_TITLE=標準日本語\nMSG_SAFETY_"
		        "REVIEW=確認しよう\n");
		write_text(L"user\\ja.ini",
		        "[language]\ncode=ja\nname=日本語\nplural=none\n["
		        "messages]\nMSG_SETTINGS_TITLE=ユーザー日本語\n");
		wf_i18n_refresh();
		CHECK(!strcmp(wf_msg_in(WF_UILANG_JA, MSG_SETTINGS_TITLE),
		        "ユーザー日本語"));
		CHECK(!strcmp(wf_msg_in(WF_UILANG_JA, MSG_SAFETY_REVIEW),
		        "確認しよう"));
		CHECK(!strcmp(wf_msg(MSG_SETTINGS_TITLE), "Override"));
		const char *old = wf_msg(MSG_SETTINGS_TITLE);
		write_text(L"user\\en.ini",
		        "[language]\ncode=en\nname=English\n[messages]\nMSG_"
		        "SETTINGS_TITLE=New user text\n");
		wf_i18n_set_language(WF_UILANG_EN);
		CHECK(!strcmp(wf_msg(MSG_SETTINGS_TITLE), "New user text") &&
		        !strcmp(old, "Override"));
		CHECK(p0_area_remove(lab.area, L"user\\en.ini"));
		wf_i18n_set_language(WF_UILANG_EN);
		CHECK(!strcmp(wf_msg(MSG_SETTINGS_TITLE), "Bundled"));
		WfLangSetting fr;
		CHECK(wf_i18n_parse_setting("fr", &fr));
		ui_selection_is_persisted(fr);
		wf_i18n_set_language(fr - 1);
		CHECK(!strcmp(wf_msg(MSG_SETTINGS_TITLE), "Bonjour"));
		CHECK(!strcmp(wf_msg(MSG_SAFETY_REVIEW), "Tout va bien"));
		CHECK(!strcmp(wf_msg(MSG_COPY_SAFETY_BLOCKED),
		        wf_msg_in(WF_UILANG_EN, MSG_COPY_SAFETY_BLOCKED)));
		write_text(L"user\\de.ini",
		        "[language]\ncode=de\nname=Identical\n[messages]\nMSG_"
		        "SETTINGS_TITLE=Deutsch\n");
		wf_i18n_refresh();
		WfLangSetting de;
		CHECK(wf_i18n_parse_setting("de", &de));
		char label_fr[192], label_de[192];
		static WfApp labels;
		wf_app_language_text(&labels, fr, label_fr, sizeof label_fr);
		wf_app_language_text(&labels, de, label_de, sizeof label_de);
		CHECK(strstr(label_fr, "fr") && strstr(label_de, "de") &&
		        strcmp(label_fr, label_de));
		size_t memory = wf_i18n_storage_used();
		for (unsigned i = 0; i < 100; i++) {
			wf_i18n_set_language(fr - 1);
			wf_i18n_set_language(WF_UILANG_EN);
		}
		CHECK(memory == wf_i18n_storage_used());
		const char *bad[] = {
		        "MSG_SETTINGS_TITLE=A\nMSG_SETTINGS_TITLE=B\n",
		        "MSG_SETTINGS_TITLE={0}\n", "MSG_SETTINGS_TITLE=%n\n",
		        "MSG_SETTINGS_TITLE=Good\nMSG_LANG_NAMED={0}{0}{1}\n",
		        "MSG_SETTINGS_TITLE=\\q\n",
		        "MSG_SETTINGS_TITLE=\x1b[31m\n",
		        "MSG_SETTINGS_TITLE=\xc0\xaf\n",
		        "MSG_SETTINGS_TITLE=\xc2\x9b\n", "broken line\n",
		        "future=A\nfuture=B\n",
		        ("MSG_SETTINGS_TITLE=Good\n[arguments]\nMSG_SETTINGS_"
		         "TITLE=2\n")};
		for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
			french(bad[i]);
			wf_i18n_set_language(fr - 1);
			CHECK(!strcmp(wf_msg(MSG_SETTINGS_TITLE), "Settings"));
		}
		french("MSG_SETTINGS_TITLE=One\\nTwo = trois # literal ; "
		       "comment?\n");
		wf_i18n_set_language(fr - 1);
		CHECK(!strcmp(wf_msg(MSG_SETTINGS_TITLE),
		        "One\nTwo = trois # literal ; comment?"));
		french("MSG_SETTINGS_TITLE=Shared snapshot\n");
		wchar_t fr_path[1100];
		path_of(L"user\\fr.ini", fr_path);
		HANDLE writer = CreateFileW(fr_path, GENERIC_WRITE,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        0, NULL);
		CHECK(writer != INVALID_HANDLE_VALUE);
		wf_i18n_set_language(fr - 1);
		CHECK(!strcmp(wf_msg(MSG_SETTINGS_TITLE), "Settings"));
		if (writer != INVALID_HANDLE_VALUE) {
			CloseHandle(writer);
		}
		wf_i18n_set_language(fr - 1);
		CHECK(!strcmp(wf_msg(MSG_SETTINGS_TITLE), "Shared snapshot"));
		char *large = malloc((1u << 20) + 1);
		CHECK(large != NULL);
		if (large) {
			memset(large, 'x', (1u << 20) + 1);
			write_bytes(L"user\\fr.ini", large, (1u << 20) + 1);
			wf_i18n_set_language(fr - 1);
			CHECK(!strcmp(wf_msg(MSG_SETTINGS_TITLE), "Settings"));
			size_t prefix = sizeof "MSG_SETTINGS_TITLE=" - 1;
			memcpy(large, "MSG_SETTINGS_TITLE=", prefix);
			large[prefix + 4097] = 0;
			french(large);
			wf_i18n_set_language(fr - 1);
			CHECK(!strcmp(wf_msg(MSG_SETTINGS_TITLE), "Settings"));
			free(large);
		}
		const char nul[] = "[language]\ncode=fr\nname=F\n[messages]"
		                   "\nMSG_SETTINGS_TITLE=bad\0hidden";
		write_bytes(L"user\\fr.ini", nul, sizeof nul - 1);
		wf_i18n_set_language(fr - 1);
		CHECK(!strcmp(wf_msg(MSG_SETTINGS_TITLE), "Settings"));
		CHECK(p0_area_remove(lab.area, L"user\\fr.ini"));
		wf_i18n_set_language(fr - 1);
		CHECK(!strcmp(wf_msg(MSG_SETTINGS_TITLE), "Settings"));
		for (unsigned i = 0; i < 40; i++) {
			char data[192];
			wchar_t name[64];
			(void)snprintf(data, sizeof data,
			        "[language]\ncode=l%u\nname=Test\n[messages]\n",
			        i);
			(void)swprintf(name, 64, L"user\\l%u.ini", i);
			write_text(name, data);
		}
		wf_i18n_refresh();
		CHECK(WF_UILANG_COUNT == WF_I18N_LANG_LIMIT);
	}
	CHECK(p0_lab_close(&lab, wf_test_failures != 0) == 0);
	return TEST_RESULT();
}

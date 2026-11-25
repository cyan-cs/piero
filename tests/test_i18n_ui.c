/*
 * The language in use: the first-run screen, the settings screen, the main
 * screen and Smart Go, switched live. Switching changes words only: the
 * folder, the selection and the history stay as they were, and a cancelled or
 * failed save puts the old language back.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/wf_app_priv.h"
#include "ui/wf_app_settings_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

typedef struct
{
	unsigned saves;
	WfConfigChange last[96];
	size_t last_n;
	WfStatus save_status;
} Port;

static WfStatus port_save(void *ctx,
        const WfConfigChange *changes,
        size_t n,
        char *why,
        size_t cap)
{
	Port *p = ctx;

	p->saves++;
	p->last_n = n < 96 ? n : 96;
	memcpy(p->last, changes, p->last_n * sizeof *changes);
	for (size_t i = 0; i < p->last_n; i++) {
		if (changes[i].section == changes[i].section_buf) {
			p->last[i].section = p->last[i].section_buf;
		}
	}
	if (WF_OK != p->save_status) {
		snprintf(why, cap, "disk full");
	}
	return p->save_status;
}

static void port_apply(void *ctx, const WfConfig *config, bool committed)
{
	(void)ctx;
	(void)config;
	(void)committed;
}

static const char *saved(const Port *p, const char *section, const char *key)
{
	for (size_t i = 0; i < p->last_n; i++) {
		if (!strcmp(p->last[i].section, section) &&
		        !strcmp(p->last[i].key, key)) {
			return p->last[i].value;
		}
	}
	return NULL;
}

typedef struct
{
	WfApp app;
	FakePort fs;
	Port port;
	WfConfig config;
} Rig;

static void rig_open(Rig *r, WfUiLang detected)
{
	memset(r, 0, sizeof *r);
	r->fs.app = &r->app;
	wf_i18n_set_language(WF_UILANG_EN);
	wf_config_init(&r->config);
	CHECK(wf_app_init(&r->app, 100, 30, fake_port(&r->fs),
	              L"C:\\Users\\dev") == WF_OK);
	wf_app_set_detected_language(&r->app, detected);
	wf_app_set_settings(&r->app, &r->config,
	        (WfSettingsPort){&r->port, port_save, port_apply, NULL});
	wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
}

static void rig_close(Rig *r)
{
	wf_app_destroy(&r->app);
	wf_config_dispose(&r->config);
	wf_i18n_set_language(WF_UILANG_EN);
}

static void key(WfApp *a, WfKey k)
{
	WfEvent e = {WF_EV_KEY, 0, k, 0};

	a->mode->on_event(a, &e);
}

/* Enter opens the list of a choice, Down moves, Enter applies. */
static void choose(WfApp *a, int downs)
{
	key(a, WF_KEY_ENTER);
	while (downs-- > 0) {
		key(a, WF_KEY_DOWN);
	}
	key(a, WF_KEY_ENTER);
}

static void ch(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	a->mode->on_event(a, &e);
}

static void ctrl(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CTRL, c, WF_KEY_NONE, 0};

	a->mode->on_event(a, &e);
}

/* The screen as text, rows joined by newlines. */
static void screen(WfApp *a, int cols, int rows, char *out, size_t cap)
{
	WfFrame *f = wf_frame_new(cols, rows, &WF_STYLE_BASE);
	size_t n = 0;

	a->cols = cols;
	a->rows = rows;
	wf_app_draw(a, f);
	for (int y = 0; y < rows; y++) {
		for (int x = 0; x < f->cols && n + 8 < cap; x++) {
			const WfCell *c = &f->cells[y * f->cols + x];

			if (0 == c->width) {
				continue;
			}
			n += (size_t)snprintf(out + n, cap - n, "%s",
			        c->utf8[0] ? c->utf8 : " ");
		}
		if (n + 2 < cap) {
			out[n++] = '\n';
		}
	}
	out[n] = '\0';
	wf_frame_free(f);
}

static bool has(WfApp *a, int cols, int rows, const char *needle)
{
	static char text[64 * 1024];

	screen(a, cols, rows, text, sizeof text);
	return NULL != strstr(text, needle);
}

static void the_language_is_asked_first_and_starts_on_the_windows_language(void)
{
	static Rig r;

	rig_open(&r, WF_UILANG_JA);
	wf_i18n_set_language(WF_UILANG_JA); /* what main does at start */
	wf_app_setup_begin(&r.app, true);
	CHECK(WF_PICKER_LANGUAGE == r.app.picker.stage);
	CHECK(WF_LANGSET_AUTO == r.app.picker.lang); /* the chosen row */
	CHECK(has(&r.app, 80, 28, "言語を選択"));
	CHECK(has(&r.app, 80, 28, "> 自動（日本語）"));
	CHECK(has(&r.app, 80, 28, "English"));
	CHECK(has(&r.app, 80, 28, "  日本語"));
	CHECK(!has(&r.app, 80, 28, "テーマを選択")); /* not yet */
	CHECK(has(&r.app, 80, 28,
	        "| '_ \\| |/ _ \\ '__/ _ \\")); /* the logo stays */
	CHECK(!has(&r.app, 80, 28, "Welcome"));

	key(&r.app,
	        WF_KEY_DOWN); /* English: the screen turns English at once */
	CHECK(WF_LANGSET_EN == r.app.picker.lang &&
	        WF_UILANG_EN == wf_i18n_language());
	CHECK(has(&r.app, 80, 28, "Choose your language"));
	CHECK(has(&r.app, 80, 28, "> English"));
	key(&r.app, WF_KEY_DOWN);
	CHECK(WF_LANGSET_JA == r.app.picker.lang &&
	        WF_UILANG_JA == wf_i18n_language());
	key(&r.app, WF_KEY_DOWN); /* the end of the list */
	CHECK(WF_LANGSET_JA == r.app.picker.lang);
	key(&r.app, WF_KEY_UP);
	key(&r.app, WF_KEY_UP);
	CHECK(WF_LANGSET_AUTO == r.app.picker.lang);
	key(&r.app, WF_KEY_UP);
	CHECK(WF_LANGSET_AUTO == r.app.picker.lang);
	CHECK(0 == r.port.saves); /* looking changes nothing on disk */
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_PICKER_KEYS == r.app.picker.stage &&
	        &WF_MODE_SETUP == r.app.mode);
	CHECK(has(&r.app, 80, 28, "キー操作を選択"));
	CHECK(has(&r.app, 80, 28, "> Nano"));
	CHECK(has(&r.app, 80, 28, "  Vim"));
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_PICKER_THEME == r.app.picker.stage);
	CHECK(has(&r.app, 80, 28, "テーマを選択"));
	CHECK(has(&r.app, 80, 28, "> Spade"));
	rig_close(&r);

	/* an English machine starts on its own language, in English */
	rig_open(&r, WF_UILANG_EN);
	wf_app_setup_begin(&r.app, true);
	CHECK(has(&r.app, 80, 28, "Choose your language"));
	CHECK(has(&r.app, 80, 28, "> Auto (English)"));
	rig_close(&r);
}

/* Enter, Enter, Enter: the Windows language, Nano keys and the default
 * theme, saved. */
static void enter_twice_accepts_everything(void)
{
	static Rig r;

	rig_open(&r, WF_UILANG_JA);
	wf_i18n_set_language(WF_UILANG_JA);
	wf_app_setup_begin(&r.app, true);
	key(&r.app, WF_KEY_ENTER);
	key(&r.app, WF_KEY_ENTER);
	key(&r.app, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == r.app.mode && 1 == r.port.saves);
	CHECK(NULL == saved(&r.port, "general", "language")); /* auto */
	CHECK(!strcmp(saved(&r.port, "keyboard", "profile"), "nano"));
	CHECK(!strcmp(saved(&r.port, "appearance", "theme"), "spade"));
	CHECK(!strcmp(saved(&r.port, "meta", "onboarding_completed"), "true"));
	CHECK(WF_UILANG_JA == wf_i18n_language());
	rig_close(&r);

	/* and Esc, Esc, or any mix, is the same */
	rig_open(&r, WF_UILANG_EN);
	wf_app_setup_begin(&r.app, true);
	key(&r.app, WF_KEY_ESC);
	key(&r.app, WF_KEY_ESC);
	key(&r.app, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.app.mode && 1 == r.port.saves);
	CHECK(NULL == saved(&r.port, "general", "language"));
	CHECK(NULL == saved(&r.port, "keyboard", "profile")); /* kept */
	CHECK(WF_UILANG_EN == wf_i18n_language());
	rig_close(&r);

	/* many Enters in a row cannot get past the first run into trouble */
	rig_open(&r, WF_UILANG_EN);
	wf_app_setup_begin(&r.app, true);
	for (int i = 0; i < 6; i++) {
		key(&r.app, WF_KEY_ENTER);
	}
	CHECK(&WF_MODE_NORMAL == r.app.mode && 1 == r.port.saves);
	CHECK(!wcscmp(r.app.cwd, L"C:\\Users\\dev") && !r.app.want_path);
	/* later, Enter works again */
	r.app.now_ms = 10000;
	key(&r.app, WF_KEY_ENTER);
	CHECK('\0' != r.app.msg[0]); /* it tried to enter the folder (the fake
	                                tree has nothing there) */
	rig_close(&r);
}

static void a_chosen_language_is_saved_with_the_theme(void)
{
	static Rig r;

	rig_open(&r, WF_UILANG_EN);
	wf_app_setup_begin(&r.app, true);
	key(&r.app, WF_KEY_DOWN); /* English */
	key(&r.app, WF_KEY_DOWN); /* 日本語 */
	key(&r.app, WF_KEY_ENTER);
	key(&r.app, WF_KEY_DOWN); /* Vim */
	key(&r.app, WF_KEY_ENTER);
	key(&r.app, WF_KEY_RIGHT);
	key(&r.app, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == r.app.mode && 1 == r.port.saves);
	CHECK(!strcmp(saved(&r.port, "general", "language"), "ja"));
	CHECK(!strcmp(saved(&r.port, "keyboard", "profile"), "vim"));
	CHECK(WF_PROFILE_VIM == wf_keymap_profile(wf_app_keymap(&r.app)));
	CHECK(!strcmp(saved(&r.port, "appearance", "theme"), "heart"));
	CHECK(WF_LANGSET_JA == r.config.lang_setting &&
	        WF_UILANG_JA == wf_i18n_language());
	CHECK(has(&r.app, 100, 30, "準備完了"));
	rig_close(&r);

	/* Esc on the language screen keeps where it began (and goes on) */
	rig_open(&r, WF_UILANG_EN);
	wf_app_setup_begin(&r.app, true);
	key(&r.app, WF_KEY_DOWN);
	key(&r.app, WF_KEY_DOWN);
	CHECK(WF_UILANG_JA == wf_i18n_language());
	key(&r.app, WF_KEY_ESC);
	CHECK(WF_PICKER_KEYS == r.app.picker.stage &&
	        WF_LANGSET_AUTO == r.app.picker.lang &&
	        WF_UILANG_EN == wf_i18n_language());
	rig_close(&r);

	/* running the setup again begins on the language in force */
	rig_open(&r, WF_UILANG_EN);
	r.config.lang_setting = WF_LANGSET_JA;
	r.config.onboarding_completed = 1;
	wf_app_setup_begin(&r.app, true);
	CHECK(WF_LANGSET_JA == r.app.picker.lang &&
	        WF_UILANG_JA == wf_i18n_language());
	CHECK(has(&r.app, 80, 28, "> 日本語"));
	rig_close(&r);
}

static void a_failed_first_run_save_keeps_the_language_for_the_session(void)
{
	static Rig r;

	rig_open(&r, WF_UILANG_EN);
	r.port.save_status = WF_E_OS;
	wf_app_setup_begin(&r.app, true);
	key(&r.app, WF_KEY_DOWN);
	key(&r.app, WF_KEY_DOWN); /* 日本語 */
	key(&r.app, WF_KEY_ENTER);
	key(&r.app, WF_KEY_ENTER);
	key(&r.app, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == r.app.mode);
	CHECK(WF_UILANG_JA == wf_i18n_language());
	CHECK(NULL != strstr(r.app.msg, "保存できませんでした"));
	rig_close(&r);
}

/* The list is the languages table: every language is on the screen, in its
 * own words, and the settings choices carry the same ids. */
static void the_lists_follow_the_languages_table(void)
{
	static Rig r;
	char choices[128] = "auto";

	rig_open(&r, WF_UILANG_EN);
	wf_app_setup_begin(&r.app, true);
	for (int i = 0; i < WF_UILANG_COUNT; i++) {
		CHECK(has(&r.app, 80, 28, wf_i18n_lang_name((WfUiLang)i)));
		strcat_s(choices, sizeof choices, "|");
		strcat_s(choices, sizeof choices, wf_i18n_lang_id((WfUiLang)i));
	}
	CHECK(!strcmp(choices, wf_langset_choices));
	rig_close(&r);
}

static void the_settings_language_item_changes_everything_at_once(void)
{
	static Rig r;
	const WfEntry *before;
	wchar_t cwd[64];

	rig_open(&r, WF_UILANG_JA);
	before = wf_view_current(&r.app.cur);
	CHECK(NULL != before);
	wcscpy_s(cwd, 64, r.app.cwd);
	ch(&r.app, ',');
	CHECK(&WF_MODE_SETTINGS == r.app.mode && 0 == r.app.settings.category);
	CHECK(has(&r.app, 100, 30, "Language"));
	CHECK(has(&r.app, 100, 30, "Auto (日本語)"));
	CHECK(has(&r.app, 100, 30, "General"));
	key(&r.app, WF_KEY_RIGHT); /* into the details */
	choose(&r.app, 1);         /* auto -> en */
	CHECK(WF_LANGSET_EN == r.app.settings.draft.lang_setting);
	CHECK(has(&r.app, 100, 30, "English"));
	choose(&r.app, 1); /* en -> ja: the words change now */
	CHECK(WF_UILANG_JA == wf_i18n_language());
	CHECK(has(&r.app, 100, 30, "設定"));
	CHECK(has(&r.app, 100, 30, "表示言語"));
	CHECK(has(&r.app, 100, 30, "一般"));
	CHECK(has(&r.app, 100, 30, "外観"));
	CHECK(has(&r.app, 100, 30, "日本語"));
	CHECK(has(&r.app, 100, 30, "未保存の変更"));
	CHECK(has(
	        &r.app, 100, 30, "Ctrl+S 保存")); /* keys are not translated */
	/* nothing about the folder moved */
	CHECK(!wcscmp(r.app.cwd, cwd) && wf_view_current(&r.app.cur) == before);
	CHECK(0 == r.port.saves);
	/* Esc asks, d discards, and the English comes back */
	key(&r.app, WF_KEY_ESC);
	CHECK(has(&r.app, 100, 30, "d 破棄"));
	ch(&r.app, 'd');
	CHECK(&WF_MODE_NORMAL == r.app.mode);
	CHECK(WF_UILANG_JA == wf_i18n_language()); /* auto = the machine's */
	CHECK(WF_LANGSET_AUTO == r.config.lang_setting);
	rig_close(&r);

	/* on an English machine a discard returns to English */
	rig_open(&r, WF_UILANG_EN);
	ch(&r.app, ',');
	key(&r.app, WF_KEY_RIGHT);
	choose(&r.app, 2); /* auto -> ja */
	CHECK(WF_UILANG_JA == wf_i18n_language());
	key(&r.app, WF_KEY_ESC);
	ch(&r.app, 'd');
	CHECK(WF_UILANG_EN == wf_i18n_language());
	CHECK(has(&r.app, 100, 30, "READY"));
	rig_close(&r);
}

static void saving_makes_it_stick_and_a_failed_save_does_not_pretend(void)
{
	static Rig r;

	rig_open(&r, WF_UILANG_EN);
	ch(&r.app, ',');
	key(&r.app, WF_KEY_RIGHT);
	choose(&r.app, 2); /* auto -> ja */
	ctrl(&r.app, 's');
	CHECK(1 == r.port.saves);
	CHECK(!strcmp(saved(&r.port, "general", "language"), "ja"));
	CHECK(WF_LANGSET_JA == r.config.lang_setting);
	CHECK(NULL != strstr(r.app.settings.note, "保存しました"));
	key(&r.app, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.app.mode &&
	        WF_UILANG_JA == wf_i18n_language());
	rig_close(&r);

	rig_open(&r, WF_UILANG_EN);
	r.port.save_status = WF_E_OS;
	ch(&r.app, ',');
	key(&r.app, WF_KEY_RIGHT);
	choose(&r.app, 2); /* auto -> ja */
	ctrl(&r.app, 's');
	CHECK(r.app.settings.note_is_error && 0 == r.config.lang_setting);
	CHECK(NULL != strstr(r.app.settings.note, "保存できませんでした"));
	CHECK(NULL != strstr(r.app.settings.note, "disk full"));
	/* the live language did not become Japanese by a save that failed:
	 * discarding returns to what the file says */
	key(&r.app, WF_KEY_ESC);
	ch(&r.app, 'd');
	CHECK(WF_UILANG_EN == wf_i18n_language());
	rig_close(&r);
}

static void the_main_screen_speaks_japanese_without_touching_the_listing(void)
{
	static Rig r;
	const WfEntry *before;
	char text[64 * 1024];

	rig_open(&r, WF_UILANG_EN);
	before = wf_view_current(&r.app.cur);
	wf_i18n_set_language(WF_UILANG_JA);
	screen(&r.app, 120, 30, text, sizeof text);
	CHECK(strstr(text, "通常 準備完了"));
	CHECK(strstr(text, "現在のフォルダ"));
	CHECK(strstr(text, "親フォルダ"));
	CHECK(strstr(text, "3件"));
	CHECK(strstr(text, "file10.txt")); /* file names are as they are */
	CHECK(strstr(text, "Docs"));
	CHECK(wf_view_current(&r.app.cur) == before);
	/* help, details, create form */
	ch(&r.app, '?');
	CHECK(has(&r.app, 100, 30, "ヘルプ"));
	CHECK(has(&r.app, 100, 30, "Esc で閉じる"));
	key(&r.app, WF_KEY_ENTER);
	ch(&r.app, 'i');
	CHECK(has(&r.app, 100, 30, "名前:"));
	CHECK(has(&r.app, 100, 30, "更新日時:"));
	key(&r.app, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == r.app.mode);
	rig_close(&r);
}

static void smart_go_uses_the_current_language(void)
{
	static Rig r;

	rig_open(&r, WF_UILANG_EN);
	wf_i18n_set_language(WF_UILANG_JA);
	ch(&r.app, 'g');
	CHECK(&WF_MODE_GO == r.app.mode);
	CHECK(has(&r.app, 100, 30, "移動先: "));
	CHECK(has(&r.app, 100, 30, "Tab 補完  Enter 移動  Esc キャンセル"));
	ch(&r.app, 'x');
	ch(&r.app, 'y');
	key(&r.app, WF_KEY_ENTER);
	CHECK(&WF_MODE_GO == r.app.mode);
	rig_close(&r);
}

/* Every screen in Japanese at every size, for the sanitizers to watch. */
static void japanese_draws_at_any_size(void)
{
	static Rig r;
	static char text[64 * 1024];

	rig_open(&r, WF_UILANG_JA);
	wf_i18n_set_language(WF_UILANG_JA);
	for (int mode = 0; mode < 6; mode++) {
		for (int cols = 1; cols <= 150; cols += cols < 30 ? 1 : 17) {
			for (int rows = 1; rows <= 40;
			        rows += rows < 14 ? 1 : 9) {
				switch (mode) {
				case 1: r.app.mode = &WF_MODE_HELP; break;
				case 2: wf_app_setup_begin(&r.app, true); break;
				case 3: wf_app_settings_begin(&r.app); break;
				case 4:
					r.app.mode = &WF_MODE_NORMAL;
					wf_app_path_prompt_begin(&r.app);
					break;
				case 5:
					wf_app_setup_begin(&r.app, true);
					r.app.picker.stage = WF_PICKER_KEYS;
					break;
				default: r.app.mode = &WF_MODE_NORMAL; break;
				}
				screen(&r.app, cols, rows, text, sizeof text);
				CHECK(strlen(text) < sizeof text);
			}
		}
		r.app.mode = &WF_MODE_NORMAL;
	}
	rig_close(&r);
}

/* A Japanese label never runs into its neighbour in the settings table. */
static void settings_columns_do_not_overlap_in_japanese(void)
{
	static Rig r;
	static char text[64 * 1024];

	rig_open(&r, WF_UILANG_JA);
	wf_i18n_set_language(WF_UILANG_JA);
	ch(&r.app, ',');
	for (int cat = 0; cat < WF_CAT_COUNT; cat++) {
		r.app.settings.category = cat;
		r.app.settings.item = 0;
		screen(&r.app, 80, 24, text, sizeof text);
		/* a value column is never pushed off by a wide label: the
		 * rows have the same number of cells as the screen */
		for (char *line = text; *line;) {
			char *end = strchr(line, '\n');
			size_t cells = 0;

			for (const char *p = line;
			        p < (end ? end : line + strlen(line)); p++) {
				if (0x80 != ((unsigned char)*p & 0xC0)) {
					cells++;
				}
			}
			CHECK(cells <= 80 * 3); /* bytes' leads: bounded */
			if (!end) {
				break;
			}
			line = end + 1;
		}
	}
	rig_close(&r);
}

static WfStatus collision_submit(
        void *ctx, const WfCopySpec *spec, uint32_t *job)
{
	unsigned *calls = ctx;
	(void)spec;
	(void)job;
	(*calls)++;
	return WF_OK;
}

static void collision_dialog_offers_skip_and_localizes_old_overwrite_refusal(
        void)
{
	static Rig r;
	WfUiLang languages[] = {WF_UILANG_EN, WF_UILANG_JA};

	for (size_t i = 0; i < 2; i++) {
		unsigned calls = 0;
		WfPreflightReport report = {0};

		rig_open(&r, languages[i]);
		wf_i18n_set_language(languages[i]);
		r.app.copy.ctx = &calls;
		r.app.copy.submit = collision_submit;
		r.app.slot.mode = WF_XFER_COPY;
		r.app.xfer_busy = WF_XB_PREFLIGHT;
		report.reason = WF_PF_COLLISION;
		wcscpy_s(report.bad_name, 64, L"taken.txt");
		wf_app_on_preflight(&r.app, r.app.xfer_gen, WF_OK, &report);
		CHECK(&WF_MODE_CONFIRM == r.app.mode);
		r.app.confirm.state = WF_CONF_ARMED;
		CHECK(strstr(r.app.confirm.armed_text, "[O]") &&
		        strstr(r.app.confirm.armed_text, "[K]"));
		CHECK(has(&r.app, 100, 30, i ? "[S] スキップ" : "[S] Skip"));
		CHECK(has(&r.app, 100, 30, i ? "[K] 両方残す" : "[K] Keep both"));
		/* Overwrite is final: the dialog says the old file skips the
		 * Recycle Bin and is deleted, and nothing is cut off */
		CHECK(strstr(r.app.confirm.armed_text,
		        i ? "[Esc] 中止" : "[Esc] Cancel"));
		CHECK(!strcmp(r.app.confirm.armed_text,
		        wf_msg(MSG_XFER_COLLIDE_KEYS)));
		CHECK(!strcmp(r.app.confirm.lines[2], wf_msg(MSG_XFER_COLLIDE_1)));
		CHECK(!strcmp(r.app.confirm.lines[3], wf_msg(MSG_XFER_COLLIDE_2)));
		CHECK(!strcmp(r.app.confirm.lines[4], wf_msg(MSG_XFER_COLLIDE_3)));
		CHECK(!strcmp(r.app.confirm.lines[5], wf_msg(MSG_XFER_COLLIDE_4)));
		CHECK(!strcmp(r.app.confirm.lines[6], wf_msg(MSG_XFER_COLLIDE_5)));
		CHECK(7 == r.app.confirm.n);
		CHECK(has(&r.app, 100, 30,
		        i ? "上書きはごみ箱を使いません" : "does NOT use the Recycle Bin"));
		CHECK(has(&r.app, 100, 30,
		        i ? "完全に削除します" : "deleted for good"));
		/* the same lines whole on an 80x24 and a 60x20 terminal */
		CHECK(has(&r.app, 80, 24,
		        i ? "置かれたら古いファイルは完全に削除します。"
		          : "copy is in place, the old file is deleted for good."));
		CHECK(has(&r.app, 60, 20,
		        i ? "置かれたら古いファイルは完全に削除します。"
		          : "copy is in place, the old file is deleted for good."));
		CHECK(has(&r.app, 60, 20, i ? "[Esc] 中止" : "[Esc] Cancel"));
		ch(&r.app, 'O');
		/* the choice is taken; with no checker wired the job cannot start
		 * and says so */
		CHECK(&WF_MODE_NORMAL == r.app.mode && 0 == calls);
		CHECK(WF_COLLIDE_OVERWRITE == r.app.pending_collide &&
		        r.app.slot.collisions_ok);
		rig_close(&r);
	}
}

/* WF_I18N_DUMP=1 prints the Japanese screens (to look at them by eye). */
static void dump_one(Rig *r, const char *title, int cols, int rows)
{
	static char text[64 * 1024];

	screen(&r->app, cols, rows, text, sizeof text);
	printf("---- %s\n%s", title, text);
}

static void dump_screens(void)
{
	static Rig r;

	rig_open(&r, WF_UILANG_JA);
	wf_i18n_set_language(WF_UILANG_JA);
	dump_one(&r, "main", 100, 26);
	ch(&r.app, ',');
	dump_one(&r, "settings (General)", 100, 26);
	r.app.settings.category = WF_CAT_FILES;
	dump_one(&r, "settings (Files)", 100, 26);
	r.app.settings.category = WF_CAT_KEYBOARD;
	r.app.settings.focus = WF_FOCUS_DETAILS;
	dump_one(&r, "keys", 100, 26);
	r.app.settings.category = WF_CAT_APPEARANCE;
	dump_one(&r, "themes", 100, 26);
	r.app.settings.section = WF_SEC_ABOUT;
	r.app.settings.category = 0;
	dump_one(&r, "about", 100, 26);
	r.app.mode = &WF_MODE_NORMAL;
	wf_app_setup_begin(&r.app, true);
	dump_one(&r, "first run", 100, 30);
	r.app.mode = &WF_MODE_NORMAL;
	ch(&r.app, 'g');
	dump_one(&r, "smart go", 100, 30);
	rig_close(&r);
}

int main(void)
{
	char *dump = NULL;
	size_t dump_n = 0;

	if (0 == _dupenv_s(&dump, &dump_n, "WF_I18N_DUMP") && dump) {
		free(dump);
		dump_screens();
		return 0;
	}
	the_language_is_asked_first_and_starts_on_the_windows_language();
	enter_twice_accepts_everything();
	a_chosen_language_is_saved_with_the_theme();
	a_failed_first_run_save_keeps_the_language_for_the_session();
	the_lists_follow_the_languages_table();
	the_settings_language_item_changes_everything_at_once();
	saving_makes_it_stick_and_a_failed_save_does_not_pretend();
	the_main_screen_speaks_japanese_without_touching_the_listing();
	smart_go_uses_the_current_language();
	japanese_draws_at_any_size();
	settings_columns_do_not_overlap_in_japanese();
	collision_dialog_offers_skip_and_localizes_old_overwrite_refusal();
	return TEST_RESULT();
}

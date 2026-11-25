/*
 * The first-run theme screen, the settings screen and the Custom colour
 * editor, driven with key events against a fake settings port that records
 * what the app asks it to save and apply.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "ui/wf_app_priv.h"
#include "ui/wf_app_settings_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"
#include "base/wf_macro.h"
#include "edit/wf_eenc.h"

typedef struct
{
	unsigned saves, applies, committed_applies, reloads;
	WfConfigChange last[96];
	size_t last_n;
	WfStatus save_status;
	char save_why[64];
	WfConfig reload_to;
	WfStatus reload_status;
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
	/* a built section name lives in the change itself */
	for (size_t i = 0; i < p->last_n; i++) {
		if (changes[i].section == changes[i].section_buf) {
			p->last[i].section = p->last[i].section_buf;
		}
	}
	if (WF_OK != p->save_status) {
		snprintf(why, cap, "%s", p->save_why);
	}
	return p->save_status;
}

static void port_apply(void *ctx, const WfConfig *config, bool committed)
{
	Port *p = ctx;

	(void)config;
	p->applies++;
	if (committed) {
		p->committed_applies++;
	}
}

static WfStatus port_reload(void *ctx, WfConfig *fresh)
{
	Port *p = ctx;

	p->reloads++;
	if (WF_OK != p->reload_status) {
		return p->reload_status;
	}
	*fresh = p->reload_to;
	fresh->start_directory = NULL;
	return WF_OK;
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

static void rig_open(Rig *r, bool with_port)
{
	memset(r, 0, sizeof *r);
	r->fs.app = &r->app;
	wf_config_init(&r->config);
	CHECK(wf_app_init(&r->app, 80, 24, fake_port(&r->fs),
	              L"C:\\Users\\dev") == WF_OK);
	if (with_port) {
		wf_app_set_settings(&r->app, &r->config,
		        (WfSettingsPort){
		                &r->port, port_save, port_apply, port_reload});
	} else {
		wf_app_set_settings(&r->app, &r->config, (WfSettingsPort){0});
	}
	wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
}

static void rig_close(Rig *r)
{
	wf_app_destroy(&r->app);
	wf_config_dispose(&r->config);
}

static void key(WfApp *a, WfKey k)
{
	WfEvent e = {WF_EV_KEY, 0, k, 0};

	a->mode->on_event(a, &e);
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

static void type(WfApp *a, const char *s)
{
	for (; *s; s++) {
		ch(a, (uint32_t)(unsigned char)*s);
	}
}

/* One screen row as plain text. */
static void row(const WfFrame *f, int y, char *out, size_t cap)
{
	size_t n = 0;

	for (int x = 0; x < f->cols && n + 4 < cap; x++) {
		const WfCell *c = &f->cells[y * f->cols + x];

		if (0 == c->width) {
			continue;
		}
		n += (size_t)snprintf(
		        out + n, cap - n, "%s", c->utf8[0] ? c->utf8 : " ");
	}
	out[n] = '\0';
}

static bool screen_has(WfApp *a, int cols, int rows, const char *needle)
{
	WfFrame *f = wf_frame_new(cols, rows, &WF_STYLE_BASE);
	bool found = false;
	char line[512];

	a->cols = cols;
	a->rows = rows;
	wf_app_draw(a, f);
	for (int y = 0; y < rows && !found; y++) {
		row(f, y, line, sizeof line);
		found = NULL != strstr(line, needle);
	}
	wf_frame_free(f);
	return found;
}

static uint32_t corner_background(WfApp *a)
{
	WfFrame *f = wf_frame_new(80, 24, &WF_STYLE_BASE);
	uint32_t bg;

	wf_app_draw(a, f);
	bg = f->cells[0].bg_rgb;
	wf_frame_free(f);
	return bg;
}

/* The first-run screens: the language comes first, Enter accepts it, and the
 * theme screen follows (the language screen has its own tests). */
static void first_run(WfApp *a)
{
	wf_app_setup_begin(a, true);
	CHECK(WF_PICKER_LANGUAGE == a->picker.stage);
	key(a, WF_KEY_ENTER);
	CHECK(WF_PICKER_KEYS == a->picker.stage && &WF_MODE_SETUP == a->mode);
	key(a, WF_KEY_ENTER); /* Nano, the row it begins on */
	CHECK(WF_PICKER_THEME == a->picker.stage && &WF_MODE_SETUP == a->mode);
}

static void the_first_run_screen_shows_the_logo_and_the_eight_themes(void)
{
	static Rig r;
	static const char *const names[] = {"Spade", "Heart", "Diamond", "Club",
	        "Joker", "King", "Queen", "Ace"};

	rig_open(&r, true);
	first_run(&r.app);
	CHECK(&WF_MODE_SETUP == r.app.mode);
	CHECK(screen_has(&r.app, 80, 24, "| '_ \\| |/ _ \\ '__/ _ \\"));
	CHECK(screen_has(&r.app, 80, 24, "| .__/|_|\\___|_|  \\___/"));
	CHECK(screen_has(&r.app, 80, 24, "Windows Native TUI File Manager"));
	CHECK(screen_has(&r.app, 80, 24, "Choose your theme"));
	CHECK(screen_has(&r.app, 80, 24, "Enter  Select"));
	CHECK(screen_has(&r.app, 80, 24, "Esc    Use default"));
	CHECK(!screen_has(&r.app, 80, 24, "Welcome"));
	for (size_t i = 0; i < 8; i++) {
		CHECK(screen_has(&r.app, 80, 24, names[i]));
	}
	CHECK(screen_has(&r.app, 80, 24, "> Spade"));
	/* only the cursor carries the marker */
	CHECK(!screen_has(&r.app, 80, 24, "> Heart"));
	/* the names are plain words: no suit symbols, no emoji */
	{
		WfFrame *f = wf_frame_new(80, 24, &WF_STYLE_BASE);
		char line[512];

		wf_app_draw(&r.app, f);
		for (int y = 0; y < 24; y++) {
			row(f, y, line, sizeof line);
			for (const unsigned char *c =
			                (const unsigned char *)line;
			        *c; c++) {
				CHECK(*c < 0x80);
			}
		}
		wf_frame_free(f);
	}
	rig_close(&r);
}

static void moving_previews_every_theme_and_saves_nothing(void)
{
	static Rig r;
	static const uint32_t bg[] = {0x0F172A, 0x21141E, 0xF8FAFC, 0x0E1B16,
	        0x100E18, 0x241610, 0x1A1426, 0x18181B};

	rig_open(&r, true);
	first_run(&r.app);
	CHECK(corner_background(&r.app) == bg[0]);
	key(&r.app, WF_KEY_RIGHT);
	CHECK(1 == r.app.picker.index && corner_background(&r.app) == bg[1]);
	key(&r.app, WF_KEY_RIGHT); /* already the right column */
	CHECK(1 == r.app.picker.index);
	key(&r.app, WF_KEY_DOWN);
	CHECK(3 == r.app.picker.index && corner_background(&r.app) == bg[3]);
	key(&r.app, WF_KEY_LEFT);
	CHECK(2 == r.app.picker.index && corner_background(&r.app) == bg[2]);
	key(&r.app, WF_KEY_LEFT); /* already the left column */
	CHECK(2 == r.app.picker.index);
	for (int i = 0; i < 6; i++) {
		key(&r.app, WF_KEY_DOWN);
	}
	CHECK(6 == r.app.picker.index && corner_background(&r.app) == bg[6]);
	key(&r.app, WF_KEY_RIGHT);
	CHECK(7 == r.app.picker.index && corner_background(&r.app) == bg[7]);
	for (int i = 0; i < 8; i++) {
		key(&r.app, WF_KEY_UP);
	}
	CHECK(1 == r.app.picker.index);
	/* the accent of the logo follows the theme too */
	{
		WfFrame *f = wf_frame_new(80, 24, &WF_STYLE_BASE);
		bool seen = false;

		wf_app_draw(&r.app, f);
		for (int i = 0; i < 80 * 24; i++) {
			if (0xF9A8D4 == f->cells[i].fg_rgb) {
				seen = true; /* Heart's accent */
			}
		}
		CHECK(seen);
		wf_frame_free(f);
	}
	CHECK(0 == r.port.saves && 0 == r.config.onboarding_completed);
	rig_close(&r);
}

static void enter_chooses_and_saves(void)
{
	static Rig r;

	rig_open(&r, true);
	first_run(&r.app);
	key(&r.app, WF_KEY_DOWN);
	key(&r.app, WF_KEY_RIGHT); /* index 3: Club */
	CHECK(0 == r.port.saves);
	key(&r.app, WF_KEY_ENTER);
	CHECK(1 == r.port.saves && &WF_MODE_NORMAL == r.app.mode);
	CHECK(!strcmp(saved(&r.port, "appearance", "theme"), "club"));
	CHECK(!strcmp(saved(&r.port, "meta", "onboarding_completed"), "true"));
	CHECK(!strcmp(saved(&r.port, "meta", "schema_version"), "1"));
	CHECK(WF_THEME_CLUB == r.config.theme && r.config.onboarding_completed);
	CHECK(0x0E1B16 == wf_theme_active()->background);
	CHECK(1 == r.port.committed_applies);
	rig_close(&r);
}

static void esc_uses_the_default_and_still_records_the_setup(void)
{
	static Rig r;

	rig_open(&r, true);
	first_run(&r.app);
	key(&r.app, WF_KEY_RIGHT);
	key(&r.app, WF_KEY_DOWN); /* looking at something else */
	key(&r.app, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.app.mode && 1 == r.port.saves);
	CHECK(!strcmp(saved(&r.port, "appearance", "theme"), "spade"));
	CHECK(!strcmp(saved(&r.port, "meta", "onboarding_completed"), "true"));
	CHECK(WF_THEME_SPADE == r.config.theme);
	CHECK(0x0F172A == wf_theme_active()->background);
	rig_close(&r);
}

static void a_failed_save_does_not_block_the_start(void)
{
	static Rig r;

	rig_open(&r, true);
	r.port.save_status = WF_E_OS;
	snprintf(r.port.save_why, sizeof r.port.save_why, "disk is read-only");
	first_run(&r.app);
	key(&r.app, WF_KEY_RIGHT);
	key(&r.app, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == r.app.mode);
	CHECK(strstr(r.app.msg, "not saved") &&
	        strstr(r.app.msg, "disk is read"));
	/* the choice still holds for this session */
	CHECK(WF_THEME_HEART == r.config.theme &&
	        0x21141E == wf_theme_active()->background);
	rig_close(&r);

	/* no port at all: same */
	rig_open(&r, false);
	first_run(&r.app);
	key(&r.app, WF_KEY_RIGHT);
	key(&r.app, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == r.app.mode &&
	        WF_THEME_HEART == r.config.theme);
	rig_close(&r);
}

static void running_setup_again_keeps_the_current_theme_on_esc(void)
{
	static Rig r;

	rig_open(&r, true);
	r.config.theme = WF_THEME_QUEEN;
	r.config.onboarding_completed = 1;
	r.config.schema_version = 1;
	first_run(&r.app);
	CHECK(WF_THEME_QUEEN == r.app.picker.index);
	key(&r.app, WF_KEY_UP);
	key(&r.app, WF_KEY_ESC);
	CHECK(WF_THEME_QUEEN == r.config.theme &&
	        0x1A1426 == wf_theme_active()->background);
	CHECK(&WF_MODE_NORMAL == r.app.mode);
	rig_close(&r);
}

static void small_screens_get_smaller_layouts(void)
{
	static Rig r;

	rig_open(&r, true);
	first_run(&r.app);
	/* medium: the big logo without the extra air */
	CHECK(screen_has(&r.app, 60, 16, "| .__/|_|"));
	CHECK(screen_has(&r.app, 60, 16, "Enter  Select"));
	/* small: one line of text for the name, still the whole chooser */
	CHECK(screen_has(&r.app, 40, 11, "piero"));
	CHECK(!screen_has(&r.app, 40, 11, "| .__/"));
	CHECK(screen_has(&r.app, 40, 11, "Choose your theme"));
	CHECK(screen_has(&r.app, 40, 11, "> Spade") &&
	        screen_has(&r.app, 40, 11, "Queen"));
	/* tiny: a single column that follows the cursor */
	CHECK(screen_has(&r.app, 16, 4, "> Spade"));
	for (int i = 0; i < 4; i++) {
		key(&r.app, WF_KEY_DOWN);
	}
	CHECK(6 == r.app.picker.index);
	CHECK(screen_has(&r.app, 16, 4, "> Queen"));
	/* and nothing breaks at absurd sizes */
	(void)screen_has(&r.app, 1, 1, "x");
	(void)screen_has(&r.app, 3, 2, "x");
	(void)screen_has(&r.app, 200, 60, "x");
	rig_close(&r);
}

static void down(WfApp *a, int n)
{
	while (n-- > 0) {
		key(a, WF_KEY_DOWN);
	}
}

/* `,`, then Down to the category: the cursor is in the middle column. */
static void open_cat(WfApp *a, int cat)
{
	ch(a, ',');
	CHECK(&WF_MODE_SETTINGS == a->mode);
	down(a, cat);
	CHECK(a->settings.category == cat);
}

static void into(WfApp *a)
{
	key(a, WF_KEY_RIGHT);
	CHECK(WF_FOCUS_DETAILS == a->settings.focus);
}

static void the_settings_key_opens_three_columns_only_with_a_configuration(void)
{
	static Rig r;

	rig_open(&r, true);
	ch(&r.app, ',');
	CHECK(&WF_MODE_SETTINGS == r.app.mode && wf_app_editing(&r.app));
	CHECK(WF_SEC_SETTINGS == r.app.settings.section &&
	        WF_FOCUS_CATEGORY == r.app.settings.focus);
	/* the sections, the categories and the details of the first one */
	CHECK(screen_has(&r.app, 100, 30, "Settings") &&
	        screen_has(&r.app, 100, 30, "Licenses") &&
	        screen_has(&r.app, 100, 30, "About"));
	CHECK(screen_has(&r.app, 100, 30, "General") &&
	        screen_has(&r.app, 100, 30, "Keyboard") &&
	        screen_has(&r.app, 100, 30, "Appearance") &&
	        screen_has(&r.app, 100, 30, "Files") &&
	        screen_has(&r.app, 100, 30, "Preview") &&
	        screen_has(&r.app, 100, 30, "Editor") &&
	        screen_has(&r.app, 100, 30, "Archives") &&
	        screen_has(&r.app, 100, 30, "Operations"));
	CHECK(screen_has(&r.app, 100, 30, "Language"));
	key(&r.app, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.app.mode && !wf_app_editing(&r.app));
	rig_close(&r);

	rig_open(&r, true);
	r.app.config = NULL;
	ch(&r.app, ',');
	CHECK(&WF_MODE_NORMAL == r.app.mode &&
	        strstr(r.app.msg, "not available"));
	rig_close(&r);
}

static void the_arrows_move_between_the_columns(void)
{
	static Rig r;

	rig_open(&r, true);
	ch(&r.app, ',');
	key(&r.app, WF_KEY_LEFT); /* sections */
	CHECK(WF_FOCUS_SECTION == r.app.settings.focus);
	key(&r.app, WF_KEY_LEFT); /* nowhere further */
	CHECK(WF_FOCUS_SECTION == r.app.settings.focus);
	key(&r.app, WF_KEY_DOWN); /* Licenses */
	CHECK(WF_SEC_LICENSES == r.app.settings.section);
	key(&r.app, WF_KEY_DOWN); /* About */
	CHECK(WF_SEC_ABOUT == r.app.settings.section);
	key(&r.app, WF_KEY_DOWN);
	CHECK(WF_SEC_ABOUT == r.app.settings.section);
	key(&r.app, WF_KEY_UP);
	key(&r.app, WF_KEY_UP);
	CHECK(WF_SEC_SETTINGS == r.app.settings.section);
	key(&r.app, WF_KEY_RIGHT);
	CHECK(WF_FOCUS_CATEGORY == r.app.settings.focus);
	key(&r.app, WF_KEY_ENTER); /* Enter opens the details too */
	CHECK(WF_FOCUS_DETAILS == r.app.settings.focus);
	key(&r.app, WF_KEY_RIGHT); /* on a choice, Right opens its list ... */
	CHECK(WF_FOCUS_DETAILS == r.app.settings.focus &&
	        WF_SS_PICK == r.app.settings.sub);
	key(&r.app, WF_KEY_LEFT); /* ... and Left closes it, changing nothing */
	CHECK(WF_SS_BROWSE == r.app.settings.sub &&
	        WF_FOCUS_DETAILS == r.app.settings.focus &&
	        WF_LANGSET_AUTO == r.app.settings.draft.lang_setting);
	key(&r.app, WF_KEY_LEFT); /* Left is back, even on a row with a value */
	CHECK(WF_FOCUS_CATEGORY == r.app.settings.focus &&
	        WF_LANGSET_AUTO == r.app.settings.draft.lang_setting);
	/* the categories */
	for (int i = 1; i < WF_CAT_COUNT; i++) {
		key(&r.app, WF_KEY_DOWN);
		CHECK(r.app.settings.category == i);
	}
	key(&r.app, WF_KEY_DOWN);
	CHECK(r.app.settings.category == WF_CAT_COUNT - 1);
	/* nothing was changed by moving about */
	key(&r.app, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.app.mode && 0 == r.port.saves);
	rig_close(&r);
}

static void changing_the_theme_previews_and_cancelling_restores_it(void)
{
	static Rig r;

	rig_open(&r, true);
	open_cat(&r.app, WF_CAT_APPEARANCE);
	into(&r.app);
	down(&r.app, 1);
	key(&r.app, WF_KEY_ENTER); /* Heart */
	CHECK(WF_THEME_HEART == r.app.settings.draft.theme);
	CHECK(0x21141E == wf_theme_active()->background);
	CHECK(WF_THEME_SPADE == r.config.theme && 0 == r.port.saves);
	CHECK(screen_has(&r.app, 100, 30, "unsaved changes"));
	key(&r.app, WF_KEY_ESC); /* asks */
	CHECK(WF_SS_DISCARD == r.app.settings.sub);
	CHECK(screen_has(&r.app, 100, 30, "d discard"));
	ch(&r.app, 'c'); /* keep editing */
	CHECK(WF_SS_BROWSE == r.app.settings.sub &&
	        &WF_MODE_SETTINGS == r.app.mode);
	key(&r.app, WF_KEY_ESC);
	ch(&r.app, 'd'); /* discard */
	CHECK(&WF_MODE_NORMAL == r.app.mode);
	CHECK(0x0F172A == wf_theme_active()->background); /* completely back */
	CHECK(WF_THEME_SPADE == r.config.theme && 0 == r.port.saves);
	rig_close(&r);
}

/*
 * The unsaved-changes question of Settings is answered the way every short
 * question is: S, D or C, or Up/Down and Enter, the selection starting on
 * "keep editing". With an IME on, a letter key never arrives as that letter.
 */
static void the_unsaved_question_is_answered_without_letters(void)
{
	static Rig r;
	WfEvent processed = {WF_EV_CHAR, 'd', WF_KEY_NONE, 0xE5};

	rig_open(&r, true);
	open_cat(&r.app, WF_CAT_APPEARANCE);
	into(&r.app);
	down(&r.app, 1);
	key(&r.app, WF_KEY_ENTER); /* Heart */
	key(&r.app, WF_KEY_ESC);
	CHECK(WF_SS_DISCARD == r.app.settings.sub);
	CHECK(screen_has(&r.app, 100, 30, "Enter = C"));

	/* a stray Enter keeps editing: nothing is discarded or saved */
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_SS_BROWSE == r.app.settings.sub &&
	        &WF_MODE_SETTINGS == r.app.mode);
	CHECK(WF_THEME_HEART == r.app.settings.draft.theme &&
	        0 == r.port.saves);

	/* what an IME delivers in place of the letters answers nothing */
	key(&r.app, WF_KEY_ESC);
	CHECK(WF_SS_DISCARD == r.app.settings.sub);
	ch(&r.app, 0xFF44); /* fullwidth d */
	ch(&r.app, 0xFF53); /* fullwidth s */
	ch(&r.app, 0x3067); /* kana de */
	ch(&r.app, 0x1F600);
	r.app.mode->on_event(&r.app, &processed); /* 'd' handled by the IME */
	CHECK(WF_SS_DISCARD == r.app.settings.sub &&
	        &WF_MODE_SETTINGS == r.app.mode);
	CHECK(WF_THEME_HEART == r.app.settings.draft.theme &&
	        0 == r.port.saves);

	/* Up selects Discard, Enter discards: back to the saved theme */
	key(&r.app, WF_KEY_UP);
	CHECK(screen_has(&r.app, 100, 30, "Enter = D"));
	key(&r.app, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == r.app.mode && 0 == r.port.saves);
	CHECK(WF_THEME_SPADE == r.config.theme);
	rig_close(&r);

	/* Down from keep-editing wraps to Save; Enter saves */
	rig_open(&r, true);
	open_cat(&r.app, WF_CAT_APPEARANCE);
	into(&r.app);
	down(&r.app, 1);
	key(&r.app, WF_KEY_ENTER);
	key(&r.app, WF_KEY_ESC);
	CHECK(WF_SS_DISCARD == r.app.settings.sub);
	key(&r.app, WF_KEY_DOWN);
	CHECK(screen_has(&r.app, 100, 30, "Enter = S"));
	key(&r.app, WF_KEY_ENTER);
	CHECK(1 == r.port.saves);
	rig_close(&r);
}

static void ctrl_s_saves_only_what_changed(void)
{
	static Rig r;

	rig_open(&r, true);
	open_cat(&r.app, WF_CAT_APPEARANCE);
	into(&r.app);
	down(&r.app, 7);
	key(&r.app, WF_KEY_ENTER); /* Ace */
	CHECK(WF_THEME_ACE == r.app.settings.draft.theme);
	ctrl(&r.app, 's');
	CHECK(1 == r.port.saves);
	CHECK(!strcmp(saved(&r.port, "appearance", "theme"), "ace"));
	CHECK(!strcmp(saved(&r.port, "meta", "onboarding_completed"), "true"));
	CHECK(NULL == saved(&r.port, "files", "sort_by"));
	CHECK(NULL == saved(&r.port, "keyboard", "profile")); /* untouched */
	CHECK(WF_THEME_ACE == r.config.theme && 1 == r.port.committed_applies);
	CHECK(strstr(r.app.settings.note, "Saved"));
	/* nothing unsaved now: Esc leaves at once */
	key(&r.app, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.app.mode &&
	        0x18181B == wf_theme_active()->background);
	/* saving with nothing changed writes nothing */
	ch(&r.app, ',');
	ctrl(&r.app, 's');
	CHECK(1 == r.port.saves && strstr(r.app.settings.note, "Nothing"));
	rig_close(&r);
}

/* The list on screen is ordered by [files] sort_by, sort_descending and
 * directories_first: changing any of the three asks for the folder again, and a
 * change of something else asks for nothing. */
static void a_change_of_the_order_reloads_the_list(void)
{
	static Rig r;
	WfConfig before;

	rig_open(&r, true);
	r.fs.defer = 1;
	r.fs.nq = 0;
	before = r.config;
	wf_set_apply_live_changes(&r.app, &before);
	CHECK(0 == r.fs.nq); /* nothing changed: nothing asked */
	r.config.directories_first = !before.directories_first;
	wf_set_apply_live_changes(&r.app, &before);
	CHECK(r.fs.nq >
	        0); /* the same folder is listed again, in the new order */
	rig_close(
	        &r); /* the request above is unanswered and holds the folder */
	rig_open(&r, true);
	r.fs.defer = 1;
	r.fs.nq = 0;
	before = r.config;
	r.config.sort_descending = !before.sort_descending;
	wf_set_apply_live_changes(&r.app, &before);
	CHECK(r.fs.nq > 0);
	rig_close(&r);
}

static void editor_previous_version_policy_is_fixed(void)
{
	static Rig r;

	rig_open(&r, true);
	open_cat(&r.app, WF_CAT_EDITOR);
	into(&r.app);
	down(&r.app, 9);
	CHECK(screen_has(&r.app, 120, 30, "Always kept as ~"));
	key(&r.app, WF_KEY_ENTER);
	CHECK(strstr(r.app.settings.note, "fixed"));
	CHECK(1 == r.app.settings.draft.editor_backup && 0 == r.port.saves);
	rig_close(&r);
}

/* Settings opens again where it was left: the category and the row, not
 * the top of the first page. */
static void settings_comes_back_where_it_was_left(void)
{
	static Rig r;
	int section, category, focus;
	size_t item;

	rig_open(&r, true);
	wf_app_settings_begin(&r.app);
	key(&r.app, WF_KEY_DOWN); /* another category */
	key(&r.app, WF_KEY_RIGHT);
	key(&r.app, WF_KEY_DOWN); /* a row inside it */
	section = r.app.settings.section;
	category = r.app.settings.category;
	focus = r.app.settings.focus;
	item = r.app.settings.item;
	CHECK(category != 0 && item > 0);
	key(&r.app, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.app.mode);
	wf_app_settings_begin(&r.app);
	CHECK(r.app.settings.section == section);
	CHECK(r.app.settings.category == category);
	CHECK(r.app.settings.focus == focus);
	CHECK(r.app.settings.item == item);
	rig_close(&r);
}

static void newer_schema_settings_are_never_written(void)
{
	static Rig r;
	char why[128];

	rig_open(&r, true);
	r.config.schema_version = 7;
	wf_app_settings_begin(&r.app);
	r.app.settings.draft.theme = WF_THEME_HEART;
	ctrl(&r.app, 's');
	ctrl(&r.app, 's');
	CHECK(0 == r.port.saves && 7 == r.config.schema_version);
	CHECK(r.app.settings.note_is_error &&
	        strstr(r.app.settings.note, "newer"));
	CHECK(!wf_app_settings_commit(
	        &r.app, &r.app.settings.draft, false, why, sizeof why));
	CHECK(0 == r.port.saves && 7 == r.config.schema_version);
	rig_close(&r);

	rig_open(&r, true);
	r.config.schema_version = 7;
	first_run(&r.app);
	key(&r.app, WF_KEY_RIGHT);
	key(&r.app, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == r.app.mode && 0 == r.port.saves);
	CHECK(7 == r.config.schema_version && strstr(r.app.msg, "newer"));
	wf_app_settings_begin(&r.app);
	ctrl(&r.app, 's');
	CHECK(0 == r.port.saves && 7 == r.config.schema_version);
	rig_close(&r);
}

static void a_failed_save_keeps_the_draft_and_says_why(void)
{
	static Rig r;

	rig_open(&r, true);
	open_cat(&r.app, WF_CAT_APPEARANCE);
	into(&r.app);
	down(&r.app, 1);
	key(&r.app, WF_KEY_ENTER);
	r.port.save_status = WF_E_INVAL;
	snprintf(r.port.save_why, sizeof r.port.save_why,
	        "the configuration file changed on disk; press r to reload it");
	ctrl(&r.app, 's');
	CHECK(1 == r.port.saves && r.app.settings.note_is_error);
	CHECK(strstr(r.app.settings.note, "changed on disk"));
	CHECK(WF_THEME_SPADE ==
	                r.config.theme && /* the live setting is intact */
	        WF_THEME_HEART == r.app.settings.draft.theme);
	/* r reloads what is on disk and drops the draft */
	r.port.reload_to = r.config;
	r.port.reload_to.theme = WF_THEME_QUEEN;
	ch(&r.app, 'r');
	CHECK(!r.port.reloads && r.app.settings.sub == WF_SS_DISCARD);
	key(&r.app, WF_KEY_ENTER);
	CHECK(!r.port.reloads && r.app.settings.draft.theme == WF_THEME_HEART);
	ch(&r.app, 'r');
	ch(&r.app, 'd');
	CHECK(1 == r.port.reloads && WF_THEME_QUEEN == r.config.theme &&
	        WF_THEME_QUEEN == r.app.settings.draft.theme &&
	        0x1A1426 == wf_theme_active()->background);
	/* a failed reload leaves things as they are */
	r.port.reload_status = WF_E_OS;
	r.config.theme = WF_THEME_KING;
	ch(&r.app, 'r');
	ch(&r.app, 'd');
	CHECK(r.app.settings.note_is_error && WF_THEME_KING == r.config.theme);
	rig_close(&r);
}

static void items_change_by_kind_and_other_categories_are_reachable(void)
{
	static Rig r;

	rig_open(&r, true);
	open_cat(&r.app, WF_CAT_FILES);
	into(&r.app);
	key(&r.app, WF_KEY_ENTER); /* Show hidden: toggled */
	CHECK(1 == r.app.settings.draft.show_hidden);
	down(&r.app, 1);
	key(&r.app, WF_KEY_ENTER); /* Sort by: a list of its values */
	CHECK(WF_SS_PICK == r.app.settings.sub);
	key(&r.app, WF_KEY_DOWN);
	key(&r.app, WF_KEY_ENTER); /* size */
	CHECK(WF_SS_BROWSE == r.app.settings.sub &&
	        WF_CONFIG_SORT_SIZE == r.app.settings.draft.sort_by);
	key(&r.app, WF_KEY_BACKSPACE);
	key(&r.app, WF_KEY_BACKSPACE); /* back, and wrapping to ext */
	CHECK(WF_CONFIG_SORT_EXT == r.app.settings.draft.sort_by);
	key(&r.app, WF_KEY_TAB);
	down(&r.app, 1); /* Preview */
	into(&r.app);
	key(&r.app, WF_KEY_ENTER); /* Directory preview: off */
	CHECK(0 == r.app.settings.draft.dir_preview);
	key(&r.app, WF_KEY_TAB);
	down(&r.app, 1); /* Editor */
	into(&r.app);
	down(&r.app, 1); /* Tab width */
	for (int i = 0; i < 40; i++) {
		ch(&r.app, '+'); /* clamped at 16 */
	}
	CHECK(16 == r.app.settings.draft.editor_tab_width);
	for (int i = 0; i < 40; i++) {
		ch(&r.app, '-');
	}
	CHECK(1 == r.app.settings.draft.editor_tab_width);
	/* Enter asks for the number: typing replaces it, a value outside the
	 * range is refused and the field stays, Esc keeps the old one */
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_SS_TEXT == r.app.settings.sub);
	ch(&r.app, '9');
	ch(&r.app, '9');
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_SS_TEXT == r.app.settings.sub &&
	        r.app.settings.note_is_error &&
	        1 == r.app.settings.draft.editor_tab_width);
	key(&r.app, WF_KEY_BACKSPACE);
	key(&r.app, WF_KEY_BACKSPACE);
	ch(&r.app, 0xFF11); /* a full-width 1 from an IME */
	ch(&r.app, 0xFF12);
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_SS_BROWSE == r.app.settings.sub &&
	        12 == r.app.settings.draft.editor_tab_width);
	key(&r.app, WF_KEY_ENTER);
	ch(&r.app, '5');
	key(&r.app, WF_KEY_ESC);
	CHECK(WF_SS_BROWSE == r.app.settings.sub &&
	        12 == r.app.settings.draft.editor_tab_width);
	key(&r.app, WF_KEY_TAB);
	down(&r.app, 1); /* Archives: read-only, and said so */
	into(&r.app);
	CHECK(screen_has(&r.app, 100, 30, "read-only"));
	key(&r.app, WF_KEY_ENTER); /* a fixed row does not change */
	CHECK(strstr(r.app.settings.note, "fixed"));
	key(&r.app, WF_KEY_LEFT);
	down(&r.app, 1); /* Operations */
	into(&r.app);
	CHECK(screen_has(&r.app, 100, 30, "Recycle Bin only"));
	key(&r.app, WF_KEY_ENTER);
	key(&r.app, WF_KEY_LEFT);

	/* save: show_hidden reaches the lists, the directory preview goes off */
	ctrl(&r.app, 's');
	CHECK(1 == r.port.saves);
	CHECK(!strcmp(saved(&r.port, "ui", "show_hidden"), "true"));
	CHECK(!strcmp(saved(&r.port, "files", "sort_by"), "ext"));
	CHECK(!strcmp(saved(&r.port, "preview", "directory_preview"), "false"));
	CHECK(!strcmp(saved(&r.port, "editor", "tab_width"), "12"));
	CHECK(1 == r.config.show_hidden && r.app.cur.show_hidden);
	CHECK(r.app.dirprev_off);
	rig_close(&r);
}

static void settings_never_list_what_does_not_exist(void)
{
	static Rig r;
	static const char *const bogus[] = {
	        "Icons", "Borders", "Emacs", "Overwrite", "Delete permanently"};

	rig_open(&r, true);
	ch(&r.app, ',');
	for (int c = 0; c < WF_CAT_COUNT; c++) {
		r.app.settings.category = c;
		for (size_t i = 0; i < WF_ARRAY_LEN(bogus); i++) {
			CHECK(!screen_has(&r.app, 100, 30, bogus[i]));
		}
	}
	rig_close(&r);
}

static const WfKeyOverride *override_of(
        const WfApp *a, WfKeyContext ctx, uint16_t cmd)
{
	return wf_keyconfig_find(&a->settings.draft.keys, ctx, cmd);
}

/* Filters the list to one command and puts the cursor on it. */
static void find_command(WfApp *a, const char *name)
{
	ch(a, '/');
	CHECK(WF_SS_TEXT == a->settings.sub);
	type(a, name);
	key(a, WF_KEY_ENTER);
	CHECK(WF_SS_BROWSE == a->settings.sub);
}

static void the_profile_is_chosen_in_the_keyboard_page(void)
{
	static Rig r;

	rig_open(&r, true);
	open_cat(&r.app, WF_CAT_KEYBOARD);
	into(&r.app);
	CHECK(screen_has(&r.app, 100, 30, "Profile") &&
	        screen_has(&r.app, 100, 30, "Standard"));
	CHECK(WF_PROFILE_LEGACY == r.app.settings.draft.keys.profile);
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_PROFILE_NANO == r.app.settings.draft.keys.profile &&
	        screen_has(&r.app, 100, 30, "Nano"));
	/* the list shows the commands and the keys they have */
	CHECK(screen_has(&r.app, 100, 30, "File manager") &&
	        screen_has(&r.app, 100, 30, "Move the cursor down"));
	/* Enter changes a row with a value; the arrows never do */
	key(&r.app, WF_KEY_RIGHT);
	CHECK(WF_PROFILE_NANO == r.app.settings.draft.keys.profile &&
	        WF_FOCUS_DETAILS == r.app.settings.focus);
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_PROFILE_VIM == r.app.settings.draft.keys.profile);
	key(&r.app, WF_KEY_LEFT); /* back to the entries, nothing changed */
	CHECK(WF_PROFILE_VIM == r.app.settings.draft.keys.profile &&
	        WF_FOCUS_CATEGORY == r.app.settings.focus);
	key(&r.app, WF_KEY_RIGHT);
	CHECK(WF_FOCUS_DETAILS == r.app.settings.focus);
	key(&r.app, WF_KEY_ENTER); /* Custom starts from the keys in use */
	CHECK(WF_PROFILE_CUSTOM == r.app.settings.draft.keys.profile &&
	        WF_BASE_VIM == r.app.settings.draft.keys.base);
	CHECK(screen_has(&r.app, 100, 30, "Custom, based on Vim") &&
	        screen_has(&r.app, 100, 30, "Restore all default keys"));
	key(&r.app, WF_KEY_ENTER); /* and round again */
	CHECK(WF_PROFILE_LEGACY == r.app.settings.draft.keys.profile);
	/* nothing is saved or in force until Ctrl+S */
	CHECK(0 == r.port.saves);
	rig_close(&r);
}

static void a_command_is_given_a_key_and_the_key_is_checked(void)
{
	static Rig r;
	const WfKeyOverride *o;

	rig_open(&r, true);
	open_cat(&r.app, WF_CAT_KEYBOARD);
	into(&r.app);
	key(&r.app, WF_KEY_ENTER); /* Nano */
	key(&r.app, WF_KEY_ENTER); /* Vim */
	key(&r.app, WF_KEY_ENTER); /* Custom (on Vim) */
	CHECK(WF_PROFILE_CUSTOM == r.app.settings.draft.keys.profile);
	find_command(&r.app, "fm.nav.down");
	CHECK(1 == r.app.settings.item);

	/* waiting for a key: Esc cancels, nothing is bound */
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_SS_CAPTURE == r.app.settings.sub &&
	        screen_has(&r.app, 100, 30, "nothing is run"));
	key(&r.app, WF_KEY_ESC);
	CHECK(WF_SS_BROWSE == r.app.settings.sub &&
	        &WF_MODE_SETTINGS == r.app.mode &&
	        !r.app.settings.note_is_error &&
	        NULL == override_of(&r.app, WF_CTX_FM, WF_FM_NAV_DOWN));

	/* a key that another command has is refused, and says which */
	key(&r.app, WF_KEY_ENTER);
	ch(&r.app, 'k');
	CHECK(WF_SS_BROWSE == r.app.settings.sub &&
	        r.app.settings.note_is_error &&
	        strstr(r.app.settings.note, "Move the cursor up"));
	CHECK(NULL == override_of(&r.app, WF_CTX_FM, WF_FM_NAV_DOWN));

	/* keys that would do something do nothing while they are captured */
	key(&r.app, WF_KEY_ENTER);
	ch(&r.app, 'q');
	CHECK(&WF_MODE_SETTINGS == r.app.mode &&
	        NULL == override_of(&r.app, WF_CTX_FM, WF_FM_NAV_DOWN));
	key(&r.app, WF_KEY_ENTER);
	key(&r.app, WF_KEY_DELETE);
	CHECK(&WF_MODE_SETTINGS == r.app.mode &&
	        WF_SS_BROWSE == r.app.settings.sub);

	/* a key the terminal cannot tell apart is refused with the reason */
	key(&r.app, WF_KEY_ENTER);
	{
		WfEvent e = {WF_EV_CTRL, 'i', WF_KEY_NONE, 0};

		r.app.mode->on_event(&r.app, &e);
	}
	CHECK(r.app.settings.note_is_error &&
	        strstr(r.app.settings.note, "Tab"));

	/* Ctrl+C reaches the screen as a key while waiting (the program keeps
	 * running) and is refused for the file manager with its reason */
	key(&r.app, WF_KEY_ENTER);
	{
		WfEvent e = {WF_EV_CTRL, 'c', WF_KEY_NONE, 0};

		CHECK(WF_ACT_REDRAW == r.app.mode->on_event(&r.app, &e));
	}
	CHECK(&WF_MODE_SETTINGS == r.app.mode && r.app.settings.note_is_error &&
	        strstr(r.app.settings.note, "interrupt"));

	/* a free key is taken */
	key(&r.app, WF_KEY_ENTER);
	ch(&r.app, 'n');
	o = override_of(&r.app, WF_CTX_FM, WF_FM_NAV_DOWN);
	CHECK(o && 1 == o->n && !r.app.settings.note_is_error);
	CHECK(screen_has(&r.app, 100, 30, "n *"));

	/* it keeps listening for a second key: the next one is added */
	CHECK(WF_SS_CAPTURE == r.app.settings.sub &&
	        screen_has(&r.app, 100, 30, "second key"));
	key(&r.app, WF_KEY_DOWN);
	o = override_of(&r.app, WF_CTX_FM, WF_FM_NAV_DOWN);
	CHECK(o && 2 == o->n && WF_SS_BROWSE == r.app.settings.sub);
	/* the keys can also be typed as text */
	ch(&r.app, 'e');
	CHECK(WF_SS_TEXT == r.app.settings.sub &&
	        !strcmp(r.app.settings.input, "n, Down"));
	key(&r.app, WF_KEY_ENTER);
	o = override_of(&r.app, WF_CTX_FM, WF_FM_NAV_DOWN);
	CHECK(o && 2 == o->n && WF_SS_BROWSE == r.app.settings.sub);
	/* text that is not keys is refused and stays to be corrected */
	ch(&r.app, 'e');
	type(&r.app, ", Hyper+Bogus");
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_SS_TEXT == r.app.settings.sub && r.app.settings.note_is_error);
	key(&r.app, WF_KEY_ESC);
	CHECK(WF_SS_BROWSE == r.app.settings.sub);

	/* nothing is in force until it is saved */
	CHECK(WF_PROFILE_LEGACY == r.config.keys.profile && 0 == r.port.saves);
	ctrl(&r.app, 's');
	CHECK(1 == r.port.saves);
	CHECK(!strcmp(saved(&r.port, "keyboard", "profile"), "custom"));
	CHECK(!strcmp(saved(&r.port, "keyboard.custom", "base"), "vim"));
	CHECK(!strcmp(saved(&r.port, "keyboard.custom.fm", "fm.nav.down"),
	        "n, Down"));
	CHECK(WF_PROFILE_CUSTOM == r.config.keys.profile);
	CHECK(WF_PROFILE_CUSTOM == wf_keymap_profile(wf_app_keymap(&r.app)));
	{
		WfKeyState st;
		WfStroke s = {'n', 0, 0};
		WfKeyResult res;

		wf_keystate_reset(&st);
		res = wf_keymap_feed(
		        wf_app_keymap(&r.app), WF_CTX_FM, &st, s, 0);
		CHECK(WF_KR_COMMAND == res.verdict &&
		        WF_FM_NAV_DOWN == res.cmd);
	}

	/* reset this command: back to the profile's keys */
	r.app.settings.item = 1;
	ch(&r.app, 'r');
	CHECK(NULL == override_of(&r.app, WF_CTX_FM, WF_FM_NAV_DOWN));
	rig_close(&r);
}

static void restoring_all_keys_asks_first(void)
{
	static Rig r;

	rig_open(&r, true);
	open_cat(&r.app, WF_CAT_KEYBOARD);
	into(&r.app);
	key(&r.app, WF_KEY_ENTER);
	key(&r.app, WF_KEY_ENTER);
	key(&r.app, WF_KEY_ENTER); /* Custom (on Vim) */
	find_command(&r.app, "fm.nav.down");
	key(&r.app, WF_KEY_ENTER);
	ch(&r.app, 'n');
	key(&r.app, WF_KEY_ESC); /* enough with one key */
	CHECK(1 == r.app.settings.draft.keys.count &&
	        WF_SS_BROWSE == r.app.settings.sub);
	/* clear the search, then the "restore all" row */
	ch(&r.app, '/');
	key(&r.app, WF_KEY_BACKSPACE);
	key(&r.app, WF_KEY_BACKSPACE);
	for (int i = 0; i < 20; i++) {
		key(&r.app, WF_KEY_BACKSPACE);
	}
	key(&r.app, WF_KEY_ENTER);
	CHECK('\0' == r.app.settings.search[0]);
	key(&r.app, WF_KEY_HOME);
	down(&r.app, 2);
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_SS_RESET_ALL == r.app.settings.sub &&
	        screen_has(&r.app, 100, 30, "y yes"));
	ch(&r.app, 'n');
	CHECK(1 == r.app.settings.draft.keys.count);
	key(&r.app, WF_KEY_ENTER);
	ch(&r.app, 'y');
	CHECK(0 == r.app.settings.draft.keys.count &&
	        WF_SS_BROWSE == r.app.settings.sub);
	rig_close(&r);
}

static void any_profile_can_be_changed_and_becomes_custom(void)
{
	static Rig r;
	const WfKeyOverride *o;

	/* Vim, then only the Recycle Bin key is moved: no need to go to Custom
	 * first */
	rig_open(&r, true);
	open_cat(&r.app, WF_CAT_KEYBOARD);
	into(&r.app);
	key(&r.app, WF_KEY_ENTER);
	key(&r.app, WF_KEY_ENTER); /* Nano, Vim */
	CHECK(WF_PROFILE_VIM == r.app.settings.draft.keys.profile);
	find_command(&r.app, "fm.recycle");
	CHECK(1 == r.app.settings.item);
	key(&r.app, WF_KEY_ENTER);
	ch(&r.app, 'X');
	o = override_of(&r.app, WF_CTX_FM, WF_FM_RECYCLE);
	CHECK(o && 1 == o->n);
	CHECK(WF_PROFILE_CUSTOM == r.app.settings.draft.keys.profile &&
	        WF_BASE_VIM == r.app.settings.draft.keys.base);
	CHECK(strstr(r.app.settings.note, "now Custom"));
	key(&r.app, WF_KEY_ESC); /* the first key stays; no second one */
	/* the cursor is still on the command (the Base and Restore rows came
	 * in above it) */
	{
		WfSetRow row;

		WF_SETPAGE_KEYS.row(&r.app, r.app.settings.item, &row);
		CHECK(strstr(row.label, "Recycle Bin") &&
		        strstr(row.value, "X"));
	}
	ctrl(&r.app, 's');
	CHECK(!strcmp(saved(&r.port, "keyboard", "profile"), "custom"));
	CHECK(!strcmp(saved(&r.port, "keyboard.custom", "base"), "vim"));
	CHECK(!strcmp(saved(&r.port, "keyboard.custom.fm", "fm.recycle"), "X"));
	rig_close(&r);

	/* from Standard too (on the Nano keys); Esc during capture changes
	 * nothing, not even the profile */
	rig_open(&r, true);
	open_cat(&r.app, WF_CAT_KEYBOARD);
	into(&r.app);
	find_command(&r.app, "fm.quit");
	key(&r.app, WF_KEY_ENTER);
	key(&r.app, WF_KEY_ESC);
	CHECK(WF_PROFILE_LEGACY == r.app.settings.draft.keys.profile);
	key(&r.app, WF_KEY_ENTER);
	ch(&r.app, 'Q');
	CHECK(WF_PROFILE_CUSTOM == r.app.settings.draft.keys.profile &&
	        WF_BASE_NANO == r.app.settings.draft.keys.base);
	rig_close(&r);
}

static void keys_are_grouped_by_what_they_do(void)
{
	static Rig r;

	rig_open(&r, true);
	open_cat(&r.app, WF_CAT_KEYBOARD);
	into(&r.app);
	CHECK(screen_has(&r.app, 100, 40, "File manager > Move around"));
	CHECK(screen_has(&r.app, 100, 40, "File manager > Select"));
	CHECK(screen_has(&r.app, 100, 40, "File manager > File operations"));
	/* PgDn goes from group to group, PgUp back */
	{
		size_t a = r.app.settings.item;

		key(&r.app, WF_KEY_PGDN);
		CHECK(r.app.settings.item > a);
		key(&r.app, WF_KEY_PGUP);
		CHECK(r.app.settings.item <= a + 1);
	}
	CHECK(screen_has(&r.app, 100, 40, "Move around"));
	/* a search shows only what matches, still under its group */
	find_command(&r.app, "fm.recycle");
	CHECK(screen_has(&r.app, 100, 40, "File operations") &&
	        !screen_has(&r.app, 100, 40, "Move around"));
	rig_close(&r);
}

/* Left must always be able to leave the details. It used to step the value of
 * every row that had one, so on those rows the way back was Tab only. */
static void arrows_move_between_columns_and_never_change_a_value(void)
{
	static Rig r;

	rig_open(&r, true);
	open_cat(&r.app, WF_CAT_FILES);
	into(&r.app);
	key(&r.app, WF_KEY_RIGHT); /* Show hidden: Right does nothing */
	CHECK(0 == r.app.settings.draft.show_hidden &&
	        WF_FOCUS_DETAILS == r.app.settings.focus);
	key(&r.app, WF_KEY_ENTER); /* Enter switches it */
	CHECK(1 == r.app.settings.draft.show_hidden);
	key(&r.app, WF_KEY_LEFT); /* Left goes back, leaving the value alone */
	CHECK(1 == r.app.settings.draft.show_hidden &&
	        WF_FOCUS_CATEGORY == r.app.settings.focus);
	key(&r.app, WF_KEY_RIGHT);
	down(&r.app, 1);           /* Sort by: a choice */
	key(&r.app, WF_KEY_RIGHT); /* opens its list, as Enter does */
	CHECK(WF_SS_PICK == r.app.settings.sub &&
	        WF_CONFIG_SORT_NAME == r.app.settings.draft.sort_by);
	key(&r.app, WF_KEY_DOWN);
	key(&r.app, WF_KEY_ESC); /* Esc closes the list only: nothing changed */
	CHECK(WF_SS_BROWSE == r.app.settings.sub &&
	        &WF_MODE_SETTINGS == r.app.mode &&
	        WF_CONFIG_SORT_NAME == r.app.settings.draft.sort_by);
	key(&r.app, WF_KEY_LEFT);
	CHECK(WF_FOCUS_CATEGORY == r.app.settings.focus);
	rig_close(&r);
}

static void a_preset_is_duplicated_before_it_is_edited(void)
{
	static Rig r;
	const WfCustomEntry *e;

	rig_open(&r, true);
	open_cat(&r.app, WF_CAT_APPEARANCE);
	into(&r.app);
	/* the presets cannot be edited */
	ch(&r.app, 'e');
	CHECK(0 == r.app.settings.draft.custom_count &&
	        r.app.settings.note_is_error &&
	        strstr(r.app.settings.note, "cannot be edited"));
	ch(&r.app, 'x');
	CHECK(WF_SS_BROWSE == r.app.settings.sub);
	/* duplicate Spade: a custom theme, shown at once */
	ch(&r.app, 'd');
	CHECK(1 == r.app.settings.draft.custom_count &&
	        0 == r.app.settings.edit_theme);
	e = &r.app.settings.draft.customs[0];
	CHECK(!strcmp(e->id, "spade-copy") && !strcmp(e->name, "Spade copy") &&
	        WF_THEME_SPADE == e->theme.base);
	CHECK(WF_THEME_CUSTOM == r.app.settings.draft.theme);
	CHECK(screen_has(&r.app, 100, 30, "Background") &&
	        screen_has(&r.app, 100, 30, "Success") &&
	        screen_has(&r.app, 100, 30, "#0F172A"));
	/* a colour is typed, and shows at once */
	down(&r.app, 0);
	r.app.settings.item = 2; /* Background */
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_SS_TEXT == r.app.settings.sub &&
	        !strcmp(r.app.settings.input, "#0F172A"));
	for (int i = 0; i < 7; i++) {
		key(&r.app, WF_KEY_BACKSPACE);
	}
	type(&r.app, "#12");
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_SS_TEXT == r.app.settings.sub &&
	        r.app.settings.note_is_error &&
	        strstr(r.app.settings.note, "Not a color"));
	type(&r.app, "zz g!3456");
	CHECK(!strcmp(r.app.settings.input, "#123456"));
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_SS_BROWSE == r.app.settings.sub &&
	        0x123456 ==
	                r.app.settings.draft.customs[0]
	                        .theme.color[WF_CUSTOM_BACKGROUND]);
	CHECK(0x123456 == wf_theme_active()->background);
	CHECK(0 == r.config.custom_count); /* not saved */
	/* the three semantic colours are theirs to change */
	r.app.settings.item = 2 + WF_CUSTOM_ERROR;
	key(&r.app, WF_KEY_ENTER);
	for (int i = 0; i < 7; i++) {
		key(&r.app, WF_KEY_BACKSPACE);
	}
	type(&r.app, "#FF0000");
	key(&r.app, WF_KEY_ENTER);
	CHECK(0xFF0000 == wf_theme_active()->error_foreground);
	/* Delete puts a colour back to the base theme's */
	key(&r.app, WF_KEY_DELETE);
	CHECK(-1 ==
	        r.app.settings.draft.customs[0].theme.color[WF_CUSTOM_ERROR]);
	/* invisible text is warned about and can be taken back */
	r.app.settings.item = 2 + WF_CUSTOM_FOREGROUND;
	key(&r.app, WF_KEY_ENTER);
	for (int i = 0; i < 7; i++) {
		key(&r.app, WF_KEY_BACKSPACE);
	}
	type(&r.app, "#123457");
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_SS_WARN == r.app.settings.sub &&
	        strstr(r.app.settings.note, "Low contrast"));
	ch(&r.app, 'n');
	CHECK(WF_SS_BROWSE == r.app.settings.sub &&
	        -1 ==
	                r.app.settings.draft.customs[0]
	                        .theme.color[WF_CUSTOM_FOREGROUND]);
	/* rename */
	ch(&r.app, 'n');
	CHECK(WF_SS_TEXT == r.app.settings.sub &&
	        !strcmp(r.app.settings.input, "Spade copy"));
	for (int i = 0; i < 12; i++) {
		key(&r.app, WF_KEY_BACKSPACE);
	}
	key(&r.app, WF_KEY_ENTER); /* empty: refused */
	CHECK(WF_SS_TEXT == r.app.settings.sub && r.app.settings.note_is_error);
	type(&r.app, "Night ");
	ch(&r.app, 0x591C); /* one character, three bytes */
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_SS_BROWSE == r.app.settings.sub &&
	        !strcmp(r.app.settings.draft.customs[0].name,
	                "Night \xE5\xA4\x9C"));
	/* the field edits in the middle: Home, Left/Right by character, Delete */
	ch(&r.app, 'n');
	CHECK(WF_SS_TEXT == r.app.settings.sub);
	key(&r.app, WF_KEY_HOME);
	key(&r.app, WF_KEY_DELETE);
	CHECK(!strcmp(r.app.settings.input, "ight \xE5\xA4\x9C"));
	key(&r.app, WF_KEY_END);
	key(&r.app, WF_KEY_LEFT); /* over the three-byte character at once */
	CHECK(r.app.settings.input_cur == strlen("ight "));
	type(&r.app, "X");
	CHECK(!strcmp(r.app.settings.input, "ight X\xE5\xA4\x9C"));
	key(&r.app, WF_KEY_BACKSPACE);
	key(&r.app, WF_KEY_RIGHT);
	key(&r.app,
	        WF_KEY_BACKSPACE); /* the whole character, not a byte of it */
	CHECK(!strcmp(r.app.settings.input, "ight "));
	key(&r.app, WF_KEY_ESC);
	CHECK(WF_SS_BROWSE == r.app.settings.sub);
	/* back to the list: Left, and the theme is in it and in use */
	key(&r.app, WF_KEY_LEFT);
	CHECK(-1 == r.app.settings.edit_theme &&
	        WF_FOCUS_DETAILS == r.app.settings.focus);
	CHECK(screen_has(&r.app, 100, 30, "Night") &&
	        screen_has(&r.app, 100, 30, "custom *"));
	ctrl(&r.app, 's');
	CHECK(1 == r.port.saves);
	CHECK(!strcmp(saved(&r.port, "appearance", "theme"), "custom"));
	CHECK(!strcmp(
	        saved(&r.port, "appearance", "custom_theme"), "spade-copy"));
	CHECK(!strcmp(
	        saved(&r.port, "theme.custom.spade-copy", "base"), "spade"));
	CHECK(!strcmp(saved(&r.port, "theme.custom.spade-copy", "background"),
	        "#123456"));
	CHECK(NULL == saved(&r.port, "theme.custom.spade-copy", "foreground"));
	CHECK(WF_THEME_CUSTOM == r.config.theme && 1 == r.config.custom_count &&
	        0x123456 == wf_theme_active()->background);
	rig_close(&r);
}

static void cancelling_theme_edits_puts_everything_back(void)
{
	static Rig r;

	rig_open(&r, true);
	open_cat(&r.app, WF_CAT_APPEARANCE);
	into(&r.app);
	ch(&r.app, 'd');
	r.app.settings.item = 2;
	key(&r.app, WF_KEY_ENTER);
	/* A full replacement previews before Enter; Esc restores inheritance. */
	type(&r.app, "#142030");
	CHECK(0x142030 == wf_theme_active()->background);
	CHECK(0 == r.config.custom_count && 0 == r.port.saves);
	key(&r.app, WF_KEY_ESC);
	CHECK(0x0F172A == wf_theme_active()->background &&
	        r.app.settings.draft.customs[0]
	                        .theme.color[WF_CUSTOM_BACKGROUND] == -1);
	key(&r.app, WF_KEY_ENTER);
	for (int i = 0; i < 7; i++) {
		key(&r.app, WF_KEY_BACKSPACE);
	}
	type(&r.app, "#0B1020");
	key(&r.app, WF_KEY_ENTER);
	CHECK(0x0B1020 == wf_theme_active()->background);
	key(&r.app, WF_KEY_ESC); /* out of the colours, back to the list */
	CHECK(WF_SS_BROWSE == r.app.settings.sub &&
	        -1 == r.app.settings.edit_theme);
	key(&r.app, WF_KEY_ESC); /* asks */
	CHECK(WF_SS_DISCARD == r.app.settings.sub);
	ch(&r.app, 'd');
	CHECK(&WF_MODE_NORMAL == r.app.mode &&
	        0x0F172A == wf_theme_active()->background);
	CHECK(WF_THEME_SPADE == r.config.theme && 0 == r.config.custom_count &&
	        0 == r.port.saves);
	rig_close(&r);
}

static void a_custom_theme_is_deleted_after_asking(void)
{
	static Rig r;
	WfCustomTheme t;

	rig_open(&r, true);
	wf_custom_init(&t);
	t.base = WF_THEME_KING;
	CHECK(wf_config_custom_add(&r.config, "mine", "Mine", &t));
	r.config.theme = WF_THEME_CUSTOM;
	snprintf(r.config.custom_theme, sizeof r.config.custom_theme, "mine");
	open_cat(&r.app, WF_CAT_APPEARANCE);
	into(&r.app);
	/* the cursor starts on the theme in use: Mine */
	CHECK(WF_THEME_PRESET_COUNT == r.app.settings.item);
	CHECK(screen_has(&r.app, 100, 30, "Mine"));
	ch(&r.app, 'x');
	CHECK(WF_SS_DELETE == r.app.settings.sub &&
	        screen_has(&r.app, 100, 30, "Delete this theme"));
	ch(&r.app, 'n');
	CHECK(1 == r.app.settings.draft.custom_count);
	ch(&r.app, 'x');
	ch(&r.app, 'y');
	CHECK(0 == r.app.settings.draft.custom_count &&
	        WF_THEME_KING == r.app.settings.draft.theme); /* its base */
	ctrl(&r.app, 's');
	CHECK(1 == r.port.saves && 0 == r.config.custom_count &&
	        WF_THEME_KING == r.config.theme);
	rig_close(&r);
}

static void licenses_and_about_show_real_texts(void)
{
	static Rig r;
	char license[4096];
	FILE *f = NULL;
	CHECK(0 == fopen_s(&f, "LICENSE", "rb"));
	if (!f) {
		return;
	}
	size_t license_n = fread(license, 1, sizeof license - 1, f);
	CHECK(feof(f) && !ferror(f));
	fclose(f);
	license[license_n] = '\0';

	rig_open(&r, true);
	wf_app_set_license_dir(&r.app, L"LICENSES");
	ch(&r.app, ',');
	key(&r.app, WF_KEY_LEFT);
	key(&r.app, WF_KEY_DOWN); /* Licenses */
	key(&r.app, WF_KEY_RIGHT);
	CHECK(screen_has(&r.app, 100, 30, "piero") &&
	        screen_has(&r.app, 100, 30, "zlib-1.3.1"));
	key(&r.app, WF_KEY_RIGHT);
	CHECK(r.app.settings.doc_n == license_n &&
	        0 == memcmp(r.app.settings.doc, license, license_n));
	CHECK(screen_has(&r.app, 100, 30, "MIT License"));
	key(&r.app, WF_KEY_END);
	CHECK(r.app.settings.top > 0 &&
	        screen_has(&r.app, 100, 30, "SOFTWARE."));
	size_t bottom = r.app.settings.top;
	key(&r.app, WF_KEY_PGUP);
	CHECK(r.app.settings.top < bottom);
	key(&r.app, WF_KEY_HOME);
	CHECK(0 == r.app.settings.top &&
	        screen_has(&r.app, 100, 30, "MIT License"));
	key(&r.app, WF_KEY_LEFT);
	/* Read the unchanged upstream notice, not a translated substitute. */
	for (int i = 0; i < r.app.settings.lic_n; i++) {
		if (0 == strcmp(r.app.settings.lic_name[i], "zlib-1.3.1")) {
			for (int k = 0; k <= i; k++) {
				key(&r.app, WF_KEY_DOWN);
			}
			break;
		}
	}
	key(&r.app, WF_KEY_RIGHT);
	CHECK(strstr(r.app.settings.doc, "Jean-loup Gailly") &&
	        strstr(r.app.settings.doc, "Mark Adler"));
	key(&r.app, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.app.mode);

	/* About is a quiet project page followed by the full legal text. */
	r.app.settings_mem.valid =
	        false; /* this part walks from the first row */
	ch(&r.app, ',');
	key(&r.app, WF_KEY_LEFT);
	key(&r.app, WF_KEY_DOWN);
	key(&r.app, WF_KEY_DOWN);
	key(&r.app, WF_KEY_RIGHT);
	CHECK(3 == wf_about_count(&r.app, WF_SEC_ABOUT));
	CHECK(screen_has(&r.app, 100, 30, "piero") &&
	        screen_has(&r.app, 100, 30, "MIT License") &&
	        screen_has(&r.app, 100, 30, "Support piero"));
	CHECK(strstr(r.app.settings.doc, "https://x.com/dev_cyan"));
	CHECK(!screen_has(&r.app, 100, 30, "Compiler") &&
	        !screen_has(&r.app, 100, 30, "License status") &&
	        !screen_has(&r.app, 100, 30, "Release status") &&
	        !strstr(r.app.settings.doc, "libarchive"));
	key(&r.app, WF_KEY_DOWN);
	key(&r.app, WF_KEY_RIGHT);
	CHECK(r.app.settings.doc_n == license_n &&
	        0 == memcmp(r.app.settings.doc, license, license_n));
	key(&r.app, WF_KEY_END);
	CHECK(screen_has(&r.app, 100, 30, "SOFTWARE."));
	key(&r.app, WF_KEY_ESC);

	/* in Japanese, the same screens */
	wf_i18n_set_language(WF_UILANG_JA);
	r.app.settings_mem.valid =
	        false; /* this part walks from the first row */
	ch(&r.app, ',');
	CHECK(screen_has(&r.app, 100, 30, "ライセンス") &&
	        screen_has(&r.app, 100, 30, "キーボード"));
	key(&r.app, WF_KEY_LEFT);
	key(&r.app, WF_KEY_DOWN);
	key(&r.app, WF_KEY_DOWN);
	key(&r.app, WF_KEY_RIGHT);
	CHECK(screen_has(&r.app, 100, 30, "pieroを支援する"));
	CHECK(strstr(r.app.settings.doc, wf_msg(MSG_ABOUT_SUPPORT_DM)) &&
	        strstr(r.app.settings.doc, "https://x.com/dev_cyan"));
	key(&r.app, WF_KEY_DOWN);
	key(&r.app, WF_KEY_RIGHT);
	CHECK(r.app.settings.doc_n == license_n &&
	        0 == memcmp(r.app.settings.doc, license, license_n));
	key(&r.app, WF_KEY_END);
	CHECK(screen_has(&r.app, 100, 30, "SOFTWARE."));
	key(&r.app, WF_KEY_ESC);
	wf_i18n_set_language(WF_UILANG_EN);
	rig_close(&r);
}

/* Every state of the settings screen draws at every size without trouble. */
static void settings_draw_at_any_size(void)
{
	static Rig r;
	static const int sizes[][2] = {{1, 1}, {2, 2}, {10, 3}, {20, 6},
	        {30, 8}, {40, 12}, {63, 20}, {64, 20}, {80, 24}, {120, 40},
	        {300, 80}};
	static const WfSettingsSub subs[] = {WF_SS_BROWSE, WF_SS_TEXT,
	        WF_SS_CAPTURE, WF_SS_WARN, WF_SS_DELETE, WF_SS_RESET_ALL,
	        WF_SS_DISCARD};
	WfCustomTheme t;

	rig_open(&r, true);
	wf_custom_init(&t);
	t.color[1] = 0x445566;
	CHECK(wf_config_custom_add(&r.config, "mine", "Mine", &t));
	wf_app_set_license_dir(&r.app, L"C:\\no\\such\\folder");
	r.app.settings_mem.valid =
	        false; /* this part walks from the first row */
	ch(&r.app, ',');
	r.app.settings.draft.keys.profile = WF_PROFILE_CUSTOM;
	snprintf(r.app.settings.note, sizeof r.app.settings.note, "%s",
	        "a long note that does not fit a small screen at all, really");
	for (int lang = 0; lang < 2; lang++) {
		wf_i18n_set_language(lang ? WF_UILANG_JA : WF_UILANG_EN);
		for (int sec = 0; sec < WF_SEC_COUNT; sec++) {
			for (int focus = 0; focus < 3; focus++) {
				for (size_t si = 0; si < WF_ARRAY_LEN(subs);
				        si++) {
					for (int cat = 0; cat < WF_CAT_COUNT;
					        cat++) {
						for (size_t z = 0;
						        z < WF_ARRAY_LEN(sizes);
						        z++) {
							r.app.settings.section =
							        sec;
							r.app.settings.focus =
							        focus;
							r.app.settings.sub =
							        subs[si];
							r.app.settings
							        .category = cat;
							r.app.settings.item =
							        (size_t)cat;
							r.app.settings
							        .edit_theme =
							        cat & 1 ? 0
							                : -1;
							snprintf(r.app.settings
							                 .input,
							        sizeof r.app
							                .settings
							                .input,
							        "#123456");
							r.app.settings.input_n =
							        7;
							(void)screen_has(&r.app,
							        sizes[z][0],
							        sizes[z][1],
							        "x");
						}
					}
				}
			}
		}
	}
	wf_i18n_set_language(WF_UILANG_EN);
	rig_close(&r);
}

static void reload_applies_services_and_names_stay_utf8(void)
{
	static Rig r;
	rig_open(&r, true);
	r.port.reload_to = r.config;
	r.port.reload_to.show_hidden = 1;
	r.port.reload_to.dir_preview = 0;
	wf_app_settings_begin(&r.app);
	ch(&r.app, 'r');
	CHECK(r.config.show_hidden && r.app.cur.show_hidden &&
	        r.app.parent.show_hidden);
	CHECK(!r.config.dir_preview && r.app.dirprev_off);
	key(&r.app, WF_KEY_ESC);
	wf_app_toggle_hidden(&r.app);
	CHECK(r.config.show_hidden == r.app.cur.show_hidden);
	CHECK(wf_config_custom_add(&r.config, "one",
	        "a\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5"
	        "\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5",
	        NULL));
	wf_app_settings_begin(&r.app);
	r.app.settings.category = WF_CAT_APPEARANCE;
	r.app.settings.focus = WF_FOCUS_DETAILS;
	r.app.settings.item = WF_THEME_PRESET_COUNT;
	WfEvent ev = {WF_EV_CHAR, 'd', WF_KEY_NONE, 0};
	WF_SETPAGE_THEMES.event(&r.app, &ev);
	CHECK(r.app.settings.draft.custom_count == 2);
	const char *name = r.app.settings.draft.customs[1].name;
	CHECK(wf_utf8_strict(name, strlen(name), NULL));
	rig_close(&r);

	rig_open(&r, true);
	r.config.keys.profile = WF_PROFILE_VIM;
	open_cat(&r.app, WF_CAT_EDITOR);
	into(&r.app);
	int before = r.app.settings.draft.editor_keymap;
	key(&r.app, WF_KEY_ENTER);
	CHECK(before == r.app.settings.draft.editor_keymap);
	CHECK(strstr(r.app.settings.note, "profile") != NULL);
	rig_close(&r);
}

int main(int argc, char **argv)
{
	static void (*const tests[])(void) = {
	        reload_applies_services_and_names_stay_utf8,
	        settings_comes_back_where_it_was_left,
	        the_first_run_screen_shows_the_logo_and_the_eight_themes,
	        moving_previews_every_theme_and_saves_nothing,
	        enter_chooses_and_saves,
	        esc_uses_the_default_and_still_records_the_setup,
	        a_failed_save_does_not_block_the_start,
	        running_setup_again_keeps_the_current_theme_on_esc,
	        small_screens_get_smaller_layouts,
	        the_settings_key_opens_three_columns_only_with_a_configuration,
	        the_arrows_move_between_the_columns,
	        changing_the_theme_previews_and_cancelling_restores_it,
	        the_unsaved_question_is_answered_without_letters,
	        ctrl_s_saves_only_what_changed,
	        a_change_of_the_order_reloads_the_list,
	        editor_previous_version_policy_is_fixed,
	        newer_schema_settings_are_never_written,
	        a_failed_save_keeps_the_draft_and_says_why,
	        items_change_by_kind_and_other_categories_are_reachable,
	        settings_never_list_what_does_not_exist,
	        the_profile_is_chosen_in_the_keyboard_page,
	        a_command_is_given_a_key_and_the_key_is_checked,
	        restoring_all_keys_asks_first,
	        any_profile_can_be_changed_and_becomes_custom,
	        keys_are_grouped_by_what_they_do,
	        arrows_move_between_columns_and_never_change_a_value,
	        a_preset_is_duplicated_before_it_is_edited,
	        cancelling_theme_edits_puts_everything_back,
	        a_custom_theme_is_deleted_after_asking,
	        licenses_and_about_show_real_texts, settings_draw_at_any_size};
	size_t i, n = WF_ARRAY_LEN(tests);

	setvbuf(stdout, NULL, _IONBF, 0); /* a crash must not eat the report */

	/* an argument runs just that test (for finding one that fails) */
	for (i = 0; i < n; i++) {
		if (argc < 2 || (size_t)atoi(argv[1]) == i) {
			tests[i]();
		}
	}
	return TEST_RESULT();
}

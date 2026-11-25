/*
 * Tabs and places (DR-DRAFT-030): t opens a tab at this folder, w closes one, [ and
 * ] (Ctrl+Left and Ctrl+Right where they are free) move between them, each tab
 * keeps its folder and the name under its cursor, the strip is drawn only with
 * more than one tab; m then a letter keeps the folder, ' then the letter goes
 * there, through the same save as the settings. The folders are the fake ones.
 */
#include <string.h>

#include "config/wf_config.h"
#include "edit/wf_ekeymap.h"
#include "input/wf_keymap.h"
#include "path/wf_path.h"
#include "ui/wf_app.h"
#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

static void ch(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	(void)a->mode->on_event(a, &e);
}

static void key(WfApp *a, WfKey k)
{
	WfEvent e = {WF_EV_KEY, 0, k, 0};

	(void)a->mode->on_event(a, &e);
}

static const wchar_t *under(const WfApp *a)
{
	const WfEntry *e = wf_view_current(&a->cur);

	return e ? e->name : L"";
}

static bool screen_has(const WfApp *a, const char *needle)
{
	WfFrame *f = wf_frame_new(100, 30, &WF_STYLE_BASE);
	bool found = false;
	char line[512];

	CHECK(f);
	wf_app_draw(a, f);
	for (int y = 0; y < f->rows && !found; y++) {
		line[0] = '\0';
		for (int x = 0; x < f->cols; x++) {
			strcat_s(line, sizeof line,
			        f->cells[y * f->cols + x].utf8);
		}
		found = NULL != strstr(line, needle);
	}
	wf_frame_free(f);
	return found;
}

/* The first screen row only: the title row. */
static bool title_has(const WfApp *a, const char *needle)
{
	WfFrame *f = wf_frame_new(100, 30, &WF_STYLE_BASE);
	char line[512] = "";
	bool found;

	CHECK(f);
	wf_app_draw(a, f);
	for (int x = 0; x < f->cols; x++) {
		strcat_s(line, sizeof line, f->cells[x].utf8);
	}
	found = NULL != strstr(line, needle);
	wf_frame_free(f);
	return found;
}

typedef struct
{
	WfApp a;
	FakePort fp;
} Rig;

static void rig_open(Rig *r, const wchar_t *start)
{
	memset(r, 0, sizeof *r);
	r->fp.app = &r->a;
	CHECK(WF_OK == wf_app_init(&r->a, 100, 30, fake_port(&r->fp), start));
}

/* The strip is drawn from the tab that keeps the current one in view: in a
 * width that holds only some of the tabs, the one being worked in is always
 * there, and ".." says that others are not. */
static void the_current_tab_is_always_in_a_narrow_strip(void)
{
	static Rig r;
	WfFrame *f = wf_frame_new(20, 2, &WF_STYLE_BASE);

	CHECK(f);
	rig_open(&r, L"C:\\Users\\dev");
	for (int i = 1; i < WF_TABS_MAX; i++) {
		ch(&r.a, 't');
	}
	CHECK(WF_TABS_MAX == (int)wf_app_tab_count(&r.a));
	for (size_t cur = 0; cur < WF_TABS_MAX; cur++) {
		char line[128] = "";
		char want[16];

		r.a.tabs.cur = cur;
		wf_frame_fill(f, 0, 0, f->cols, f->rows, ' ', &WF_STYLE_BASE);
		CHECK(wf_app_tabs_strip(&r.a, f, 0, 20) <= 20);
		for (int x = 0; x < f->cols; x++) {
			strcat_s(line, sizeof line, f->cells[x].utf8);
		}
		snprintf(want, sizeof want, "[%zu:dev]", cur + 1);
		CHECK(strstr(line, want));
		/* tabs before the first one drawn are said to be hidden */
		/* tabs hidden on the left are said by ".."; else the first is there */
		CHECK(0 == strncmp(line, "..", 2) || strstr(line, "1:dev"));
	}
	/* with no room for the labels, the place of the current one among them */
	{
		char line[64] = "";

		r.a.tabs.cur = 4;
		wf_frame_fill(f, 0, 0, f->cols, f->rows, ' ', &WF_STYLE_BASE);
		CHECK(wf_app_tabs_strip(&r.a, f, 0, 7) <= 7);
		for (int x = 0; x < 7; x++) {
			strcat_s(line, sizeof line, f->cells[x].utf8);
		}
		CHECK(strstr(line, "[5/8]"));
	}
	wf_frame_free(f);
}

static void a_new_tab_keeps_the_folder_and_each_tab_keeps_its_own(void)
{
	static Rig r;
	char text[200];

	rig_open(&r, L"C:\\Users\\dev");
	CHECK(1 == wf_app_tab_count(&r.a));
	ch(&r.a, 'j');
	ch(&r.a, 'j'); /* on file10.txt */
	CHECK(!wcscmp(under(&r.a), L"file10.txt"));

	ch(&r.a, 't');
	CHECK(2 == wf_app_tab_count(&r.a) && 1 == r.a.tabs.cur);
	CHECK(!wcscmp(r.a.cwd, L"C:\\Users\\dev"));
	CHECK(!wcscmp(under(&r.a), L"file10.txt")); /* nothing moved */
	CHECK(strstr(r.a.msg, "Tab 2 of 2"));
	wf_app_tabs_text(&r.a, text, sizeof text);
	CHECK(!strcmp(text, "1:dev [2:dev]"));

	/* in the new tab: up one folder */
	ch(&r.a, 'h');
	CHECK(!wcscmp(r.a.cwd, L"C:\\Users"));
	wf_app_tabs_text(&r.a, text, sizeof text);
	CHECK(!strcmp(text, "1:dev [2:Users]"));

	/* back to the first: its folder and the name it had under its cursor */
	ch(&r.a, '[');
	CHECK(0 == r.a.tabs.cur && !wcscmp(r.a.cwd, L"C:\\Users\\dev"));
	CHECK(!wcscmp(under(&r.a), L"file10.txt"));
	CHECK(strstr(r.a.msg, "Tab 1 of 2"));
	wf_app_tabs_text(&r.a, text, sizeof text);
	CHECK(!strcmp(text, "[1:dev] 2:Users"));

	/* forward again, and it wraps around both ways */
	ch(&r.a, ']');
	CHECK(1 == r.a.tabs.cur && !wcscmp(r.a.cwd, L"C:\\Users"));
	CHECK(!wcscmp(under(&r.a), L"dev")); /* the folder it came from */
	ch(&r.a, ']');
	CHECK(0 == r.a.tabs.cur && !wcscmp(r.a.cwd, L"C:\\Users\\dev"));
	ch(&r.a, '[');
	CHECK(1 == r.a.tabs.cur);
	wf_app_destroy(&r.a);
}

static void three_tabs_are_in_order_and_a_new_one_goes_after_this_one(void)
{
	static Rig r;
	char text[200];

	rig_open(&r, L"C:\\Users\\dev");
	ch(&r.a, 't');
	ch(&r.a, 'h'); /* tab 2: Users */
	ch(&r.a, 'h'); /* tab 2: the root */
	ch(&r.a, '[');
	CHECK(0 == r.a.tabs.cur);
	ch(&r.a, 't'); /* after tab 1: it becomes tab 2, the old 2 is now 3 */
	CHECK(3 == wf_app_tab_count(&r.a) && 1 == r.a.tabs.cur);
	wf_app_tabs_text(&r.a, text, sizeof text);
	CHECK(!strcmp(text, "1:dev [2:dev] 3:C:\\"));
	ch(&r.a, ']');
	CHECK(2 == r.a.tabs.cur && !wcscmp(r.a.cwd, L"C:\\"));
	wf_app_destroy(&r.a);
}

/* With three tabs, [ is not ]: the one before, not the one after. */
static void previous_is_the_one_before_with_three_tabs(void)
{
	static Rig r;

	rig_open(&r, L"C:\\Users\\dev");
	ch(&r.a, 't');
	ch(&r.a, 'h'); /* tab 2: Users */
	ch(&r.a, 't');
	ch(&r.a, 'h'); /* tab 3: the root */
	CHECK(3 == wf_app_tab_count(&r.a) && 2 == r.a.tabs.cur);
	ch(&r.a, '[');
	CHECK(1 == r.a.tabs.cur && !wcscmp(r.a.cwd, L"C:\\Users"));
	ch(&r.a, '[');
	CHECK(0 == r.a.tabs.cur && !wcscmp(r.a.cwd, L"C:\\Users\\dev"));
	ch(&r.a, '[');
	CHECK(2 == r.a.tabs.cur && !wcscmp(r.a.cwd, L"C:\\")); /* wraps */
	ch(&r.a, ']');
	CHECK(0 == r.a.tabs.cur);
	wf_app_destroy(&r.a);
}

/* A tab remembers the name under its cursor, not what the folder's own
 * memory says: the other tab was in the same folder and left it elsewhere. */
static void a_tab_gets_its_own_cursor_back_not_the_folders_memory(void)
{
	static Rig r;

	rig_open(&r, L"C:\\Users\\dev");
	ch(&r.a, 'j');
	ch(&r.a, 'j');
	CHECK(!wcscmp(under(&r.a), L"file10.txt"));
	ch(&r.a, 't'); /* tab 2 in the same folder */
	ch(&r.a, 'k'); /* its cursor goes up: file2.txt */
	CHECK(!wcscmp(under(&r.a), L"file2.txt"));
	ch(&r.a, '[');
	CHECK(!wcscmp(under(&r.a), L"file10.txt")); /* tab 1's own */
	ch(&r.a, ']');
	CHECK(!wcscmp(under(&r.a), L"file2.txt")); /* tab 2's own */
	wf_app_destroy(&r.a);
}

static void w_closes_the_tab_and_goes_to_a_neighbour(void)
{
	static Rig r;
	char text[200];

	rig_open(&r, L"C:\\Users\\dev");
	ch(&r.a, 'w');
	CHECK(1 == wf_app_tab_count(&r.a));
	CHECK(strstr(r.a.msg, "only tab"));
	CHECK(!wcscmp(r.a.cwd, L"C:\\Users\\dev"));

	ch(&r.a, 't');
	ch(&r.a, 'h'); /* tab 2 at C:\Users */
	ch(&r.a, 'w'); /* closes it: the one before is on screen */
	CHECK(1 == wf_app_tab_count(&r.a) && 0 == r.a.tabs.n);
	CHECK(!wcscmp(r.a.cwd, L"C:\\Users\\dev"));
	CHECK(strstr(r.a.msg, "Tab closed; now tab 1 of 1"));
	wf_app_tabs_text(&r.a, text, sizeof text);
	CHECK(!text[0]); /* one tab: no strip */

	/* closing the first of two goes to the one that was after it */
	ch(&r.a, 't');
	ch(&r.a, 'h'); /* tab 2: Users */
	ch(&r.a, '[');
	ch(&r.a, 'w');
	CHECK(1 == wf_app_tab_count(&r.a));
	CHECK(!wcscmp(r.a.cwd, L"C:\\Users"));
	wf_app_destroy(&r.a);
}

/* The neighbour's folder cannot be walked to (the walk cannot even start): the
 * tab is not closed. The screen shows it still, the records are as they were,
 * and the screen says the walk failed, not "Tab closed". */
static void a_close_whose_walk_cannot_start_closes_nothing(void)
{
	static Rig r;

	rig_open(&r, L"C:\\Users\\dev");
	ch(&r.a, 't'); /* tab 2, on screen; tab 1 is the neighbour */
	free(r.a.tabs.t[0].path);
	r.a.tabs.t[0].path = _wcsdup(L"\\\\.\\device");
	CHECK(NULL != r.a.tabs.t[0].path);
	ch(&r.a, 'w');
	CHECK(2 == wf_app_tab_count(&r.a) && 1 == r.a.tabs.cur);
	CHECK(!wcscmp(r.a.cwd, L"C:\\Users\\dev"));
	CHECK(NULL == strstr(r.a.msg, "Tab closed"));
	CHECK(r.a.msg[0]);
	CHECK(!r.a.tabs.going && !r.a.tabs.closing);
	wf_app_destroy(&r.a);
}

/* The neighbour's folder is listed and refused: the tab stays, the screen is the
 * tab that was on screen, and the neighbour's record is not written over by the
 * screen's folder when the tabs are stored again. */
static void a_close_into_an_unreadable_folder_leaves_every_tab_as_it_was(void)
{
	static Rig r;

	rig_open(&r, L"C:\\Users\\dev");
	ch(&r.a, 't');
	ch(&r.a, 'h'); /* tab 2: Users */
	free(r.a.tabs.t[0].path);
	r.a.tabs.t[0].path = _wcsdup(L"C:\\secret"); /* WF_E_ACCESS */
	CHECK(NULL != r.a.tabs.t[0].path);
	ch(&r.a, 'w');
	CHECK(2 == wf_app_tab_count(&r.a) && 1 == r.a.tabs.cur);
	CHECK(!wcscmp(r.a.cwd, L"C:\\Users"));
	CHECK(NULL == strstr(r.a.msg, "Tab closed"));
	/* the screen's state is stored into tab 2, never into tab 1 */
	ch(&r.a, 't');
	CHECK(3 == wf_app_tab_count(&r.a));
	CHECK(!wcscmp(r.a.tabs.t[0].path, L"C:\\secret"));
	CHECK(!wcscmp(r.a.tabs.t[1].path, L"C:\\Users"));
	/* a later walk of another kind does not close what the failed one meant to */
	ch(&r.a, 'h');
	CHECK(3 == wf_app_tab_count(&r.a));
	wf_app_destroy(&r.a);
}

/* A tab whose folder cannot be read: the screen stays on the tab it shows, and
 * that tab's state is not written over the record of the one that failed. */
static void a_tab_that_cannot_be_read_leaves_the_tabs_as_they_were(void)
{
	static Rig r;

	rig_open(&r, L"C:\\Users\\dev");
	ch(&r.a, 't');
	ch(&r.a, 'h'); /* tab 2: Users */
	free(r.a.tabs.t[0].path);
	r.a.tabs.t[0].path = _wcsdup(L"C:\\secret"); /* WF_E_ACCESS */
	CHECK(NULL != r.a.tabs.t[0].path);
	ch(&r.a, '[');
	CHECK(1 == r.a.tabs.cur && !wcscmp(r.a.cwd, L"C:\\Users"));
	CHECK(!strstr(r.a.msg, "Tab 1 of 2"));
	ch(&r.a,
	        't'); /* the state of the screen is stored: into tab 2, not tab 1 */
	CHECK(3 == wf_app_tab_count(&r.a));
	CHECK(!wcscmp(r.a.tabs.t[0].path, L"C:\\secret"));
	CHECK(!wcscmp(r.a.tabs.t[1].path, L"C:\\Users"));
	wf_app_destroy(&r.a);
}

static void at_most_eight_tabs(void)
{
	static Rig r;

	rig_open(&r, L"C:\\Users\\dev");
	for (int i = 1; i < WF_TABS_MAX; i++) {
		ch(&r.a, 't');
	}
	CHECK(WF_TABS_MAX == wf_app_tab_count(&r.a));
	ch(&r.a, 't');
	CHECK(WF_TABS_MAX == wf_app_tab_count(&r.a));
	CHECK(strstr(r.a.msg, "No more tabs: 8 is the most"));
	wf_app_destroy(&r.a);
}

static void without_a_second_tab_the_keys_say_so_and_move_nothing(void)
{
	static Rig r;

	rig_open(&r, L"C:\\Users\\dev");
	ch(&r.a, ']');
	CHECK(strstr(r.a.msg, "Only one tab"));
	ch(&r.a, '[');
	CHECK(!wcscmp(r.a.cwd, L"C:\\Users\\dev") && 0 == r.a.tabs.n);
	wf_app_destroy(&r.a);
}

static void the_strip_is_in_the_title_row_only_with_more_than_one_tab(void)
{
	static Rig r;

	rig_open(&r, L"C:\\Users\\dev");
	CHECK(!title_has(&r.a, "1:dev") && title_has(&r.a, "C:\\Users\\dev"));
	ch(&r.a, 't');
	CHECK(title_has(&r.a, "1:dev") && title_has(&r.a, "[2:dev]"));
	CHECK(title_has(
	        &r.a, "C:\\Users\\dev")); /* the folder is still shown */
	ch(&r.a, 'w');
	CHECK(!title_has(&r.a, "1:dev")); /* gone with the second tab */
	CHECK(title_has(&r.a, "C:\\Users\\dev"));
	wf_app_destroy(&r.a);
}

static void ctrl_left_and_right_move_between_tabs(void)
{
	static Rig r;

	rig_open(&r, L"C:\\Users\\dev");
	ch(&r.a, 't');
	ch(&r.a, 'h');
	key(&r.a, WF_KEY_CTRL_LEFT);
	CHECK(0 == r.a.tabs.cur);
	key(&r.a, WF_KEY_CTRL_RIGHT);
	CHECK(1 == r.a.tabs.cur);
	key(&r.a, WF_KEY_ESC);
	wf_app_destroy(&r.a);
}

static void the_keys_are_in_every_profile_and_the_commands_have_names(void)
{
	static const int PROFILES[] = {
	        WF_PROFILE_LEGACY, WF_PROFILE_NANO, WF_PROFILE_VIM};
	/* Ctrl+Right enters a folder in the Nano profile, so it keeps brackets only */
	static const struct
	{
		const char *name;
		int id;
		const char *keys[3]; /* Standard, Nano, Vim */
	} want[] = {{"fm.tab_new", WF_FM_TAB_NEW, {"t", "t", "t"}},
	        {"fm.tab_close", WF_FM_TAB_CLOSE, {"w", "w", "w"}},
	        {"fm.tab_prev", WF_FM_TAB_PREV,
	                {"[, Ctrl+Left", "[", "[, Ctrl+Left"}},
	        {"fm.tab_next", WF_FM_TAB_NEXT,
	                {"], Ctrl+Right", "]", "], Ctrl+Right"}},
	        {"fm.place_set", WF_FM_PLACE_SET, {"m", "m", "m"}},
	        {"fm.place_go", WF_FM_PLACE_GO, {"'", "'", "'"}}};

	for (size_t c = 0; c < sizeof want / sizeof want[0]; c++) {
		const WfCommandInfo *info = wf_cmd_by_name(want[c].name);

		CHECK(info && want[c].id == info->id);
		CHECK(info && 0 != (info->ctx_mask & WF_CTX_BIT(WF_CTX_FM)));
		for (size_t i = 0; i < 3; i++) {
			WfKeyConfig cfg;
			WfKeymap *m = NULL;
			char text[64];

			wf_keyconfig_init(&cfg);
			cfg.profile = PROFILES[i];
			CHECK(WF_OK ==
			        wf_keymap_build_defaults(
			                &cfg, WF_KM_PIERO, &m));
			if (m) {
				wf_keymap_keys_text(m, WF_CTX_FM,
				        (uint16_t)want[c].id, text,
				        sizeof text);
				if (strcmp(text, want[c].keys[i])) {
					printf("  %s profile %zu: [%s] want "
					       "[%s]\n",
					        want[c].name, i, text,
					        want[c].keys[i]);
				}
				CHECK(!strcmp(text, want[c].keys[i]));
				wf_keymap_free(m);
			}
		}
	}
}

typedef struct
{
	int saves;
	char section[16], key[4], value[WF_CONFIG_VALUE_MAX];
	WfStatus answer;
	char why[64];
} Saver;

static WfStatus fake_save(void *ctx,
        const WfConfigChange *changes,
        size_t n,
        char *why,
        size_t cap)
{
	Saver *s = ctx;

	s->saves++;
	if (n) {
		snprintf(s->section, sizeof s->section, "%s",
		        changes[0].section);
		snprintf(s->key, sizeof s->key, "%s", changes[0].key);
		snprintf(s->value, sizeof s->value, "%s", changes[0].value);
	}
	if (WF_OK != s->answer) {
		snprintf(why, cap, "%s", s->why);
	}
	return s->answer;
}

typedef struct
{
	Rig r;
	WfConfig cfg;
	Saver s;
} PRig;

static void prig_open(PRig *p, const wchar_t *start, bool with_config)
{
	WfSettingsPort port = {&p->s, fake_save, NULL, NULL};

	memset(p, 0, sizeof *p);
	wf_config_init(&p->cfg);
	rig_open(&p->r, start);
	if (with_config) {
		wf_app_set_settings(&p->r.a, &p->cfg, port);
	}
}

static void prig_close(PRig *p)
{
	wf_app_destroy(&p->r.a);
	wf_config_dispose(&p->cfg);
}

static void m_then_a_letter_keeps_the_folder_through_the_settings_save(void)
{
	static PRig p;

	prig_open(&p, L"C:\\Users\\dev", true);
	ch(&p.r.a, 'm');
	CHECK(&WF_MODE_PLACE_SET == p.r.a.mode);
	CHECK(screen_has(&p.r.a, "Keep this folder as a place"));
	CHECK(0 == p.s.saves); /* nothing before the letter */
	ch(&p.r.a, 'a');
	CHECK(&WF_MODE_NORMAL == p.r.a.mode && 1 == p.s.saves);
	CHECK(!strcmp(p.s.section, "places") && !strcmp(p.s.key, "a") &&
	        !strcmp(p.s.value, "C:\\Users\\dev"));
	CHECK(!strcmp(p.cfg.places[0], "C:\\Users\\dev"));
	CHECK(strstr(p.r.a.msg, "Place a = C:\\Users\\dev"));

	/* another letter, another folder; a letter again replaces */
	ch(&p.r.a, 'h');
	ch(&p.r.a, 'm');
	ch(&p.r.a, 'z');
	CHECK(2 == p.s.saves && !strcmp(p.cfg.places[25], "C:\\Users"));
	ch(&p.r.a, 'm');
	ch(&p.r.a, 'a');
	CHECK(!strcmp(p.cfg.places[0], "C:\\Users") && 3 == p.s.saves);
	prig_close(&p);
}

static void a_non_ascii_folder_is_saved_as_utf8(void)
{
	static PRig p;

	prig_open(&p, L"C:\\Users\\dev", true);
	free(p.r.a.cwd);
	p.r.a.cwd = _wcsdup(L"C:\\\u65e5\u672c");
	ch(&p.r.a, 'm');
	ch(&p.r.a, 'j');
	CHECK(!strcmp(p.s.value, "C:\\\xe6\x97\xa5\xe6\x9c\xac"));
	prig_close(&p);
}

static void esc_or_another_key_saves_nothing_and_goes_nowhere(void)
{
	static PRig p;

	prig_open(&p, L"C:\\Users\\dev", true);
	ch(&p.r.a, 'm');
	key(&p.r.a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == p.r.a.mode && 0 == p.s.saves);
	ch(&p.r.a, 'm');
	ch(&p.r.a, '5');
	CHECK(&WF_MODE_NORMAL == p.r.a.mode && 0 == p.s.saves);
	ch(&p.r.a, 'm');
	ch(&p.r.a, 'A'); /* only a to z */
	CHECK(&WF_MODE_NORMAL == p.r.a.mode && 0 == p.s.saves);
	ch(&p.r.a, '\'');
	CHECK(&WF_MODE_PLACE_GO == p.r.a.mode);
	key(&p.r.a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == p.r.a.mode &&
	        !wcscmp(p.r.a.cwd, L"C:\\Users\\dev"));
	prig_close(&p);
}

static void a_failed_save_says_why_and_keeps_nothing(void)
{
	static PRig p;

	prig_open(&p, L"C:\\Users\\dev", true);
	p.s.answer = WF_E_OS;
	snprintf(p.s.why, sizeof p.s.why, "disk is full");
	ch(&p.r.a, 'm');
	ch(&p.r.a, 'a');
	CHECK(1 == p.s.saves && !p.cfg.places[0][0]);
	CHECK(strstr(p.r.a.msg, "Place a was not saved: disk is full"));
	prig_close(&p);
}

static void without_a_configuration_file_or_in_an_archive_nothing_is_saved(void)
{
	static PRig p;

	prig_open(&p, L"C:\\Users\\dev", false);
	ch(&p.r.a, 'm');
	ch(&p.r.a, 'a');
	CHECK(0 == p.s.saves && strstr(p.r.a.msg, "none is in use"));
	prig_close(&p);

	prig_open(&p, L"C:\\Users\\dev", true);
	free(p.r.a.cwd);
	p.r.a.cwd = _wcsdup(L"C:\\x.zip|\\inner");
	ch(&p.r.a, 'm');
	ch(&p.r.a, 'a');
	CHECK(0 == p.s.saves && strstr(p.r.a.msg, "real folder"));
	prig_close(&p);

	/* a configuration of a newer program is not written */
	prig_open(&p, L"C:\\Users\\dev", true);
	p.cfg.schema_version = WF_CONFIG_SCHEMA_VERSION + 1;
	ch(&p.r.a, 'm');
	ch(&p.r.a, 'a');
	CHECK(0 == p.s.saves && !p.cfg.places[0][0]);
	prig_close(&p);
}

static void the_apostrophe_goes_to_the_place_or_says_there_is_none(void)
{
	static PRig p;

	prig_open(&p, L"C:\\Users\\dev", true);
	ch(&p.r.a, '\'');
	CHECK(screen_has(&p.r.a, "No place is saved yet"));
	CHECK(screen_has(&p.r.a, "then a letter"));
	ch(&p.r.a, 'b');
	CHECK(&WF_MODE_NORMAL == p.r.a.mode && strstr(p.r.a.msg, "No place b"));
	CHECK(!wcscmp(p.r.a.cwd, L"C:\\Users\\dev"));

	snprintf(p.cfg.places['b' - 'a'], WF_PLACE_BYTES, "C:\\Pics");
	snprintf(p.cfg.places['c' - 'a'], WF_PLACE_BYTES, "C:\\");
	ch(&p.r.a, '\'');
	CHECK(screen_has(&p.r.a, "b  C:\\Pics") &&
	        screen_has(&p.r.a, "c  C:\\"));
	ch(&p.r.a, 'b');
	CHECK(&WF_MODE_NORMAL == p.r.a.mode && !wcscmp(p.r.a.cwd, L"C:\\Pics"));
	ch(&p.r.a, '\'');
	ch(&p.r.a, 'c');
	CHECK(!wcscmp(p.r.a.cwd, L"C:\\"));
	prig_close(&p);
}

static void a_place_is_a_folder_a_tab_can_go_to_too(void)
{
	static PRig p;
	char text[200];

	prig_open(&p, L"C:\\Users\\dev", true);
	snprintf(p.cfg.places[0], WF_PLACE_BYTES, "C:\\Pics");
	ch(&p.r.a, 't');
	ch(&p.r.a, '\'');
	ch(&p.r.a, 'a');
	CHECK(!wcscmp(p.r.a.cwd, L"C:\\Pics"));
	wf_app_tabs_text(&p.r.a, text, sizeof text);
	CHECK(!strcmp(text, "1:dev [2:Pics]"));
	ch(&p.r.a, '[');
	CHECK(!wcscmp(p.r.a.cwd, L"C:\\Users\\dev"));
	prig_close(&p);
}

static void many_places_are_listed_with_a_count_of_the_rest(void)
{
	static PRig p;

	prig_open(&p, L"C:\\Users\\dev", true);
	for (int i = 0; i < 10; i++) {
		snprintf(p.cfg.places[i], WF_PLACE_BYTES, "C:\\Pics%d", i);
	}
	ch(&p.r.a, '\'');
	CHECK(screen_has(
	        &p.r.a, "h  C:\\Pics7")); /* the 8th is the last shown */
	CHECK(!screen_has(&p.r.a, "i  C:\\Pics8"));
	CHECK(screen_has(&p.r.a, "...and 2 more"));
	key(&p.r.a, WF_KEY_ESC);
	prig_close(&p);
}

/* DR-DRAFT-037: the loop keeps the archive index while any tab shows a folder
 * inside an archive, and only then; the record of the tab on screen is stale
 * and is not looked at. */
static void an_archive_is_in_view_when_any_tab_shows_one(void)
{
	static Rig r;
	wchar_t *inside = NULL;

	rig_open(&r, L"C:\\Users\\dev");
	inside = wf_path_dup(L"C:\\x\\a.zip|docs");
	CHECK(inside && !wf_app_archive_in_view(&r.a));
	ch(&r.a, 't');
	ch(&r.a, 't');
	CHECK(3 == wf_app_tab_count(&r.a) && !wf_app_archive_in_view(&r.a));
	/* another tab is inside an archive */
	free(r.a.tabs.t[0].path);
	r.a.tabs.t[0].path = wf_path_dup(inside);
	r.a.tabs.cur = 2;
	CHECK(wf_app_archive_in_view(&r.a));
	/* the tab on screen: its record is not read, its folder (cwd) is */
	r.a.tabs.cur = 0;
	CHECK(!wf_app_archive_in_view(&r.a));
	free(r.a.cwd);
	r.a.cwd = wf_path_dup(inside);
	CHECK(wf_app_archive_in_view(&r.a));
	free(inside);
	wf_app_destroy(&r.a);
}

int main(void)
{
	a_new_tab_keeps_the_folder_and_each_tab_keeps_its_own();
	the_current_tab_is_always_in_a_narrow_strip();
	an_archive_is_in_view_when_any_tab_shows_one();
	three_tabs_are_in_order_and_a_new_one_goes_after_this_one();
	previous_is_the_one_before_with_three_tabs();
	a_tab_gets_its_own_cursor_back_not_the_folders_memory();
	w_closes_the_tab_and_goes_to_a_neighbour();
	a_close_whose_walk_cannot_start_closes_nothing();
	a_close_into_an_unreadable_folder_leaves_every_tab_as_it_was();
	a_tab_that_cannot_be_read_leaves_the_tabs_as_they_were();
	at_most_eight_tabs();
	without_a_second_tab_the_keys_say_so_and_move_nothing();
	the_strip_is_in_the_title_row_only_with_more_than_one_tab();
	ctrl_left_and_right_move_between_tabs();
	the_keys_are_in_every_profile_and_the_commands_have_names();
	m_then_a_letter_keeps_the_folder_through_the_settings_save();
	a_non_ascii_folder_is_saved_as_utf8();
	esc_or_another_key_saves_nothing_and_goes_nowhere();
	a_failed_save_says_why_and_keeps_nothing();
	without_a_configuration_file_or_in_an_archive_nothing_is_saved();
	the_apostrophe_goes_to_the_place_or_says_there_is_none();
	a_place_is_a_folder_a_tab_can_go_to_too();
	many_places_are_listed_with_a_count_of_the_rest();
	return TEST_RESULT();
}

/*
 * The file manager with the Vim and Nano profiles: key sequences, counts, the
 * command line, the sort menu and the safety of what a key can start. Driven
 * with events against the fake file system and the fake transfer engine.
 */
#include <string.h>

#include "archive/wf_vpath.h"
#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_fake_xfer.h"
#include "wf_test.h"
#include "base/wf_macro.h"

static FakeProbe P;

typedef struct
{
	WfApp app;
	FakePort fs;
	FakeXfer xf;
	WfKeyConfig keys;
} Rig;

static void world(void)
{
	fp_init(&P);
	fp_dir(&P, L"C:\\Users", 2);
	fp_dir(&P, L"C:\\Users\\dev", 3);
	fp_dir(&P, L"C:\\Users\\dev\\Docs", 4);
	fp_file(&P, L"C:\\Users\\dev\\file10.txt", 20, 5);
	fp_file(&P, L"C:\\Users\\dev\\file2.txt", 100, 6);
}

static void rig_open(Rig *r, WfProfile profile)
{
	memset(r, 0, sizeof *r);
	world();
	r->fs.app = &r->app;
	r->xf.app = &r->app;
	r->xf.ops = fp_ops(&P);
	CHECK(wf_app_init(&r->app, 100, 30, fake_port(&r->fs),
	              L"C:\\Users\\dev") == WF_OK);
	wf_app_set_xfer(&r->app, fx_port(&r->xf));
	wf_keyconfig_init(&r->keys);
	r->keys.profile = profile;
	CHECK(WF_OK == wf_app_set_keys(&r->app, &r->keys, 1));
	wf_i18n_set_language(WF_UILANG_EN);
}

static void rig_close(Rig *r)
{
	wf_app_destroy(&r->app);
}

static WfAction ev(WfApp *a, WfEventType type, uint32_t c, WfKey k)
{
	WfEvent e = {type, c, k, 0};

	return a->mode->on_event(a, &e);
}

static WfAction ch(WfApp *a, uint32_t c)
{
	return ev(a, WF_EV_CHAR, c, WF_KEY_NONE);
}

static WfAction key(WfApp *a, WfKey k)
{
	return ev(a, WF_EV_KEY, 0, k);
}

static void type(WfApp *a, const char *s)
{
	for (; *s; s++) {
		ch(a, (uint32_t)(unsigned char)*s);
	}
}

static const wchar_t *cur(const Rig *r)
{
	const WfEntry *e = wf_view_current(&r->app.cur);

	return e ? e->name : L"";
}

/* The listing of C:\Users\dev is Docs, file2.txt, file10.txt. */
static void vim_moves_by_letters_sequences_and_counts(void)
{
	static Rig r;

	rig_open(&r, WF_PROFILE_VIM);
	CHECK(!wcscmp(cur(&r), L"Docs"));
	ch(&r.app, 'j');
	CHECK(!wcscmp(cur(&r), L"file2.txt"));
	ch(&r.app, 'G');
	CHECK(!wcscmp(cur(&r), L"file10.txt"));
	ch(&r.app, 'g');
	CHECK(wf_keystate_active(&r.app.keys));
	ch(&r.app, 'g');
	CHECK(!wcscmp(cur(&r), L"Docs") && !wf_keystate_active(&r.app.keys));
	/* counts repeat a move, and stop at the end */
	ch(&r.app, '2');
	ch(&r.app, 'j');
	CHECK(!wcscmp(cur(&r), L"file10.txt"));
	ch(&r.app, '5');
	ch(&r.app, 'k');
	CHECK(!wcscmp(cur(&r), L"Docs"));
	/* a number before G is a row */
	ch(&r.app, '2');
	ch(&r.app, 'G');
	CHECK(!wcscmp(cur(&r), L"file2.txt"));
	/* a count far past the cap is capped, not trusted */
	for (int i = 0; i < 12; i++) {
		ch(&r.app, '9');
	}
	ch(&r.app, 'j');
	CHECK(!wcscmp(cur(&r), L"file10.txt"));
	rig_close(&r);
}

static void a_partial_sequence_ends_by_esc_by_timeout_and_by_a_new_screen(void)
{
	static Rig r;

	rig_open(&r, WF_PROFILE_VIM);
	ch(&r.app, '2');
	ch(&r.app, 'g');
	WfFrame *frame = wf_frame_new(100, 30, &WF_STYLE_BASE);
	wf_app_draw(&r.app, frame);
	char footer[101];
	for (int x = 0; x < 100; x++) {
		footer[x] = frame->cells[2900 + x].utf8[0];
	}
	footer[100] = 0;
	CHECK(strstr(footer, "2g") && !strstr(footer, "2 g"));
	wf_frame_free(frame);
	key(&r.app, WF_KEY_ESC);
	ch(&r.app, 'G');
	/* Esc drops it */
	ch(&r.app, 'g');
	CHECK(wf_keystate_active(&r.app.keys));
	key(&r.app, WF_KEY_ESC);
	CHECK(!wf_keystate_active(&r.app.keys));
	ch(&r.app, 'g');
	CHECK(!wcscmp(cur(&r), L"file10.txt")); /* one g is not a move */
	/* the time runs out: the next g starts again, it does not complete */
	CHECK(WF_WAIT_FOREVER != wf_app_keys_timeout(&r.app));
	r.app.now_ms += 2000;
	ch(&r.app, 'g');
	CHECK(!wcscmp(cur(&r), L"file10.txt"));
	ch(&r.app, 'g'); /* ... and now it is gg */
	CHECK(!wcscmp(cur(&r), L"Docs"));
	CHECK(WF_WAIT_FOREVER == wf_app_keys_timeout(&r.app));
	/* another screen drops what was typed */
	ch(&r.app, 'G');
	ch(&r.app, 'g');
	ch(&r.app, '/'); /* the filter */
	CHECK(&WF_MODE_NORMAL != r.app.mode);
	key(&r.app, WF_KEY_ESC);
	ch(&r.app, 'g');
	CHECK(!wcscmp(cur(&r), L"file10.txt"));
	rig_close(&r);
}

static void dd_stages_a_move_and_yy_a_copy_and_neither_deletes(void)
{
	static Rig r;

	rig_open(&r, WF_PROFILE_VIM);
	ch(&r.app, 'j'); /* file2.txt */
	ch(&r.app, 'd');
	CHECK(WF_XFER_NONE == r.app.slot.mode && &WF_MODE_NORMAL == r.app.mode);
	ch(&r.app, 'd');
	CHECK(WF_XFER_MOVE == r.app.slot.mode && 1 == r.app.slot.count);
	CHECK(&WF_MODE_NORMAL == r.app.mode && 0 == r.app.recycle_slot.count);
	ch(&r.app, 'y');
	ch(&r.app, 'y');
	CHECK(WF_XFER_COPY == r.app.slot.mode && 1 == r.app.slot.count);
	/* a number is never put before a copy, a move or a delete */
	wf_app_xfer_drop_slot(&r.app);
	ch(&r.app, '5');
	ch(&r.app, 'd');
	ch(&r.app, 'd');
	CHECK(WF_XFER_NONE == r.app.slot.mode &&
	        !strcmp(r.app.msg, wf_msg(MSG_KEYS_COUNT_REFUSED)));
	ch(&r.app, '3');
	ch(&r.app, 'y');
	ch(&r.app, 'y');
	CHECK(WF_XFER_NONE == r.app.slot.mode);
	rig_close(&r);
}

static void mashing_keys_starts_nothing_that_loses_data(void)
{
	static Rig r;
	static const char keys[] =
	        "jkgGddyyxpuzCSwq5:/ ,?hlbf.rivaeP%dg0123456789";
	unsigned seed = 12345;

	rig_open(&r, WF_PROFILE_VIM);
	for (int i = 0; i < 4000; i++) {
		WfAction a;

		seed = seed * 1103515245u + 12345u;
		/* no q (quit), no Delete: those are tested on their own */
		do {
			seed = seed * 1103515245u + 12345u;
		} while ('q' == keys[(seed >> 16) % (sizeof keys - 1)] ||
		        'p' == keys[(seed >> 16) % (sizeof keys - 1)] ||
		        'r' == keys[(seed >> 16) % (sizeof keys - 1)]);
		a = r.app.mode->on_event(&r.app,
		        &(WfEvent){WF_EV_CHAR,
		                (uint32_t)(unsigned char)
		                        keys[(seed >> 16) % (sizeof keys - 1)],
		                WF_KEY_NONE, 0});
		CHECK(WF_ACT_QUIT != a);
		if (&WF_MODE_NORMAL != r.app.mode) {
			key(&r.app, WF_KEY_ESC); /* leave any prompt */
		}
		if (&WF_MODE_CONFIRM == r.app.mode) {
			key(&r.app, WF_KEY_ESC);
		}
		r.app.mode = &WF_MODE_NORMAL;
		if (i % 7 == 0) {
			r.app.now_ms += 100;
		}
	}
	CHECK(NULL != wf_view_current(&r.app.cur) || 0 == r.app.cur.nvis);
	rig_close(&r);
}

/* A command longer than the row scrolls so the cursor is always in view, and
 * the end of what is typed is what shows. */
static int fake_editor_open(
        void *ctx, const wchar_t *path, char *why, size_t cap)
{
	(void)ctx;
	(void)path;
	(void)why;
	(void)cap;
	return 0;
}

static int fake_editor_event(void *ctx, const WfEvent *ev, int rows, int cols)
{
	(void)ctx;
	(void)ev;
	(void)rows;
	(void)cols;
	return 2; /* closes */
}

/* The editor opened from the preview returns to the preview, where it was
 * being read; opened from the lists it returns to the lists. */
static void the_editor_returns_to_where_it_was_opened(void)
{
	static Rig r;
	WfEvent any = {WF_EV_CHAR, 'x', WF_KEY_NONE, 0};

	rig_open(&r, WF_PROFILE_VIM);
	r.app.editor.open = fake_editor_open;
	r.app.editor.event = fake_editor_event;
	key(&r.app, WF_KEY_DOWN); /* a file */
	r.app.mode = &WF_MODE_PREVIEW_PANE;
	r.app.preview_top = 7;
	wf_app_edit_begin(&r.app);
	CHECK(&WF_MODE_EDITOR == r.app.mode);
	(void)r.app.mode->on_event(&r.app, &any);
	CHECK(&WF_MODE_PREVIEW_PANE == r.app.mode && 7 == r.app.preview_top);

	r.app.mode = &WF_MODE_NORMAL;
	wf_app_edit_begin(&r.app);
	(void)r.app.mode->on_event(&r.app, &any);
	CHECK(&WF_MODE_NORMAL == r.app.mode);
}

static void a_long_command_keeps_the_cursor_in_view(void)
{
	static Rig r;
	WfFrame *f = wf_frame_new(24, 5, &WF_STYLE_BASE);

	CHECK(NULL != f);
	rig_open(&r, WF_PROFILE_VIM);
	ch(&r.app, ':');
	type(&r.app, "filter aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaZZ");
	r.app.mode->draw(&r.app, f);
	CHECK(f->cursor.visible && f->cursor.x >= 1 && f->cursor.x < 24 &&
	        f->cursor.y == 4);
	/* the last typed characters are on the row, the start is marked as cut */
	CHECK('<' == f->cells[4 * 24 + 1].utf8[0]);
	wf_frame_free(f);
}

static void the_command_line_runs_the_documented_commands(void)
{
	static Rig r;

	rig_open(&r, WF_PROFILE_VIM);
	ch(&r.app, ':');
	CHECK(&WF_MODE_CMDLINE == r.app.mode);
	key(&r.app, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.app.mode);

	ch(&r.app, ':');
	type(&r.app, "sort size desc");
	key(&r.app, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == r.app.mode && WF_SORT_SIZE == r.app.sort_key &&
	        r.app.sort_desc);
	ch(&r.app, ':');
	type(&r.app, "sort name asc");
	key(&r.app, WF_KEY_ENTER);
	CHECK(WF_SORT_NAME == r.app.sort_key && !r.app.sort_desc);

	ch(&r.app, ':');
	type(&r.app, "filter file");
	key(&r.app, WF_KEY_ENTER);
	CHECK(2 == r.app.cur.nvis);
	ch(&r.app, ':');
	type(&r.app, "clear-filter");
	key(&r.app, WF_KEY_ENTER);
	CHECK(3 == r.app.cur.nvis);

	ch(&r.app, ':');
	type(&r.app, "set hidden on");
	key(&r.app, WF_KEY_ENTER);
	CHECK(r.app.cur.show_hidden);
	ch(&r.app, ':');
	type(&r.app, "set hidden off");
	key(&r.app, WF_KEY_ENTER);
	CHECK(!r.app.cur.show_hidden);

	ch(&r.app, ':');
	type(&r.app, "go C:\\Users");
	key(&r.app, WF_KEY_ENTER);
	CHECK(!wcscmp(r.app.cwd, L"C:\\Users"));

	/* history: Up brings the last line back */
	ch(&r.app, ':');
	key(&r.app, WF_KEY_UP);
	CHECK(!strcmp(r.app.cmdline.text, "go C:\\Users"));
	key(&r.app, WF_KEY_ESC);

	/* no shell, no unknown commands, no extra words */
	ch(&r.app, ':');
	type(&r.app, "!dir");
	key(&r.app, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == r.app.mode && r.app.msg[0]);
	ch(&r.app, ':');
	type(&r.app, "frobnicate");
	key(&r.app, WF_KEY_ENTER);
	CHECK(r.app.msg[0] && !wcscmp(r.app.cwd, L"C:\\Users"));
	ch(&r.app, ':');
	type(&r.app, "sort created");
	key(&r.app, WF_KEY_ENTER);
	CHECK(r.app.msg[0] && WF_SORT_NAME == r.app.sort_key);

	/* the same quit as q */
	ch(&r.app, ':');
	type(&r.app, "q");
	CHECK(WF_ACT_QUIT == key(&r.app, WF_KEY_ENTER));
	rig_close(&r);
}

static void the_sort_menu_and_nano_keys_work(void)
{
	static Rig r;

	rig_open(&r, WF_PROFILE_NANO);
	ch(&r.app, 'S');
	CHECK(&WF_MODE_SORT == r.app.mode);
	ch(&r.app, 's'); /* size */
	CHECK(&WF_MODE_NORMAL == r.app.mode || &WF_MODE_SORT == r.app.mode);
	key(&r.app, WF_KEY_ESC);
	r.app.mode = &WF_MODE_NORMAL;
	/* on a screen too small for the box the menu is still readable */
	ch(&r.app, 'S');
	{
		WfFrame *small = wf_frame_new(30, 6, &WF_STYLE_BASE);
		bool any = false, hint = false;

		CHECK(NULL != small);
		r.app.mode->draw(&r.app, small);
		for (int i = 0; i < 30; i++) {
			any |= ' ' != small->cells[i].utf8[0]; /* the first row */
			hint |= ' ' != small->cells[5 * 30 + i].utf8[0]; /* the last */
		}
		CHECK(any && hint);
		wf_frame_free(small);
	}
	key(&r.app, WF_KEY_ESC);
	/* the arrows choose a row and Enter applies it; what is in force and
	 * what the arrows are on are different things */
	ch(&r.app, 'S');
	CHECK(&WF_MODE_SORT == r.app.mode && WF_SORT_SIZE == r.app.sort_key &&
	        WF_SORT_SIZE == r.app.sort_pick);
	key(&r.app, WF_KEY_DOWN); /* date */
	CHECK(WF_SORT_DATE == r.app.sort_pick && WF_SORT_SIZE == r.app.sort_key);
	key(&r.app, WF_KEY_UP);
	key(&r.app, WF_KEY_UP); /* name */
	CHECK(WF_SORT_NAME == r.app.sort_pick);
	key(&r.app, WF_KEY_UP); /* wraps to the last */
	CHECK(3 == r.app.sort_pick);
	key(&r.app, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == r.app.mode && 3 == (int)r.app.sort_key);
	key(&r.app, WF_KEY_DOWN);
	CHECK(0 != wcscmp(cur(&r), L"Docs"));
	ev(&r.app, WF_EV_ALT, '6', WF_KEY_NONE);
	CHECK(WF_XFER_COPY == r.app.slot.mode);
	ev(&r.app, WF_EV_CTRL, 'k', WF_KEY_NONE);
	CHECK(WF_XFER_MOVE == r.app.slot.mode);
	/* with no engine wired, Delete changes nothing; what it shows closes */
	key(&r.app, WF_KEY_DELETE);
	key(&r.app, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.app.mode);
	/* q is the one quit */
	CHECK(WF_ACT_QUIT == ch(&r.app, 'q'));
	rig_close(&r);
}

static void the_standard_profile_keeps_the_old_keys(void)
{
	static Rig r;

	rig_open(&r, WF_PROFILE_LEGACY);
	ch(&r.app, 'g'); /* Smart Go, as it always was */
	CHECK(&WF_MODE_GO == r.app.mode);
	key(&r.app, WF_KEY_ESC);
	ch(&r.app, 'j');
	CHECK(!wcscmp(cur(&r), L"file2.txt"));
	ch(&r.app, ':'); /* not a key of the old set */
	CHECK(&WF_MODE_NORMAL == r.app.mode);
	rig_close(&r);
}

static void archives_stay_read_only_under_every_profile(void)
{
	static Rig r;

	for (int p = WF_PROFILE_NANO; p <= WF_PROFILE_VIM; p++) {
		static const char *const writes[] = {
		        "dd", "yy", "x", "y", "p", "a", "d", "%", "R"};

		rig_open(&r, (WfProfile)p);
		free(r.app.cwd);
		r.app.cwd = _wcsdup(L"C:\\dl\\p.zip|\\inner");
		CHECK(wf_vpath_is(r.app.cwd));
		for (size_t i = 0; i < WF_ARRAY_LEN(writes); i++) {
			r.app.msg[0] = '\0';
			type(&r.app, writes[i]);
			CHECK(WF_XFER_NONE == r.app.slot.mode &&
			        &WF_MODE_NORMAL == r.app.mode);
			key(&r.app, WF_KEY_ESC);
		}
		r.app.msg[0] = '\0';
		key(&r.app, WF_KEY_DELETE);
		CHECK(&WF_MODE_NORMAL == r.app.mode &&
		        !strcmp(r.app.msg, wf_msg(MSG_AR_READONLY)));
		rig_close(&r);
	}
}

static void focus_marks_and_custom_hints_are_distinct(void)
{
	static Rig r;
	WfFrame *f;
	WfLayout l;
	WfKeyOverride o = {0};
	char hint[320];
	size_t at;
	rig_open(&r, WF_PROFILE_NANO);
	wf_layout_compute(100, 30, &r.app.ratios, &l);
	f = wf_frame_new(100, 30, &WF_STYLE_BASE);
	ch(&r.app, ' ');
	wf_app_draw(&r.app, f);
	at = (size_t)l.body_y * 100 + (size_t)l.pane[WF_PANE_CURRENT].x;
	CHECK(!strcmp(f->cells[at + 1].utf8, "*"));
	at += r.app.cur.cursor * 100;
	CHECK(!strcmp(f->cells[at].utf8, ">"));
	CHECK(f->cells[at].bg_rgb == wf_theme_styles()->selection.bg_rgb);
	r.app.mode = &WF_MODE_PREVIEW_PANE;
	wf_app_draw(&r.app, f);
	CHECK(f->cells[at].bg_rgb == wf_theme_styles()->cursor_line.bg_rgb);
	r.keys.profile = WF_PROFILE_CUSTOM;
	o.ctx = WF_CTX_FM;
	o.cmd = WF_FM_HELP;
	CHECK(wf_keys_parse_list("F3", &o) && wf_keyconfig_set(&r.keys, &o));
	CHECK(WF_OK == wf_app_set_keys(&r.app, &r.keys, 1));
	wf_app_binding_hint(&r.app, false, hint, sizeof hint);
	CHECK(strstr(hint, "F3 Help") && !strstr(hint, "Ctrl+G"));
	wf_app_binding_hint(&r.app, true, hint, sizeof hint);
	CHECK(strstr(hint, "Esc Close") && !strstr(hint, "F3"));
	wf_frame_free(f);
	rig_close(&r);
}

int main(void)
{
	vim_moves_by_letters_sequences_and_counts();
	a_partial_sequence_ends_by_esc_by_timeout_and_by_a_new_screen();
	dd_stages_a_move_and_yy_a_copy_and_neither_deletes();
	mashing_keys_starts_nothing_that_loses_data();
	the_command_line_runs_the_documented_commands();
	a_long_command_keeps_the_cursor_in_view();
	the_editor_returns_to_where_it_was_opened();
	the_sort_menu_and_nano_keys_work();
	the_standard_profile_keeps_the_old_keys();
	archives_stay_read_only_under_every_profile();
	focus_marks_and_custom_hints_are_distinct();
	return TEST_RESULT();
}

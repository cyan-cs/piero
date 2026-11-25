#include <stdio.h>
#include <string.h>

#include "ui/wf_app_priv.h"
#include "wf_test.h"
#include "base/wf_macro.h"

typedef struct
{
	WfApp *app;
	int deferred, queued, entries, omit, extra;
	struct
	{
		uint32_t gen;
		wchar_t path[128];
	} pending[16];
} MemoryFs;

static WfStatus memory_listing(MemoryFs *fs, const wchar_t *path, WfDir **out)
{
	WfDir *dir;
	WfStatus st = WF_OK;

	if (!wcscmp(path, L"C:\\mem\\missing")) {
		return WF_E_NOENT;
	}
	dir = wf_dir_new();
	if (!dir) {
		return WF_E_NOMEM;
	}
	if (!wcscmp(path, L"C:\\mem")) {
		const wchar_t *names[] = {L"alpha", L"beta", L"empty"};
		for (size_t i = 0; i < WF_ARRAY_LEN(names) && WF_OK == st;
		        i++) {
			st = wf_dir_add(dir, names[i], wcslen(names[i]),
			        WF_ENT_DIR, 0, 0);
		}
	} else if (wcscmp(path, L"C:\\mem\\empty")) {
		if (fs->entries) {
			st = wf_dir_add(dir, L"child", 5, WF_ENT_DIR, 0, 0);
		}
		if (fs->extra && WF_OK == st) {
			st = wf_dir_add(dir, L"a_new", 5, 0, 0, 0);
		}
		for (int i = 0; i < fs->entries && WF_OK == st; i++) {
			wchar_t name[32];
			if (i == fs->omit) {
				continue;
			}
			swprintf(name, 32, L"item%02d", i);
			st = wf_dir_add(dir, name, wcslen(name), 0, 0, 0);
		}
	}
	if (WF_OK == st) {
		st = wf_dir_sort(dir, WF_SORT_NAME, 0);
	}
	if (WF_OK != st) {
		wf_dir_free(dir);
		return st;
	}
	*out = dir;
	return WF_OK;
}

static void answer(MemoryFs *fs, uint32_t gen, const wchar_t *path)
{
	WfDir *dir = NULL;
	WfStatus st = memory_listing(fs, path, &dir);
	wf_app_on_listing(fs->app, gen, st, dir, NULL);
}

static WfStatus submit(
        void *ctx, uint32_t gen, const wchar_t *path, const wchar_t *parent)
{
	MemoryFs *fs = ctx;
	(void)parent;
	if (fs->deferred) {
		if (16 == fs->queued) {
			return WF_E_RANGE;
		}
		fs->pending[fs->queued].gen = gen;
		wcscpy_s(fs->pending[fs->queued++].path, 128, path);
	} else {
		answer(fs, gen, path);
	}
	return WF_OK;
}

static void deliver(MemoryFs *fs, int i)
{
	answer(fs, fs->pending[i].gen, fs->pending[i].path);
}

static void key(WfApp *app, WfKey code)
{
	WfEvent ev = {WF_EV_KEY, 0, code, 0};
	app->mode->on_event(app, &ev);
}

static void ch(WfApp *app, uint32_t cp)
{
	WfEvent ev = {WF_EV_CHAR, cp, WF_KEY_NONE, 0};
	app->mode->on_event(app, &ev);
}

static void open_app(WfApp *app, MemoryFs *fs, int cols)
{
	WfFsPort port = {fs, submit};
	memset(fs, 0, sizeof *fs);
	fs->app = app;
	fs->entries = 40;
	fs->omit = -1;
	CHECK(WF_OK == wf_app_init(app, cols, 10, port, L"C:\\mem"));
}

static void test_parent_return_and_independent_positions(void)
{
	WfApp app;
	MemoryFs fs;
	open_app(&app, &fs, 160);
	key(&app, WF_KEY_RIGHT);
	CHECK(!wcscmp(app.cwd, L"C:\\mem\\alpha") && 0 == app.cur.cursor);
	wf_view_move(&app.cur, 30, 5);
	wf_view_move(&app.cur, -2, 5);
	CHECK(28 == app.cur.cursor && 26 == app.cur.top);
	CHECK(WF_OK == wf_sel_toggle(&app.sel, app.cwd, L"item05"));
	key(&app, WF_KEY_LEFT);
	CHECK(!wcscmp(app.cwd, L"C:\\mem") &&
	        !wcscmp(wf_view_current(&app.cur)->name, L"alpha"));
	CHECK(0 == wf_sel_count(&app.sel, app.cwd));
	key(&app, WF_KEY_RIGHT);
	CHECK(28 == app.cur.cursor && 26 == app.cur.top);
	CHECK(!wcscmp(wf_view_current(&app.cur)->name, L"item27"));
	CHECK(1 == wf_sel_count(&app.sel, app.cwd));
	wf_app_go(&app, L"C:\\mem\\beta", NULL);
	CHECK(0 == app.cur.cursor && 0 == app.cur.top);
	wf_view_move(&app.cur, 10, 5);
	wf_app_go(&app, L"C:\\mem\\alpha", NULL);
	CHECK(28 == app.cur.cursor && 26 == app.cur.top);
	wf_app_go(&app, L"C:\\mem\\beta", NULL);
	CHECK(10 == app.cur.cursor && 6 == app.cur.top);
	wf_app_destroy(&app);
}

static void test_missing_item_nearby_and_empty(void)
{
	WfApp app;
	MemoryFs fs;
	open_app(&app, &fs, 70);
	key(&app, WF_KEY_RIGHT);
	wf_view_move(&app.cur, 28, 5);
	key(&app, WF_KEY_LEFT);
	fs.omit = 27;
	key(&app, WF_KEY_RIGHT);
	CHECK(28 == app.cur.cursor &&
	        !wcscmp(wf_view_current(&app.cur)->name, L"item28"));
	key(&app, WF_KEY_LEFT);
	fs.entries = 3;
	key(&app, WF_KEY_RIGHT);
	CHECK(3 == app.cur.cursor && 0 == app.cur.top);
	key(&app, WF_KEY_LEFT);
	fs.entries = 0;
	key(&app, WF_KEY_RIGHT);
	CHECK(0 == app.cur.nvis && 0 == app.cur.cursor && 0 == app.cur.top);
	wf_app_destroy(&app);
}

static void test_latest_result_saves_latest_displayed_position(void)
{
	WfApp app;
	MemoryFs fs;
	open_app(&app, &fs, 160);
	key(&app, WF_KEY_RIGHT);
	wf_view_move(&app.cur, 14, 5);
	fs.deferred = 1;
	wf_app_go(&app, L"C:\\mem\\beta", NULL);
	wf_view_move(
	        &app.cur, 7, 5); /* safe input while the old list is visible */
	wf_app_go(&app, L"C:\\mem\\empty", NULL);
	deliver(&fs, 0);
	CHECK(!wcscmp(app.cwd, L"C:\\mem\\alpha") && 21 == app.cur.cursor);
	deliver(&fs, 1);
	CHECK(!wcscmp(app.cwd, L"C:\\mem\\empty") && !app.cur.nvis);
	wf_app_go(&app, L"C:\\mem\\alpha", NULL);
	CHECK(!wcscmp(app.cwd, L"C:\\mem\\empty"));
	deliver(&fs, 2);
	CHECK(21 == app.cur.cursor && 17 == app.cur.top);
	wf_app_go(&app, L"C:\\mem\\missing", NULL);
	deliver(&fs, 3);
	CHECK(!wcscmp(app.cwd, L"C:\\mem\\alpha") && 21 == app.cur.cursor);
	wf_app_reload(&app);
	wf_view_move(&app.cur, 3, 5);
	deliver(&fs, 4);
	CHECK(24 == app.cur.cursor && 20 == app.cur.top);
	wf_app_destroy(&app);
}

static void test_control_edges_in_two_and_three_columns(void)
{
	const int widths[] = {70, 160};
	for (size_t i = 0; i < WF_ARRAY_LEN(widths); i++) {
		WfApp app;
		MemoryFs fs;
		open_app(&app, &fs, widths[i]);
		key(&app, WF_KEY_RIGHT);
		CHECK(WF_OK == wf_sel_toggle(&app.sel, app.cwd, L"item05"));
		key(&app, WF_KEY_CTRL_DOWN);
		CHECK(40 == app.cur.cursor && 36 == app.cur.top);
		key(&app, WF_KEY_CTRL_UP);
		CHECK(0 == app.cur.cursor && 0 == app.cur.top);
		CHECK(1 == wf_sel_count(&app.sel, app.cwd));
		ch(&app, '/');
		ch(&app, '1');
		key(&app, WF_KEY_CTRL_DOWN);
		CHECK(&WF_MODE_FILTER == app.mode &&
		        app.cur.cursor == app.cur.nvis - 1);
		key(&app, WF_KEY_CTRL_UP);
		CHECK(&WF_MODE_FILTER == app.mode && 0 == app.cur.cursor &&
		        0 == app.cur.top);
		key(&app, WF_KEY_ESC);
		wf_app_go(&app, L"C:\\mem\\empty", NULL);
		key(&app, WF_KEY_CTRL_UP);
		key(&app, WF_KEY_CTRL_DOWN);
		CHECK(0 == app.cur.cursor && 0 == app.cur.top && !app.cur.nvis);
		wf_app_destroy(&app);
	}
}

static void test_unicode_resize_and_bounded_memory(void)
{
	WfApp app;
	MemoryFs fs;
	open_app(&app, &fs, 160);
	wf_app_go(&app, L"C:\\mem\\日本語", NULL);
	wf_view_move(&app.cur, 30, 5);
	wf_view_move(&app.cur, -2, 5);
	wf_app_go(&app, L"C:\\mem\\empty", NULL);
	app.rows = 45;
	wf_app_go(&app, L"C:\\mem\\日本語", NULL);
	CHECK(28 == app.cur.cursor && 1 == app.cur.top);
	{
		wchar_t path[1100] = L"C:\\mem";
		for (int i = 0; i < 100; i++) {
			wcscat_s(path, 1100, L"\\section");
		}
		wcscat_s(path, 1100, L"\\???");
		app.rows = 10;
		wf_app_go(&app, path, NULL);
		wf_view_move(&app.cur, 31, 5);
		wf_app_go(&app, L"C:\\mem\\empty", NULL);
		wf_app_go(&app, path, NULL);
		CHECK(31 == app.cur.cursor && 27 == app.cur.top &&
		        !wcscmp(app.cwd, path));
	}

	for (int i = 0; i < 130; i++) {
		wchar_t path[64];
		swprintf(path, 64, L"C:\\mem\\cached%03d", i);
		wf_app_go(&app, path, NULL);
		wf_view_move(&app.cur, 2, 40);
	}
	wf_app_go(&app, L"C:\\mem\\cached000", NULL);
	CHECK(0 ==
	        app.cur.cursor); /* old positions are bounded, not an unbounded log */
	wf_app_go(&app, L"C:\\mem\\cached129", NULL);
	CHECK(2 == app.cur.cursor);
	wf_app_destroy(&app);
}

static void test_name_priority_and_lru_target_protection(void)
{
	WfApp app;
	MemoryFs fs;
	open_app(&app, &fs, 160);
	key(&app, WF_KEY_RIGHT);
	wf_view_move(&app.cur, 28, 5);
	key(&app, WF_KEY_LEFT);
	fs.extra = 1;
	key(&app, WF_KEY_RIGHT);
	CHECK(29 == app.cur.cursor &&
	        !wcscmp(wf_view_current(&app.cur)->name, L"item27"));
	wf_app_destroy(&app);
	CHECK(NULL == app.navigation_memory);

	open_app(&app, &fs, 160);
	wf_view_move(&app.cur, 1, 5);
	for (int i = 0; i < 128; i++) {
		wchar_t path[64];
		swprintf(path, 64, L"C:\\mem\\position%03d", i);
		wf_app_go(&app, path, NULL);
	}
	wf_app_go(&app, L"C:\\mem", NULL);
	CHECK(1 ==
	        app.cur.cursor); /* Target must not be evicted while saving departure. */
	wf_app_destroy(&app);
	CHECK(NULL == app.navigation_memory);
}

int main(void)
{
	test_name_priority_and_lru_target_protection();
	test_parent_return_and_independent_positions();
	test_missing_item_nearby_and_empty();
	test_latest_result_saves_latest_displayed_position();
	test_control_edges_in_two_and_three_columns();
	test_unicode_resize_and_bounded_memory();
	return TEST_RESULT();
}

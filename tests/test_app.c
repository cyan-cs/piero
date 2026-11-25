#include <stdlib.h>
#include <string.h>

#include "text/wf_text.h"
#include "ui/wf_app.h"
#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_fake_probe.h"
#include "wf_test.h"
#include "base/wf_macro.h"

static void ch(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	a->mode->on_event(a, &e);
}

static void keyp(WfApp *a, WfKey k)
{
	WfEvent e = {WF_EV_KEY, 0, k, 0};

	a->mode->on_event(a, &e);
}

static const wchar_t *cur_name(WfApp *a)
{
	const WfEntry *e = wf_view_current(&a->cur);

	return e ? e->name : L"(none)";
}

typedef struct
{
	WfApp *app;
	size_t capture_calls;
	size_t preflight_calls;
	wchar_t last_dir[260];
	size_t last_count;
} FakeXfer;

static WfStatus fake_capture(void *ctx,
        uint32_t generation,
        WfTransferMode mode,
        const wchar_t *dir,
        const wchar_t *const *names,
        size_t n,
        size_t hidden_excluded)
{
	FakeXfer *port = ctx;

	(void)generation;
	(void)mode;
	(void)names;
	(void)hidden_excluded;
	port->capture_calls++;
	wcsncpy_s(port->last_dir, WF_ARRAY_LEN(port->last_dir), dir, _TRUNCATE);
	port->last_count = n;
	return WF_OK;
}

static WfStatus fake_preflight(void *ctx,
        uint32_t generation,
        const WfTransferSlot *slot,
        const wchar_t *dest)
{
	FakeXfer *port = ctx;

	(void)generation;
	(void)slot;
	port->preflight_calls++;
	wcsncpy_s(
	        port->last_dir, WF_ARRAY_LEN(port->last_dir), dest, _TRUNCATE);
	return WF_OK;
}

static WfXferPort fake_xfer_port(FakeXfer *port)
{
	WfXferPort xfer = {port, fake_capture, fake_preflight};

	return xfer;
}

static void test_stale_generation_is_dropped(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	fp.defer = 1;
	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
	fake_deliver(&fp, 0);
	CHECK(0 == wcscmp(a.cwd, L"C:\\Users") &&
	        0 == wcscmp(cur_name(&a), L"dev"));

	ch(&a, 'l'); /* request 1: C:\Users\dev */
	ch(&a, 'h'); /* request 2 supersedes it: back to C:\ */
	CHECK(3 == fp.nq && NULL != a.want_path);

	/* the slow, older answer arrives last-but-one: must be ignored */
	fake_deliver(&fp, 1);
	CHECK(0 == wcscmp(a.cwd, L"C:\\Users"));
	CHECK(NULL != a.want_path); /* still waiting for the newest */

	fake_deliver(&fp, 2);
	CHECK(0 == wcscmp(a.cwd, L"C:\\") && NULL == a.want_path);
	CHECK(2 == a.cwd_version); /* initial 1, then one real change */
	CHECK(2 == a.history_n && 0 == wcscmp(a.history[0].path, L"C:\\Users"));
	wf_app_destroy(&a);
}

static void test_failure_keeps_old_screen(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\"));
	ch(&a, 'k'); /* secret/ sorts before Users/ */
	ch(&a, 'k');
	while (0 != wcscmp(cur_name(&a), L"secret")) {
		ch(&a, 'k');
	}
	ch(&a, 'l');
	CHECK(0 == wcscmp(a.cwd, L"C:\\") && NULL != strstr(a.msg, "ACCESS"));
	CHECK(NULL != a.cur.dir && NULL == a.want_path);
	wf_app_destroy(&a);
}

static void test_reload_keeps_cursor(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	ch(&a, 'j');
	ch(&a, 'j');
	CHECK(0 == wcscmp(cur_name(&a), L"file10.txt"));
	wf_app_reload(&a);
	CHECK(0 == wcscmp(cur_name(&a), L"file10.txt"));
	CHECK(1 == a.cwd_version); /* a reload is not a navigation */
	wf_app_destroy(&a);
}

static void test_selection(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	const WfNameSet *set;

	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	/* Docs, file2.txt, file10.txt */
	ch(&a, ' ');
	ch(&a, ' '); /* selects Docs and file2.txt, cursor ends on file10.txt */
	CHECK(0 == wcscmp(cur_name(&a), L"file10.txt"));
	set = wf_sel_folder(&a.sel, a.cwd);
	CHECK(set && 2 == set->n && wf_nameset_has(set, L"Docs") &&
	        wf_nameset_has(set, L"file2.txt"));

	/* selection survives navigating away and back */
	ch(&a, 'h');
	CHECK(0 == wcscmp(a.cwd, L"C:\\Users"));
	ch(&a, 'l');
	CHECK(0 == wcscmp(a.cwd, L"C:\\Users\\dev"));
	CHECK(2 == wf_sel_count(&a.sel, a.cwd));

	/* ...and survives hiding/filtering the selected entries */
	ch(&a, '/');
	ch(&a, 'x');
	ch(&a, '1');
	ch(&a, '0'); /* "x10" matches nothing */
	CHECK(0 == a.cur.nvis && 2 == wf_sel_count(&a.sel, a.cwd));
	ch(&a, ' '); /* nothing visible: toggling must be a no-op, not a crash
	              */
	keyp(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode && 3 == a.cur.nvis);

	keyp(&a, WF_KEY_ESC); /* in NORMAL: clear the selection */
	CHECK(0 == wf_sel_count(&a.sel, a.cwd));
	wf_app_destroy(&a);
}

static void test_status_counts_hidden_selection(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfFrame *f;
	int found = 0, y, x;

	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	ch(&a, ' '); /* Docs */
	ch(&a, ' '); /* file2.txt */
	ch(&a, '/');
	ch(&a, 'f');
	ch(&a, 'i');
	ch(&a, 'l');
	ch(&a, 'e'); /* shows file2, file10; Docs is selected but hidden */
	keyp(&a, WF_KEY_ENTER);

	f = wf_frame_new(120, 30, &WF_STYLE_BASE);
	wf_app_draw(&a, f);
	y = 29;
	{
		char row[512] = "";

		for (x = 0; x < f->cols; x++) {
			strcat_s(row, sizeof row,
			        f->cells[y * f->cols + x].utf8);
		}
		found = NULL !=
		                strstr(row,
		                        "selected 2 (visible 1, hidden 1)") &&
		        NULL != strstr(row, "filter: file");
	}
	CHECK(found);
	wf_frame_free(f);
	wf_app_destroy(&a);
}

/* A filter that is on, and no longer being typed, is told in the status line
 * even where the counts and the hints do not fit: some files are out of sight,
 * and the person must not have to guess. */
static void test_an_applied_filter_is_told_in_a_narrow_status_line(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfFrame *f;
	char row[256] = "";

	CHECK(WF_OK ==
	        wf_app_init(&a, 40, 12, fake_port(&fp), L"C:\\Users\\dev"));
	ch(&a, '/');
	ch(&a, 'f');
	keyp(&a,
	        WF_KEY_ENTER); /* applied: back in the list, the filter stays */
	f = wf_frame_new(40, 12, &WF_STYLE_BASE);
	CHECK(f);
	wf_app_draw(&a, f);
	for (int x = 0; x < f->cols; x++) {
		strcat_s(row, sizeof row, f->cells[11 * f->cols + x].utf8);
	}
	CHECK(strstr(row, "[filter]"));
	wf_frame_free(f);
	wf_app_destroy(&a);
}

/* A filter longer than the row is shown from its end: the cut start is marked
 * with "<", the last typed letters and the cursor stay on the row. */
static void test_a_long_filter_keeps_its_end_and_the_cursor_on_screen(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfFrame *f;
	char row[512] = "";

	CHECK(WF_OK ==
	        wf_app_init(&a, 40, 12, fake_port(&fp), L"C:\\Users\\dev"));
	ch(&a, '/');
	for (int i = 0; i < 60; i++) {
		ch(&a, (uint32_t)('a' + i % 26));
	}
	ch(&a, 'X');
	ch(&a, 'Y');
	CHECK(&WF_MODE_FILTER == a.mode);
	f = wf_frame_new(40, 12, &WF_STYLE_BASE);
	CHECK(f);
	wf_app_draw(&a, f);
	for (int x = 0; x < f->cols; x++) {
		strcat_s(row, sizeof row, f->cells[10 * f->cols + x].utf8);
	}
	CHECK(strstr(row, "XY")); /* the end of what was typed is seen */
	CHECK(f->cursor.visible && 10 == f->cursor.y && f->cursor.x < f->cols);
	wf_frame_free(f);
	wf_app_destroy(&a);
}

/* A list with nothing in it says why: the filter leaves nothing, in words, at
 * the top of the pane, and not as a blank. */
static void test_an_empty_list_says_why(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfFrame *f;
	bool found = false;

	CHECK(WF_OK ==
	        wf_app_init(&a, 100, 24, fake_port(&fp), L"C:\\Users\\dev"));
	ch(&a, '/');
	for (int i = 0; i < 4; i++) {
		ch(&a, 'z');
	}
	keyp(&a, WF_KEY_ENTER);
	CHECK(0 == a.cur.nvis);
	f = wf_frame_new(100, 24, &WF_STYLE_BASE);
	CHECK(f);
	wf_app_draw(&a, f);
	for (int y = 0; y < f->rows && !found; y++) {
		char row[512] = "";

		for (int x = 0; x < f->cols; x++) {
			strcat_s(row, sizeof row,
			        f->cells[y * f->cols + x].utf8);
		}
		found = NULL != strstr(row, "No item matches the filter");
	}
	CHECK(found);
	wf_frame_free(f);
	wf_app_destroy(&a);
}

/* With no filter the line still has the counts: how many items, how many are
 * hidden, and that the list was cut. */
static void test_the_counts_are_shown_without_a_filter(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfFrame *f;
	char row[512] = "";

	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	f = wf_frame_new(120, 30, &WF_STYLE_BASE);
	CHECK(f);
	wf_app_draw(&a, f);
	for (int x = 0; x < f->cols; x++) {
		strcat_s(row, sizeof row, f->cells[29 * f->cols + x].utf8);
	}
	CHECK(strstr(row, " items") && strstr(row, "hidden"));
	wf_frame_free(f);
	wf_app_destroy(&a);
}

static void test_filter_editing(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	ch(&a, '/');
	CHECK(&WF_MODE_FILTER == a.mode);
	ch(&a, 'F'); /* case-insensitive */
	ch(&a, 'I');
	ch(&a, 'L');
	ch(&a, 'E');
	CHECK(2 == a.cur.nvis);
	ch(&a, '1');
	CHECK(1 == a.cur.nvis && 0 == wcscmp(cur_name(&a), L"file10.txt"));
	keyp(&a, WF_KEY_BACKSPACE);
	CHECK(2 == a.cur.nvis);

	/* a character outside the BMP is one backspace, not two */
	ch(&a, 0x1F600);
	CHECK(6 == wcslen(a.cur.filter));
	keyp(&a, WF_KEY_BACKSPACE);
	CHECK(4 == wcslen(a.cur.filter));

	keyp(&a, WF_KEY_ENTER); /* keeps the filter */
	CHECK(&WF_MODE_NORMAL == a.mode && 2 == a.cur.nvis);

	/* navigating to another directory drops it */
	ch(&a, 'h');
	CHECK(L'\0' == a.cur.filter[0]);

	/* Esc discards a filter being typed */
	ch(&a, '/');
	ch(&a, 'z');
	keyp(&a, WF_KEY_ESC);
	CHECK(L'\0' == a.cur.filter[0] && &WF_MODE_NORMAL == a.mode);

	/* overflow is ignored, never a buffer overrun */
	ch(&a, '/');
	{
		int i;

		for (i = 0; i < 200; i++) {
			ch(&a, 'a');
		}
	}
	CHECK(wcslen(a.cur.filter) == WF_FILTER_MAX - 1);
	keyp(&a, WF_KEY_ESC);
	wf_app_destroy(&a);
}

/* Esc cancels one thing per press, in the order selection, then a filter that
 * is on; with neither, it changes nothing. */
static void test_esc_cancels_selection_then_filter(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	ch(&a, '/');
	ch(&a, 'f');
	keyp(&a, WF_KEY_ENTER); /* the filter stays on */
	CHECK(2 == a.cur.nvis && L'f' == a.cur.filter[0]);
	ch(&a, ' '); /* mark one */
	CHECK(1 == wf_sel_count(&a.sel, a.cwd));
	keyp(&a, WF_KEY_ESC); /* first the mark */
	CHECK(0 == wf_sel_count(&a.sel, a.cwd) && L'f' == a.cur.filter[0] &&
	        2 == a.cur.nvis);
	keyp(&a, WF_KEY_ESC); /* then the filter */
	CHECK(L'\0' == a.cur.filter[0] && 3 == a.cur.nvis &&
	        &WF_MODE_NORMAL == a.mode);
	keyp(&a, WF_KEY_ESC); /* nothing left: nothing happens */
	CHECK(3 == a.cur.nvis && &WF_MODE_NORMAL == a.mode &&
	        0 == wcscmp(a.cwd, L"C:\\Users\\dev"));
	wf_app_destroy(&a);
}

/* Every Esc peels exactly one state, in the order marks, filter, pending
 * copy: the staged copy is the costliest to lose, so it goes last. */
static void test_esc_peels_one_state_at_a_time(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	a.slot.mode = WF_XFER_COPY;
	ch(&a, '/');
	ch(&a, 'f');
	keyp(&a, WF_KEY_ENTER);
	ch(&a, ' ');
	CHECK(1 == wf_sel_count(&a.sel, a.cwd) && a.cur.filter[0] &&
	        WF_XFER_COPY == a.slot.mode);
	keyp(&a, WF_KEY_ESC); /* the mark only */
	CHECK(0 == wf_sel_count(&a.sel, a.cwd) && a.cur.filter[0] &&
	        WF_XFER_COPY == a.slot.mode);
	keyp(&a, WF_KEY_ESC); /* the filter only */
	CHECK(!a.cur.filter[0] && WF_XFER_COPY == a.slot.mode);
	keyp(&a, WF_KEY_ESC); /* the pending copy last */
	CHECK(WF_XFER_NONE == a.slot.mode);
	keyp(&a, WF_KEY_ESC); /* nothing left */
	CHECK(WF_XFER_NONE == a.slot.mode && &WF_MODE_NORMAL == a.mode);
	/* a mark the filter hides is still the first thing Esc clears */
	ch(&a, ' ');
	ch(&a, '/');
	ch(&a, 'z');
	keyp(&a, WF_KEY_ENTER);
	CHECK(0 == a.cur.nvis && 1 == wf_sel_count(&a.sel, a.cwd));
	keyp(&a, WF_KEY_ESC);
	CHECK(0 == wf_sel_count(&a.sel, a.cwd) && a.cur.filter[0]);
	wf_app_destroy(&a);
}

static void test_prune_on_reload(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	/* select a name that the listing does not contain, as if it was deleted
	 */
	CHECK(WF_OK == wf_sel_toggle(&a.sel, a.cwd, L"vanished.txt"));
	CHECK(1 == wf_sel_count(&a.sel, a.cwd));
	wf_app_reload(&a);
	CHECK(0 == wf_sel_count(&a.sel, a.cwd));
	wf_app_destroy(&a);
}

static void type_path(WfApp *app, const wchar_t *path)
{
	ch(app, 'g');
	for (size_t i = 0; path[i]; i++) {
		ch(app, (uint32_t)path[i]);
	}
	keyp(app, WF_KEY_ENTER);
}

/* Catches history changes on failed/stale requests, preserving forward
 * history after reload, and path input falling through to normal commands. */
static void test_path_and_history(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
	ch(&a, 'g');
	ch(&a, 'q');
	CHECK(0 == strcmp(a.mode->name, "PATH"));
	keyp(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode && !a.go.text);
	type_path(&a, L"dev");
	CHECK(0 == wcscmp(a.cwd, L"C:\\Users\\dev"));
	ch(&a, 'j');
	CHECK(0 == wcscmp(cur_name(&a), L"file2.txt"));
	ch(&a, 'b');
	CHECK(0 == wcscmp(a.cwd, L"C:\\Users"));
	ch(&a, 'r');
	ch(&a, 'f');
	CHECK(0 == wcscmp(a.cwd, L"C:\\Users\\dev") &&
	        0 == wcscmp(cur_name(&a), L"file2.txt"));
	type_path(&a, L"C:\\secret");
	/* a failed Go keeps the typed text and the old folder */
	CHECK(0 == wcscmp(a.cwd, L"C:\\Users\\dev") && &WF_MODE_GO == a.mode);
	keyp(&a, WF_KEY_ESC);
	ch(&a, 'b');
	CHECK(0 == wcscmp(a.cwd, L"C:\\Users"));
	type_path(&a, L"..");
	CHECK(0 == wcscmp(a.cwd, L"C:\\"));
	ch(&a, 'f');
	CHECK(0 == wcscmp(a.cwd, L"C:\\"));
	CHECK(2 ==
	        a.history_n); /* the new branch pruned the old forward entry */
	wf_app_destroy(&a);
}

static void test_unc_navigation_is_guarded(void)
{
	WfApp a;
	FakePort fp = {.app = &a, .defer = 1};
	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"\\\\host\\share"));
	CHECK(0 == fp.nq && &WF_MODE_CONFIRM == a.mode && a.flush_input);
	ch(&a, 'y');
	CHECK(0 == fp.nq);
	wf_app_on_presented(&a);
	a.now_ms = WF_CONFIRM_GUARD_MS;
	ch(&a, 'y');
	CHECK(1 == fp.nq);
	wf_app_destroy(&a);
	memset(&fp, 0, sizeof fp);
	fp.app = &a;
	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
	fp.defer = 1;
	type_path(&a, L"\\\\host\\share");
	CHECK(0 == fp.nq && &WF_MODE_CONFIRM == a.mode);
	keyp(&a, WF_KEY_ESC);
	CHECK(0 == fp.nq && 0 == wcscmp(a.cwd, L"C:\\Users"));
	wf_app_destroy(&a);
}

/* On a screen too small for the box, the one-line form still shows the choice. */
static void test_create_choice_on_small_screen(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfFrame *f;
	char row[1024];

	CHECK(WF_OK ==
	        wf_app_init(&a, 80, 10, fake_port(&fp), L"C:\\Users\\dev"));
	for (int folder = 0; folder < 2; folder++) {
		ch(&a, 'a');
		if (folder) {
			keyp(&a, WF_KEY_DOWN);
		}
		f = wf_frame_new(80, 10, &WF_STYLE_BASE);
		wf_app_draw(&a, f);
		row[0] = 0;
		for (int x = 0; x < f->cols; x++) {
			strcat_s(row, sizeof row,
			        f->cells[(f->rows - 2) * f->cols + x].utf8);
		}
		wf_frame_free(f);
		CHECK(NULL != strstr(row, folder ? "> d" : "> %"));
		CHECK(NULL == strstr(row, folder ? "> %" : "> d"));
		keyp(&a, WF_KEY_ESC);
	}
	wf_app_destroy(&a);
}

/* The text of row `y` of the whole screen. */
static void screen_row(const WfApp *a, int y, char *out, size_t cap)
{
	WfFrame *f = wf_frame_new(a->cols, a->rows, &WF_STYLE_BASE);

	out[0] = 0;
	wf_app_draw(a, f);
	for (int x = 0; x < f->cols; x++) {
		strcat_s(out, cap, f->cells[y * f->cols + x].utf8);
	}
	wf_frame_free(f);
}

/* The help has a search box that is always there, and a search lists the keys
 * that are the text itself before the ones that only hold it, and before the
 * words. */
static void test_help_search_box_and_order(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	char row[1024];

	CHECK(WF_OK ==
	        wf_app_init(&a, 100, 30, fake_port(&fp), L"C:\\Users\\dev"));
	ch(&a, '?');
	screen_row(&a, 1, row, sizeof row);
	CHECK(NULL != strstr(row, "Search:") && NULL == strstr(row, "Found"));
	ch(&a, 'g');
	screen_row(&a, 1, row, sizeof row);
	CHECK(NULL != strstr(row, "Search: g") &&
	        NULL != strstr(row, "Found: "));
	screen_row(&a, 3, row, sizeof row); /* the key g itself comes first */
	CHECK(NULL != strstr(row, "Smart Go: type a place"));
	ch(&a, 'o'); /* "go": no key is that, so the words answer */
	screen_row(&a, 3, row, sizeof row);
	CHECK(NULL != strstr(row, "Go to") || NULL != strstr(row, "Smart Go"));
	keyp(&a, WF_KEY_ESC);
	screen_row(&a, 1, row, sizeof row);
	CHECK(NULL == strstr(row, "Found") && &WF_MODE_HELP == a.mode);
	keyp(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode);
	wf_app_destroy(&a);
}

static void test_gated_file_commands(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	ch(&a, 'j');
	ch(&a, 'l');
	CHECK(0 == a.msg[0]);
	keyp(&a, WF_KEY_ENTER);
	CHECK(NULL != strstr(a.msg, "opening files is not set up"));
	keyp(&a, WF_KEY_F2);
	CHECK(!strcmp(a.mode->name, "NAME"));
	keyp(&a, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == a.mode && strstr(a.msg, "BLOCKED"));
	keyp(&a, WF_KEY_F2);
	keyp(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode);
	ch(&a, 'a');
	CHECK(!strcmp(a.mode->name, "CREATE"));
	keyp(&a, WF_KEY_ESC);
	/* the create menu: arrows choose, Enter takes the marked line */
	ch(&a, 'a');
	keyp(&a, WF_KEY_ENTER);
	CHECK(!strcmp(a.mode->name, "NAME") && 0 == a.create_pick);
	keyp(&a, WF_KEY_ESC);
	ch(&a, 'a');
	keyp(&a, WF_KEY_DOWN);
	CHECK(1 == a.create_pick && !strcmp(a.mode->name, "CREATE"));
	keyp(&a, WF_KEY_ENTER);
	CHECK(!strcmp(a.mode->name, "NAME"));
	keyp(&a, WF_KEY_ESC);
	ch(&a, 'a');
	CHECK(0 == a.create_pick);
	/* the third stop is cancel: Enter closes the menu */
	keyp(&a, WF_KEY_UP);
	CHECK(2 == a.create_pick);
	keyp(&a, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == a.mode);
	ch(&a, 'a');
	keyp(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode);
	wf_app_destroy(&a);
}

static int status_has(const WfApp *a, const char *needle)
{
	WfFrame *f = wf_frame_new(140, 30, &WF_STYLE_BASE);
	char row[1024] = "";
	wf_app_draw(a, f);
	for (int x = 0; x < f->cols; x++) {
		strcat_s(row, sizeof row, f->cells[29 * f->cols + x].utf8);
	}
	wf_frame_free(f);
	return NULL != strstr(row, needle);
}

static bool header_has(const WfApp *app, int cols, const char *needle)
{
	WfFrame *f = wf_frame_new(cols, 30, &WF_STYLE_BASE);
	char row[1024] = "";
	wf_app_draw(app, f);
	for (int x = 0; x < f->cols; x++) {
		strcat_s(row, sizeof row, f->cells[x].utf8);
	}
	bool found = strstr(row, needle) != NULL;
	wf_frame_free(f);
	return found;
}

static void test_recovery_status_persists(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	CHECK(WF_OK ==
	        wf_app_init(&a, 140, 30, fake_port(&fp), L"C:\\Users\\dev"));
	a.recovery.unresolved = 7;
	WfInfo safety;
	wf_app_safety_info(&a, &safety);
	char expected[192];
	(void)wf_msgf(expected, sizeof expected, MSG_STATUS_RECOVERY_UNRESOLVED,
	        &WF_ARG_N(7), 1);
	bool counted = false;
	for (int i = 0; i < safety.n; i++) {
		counted |= !strcmp(safety.text[i], expected);
	}
	CHECK(counted);
	CHECK(header_has(&a, 140, wf_msg(MSG_SAFETY_REVIEW)));
	ch(&a, 'j');
	keyp(&a, WF_KEY_ENTER);
	CHECK(header_has(&a, 140, wf_msg(MSG_SAFETY_REVIEW)) &&
	        status_has(&a, "opening files is not set up"));
	ch(&a, 'k');
	CHECK(header_has(&a, 140, wf_msg(MSG_SAFETY_REVIEW)));
	a.recovery.unknown = 1;
	CHECK(header_has(&a, 140, wf_msg(MSG_SAFETY_REVIEW)));
	wf_app_destroy(&a);
}
typedef struct
{
	WfApp *app;
	size_t calls;
} AnyDirectory;
static WfStatus any_directory_submit(void *ctx,
        uint32_t generation,
        const wchar_t *path,
        const wchar_t *parent)
{
	AnyDirectory *port = ctx;
	WfDir *dir = wf_dir_new();
	(void)path;
	(void)parent;
	if (!dir) {
		return WF_E_NOMEM;
	}
	port->calls++;
	wf_app_on_listing(port->app, generation, WF_OK, dir, NULL);
	return WF_OK;
}

static void test_bounded_history_and_share_approval(void)
{
	WfApp a;
	AnyDirectory port = {&a, 0};
	WfFsPort fs = {&port, any_directory_submit};
	wchar_t path[64];
	CHECK(WF_OK == wf_app_init(&a, 120, 30, fs, L"C:\\"));
	for (size_t i = 0; i < 140; i++) {
		swprintf(path, 64, L"C:\\nav%zu", i);
		type_path(&a, path);
	}
	CHECK(128 == a.history_n && 127 == a.history_index);
	CHECK(0 == wcscmp(a.history[0].path, L"C:\\nav12"));
	for (size_t i = 0; i < 140; i++) {
		ch(&a, 'b');
	}
	CHECK(0 == wcscmp(a.cwd, L"C:\\nav12"));
	type_path(&a, L"\\\\host\\share");
	CHECK(&WF_MODE_CONFIRM == a.mode && 268 == port.calls);
	wf_app_on_presented(&a);
	a.now_ms += WF_CONFIRM_GUARD_MS;
	ch(&a, 'y');
	CHECK(&WF_MODE_NORMAL == a.mode && 269 == port.calls);
	type_path(&a, L"\\\\host\\share\\child");
	CHECK(&WF_MODE_NORMAL == a.mode && 270 == port.calls);
	type_path(&a, L"\\\\other\\share");
	CHECK(&WF_MODE_CONFIRM == a.mode && 270 == port.calls);
	keyp(&a, WF_KEY_ESC);
	CHECK(!a.pending_path && !a.pending_unc_root);
	a.confirm_unc_navigation = 0;
	type_path(&a, L"\\\\other\\share");
	CHECK(&WF_MODE_NORMAL == a.mode && 271 == port.calls);
	wf_app_destroy(&a);
}

static void test_path_editing_and_refusals(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\"));
	type_path(&a, L"Users");
	CHECK(0 == wcscmp(a.cwd, L"C:\\Users"));
	ch(&a, 'g');
	ch(&a, 0x1F600);
	CHECK(2 == a.go.len);
	keyp(&a, WF_KEY_BACKSPACE);
	CHECK(0 == a.go.len);
	ch(&a, 0xD800);
	CHECK(0 == a.go.len);
	for (size_t i = 0; i < 5000; i++) {
		ch(&a, 'x');
	}
	CHECK(5000 == a.go.len);
	keyp(&a, WF_KEY_ESC);
	type_path(&a, L"C:/Users/dev");
	CHECK(0 == wcscmp(a.cwd, L"C:\\Users\\dev"));
	fp.defer = 1;
	type_path(&a, L"C:\\Users\\..\\secret");
	/* ".." is resolved in the text: the listing asked for is C:\secret */
	CHECK(1 == fp.nq && !wcscmp(fp.q[0].path, L"C:\\secret"));
	keyp(&a, WF_KEY_ESC);
	fp.nq = 0;
	type_path(&a, L"\\\\?\\UNC\\host");
	CHECK(0 == strcmp(a.mode->name, "PATH") && 0 == fp.nq);
	keyp(&a, WF_KEY_ESC);
	type_path(&a, L"\\\\.\\pipe\\name");
	CHECK(0 == strcmp(a.mode->name, "PATH") && 0 == fp.nq);
	keyp(&a, WF_KEY_ESC);
	type_path(&a, L"C:\\*");
	CHECK(0 == strcmp(a.mode->name, "PATH") && 0 == fp.nq);
	keyp(&a, WF_KEY_ESC);
	wf_app_destroy(&a);
	memset(&fp, 0, sizeof fp);
	fp.app = &a;
	fp.defer = 1;
	CHECK(WF_E_INVAL ==
	                wf_app_init(&a, 120, 30, fake_port(&fp),
	                        L"\\\\?\\UNC\\host") &&
	        0 == fp.nq);
}

static void test_transfer_refused_while_listing_pending(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	FakeXfer xp = {.app = &a};

	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
	wf_app_set_xfer(&a, fake_xfer_port(&xp));
	fp.defer = 1;
	ch(&a, 'l');
	CHECK(NULL != a.want_path);

	ch(&a, 'y');
	CHECK(0 == xp.capture_calls);
	CHECK(NULL != strstr(a.msg, "Updating") ||
	        NULL != strstr(a.msg, "STALE") ||
	        NULL != strstr(a.msg, "loading") ||
	        NULL != strstr(a.msg, "LOADING"));

	wf_slot_init(&a.slot);
	{
		static FakeProbe probe;
		WfProbeOps ops;
		WfTransferSlot slot;
		const wchar_t *const names[] = {L"dev"};

		fp_init(&probe);
		fp_dir(&probe, L"C:\\Users", 2);
		fp_file(&probe, L"C:\\Users\\dev", 0, 3);
		ops = fp_ops(&probe);
		wf_slot_init(&slot);
		CHECK(wf_xfer_capture(&ops, WF_XFER_COPY, L"C:\\Users", names,
		              1, 0, &slot) == WF_OK);
		wf_slot_free(&a.slot);
		a.slot = slot;
	}
	a.xfer_busy = WF_XB_IDLE;
	ch(&a, 'p');
	CHECK(0 == xp.preflight_calls);
	CHECK(NULL != strstr(a.msg, "Updating") ||
	        NULL != strstr(a.msg, "STALE") ||
	        NULL != strstr(a.msg, "loading") ||
	        NULL != strstr(a.msg, "LOADING"));

	fake_deliver(&fp, 0);
	CHECK(NULL == a.want_path && 0 == wcscmp(a.cwd, L"C:\\Users\\dev"));
	a.xfer_busy = WF_XB_IDLE;
	ch(&a, 'y');
	CHECK(1 == xp.capture_calls && 1 == xp.last_count &&
	        0 == wcscmp(xp.last_dir, L"C:\\Users\\dev"));
	wf_app_destroy(&a);
}

static void test_filter_arrow_movement_and_escape(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	ch(&a, '/');
	ch(&a, 'f');
	ch(&a, 'i');
	ch(&a, 'l');
	ch(&a, 'e');
	wchar_t before[64], after_down[64];

	CHECK(&WF_MODE_FILTER == a.mode && 2 == a.cur.nvis);
	wcsncpy_s(before, WF_ARRAY_LEN(before), cur_name(&a), _TRUNCATE);
	keyp(&a, WF_KEY_DOWN);
	wcsncpy_s(
	        after_down, WF_ARRAY_LEN(after_down), cur_name(&a), _TRUNCATE);
	CHECK(&WF_MODE_FILTER == a.mode && 0 != wcscmp(after_down, before));
	keyp(&a, WF_KEY_UP);
	CHECK(&WF_MODE_FILTER == a.mode && 0 == wcscmp(cur_name(&a), before));
	keyp(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode && L'\0' == a.cur.filter[0]);
	wf_app_destroy(&a);
}

static void test_path_cursor_editing_and_surrogate_boundaries(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
	ch(&a, 'g');
	ch(&a, 'a');
	ch(&a, 'b');
	keyp(&a, WF_KEY_LEFT);
	ch(&a, 'X');
	CHECK(3 == a.go.len && 0 == wcscmp(a.go.text, L"aXb"));
	keyp(&a, WF_KEY_BACKSPACE);
	CHECK(2 == a.go.len && 0 == wcscmp(a.go.text, L"ab"));
	ch(&a, 0x1F600);
	CHECK(4 == a.go.len && L'a' == a.go.text[0] &&
	        a.go.text[1] == (wchar_t)0xD83D &&
	        a.go.text[2] == (wchar_t)0xDE00 && L'b' == a.go.text[3] &&
	        L'\0' == a.go.text[4]);
	keyp(&a, WF_KEY_BACKSPACE);
	CHECK(2 == a.go.len && 0 == wcscmp(a.go.text, L"ab"));
	keyp(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode && NULL == a.go.text && 0 == a.go.len &&
	        0 == a.go.cap);
	wf_app_destroy(&a);
}

static void test_path_prompt_cursor_fits_at_minimum_width(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfFrame *f;

	CHECK(WF_OK == wf_app_init(&a, 4, 30, fake_port(&fp), L"C:\\Users"));
	ch(&a, 'g');
	CHECK(NULL != a.mode->name && 0 == strcmp(a.mode->name, "PATH"));
	f = wf_frame_new(4, 30, &WF_STYLE_BASE);
	CHECK(NULL != f);
	if (f) {
		wf_app_draw(
		        &a, f); /* too narrow for the panel: nothing breaks */
		wf_frame_free(f);
	}
	wf_app_destroy(&a);
}

static int status_row_contains(const WfApp *a, int cols, const char *needle)
{
	WfFrame *f = wf_frame_new(cols, 30, &WF_STYLE_BASE);
	char row[1024] = "";
	int found;

	wf_app_draw(a, f);
	for (int x = 0; x < f->cols; x++) {
		strcat_s(row, sizeof row,
		        f->cells[(f->rows - 1) * f->cols + x].utf8);
	}
	found = NULL != strstr(row, needle);
	wf_frame_free(f);
	return found;
}

static int title_row_contains(const WfApp *a, const char *needle)
{
	WfFrame *f = wf_frame_new(120, 30, &WF_STYLE_BASE);
	char row[1024] = "";
	int found;

	wf_app_draw(a, f);
	for (int x = 0; x < f->cols; x++) {
		strcat_s(row, sizeof row, f->cells[x].utf8);
	}
	found = NULL != strstr(row, needle);
	wf_frame_free(f);
	return found;
}

/* Everything the winning navigation committed, so a later stale answer can be
 * shown to have changed none of it. */
typedef struct
{
	WfDir *dir, *parent;
	size_t cursor, top, nvis, history_n, history_index;
	uint32_t cwd_version;
	WfTransferMode slot_mode;
	WfSourceSnap *slot_src;
	size_t slot_count;
	wchar_t cwd[64], history0[64], history1[64], history0_select[64];
} Committed;

static void capture_committed(const WfApp *a, Committed *c)
{
	c->dir = a->cur.dir;
	c->parent = a->parent.dir;
	c->cursor = a->cur.cursor;
	c->top = a->cur.top;
	c->nvis = a->cur.nvis;
	c->history_n = a->history_n;
	c->history_index = a->history_index;
	c->cwd_version = a->cwd_version;
	c->slot_mode = a->slot.mode;
	c->slot_src = a->slot.src;
	c->slot_count = a->slot.count;
	CHECK(wcsncpy_s(c->cwd, WF_ARRAY_LEN(c->cwd), a->cwd, _TRUNCATE) == 0);
	CHECK(wcsncpy_s(c->history0, WF_ARRAY_LEN(c->history0),
	              a->history[0].path, _TRUNCATE) == 0);
	CHECK(wcsncpy_s(c->history1, WF_ARRAY_LEN(c->history1),
	              a->history[1].path, _TRUNCATE) == 0);
	CHECK(NULL != a->history[0].select);
	CHECK(wcsncpy_s(c->history0_select, WF_ARRAY_LEN(c->history0_select),
	              a->history[0].select, _TRUNCATE) == 0);
}

/* Nothing committed, requested or remembered has moved. */
static bool committed_unchanged(const WfApp *a, const Committed *c)
{
	return 0 == wcscmp(a->cwd, c->cwd) && NULL == a->want_path &&
	        NULL == a->want_select && NULL == a->want_history_path &&
	        SIZE_MAX == a->want_history_index && a->cur.dir == c->dir &&
	        a->parent.dir == c->parent && a->cur.cursor == c->cursor &&
	        a->cur.top == c->top && a->cur.nvis == c->nvis &&
	        a->cwd_version == c->cwd_version &&
	        a->history_n == c->history_n &&
	        a->history_index == c->history_index &&
	        0 == wcscmp(a->history[0].path, c->history0) &&
	        0 == wcscmp(a->history[1].path, c->history1) &&
	        0 == wcscmp(a->history[0].select, c->history0_select) &&
	        a->slot.mode == c->slot_mode && a->slot.src == c->slot_src &&
	        a->slot.count == c->slot_count;
}

/* The selection made before the navigation is still on the folder it was made
 * in. */
static void selection_kept(
        const WfApp *a, const wchar_t names[][64], size_t count)
{
	const WfNameSet *kept = wf_sel_folder(&a->sel, L"C:\\Users\\dev");

	CHECK(NULL != kept && kept->n == count);
	for (size_t i = 0; i < count; i++) {
		CHECK(wf_nameset_has(kept, names[i]));
	}
}

/* While the newest request is pending the old snapshot stays browseable and
 * is marked loading. */
static void old_screen_is_browseable_and_marked_loading(
        WfApp *a, WfDir *old_dir)
{
	ch(a, 'j');
	ch(a, '/');
	ch(a, 'f');
	keyp(a, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == a->mode && a->cur.dir == old_dir &&
	        2 == a->cur.nvis && 0 == wcscmp(a->cur.filter, L"f") &&
	        0 == wcscmp(cur_name(a), L"file2.txt"));
	CHECK(status_row_contains(a, 120, "LOADING"));
	CHECK(!status_row_contains(a, 120, "READY"));
	CHECK(title_row_contains(a, "C:\\Users\\dev"));
}

/* Browsing is allowed while stale; every transfer admission is refused. */
static void transfers_are_refused_while_stale(WfApp *a, const FakeXfer *xp)
{
	ch(a, 'y');
	ch(a, 'x');
	wf_slot_init(&a->slot);
	a->slot.mode = WF_XFER_COPY;
	a->slot.src = calloc(1, sizeof *a->slot.src);
	CHECK(NULL != a->slot.src);
	a->slot.count = a->slot.src ? 1 : 0;
	ch(a, 'p');
	CHECK(0 == xp->capture_calls && 0 == xp->preflight_calls);
	CHECK(NULL != strstr(a->msg, "Updating") ||
	        NULL != strstr(a->msg, "STALE") ||
	        NULL != strstr(a->msg, "loading") ||
	        NULL != strstr(a->msg, "LOADING"));
}

static void test_rapid_navigation_keeps_old_screen_marked_loading(void)
{
	WfApp a;
	FakePort fp = {.app = &a, .defer = 1};
	FakeXfer xp = {.app = &a};
	const WfNameSet *selected;
	WfDir *old_dir;
	wchar_t selected_names[2][64];
	size_t old_history_n, old_history_index, selected_count;
	uint32_t old_cwd_version;
	Committed winning;

	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	fake_deliver(&fp, 0);
	old_dir = a.cur.dir;
	old_history_n = a.history_n;
	old_history_index = a.history_index;
	old_cwd_version = a.cwd_version;
	CHECK(NULL != old_dir && 3 == a.cur.nvis);
	CHECK(0 == wcscmp(a.cwd, L"C:\\Users\\dev"));
	CHECK(WF_OK == wf_sel_toggle(&a.sel, a.cwd, L"Docs"));
	CHECK(WF_OK == wf_sel_toggle(&a.sel, a.cwd, L"file2.txt"));
	selected = wf_sel_folder(&a.sel, a.cwd);
	CHECK(NULL != selected && 2 == selected->n);
	selected_count = selected ? selected->n : 0;
	for (size_t i = 0; i < selected_count && i < 2; i++) {
		CHECK(wcsncpy_s(selected_names[i],
		              WF_ARRAY_LEN(selected_names[i]),
		              selected->names[i], _TRUNCATE) == 0);
	}

	wf_app_set_xfer(&a, fake_xfer_port(&xp));
	CHECK(WF_OK == wf_app_navigate(&a, L"C:\\Users", NULL, SIZE_MAX));
	CHECK(WF_OK == wf_app_navigate(&a, L"C:\\", NULL, SIZE_MAX));
	CHECK(WF_OK == wf_app_navigate(&a, L"C:\\Users", L"dev", SIZE_MAX));
	CHECK(4 == fp.nq && NULL != a.want_path &&
	        0 == wcscmp(a.want_path, L"C:\\Users"));

	old_screen_is_browseable_and_marked_loading(&a, old_dir);
	transfers_are_refused_while_stale(&a, &xp);

	/* The newest answer commits first, restores the requested cursor, and
	 * clears loading. */
	fake_deliver(&fp, 3);
	CHECK(0 == wcscmp(a.cwd, L"C:\\Users") && NULL == a.want_path);
	CHECK(a.cwd_version == old_cwd_version + 1 &&
	        a.history_n == old_history_n + 1 &&
	        a.history_index == old_history_index + 1 &&
	        0 == wcscmp(a.history[0].path, L"C:\\Users\\dev") &&
	        0 == wcscmp(a.history[1].path, L"C:\\Users"));
	CHECK(0 == wcscmp(cur_name(&a), L"dev") && L'\0' == a.cur.filter[0]);
	CHECK(0 == wf_sel_count(&a.sel, L"C:\\Users"));
	selection_kept(&a, selected_names, selected_count);
	capture_committed(&a, &winning);

	/* Late stale answers are owned and discarded without changing committed
	 * or selected state. */
	for (int late = 2; late >= 1; late--) {
		fake_deliver(&fp, late);
		CHECK(committed_unchanged(&a, &winning));
		selection_kept(&a, selected_names, selected_count);
	}
	wf_app_destroy(&a);
}
static void test_status_priority_and_state_labels(void)
{
	const int widths[] = {140, 40};

	for (size_t i = 0; i < WF_ARRAY_LEN(widths); i++) {
		WfApp a;
		FakePort fp = {.app = &a};

		CHECK(wf_app_init(&a, widths[i], 30, fake_port(&fp),
		              L"C:\\Users\\dev") == WF_OK);
		CHECK(status_row_contains(&a, widths[i], "NORMAL"));
		CHECK(status_row_contains(&a, widths[i], "READY"));
		CHECK(status_row_contains(&a, widths[i], "? Help"));
		if (140 == widths[i]) {
			CHECK(status_row_contains(&a, widths[i], "? Help"));
			a.history_n = 2;
			a.history_index = 1;
			CHECK(!status_row_contains(&a, widths[i], "BACK"));
			CHECK(!status_row_contains(&a, widths[i], "FORWARD"));
			a.slot.mode = WF_XFER_COPY;
			a.slot.src = calloc(3, sizeof *a.slot.src);
			a.slot.count = 3;
			CHECK(status_row_contains(
			        &a, widths[i], "COPY 3 items"));
			a.mode = &WF_MODE_FILTER;
			CHECK(status_row_contains(&a, widths[i],
			        "arrows move  Enter keep  Esc clear"));
			a.mode = &WF_MODE_PREVIEW;
			CHECK(status_row_contains(&a, widths[i], "Esc Close"));
			a.mode = &WF_MODE_NORMAL;
		}

		fp.defer = 1;
		ch(&a, 'l');
		CHECK(status_row_contains(&a, widths[i], "LOADING"));
		CHECK(!status_row_contains(&a, widths[i], "READY"));

		strcpy_s(a.msg, sizeof a.msg, "ERROR: transfer blocked");
		CHECK(status_row_contains(
		        &a, widths[i], "ERROR: transfer blocked"));
		CHECK(!status_row_contains(&a, widths[i], "LOADING"));
		CHECK(!status_row_contains(&a, widths[i], "READY"));

		a.msg[0] = '\0';
		a.recovery.unknown = 1;
		CHECK(header_has(&a, widths[i], wf_msg(MSG_SAFETY_REVIEW)));
		CHECK(!status_row_contains(
		        &a, widths[i], wf_msg(MSG_SAFETY_REVIEW)));
		CHECK(status_row_contains(&a, widths[i], "LOADING"));
		CHECK(!status_row_contains(&a, widths[i], "READY"));
		if (40 == widths[i]) {
			strcpy_s(
			        a.msg, sizeof a.msg, "ERROR: transfer blocked");
			CHECK(status_row_contains(
			        &a, widths[i], "ERROR: transfer blocked"));
			CHECK(header_has(
			        &a, widths[i], wf_msg(MSG_SAFETY_REVIEW)));
		}
		wf_app_destroy(&a);
	}
}

static void test_minimal_status_priority(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	CHECK(WF_OK ==
	        wf_app_init(&a, 20, 30, fake_port(&fp), L"C:\\Users\\dev"));
	strcpy_s(a.msg, sizeof a.msg, "ERROR: transfer blocked");
	CHECK(status_row_contains(&a, 20, "ERROR"));
	a.msg[0] = '\0';
	a.recovery.unknown = 1;
	CHECK(header_has(&a, 20, wf_msg(MSG_SAFETY_REVIEW)));
	CHECK(status_row_contains(&a, 20, "READY"));
	a.recovery.unknown = 0;
	fp.defer = 1;
	ch(&a, 'l');
	CHECK(status_row_contains(&a, 20, "LOADING"));
	a.slot.mode = WF_XFER_COPY;
	a.slot.src = calloc(2, sizeof *a.slot.src);
	a.slot.count = 2;
	CHECK(status_row_contains(&a, 20, "COPY"));
	wf_app_destroy(&a);
}

static void test_combined_priority_markers_at_min_width(void)
{
	const int widths[] = {4, 5, 6};

	for (size_t i = 0; i < WF_ARRAY_LEN(widths); i++) {
		WfApp a;
		FakePort fp = {.app = &a};

		CHECK(wf_app_init(&a, widths[i], 30, fake_port(&fp),
		              L"C:\\Users\\dev") == WF_OK);
		strcpy_s(a.msg, sizeof a.msg, "ERROR: transfer blocked");
		a.recovery.unknown = 1;
		CHECK(header_has(&a, widths[i], "N"));
		CHECK(status_row_contains(&a, widths[i], "ER"));
		wf_app_destroy(&a);
	}
}

/* Invalid events must not introduce embedded NULs or unpaired surrogates. */
static void test_invalid_scalars_leave_input_unchanged(void)
{
	const uint32_t invalid[] = {
	        0, 31, 127, 0xD800, 0xDFFF, 0x110000, UINT32_MAX};
	WfApp a;
	FakePort fp = {.app = &a};

	CHECK(WF_OK == wf_app_init(&a, 160, 30, fake_port(&fp), L"C:\\Users"));
	for (size_t i = 0; i < WF_ARRAY_LEN(invalid); i++) {
		ch(&a, '/');
		ch(&a, 'a');
		ch(&a, invalid[i]);
		CHECK(&WF_MODE_FILTER == a.mode && !wcscmp(a.cur.filter, L"a"));
		keyp(&a, WF_KEY_ESC);
		ch(&a, 'g');
		ch(&a, 'a');
		ch(&a, invalid[i]);
		CHECK(!wcscmp(a.go.text, L"a") && 1 == a.go.len &&
		        1 == a.go.cursor);
		keyp(&a, WF_KEY_ESC);
	}
	wf_app_destroy(&a);
}

static void test_path_cursor_endpoints_and_pair_motion(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	const size_t left[] = {3, 1, 0, 0};
	const size_t right[] = {1, 3, 4, 4};

	CHECK(WF_OK == wf_app_init(&a, 160, 30, fake_port(&fp), L"C:\\Users"));
	ch(&a, 'g');
	keyp(&a, WF_KEY_LEFT);
	keyp(&a, WF_KEY_RIGHT);
	keyp(&a, WF_KEY_BACKSPACE);
	CHECK(0 == a.go.cursor && 0 == a.go.len);
	ch(&a, 'a');
	ch(&a, 0x1F600);
	ch(&a, 'b');
	for (size_t i = 0; i < WF_ARRAY_LEN(left); i++) {
		keyp(&a, WF_KEY_LEFT);
		CHECK(a.go.cursor == left[i]);
	}
	for (size_t i = 0; i < WF_ARRAY_LEN(right); i++) {
		keyp(&a, WF_KEY_RIGHT);
		CHECK(a.go.cursor == right[i]);
	}
	keyp(&a, WF_KEY_LEFT);
	keyp(&a, WF_KEY_LEFT);
	ch(&a, 0x4E2D);
	CHECK(2 == a.go.cursor && 5 == a.go.len &&
	        a.go.text[1] == (wchar_t)0x4E2D &&
	        a.go.text[2] == (wchar_t)0xD83D &&
	        a.go.text[3] == (wchar_t)0xDE00 && L'b' == a.go.text[4]);
	keyp(&a, WF_KEY_BACKSPACE);
	CHECK(1 == a.go.cursor && 4 == a.go.len);
	keyp(&a, WF_KEY_RIGHT);
	keyp(&a, WF_KEY_BACKSPACE);
	CHECK(1 == a.go.cursor && !wcscmp(a.go.text, L"ab"));
	keyp(&a, WF_KEY_RIGHT);
	keyp(&a, WF_KEY_BACKSPACE);
	keyp(&a, WF_KEY_BACKSPACE);
	keyp(&a, WF_KEY_BACKSPACE);
	CHECK(0 == a.go.len && 0 == a.go.cursor && !a.go.text[0]);
	keyp(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode && NULL == a.go.text);
	wf_app_destroy(&a);
}

static void test_mode_hints_and_actual_listing_error(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfDir *committed;

	CHECK(WF_OK == wf_app_init(&a, 180, 30, fake_port(&fp), L"C:\\Users"));
	CHECK(status_row_contains(&a, 180, "NORMAL READY"));
	ch(&a, '/');
	CHECK(status_row_contains(&a, 180, "FILTER READY"));
	CHECK(status_row_contains(&a, 180, "Enter keep  Esc clear"));
	ch(&a, 'd');
	keyp(&a, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == a.mode && !wcscmp(a.cur.filter, L"d"));
	ch(&a, '/');
	keyp(&a, WF_KEY_ESC);
	ch(&a, 'g');
	CHECK(title_row_contains(&a, "PATH"));
	a.msg[0] = 0;
	CHECK(status_row_contains(&a, 180, "PATH READY"));
	CHECK(status_row_contains(
	        &a, 180, "arrows edit  Enter go  Esc cancel"));
	keyp(&a, WF_KEY_ESC);
	ch(&a, 'v');
	CHECK(status_row_contains(&a, 180, "PREVIEW READY"));
	keyp(&a, WF_KEY_ESC);
	committed = a.cur.dir;
	fp.defer = 1;
	wf_app_go(&a, L"C:\\secret", NULL);
	CHECK(status_row_contains(&a, 180, "LOADING"));
	fake_deliver(&fp, 0);
	CHECK(!a.want_path && a.cur.dir == committed &&
	        !wcscmp(a.cwd, L"C:\\Users"));
	CHECK(status_row_contains(&a, 180, "ERROR ACCESS"));
	CHECK(!status_row_contains(&a, 180, "READY"));
	CHECK(!status_row_contains(&a, 180, "LOADING"));
	wf_app_reload(&a);
	fake_deliver(&fp, 1);
	CHECK(status_row_contains(&a, 180, "NORMAL READY") && !a.msg[0]);
	wf_app_destroy(&a);
}

static void test_unicode_clipping_stays_on_scalar_boundaries(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	const int widths[] = {1, 4, 20, 40, 80, 160};
	char message[400];

	CHECK(WF_OK == wf_app_init(&a, 160, 30, fake_port(&fp), L"C:\\Users"));
	ch(&a, 'g');
	for (int i = 0; i < 40; i++) {
		ch(&a, 0x4E2D);
		ch(&a, 0x1F600);
	}
	for (int i = 0; i < 37; i++) {
		keyp(&a, WF_KEY_LEFT);
	}
	for (size_t i = 0; i < 50; i++) {
		memcpy(message + i * 7, "\xE4\xB8\xAD\xF0\x9F\x98\x80", 7);
	}
	message[350] = 0;
	wf_app_set_msg(&a, message);
	for (size_t i = 0; i < WF_ARRAY_LEN(widths); i++) {
		WfFrame *f = wf_frame_new(widths[i], 30, &WF_STYLE_BASE);

		CHECK(NULL != f);
		if (!f) {
			continue;
		}
		wf_app_draw(&a, f);
		for (int cell = 0; cell < f->cols * f->rows; cell++) {
			const char *text = f->cells[cell].utf8;
			size_t len = strlen(text), pos = 0;

			while (pos < len) {
				uint32_t cp;
				pos += wf_utf8_decode(
				        text + pos, len - pos, &cp);
				CHECK(WF_CP_REPLACEMENT != cp);
			}
		}
		wf_frame_free(f);
	}
	wf_app_destroy(&a);
}

static void test_empty_committed_destination_allows_preflight(void)
{
	WfApp a;
	AnyDirectory directory = {&a, 0};
	WfFsPort fs = {&directory, any_directory_submit};
	FakeXfer transfer = {.app = &a};

	CHECK(WF_OK == wf_app_init(&a, 160, 30, fs, L"C:\\empty"));
	wf_app_set_xfer(&a, fake_xfer_port(&transfer));
	CHECK(NULL != a.cur.dir && !a.want_path && 0 == a.cur.nvis);
	ch(&a, 'y');
	CHECK(0 == transfer.capture_calls && WF_XB_IDLE == a.xfer_busy);
	a.slot.mode = WF_XFER_COPY;
	a.slot.src = calloc(1, sizeof *a.slot.src);
	CHECK(NULL != a.slot.src);
	a.slot.count = a.slot.src ? 1 : 0;
	ch(&a, 'p');
	CHECK(1 == transfer.preflight_calls && WF_XB_PREFLIGHT == a.xfer_busy &&
	        !wcscmp(transfer.last_dir, L"C:\\empty"));
	wf_app_destroy(&a);
}

/* q leaves at once when idle, and asks first while work is running. */
static void test_quit_asks_while_work_is_running(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfEvent q = {WF_EV_CHAR, 'q', WF_KEY_NONE, 0};

	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
	CHECK(WF_ACT_QUIT == a.mode->on_event(&a, &q));
	a.copy_jobs = 2;
	CHECK(WF_ACT_QUIT != a.mode->on_event(&a, &q));
	CHECK(&WF_MODE_CONFIRM == a.mode && !a.quit_now);
	CHECK(NULL != strstr(a.confirm.lines[1], "2 copy job"));
	keyp(&a, WF_KEY_ESC); /* no: nothing leaves */
	CHECK(&WF_MODE_NORMAL == a.mode && !a.quit_now);
	CHECK(WF_ACT_QUIT != a.mode->on_event(&a, &q));
	a.confirm.state = WF_CONF_ARMED;
	ch(&a, 'y');
	CHECK(a.quit_now);
	a.copy_jobs = 0;
	a.move.active = true;
	a.move.moved = 3;
	a.move.slot.count = 7;
	a.quit_now = false;
	a.mode = &WF_MODE_NORMAL;
	CHECK(WF_ACT_QUIT != a.mode->on_event(&a, &q));
	CHECK(NULL != strstr(a.confirm.lines[1], "3 of 7"));
	a.move.active = false;
	a.mode = &WF_MODE_NORMAL;
	wf_app_destroy(&a);
}

/* The sort menu orders folders with the files only when
 * `[files] directories_first` is off; the setting is read at each sort. */
static void test_sort_follows_directories_first(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfConfig cfg;
	WfDir *d = wf_dir_new();
	static const wchar_t *const ordered[] = {L"a.txt", L"mid", L"z.txt"};

	wf_config_init(&cfg);
	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\"));
	a.config = &cfg;
	CHECK(NULL != d);
	CHECK(WF_OK == wf_dir_add(d, L"z.txt", 5, 0, 1, 1));
	CHECK(WF_OK == wf_dir_add(d, L"mid", 3, WF_ENT_DIR, 0, 0));
	CHECK(WF_OK == wf_dir_add(d, L"a.txt", 5, 0, 1, 1));
	CHECK(WF_OK == wf_view_set_dir(&a.cur, d, NULL));

	wf_app_set_sort(&a, WF_SORT_NAME, false);
	CHECK(0 == wcscmp(a.cur.dir->e[0].name, L"mid")); /* the default */
	cfg.directories_first = 0;
	wf_app_set_sort(&a, WF_SORT_NAME, false);
	for (size_t i = 0; i < WF_ARRAY_LEN(ordered); i++) {
		CHECK(0 == wcscmp(a.cur.dir->e[i].name, ordered[i]));
	}
	cfg.directories_first = 1;
	wf_app_set_sort(&a, WF_SORT_NAME, false);
	CHECK(0 == wcscmp(a.cur.dir->e[0].name, L"mid"));
	a.config = NULL; /* the config is the test's own */
	wf_app_destroy(&a);
}

/* The bottom row of a frame, as text. */
static void bottom_row(const WfFrame *f, char *row, size_t cap)
{
	row[0] = '\0';
	for (int x = 0; x < f->cols; x++) {
		strcat_s(row, cap, f->cells[(f->rows - 1) * f->cols + x].utf8);
	}
}

/* The number after "visible " in the status row, or 0. */
static size_t visible_in(const char *row)
{
	const char *at = strstr(row, "visible ");

	return at ? (size_t)strtoul(at + 8, NULL, 10) : 0;
}

static double frames_ms(WfApp *a, WfFrame *f, int n)
{
	LARGE_INTEGER t0, t1, freq;

	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	for (int i = 0; i < n; i++) {
		wf_app_draw(a, f);
	}
	QueryPerformanceCounter(&t1);
	return (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
	        (double)freq.QuadPart / n;
}

/* The status line counts the selection and the hidden entries of the whole list.
 * That is done when the list or the selection changes (wf_app_sync_counts), not
 * at every frame; and the line says the same either way. */
static void test_status_counts_are_not_made_at_every_frame(void)
{
	enum
	{
		N = 200000,
		SELECTED = 3000
	};
	WfApp a;
	FakePort fp = {.app = &a};
	WfDir *d = wf_dir_new();
	WfFrame *f;
	char before[512], after[512], now[512];
	size_t visible_before;
	double slow, fast;

	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	for (unsigned i = 0; i < N; i++) {
		wchar_t name[16];

		swprintf(name, 16, L"f%06u", i);
		CHECK(WF_OK ==
		        wf_dir_add(d, name, wcslen(name),
		                0 == i % 10 ? WF_ENT_HIDDEN : 0, 1, 1));
	}
	CHECK(WF_OK == wf_view_set_dir(&a.cur, d, NULL));
	for (unsigned i = 1; i <= SELECTED; i++) {
		wchar_t name[16];

		swprintf(name, 16, L"f%06u", 7 * i); /* some are hidden ones */
		CHECK(WF_OK == wf_sel_toggle(&a.sel, a.cwd, name));
	}
	f = wf_frame_new(120, 30, &WF_STYLE_BASE);
	slow = frames_ms(
	        &a, f, 5); /* not synced: the frame counts, as it always did */
	bottom_row(f, before, sizeof before);
	wf_app_sync_counts(&a);
	fast = frames_ms(&a, f, 5);
	bottom_row(f, after, sizeof after);
	printf("info: status line, %d entries and %d selected: %.3f ms a frame "
	       "counting, %.3f ms from the counts\n",
	        (int)N, (int)SELECTED, slow, fast);
	CHECK(!strcmp(before, after)); /* the same words */
	CHECK(strstr(after, "selected 3000") != NULL);
	visible_before = visible_in(after);
	CHECK(visible_before > 0 &&
	        visible_before < SELECTED); /* some are hidden ones */
	CHECK(fast < slow);
	/* a change to the selection is seen at once, with or without the sync */
	CHECK(WF_OK ==
	        wf_sel_toggle(&a.sel, a.cwd, L"f000001")); /* a visible one */
	wf_app_draw(&a,
	        f); /* the counts held are of the selection before: not used */
	bottom_row(f, now, sizeof now);
	CHECK(strstr(now, "selected 3001") != NULL);
	CHECK(visible_before + 1 == visible_in(now));
	wf_app_sync_counts(&a);
	wf_app_draw(&a, f);
	bottom_row(f, now, sizeof now);
	CHECK(strstr(now, "selected 3001") != NULL);
	CHECK(visible_before + 1 == visible_in(now));
	{
		/* a change of the selection counts the selected names again and leaves
		 * the hidden entries alone (they are of the list, which is the same):
		 * a number put in their place is still there after it */
		LARGE_INTEGER t0, t1, freq;
		size_t real_hidden = a.counts.attr_hidden;

		CHECK(real_hidden == N / 10);
		a.counts.attr_hidden = 12345;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&t0);
		CHECK(WF_OK == wf_sel_toggle(&a.sel, a.cwd, L"f000002"));
		wf_app_sync_counts(&a);
		QueryPerformanceCounter(&t1);
		printf("info: counts after one selection change, %d entries: "
		       "%.3f ms\n",
		        (int)N,
		        (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
		                (double)freq.QuadPart);
		CHECK(12345 == a.counts.attr_hidden);
		CHECK(a.counts.sel_visible == visible_before + 2);
		a.cur.gen++; /* the list is another one: the hidden are counted */
		wf_app_sync_counts(&a);
		CHECK(real_hidden == a.counts.attr_hidden);
	}
	{
		/* Space on the cursor entry: the counts follow by one, without a walk,
		 * and are what a count of the list gives (selecting, then unselecting) */
		LARGE_INTEGER t0, t1, freq;
		size_t sel_before = a.counts.sel_visible;
		bool was = wf_nameset_has(wf_sel_folder(&a.sel, a.cwd),
		        wf_view_current(&a.cur)->name);

		for (int round = 0; round < 2; round++) {
			size_t kept;

			QueryPerformanceFrequency(&freq);
			QueryPerformanceCounter(&t0);
			wf_app_toggle_select(&a);
			QueryPerformanceCounter(&t1);
			CHECK(a.counts.valid && a.counts.sel == a.sel.gen);
			kept = a.counts.sel_visible;
			CHECK(kept ==
			        (0 == round ? (was ? sel_before - 1
			                           : sel_before + 1)
			                    : sel_before));
			a.counts.valid = false; /* the walk gives the same */
			wf_app_sync_counts(&a);
			CHECK(kept == a.counts.sel_visible);
			if (0 == round) {
				printf("info: Space, %d entries, counts kept: "
				       "%.3f ms\n",
				        (int)N,
				        (double)(t1.QuadPart - t0.QuadPart) *
				                1000.0 / (double)freq.QuadPart);
			}
			/* the cursor moved down: back up to the same entry */
			wf_view_move(&a.cur, -1, 20);
		}
	}
	wf_frame_free(f);
	wf_app_destroy(&a);
}

int main(void)
{
	/* Keep fixtures in separate stack frames. Optimized ASan otherwise
	 * inlines the suite into a near-1-MiB main frame. */
	void (*volatile cases[])(void) = {test_quit_asks_while_work_is_running,
	        test_empty_committed_destination_allows_preflight,
	        test_unicode_clipping_stays_on_scalar_boundaries,
	        test_invalid_scalars_leave_input_unchanged,
	        test_path_cursor_endpoints_and_pair_motion,
	        test_mode_hints_and_actual_listing_error,
	        test_stale_generation_is_dropped, test_failure_keeps_old_screen,
	        test_reload_keeps_cursor, test_selection,
	        test_status_counts_hidden_selection, test_filter_editing,
	        test_the_counts_are_shown_without_a_filter,
	        test_an_empty_list_says_why,
	        test_a_long_filter_keeps_its_end_and_the_cursor_on_screen,
	        test_an_applied_filter_is_told_in_a_narrow_status_line,
	        test_esc_cancels_selection_then_filter,
	        test_esc_peels_one_state_at_a_time, test_prune_on_reload,
	        test_path_and_history, test_unc_navigation_is_guarded,
	        test_gated_file_commands, test_help_search_box_and_order,
	        test_create_choice_on_small_screen,
	        test_recovery_status_persists,
	        test_bounded_history_and_share_approval,
	        test_path_editing_and_refusals,
	        test_transfer_refused_while_listing_pending,
	        test_filter_arrow_movement_and_escape,
	        test_path_cursor_editing_and_surrogate_boundaries,
	        test_path_prompt_cursor_fits_at_minimum_width,
	        test_rapid_navigation_keeps_old_screen_marked_loading,
	        test_status_priority_and_state_labels,
	        test_minimal_status_priority,
	        test_combined_priority_markers_at_min_width,
	        test_sort_follows_directories_first,
	        test_status_counts_are_not_made_at_every_frame};
	for (size_t i = 0; i < WF_ARRAY_LEN(cases); i++) {
		cases[i]();
	}
	return TEST_RESULT();
}

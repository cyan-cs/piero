#include <stdio.h>
#include <string.h>

#include "ui/wf_app.h"
#include "ui/wf_app_priv.h"
#include "ui/wf_layout.h"
#include "wf_fake_fs.h"
#include "wf_test.h"
#include "base/wf_macro.h"

static int panes(const WfLayout *l)
{
	int i, n = 0;

	for (i = 0; i < WF_PANE_COUNT; i++) {
		n += l->pane[i].w > 0;
	}
	return n;
}

/* Widths plus separators must tile the row exactly, with no overlap. */
static int tiles(const WfLayout *l, int cols)
{
	int i, end = 0, n = 0;

	for (i = 0; i < WF_PANE_COUNT; i++) {
		if (0 == l->pane[i].w) {
			continue;
		}
		if (n++ > 0) {
			end++; /* separator */
		}
		if (l->pane[i].x != end) {
			return 0;
		}
		end += l->pane[i].w;
	}
	return end == cols;
}

static void test_tiers(void)
{
	WfLayout l;

	wf_layout_compute(120, 30, &WF_RATIOS_DEFAULT, &l);
	CHECK(3 == panes(&l) && tiles(&l, 120));
	/* 118 content columns: 25/45/30 -> 29, 53, 35 +1 remainder to Current
	 */
	CHECK(29 == l.pane[WF_PANE_PARENT].w);
	CHECK(54 == l.pane[WF_PANE_CURRENT].w);
	CHECK(35 == l.pane[WF_PANE_PREVIEW].w);
	CHECK(3 == l.body_y && 25 == l.body_h);

	wf_layout_compute(100, 30, &WF_RATIOS_DEFAULT, &l);
	CHECK(3 == panes(&l) && tiles(&l, 100));
	CHECK(l.pane[WF_PANE_PARENT].w < l.pane[WF_PANE_PREVIEW].w);

	wf_layout_compute(70, 30, &WF_RATIOS_DEFAULT, &l);
	CHECK(2 == panes(&l) && 0 == l.pane[WF_PANE_PARENT].w && tiles(&l, 70));

	wf_layout_compute(50, 30, &WF_RATIOS_DEFAULT, &l);
	CHECK(1 == panes(&l) && 50 == l.pane[WF_PANE_CURRENT].w &&
	        tiles(&l, 50));

	wf_layout_compute(39, 30, &WF_RATIOS_DEFAULT, &l);
	CHECK(l.minimal);
	wf_layout_compute(120, 7, &WF_RATIOS_DEFAULT, &l);
	CHECK(l.minimal);
}

static void test_every_width_tiles(void)
{
	int cols;

	for (cols = 40; cols <= 400; cols++) {
		WfLayout l;

		wf_layout_compute(cols, 24, &WF_RATIOS_DEFAULT, &l);
		CHECK(tiles(&l, cols));
		CHECK(l.pane[WF_PANE_CURRENT].w > 0);
	}
}

static void test_bad_ratios(void)
{
	WfRatios zero = {0, 0, 0}, neg = {-5, 50, 55}, no_current = {50, 0, 50};
	WfLayout l, d;

	wf_layout_compute(120, 30, &WF_RATIOS_DEFAULT, &d);
	wf_layout_compute(120, 30, &zero, &l);
	CHECK(l.pane[WF_PANE_CURRENT].w == d.pane[WF_PANE_CURRENT].w);
	wf_layout_compute(120, 30, &neg, &l);
	CHECK(l.pane[WF_PANE_CURRENT].w == d.pane[WF_PANE_CURRENT].w);
	/* Current can never vanish */
	wf_layout_compute(120, 30, &no_current, &l);
	CHECK(l.pane[WF_PANE_CURRENT].w > 0 && tiles(&l, 120));
}

static void test_modes(void)
{
	WfApp app;
	FakePort fp = {.app = &app};
	WfEvent q = {WF_EV_CHAR, 'q', WF_KEY_NONE, 0};
	WfEvent help = {WF_EV_CHAR, '?', WF_KEY_NONE, 0};
	WfEvent any = {WF_EV_KEY, 0, WF_KEY_ESC, 0};
	WfFrame *f;
	int sizes[][2] = {{120, 30}, {44, 20}, {40, 8}, {20, 5}, {1, 1}};
	size_t i;

	CHECK(WF_OK ==
	        wf_app_init(&app, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	CHECK(WF_ACT_REDRAW == app.mode->on_event(&app, &help) &&
	        &WF_MODE_HELP == app.mode);
	/* typing narrows the help; the first Esc clears it, the next closes */
	{
		WfEvent u = {WF_EV_CHAR, 'u', WF_KEY_NONE, 0};

		CHECK(WF_ACT_REDRAW == app.mode->on_event(&app, &u) &&
		        !strcmp(app.help.q, "u") && &WF_MODE_HELP == app.mode);
		CHECK(WF_ACT_REDRAW == app.mode->on_event(&app, &any) &&
		        !app.help.q[0] && &WF_MODE_HELP == app.mode);
	}
	CHECK(WF_ACT_REDRAW == app.mode->on_event(&app, &any) &&
	        &WF_MODE_NORMAL == app.mode);
	CHECK(WF_ACT_QUIT == app.mode->on_event(&app, &q));

	/* asked from the preview, the help opens at the preview's keys */
	{
		WfEvent v = {WF_EV_CHAR, 'v', WF_KEY_NONE, 0};

		app.mode->on_event(&app, &v);
		CHECK(&WF_MODE_PREVIEW == app.mode);
		app.mode->on_event(&app, &help);
		CHECK(&WF_MODE_HELP == app.mode && app.help.cat > 0);
		app.mode->on_event(&app, &any);
		/* closed, it is back at the preview it was asked from, not at the
		 * list: the person goes on from where they were */
		CHECK(&WF_MODE_PREVIEW == app.mode);
		app.mode->on_event(&app, &help);
		app.mode->on_event(&app, &any);
		CHECK(&WF_MODE_PREVIEW == app.mode); /* and again, from there */
		app.mode = &WF_MODE_NORMAL;
	}

	/* drawing must be safe at every size, in every mode */
	for (i = 0; i < WF_ARRAY_LEN(sizes); i++) {
		f = wf_frame_new(sizes[i][0], sizes[i][1], &WF_STYLE_BASE);
		CHECK(NULL != f);
		app.mode = &WF_MODE_NORMAL;
		wf_app_draw(&app, f);
		app.mode = &WF_MODE_HELP;
		wf_app_draw(&app, f);
		wf_frame_free(f);
	}
	wf_app_destroy(&app);
}

/* On a narrow screen the keys and the description of an action share one
 * fitted line, and the explanation below still has it in full: End reaches
 * the same last action either way. */
static void help_wraps_on_a_narrow_screen(void)
{
	WfApp narrow, wide;
	FakePort fn = {.app = &narrow}, fw = {.app = &wide};
	WfEvent end = {WF_EV_KEY, 0, WF_KEY_END, 0};

	CHECK(WF_OK == wf_app_init(&narrow, 44, 20, fake_port(&fn),
	                       L"C:\\Users\\dev"));
	CHECK(WF_OK == wf_app_init(&wide, 120, 20, fake_port(&fw),
	                       L"C:\\Users\\dev"));
	wf_app_help_begin(&narrow);
	wf_app_help_begin(&wide);
	(void)narrow.mode->on_event(&narrow, &end);
	(void)wide.mode->on_event(&wide, &end);
	CHECK(narrow.help.sel > 0 && narrow.help.sel == wide.help.sel);
	{
		WfFrame *f = wf_frame_new(44, 20, &WF_STYLE_BASE);

		CHECK(NULL != f);
		wf_app_draw(&narrow, f);
		wf_frame_free(f);
	}
	wf_app_destroy(&narrow);
	wf_app_destroy(&wide);
}

/* A result longer than the screen scrolls, every line reachable; a key
 * that is not a scroll key closes it. */
static void a_long_report_scrolls_on_a_small_screen(void)
{
	WfApp app;
	FakePort fp = {.app = &app};
	WfEvent down = {WF_EV_KEY, 0, WF_KEY_DOWN, 0};
	WfEvent end = {WF_EV_KEY, 0, WF_KEY_END, 0};
	WfEvent other = {WF_EV_CHAR, 'x', WF_KEY_NONE, 0};
	WfFrame *f;

	CHECK(WF_OK == wf_app_init(&app, 44, 10, fake_port(&fp),
	                       L"C:\\Users\\dev"));
	for (int i = 0; i < WF_REPORT_LINES; i++) {
		snprintf(app.report[i], sizeof app.report[i], "line %d", i);
	}
	app.report_n = WF_REPORT_LINES;
	wf_app_report_show(&app);
	CHECK(&WF_MODE_REPORT == app.mode && 0 == app.report_top);
	(void)app.mode->on_event(&app, &down);
	CHECK(1 == app.report_top);
	(void)app.mode->on_event(&app, &end);
	CHECK(app.report_top > 1);
	f = wf_frame_new(44, 10, &WF_STYLE_BASE);
	CHECK(NULL != f);
	wf_app_draw(&app, f);
	wf_frame_free(f);
	(void)app.mode->on_event(&app, &other);
	CHECK(&WF_MODE_NORMAL == app.mode);
	wf_app_destroy(&app);
}

/* The details window keeps the line being read when the terminal is resized:
 * the text re-wraps, and the same line is still at the top. */
static void details_keep_their_line_across_a_resize(void)
{
	WfApp app;
	FakePort fp = {.app = &app};
	WfEvent resize = {WF_EV_RESIZE, 0, WF_KEY_NONE, 0};

	CHECK(WF_OK == wf_app_init(&app, 100, 12, fake_port(&fp),
	                       L"C:\\Users\\dev"));
	wf_app_info_begin(&app);
	/* ten lines of 90 characters: one row each at 100 columns, two at 50 */
	app.info.n = 10;
	for (int i = 0; i < 10; i++) {
		memset(app.info.text[i], 'a' + i, 90);
		app.info.text[i][90] = 0;
	}
	app.info_cols = 100;
	app.info.scroll = 4; /* the fifth line is at the top */
	app.cols = 50;
	(void)app.mode->on_event(&app, &resize);
	/* at 50 columns every line takes two rows: line 4 starts at row 8 */
	CHECK(8 == app.info.scroll && 50 == app.info_cols);
	wf_app_destroy(&app);
}

static void fitting_uses_cells_and_keeps_the_extension(void)
{
	WfFrame *f = wf_frame_new(20, 2, &WF_STYLE_BASE);
	const char *name = "日本語の長い名前-か\xcc\x99-\xf0\x9f\x98\x80.txt";
	char row[128] = "";
	size_t used = 0;
	CHECK(NULL != f);
	wf_app_put_fit(f, 0, 0, name, &WF_STYLE_BASE, 18);
	for (int x = 0; x < 18; x++) {
		const WfCell *c = &f->cells[x];
		if (c->width) {
			size_t n = strlen(c->utf8);
			memcpy(row + used, c->utf8, n);
			used += n;
		}
	}
	row[used] = '\0';
	CHECK(strstr(row, "...") && strstr(row, ".txt"));
	CHECK(!strstr(row, "?"));
	CHECK(!strcmp(f->cells[18].utf8, " "));
	CHECK(!wf_ui_alert(NULL, NULL, 0));
	wf_frame_free(f);
}

static void fitting_returns_the_drawn_width(void)
{
	WfFrame *f = wf_frame_new(30, 1, &WF_STYLE_BASE);
	static const char *const names[] = {"short", "abcdefghijklmnop.txt",
	        "日本語の長い名前.txt", "a\xcc\x99-long-\xf0\x9f\x98\x80.txt"};
	CHECK(NULL != f);
	for (unsigned i = 0; i < WF_ARRAY_LEN(names); i++) {
		for (int width = 1; width <= 20; width++) {
			wf_frame_fill(f, 0, 0, 30, 1, ' ', &WF_STYLE_BASE);
			int n = wf_app_put_fit(
			        f, 0, 0, names[i], &WF_STYLE_BASE, width);
			CHECK(n > 0 && n <= width);
			CHECK(1 ==
			        wf_frame_put(
			                f, n, 0, "/", 1, &WF_STYLE_BASE, 1));
			CHECK(!strcmp(f->cells[n].utf8, "/"));
			/* Neither a wide continuation nor an unused padding cell can
			 * displace the directory suffix returned to the list renderer. */
			CHECK(2 != f->cells[n - 1].width);
		}
	}
	wf_frame_free(f);
}

int main(void)
{
	test_tiers();
	test_every_width_tiles();
	test_bad_ratios();
	test_modes();
	help_wraps_on_a_narrow_screen();
	a_long_report_scrolls_on_a_small_screen();
	details_keep_their_line_across_a_resize();
	fitting_uses_cells_and_keeps_the_extension();
	fitting_returns_the_drawn_width();
	return TEST_RESULT();
}

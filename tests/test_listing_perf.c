/*
 * How the list model and the screen behave on a big folder. The figures are
 * printed (best / median / worst of several runs) and each is held to a loose
 * ceiling that catches work that has become proportional to the folder size
 * where it should be proportional to the screen.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/wf_present.h"
#include "ui/wf_app.h"
#include "wf_test.h"

#define ENTRIES 200000u
#define RUNS 3

static double now_ms(void)
{
	LARGE_INTEGER f, c;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

/* Prints best/median/worst and returns the median. */
static double report(const char *what, double *t, int n)
{
	qsort(t, (size_t)n, sizeof *t, cmp_double);
	printf("%-34s best %9.3f  median %9.3f  worst %9.3f ms\n", what, t[0],
	        t[n / 2], t[n - 1]);
	return t[n / 2];
}

static WfDir *make_dir(size_t n)
{
	WfDir *d = wf_dir_new();
	size_t i;
	unsigned seed = 12345u;

	CHECK(NULL != d);
	for (i = 0; i < n; i++) {
		wchar_t name[48];

		seed = seed * 1664525u + 1013904223u;
		swprintf(name, 48, L"%s%05u_item_%zu.%s",
		        (seed >> 24) & 1 ? L"Report" : L"data", seed % 99999u,
		        i, (seed >> 20) & 1 ? L"txt" : L"bin");
		CHECK(wf_dir_add(d, name, wcslen(name),
		              (i % 50 == 0 ? WF_ENT_DIR : 0) |
		                      (i % 97 == 0 ? WF_ENT_HIDDEN : 0),
		              (uint64_t)seed, (WfFileTime)(seed ^ i)) == WF_OK);
	}
	return d;
}

static WfStatus no_listing(void *ctx,
        uint32_t gen,
        const wchar_t *path,
        const wchar_t *parent_path)
{
	(void)ctx;
	(void)gen;
	(void)path;
	(void)parent_path;
	return WF_OK;
}

static void sorting(void)
{
	static const WfSortKey keys[] = {
	        WF_SORT_NAME, WF_SORT_SIZE, WF_SORT_DATE, WF_SORT_EXT};
	static const char *const names[] = {"sort by name (200k)",
	        "sort by size (200k)", "sort by date (200k)",
	        "sort by ext (200k)"};

	for (size_t k = 0; k < 4; k++) {
		double t[RUNS];

		for (int r = 0; r < RUNS; r++) {
			WfDir *d = make_dir(ENTRIES);
			double t0 = now_ms();

			CHECK(WF_OK == wf_dir_sort(d, keys[k], false));
			t[r] = now_ms() - t0;
			wf_dir_free(d);
		}
		CHECK(report(names[k], t, RUNS) < 2000.0);
	}
}

static void view_operations(void)
{
	double t_set[RUNS], t_filter[RUNS], t_hidden[RUNS], t_move[RUNS];

	for (int r = 0; r < RUNS; r++) {
		WfView v;
		WfDir *d = make_dir(ENTRIES);
		double t0;

		CHECK(WF_OK == wf_dir_sort(d, WF_SORT_NAME, false));
		wf_view_init(&v);
		t0 = now_ms();
		CHECK(WF_OK == wf_view_set_dir(&v, d, NULL));
		t_set[r] = now_ms() - t0;

		/* typing a filter, one key at a time */
		t0 = now_ms();
		CHECK(WF_OK == wf_view_set_filter(&v, L"r"));
		CHECK(WF_OK == wf_view_set_filter(&v, L"re"));
		CHECK(WF_OK == wf_view_set_filter(&v, L"rep"));
		CHECK(WF_OK == wf_view_set_filter(&v, L"repo"));
		t_filter[r] = (now_ms() - t0) / 4.0;
		CHECK(WF_OK == wf_view_set_filter(&v, L""));

		t0 = now_ms();
		CHECK(WF_OK == wf_view_set_hidden(&v, true));
		CHECK(WF_OK == wf_view_set_hidden(&v, false));
		t_hidden[r] = (now_ms() - t0) / 2.0;

		t0 = now_ms();
		for (int i = 0; i < 1000; i++) {
			wf_view_move(&v, i & 1 ? -1 : 1, 40);
		}
		t_move[r] = (now_ms() - t0) / 1000.0;
		wf_view_free(&v);
	}
	CHECK(report("view: set directory (200k)", t_set, RUNS) < 1000.0);
	CHECK(report("view: one filter keystroke", t_filter, RUNS) < 500.0);
	CHECK(report("view: toggle hidden", t_hidden, RUNS) < 500.0);
	CHECK(report("view: one cursor move", t_move, RUNS) < 5.0);
}

static void drawing(void)
{
	double t_frame[RUNS], t_move[RUNS];

	for (int r = 0; r < RUNS; r++) {
		static WfApp app;
		WfFsPort port = {NULL, no_listing};
		WfFrame *f;
		WfDir *d = make_dir(ENTRIES);
		double t0;

		CHECK(WF_OK == wf_app_init(&app, 160, 40, port, L"C:\\big"));
		CHECK(WF_OK == wf_dir_sort(d, WF_SORT_NAME, false));
		wf_app_on_listing(&app, app.nav_gen, WF_OK, d, NULL);
		CHECK(app.cur.nvis > 100000);
		f = wf_frame_new(160, 40, &WF_STYLE_BASE);
		CHECK(NULL != f);
		t0 = now_ms();
		for (int i = 0; i < 100; i++) {
			wf_app_draw(&app, f);
		}
		t_frame[r] = (now_ms() - t0) / 100.0;
		t0 = now_ms();
		for (int i = 0; i < 100; i++) {
			WfEvent e = {WF_EV_CHAR, 'j', WF_KEY_NONE, 0};

			app.mode->on_event(&app, &e);
			wf_app_draw(&app, f);
		}
		t_move[r] = (now_ms() - t0) / 100.0;
		wf_frame_free(f);
		wf_app_destroy(&app);
	}
	CHECK(report("draw: one 160x40 frame, 200k rows", t_frame, RUNS) <
	        20.0);
	CHECK(report("key j + draw, 200k rows", t_move, RUNS) < 25.0);
}

/* Changing the theme repaints every cell once: activation, the draw, and the
 * diff against the previous frame, which this time differs everywhere. */
static void theme_switch(void)
{
	double t[RUNS];
	WfApp app;
	WfFsPort port = {NULL, no_listing};
	WfDir *d = make_dir(2000);

	CHECK(WF_OK == wf_app_init(&app, 160, 40, port, L"C:\big"));
	CHECK(WF_OK == wf_dir_sort(d, WF_SORT_NAME, false));
	wf_app_on_listing(&app, app.nav_gen, WF_OK, d, NULL);
	for (int r = 0; r < RUNS; r++) {
		WfFrame *a = wf_frame_new(160, 40, &WF_STYLE_BASE);
		WfFrame *b = wf_frame_new(160, 40, &WF_STYLE_BASE);
		WfBuf out;
		double t0;

		wf_buf_init(&out);
		wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
		wf_app_draw(&app, a);
		t0 = now_ms();
		for (int i = 0; i < 20; i++) {
			wf_theme_activate(&wf_theme_preset(
			        (WfThemeId)(i & 1 ? WF_THEME_SPADE
			                          : WF_THEME_DIAMOND))
			                ->palette);
			wf_app_draw(&app, b);
			wf_buf_clear(&out);
			CHECK(WF_OK == wf_present_diff(a, b, &out));
			CHECK(out.len > 1000); /* everything was repainted */
			{
				WfFrame *tmp = a;

				a = b;
				b = tmp;
			}
		}
		t[r] = (now_ms() - t0) / 20.0;
		wf_buf_free(&out);
		wf_frame_free(a);
		wf_frame_free(b);
	}
	wf_app_destroy(&app);
	CHECK(report("theme switch: draw + full repaint diff", t, RUNS) < 20.0);
	wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
}

int main(void)
{
	sorting();
	view_operations();
	drawing();
	theme_switch();
	return TEST_RESULT();
}

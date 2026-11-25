#include <string.h>

#include "ui/wf_app.h"
#include "wf_fake_fs.h"
#include "wf_test.h"
#include "base/wf_macro.h"

static int yes_calls;

static void on_yes(WfApp *app, void *ctx)
{
	(void)app;
	yes_calls += ctx ? *(int *)ctx : 1;
}

static void on_yes_reopen(WfApp *app, void *ctx)
{
	static const char *const l[] = {"again"};

	(void)ctx;
	yes_calls++;
	wf_app_confirm_begin(app, l, 1, on_yes, NULL, NULL);
}

static const WfEvent EV_Y = {WF_EV_CHAR, 'y', WF_KEY_NONE, 0x59};
static const WfEvent EV_UPY = {WF_EV_CHAR, 'Y', WF_KEY_NONE, 0};
static const WfEvent EV_N = {WF_EV_CHAR, 'n', WF_KEY_NONE, 0};
static const WfEvent EV_ESC = {WF_EV_KEY, 0, WF_KEY_ESC, 0};
static const WfEvent EV_ENTER = {WF_EV_KEY, 0, WF_KEY_ENTER, 0};
static const WfEvent EV_REL_Y = {WF_EV_RELEASE, 'y', WF_KEY_NONE, 0x59};

static void send(WfApp *a, const WfEvent *e)
{
	a->mode->on_event(a, e);
}

/* Opens a modal at t=1000 and presents it, so the guard runs 1000..1300. */
static void open_modal(WfApp *a, FakePort *fp, const WfEvent *trigger)
{
	static const char *const lines[] = {"MOVE", "Move 2 items?"};

	yes_calls = 0;
	CHECK(WF_OK ==
	        wf_app_init(a, 120, 30, fake_port(fp), L"C:\\Users\\dev"));
	a->now_ms = 1000;
	CHECK(WF_OK ==
	        wf_app_confirm_begin(a, lines, 2, on_yes, NULL, trigger));
}

static void test_guard_sequence(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	open_modal(&a, &fp, NULL);
	CHECK(&WF_MODE_CONFIRM == a.mode && a.flush_input);
	CHECK(WF_CONF_PENDING == a.confirm.state);
	CHECK(WF_WAIT_FOREVER == wf_app_timeout(&a));

	send(&a, &EV_Y); /* before the modal was even presented */
	CHECK(0 == yes_calls && &WF_MODE_CONFIRM == a.mode);

	wf_app_on_presented(&a);
	CHECK(WF_CONF_GUARD == a.confirm.state && 300 == wf_app_timeout(&a));

	a.now_ms = 1100;
	send(&a, &EV_Y); /* inside the guard */
	CHECK(0 == yes_calls && &WF_MODE_CONFIRM == a.mode);
	CHECK(200 == wf_app_timeout(&a));
	CHECK(WF_ACT_NONE == wf_app_tick(&a));

	a.now_ms = 1300;
	CHECK(0 == wf_app_timeout(&a));
	CHECK(WF_ACT_REDRAW == wf_app_tick(&a) &&
	        WF_CONF_ARMED == a.confirm.state);
	CHECK(WF_ACT_NONE == wf_app_tick(&a));

	send(&a, &EV_Y);
	CHECK(1 == yes_calls && &WF_MODE_NORMAL == a.mode);
	wf_app_destroy(&a);
}

static void test_event_arms_lazily(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	open_modal(&a, &fp, NULL);
	wf_app_on_presented(&a);
	a.now_ms = 1301; /* no tick happened; the key itself must notice */
	send(&a, &EV_UPY);
	CHECK(1 == yes_calls);
	wf_app_destroy(&a);
}

static void test_cancel_paths(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	/* Esc works at every stage */
	open_modal(&a, &fp, NULL);
	send(&a, &EV_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode && 0 == yes_calls);
	wf_app_destroy(&a);

	open_modal(&a, &fp, NULL);
	wf_app_on_presented(&a);
	a.now_ms = 1100;
	send(&a, &EV_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode && 0 == yes_calls);
	wf_app_destroy(&a);

	/* Enter never confirms; N cancels */
	open_modal(&a, &fp, NULL);
	wf_app_on_presented(&a);
	a.now_ms = 2000;
	send(&a, &EV_ENTER);
	CHECK(&WF_MODE_NORMAL == a.mode && 0 == yes_calls);
	wf_app_destroy(&a);

	open_modal(&a, &fp, NULL);
	wf_app_on_presented(&a);
	a.now_ms = 2000;
	send(&a, &EV_N);
	CHECK(&WF_MODE_NORMAL == a.mode && 0 == yes_calls);
	wf_app_destroy(&a);
}

static void test_held_trigger(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	open_modal(&a, &fp, &EV_Y); /* opened by 'y', still physically held */
	wf_app_on_presented(&a);
	a.now_ms = 2000;
	send(&a, &EV_Y); /* auto-repeat after the guard */
	CHECK(0 == yes_calls && &WF_MODE_CONFIRM == a.mode);
	send(&a, &EV_UPY); /* different char but unknown vk: not the held key */
	CHECK(1 == yes_calls);
	wf_app_destroy(&a);

	open_modal(&a, &fp, &EV_Y);
	wf_app_on_presented(&a);
	a.now_ms = 2000;
	send(&a, &EV_REL_Y); /* released: now a fresh press counts */
	send(&a, &EV_Y);
	CHECK(1 == yes_calls);
	wf_app_destroy(&a);
}

static void test_ime_hint(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfEvent jp = {WF_EV_CHAR, 0x306F, WF_KEY_NONE, 0}; /* は */

	open_modal(&a, &fp, NULL);
	wf_app_on_presented(&a);
	a.now_ms = 2000;
	send(&a, &jp);
	CHECK(a.confirm.ime_hint && 0 == yes_calls &&
	        &WF_MODE_CONFIRM == a.mode);
	send(&a, &EV_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode);
	wf_app_destroy(&a);
}

static void test_reopen_from_callback(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	static const char *const l[] = {"first"};

	open_modal(&a, &fp, NULL);
	CHECK(WF_OK ==
	        wf_app_confirm_begin(&a, l, 1, on_yes_reopen, NULL, NULL));
	wf_app_on_presented(&a);
	a.now_ms = 2000;
	send(&a, &EV_UPY);
	/* the callback opened a second modal; it must be live and locked */
	CHECK(1 == yes_calls && &WF_MODE_CONFIRM == a.mode);
	CHECK(WF_CONF_PENDING == a.confirm.state);
	wf_app_destroy(&a);
}

static void test_text_handling(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	char longline[200];
	char edge[80];
	const char *l1[1], *l2[1], *bad[9] = {0};

	open_modal(&a, &fp, NULL);

	memset(longline, 'x', sizeof longline - 1);
	longline[sizeof longline - 1] = '\0';
	longline[0] = '\x1b'; /* a terminal escape inside "file name" text */
	l1[0] = longline;
	CHECK(WF_OK == wf_app_confirm_begin(&a, l1, 1, on_yes, NULL, NULL));
	CHECK(strlen(a.confirm.lines[0]) == WF_CONFIRM_LINE_BYTES - 1);
	CHECK(0 == strncmp(a.confirm.lines[0], "^[x", 3));

	/* the cut must not land inside a multi-byte character */
	memset(edge, 'a', 62);
	memcpy(edge + 62, "\xE3\x81\x82", 3); /* あ occupies bytes 62..64 */
	edge[65] = '\0';
	l2[0] = edge;
	CHECK(WF_OK == wf_app_confirm_begin(&a, l2, 1, on_yes, NULL, NULL));
	CHECK(62 == strlen(a.confirm.lines[0]));

	CHECK(WF_E_INVAL ==
	        wf_app_confirm_begin(&a, bad, 9, on_yes, NULL, NULL));
	CHECK(WF_E_INVAL == wf_app_confirm_begin(&a, l1, 1, NULL, NULL, NULL));
	wf_app_destroy(&a);
}

static int row_has(const WfFrame *f, int y, const char *needle)
{
	char row[512] = "";
	int x;

	for (x = 0; x < f->cols && strlen(row) + 24 < sizeof row; x++) {
		strcat_s(row, sizeof row, f->cells[y * f->cols + x].utf8);
	}
	return NULL != strstr(row, needle);
}

static int frame_has(const WfFrame *f, const char *needle)
{
	int y;

	for (y = 0; y < f->rows; y++) {
		if (row_has(f, y, needle)) {
			return 1;
		}
	}
	return 0;
}

static void test_draw(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfFrame *f;

	open_modal(&a, &fp, NULL);

	f = wf_frame_new(120, 30, &WF_STYLE_BASE);
	wf_app_draw(&a, f);
	CHECK(frame_has(f, "MOVE") && frame_has(f, "input locked"));
	CHECK(!frame_has(f, "[Y] Yes"));
	a.confirm.state = WF_CONF_ARMED;
	wf_app_draw(&a, f);
	CHECK(frame_has(f, "[Y] Yes") && frame_has(f, "[N] No"));
	wf_frame_free(f);

	/* a terminal too small for the box still shows an answerable prompt */
	f = wf_frame_new(20, 5, &WF_STYLE_BASE);
	wf_app_draw(&a, f);
	CHECK(row_has(f, 0, "CONFIRM: Y/N"));
	wf_frame_free(f);
	wf_app_destroy(&a);
}

/* Spec 5.1: at every size that can be answered, the verb and the destination
 * stay on screen; below 20x8 only the full-screen notice remains. */
static void test_one_row_modal_keeps_prompt(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfFrame *f;

	open_modal(&a, &fp, NULL);
	f = wf_frame_new(20, 1, &WF_STYLE_BASE);
	CHECK(NULL != f);
	if (f) {
		wf_app_draw(&a, f);
		CHECK(row_has(f, 0, "CONFIRM: Y/N"));
		CHECK(!row_has(f, 0, "READY"));
		CHECK(!row_has(f, 0, "LOADING"));
		wf_frame_free(f);
	}
	wf_app_destroy(&a);
}

static void test_modal_tiers(void)
{
	static const int sizes[][2] = {{120, 30}, {80, 24}, {60, 20}, {44, 20},
	        {40, 15}, {40, 12}, {40, 8}, {30, 10}, {20, 8}};
	static const int tiny[][2] = {{20, 7}, {20, 5}, {19, 24}, {10, 3}};
	static const char *const lines[] = {"MOVE", "Move 2 selected items?",
	        "Files 2  Directories 0  Links 0", "Destination:",
	        "C:\\Archive\\2026\\very-long-folder-name-here\\final-part"};
	WfApp a;
	FakePort fp = {.app = &a};
	size_t i;

	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
	CHECK(WF_OK == wf_app_confirm_begin(&a, lines, 5, on_yes, NULL, NULL));
	a.confirm.state = WF_CONF_ARMED;

	for (i = 0; i < WF_ARRAY_LEN(sizes); i++) {
		WfFrame *f =
		        wf_frame_new(sizes[i][0], sizes[i][1], &WF_STYLE_BASE);
		wf_app_draw(&a, f);
		if (!(frame_has(f, "MOVE") && frame_has(f, "-part") &&
		            frame_has(f, "[Y] Yes"))) {
			printf("  FAILED at %dx%d\n", sizes[i][0], sizes[i][1]);
		}
		CHECK(frame_has(f, "MOVE"));
		CHECK(frame_has(f, "-part")); /* the end of the path is the part
		                                 that matters */
		CHECK(frame_has(f, "[Y] Yes"));
		CHECK(!frame_has(f, "CONFIRM: Y/N"));
		wf_frame_free(f);
	}
	/* The box itself (rules of '=') is used wherever it fits; 40 columns is
	 * the floor of spec 5.1, and 40x12 is too short for it. */
	{
		static const struct
		{
			int cols, rows, box;
		} want[] = {{120, 30, 1}, {80, 24, 1}, {60, 20, 1}, {44, 20, 1},
		        {40, 15, 1}, {40, 12, 0}};

		for (i = 0; i < WF_ARRAY_LEN(want); i++) {
			WfFrame *f = wf_frame_new(
			        want[i].cols, want[i].rows, &WF_STYLE_BASE);

			wf_app_draw(&a, f);
			if (frame_has(f, "========") != want[i].box) {
				int yy, xx;

				printf("  box expectation failed at %dx%d; "
				       "screen:\n",
				        want[i].cols, want[i].rows);
				for (yy = 0; yy < f->rows; yy++) {
					for (xx = 0; xx < f->cols; xx++) {
						putchar(f->cells[yy * f->cols +
						                 xx]
						                        .utf8[0]
						                ? f->cells[yy * f->cols +
						                           xx]
						                          .utf8[0]
						                : ' ');
					}
					putchar('\n');
				}
			}
			CHECK(frame_has(f, "========") == want[i].box);
			wf_frame_free(f);
		}
	}
	for (i = 0; i < WF_ARRAY_LEN(tiny); i++) {
		WfFrame *f =
		        wf_frame_new(tiny[i][0], tiny[i][1], &WF_STYLE_BASE);

		wf_app_draw(&a, f);
		/* even a 10-column screen is told what this is */
		CHECK(row_has(f, 0, "CONFIRM:") || row_has(f, 0, "CONFIRM"));
		wf_frame_free(f);
	}
	wf_app_destroy(&a);
}

/* More lines than rows: the compact layout keeps the verb first and the
 * closing lines (the destination) last, and drops the middle. */
static void test_compact_drops_the_middle(void)
{
	static const char *const lines[] = {"MOVE", "m1", "m2", "m3", "m4",
	        "m5", "Destination:", "TARGET-PATH"};
	WfApp a;
	FakePort fp = {.app = &a};
	WfFrame *f;

	CHECK(WF_OK == wf_app_init(&a, 20, 8, fake_port(&fp), L"C:\\Users"));
	CHECK(WF_OK == wf_app_confirm_begin(&a, lines, 8, on_yes, NULL, NULL));
	a.confirm.state = WF_CONF_ARMED;

	f = wf_frame_new(20, 8, &WF_STYLE_BASE);
	wf_app_draw(&a, f);
	CHECK(frame_has(f, "MOVE") && frame_has(f, "TARGET-PATH") &&
	        frame_has(f, "Destination:"));
	CHECK(frame_has(f, "[Y] Yes"));
	CHECK(!frame_has(f, "m1")); /* the middle makes room */
	wf_frame_free(f);

	/* the IME hint line takes one more row; the destination still survives
	 */
	a.confirm.ime_hint = 1;
	f = wf_frame_new(30, 8, &WF_STYLE_BASE);
	wf_app_draw(&a, f);
	CHECK(frame_has(f, "MOVE") && frame_has(f, "TARGET-PATH") &&
	        frame_has(f, "[Y] Yes"));
	wf_frame_free(f);
	wf_app_destroy(&a);
}

/* A question that cannot be shown cannot be answered "yes": below 20x8 only
 * Esc and a resize do anything. */
static void test_nothing_is_approved_unseen(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	open_modal(&a, &fp, NULL);
	a.confirm.state = WF_CONF_ARMED;
	a.cols = 19; /* too narrow to show it */
	send(&a, &EV_Y);
	send(&a, &EV_UPY);
	CHECK(0 == yes_calls && &WF_MODE_CONFIRM == a.mode);
	a.cols = 120;
	a.rows = 7; /* too short */
	send(&a, &EV_Y);
	CHECK(0 == yes_calls && &WF_MODE_CONFIRM == a.mode);
	/* Esc still gets out of it */
	send(&a, &EV_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode && 0 == yes_calls);
	wf_app_destroy(&a);

	/* the same question at a size where it is shown is answered */
	open_modal(&a, &fp, NULL);
	a.confirm.state = WF_CONF_ARMED;
	a.cols = 20;
	a.rows = 8;
	send(&a, &EV_Y);
	CHECK(1 == yes_calls && &WF_MODE_NORMAL == a.mode);
	wf_app_destroy(&a);
}

static const WfEvent EV_LEFT = {WF_EV_KEY, 0, WF_KEY_LEFT, 0};
static const WfEvent EV_RIGHT = {WF_EV_KEY, 0, WF_KEY_RIGHT, 0};
static const WfEvent EV_UP = {WF_EV_KEY, 0, WF_KEY_UP, 0};
static const WfEvent EV_DOWN = {WF_EV_KEY, 0, WF_KEY_DOWN, 0};

static void open_armed(WfApp *a, FakePort *fp)
{
	open_modal(a, fp, NULL);
	wf_app_on_presented(a);
	a->now_ms = 2000;
	wf_app_tick(a);
}

/* Arrows choose yes or no; Enter takes the one chosen. Enter alone is still a
 * "no", and nothing is chosen before the question can be answered. */
static void test_arrows_choose(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfFrame *f;

	open_armed(&a, &fp);
	send(&a, &EV_LEFT);
	CHECK(&WF_MODE_CONFIRM == a.mode && 0 == yes_calls);
	f = wf_frame_new(120, 30, &WF_STYLE_BASE);
	wf_app_draw(&a, f);
	CHECK(frame_has(f, "> [Y] Yes") && !frame_has(f, "> [N] No"));
	wf_frame_free(f);
	send(&a, &EV_ENTER);
	CHECK(1 == yes_calls && &WF_MODE_NORMAL == a.mode);
	wf_app_destroy(&a);

	open_armed(&a, &fp); /* up is yes, down is no */
	send(&a, &EV_UP);
	send(&a, &EV_DOWN);
	f = wf_frame_new(120, 30, &WF_STYLE_BASE);
	wf_app_draw(&a, f);
	CHECK(frame_has(f, "> [N] No"));
	wf_frame_free(f);
	send(&a, &EV_ENTER);
	CHECK(0 == yes_calls && &WF_MODE_NORMAL == a.mode);
	wf_app_destroy(&a);

	open_armed(&a, &fp);
	send(&a, &EV_LEFT);
	send(&a, &EV_RIGHT); /* changed the mind */
	send(&a, &EV_ENTER);
	CHECK(0 == yes_calls && &WF_MODE_NORMAL == a.mode);
	wf_app_destroy(&a);

	open_armed(&a, &fp);
	send(&a, &EV_ENTER); /* no arrow: still no */
	CHECK(0 == yes_calls && &WF_MODE_NORMAL == a.mode);
	wf_app_destroy(&a);

	open_armed(&a, &fp);
	send(&a, &EV_LEFT);
	send(&a, &EV_ESC); /* Esc is always no */
	CHECK(0 == yes_calls && &WF_MODE_NORMAL == a.mode);
	wf_app_destroy(&a);

	open_modal(&a, &fp, NULL); /* inside the guard: an arrow is dropped */
	wf_app_on_presented(&a);
	a.now_ms = 1100;
	send(&a, &EV_LEFT);
	a.now_ms = 2000;
	send(&a, &EV_ENTER);
	CHECK(0 == yes_calls && &WF_MODE_NORMAL == a.mode);
	wf_app_destroy(&a);

	open_armed(&a, &fp); /* a new question starts at no again */
	send(&a, &EV_LEFT);
	wf_app_confirm_begin(
	        &a, (const char *const[]){"next"}, 1, on_yes, NULL, NULL);
	wf_app_on_presented(&a);
	a.now_ms = 4000;
	wf_app_tick(&a);
	send(&a, &EV_ENTER);
	CHECK(0 == yes_calls && &WF_MODE_NORMAL == a.mode);
	wf_app_destroy(&a);
}

/* A question with its own button line or another answer is not a plain Y/N:
 * the arrows do nothing there, and Enter is a "no" as before. */
static void test_arrows_not_on_other_questions(void)
{
	static const char *const lines[] = {"title", "text"};
	WfApp a;
	FakePort fp = {.app = &a};

	yes_calls = 0;
	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	a.now_ms = 1000;
	CHECK(WF_OK ==
	        wf_app_confirm_begin_ex(&a, lines, 2, 'y', "[Y] Go  [N] Stay",
	                on_yes, NULL, NULL));
	wf_app_on_presented(&a);
	a.now_ms = 2000;
	wf_app_tick(&a);
	send(&a, &EV_LEFT);
	send(&a, &EV_ENTER);
	CHECK(0 == yes_calls && &WF_MODE_NORMAL == a.mode);
	wf_app_destroy(&a);
}

int main(void)
{
	test_nothing_is_approved_unseen();
	test_guard_sequence();
	test_event_arms_lazily();
	test_cancel_paths();
	test_held_trigger();
	test_ime_hint();
	test_reopen_from_callback();
	test_text_handling();
	test_draw();
	test_one_row_modal_keeps_prompt();
	test_modal_tiers();
	test_arrows_choose();
	test_arrows_not_on_other_questions();
	test_compact_drops_the_middle();
	return TEST_RESULT();
}

/*
 * The pointer in the file list, and pasted text in the file manager: a click
 * puts the cursor on the entry under it, a double click opens it, a click in
 * the parent column goes up, the wheel moves by three; pasted characters are
 * never run as commands.
 */
#include <string.h>

#include "ui/wf_app_priv.h"
#include "ui/wf_layout.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

#define COLS 120
#define ROWS 30

static WfEvent mouse(int x, int y, unsigned flags)
{
	WfEvent e = {WF_EV_MOUSE, (uint32_t)x | ((uint32_t)y << 16),
	        WF_KEY_NONE, (uint16_t)flags};

	return e;
}

static void send(WfApp *a, WfEvent e)
{
	a->mode->on_event(a, &e);
}

static void start(WfApp *a, FakePort *fp)
{
	CHECK(WF_OK ==
	        wf_app_init(a, COLS, ROWS, fake_port(fp), L"C:\\Users\\dev"));
}

static void a_click_puts_the_cursor_on_the_entry(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfLayout lay;
	int x;

	start(&a, &fp);
	wf_layout_compute(COLS, ROWS, &a.ratios, &lay);
	x = lay.pane[WF_PANE_CURRENT].x + 4;
	CHECK(3 == a.cur.nvis && 0 == a.cur.cursor);

	send(&a, mouse(x, lay.body_y + 2, WF_MOUSE_LEFT));
	CHECK(2 == a.cur.cursor);
	send(&a, mouse(x, lay.body_y + 1, WF_MOUSE_LEFT));
	CHECK(1 == a.cur.cursor);
	/* below the last entry, above the list, and on a release: nothing */
	send(&a, mouse(x, lay.body_y + 9, WF_MOUSE_LEFT));
	CHECK(1 == a.cur.cursor);
	send(&a, mouse(x, 0, WF_MOUSE_LEFT));
	CHECK(1 == a.cur.cursor);
	send(&a, mouse(x, lay.body_y, 0));
	CHECK(1 == a.cur.cursor);
	/* the right button does nothing here */
	send(&a, mouse(x, lay.body_y, WF_MOUSE_RIGHT));
	CHECK(1 == a.cur.cursor);
	wf_app_destroy(&a);
}

static void the_wheel_moves_by_three_and_stops_at_the_ends(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfLayout lay;
	int x;

	start(&a, &fp);
	wf_layout_compute(COLS, ROWS, &a.ratios, &lay);
	x = lay.pane[WF_PANE_CURRENT].x + 2;
	send(&a, mouse(x, lay.body_y, WF_MOUSE_WHEEL_DOWN));
	CHECK(2 == a.cur.cursor); /* three steps, three entries: the last */
	send(&a, mouse(x, lay.body_y, WF_MOUSE_WHEEL_UP));
	CHECK(0 == a.cur.cursor);
	/* over the title row it is not the list's */
	send(&a, mouse(x, 0, WF_MOUSE_WHEEL_DOWN));
	CHECK(0 == a.cur.cursor);
	wf_app_destroy(&a);
}

static void a_click_on_the_current_folder_in_the_parent_column_stays(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfLayout lay;

	start(&a, &fp);
	wf_layout_compute(COLS, ROWS, &a.ratios, &lay);
	CHECK(lay.pane[WF_PANE_PARENT].w > 0);
	send(&a,
	        mouse(lay.pane[WF_PANE_PARENT].x + 1, lay.body_y,
	                WF_MOUSE_LEFT));
	/* its row is the folder we are in already */
	CHECK(!wcscmp(a.cwd, L"C:\\Users\\dev"));
	wf_app_destroy(&a);
}

static void a_double_click_opens_the_entry(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfLayout lay;
	int x;

	start(&a, &fp);
	wf_layout_compute(COLS, ROWS, &a.ratios, &lay);
	x = lay.pane[WF_PANE_CURRENT].x + 4;
	/* the first entry is the folder Docs, which the fake tree cannot list;
	 * the double click is Enter on it, which starts the listing */
	send(&a, mouse(x, lay.body_y, WF_MOUSE_LEFT | WF_MOUSE_DOUBLE));
	CHECK(a.want_path || !wcscmp(a.cwd, L"C:\\Users\\dev\\Docs") ||
	        a.msg[0]);
	wf_app_destroy(&a);
}

static void pasted_characters_are_not_commands(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfEvent j = {WF_EV_CHAR, 'j', WF_KEY_NONE, 0};
	WfEvent pasted_j = {WF_EV_CHAR, 'j', WF_KEY_NONE, WF_VK_PASTED};
	WfEvent pasted_enter = {WF_EV_KEY, 0, WF_KEY_ENTER, WF_VK_PASTED};

	start(&a, &fp);
	send(&a, j);
	CHECK(1 == a.cur.cursor); /* typed: a command */
	send(&a, pasted_j);
	send(&a, pasted_j);
	send(&a, pasted_enter);
	CHECK(1 == a.cur.cursor && !a.want_path); /* pasted: nothing ran */
	CHECK(strstr(a.msg, "pasted text"));

	/* into a prompt the same characters are text */
	{
		WfEvent f = {WF_EV_CHAR, '/', WF_KEY_NONE, 0};

		send(&a, f);
		CHECK(&WF_MODE_NORMAL != a.mode);
		send(&a, pasted_j);
		CHECK(NULL != wcschr(a.cur.filter, L'j'));
	}
	wf_app_destroy(&a);
}

static void the_wheel_over_the_preview_does_not_move_the_list(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfLayout lay;

	start(&a, &fp);
	wf_layout_compute(COLS, ROWS, &a.ratios, &lay);
	CHECK(lay.pane[WF_PANE_PREVIEW].w > 0);
	send(&a,
	        mouse(lay.pane[WF_PANE_PREVIEW].x + 2, lay.body_y + 1,
	                WF_MOUSE_WHEEL_DOWN));
	CHECK(0 == a.cur.cursor); /* the file being looked at is still it */
	send(&a,
	        mouse(lay.pane[WF_PANE_PREVIEW].x + 2, lay.body_y + 1,
	                WF_MOUSE_WHEEL_UP));
	CHECK(0 == a.cur.cursor);
	/* and the scroll itself stays inside the preview's limits */
	wf_app_preview_scroll(&a, 5);
	wf_app_preview_scroll(&a, -50);
	CHECK(0 == a.preview_top);
	wf_app_destroy(&a);
}

static void a_click_in_the_parent_column_goes_to_the_clicked_entry(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfLayout lay;
	int x;

	CHECK(WF_OK ==
	        wf_app_init(&a, COLS, ROWS, fake_port(&fp), L"C:\\Users"));
	wf_layout_compute(COLS, ROWS, &a.ratios, &lay);
	x = lay.pane[WF_PANE_PARENT].x + 1;
	CHECK(a.parent.dir && 3 == a.parent.nvis); /* Users, secret, a.txt */
	/* a file: its own folder, with the file chosen */
	send(&a, mouse(x, lay.body_y + 2, WF_MOUSE_LEFT));
	CHECK(!wcscmp(a.cwd, L"C:\\"));
	CHECK(a.cur.dir && a.cur.cursor < a.cur.nvis &&
	        !wcscmp(a.cur.dir->e[a.cur.vis[a.cur.cursor]].name, L"a.txt"));
	/* below the last entry: nothing */
	send(&a, mouse(x, lay.body_y + 8, WF_MOUSE_LEFT));
	CHECK(!wcscmp(a.cwd, L"C:\\"));
	wf_app_destroy(&a);
}

static void yes(WfApp *a, void *ctx)
{
	(void)a;
	(void)ctx;
}

static bool filt(WfApp *a, WfEvent e)
{
	return wf_app_paste_filter(a, &e);
}

static WfEvent pasted_char(uint32_t c)
{
	return (WfEvent){WF_EV_CHAR, c, WF_KEY_NONE, WF_VK_PASTED};
}

static void pasted_keys_reach_only_what_takes_text(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfEvent enter = {WF_EV_KEY, 0, WF_KEY_ENTER, WF_VK_PASTED};
	WfEvent release = {WF_EV_RELEASE, 'x', WF_KEY_NONE, 0};
	WfEvent typed = {WF_EV_CHAR, 'z', WF_KEY_NONE, 0};
	WfEvent g = {WF_EV_CHAR, 'g', WF_KEY_NONE, 0};
	const char *lines[] = {"sure?"};

	start(&a, &fp);
	/* the list handles it itself */
	CHECK(filt(&a, pasted_char('x')));

	/* a path prompt takes the first pasted line as text and not the rest */
	send(&a, g);
	CHECK(&WF_MODE_GO == a.mode);
	CHECK(filt(&a, pasted_char('a')));
	CHECK(filt(&a, release)); /* key-ups in between */
	CHECK(filt(&a, pasted_char('b')));
	CHECK(!filt(&a, enter)); /* never confirms */
	CHECK(filt(&a, release));
	CHECK(!filt(&a, pasted_char('c'))); /* line 2 */
	CHECK(!filt(&a, enter));
	CHECK(filt(&a, typed)); /* typing ends the cut */
	CHECK(filt(&a, pasted_char('d')));

	/* a question is never answered by pasted keys */
	a.mode = &WF_MODE_NORMAL;
	CHECK(WF_OK == wf_app_confirm_begin(&a, lines, 1, yes, NULL, NULL));
	CHECK(&WF_MODE_CONFIRM == a.mode);
	CHECK(!filt(&a, pasted_char('y')));
	CHECK(!filt(&a, enter));
	CHECK(filt(&a, typed));
	wf_app_destroy(&a);
}

/* The wheel scrolls the windows that scroll, and nothing else in a window
 * answers to the pointer. */
static void the_wheel_scrolls_a_window_and_never_closes_it(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfAction act = WF_ACT_NONE;
	WfEvent down = mouse(5, 5, WF_MOUSE_WHEEL_DOWN);
	WfEvent up = mouse(5, 5, WF_MOUSE_WHEEL_UP);
	WfEvent click = mouse(5, 5, WF_MOUSE_LEFT);

	start(&a, &fp);
	/* the list is the pointer's own business, not this function's */
	CHECK(!wf_app_window_wheel(&a, &down, &act));

	wf_app_help_begin(&a);
	CHECK(wf_app_window_wheel(&a, &down, &act) && WF_ACT_REDRAW == act &&
	        &WF_MODE_HELP == a.mode && 3 == a.help.sel);
	CHECK(wf_app_window_wheel(&a, &up, &act) && 0 == a.help.sel &&
	        &WF_MODE_HELP == a.mode);
	/* a click in a window is not a wheel: it is left alone */
	CHECK(!wf_app_window_wheel(&a, &click, &act) &&
	        &WF_MODE_HELP == a.mode);

	wf_app_joblog_begin(&a);
	CHECK(wf_app_window_wheel(&a, &down, &act) &&
	        &WF_MODE_JOBLOG == a.mode);
	wf_app_info_begin(&a);
	CHECK(wf_app_window_wheel(&a, &down, &act) && &WF_MODE_INFO == a.mode);
	a.mode = &WF_MODE_NORMAL;
	wf_app_settings_begin(&a);
	/* no settings port in this rig: the screen may refuse to open */
	if (&WF_MODE_SETTINGS == a.mode) {
		CHECK(wf_app_window_wheel(&a, &down, &act));
	}
	a.mode = &WF_MODE_NORMAL;
	wf_app_destroy(&a);
}

int main(void)
{
	a_click_puts_the_cursor_on_the_entry();
	the_wheel_moves_by_three_and_stops_at_the_ends();
	a_click_on_the_current_folder_in_the_parent_column_stays();
	a_double_click_opens_the_entry();
	the_wheel_over_the_preview_does_not_move_the_list();
	a_click_in_the_parent_column_goes_to_the_clicked_entry();
	pasted_keys_reach_only_what_takes_text();
	pasted_characters_are_not_commands();
	the_wheel_scrolls_a_window_and_never_closes_it();
	return TEST_RESULT();
}

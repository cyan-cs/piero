/* Test-only inclusion makes the actual static translator observable without
 * exposing Win32 input records through the product API. No console is opened.
 */
#include <windows.h>
static BOOL WINAPI test_set_console_mode(HANDLE handle, DWORD mode)
{
	(void)handle;
	(void)mode;
	return TRUE;
}
#define SetConsoleMode test_set_console_mode
#include "../src/term/wf_term.c"
#undef SetConsoleMode
#include "wf_test.h"

static void check_key(WORD vk, DWORD controls, WfKey expected)
{
	WfTerm term = {0};
	INPUT_RECORD record = {0};
	WfEvent event = {WF_EV_CHAR, 0, WF_KEY_NONE, 0};
	record.EventType = KEY_EVENT;
	record.Event.KeyEvent.bKeyDown = TRUE;
	record.Event.KeyEvent.wVirtualKeyCode = vk;
	record.Event.KeyEvent.dwControlKeyState = controls;
	CHECK(translate(&term, &record, &event));
	CHECK(WF_EV_KEY == event.type && event.key == expected &&
	        event.vk == vk);
}

static void check_unmapped(WORD vk, DWORD controls)
{
	WfTerm term = {0};
	INPUT_RECORD record = {0};
	WfEvent event = {WF_EV_CHAR, 0, WF_KEY_NONE, 0};
	record.EventType = KEY_EVENT;
	record.Event.KeyEvent.bKeyDown = TRUE;
	record.Event.KeyEvent.wVirtualKeyCode = vk;
	record.Event.KeyEvent.dwControlKeyState = controls;
	CHECK(!translate(&term, &record, &event));
}

static void terminal_replies_are_not_commands(void)
{
	WfTerm term = {0};
	INPUT_RECORD r = {0};
	WfEvent e = {0};
	const char *reply = "\x1b[?61;4;28c\x1b[6;20;10t";
	wf_term_caps_begin(&term.caps, GetTickCount64());
	r.EventType = KEY_EVENT;
	r.Event.KeyEvent.bKeyDown = TRUE;
	for (size_t i = 0; reply[i]; i++) {
		r.Event.KeyEvent.uChar.UnicodeChar = (wchar_t)reply[i];
		CHECK(!translate(&term, &r, &e));
	}
	CHECK(wf_term_caps_ready(&term.caps));
	r.Event.KeyEvent.wVirtualKeyCode = 'Q';
	r.Event.KeyEvent.uChar.UnicodeChar = 'q';
	CHECK(translate(&term, &r, &e) && e.type == WF_EV_CHAR && e.ch == 'q');
}

/*
 * What the application can know about Japanese input: the terminal host owns
 * the IME composition and delivers only committed text, as key records that
 * carry UnicodeChar. These tests feed such records. They do not and cannot
 * exercise a real IME (the manual steps are in the manual-verification docs).
 */
static bool feed(WfTerm *t, WORD vk, WORD scan, wchar_t ch, WfEvent *e)
{
	INPUT_RECORD r = {0};

	r.EventType = KEY_EVENT;
	r.Event.KeyEvent.bKeyDown = TRUE;
	r.Event.KeyEvent.wVirtualKeyCode = vk;
	r.Event.KeyEvent.wVirtualScanCode = scan;
	r.Event.KeyEvent.uChar.UnicodeChar = ch;
	e->type = WF_EV_CHAR;
	e->key = WF_KEY_NONE;
	e->ch = 0;
	return translate(t, &r, e);
}

static void committed_text_arrives_as_characters(void)
{
	static const wchar_t text[] = {
	        0x3053, 0x3093, 0x306B, 0x3061, 0x306F, 0xFF21, 0x65E5};
	/* ConPTY delivers committed text without a virtual key or scan code;
	 * a classic console may report VK_PROCESSKEY. Both give plain characters. */
	static const WORD vks[] = {0, VK_PROCESSKEY, VK_PACKET};
	size_t v, i;

	for (v = 0; v < WF_ARRAY_LEN(vks); v++) {
		WfTerm t = {0};
		WfEvent e;

		for (i = 0; i < WF_ARRAY_LEN(text); i++) {
			CHECK(feed(
			        &t, vks[v], 0 == vks[v] ? 0 : 1, text[i], &e));
			CHECK(WF_EV_CHAR == e.type && text[i] == e.ch);
		}
	}
}

/*
 * A letter the IME was handling reaches the program as a character record
 * whose virtual key is VK_PROCESSKEY. The translation keeps that virtual key
 * on the event, so a question that must not be answered by such a letter (see
 * input/wf_choice.c) can tell it from the real key.
 */
static void the_virtual_key_of_a_processed_letter_is_kept(void)
{
	WfTerm t = {0};
	WfEvent e;

	CHECK(feed(&t, VK_PROCESSKEY, 0x20, L'd', &e));
	CHECK(WF_EV_CHAR == e.type && L'd' == e.ch && VK_PROCESSKEY == e.vk);
	CHECK(feed(&t, 'D', 0x20, L'd', &e));
	CHECK(WF_EV_CHAR == e.type && L'd' == e.ch && 'D' == e.vk);
	/* text from the terminal has no virtual key at all */
	CHECK(feed(&t, 0, 0, L'd', &e));
	CHECK(WF_EV_CHAR == e.type && 0 == e.vk);
}

static void surrogate_pairs_become_one_character(void)
{
	WfTerm t = {0};
	WfEvent e;

	/* U+20BB7 (a CJK ideograph outside the BMP) = D842 DFB7 */
	CHECK(!feed(&t, 0, 0, 0xD842, &e));
	CHECK(feed(&t, 0, 0, 0xDFB7, &e));
	CHECK(WF_EV_CHAR == e.type && 0x20BB7 == e.ch);
	/* a lone low surrogate is replaced, never passed through */
	CHECK(feed(&t, 0, 0, 0xDFB7, &e));
	CHECK(WF_CP_REPLACEMENT == e.ch);
}

/* How ConPTY really delivers a character outside the BMP (seen in the raw
 * console records): per UTF-16 unit an Alt key-down without a character,
 * then an Alt key-up that carries the unit. */
static bool alt_unit(WfTerm *t, wchar_t unit, bool down, WfEvent *e)
{
	INPUT_RECORD r = {0};

	r.EventType = KEY_EVENT;
	r.Event.KeyEvent.bKeyDown = down;
	r.Event.KeyEvent.wVirtualKeyCode = VK_MENU;
	r.Event.KeyEvent.wVirtualScanCode = 0x38;
	r.Event.KeyEvent.uChar.UnicodeChar = down ? 0 : unit;
	r.Event.KeyEvent.dwControlKeyState = down ? LEFT_ALT_PRESSED : 0;
	e->type = WF_EV_CHAR;
	e->key = WF_KEY_NONE;
	e->ch = 0;
	return translate(t, &r, e);
}

static void conpty_alt_delivered_pair_is_one_character(void)
{
	WfTerm t = {0};
	WfEvent e;
	int mode;

	for (mode = 0; mode < 2; mode++) {
		t.editing = 1 == mode; /* a text field is open, or not */
		CHECK(!alt_unit(&t, 0, true, &e));
		CHECK(!alt_unit(&t, 0xD842, false, &e));
		CHECK(!alt_unit(
		        &t, 0, true, &e)); /* the bare Alt must not drop it */
		CHECK(alt_unit(&t, 0xDFB7, false, &e));
		CHECK(WF_EV_CHAR == e.type && 0x20BB7 == e.ch);
	}
}

static void orphan_high_surrogate_does_not_swallow_the_next_key(void)
{
	WfTerm t = {0};
	WfEvent e;

	CHECK(!feed(&t, 0, 0, 0xD842, &e));
	CHECK(feed(&t, VK_RETURN, 0x1C, L'\r', &e));
	CHECK(WF_EV_KEY == e.type && WF_KEY_ENTER == e.key);

	CHECK(!feed(&t, 0, 0, 0xD842, &e));
	CHECK(feed(&t, 'A', 0x1E, L'a', &e));
	CHECK(WF_EV_CHAR == e.type && L'a' == e.ch);
	CHECK(0 == t.pending_high);

	/* the same inside a text field, where editing keys are translated first:
	 * an orphan must not pair with a later lone low surrogate */
	t.editing = 1;
	CHECK(!feed(&t, 0, 0, 0xD842, &e));
	CHECK(feed(&t, VK_LEFT, 0x4B, 0, &e));
	CHECK(WF_EV_KEY == e.type && WF_KEY_LEFT == e.key);
	CHECK(feed(&t, 0, 0, 0xDFB7, &e));
	CHECK(WF_CP_REPLACEMENT == e.ch);
}

/* Enter is decided by the virtual key. A carriage return that arrives while
 * the IME is processing the key (VK_PROCESSKEY) is not an Enter. */
static void process_key_carriage_return_is_not_enter(void)
{
	WfTerm t = {0};
	WfEvent e;

	CHECK(!feed(&t, VK_PROCESSKEY, 0x1C, L'\r', &e));
	CHECK(!feed(&t, VK_PROCESSKEY, 0x1C, 0, &e));
	CHECK(feed(&t, VK_RETURN, 0x1C, L'\r', &e));
	CHECK(WF_EV_KEY == e.type && WF_KEY_ENTER == e.key);
}

static bool mouse_rec(
        WfTerm *t, WORD x, WORD y, DWORD buttons, DWORD flags, WfEvent *e)
{
	INPUT_RECORD r = {0};

	r.EventType = MOUSE_EVENT;
	r.Event.MouseEvent.dwMousePosition.X = (SHORT)x;
	r.Event.MouseEvent.dwMousePosition.Y = (SHORT)y;
	r.Event.MouseEvent.dwButtonState = buttons;
	r.Event.MouseEvent.dwEventFlags = flags;
	e->type = WF_EV_CHAR;
	e->key = WF_KEY_NONE;
	e->ch = 0;
	e->vk = 0;
	return translate(t, &r, e);
}

static void the_pointer_gives_presses_double_clicks_and_wheel_steps(void)
{
	WfTerm t = {0};
	WfEvent e;

	/* a press is an event, with the cell it was in */
	CHECK(mouse_rec(&t, 12, 7, FROM_LEFT_1ST_BUTTON_PRESSED, 0, &e));
	CHECK(WF_EV_MOUSE == e.type && 12 == WF_MOUSE_X(&e) &&
	        7 == WF_MOUSE_Y(&e) && WF_MOUSE_LEFT == e.vk);
	/* holding it, moving with it down, and letting go are not */
	CHECK(!mouse_rec(
	        &t, 13, 7, FROM_LEFT_1ST_BUTTON_PRESSED, MOUSE_MOVED, &e));
	CHECK(!mouse_rec(&t, 13, 7, 0, 0, &e));
	CHECK(!mouse_rec(&t, 14, 7, 0, MOUSE_MOVED, &e));
	/* the second press of a double click */
	CHECK(mouse_rec(
	        &t, 12, 7, FROM_LEFT_1ST_BUTTON_PRESSED, DOUBLE_CLICK, &e));
	CHECK((WF_MOUSE_LEFT | WF_MOUSE_DOUBLE) == e.vk);
	CHECK(!mouse_rec(&t, 12, 7, 0, 0, &e));
	/* the right button */
	CHECK(mouse_rec(&t, 1, 2, RIGHTMOST_BUTTON_PRESSED, 0, &e));
	CHECK(WF_MOUSE_RIGHT == e.vk);
	CHECK(!mouse_rec(&t, 1, 2, 0, 0, &e));
	/* the wheel: the sign of the high word says which way */
	CHECK(mouse_rec(&t, 3, 4, (DWORD)(120 << 16), MOUSE_WHEELED, &e));
	CHECK(WF_MOUSE_WHEEL_UP == e.vk);
	CHECK(mouse_rec(
	        &t, 3, 4, (DWORD)((WORD)-120 << 16), MOUSE_WHEELED, &e));
	CHECK(WF_MOUSE_WHEEL_DOWN == e.vk);
	/* the editor does not get the pointer */
	t.editing = true;
	CHECK(!mouse_rec(&t, 12, 7, FROM_LEFT_1ST_BUTTON_PRESSED, 0, &e));
	CHECK(!mouse_rec(&t, 12, 7, 0, 0, &e));
	t.editing = false;
	CHECK(mouse_rec(&t, 0, 0, FROM_LEFT_1ST_BUTTON_PRESSED, 0, &e));
}

static bool key_down(WfTerm *t, WORD vk, wchar_t ch, DWORD ctl, WfEvent *e)
{
	INPUT_RECORD r = {0};

	r.EventType = KEY_EVENT;
	r.Event.KeyEvent.bKeyDown = TRUE;
	r.Event.KeyEvent.wVirtualKeyCode = vk;
	r.Event.KeyEvent.wVirtualScanCode = 0x1c;
	r.Event.KeyEvent.uChar.UnicodeChar = ch;
	r.Event.KeyEvent.dwControlKeyState = ctl;
	e->type = WF_EV_CHAR;
	e->key = WF_KEY_NONE;
	e->ch = 0;
	e->vk = 0;
	return translate(t, &r, e);
}

/* A pasted line break reaches the program as Enter and then Ctrl+Enter. */
static void a_pasted_line_break_is_one_enter(void)
{
	WfTerm t = {0};
	WfEvent e;

	CHECK(key_down(&t, VK_RETURN, L'\r', 0, &e) && WF_EV_KEY == e.type &&
	        WF_KEY_ENTER == e.key);
	CHECK(!key_down(&t, VK_RETURN, L'\n', LEFT_CTRL_PRESSED, &e));
	/* the one after that is a key of its own */
	CHECK(key_down(&t, VK_RETURN, L'\n', LEFT_CTRL_PRESSED, &e) &&
	        WF_KEY_ENTER == e.key);
	/* Ctrl+Enter alone has no Enter before it */
	{
		WfTerm u = {0};

		CHECK(key_down(&u, VK_RETURN, L'\n', LEFT_CTRL_PRESSED, &e) &&
		        WF_KEY_ENTER == e.key);
	}
	/* an Enter a while ago does not eat a later Ctrl+Enter */
	{
		WfTerm u = {0};

		CHECK(key_down(&u, VK_RETURN, L'\r', 0, &e));
		u.cr_tick = 1; /* long ago */
		CHECK(key_down(&u, VK_RETURN, L'\n', LEFT_CTRL_PRESSED, &e));
	}
}

static WfEvent ch(uint32_t c)
{
	return (WfEvent){WF_EV_CHAR, c, WF_KEY_NONE, 0};
}

static size_t pasted_count(const WfEvent *ev, size_t n)
{
	size_t k = 0;

	for (size_t i = 0; i < n; i++) {
		k += 0 != (ev[i].vk & WF_VK_PASTED);
	}
	return k;
}

static void a_burst_of_different_keys_is_a_paste(void)
{
	WfTerm t = {0};
	WfEvent ev[8];

	/* three different keys in one read: a paste (even a short one) */
	ev[0] = ch('r');
	ev[1] = ch('m');
	ev[2] = ch(' ');
	mark_paste(&t, ev, 3, 1000);
	CHECK(3 == pasted_count(ev, 3));

	/* two keys, or a held key, are typing */
	memset(&t, 0, sizeof t);
	ev[0] = ch('d');
	ev[1] = ch('d');
	mark_paste(&t, ev, 2, 1000);
	CHECK(0 == pasted_count(ev, 2));
	memset(&t, 0, sizeof t);
	ev[0] = ch('g');
	ev[1] = ch('h');
	mark_paste(&t, ev, 2, 1000);
	CHECK(0 == pasted_count(ev, 2));
	for (int i = 0; i < 8; i++) {
		ev[i] = ch('j');
	}
	mark_paste(&t, ev, 8, 1000);
	CHECK(0 == pasted_count(ev, 8));

	/* an Enter with one other key is a pasted line */
	memset(&t, 0, sizeof t);
	ev[0] = ch('l');
	ev[1] = (WfEvent){WF_EV_KEY, 0, WF_KEY_ENTER, 13};
	mark_paste(&t, ev, 2, 1000);
	CHECK(2 == pasted_count(ev, 2) && 13 == (ev[1].vk & 0xFF));
	/* two Enters alone are not */
	memset(&t, 0, sizeof t);
	ev[0] = (WfEvent){WF_EV_KEY, 0, WF_KEY_ENTER, 13};
	ev[1] = ev[0];
	mark_paste(&t, ev, 2, 1000);
	CHECK(0 == pasted_count(ev, 2));
	/* arrow keys are never part of it */
	memset(&t, 0, sizeof t);
	ev[0] = ch('a');
	ev[1] = ch('b');
	ev[2] = ch('c');
	ev[3] = (WfEvent){WF_EV_KEY, 0, WF_KEY_UP, 38};
	mark_paste(&t, ev, 4, 1000);
	CHECK(3 == pasted_count(ev, 4) && 0 == (ev[3].vk & WF_VK_PASTED));
}

/* A long paste arrives in several reads: the rest of it is still the paste. */
static void the_rest_of_a_paste_in_later_reads_is_marked_too(void)
{
	WfTerm t = {0};
	WfEvent a[3], b[1], c[1];

	a[0] = ch('a');
	a[1] = ch('b');
	a[2] = ch('c');
	mark_paste(&t, a, 3, 1000);
	CHECK(3 == pasted_count(a, 3));
	b[0] = ch('d'); /* the next read, a few ms later */
	mark_paste(&t, b, 1, 1000 + WF_PASTE_TAIL_MS - 1);
	CHECK(1 == pasted_count(b, 1));
	/* each marked key extends the window */
	c[0] = ch('e');
	mark_paste(&t, c, 1, 1000 + 2 * WF_PASTE_TAIL_MS - 2);
	CHECK(1 == pasted_count(c, 1));
	/* a key after a pause is typing again */
	c[0] = ch('f');
	mark_paste(&t, c, 1, 1000 + 10 * WF_PASTE_TAIL_MS);
	CHECK(0 == pasted_count(c, 1));
}

static INPUT_RECORD held(WORD vk, wchar_t c, WORD repeat, BOOL down)
{
	INPUT_RECORD r = {0};

	r.EventType = KEY_EVENT;
	r.Event.KeyEvent.bKeyDown = down;
	r.Event.KeyEvent.wVirtualKeyCode = vk;
	r.Event.KeyEvent.uChar.UnicodeChar = c;
	r.Event.KeyEvent.wRepeatCount = repeat;
	return r;
}

/* A key held down can come as one record that says how many times it repeated:
 * each is an event. What a read cannot hold goes to the next, in order, and the
 * records after it wait their turn. */
static void a_repeat_count_gives_that_many_events(void)
{
	WfTerm t = {0};
	WfEvent ev[16];
	INPUT_RECORD r[3];
	size_t n;

	r[0] = held('A', L'a', 5, TRUE);
	n = take_records(&t, r, 1, ev, 0, 16);
	CHECK(5 == n && 0 == t.rep_left && 0 == t.npend);
	for (size_t i = 0; i < n; i++) {
		CHECK(WF_EV_CHAR == ev[i].type && 'a' == ev[i].ch);
	}
	r[0] = held(VK_DOWN, 0, 5, TRUE);
	n = take_records(&t, r, 1, ev, 0, 16);
	CHECK(5 == n && WF_EV_KEY == ev[4].type && WF_KEY_DOWN == ev[4].key);
	/* a count of 0 or 1 is one event, and a key-up is not repeated */
	r[0] = held('B', L'b', 0, TRUE);
	r[1] = held('C', L'c', 1, TRUE);
	r[2] = held(VK_DOWN, 0, 5, FALSE);
	n = take_records(&t, r, 3, ev, 0, 16);
	CHECK(n >= 2 && n <= 3 && 'b' == ev[0].ch && 'c' == ev[1].ch);
	CHECK(2 == n || WF_EV_RELEASE == ev[2].type);
}

static void what_does_not_fit_is_kept_for_the_next_read(void)
{
	WfTerm t = {0};
	WfEvent ev[4];
	INPUT_RECORD r[3];
	size_t n;

	r[0] = held('A', L'a', 6, TRUE);
	r[1] = held('B', L'b', 2, TRUE);
	r[2] = held('C', L'c', 1, TRUE);
	n = take_records(&t, r, 3, ev, 0, 4); /* room for 4 */
	CHECK(4 == n && 2 == t.rep_left && 2 == t.npend);
	for (size_t i = 0; i < n; i++) {
		CHECK('a' == ev[i].ch);
	}
	n = take_records(
	        &t, t.pend, t.npend, ev, 0, 4); /* the 2 a's, then b b */
	CHECK(4 == n && 0 == t.rep_left && 1 == t.npend);
	CHECK('a' == ev[0].ch && 'a' == ev[1].ch && 'b' == ev[2].ch &&
	        'b' == ev[3].ch);
	n = take_records(&t, t.pend, t.npend, ev, 0, 4);
	CHECK(1 == n && 'c' == ev[0].ch && 0 == t.npend && 0 == t.rep_left);
}

static void modal_drain_keeps_buffered_key_ups_in_fifo_order(void)
{
	WfTerm t = {0};
	WfEvent events[8] = {0};
	t.kept[0] = (WfEvent){WF_EV_RELEASE, 'a', WF_KEY_NONE, 0};
	t.kept[1] = (WfEvent){WF_EV_RELEASE, 'b', WF_KEY_NONE, 0};
	t.nkept = 2;
	t.pend[0] = held('C', L'c', 1, FALSE);
	t.pend[1] = held('X', L'x', 1, TRUE);
	t.npend = 2;
	t.rep_ev = (WfEvent){WF_EV_CHAR, 'x', WF_KEY_NONE, 0};
	t.rep_left = 5;
	wf_term_drain_input(&t);
	CHECK(!t.npend && !t.rep_left && t.nkept == 3);
	CHECK(1 == wf_term_read_events(&t, events, 1) && events[0].ch == 'a');
	CHECK(2 == wf_term_read_events(&t, events, 8));
	CHECK(events[0].ch == 'b' && events[1].ch == 'c');
	CHECK(!wf_term_has_pending_input(&t));
}

static void mode_changes_preserve_pending_input_in_delivery_order(void)
{
	WfTerm t = {0};
	WfEvent events[8] = {0};
	t.kept[0] = (WfEvent){WF_EV_RELEASE, 'z', WF_KEY_NONE, 0};
	t.nkept = 1;
	t.rep_ev = (WfEvent){WF_EV_CHAR, 'x', WF_KEY_NONE, 0};
	t.rep_left = 2;
	t.pend[0].EventType = KEY_EVENT;
	t.pend[0].Event.KeyEvent.bKeyDown = TRUE;
	t.pend[0].Event.KeyEvent.wVirtualKeyCode = 'B';
	t.pend[0].Event.KeyEvent.uChar.UnicodeChar = L'b';
	t.npend = 1;
	CHECK(wf_term_has_pending_input(&t));
	CHECK(WF_OK == wf_term_set_modified_keys(&t, true));
	CHECK(t.nkept == 1 && t.rep_left == 2 && t.npend == 1);
	CHECK(WF_OK == wf_term_set_modified_keys(&t, false));
	CHECK(WF_OK == wf_term_set_editing(&t, true));
	CHECK(WF_OK == wf_term_set_editing(&t, false));
	CHECK(t.nkept == 1 && t.rep_left == 2 && t.npend == 1);
	CHECK(4 == wf_term_read_events(&t, events, 8));
	CHECK(events[0].ch == 'z' && events[1].ch == 'x' &&
	        events[2].ch == 'x' && events[3].ch == 'b');
	CHECK(!wf_term_has_pending_input(&t));
}

int main(void)
{
	mode_changes_preserve_pending_input_in_delivery_order();
	modal_drain_keeps_buffered_key_ups_in_fifo_order();
	a_repeat_count_gives_that_many_events();
	what_does_not_fit_is_kept_for_the_next_read();
	the_pointer_gives_presses_double_clicks_and_wheel_steps();
	a_pasted_line_break_is_one_enter();
	a_burst_of_different_keys_is_a_paste();
	the_rest_of_a_paste_in_later_reads_is_marked_too();
	committed_text_arrives_as_characters();
	the_virtual_key_of_a_processed_letter_is_kept();
	surrogate_pairs_become_one_character();
	conpty_alt_delivered_pair_is_one_character();
	orphan_high_surrogate_does_not_swallow_the_next_key();
	process_key_carriage_return_is_not_enter();
	terminal_replies_are_not_commands();
	check_key(VK_F2, 0, WF_KEY_F2);
	check_key(VK_DELETE, 0, WF_KEY_DELETE);
	check_key(VK_HOME, 0, WF_KEY_HOME);
	check_key(VK_END, 0, WF_KEY_END);
	check_key('L', LEFT_CTRL_PRESSED, WF_KEY_CTRL_L);
	check_key('L', RIGHT_CTRL_PRESSED, WF_KEY_CTRL_L);
	check_key(VK_UP, LEFT_CTRL_PRESSED, WF_KEY_CTRL_UP);
	check_key(VK_DOWN, RIGHT_CTRL_PRESSED, WF_KEY_CTRL_DOWN);
	check_key(VK_UP, 0, WF_KEY_UP);
	check_key(VK_DOWN, SHIFT_PRESSED, WF_KEY_DOWN);
	check_key(VK_RIGHT, LEFT_CTRL_PRESSED, WF_KEY_RIGHT);
	check_key(VK_UP, LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED, WF_KEY_UP);
	check_key(VK_DOWN, LEFT_CTRL_PRESSED | SHIFT_PRESSED, WF_KEY_DOWN);
	check_unmapped('L', 0);
	check_unmapped('L', LEFT_CTRL_PRESSED | LEFT_ALT_PRESSED);
	check_unmapped('L', LEFT_CTRL_PRESSED | SHIFT_PRESSED);
	return TEST_RESULT();
}

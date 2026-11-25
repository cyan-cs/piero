/* Editing-mode key translation (the real static translator, no console). The
 * file manager's translation, covered by test_term_input, must not change. */
#include "../src/term/wf_term.c"
#include "wf_test.h"

static WfEvent run(WORD vk, DWORD controls, wchar_t ch, bool editing, bool *got)
{
	WfTerm term = {0};
	INPUT_RECORD record = {0};
	WfEvent event = {WF_EV_CHAR, 0, WF_KEY_NONE, 0};

	term.editing = editing;
	record.EventType = KEY_EVENT;
	record.Event.KeyEvent.bKeyDown = TRUE;
	record.Event.KeyEvent.wVirtualKeyCode = vk;
	record.Event.KeyEvent.dwControlKeyState = controls;
	record.Event.KeyEvent.uChar.UnicodeChar = ch;
	*got = translate(&term, &record, &event);
	return event;
}

static void expect_ctrl(
        WORD vk, DWORD controls, wchar_t ch, WfEventType type, uint32_t letter)
{
	bool got;
	WfEvent e = run(vk, controls, ch, true, &got);

	CHECK(got && e.type == type && e.ch == letter);
}

static void expect_key(WORD vk, DWORD controls, wchar_t ch, WfKey key)
{
	bool got;
	WfEvent e = run(vk, controls, ch, true, &got);

	CHECK(got && WF_EV_KEY == e.type && e.key == key);
}

static void control_letters(void)
{
	expect_ctrl('S', LEFT_CTRL_PRESSED, 0x13, WF_EV_CTRL, 's');
	expect_ctrl('X', RIGHT_CTRL_PRESSED, 0x18, WF_EV_CTRL, 'x');
	expect_ctrl('S', LEFT_CTRL_PRESSED | SHIFT_PRESSED, 0x13,
	        WF_EV_CTRL_SHIFT, 's');
	expect_ctrl('6', LEFT_CTRL_PRESSED, 0x1E, WF_EV_CTRL, '6');
	expect_ctrl(VK_OEM_MINUS, LEFT_CTRL_PRESSED | SHIFT_PRESSED, 0x1F,
	        WF_EV_CTRL, '_');
	/* Ctrl+H is not Backspace, Ctrl+I not Tab, Ctrl+M not Enter */
	expect_ctrl('H', LEFT_CTRL_PRESSED, 0x08, WF_EV_CTRL, 'h');
	expect_ctrl('I', LEFT_CTRL_PRESSED, 0x09, WF_EV_CTRL, 'i');
	expect_ctrl('M', LEFT_CTRL_PRESSED, 0x0D, WF_EV_CTRL, 'm');
	expect_key(VK_BACK, 0, 0x08, WF_KEY_BACKSPACE);
	expect_key(VK_TAB, 0, 0x09, WF_KEY_TAB);
	expect_key(VK_RETURN, 0, 0x0D, WF_KEY_ENTER);
	/* Ctrl+C reaches the editor as a key */
	expect_ctrl('C', LEFT_CTRL_PRESSED, 0x03, WF_EV_CTRL, 'c');
}

static void alt_chords_and_altgr(void)
{
	bool got;
	WfEvent e;

	expect_ctrl('R', LEFT_ALT_PRESSED, 'r', WF_EV_ALT, 'r');
	expect_ctrl('6', LEFT_ALT_PRESSED, '6', WF_EV_ALT, '6');
	expect_ctrl('U', RIGHT_ALT_PRESSED, 'u', WF_EV_ALT, 'u');
	/* AltGr (Ctrl+Alt with a character) is that character, not a chord */
	e = run('Q', LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED, '@', true, &got);
	CHECK(got && WF_EV_CHAR == e.type && '@' == e.ch);
	/* plain typing and non-ASCII input are untouched */
	e = run('A', 0, 'a', true, &got);
	CHECK(got && WF_EV_CHAR == e.type && 'a' == e.ch);
	e = run(0, 0, 0x65E5, true, &got);
	CHECK(got && WF_EV_CHAR == e.type && 0x65E5 == e.ch);
}

static void navigation_with_modifiers(void)
{
	expect_key(VK_LEFT, SHIFT_PRESSED, 0, WF_KEY_SHIFT_LEFT);
	expect_key(VK_RIGHT, LEFT_CTRL_PRESSED, 0, WF_KEY_CTRL_RIGHT);
	expect_key(VK_LEFT, LEFT_CTRL_PRESSED | SHIFT_PRESSED, 0,
	        WF_KEY_CTRL_SHIFT_LEFT);
	expect_key(VK_UP, SHIFT_PRESSED, 0, WF_KEY_SHIFT_UP);
	expect_key(VK_HOME, LEFT_CTRL_PRESSED, 0, WF_KEY_CTRL_HOME);
	expect_key(VK_END, LEFT_CTRL_PRESSED | SHIFT_PRESSED, 0,
	        WF_KEY_CTRL_SHIFT_END);
	expect_key(VK_PRIOR, 0, 0, WF_KEY_PGUP);
	expect_key(VK_NEXT, SHIFT_PRESSED, 0, WF_KEY_SHIFT_PGDN);
	expect_key(VK_TAB, SHIFT_PRESSED, 0x09, WF_KEY_SHIFT_TAB);
	expect_key(VK_DELETE, LEFT_CTRL_PRESSED, 0, WF_KEY_CTRL_DELETE);
	expect_key(VK_BACK, LEFT_CTRL_PRESSED, 0x7F, WF_KEY_CTRL_BACKSPACE);
	expect_key(VK_INSERT, 0, 0, WF_KEY_INSERT);
	expect_key(VK_F3, 0, 0, WF_KEY_F3);
	expect_key(VK_ESCAPE, 0, 0x1B, WF_KEY_ESC);
}

/* Outside editing mode nothing new is produced: the file manager sees what it
 * always saw. */
static void the_file_manager_mapping_is_unchanged(void)
{
	bool got;
	WfEvent e;

	e = run('S', LEFT_CTRL_PRESSED, 0x13, false, &got);
	CHECK(!got);
	e = run('R', LEFT_ALT_PRESSED, 'r', false, &got);
	CHECK(got && WF_EV_CHAR == e.type);
	e = run(VK_LEFT, SHIFT_PRESSED, 0, false, &got);
	CHECK(got && WF_EV_KEY == e.type && WF_KEY_LEFT == e.key);
	e = run(VK_RIGHT, LEFT_CTRL_PRESSED, 0, false, &got);
	CHECK(got && WF_EV_KEY == e.type && WF_KEY_RIGHT == e.key);
	/* paging keys reach the file manager now: the PDF preview turns pages
	 * with them (they were the one thing its reader dropped) */
	e = run(VK_PRIOR, 0, 0, false, &got);
	CHECK(got && WF_EV_KEY == e.type && WF_KEY_PGUP == e.key);
	e = run(VK_NEXT, 0, 0, false, &got);
	CHECK(got && WF_EV_KEY == e.type && WF_KEY_PGDN == e.key);
}

int main(void)
{
	control_letters();
	alt_chords_and_altgr();
	navigation_with_modifiers();
	the_file_manager_mapping_is_unchanged();
	return TEST_RESULT();
}

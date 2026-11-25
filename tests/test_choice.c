/*
 * The shared answer model of short questions (Save/Discard/Cancel ...).
 * Enter answers with the selection, which starts on Cancel. Only configured
 * ASCII letters answer directly; VK_PROCESSKEY and unmatched text are ignored.
 */
#include <string.h>

#include "input/wf_choice.h"
#include "wf_test.h"

static WfEvent ch(uint32_t c, uint16_t vk)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, vk};

	return e;
}

static WfEvent key(WfKey k)
{
	WfEvent e = {WF_EV_KEY, 0, k, 0};

	return e;
}

static WfChoiceStep step(WfChoice *c, WfEvent e, char *answer)
{
	*answer = 0;
	return wf_choice_event(c, &e, answer);
}

static void a_choice_starts_on_cancel(void)
{
	WfChoice c;
	char a;

	wf_choice_begin(&c, "sdc", 2);
	CHECK(3 == c.n && 'c' == wf_choice_selected(&c));
	/* a stray Enter cancels */
	CHECK(WF_CHOICE_ANSWER == step(&c, key(WF_KEY_ENTER), &a) && 'c' == a);
	CHECK(WF_CHOICE_ANSWER == step(&c, key(WF_KEY_ESC), &a) && 'c' == a);
	{
		WfEvent ctrl_c = {WF_EV_CTRL, 'c', WF_KEY_NONE, 0};

		CHECK(WF_CHOICE_ANSWER == step(&c, ctrl_c, &a) && 'c' == a);
	}
}

static void up_and_down_select_and_enter_answers(void)
{
	WfChoice c;
	char a;

	wf_choice_begin(&c, "sdc", 2);
	CHECK(WF_CHOICE_MOVED == step(&c, key(WF_KEY_UP), &a) &&
	        'd' == wf_choice_selected(&c));
	CHECK(WF_CHOICE_MOVED == step(&c, key(WF_KEY_UP), &a) &&
	        's' == wf_choice_selected(&c));
	CHECK(WF_CHOICE_MOVED == step(&c, key(WF_KEY_UP), &a) &&
	        'c' == wf_choice_selected(&c)); /* wraps */
	CHECK(WF_CHOICE_MOVED == step(&c, key(WF_KEY_DOWN), &a) &&
	        's' == wf_choice_selected(&c)); /* wraps the other way */
	CHECK(WF_CHOICE_MOVED == step(&c, key(WF_KEY_RIGHT), &a) &&
	        'd' == wf_choice_selected(&c));
	CHECK(WF_CHOICE_MOVED == step(&c, key(WF_KEY_LEFT), &a) &&
	        's' == wf_choice_selected(&c));
	CHECK(WF_CHOICE_ANSWER == step(&c, key(WF_KEY_ENTER), &a) && 's' == a);
	/* Esc is Cancel whatever is selected */
	CHECK(WF_CHOICE_ANSWER == step(&c, key(WF_KEY_ESC), &a) && 'c' == a);
}

static void letters_answer_in_either_case(void)
{
	WfChoice c;
	char a;

	wf_choice_begin(&c, "sdc", 2);
	CHECK(WF_CHOICE_ANSWER == step(&c, ch('s', 'S'), &a) && 's' == a);
	CHECK(WF_CHOICE_ANSWER == step(&c, ch('D', 'D'), &a) && 'd' == a);
	CHECK(WF_CHOICE_ANSWER == step(&c, ch('c', 'C'), &a) && 'c' == a);
	/* a letter that comes with no virtual key (text from a terminal) */
	CHECK(WF_CHOICE_ANSWER == step(&c, ch('d', 0), &a) && 'd' == a);
	/* the selection does not change what a letter means */
	step(&c, key(WF_KEY_UP), &a);
	CHECK(WF_CHOICE_ANSWER == step(&c, ch('s', 0), &a) && 's' == a);
}

static void process_keys_and_unmatched_text_are_ignored(void)
{
	static const uint32_t other[] = {'x', ' ', '1', 0xFF53, 0xFF44, 0xFF43,
	        0xFF33, 0xFF24, 0x3059, 0x3067, 0x3061, 0x1F600, 0x20BB7,
	        0xFFFD};
	WfChoice c;
	size_t i;
	char a;

	wf_choice_begin(&c, "sdc", 2);
	for (i = 0; i < sizeof other / sizeof *other; i++) {
		CHECK(WF_CHOICE_IDLE == step(&c, ch(other[i], 0), &a));
	}
	/* the letters themselves, when the IME was handling the key */
	CHECK(WF_CHOICE_IDLE == step(&c, ch('s', WF_VK_PROCESSKEY), &a));
	CHECK(WF_CHOICE_IDLE == step(&c, ch('D', WF_VK_PROCESSKEY), &a));
	CHECK(WF_CHOICE_IDLE == step(&c, ch('c', WF_VK_PROCESSKEY), &a));
	/* none of it moved the selection */
	CHECK('c' == wf_choice_selected(&c));
	/* other keys and events are not for the question */
	CHECK(WF_CHOICE_IDLE == step(&c, key(WF_KEY_TAB), &a));
	CHECK(WF_CHOICE_IDLE == step(&c, key(WF_KEY_BACKSPACE), &a));
	CHECK(WF_CHOICE_IDLE == step(&c, key(WF_KEY_DELETE), &a));
	{
		WfEvent resize = {WF_EV_RESIZE, 0, WF_KEY_NONE, 0};
		WfEvent release = {WF_EV_RELEASE, 's', WF_KEY_NONE, 'S'};
		WfEvent alt = {WF_EV_ALT, 'd', WF_KEY_NONE, 'D'};
		WfEvent ctrl_d = {WF_EV_CTRL, 'd', WF_KEY_NONE, 'D'};

		CHECK(WF_CHOICE_IDLE == step(&c, resize, &a));
		CHECK(WF_CHOICE_IDLE == step(&c, release, &a));
		CHECK(WF_CHOICE_IDLE == step(&c, alt, &a));
		CHECK(WF_CHOICE_IDLE == step(&c, ctrl_d, &a));
	}
}

static void a_bad_description_answers_nothing(void)
{
	WfChoice c;
	char a;

	wf_choice_begin(&c, "", 0);
	CHECK(0 == c.n && 0 == wf_choice_selected(&c));
	CHECK(WF_CHOICE_IDLE == step(&c, key(WF_KEY_ENTER), &a));
	wf_choice_begin(&c, "sdc", 3); /* cancel out of range */
	CHECK(0 == c.n);
	CHECK(WF_CHOICE_IDLE == step(&c, key(WF_KEY_ENTER), &a));
	CHECK(WF_CHOICE_IDLE == step(&c, ch('s', 0), &a));
	wf_choice_begin(&c, "abcde", 0); /* too many */
	CHECK(0 == c.n);
	wf_choice_begin(&c, NULL, 0);
	CHECK(0 == c.n);
	/* two options */
	wf_choice_begin(&c, "yn", 1);
	CHECK('n' == wf_choice_selected(&c));
	CHECK(WF_CHOICE_MOVED == step(&c, key(WF_KEY_DOWN), &a) &&
	        'y' == wf_choice_selected(&c));
}

int main(void)
{
	a_choice_starts_on_cancel();
	up_and_down_select_and_enter_answers();
	letters_answer_in_either_case();
	process_keys_and_unmatched_text_are_ignored();
	a_bad_description_answers_nothing();
	return TEST_RESULT();
}

/*
 * The ':' command line: what it accepts and what it refuses. An empty quoted
 * word ("") is a word, not the end of the line: nothing after it is skipped.
 */
#include <string.h>
#include <stdint.h>

#include "input/wf_exline.h"
#include "wf_test.h"

static bool parse_fm(const char *text, WfEx *e)
{
	return wf_ex_parse(text, strlen(text), WF_EXS_FM, e);
}

static void sort_and_set_take_what_they_are_given(void)
{
	WfEx e;

	CHECK(parse_fm("sort name", &e) && WF_EX_SORT == e.id);
	CHECK(parse_fm("sort name asc", &e) && !e.descending);
	CHECK(parse_fm("sort size desc", &e) && e.descending);
	CHECK(parse_fm("sort name \"\"", &e) && WF_EX_SORT == e.id &&
	        !e.descending);
	CHECK(parse_fm("set hidden on", &e) && e.on);
	CHECK(parse_fm("set hidden off", &e) && !e.on);
	CHECK(!parse_fm("set hidden \"\"", &e) &&
	        WF_EXE_MISSING_ARG == e.error);
}

static void nothing_after_an_empty_word_is_skipped(void)
{
	WfEx e;

	CHECK(!parse_fm("sort name asc garbage", &e) &&
	        WF_EXE_TRAILING == e.error);
	CHECK(!parse_fm("sort name asc \"\" garbage", &e) &&
	        WF_EXE_TRAILING == e.error);
	CHECK(!parse_fm("sort name asc \"\"", &e) &&
	        WF_EXE_TRAILING == e.error);
	CHECK(!parse_fm("sort name \"\" garbage", &e) &&
	        WF_EXE_TRAILING == e.error);
	CHECK(!parse_fm("sort name \"\" asc", &e) &&
	        WF_EXE_TRAILING == e.error);
	CHECK(!parse_fm("set hidden on garbage", &e) &&
	        WF_EXE_TRAILING == e.error);
	CHECK(!parse_fm("set hidden on \"\" garbage", &e) &&
	        WF_EXE_TRAILING == e.error);
	CHECK(!parse_fm("set hidden on ''", &e) && WF_EXE_TRAILING == e.error);
	/* blanks at the end are not a word */
	CHECK(parse_fm("sort name asc   ", &e));
	CHECK(parse_fm("set hidden on\t ", &e));
}

/* A NUL ends the word the command is told by, but the text goes on past it:
 * what follows it would never be read. A line with a NUL is refused whole,
 * in either place, wherever the NUL stands. */
static void a_line_with_a_nul_is_never_a_command(void)
{
	static const char forced[] = "q!\0garbage";
	static const char plain[] = "q\0 more";
	static const char leading[] = "\0q!";
	static const char in_arg[] = "set hidden on\0x";
	WfEx e;

	CHECK(!wf_ex_parse(forced, sizeof forced - 1, WF_EXS_EDITOR, &e) &&
	        WF_EX_NONE == e.id && WF_EXE_UNKNOWN == e.error);
	CHECK(!strcmp(e.name, "q!"));
	CHECK(!wf_ex_parse(plain, sizeof plain - 1, WF_EXS_EDITOR, &e) &&
	        WF_EX_NONE == e.id);
	CHECK(!wf_ex_parse(leading, sizeof leading - 1, WF_EXS_EDITOR, &e) &&
	        WF_EX_NONE == e.id);
	CHECK(!wf_ex_parse(in_arg, sizeof in_arg - 1, WF_EXS_FM, &e) &&
	        WF_EX_NONE == e.id);
	/* the same commands without the NUL are what they were */
	CHECK(wf_ex_parse("q!", 2, WF_EXS_EDITOR, &e) && WF_EX_NONE != e.id);
}

static void explicit_lengths_and_terminator_boundaries(void)
{
	WfEx e;
	const char command[2] = {
	        'q', '!'}; /* deliberately not NUL-terminated */
	CHECK(wf_ex_parse(command, sizeof command, WF_EXS_EDITOR, &e));
	CHECK(e.id == WF_EX_QUIT_FORCE);
	CHECK(!wf_ex_parse(command, 0, WF_EXS_EDITOR, &e) &&
	        e.error == WF_EXE_EMPTY);
	char *text = malloc(2 * WF_EX_ARG_MAX);
	CHECK(text);
	if (!text) {
		return;
	}
	memset(text, 'x', 2 * WF_EX_ARG_MAX);
	CHECK(!wf_ex_parse(text, 2 * WF_EX_ARG_MAX, WF_EXS_EDITOR, &e));
	CHECK(e.error == WF_EXE_UNKNOWN && strlen(e.name) == sizeof e.name - 1);
	for (size_t at = 0; at < 2 * WF_EX_ARG_MAX; at++) {
		memset(text, 'x', 2 * WF_EX_ARG_MAX);
		text[at] = 0;
		CHECK(!wf_ex_parse(text, 2 * WF_EX_ARG_MAX, WF_EXS_EDITOR, &e));
		CHECK(e.error == WF_EXE_UNKNOWN && e.id == WF_EX_NONE);
		CHECK(strlen(e.name) ==
		        (at < sizeof e.name - 1 ? at : sizeof e.name - 1));
	}
	free(text);
	const char unicode[] = "go \xe6\x97\xa5\xe6\x9c\xac\xf0\x9f\x98\x80";
	CHECK(wf_ex_parse(unicode, sizeof unicode - 1, WF_EXS_FM, &e));
	CHECK(!strcmp(e.arg, unicode + 3));
}

int main(void)
{
	WfEx e;
	char oversized[2 * WF_EX_ARG_MAX + 1];

	CHECK(!wf_ex_parse(NULL, 0, WF_EXS_FM, &e) && WF_EXE_EMPTY == e.error &&
	        WF_EX_NONE == e.id);
	CHECK(!wf_ex_parse(NULL, 1, WF_EXS_FM, &e) &&
	        WF_EXE_UNKNOWN == e.error);
	CHECK(!wf_ex_parse("q", SIZE_MAX, WF_EXS_FM, &e) &&
	        WF_EXE_TOO_LONG == e.error);
	memset(oversized, ' ', sizeof oversized);
	CHECK(!wf_ex_parse(oversized, sizeof oversized, WF_EXS_FM, &e) &&
	        WF_EXE_TOO_LONG == e.error);
	sort_and_set_take_what_they_are_given();
	nothing_after_an_empty_word_is_skipped();
	a_line_with_a_nul_is_never_a_command();
	explicit_lengths_and_terminator_boundaries();
	return TEST_RESULT();
}

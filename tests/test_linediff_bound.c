/*
 * The line diff's own bounds, on its own: a text with more lines than it may
 * have is refused (WF_E_RANGE) from the text as it is, before anything is
 * copied or allocated, so the refusal is never an out-of-memory, and a length
 * that cannot take the closing NUL of a copy is refused before it is read.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wf_test.h"

static size_t allocs;
static bool fail_all;

static void *test_malloc(size_t n)
{
	allocs++;
	return fail_all ? NULL : malloc(n);
}

static void *test_calloc(size_t n, size_t size)
{
	allocs++;
	return fail_all ? NULL : calloc(n, size);
}

#define malloc test_malloc
#define calloc test_calloc
#include "../src/text/wf_linediff.c"
#undef calloc
#undef malloc

int main(void)
{
	WfLineDiff d;
	size_t big = (size_t)WF_LD_LINES_MAX + 1;
	char *many = malloc(big + 1), *few = malloc(8);

	memset(many, '\n', big);
	many[big] = '\0';
	memcpy(few, "x\ny\n", 5);

	/* one more line than the bound: refused, nothing allocated, whichever side */
	allocs = 0;
	CHECK(WF_E_RANGE == wf_linediff(many, big, few, 4, &d));
	CHECK(0 == allocs && !d.a_text && !d.h);
	CHECK(WF_E_RANGE == wf_linediff(few, 4, many, big, &d));
	CHECK(0 == allocs && !d.a_text && !d.h);
	/* and still a range error, not "no memory", when allocation would fail */
	fail_all = true;
	CHECK(WF_E_RANGE == wf_linediff(many, big, few, 4, &d));
	CHECK(WF_E_NOMEM == wf_linediff(few, 4, few, 4, &d));
	fail_all = false;
	/* a last line without a break counts */
	many[big - 1] = 'z';
	CHECK(WF_E_RANGE == wf_linediff(many, big, "", 0, &d));
	/* exactly the bound is accepted */
	CHECK(WF_OK == wf_linediff(many, big - 1, many, big - 1, &d));
	wf_linediff_free(&d);
	/* a length that cannot take a closing NUL is refused without reading */
	allocs = 0;
	CHECK(WF_E_RANGE == wf_linediff("x", SIZE_MAX, "y", 1, &d));
	CHECK(WF_E_RANGE == wf_linediff("x", 1, "y", SIZE_MAX, &d));
	CHECK(0 == allocs);
	/* the arguments that are not valid are still told apart */
	CHECK(WF_E_INVAL == wf_linediff(NULL, 3, "y", 1, &d));
	CHECK(WF_E_INVAL == wf_linediff("x", 1, "y", 1, NULL));
	free(many);
	free(few);
	return TEST_RESULT();
}

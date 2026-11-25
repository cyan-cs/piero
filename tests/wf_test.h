#ifndef WF_TEST_H
#define WF_TEST_H

#include <stdio.h>
#include <stdlib.h>

static int wf_test_failures;

/* Only the mutation harness sets WF_TEST_FAIL_FAST=1: a mutant is "killed" by
 * its first failing check, so it need not run the rest of a test that was
 * written to pass. Everything else keeps counting failures and running on. */
static inline void wf_test_failed(void)
{
	char *fast = NULL;
	size_t fast_n = 0;
	int stop;

	wf_test_failures++;
	stop = 0 == _dupenv_s(&fast, &fast_n, "WF_TEST_FAIL_FAST") && fast &&
	        '1' == fast[0] && !fast[1];
	free(fast);
	if (stop) {
		fflush(stdout);
		exit(1);
	}
}

/* Unconditional failure with a message (CHECK(!"msg") trips
 * -Wstring-conversion). */
#define CHECK_FAIL(msg)                                                        \
	do {                                                                   \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);           \
		wf_test_failed();                                              \
	} while (0)

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			wf_test_failed();                                      \
		}                                                              \
	} while (0)

#define TEST_RESULT() (wf_test_failures ? 1 : (puts("ok"), 0))

#endif

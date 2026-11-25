/* Allocation ownership in the actual completion implementation, including
 * partial lists. Windows ASan does not provide LeakSanitizer. */
#include <stdlib.h>
#include <string.h>
#include "edit/wf_etext.h"
#include "wf_test.h"

static void *tracked_ptrs[128];
static size_t owned_n;
static long fail_after = -1;
static bool tracking;
static bool refused(void)
{
	if (!tracking || fail_after < 0) {
		return false;
	}
	if (!fail_after) {
		return true;
	}
	fail_after--;
	return false;
}
static void remember(void *p)
{
	if (p && tracking) {
		CHECK(owned_n < 128);
		if (owned_n < 128) {
			tracked_ptrs[owned_n++] = p;
		}
	}
}
static void forget(void *p)
{
	for (size_t i = 0; i < owned_n; i++) {
		if (tracked_ptrs[i] == p) {
			tracked_ptrs[i] = tracked_ptrs[--owned_n];
			return;
		}
	}
}
static void *checked_malloc(size_t n)
{
	if (refused()) {
		return NULL;
	}
	void *p = wf_et_malloc(n);
	remember(p);
	return p;
}
static void *checked_realloc(void *p, size_t n)
{
	if (refused()) {
		return NULL;
	}
	size_t index = owned_n;
	for (size_t i = 0; i < owned_n; i++) {
		if (tracked_ptrs[i] == p) {
			index = i;
		}
	}
	void *next = wf_et_realloc(p, n);
	if (next) {
		if (index < owned_n) {
			tracked_ptrs[index] = next;
		} else {
			remember(next);
		}
	}
	return next;
}
static void checked_free(void *p)
{
	forget(p);
	free(p);
}
#define wf_et_malloc checked_malloc
#define wf_et_realloc checked_realloc
#define free checked_free
#include "../src/edit/wf_editor.c"
#undef free
#undef wf_et_realloc
#undef wf_et_malloc

int main(void)
{
	static const char text[] = "a\na00 a01 a02 a03 a04 a05 a06 a07 a08 a09 "
	                           "a10 a11 a12 a13 a14 a15";
	size_t failed = 0, completed = 0;

	/* the completion needs few allocations (a list of borrowed words and the
	 * one chosen word), so the later refusals never happen: every refusal point
	 * is either an error that leaves the text and the allocations as they were,
	 * or a completion that was made */
	for (long fail = 0; fail < 13; fail++) {
		WfEditor e;
		WfStatus st;
		CHECK(wf_ed_init(&e, NULL) == WF_OK);
		CHECK(wf_ed_set_text(&e, text, sizeof text - 1,
		              (WfEncoding){WF_ENC_UTF8, 0}) == WF_OK);
		e.cur = (WfPos){0, 1};
		e.comp_prefix = malloc(2);
		memcpy(e.comp_prefix, "a", 2);
		e.comp_len = 1;
		e.comp_start = 0;
		e.comp_end = e.cur;
		e.comp_gen = e.text.gen;
		e.comp_index = 7;
		tracking = true;
		fail_after = fail;
		st = wf_ed_complete_word(&e);
		tracking = false;
		if (e.text.bytes == sizeof text - 1 && !wf_ed_modified(&e)) {
			CHECK(WF_E_NOMEM == st); /* said, not "no completion" */
			CHECK(owned_n == 0); /* and nothing was left behind */
			failed++;
		} else {
			CHECK(WF_OK == st && wf_ed_modified(&e));
			completed++;
		}
		wf_ed_free(&e);
	}
	CHECK(failed >= 3 && completed >= 1); /* both outcomes were reached */
	return TEST_RESULT();
}

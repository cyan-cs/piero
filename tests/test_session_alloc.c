/* Fail each allocation in the real codec. A snapshot is complete or an error,
 * and failed decoding releases every partial field. No session files are used. */
#include <windows.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "base/wf_macro.h"
#include "wf_test.h"

static void *alloc_blocks[64];
static size_t alloc_owned, alloc_attempts, alloc_fail_at;

static size_t allocation_index(void *p)
{
	for (size_t i = 0; i < alloc_owned; i++) {
		if (alloc_blocks[i] == p) {
			return i;
		}
	}
	return alloc_owned;
}

static void remember_allocation(void *p)
{
	if (p) {
		CHECK(alloc_owned < WF_ARRAY_LEN(alloc_blocks));
		if (alloc_owned < WF_ARRAY_LEN(alloc_blocks)) {
			alloc_blocks[alloc_owned++] = p;
		}
	}
}

static void *checked_malloc(size_t bytes)
{
	if (++alloc_attempts == alloc_fail_at) {
		return NULL;
	}
	void *p = malloc(bytes);
	remember_allocation(p);
	return p;
}

static void *checked_realloc(void *p, size_t bytes)
{
	size_t index = allocation_index(p);

	if (++alloc_attempts == alloc_fail_at) {
		return NULL;
	}
	void *next = realloc(p, bytes);
	if (next) {
		if (index < alloc_owned) {
			alloc_blocks[index] = next;
		} else {
			remember_allocation(next);
		}
	}
	return next;
}

static void checked_free(void *p)
{
	if (p) {
		size_t index = allocation_index(p);

		CHECK(index < alloc_owned);
		if (index < alloc_owned) {
			alloc_blocks[index] = alloc_blocks[--alloc_owned];
		}
	}
	free(p);
}

#define malloc checked_malloc
#define realloc checked_realloc
#define free checked_free
#ifndef WF_SESSION_CODEC_SOURCE
#	define WF_SESSION_CODEC_SOURCE "../src/session/wf_session.c"
#endif
#include "../src/base/wf_buf.c"
#include WF_SESSION_CODEC_SOURCE
#include "session/wf_session_dedupe.h"
#undef free
#undef realloc
#undef malloc

static void complete_snapshots_or_nomem(void)
{
	wchar_t dir[] = L"C:\\work", entry[181], selected[256];
	wchar_t editor[] = L"C:\\work\\editor.txt";
	wchar_t *names[] = {selected};
	WfSession input = {.dir = dir,
	        .entry = entry,
	        .selected = names,
	        .n_selected = 1,
	        .has_edit = true,
	        .edit = {.path = editor}};
	char *expected;
	size_t expected_n;

	wmemset(entry, L'a', WF_ARRAY_LEN(entry) - 1);
	entry[WF_ARRAY_LEN(entry) - 1] = 0;
	wmemset(selected, L'b', WF_ARRAY_LEN(selected) - 1);
	selected[WF_ARRAY_LEN(selected) - 1] = 0;
	alloc_attempts = 0;
	CHECK(WF_OK == wf_session_encode(&input, &expected, &expected_n));
	size_t encode_allocations = alloc_attempts;
	CHECK(expected && encode_allocations > 0 && alloc_owned == 1);
	for (size_t fail = 1; fail <= encode_allocations + 1; fail++) {
		char *text = NULL;
		size_t n = 0;

		alloc_fail_at = fail;
		alloc_attempts = 0;
		WfStatus status = wf_session_encode(&input, &text, &n);
		if (fail <= encode_allocations) {
			CHECK(status == WF_E_NOMEM && !text && !n);
		} else {
			CHECK(status == WF_OK && text && n == expected_n);
			CHECK(text && !memcmp(text, expected, expected_n));
		}
		checked_free(text);
		CHECK(alloc_owned == 1);
	}
	alloc_fail_at = 0;
	alloc_attempts = 0;
	WfSession output;
	CHECK(WF_OK == wf_session_decode(expected, expected_n, &output));
	size_t decode_allocations = alloc_attempts;
	wf_session_free(&output);
	CHECK(decode_allocations > 0 && alloc_owned == 1);
	for (size_t fail = 1; fail <= decode_allocations + 1; fail++) {
		alloc_fail_at = fail;
		alloc_attempts = 0;
		WfStatus status =
		        wf_session_decode(expected, expected_n, &output);

		if (fail <= decode_allocations) {
			CHECK(status == WF_E_NOMEM);
			CHECK(!output.dir && !output.entry &&
			        !output.selected && !output.n_selected &&
			        !output.edit.path);
		} else {
			CHECK(status == WF_OK && output.has_edit &&
			        output.n_selected == 1);
			CHECK(output.entry && !wcscmp(output.entry, entry));
		}
		wf_session_free(&output);
		CHECK(alloc_owned == 1);
	}
	alloc_fail_at = 0;
	checked_free(expected);
	CHECK(alloc_owned == 0);
}

static void failed_dedupe_allocation_forgets_the_old_state(void)
{
	WfSessionDedupe seen = {0};

	CHECK(wf_dedupe_remember(&seen, "old", 3));
	alloc_attempts = 0;
	alloc_fail_at = 1;
	CHECK(!wf_dedupe_remember(&seen, "new", 3));
	CHECK(!seen.last && !seen.n);
	CHECK(!wf_dedupe_is_repeat(&seen, "old", 3));
	wf_dedupe_forget(&seen);
	alloc_fail_at = 0;
	CHECK(wf_dedupe_remember(&seen, NULL, 0));
	CHECK(wf_dedupe_is_repeat(&seen, NULL, 0));
	CHECK(!wf_dedupe_remember(&seen, "x", SIZE_MAX));
	CHECK(!seen.last && !seen.n);
	CHECK(!wf_dedupe_remember(&seen, NULL, 1));
	CHECK(!seen.last && !seen.n);
	CHECK(alloc_owned == 0);
}

int main(void)
{
	complete_snapshots_or_nomem();
	failed_dedupe_allocation_forgets_the_old_state();
	WfSession empty;
	CHECK(WF_E_CORRUPT == wf_session_decode(NULL, 0, &empty));
	CHECK(!empty.dir);
	return TEST_RESULT();
}

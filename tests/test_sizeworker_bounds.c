/*
 * The folder size worker's queue of folders still to list, on its own: what it
 * holds is bounded in count, in the bytes of the paths, and in the length of
 * one path; a path it cannot take stays with the caller; and when the first
 * queueing fails (no memory) the copy that was made for it is freed. Real
 * folder for the last check, removed afterwards.
 */
#include <windows.h>

#include <crtdbg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wf_test.h"

static bool fail_realloc;

static void *test_realloc(void *p, size_t n)
{
	return fail_realloc ? NULL : realloc(p, n);
}

#define realloc test_realloc
#include "../src/fs/wf_sizeworker.c"
#undef realloc

static wchar_t *path_of(size_t units)
{
	wchar_t *p = malloc((units + 1) * sizeof *p);

	for (size_t i = 0; i < units; i++) {
		p[i] = L'a';
	}
	p[units] = L'\0';
	return p;
}

static void the_bytes_of_the_paths_are_bounded(void)
{
	Pending p;
	size_t pushed = 0;
	wchar_t *s;

	memset(&p, 0, sizeof p);
	for (;;) {
		s = path_of(10000);
		if (PUSH_OK != pending_push(&p, s)) {
			break; /* still ours */
		}
		pushed++;
	}
	CHECK(PUSH_FULL == pending_push(&p, s));
	CHECK(pushed > 3000 && pushed < MAX_PENDING);
	CHECK(p.bytes <= MAX_PENDING_BYTES &&
	        p.bytes + 2 * 10001 > MAX_PENDING_BYTES);
	/* taking one off makes room, and the total follows */
	free(pending_pop(&p));
	CHECK(PUSH_OK == pending_push(&p, s));
	CHECK(p.bytes <= MAX_PENDING_BYTES);
	pending_free(&p);
	CHECK(0 == p.bytes && 0 == p.n);
}

static void the_count_and_the_length_of_a_path_are_bounded(void)
{
	Pending p;
	wchar_t *s;

	memset(&p, 0, sizeof p);
	for (size_t i = 0; i < MAX_PENDING; i++) {
		s = path_of(3);
		if (PUSH_OK != pending_push(&p, s)) {
			CHECK(0 && "the count bound is reached too early");
			free(s);
			break;
		}
	}
	s = path_of(3);
	CHECK(PUSH_FULL == pending_push(&p, s));
	free(s);
	pending_free(&p);
	/* one path: 32000 units may be entered, one more may not */
	s = path_of(MAX_PATH_UNITS);
	CHECK(PUSH_OK == pending_push(&p, s));
	pending_free(&p);
	s = path_of(MAX_PATH_UNITS + 1);
	CHECK(PUSH_FULL == pending_push(&p, s));
	free(s);
}

static void a_path_that_cannot_be_queued_stays_with_the_caller(void)
{
	Pending p;
	wchar_t *s = path_of(5);

	memset(&p, 0, sizeof p);
	fail_realloc = true;
	CHECK(PUSH_NOMEM == pending_push(&p, s));
	fail_realloc = false;
	CHECK(0 == p.n && 0 == p.bytes);
	free(s); /* a second free here would be the failure this guards against */
}

/* the folder's own copy, made to be queued first, is not lost when queueing it
 * fails */
static void the_first_folder_is_not_leaked_when_it_cannot_be_queued(void)
{
	wchar_t tmp[MAX_PATH], dir[MAX_PATH + 40];
	WfSizeWorker w;
#ifdef _DEBUG
	_CrtMemState before, after, diff;
#endif

	GetTempPathW(MAX_PATH, tmp);
	swprintf(dir, MAX_PATH + 40, L"%lspiero-sizebound-%lu", tmp,
	        GetCurrentProcessId());
	CHECK(CreateDirectoryW(dir, NULL));
	memset(&w, 0, sizeof w);
	w.slots = calloc(1, sizeof *w.slots);
	w.dirty = calloc(1, 1);
	w.n = 1;
	w.live_gen = 1;
#ifdef _DEBUG
	_CrtMemCheckpoint(&before);
#endif
	fail_realloc = true;
	count_folder(&w, 1, 0, dir);
	fail_realloc = false;
#ifdef _DEBUG
	_CrtMemCheckpoint(&after);
	CHECK(!_CrtMemDifference(&diff, &before, &after));
#else
	printf("skipped: the leak check needs the debug runtime\n");
#endif
	CHECK(w.slots[0].partial && WF_E_NOMEM == w.slots[0].st);
	/* and with memory, the same folder is counted */
	memset(w.slots, 0, sizeof *w.slots);
	count_folder(&w, 1, 0, dir);
	CHECK(w.slots[0].finished && !w.slots[0].partial && WF_OK == w.slots[0].st);
	free(w.slots);
	free(w.dirty);
	CHECK(RemoveDirectoryW(dir));
}

int main(void)
{
	the_bytes_of_the_paths_are_bounded();
	the_count_and_the_length_of_a_path_are_bounded();
	a_path_that_cannot_be_queued_stays_with_the_caller();
	the_first_folder_is_not_leaked_when_it_cannot_be_queued();
	return TEST_RESULT();
}

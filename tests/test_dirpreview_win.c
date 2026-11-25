/* The preview pane's listing worker against the real filesystem (Windows
 * only): the entry limit bounds what a huge folder costs, the newest request
 * is always answered, and a missing folder is reported rather than guessed. */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "fs/wf_fsworker.h"
#include "fs/wf_enum_win.h"
#include "fs/wf_watch_win.h"
#include "wf_test.h"

#define FILES 40u
#define LIMIT 10u

static bool cancel_now(void *ctx)
{
	(*(unsigned *)ctx)++;
	return true;
}

static void bounded_listing_and_subtree_watch(const wchar_t *big)
{
	WfDir *d = NULL;
	unsigned polls = 0;
	CHECK(wf_enum_dir_win_budget(big, 0, 4096, &d) == WF_E_RANGE && !d);
	CHECK(wf_enum_dir_win_controlled(
	              big, 0, 8u << 20, cancel_now, &polls, &d) != WF_OK);
	CHECK(polls != 0 && !d);
	WfWatch *watch = NULL;
	wchar_t parent[1100];
	wcscpy_s(parent, 1100, big);
	wchar_t *leaf = wcsrchr(parent, L'\\');
	CHECK(leaf != NULL);
	if (!leaf) {
		return;
	}
	*leaf = 0;
	CHECK(wf_watch_open(parent, &watch) == WF_OK);
	if (!watch) {
		return;
	}
	/* A child file update must invalidate a parent directory's cached preview. */
	wchar_t path[1100];
	swprintf(path, 1100, L"%ls\\f00.txt", big);
	HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(file != INVALID_HANDLE_VALUE);
	if (file != INVALID_HANDLE_VALUE) {
		DWORD done = 0;
		CHECK(WriteFile(file, "changed", 7, &done, NULL) && done == 7);
		CloseHandle(file);
		CHECK(WaitForSingleObject(wf_watch_event(watch), 5000) ==
		        WAIT_OBJECT_0);
		CHECK(wf_watch_consume(watch) && wf_watch_active(watch));
	}
	wf_watch_close(watch);
}

static bool wait_result(WfFsWorker *w, WfFsResult *r, uint32_t gen)
{
	ULONGLONG deadline = GetTickCount64() + 10000;

	while (GetTickCount64() < deadline) {
		if (wf_fsworker_poll(w, r)) {
			if (r->gen == gen) {
				return true;
			}
			wf_dir_free(r->dir);
			wf_dir_free(r->parent);
			continue;
		}
		WaitForSingleObject(wf_fsworker_event(w), 200);
	}
	return false;
}

static void make_files(P0Lab *lab)
{
	unsigned i;

	CHECK(p0_area_mkdir(lab->area, L"big"));
	CHECK(p0_area_mkdir(lab->area, L"empty"));
	for (i = 0; i < FILES; i++) {
		wchar_t rel[64], path[1100];
		HANDLE h;

		swprintf(rel, 64, L"big\\f%02u.txt", i);
		CHECK(p0_lab_path(lab, rel, path, 1100));
		h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
		        FILE_ATTRIBUTE_NORMAL, NULL);
		CHECK(INVALID_HANDLE_VALUE != h);
		if (INVALID_HANDLE_VALUE != h) {
			CloseHandle(h);
		}
		CHECK(p0_area_track(lab->area, rel, 0));
	}
}

int main(void)
{
	P0Lab lab;
	WfFsWorker *w = NULL, *full = NULL;
	WfFsPort port;
	WfFsResult r;
	wchar_t big[1024], empty[1024], gone[1100];

	CHECK(p0_lab_open(&lab, "dirpreview", 0));
	if (!lab.area) {
		return TEST_RESULT();
	}
	make_files(&lab);
	CHECK(p0_lab_path(&lab, L"big", big, 1024));
	CHECK(p0_lab_path(&lab, L"empty", empty, 1024));
	swprintf(gone, 1100, L"%ls\\nothing", big);
	bounded_listing_and_subtree_watch(big);

	CHECK(WF_OK == wf_fsworker_start_limited(&w, LIMIT));
	CHECK(WF_OK == wf_fsworker_start(&full));
	port = wf_fsworker_port(w);

	/* the limit bounds the snapshot and marks it partial */
	CHECK(WF_OK == port.submit(port.ctx, 1, big, NULL));
	CHECK(wait_result(w, &r, 1) && WF_OK == r.st && r.dir);
	CHECK(r.dir && LIMIT == r.dir->n && r.dir->truncated);
	wf_dir_free(r.dir);

	/* the usual worker still lists everything */
	port = wf_fsworker_port(full);
	CHECK(WF_OK == port.submit(port.ctx, 1, big, NULL));
	CHECK(wait_result(full, &r, 1) && WF_OK == r.st && r.dir);
	CHECK(r.dir && FILES == r.dir->n && !r.dir->truncated);
	wf_dir_free(r.dir);

	/* an empty folder and a missing one */
	port = wf_fsworker_port(w);
	CHECK(WF_OK == port.submit(port.ctx, 2, empty, NULL));
	CHECK(wait_result(w, &r, 2) && WF_OK == r.st && r.dir && 0 == r.dir->n);
	wf_dir_free(r.dir);
	CHECK(WF_OK == port.submit(port.ctx, 3, gone, NULL));
	CHECK(wait_result(w, &r, 3) && WF_OK != r.st && !r.dir);

	/* a burst: whatever else happens, the newest request is answered */
	{
		uint32_t gen;

		for (gen = 10; gen < 30; gen++) {
			CHECK(WF_OK == port.submit(port.ctx, gen, big, NULL));
		}
		CHECK(wait_result(w, &r, 29) && WF_OK == r.st);
		wf_dir_free(r.dir);
	}

	/* A delayed consumer retains only one completed preview, even when each
	 * new request finishes before the following request is submitted. */
	for (uint32_t gen = 100; gen < 106; gen++) {
		uint64_t before = wf_fsworker_stats(w).completed_requests;
		CHECK(WF_OK == port.submit(port.ctx, gen, big, NULL));
		ULONGLONG deadline = GetTickCount64() + 5000;
		while (wf_fsworker_stats(w).completed_requests == before &&
		        GetTickCount64() < deadline) {
			Sleep(1);
		}
		CHECK(wf_fsworker_stats(w).completed_requests > before);
	}
	CHECK(wf_fsworker_poll(w, &r) && r.gen == 105 && r.st == WF_OK);
	wf_dir_free(r.dir);
	wf_dir_free(r.parent);
	CHECK(!wf_fsworker_poll(w, &r));
	CHECK(wf_fsworker_stats(w).discarded_results >= 5);
	CHECK(WF_OK == wf_fsworker_stop(w).status);
	CHECK(WF_OK == wf_fsworker_stop(full).status);
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}

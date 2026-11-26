/*
 * The workers of # D L F (DR-DRAFT-035) are made when the first request needs
 * them and are stopped by the loop at the end; this pins down what that relies
 * on: a worker can be started and stopped over and over without a thread or a
 * handle staying behind, stopping a worker with a request in flight is safe and
 * leaves nothing, and stopping a worker that was never started is a no-op.
 */
#include <windows.h>

#include <tlhelp32.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _DEBUG
#	include <crtdbg.h>
#endif

#include "../tools/p0/p0_lab.h"
#include "async/wf_workcore.h"
#include "search/wf_query.h"
#include "base/wf_macro.h"
#include "clipboard/wf_clipworker_win.h"
#include "fs/wf_launchworker.h"
#include "fs/wf_sizeworker.h"
#include "preview/wf_imageworker.h"
#include "process/wf_lockworker_win.h"
#include "search/wf_dupehash_win.h"
#include "search/wf_searchworker.h"
#include "wf_test.h"
#include "xfer/wf_hashworker.h"

#define CYCLES 40

static size_t threads_now(void)
{
	DWORD pid = GetCurrentProcessId();
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	THREADENTRY32 te;
	size_t n = 0;

	if (INVALID_HANDLE_VALUE == snap) {
		return 0;
	}
	te.dwSize = sizeof te;
	for (BOOL ok = Thread32First(snap, &te); ok;
	        ok = Thread32Next(snap, &te)) {
		n += te.th32OwnerProcessID == pid;
	}
	CloseHandle(snap);
	return n;
}

static DWORD handles_now(void)
{
	DWORD h = 0;

	GetProcessHandleCount(GetCurrentProcess(), &h);
	return h;
}

static WfStatus no_list(
        const wchar_t *dir, bool (*cancel)(void *), void *cctx, WfDir **out)
{
	(void)dir;
	(void)cancel;
	(void)cctx;
	(void)out;
	return WF_E_NOENT;
}

static P0Lab lab;
static wchar_t big_file[1100], small_file[1100], folder[1100];

static void make_fixtures(void)
{
	HANDLE h;
	char block[65536];
	DWORD put;

	CHECK(p0_lab_path(&lab, L"big.bin", big_file, 1024));
	CHECK(p0_lab_path(&lab, L"small.txt", small_file, 1024));
	CHECK(p0_lab_path(&lab, L"dir", folder, 1024));
	CHECK(p0_area_mkdir(lab.area, L"dir"));
	CHECK(p0_area_mkdir(lab.area, L"dir\\a"));
	h = CreateFileW(big_file, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		memset(block, 'h', sizeof block);
		for (int i = 0; i < 160;
		        i++) { /* 10 MiB: a request that is still
			          running when it is stopped */
			CHECK(WriteFile(h, block, sizeof block, &put, NULL));
		}
		CloseHandle(h);
	}
	CHECK(p0_area_track(lab.area, L"big.bin", 0));
	h = CreateFileW(
	        small_file, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		CHECK(WriteFile(h, "abc", 3, &put, NULL));
		CloseHandle(h);
	}
	CHECK(p0_area_track(lab.area, L"small.txt", 0));
}

/* One kind of worker: start, optionally a request, stop. */
typedef struct
{
	const char *name;
	void *(*start)(void);
	void (*request)(void *w);
	void (*stop)(void *w);
	bool (*idle)(void *w);
	/* a small request, and every answer to it taken: false if it never came */
	bool (*run_to_end)(void *w, uint32_t gen);
	/* the request alone; the event the worker signals when an answer waits;
	 * and taking every answer that waits */
	bool (*post)(void *w, uint32_t gen);
	HANDLE (*event)(void *w);
	void (*drain)(void *w);
} Kind;

static void *start_hash(void)
{
	WfHashWorker *w = NULL;

	return WF_OK == wf_hashworker_start(&w) ? w : NULL;
}

static void ask_hash(void *w)
{
	WfHashPort p = wf_hashworker_port(w);

	CHECK(WF_OK == p.start(p.ctx, 1, big_file));
}

static void stop_hash(void *w)
{
	WfWorkerStopReport r = wf_hashworker_stop(w);

	CHECK(WF_OK == r.status && !r.deferred);
}

static void *start_size(void)
{
	WfSizeWorker *w = NULL;

	return WF_OK == wf_sizeworker_start(&w) ? w : NULL;
}

static void ask_size(void *w)
{
	WfSizePort p = wf_sizeworker_port(w);
	const wchar_t *names[] = {L"a"};

	CHECK(WF_OK == p.start(p.ctx, 1, folder, names, 1));
}

static void stop_size(void *w)
{
	WfWorkerStopReport r = wf_sizeworker_stop(w);

	CHECK(WF_OK == r.status && !r.deferred);
}

static void *start_lock(void)
{
	WfLockWorker *w = NULL;

	return WF_OK == wf_lockworker_start(&w, NULL) ? w : NULL;
}

static void ask_lock(void *w)
{
	WfLockPort p = wf_lockworker_port(w);

	CHECK(WF_OK == p.query(p.ctx, 1, big_file));
}

static void stop_lock(void *w)
{
	WfWorkerStopReport r = wf_lockworker_stop(w);

	CHECK(WF_OK == r.status && !r.deferred);
}

static bool idle_hash(void *w)
{
	return wf_hashworker_idle(w);
}
static bool idle_size(void *w)
{
	return wf_sizeworker_idle(w);
}
static bool idle_lock(void *w)
{
	return wf_lockworker_idle(w);
}
static bool idle_search(void *w)
{
	return wf_searchworker_idle(w);
}

static bool run_hash(void *w, uint32_t gen)
{
	WfHashPort p = wf_hashworker_port(w);
	ULONGLONG end = GetTickCount64() + 5000;

	if (WF_OK != p.start(p.ctx, gen, small_file)) {
		return false;
	}
	while (GetTickCount64() < end) {
		WfHashUpdate u;

		WaitForSingleObject(wf_hashworker_event(w), 50);
		while (wf_hashworker_poll(w, &u)) {
			CHECK(u.gen ==
			        gen); /* never the answer of an older request */
			if (u.finished) {
				return true;
			}
		}
	}
	return false;
}

static bool run_size(void *w, uint32_t gen)
{
	WfSizePort p = wf_sizeworker_port(w);
	const wchar_t *names[] = {L"a"};
	ULONGLONG end = GetTickCount64() + 5000;

	if (WF_OK != p.start(p.ctx, gen, folder, names, 1)) {
		return false;
	}
	while (GetTickCount64() < end) {
		WfSizeUpdate u;

		WaitForSingleObject(wf_sizeworker_event(w), 50);
		while (wf_sizeworker_poll(w, &u)) {
			CHECK(u.gen == gen);
			if (u.finished) {
				return true;
			}
		}
	}
	return false;
}

static bool run_lock(void *w, uint32_t gen)
{
	WfLockPort p = wf_lockworker_port(w);
	ULONGLONG end = GetTickCount64() + 10000;

	if (WF_OK != p.query(p.ctx, gen, small_file)) {
		return false;
	}
	while (GetTickCount64() < end) {
		WfLockAnswer a;

		WaitForSingleObject(wf_lockworker_event(w), 50);
		if (wf_lockworker_poll(w, &a)) {
			CHECK(a.gen == gen);
			wf_lockanswer_free(&a);
			return true;
		}
	}
	return false;
}

static bool run_search(void *w, uint32_t gen)
{
	WfSearchPort p = wf_searchworker_port(w);
	WfQuery q;
	size_t pos = 0;
	WfFileId none;
	ULONGLONG end = GetTickCount64() + 5000;

	memset(&none, 0, sizeof none);
	if (WF_OK != wf_query_parse(L"x", &q, &pos) ||
	        WF_OK != p.start(p.ctx, gen, folder, none, &q, false)) {
		return false;
	}
	while (GetTickCount64() < end) {
		WfSearchBatch b;

		WaitForSingleObject(wf_searchworker_event(w), 50);
		while (wf_searchworker_poll(w, &b)) {
			bool done = b.done;

			CHECK(b.gen == gen);
			wf_search_batch_free(&b);
			if (done) {
				return true;
			}
		}
	}
	return false;
}

static bool post_hash(void *w, uint32_t gen)
{
	WfHashPort p = wf_hashworker_port(w);

	return WF_OK == p.start(p.ctx, gen, small_file);
}

static bool post_size(void *w, uint32_t gen)
{
	WfSizePort p = wf_sizeworker_port(w);
	const wchar_t *names[] = {L"a"};

	return WF_OK == p.start(p.ctx, gen, folder, names, 1);
}

static bool post_lock(void *w, uint32_t gen)
{
	WfLockPort p = wf_lockworker_port(w);

	return WF_OK == p.query(p.ctx, gen, small_file);
}

static bool post_search(void *w, uint32_t gen)
{
	WfSearchPort p = wf_searchworker_port(w);
	WfQuery q;
	size_t pos = 0;
	WfFileId none;

	memset(&none, 0, sizeof none);
	return WF_OK == wf_query_parse(L"x", &q, &pos) &&
	        WF_OK == p.start(p.ctx, gen, folder, none, &q, false);
}

static HANDLE event_hash(void *w)
{
	return wf_hashworker_event(w);
}
static HANDLE event_size(void *w)
{
	return wf_sizeworker_event(w);
}
static HANDLE event_lock(void *w)
{
	return wf_lockworker_event(w);
}
static HANDLE event_search(void *w)
{
	return wf_searchworker_event(w);
}

static void drain_hash(void *w)
{
	WfHashUpdate u;

	while (wf_hashworker_poll(w, &u)) {
	}
}

static void drain_size(void *w)
{
	WfSizeUpdate u;

	while (wf_sizeworker_poll(w, &u)) {
	}
}

static void drain_lock(void *w)
{
	WfLockAnswer a;

	while (wf_lockworker_poll(w, &a)) {
		wf_lockanswer_free(&a);
	}
}

static void drain_search(void *w)
{
	WfSearchBatch b;

	while (wf_searchworker_poll(w, &b)) {
		wf_search_batch_free(&b);
	}
}

/* The clipboard worker is asked to read (the clipboard is not changed); the
 * launch worker is asked to open a file that is not there, which the program's
 * own check refuses before the shell is called, so no program is started. */
static void *start_clip(void)
{
	WfClipWorker *w = NULL;

	return WF_OK == wf_clipworker_start(&w) ? w : NULL;
}

static void stop_clip(void *w)
{
	WfWorkerStopReport r = wf_clipworker_stop(w);

	CHECK(WF_OK == r.status && !r.deferred);
}

static bool idle_clip(void *w)
{
	return wf_clipworker_idle(w);
}

static bool post_clip(void *w, uint32_t gen)
{
	WfClipFilesPort p = wf_clipworker_port(w);

	return WF_OK == p.read(p.ctx, gen);
}

static void ask_clip(void *w)
{
	CHECK(post_clip(w, 1));
}
static HANDLE event_clip(void *w)
{
	return wf_clipworker_event(w);
}

static void drain_clip(void *w)
{
	WfClipFiles f;

	while (wf_clipworker_poll(w, &f)) {
		wf_clipfiles_free(&f);
	}
}

static bool run_clip(void *w, uint32_t gen)
{
	ULONGLONG end = GetTickCount64() + 15000;

	if (!post_clip(w, gen)) {
		return false;
	}
	while (GetTickCount64() < end) {
		WfClipFiles f;

		WaitForSingleObject(wf_clipworker_event(w), 50);
		if (wf_clipworker_poll(w, &f)) {
			CHECK(f.gen == gen);
			wf_clipfiles_free(&f);
			return true;
		}
	}
	return false;
}

/* The picture worker is given no decoder to start: a request is answered at
 * once with "unavailable", which is an answer like any other (the cost of the
 * thread, the event and the unread answer are what is under test here; the
 * decoder host and the cache are in test_imageworker_win). */
static void *start_image(void)
{
	WfImageWorkerOptions o;
	WfImageWorker *w = NULL;

	memset(&o, 0, sizeof o);
	o.host_exe = L"C:\\no\\such\\host.exe";
	o.sandbox = 0;
	return WF_OK == wf_imageworker_start(&w, &o) ? w : NULL;
}

static void stop_image(void *w)
{
	WfWorkerStopReport r = wf_imageworker_stop(w);

	CHECK(WF_OK == r.status && !r.deferred);
}

static bool idle_image(void *w)
{
	return wf_imageworker_idle(w);
}

static bool post_image(void *w, uint32_t gen)
{
	WfImagePort p = wf_imageworker_port(w);
	WfImageRequest rq;

	memset(&rq, 0, sizeof rq);
	rq.gen = gen;
	rq.path = small_file;
	rq.kind = WF_DKIND_IMAGE;
	rq.session = 1;
	rq.box_w = 100;
	rq.box_h = 100;
	return WF_OK == p.submit(p.ctx, &rq);
}

static void ask_image(void *w)
{
	CHECK(post_image(w, 1));
}
static HANDLE event_image(void *w)
{
	return wf_imageworker_event(w);
}

static void drain_image(void *w)
{
	WfImageResult r;

	while (wf_imageworker_poll(w, &r)) {
		wf_pixmap_free(r.pm);
		wf_buf_free(&r.sixel);
	}
}

static bool run_image(void *w, uint32_t gen)
{
	ULONGLONG end = GetTickCount64() + 15000;

	if (!post_image(w, gen)) {
		return false;
	}
	while (GetTickCount64() < end) {
		WfImageResult r;

		WaitForSingleObject(wf_imageworker_event(w), 50);
		if (wf_imageworker_poll(w, &r)) {
			if (r.gen != gen || !r.unavailable) {
				printf("image response: requested=%u "
				       "received=%u "
				       "status=%d unavailable=%d refused=%d "
				       "Win32=%lu\n",
				        (unsigned)gen, (unsigned)r.gen,
				        (int)r.status, r.unavailable, r.refused,
				        r.os_error);
			}
			CHECK(r.gen == gen && r.unavailable);
			wf_pixmap_free(r.pm);
			wf_buf_free(&r.sixel);
			return true;
		}
	}
	return false;
}

static void *start_launch(void)
{
	WfLaunchWorker *w = NULL;

	return WF_OK == wf_launchworker_start(&w) ? w : NULL;
}

static void stop_launch(void *w)
{
	WfWorkerStopReport r = wf_launchworker_stop(w);

	CHECK(WF_OK == r.status && !r.deferred);
}

static bool idle_launch(void *w)
{
	return wf_launchworker_idle(w);
}

static bool post_launch(void *w, uint32_t gen)
{
	WfLaunchPort p = wf_launchworker_port(w);
	WfLaunchRequest req;
	wchar_t absent[1100];

	swprintf(absent, 1100, L"%ls.absent", small_file);
	memset(&req, 0, sizeof req);
	req.path = absent;
	req.id.kind = WF_ID_STRONG;
	req.directory = folder;
	return WF_OK == p.submit(p.ctx, gen, &req);
}

static void ask_launch(void *w)
{
	CHECK(post_launch(w, 1));
}
static HANDLE event_launch(void *w)
{
	return wf_launchworker_event(w);
}

static void drain_launch(void *w)
{
	WfLaunchResult r;

	while (wf_launchworker_poll(w, &r)) {
	}
}

static bool run_launch(void *w, uint32_t gen)
{
	ULONGLONG end = GetTickCount64() + 10000;

	if (!post_launch(w, gen)) {
		return false;
	}
	while (GetTickCount64() < end) {
		WfLaunchResult r;

		WaitForSingleObject(wf_launchworker_event(w), 50);
		if (wf_launchworker_poll(w, &r)) {
			CHECK(r.gen == gen && WF_OK != r.outcome.st);
			return true;
		}
	}
	return false;
}

static void *start_search(void)
{
	WfSearchWorker *w = NULL;

	if (WF_OK != wf_searchworker_start(&w, no_list)) {
		return NULL;
	}
	wf_searchworker_set_hash(w, wf_dupehash_file);
	return w;
}

static void stop_search(void *w)
{
	WfWorkerStopReport r = wf_searchworker_stop(w);

	CHECK(WF_OK == r.status && !r.deferred);
}

static void a_worker_comes_and_goes_without_leaving_a_thread_or_a_handle(
        const Kind *k)
{
	size_t threads_before;
	DWORD handles_before;

	/* the first start loads what a worker loads once (a library, a pool) */
	for (int warm = 0; warm < 3; warm++) {
		void *w = k->start();

		CHECK(NULL != w);
		if (w) {
			if (k->request) {
				k->request(w);
			}
			k->stop(w);
		}
	}
	Sleep(100);
	threads_before = threads_now();
	handles_before = handles_now();
	for (int i = 0; i < CYCLES; i++) {
		void *w = k->start();

		CHECK(NULL != w);
		if (!w) {
			return;
		}
		if (k->request && 0 == i % 2) {
			k->request(w); /* stopped with the request in flight */
		}
		k->stop(w);
	}
	Sleep(100);
	if (threads_now() != threads_before ||
	        handles_now() != handles_before) {
		printf("  %s: threads %zu -> %zu, handles %lu -> %lu\n",
		        k->name, threads_before, threads_now(),
		        (unsigned long)handles_before,
		        (unsigned long)handles_now());
	}
	CHECK(threads_now() == threads_before);
	CHECK(handles_now() == handles_before);
}

/* The whole life, again and again: activate, use, wait until the worker has
 * nothing in flight and nothing to poll, deactivate, activate again. Every
 * turn the worker is idle before the request, is not idle with it pending, and
 * is idle again once the answer was taken; nothing stays behind. */
static void a_worker_is_used_released_and_made_again(const Kind *k, int turns)
{
	size_t threads_before;
	DWORD handles_before;
#ifdef _DEBUG
	_CrtMemState heap_before, heap_after, heap_diff;
#endif

	for (int warm = 0; warm < 2; warm++) {
		void *w = k->start();

		CHECK(NULL != w && k->run_to_end(w, 1));
		if (w) {
			k->stop(w);
		}
	}
	Sleep(100);
	threads_before = threads_now();
	handles_before = handles_now();
#ifdef _DEBUG
	_CrtMemCheckpoint(&heap_before);
#endif
	for (int i = 0; i < turns; i++) {
		void *w = k->start();
		bool idle_again = false;

		CHECK(NULL != w);
		if (!w) {
			return;
		}
		CHECK(k->idle(
		        w)); /* just made: nothing to lose by stopping it */
		CHECK(k->run_to_end(w, (uint32_t)i + 1));
		for (int t = 0; t < 400 && !idle_again; t++) {
			idle_again = k->idle(w);
			if (!idle_again) {
				Sleep(5);
			}
		}
		CHECK(idle_again); /* the answer was taken and the thread is done */
		k->stop(w);
	}
	Sleep(100);
#ifdef _DEBUG
	/* what the workers made over all the turns is given back: the heap in use
	 * is what it was (the debug heap; a worker's own thread frees its state
	 * before it ends, which the stop has waited for) */
	_CrtMemCheckpoint(&heap_after);
	if (_CrtMemDifference(&heap_diff, &heap_before, &heap_after)) {
		printf("  %s: heap in use %+ld bytes in %+ld blocks after %d "
		       "turns\n",
		        k->name, (long)heap_diff.lSizes[_NORMAL_BLOCK],
		        (long)heap_diff.lCounts[_NORMAL_BLOCK], turns);
	}
	CHECK(heap_diff.lSizes[_NORMAL_BLOCK] <= 0);
#endif
	if (threads_now() != threads_before ||
	        handles_now() != handles_before) {
		printf("  %s: threads %zu -> %zu, handles %lu -> %lu\n",
		        k->name, threads_before, threads_now(),
		        (unsigned long)handles_before,
		        (unsigned long)handles_now());
	}
	CHECK(threads_now() == threads_before);
	CHECK(handles_now() == handles_before);
}

/* A worker with a request pending, or an answer waiting, is not idle: it must
 * not be stopped under either. */
static void a_worker_with_work_or_an_answer_waiting_is_not_idle(const Kind *k)
{
	void *w = k->start();

	CHECK(NULL != w);
	if (!w) {
		return;
	}
	if (k->request) {
		k->request(w);
		CHECK(!k->idle(w)); /* pending or running */
	}
	k->stop(w);
}

/* An answer that nobody has taken is work the worker still holds: it is not
 * idle until the screen has polled it, and stopping it then would lose it. */
static void a_worker_holding_an_answer_is_not_idle(const Kind *k)
{
	void *w = k->start();
	bool idle_at_last = false;
	bool completed = false;

	CHECK(NULL != w);
	if (!w) {
		return;
	}
	CHECK(k->post(w, 1));
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(k->event(w), 5000));
	/* Ready can be signalled before the worker clears busy. Wait for the
	 * completed request under the shared core lock, without polling away its
	 * answer. Otherwise busy alone can hide a broken unread-answer check.
	 * Every worker embeds WfWorkCore as its first field. */
	for (ULONGLONG end = GetTickCount64() + 5000;
	        !completed && GetTickCount64() < end;) {
		WfWorkCore *core = w;

		AcquireSRWLockShared(&core->lock);
		completed = core->stats.completed_requests == 1 && !core->busy;
		ReleaseSRWLockShared(&core->lock);
		if (!completed) {
			Sleep(1);
		}
	}
	CHECK(completed);
	if (!strcmp(k->name, "picture")) {
		WfImageWorkerStats stats = wf_imageworker_stats(w);

		CHECK(stats.worker.completed_requests == 1);
	}
	CHECK(!k->idle(w)); /* the answer waits, whatever the thread is doing */
	k->drain(w);
	for (int t = 0; t < 600 && !idle_at_last; t++) {
		k->drain(w); /* a request that goes on answers again */
		idle_at_last = k->idle(w);
		if (!idle_at_last) {
			Sleep(5);
		}
	}
	CHECK(idle_at_last);
	k->stop(w);
}

static bool fail_event_step(int step)
{
	return 0 == step;
}
static bool fail_thread_step(int step)
{
	return 1 == step;
}

/* A start that fails at either step leaves nothing open and nothing allocated,
 * gives the error back, and the next start works. */
static void a_start_that_fails_leaves_nothing_and_can_be_tried_again(
        const Kind *k)
{
	static bool (*const hooks[])(int) = {fail_event_step, fail_thread_step};
	size_t threads_before;
	DWORD handles_before;
#ifdef _DEBUG
	_CrtMemState before, after, diff;
#endif
	void *w;

	/* the first start loads what a worker loads once */
	w = k->start();
	CHECK(NULL != w);
	if (w) {
		k->stop(w);
	}
	Sleep(100);
	threads_before = threads_now();
	handles_before = handles_now();
#ifdef _DEBUG
	_CrtMemCheckpoint(&before);
#endif
	for (size_t h = 0; h < WF_ARRAY_LEN(hooks); h++) {
		wf_wc_test_fail = hooks[h];
		w = k->start();
		wf_wc_test_fail = NULL;
		CHECK(NULL == w); /* the error came back, no half-made worker */
		CHECK(threads_now() == threads_before);
		CHECK(handles_now() == handles_before);
	}
#ifdef _DEBUG
	_CrtMemCheckpoint(&after);
	CHECK(!_CrtMemDifference(&diff, &before, &after) ||
	        diff.lSizes[_NORMAL_BLOCK] <= 0);
#endif
	w = k->start(); /* tried again */
	CHECK(NULL != w);
	if (w) {
		CHECK(k->run_to_end(w, 1) || !k->run_to_end);
		k->stop(w);
	}
}

/* A request that is superseded: only the newer one is ever answered. */
static void an_older_answer_never_reaches_a_newer_request(void)
{
	void *w = start_hash();
	WfHashPort p;
	ULONGLONG end = GetTickCount64() + 10000;
	bool done = false;

	CHECK(NULL != w);
	if (!w) {
		return;
	}
	p = wf_hashworker_port(w);
	CHECK(WF_OK == p.start(p.ctx, 1, big_file)); /* a long one ... */
	CHECK(WF_OK ==
	        p.start(p.ctx, 2, small_file)); /* ... replaced at once */
	while (!done && GetTickCount64() < end) {
		WfHashUpdate u;

		WaitForSingleObject(wf_hashworker_event(w), 50);
		while (wf_hashworker_poll(w, &u)) {
			CHECK(2 == u.gen);
			done = done || u.finished;
		}
	}
	CHECK(done);
	stop_hash(w);
}

static void a_worker_that_was_never_started_is_stopped_as_nothing(void)
{
	WfWorkerStopReport a = wf_hashworker_stop(NULL),
	                   b = wf_sizeworker_stop(NULL),
	                   c = wf_lockworker_stop(NULL),
	                   d = wf_searchworker_stop(NULL);

	CHECK(WF_OK == a.status && !a.deferred);
	CHECK(WF_OK == b.status && !b.deferred);
	CHECK(WF_OK == c.status && !c.deferred);
	CHECK(WF_OK == d.status && !d.deferred);
}

int main(void)
{
	static const Kind kinds[] = {
	        {"hash", start_hash, ask_hash, stop_hash, idle_hash, run_hash,
	                post_hash, event_hash, drain_hash},
	        {"sizes", start_size, ask_size, stop_size, idle_size, run_size,
	                post_size, event_size, drain_size},
	        {"locks", start_lock, ask_lock, stop_lock, idle_lock, run_lock,
	                post_lock, event_lock, drain_lock},
	        {"search", start_search, NULL, stop_search, idle_search,
	                run_search, post_search, event_search, drain_search},
	        {"clipboard", start_clip, ask_clip, stop_clip, idle_clip,
	                run_clip, post_clip, event_clip, drain_clip},
	        {"launch", start_launch, ask_launch, stop_launch, idle_launch,
	                run_launch, post_launch, event_launch, drain_launch},
	        {"picture", start_image, ask_image, stop_image, idle_image,
	                run_image, post_image, event_image, drain_image},
	};

	/* Start the preview reader at the owned root, so unrelated profile
	 * ancestors cannot prevent the missing-host path from being exercised. */
	CHECK(p0_lab_open(&lab, "lifecycle", 1));
	if (!lab.area) {
		return TEST_RESULT();
	}
	make_fixtures();
	for (size_t i = 0; i < WF_ARRAY_LEN(kinds); i++) {
		a_worker_comes_and_goes_without_leaving_a_thread_or_a_handle(
		        &kinds[i]);
		a_worker_is_used_released_and_made_again(&kinds[i], 150);
		a_worker_with_work_or_an_answer_waiting_is_not_idle(&kinds[i]);
		a_worker_holding_an_answer_is_not_idle(&kinds[i]);
		a_start_that_fails_leaves_nothing_and_can_be_tried_again(
		        &kinds[i]);
	}
	an_older_answer_never_reaches_a_newer_request();
	a_worker_that_was_never_started_is_stopped_as_nothing();
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}

/* The fuzzy search worker on a real thread (DR-DRAFT-023): the answer is the
 * foreground's answer; of many quick requests only the newest is ever delivered;
 * release() returns only when the thread no longer reads the listing, so the
 * listing can be freed at once (run under ASan to see a read after free); the
 * time a release takes; bad requests are refused; stop with work pending. */
#include <stdlib.h>
#include <string.h>

#include "search/wf_fuzzyworker.h"
#include "wf_test.h"

#define NAMES 60000u

typedef struct
{
	WfDir *dir;
	size_t *vis;
} Listing;

static Listing make_listing(unsigned n)
{
	Listing l = {wf_dir_new(), malloc(n * sizeof(size_t))};

	CHECK(l.dir && l.vis);
	for (unsigned i = 0; i < n; i++) {
		wchar_t name[40];
		int len = swprintf_s(name, 40, L"file_%05u_config_%u.dat", i,
		        (i * 7919u) % 1000u);

		CHECK(len > 0 &&
		        WF_OK == wf_dir_add(l.dir, name, (size_t)len, 0, 0, 0));
		l.vis[i] = i;
	}
	return l;
}

static void free_listing(Listing *l)
{
	wf_dir_free(l->dir);
	free(l->vis);
	l->dir = NULL;
	l->vis = NULL;
}

static WfFzJob job_for(const Listing *l, size_t n, const wchar_t *q, size_t k)
{
	WfFzJob j = {l->dir->e, l->vis, n, {{0}, 0}, k};

	CHECK(wf_fz_query_set(&j.query, q, wcslen(q)));
	return j;
}

static bool wait_answer(WfFuzzyWorker *w, WfFzAnswer *a, DWORD ms)
{
	ULONGLONG end = GetTickCount64() + ms;

	while (GetTickCount64() < end) {
		if (WAIT_OBJECT_0 ==
		        WaitForSingleObject(wf_fuzzyworker_event(w), 20)) {
			if (wf_fuzzyworker_poll(w, a)) {
				return true;
			}
		}
	}
	return false;
}

static void the_answer_is_the_foregrounds(void)
{
	WfFuzzyWorker *w;
	Listing l = make_listing(NAMES);
	WfFuzzyPort port;
	WfFzJob job = job_for(&l, NAMES, L"cfg1", 64);
	WfFzAnswer a;
	WfFzTopK top;
	WfFzStats st;
	WfFzScratch *x = wf_fz_scratch_new();

	CHECK(WF_OK == wf_fuzzyworker_start(&w) && x);
	port = wf_fuzzyworker_port(w);
	CHECK(WF_OK == port.start(port.ctx, 7, &job));
	CHECK(wait_answer(w, &a, 10000));
	CHECK(7 == a.gen && WF_OK == a.st);
	CHECK(WF_OK == wf_fz_topk_init(&top, 64));
	CHECK(WF_OK ==
	        wf_fz_search(&job.query, l.dir->e, l.vis, NAMES, x, &top, NULL,
	                NULL, &st));
	CHECK(a.nhit == top.n && a.matched == st.matched && a.nhit > 0);
	for (size_t i = 0; i < a.nhit; i++) {
		CHECK(a.hit[i].index == top.hit[i].index &&
		        a.hit[i].score == top.hit[i].score);
	}
	wf_fz_topk_free(&top);
	wf_fz_answer_free(&a);
	/* nothing more is waiting */
	CHECK(!wf_fuzzyworker_poll(w, &a));
	wf_fz_scratch_free(x);
	wf_fuzzyworker_stop(w);
	free_listing(&l);
}

/* A request that fails inside the worker is answered too: the screen is waiting
 * for this very request, and a failure that is dropped leaves it waiting for
 * ever. A failed answer of a request that was replaced is still dropped. */
static void a_failure_of_the_newest_request_is_delivered_and_an_old_one_is_not(
        void)
{
	WfFuzzyWorker *w;
	Listing l = make_listing(NAMES);
	WfFuzzyPort port;
	WfFzJob bad = job_for(&l, NAMES, L"cfg1",
	        SIZE_MAX); /* k times the hit size overflows: WF_E_RANGE */
	WfFzJob good = job_for(&l, NAMES, L"cfg1", 16);
	WfFzAnswer a;

	CHECK(WF_OK == wf_fuzzyworker_start(&w));
	port = wf_fuzzyworker_port(w);
	CHECK(WF_OK == port.start(port.ctx, 11, &bad));
	CHECK(wait_answer(w, &a, 10000));
	CHECK(11 == a.gen && WF_OK != a.st && 0 == a.nhit && NULL == a.hit);
	wf_fz_answer_free(&a);
	/* a failure that was replaced by a newer request before it ended is not delivered */
	CHECK(WF_OK == port.start(port.ctx, 12, &bad));
	CHECK(WF_OK == port.start(port.ctx, 13, &good));
	CHECK(wait_answer(w, &a, 10000));
	CHECK(a.gen >=
	        12); /* never the answer of an earlier request than the failure's */
	if (12 == a.gen) {
		CHECK(WF_OK != a.st);
		wf_fz_answer_free(&a);
		CHECK(wait_answer(w, &a, 10000));
	}
	CHECK(13 == a.gen && WF_OK == a.st && a.nhit > 0);
	wf_fz_answer_free(&a);
	wf_fuzzyworker_stop(w);
	free_listing(&l);
}

static void of_many_quick_requests_only_the_newest_is_delivered(void)
{
	WfFuzzyWorker *w;
	Listing l = make_listing(NAMES);
	WfFuzzyPort port;
	WfFzAnswer a;
	static const wchar_t *const qs[] = {
	        L"c", L"co", L"con", L"conf", L"confi", L"config"};

	CHECK(WF_OK == wf_fuzzyworker_start(&w));
	port = wf_fuzzyworker_port(w);
	for (uint32_t g = 1; g <= 300; g++) {
		WfFzJob j = job_for(&l, NAMES, qs[g % 6], 64);

		CHECK(WF_OK == port.start(port.ctx, g, &j));
	}
	CHECK(wait_answer(w, &a, 20000));
	/* the answer that may be delivered is the last one asked for */
	CHECK(300 == a.gen);
	wf_fz_answer_free(&a);
	Sleep(100);
	CHECK(!wf_fuzzyworker_poll(w, &a)); /* none of the others came after */
	wf_fuzzyworker_stop(w);
	free_listing(&l);
}

static void release_lets_the_listing_be_freed_at_once(void)
{
	WfFuzzyWorker *w;
	WfFuzzyPort port;
	WfFzAnswer a;
	ULONGLONG worst = 0, total = 0;

	CHECK(WF_OK == wf_fuzzyworker_start(&w));
	port = wf_fuzzyworker_port(w);
	for (int round = 0; round < 40; round++) {
		Listing l = make_listing(NAMES);
		WfFzJob j = job_for(&l, NAMES, L"fcd", 64);
		LARGE_INTEGER t0, t1, fq;

		CHECK(WF_OK == port.start(port.ctx, (uint32_t)round + 1, &j));
		Sleep((DWORD)(round %
		        4)); /* sometimes not begun, sometimes mid-search */
		QueryPerformanceFrequency(&fq);
		QueryPerformanceCounter(&t0);
		port.release(port.ctx);
		QueryPerformanceCounter(&t1);
		{
			ULONGLONG us = (ULONGLONG)((t1.QuadPart - t0.QuadPart) *
			        1000000 / fq.QuadPart);

			total += us;
			if (us > worst) {
				worst = us;
			}
		}
		/* the thread must not read the listing now */
		free_listing(&l);
		/* ... and no answer of that request may appear */
		Sleep(2);
		CHECK(!wf_fuzzyworker_poll(w, &a));
	}
	printf("fuzzyworker: release took %llu us on average, %llu us at most "
	       "(%u names)\n",
	        total / 40, worst, NAMES);
	wf_fuzzyworker_stop(w);
}

static void bad_requests_are_refused(void)
{
	WfFuzzyWorker *w;
	Listing l = make_listing(10);
	WfFuzzyPort port;
	WfFzJob j = job_for(&l, 10, L"f", 4);

	CHECK(WF_OK == wf_fuzzyworker_start(&w));
	port = wf_fuzzyworker_port(w);
	CHECK(WF_OK != port.start(port.ctx, 0, &j)); /* generation 0 */
	CHECK(WF_OK != port.start(port.ctx, 1, NULL));
	j.k = 0;
	CHECK(WF_OK != port.start(port.ctx, 1, &j));
	j.k = 4;
	j.nvis = 0;
	CHECK(WF_OK != port.start(port.ctx, 1, &j));
	j.nvis = 10;
	j.query.n = 0;
	CHECK(WF_OK != port.start(port.ctx, 1, &j));
	/* release with nothing out is harmless */
	port.release(port.ctx);
	wf_fuzzyworker_stop(w);
	free_listing(&l);
}

static void stop_with_a_request_out_is_clean(void)
{
	WfFuzzyWorker *w;
	Listing l = make_listing(NAMES);
	WfFuzzyPort port;
	WfFzJob j = job_for(&l, NAMES, L"config", 64);
	WfWorkerStopReport r;

	CHECK(WF_OK == wf_fuzzyworker_start(&w));
	port = wf_fuzzyworker_port(w);
	CHECK(WF_OK == port.start(port.ctx, 1, &j));
	r = wf_fuzzyworker_stop(w);
	CHECK(!r.deferred);
	free_listing(&l);
}

int main(void)
{
	the_answer_is_the_foregrounds();
	a_failure_of_the_newest_request_is_delivered_and_an_old_one_is_not();
	of_many_quick_requests_only_the_newest_is_delivered();
	release_lets_the_listing_be_freed_at_once();
	bad_requests_are_refused();
	stop_with_a_request_out_is_clean();
	return TEST_RESULT();
}

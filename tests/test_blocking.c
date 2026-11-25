/*
 * wf_blocking_run: the work runs on a thread of its own, the caller is called
 * back while it waits, and a request to stop reaches the job through the token
 * bound to its thread (and only that job).
 */
#include <stdio.h>

#include "async/wf_blocking.h"
#include "wf_test.h"

typedef struct
{
	DWORD thread;
	int slept_ms;
	bool saw_stop;
	int stop_after_ms; /* 0: the job runs by itself to its end */
} Job;

static WfStatus job_fn(void *p)
{
	Job *j = p;

	j->thread = GetCurrentThreadId();
	for (int waited = 0; waited < j->slept_ms; waited += 10) {
		if (wf_cancel_requested()) {
			j->saw_stop = true;
			return WF_E_CANCELLED;
		}
		Sleep(10);
	}
	return WF_OK;
}

typedef struct
{
	int ticks;
	unsigned last_ms;
	unsigned stop_at_ms; /* 0: never ask */
} Watch;

static bool tick(void *ctx, unsigned elapsed_ms)
{
	Watch *w = ctx;

	w->ticks++;
	w->last_ms = elapsed_ms;
	return !(w->stop_at_ms && elapsed_ms >= w->stop_at_ms);
}

static void without_a_tick_the_job_runs_here(void)
{
	Job j = {0, 0, false, 0};

	CHECK(WF_OK == wf_blocking_run(job_fn, &j, NULL));
	CHECK(j.thread == GetCurrentThreadId());
}

static void with_a_tick_it_runs_on_another_thread_and_calls_back(void)
{
	Job j = {0, 400, false, 0};
	Watch w = {0, 0, 0};
	WfBlockingWait wait = {100, 40, tick, &w};

	CHECK(WF_OK == wf_blocking_run(job_fn, &j, &wait));
	CHECK(0 != j.thread && j.thread != GetCurrentThreadId());
	CHECK(w.ticks >= 3 && w.last_ms >= 100); /* none before first_ms */
	CHECK(!j.saw_stop);
}

static void asking_to_stop_reaches_the_job(void)
{
	Job j = {0, 5000, false, 0};
	Watch w = {0, 0, 150};
	WfBlockingWait wait = {50, 25, tick, &w};
	ULONGLONG t0 = GetTickCount64();

	CHECK(WF_E_CANCELLED == wf_blocking_run(job_fn, &j, &wait));
	CHECK(j.saw_stop);
	CHECK(GetTickCount64() - t0 < 3000); /* it did not run its 5 s */
}

static void no_thread_means_no_run(void)
{
	Job j = {0, 0, false, 0};
	Watch w = {0, 0, 0};
	WfBlockingWait wait = {50, 25, tick, &w};

	wf_blocking_fail_next_thread();
	CHECK(WF_E_NOMEM == wf_blocking_run(job_fn, &j, &wait));
	CHECK(0 == j.thread && 0 == w.ticks); /* the job did not run anywhere */
	CHECK(WF_OK == wf_blocking_run(job_fn, &j, &wait)); /* once only */
	CHECK(0 != j.thread && j.thread != GetCurrentThreadId());
}

static void a_thread_without_a_token_is_never_asked(void)
{
	WfCancel c = {0};

	wf_cancel_ask(&c);
	CHECK(!wf_cancel_requested());
	wf_cancel_bind(&c);
	CHECK(wf_cancel_requested());
	wf_cancel_bind(NULL);
	CHECK(!wf_cancel_requested());
}

int main(void)
{
	without_a_tick_the_job_runs_here();
	with_a_tick_it_runs_on_another_thread_and_calls_back();
	asking_to_stop_reaches_the_job();
	no_thread_means_no_run();
	a_thread_without_a_token_is_never_asked();
	return TEST_RESULT();
}

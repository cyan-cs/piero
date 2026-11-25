#include "wf_blocking.h"

static __declspec(thread) const WfCancel *g_bound;

void wf_cancel_ask(WfCancel *c)
{
	InterlockedExchange(&c->asked, 1);
}

bool wf_cancel_asked(const WfCancel *c)
{
	/* a plain read of an aligned LONG that another thread sets once; the
	 * volatile keeps the compiler from caching it */
	return 0 != c->asked;
}

void wf_cancel_bind(const WfCancel *c)
{
	g_bound = c;
}

bool wf_cancel_requested(void)
{
	return g_bound && wf_cancel_asked(g_bound);
}

typedef struct
{
	WfBlockingFn fn;
	void *arg;
	const WfCancel *cancel;
	WfStatus st;
} Run;

static DWORD WINAPI run_main(LPVOID p)
{
	Run *r = p;

	wf_cancel_bind(r->cancel);
	r->st = r->fn(r->arg);
	wf_cancel_bind(NULL);
	return 0;
}

static volatile LONG fail_next_thread;

void wf_blocking_fail_next_thread(void)
{
	InterlockedExchange(&fail_next_thread, 1);
}

WfStatus wf_blocking_run(WfBlockingFn fn, void *arg, const WfBlockingWait *wait)
{
	WfCancel cancel = {0};
	Run r = {fn, arg, &cancel, WF_E_OS};
	ULONGLONG start;
	HANDLE t;
	bool asked = false;

	if (!fn) {
		return WF_E_INVAL;
	}
	if (!wait || !wait->tick) {
		return fn(arg);
	}
	t = InterlockedExchange(&fail_next_thread, 0)
	        ? NULL
	        : CreateThread(NULL, 0, run_main, &r, 0, NULL);
	if (!t) {
		return WF_E_NOMEM; /* not started: on this thread it would hold the screen */
	}
	start = GetTickCount64();
	for (;;) {
		ULONGLONG gone;
		DWORD w = WaitForSingleObject(
		        t, wait->every_ms ? wait->every_ms : 50u);

		if (WAIT_OBJECT_0 == w) {
			break;
		}
		if (WAIT_TIMEOUT !=
		        w) { /* the wait itself failed: wait it out */
			WaitForSingleObject(t, INFINITE);
			break;
		}
		gone = GetTickCount64() - start;
		if (gone >= wait->first_ms &&
		        !wait->tick(wait->ctx, (unsigned)gone) && !asked) {
			asked = true;
			wf_cancel_ask(&cancel);
		}
	}
	CloseHandle(t);
	return r.st;
}

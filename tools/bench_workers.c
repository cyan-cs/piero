/*
 * What starting a worker costs a process that never uses it: private bytes,
 * commit, threads and handles after the worker has started and settled, for
 * each of the four on-demand features' workers (hash #, folder sizes D, locks
 * L, search / duplicates F) and the fuzzy-search worker beside them. Each is
 * started and stopped RUNS times; the median and the spread of each figure are
 * printed, with the time the stop of an idle worker takes (what releasing it
 * costs the UI thread). Working set is not printed as a result: the system moves it about
 * without the program's help.
 */
#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clipboard/wf_clipworker_win.h"
#include "fs/wf_candworker.h"
#include "fs/wf_launchworker.h"
#include "fs/wf_sizeworker.h"
#include "preview/wf_imageworker.h"
#include "process/wf_lockworker_win.h"
#include "search/wf_dupehash_win.h"
#include "search/wf_fuzzyworker.h"
#include "search/wf_searchworker.h"
#include "xfer/wf_hashworker.h"

#define RUNS 31

typedef struct
{
	double private_bytes, commit, threads, handles, ms;
} Sample;

static size_t thread_count(void)
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

static void now_counts(Sample *s)
{
	PROCESS_MEMORY_COUNTERS_EX m;
	DWORD h = 0;

	memset(&m, 0, sizeof m);
	m.cb = sizeof m;
	K32GetProcessMemoryInfo(
	        GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof m);
	GetProcessHandleCount(GetCurrentProcess(), &h);
	s->private_bytes = (double)m.PrivateUsage;
	s->commit = (double)m.PagefileUsage;
	s->threads = (double)thread_count();
	s->handles = (double)h;
}

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

static void report(
        const char *name, const double *v, size_t n, const char *unit)
{
	double s[RUNS];

	memcpy(s, v, n * sizeof *s);
	qsort(s, n, sizeof *s, cmp);
	printf("  %-22s median %10.1f %s  (min %.1f, max %.1f, %zu runs)\n",
	        name, s[n / 2], unit, s[0], s[n - 1], n);
}

typedef struct
{
	const char *name;
	void *(*start)(double *ms);
	void (*stop)(void *w);
} Kind;

static double qpc_ms(LARGE_INTEGER a, LARGE_INTEGER b)
{
	LARGE_INTEGER f;

	QueryPerformanceFrequency(&f);
	return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
}

#define STARTER(fn_name, type, start_call)                                     \
	static void *fn_name(double *ms)                                       \
	{                                                                      \
		type *w = NULL;                                                \
		LARGE_INTEGER a, b;                                            \
                                                                               \
		QueryPerformanceCounter(&a);                                   \
		if (WF_OK != (start_call)) {                                   \
			return NULL;                                           \
		}                                                              \
		QueryPerformanceCounter(&b);                                   \
		*ms = qpc_ms(a, b);                                            \
		return w;                                                      \
	}

STARTER(start_hash, WfHashWorker, wf_hashworker_start(&w))
STARTER(start_size, WfSizeWorker, wf_sizeworker_start(&w))
STARTER(start_lock, WfLockWorker, wf_lockworker_start(&w, NULL))
STARTER(start_fuzzy, WfFuzzyWorker, wf_fuzzyworker_start(&w))
STARTER(start_cand, WfCandWorker, wf_candworker_start(&w))
STARTER(start_clip, WfClipWorker, wf_clipworker_start(&w))
STARTER(start_launch, WfLaunchWorker, wf_launchworker_start(&w))

static void *start_image(double *ms)
{
	WfImageWorker *w = NULL;
	WfImageWorkerOptions o;
	LARGE_INTEGER a, b;

	memset(&o, 0, sizeof o);
	o.sandbox = 1;
	o.cache_bytes = (size_t)64 << 20;
	QueryPerformanceCounter(&a);
	if (WF_OK != wf_imageworker_start(&w, &o)) {
		return NULL;
	}
	QueryPerformanceCounter(&b);
	*ms = qpc_ms(a, b);
	return w;
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

static void *start_search(double *ms)
{
	WfSearchWorker *w = NULL;
	LARGE_INTEGER a, b;

	QueryPerformanceCounter(&a);
	if (WF_OK != wf_searchworker_start(&w, no_list)) {
		return NULL;
	}
	QueryPerformanceCounter(&b);
	*ms = qpc_ms(a, b);
	return w;
}

static void stop_hash(void *w)
{
	(void)wf_hashworker_stop(w);
}
static void stop_size(void *w)
{
	(void)wf_sizeworker_stop(w);
}
static void stop_lock(void *w)
{
	(void)wf_lockworker_stop(w);
}
static void stop_fuzzy(void *w)
{
	(void)wf_fuzzyworker_stop(w);
}
static void stop_cand(void *w)
{
	(void)wf_candworker_stop(w);
}
static void stop_clip(void *w)
{
	(void)wf_clipworker_stop(w);
}
static void stop_launch(void *w)
{
	(void)wf_launchworker_stop(w);
}
static void stop_image(void *w)
{
	(void)wf_imageworker_stop(w);
}
static void stop_search(void *w)
{
	(void)wf_searchworker_stop(w);
}

int main(void)
{
	static const Kind kinds[] = {
	        {"hash (#)", start_hash, stop_hash},
	        {"folder sizes (D)", start_size, stop_size},
	        {"locks (L)", start_lock, stop_lock},
	        {"search / dupes (F)", start_search, stop_search},
	        {"fuzzy search", start_fuzzy, stop_fuzzy},
	        {"smart go candidates", start_cand, stop_cand},
	        {"clipboard files (V)", start_clip, stop_clip},
	        {"open / terminal", start_launch, stop_launch},
	        {"picture decoding", start_image, stop_image},
	};

	printf("what starting an unused worker costs (idle, settled 100 ms)\n");
	for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
		double pb[RUNS], cm[RUNS], th[RUNS], hd[RUNS], ms[RUNS],
		        stops[RUNS];

		/* one start and stop first: the heap and the loader are warm */
		{
			double warm;
			void *w = kinds[k].start(&warm);

			if (w) {
				kinds[k].stop(w);
			}
			Sleep(100);
		}
		for (size_t i = 0; i < RUNS; i++) {
			Sample before, after;
			double took = 0;
			void *w;

			now_counts(&before);
			w = kinds[k].start(&took);
			if (!w) {
				printf("  %s: could not start\n",
				        kinds[k].name);
				return 1;
			}
			Sleep(100);
			now_counts(&after);
			pb[i] = after.private_bytes - before.private_bytes;
			cm[i] = after.commit - before.commit;
			th[i] = after.threads - before.threads;
			hd[i] = after.handles - before.handles;
			ms[i] = took;
			{
				LARGE_INTEGER a, b;

				QueryPerformanceCounter(&a);
				kinds[k].stop(
				        w); /* idle: what a release costs */
				QueryPerformanceCounter(&b);
				stops[i] = qpc_ms(a, b);
			}
			Sleep(20);
		}
		printf("%s\n", kinds[k].name);
		report("private bytes", pb, RUNS, "B");
		report("commit", cm, RUNS, "B");
		report("threads", th, RUNS, "");
		report("handles", hd, RUNS, "");
		report("start call", ms, RUNS, "ms");
		report("stop call (idle)", stops, RUNS, "ms");
	}
	return 0;
}

/* Deterministic service model: 3 ms IO, real coordinator and View work.
 * Virtual latency is not a Windows Terminal paint-time measurement. */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/wf_app_priv.h"

static WfApp app;
static bool pending;
static uint64_t ready_at;
static uint32_t ready_gen;
static wchar_t ready_path[256];
static unsigned requests, waits, cancelled;
static double cpu_samples[80], latency[80];

static double us(void)
{
	LARGE_INTEGER f, t;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1e6 / (double)f.QuadPart;
}
static WfFileId id(unsigned n)
{
	WfFileId r = {0};
	r.valid = true;
	r.volume = 1;
	memcpy(r.id, &n, sizeof n);
	return r;
}
static WfStatus submit(
        void *ctx, uint32_t gen, const wchar_t *path, const wchar_t *parent)
{
	(void)ctx;
	(void)parent;
	if (pending) {
		cancelled++;
	}
	requests++;
	pending = true;
	ready_gen = gen;
	ready_at = app.now_ms + 3;
	wcscpy_s(ready_path, 256, path);
	return WF_OK;
}
static void progress(uint64_t until)
{
	for (unsigned guard = 0; app.now_ms < until && guard < 10000; guard++) {
		uint64_t next = until;
		uint32_t wait = wf_app_dirprev_timeout(&app);
		if (wait != WF_WAIT_FOREVER && app.now_ms + wait < next) {
			next = app.now_ms + wait;
		}
		if (pending && ready_at < next) {
			next = ready_at;
		}
		app.now_ms = next;
		(void)wf_app_dirprev_tick(&app);
		if (pending && ready_at <= app.now_ms) {
			unsigned n = 0;
			const wchar_t *leaf = wcsrchr(ready_path, L'\\');
			if (!leaf || swscanf_s(leaf + 1, L"f%u", &n) != 1) {
				exit(2);
			}
			uint32_t gen = ready_gen;
			pending = false;
			WfDir *d = wf_dir_new();
			if (!d) {
				exit(2);
			}
			d->identity = id(n + 1);
			for (unsigned j = 0; j < 100; j++) {
				wchar_t name[32];
				swprintf(name, 32, L"file%u", j);
				if (wf_dir_add(d, name, wcslen(name), 0, 1,
				            0) != WF_OK) {
					exit(2);
				}
			}
			wf_app_on_dirpreview(&app, gen, WF_OK, d);
		}
	}
}
static int compare(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}
int main(void)
{
	app.cwd = _wcsdup(L"C:\\bench");
	app.now_ms = 100;
	app.dirprev_port = (WfDirPreviewPort){NULL, submit};
	WfDir *d = wf_dir_new();
	if (!app.cwd || !d) {
		return 2;
	}
	d->identity = id(1000);
	for (unsigned i = 0; i < 30; i++) {
		wchar_t name[16];
		swprintf(name, 16, L"f%02u", i);
		if (wf_dir_add(d, name, wcslen(name), WF_ENT_DIR, 0, 0) !=
		        WF_OK) {
			return 2;
		}
		d->e[i].identity = id(i + 1);
	}
	if (wf_view_set_dir(&app.cur, d, NULL) != WF_OK) {
		return 2;
	}
#ifdef WF_BENCH_DIRCACHE
	wf_app_dircache_enable(&app, true);
#endif
	wf_app_preview_sync(&app, false);
	uint64_t cold = app.now_ms;
	while (app.dirprev_state != WF_DIRPREV_READY &&
	        app.now_ms - cold < 1000) {
		progress(app.now_ms + 1);
	}
	printf("cold_virtual_ms,%llu\n", app.now_ms - cold);
	progress(app.now_ms + 40);
	for (unsigned i = 0; i < 58; i++) {
		app.cur.cursor = i < 29 ? i + 1 : 57 - i;
		uint64_t start = app.now_ms;
		double t = us();
		wf_app_preview_sync(&app, false);
		cpu_samples[i] = us() - t;
		if (app.dirprev_state != WF_DIRPREV_READY) {
			waits++;
		}
		while (app.dirprev_state != WF_DIRPREV_READY &&
		        app.now_ms - start < 1000) {
			progress(app.now_ms + 1);
		}
		latency[i] = (double)(app.now_ms - start);
		progress(start + 80);
	}
	qsort(latency, 58, sizeof *latency, compare);
	qsort(cpu_samples, 58, sizeof *cpu_samples, compare);
	printf("selection_virtual_ms,median=%.3f,p95=%.3f,max=%.3f\n",
	        latency[29], latency[55], latency[57]);
	printf("ui_select_cpu_us,median=%.3f,p95=%.3f,max=%.3f\n",
	        cpu_samples[29], cpu_samples[55], cpu_samples[57]);
	printf("wait_frames,%u,requests=%u,superseded=%u\n", waits, requests,
	        cancelled);
#ifdef WF_BENCH_DIRCACHE
	printf("cache,hits=%llu,misses=%llu,prefetched=%llu,bytes=%zu,peak=%"
	       "zu\n",
	        app.dircache->hits, app.dircache->misses,
	        app.dircache->prefetched, wf_app_dircache_bytes(&app),
	        app.dircache->peak_bytes);
#endif
	PROCESS_MEMORY_COUNTERS m = {0};
	m.cb = sizeof m;
	GetProcessMemoryInfo(GetCurrentProcess(), &m, sizeof m);
	printf("working_set,%zu,peak=%zu\n", (size_t)m.WorkingSetSize,
	        (size_t)m.PeakWorkingSetSize);
	wf_app_preview_clear(&app);
#ifdef WF_BENCH_DIRCACHE
	wf_app_dircache_destroy(&app);
#endif
	wf_view_free(&app.cur);
	wf_view_free(&app.dirprev);
	free(app.cwd);
	return 0;
}

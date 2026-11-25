/* Benchmark of the fuzzy search (DR-DRAFT-023). A developer tool: it prints
 * numbers and asserts nothing about speed. Names are made in memory with a
 * fixed seed (no disk). For each count and query: p50, p95 and the worst of
 * many runs of the whole search (prefilter, scoring, Top-K), how many names
 * passed the prefilter and how many matched.
 *
 *   bench_fuzzy.exe [runs]
 */
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "search/wf_fuzzy.h"
#include "search/wf_fuzzyworker.h"

#ifndef WF_BENCH_CONFIGURATION
#	define WF_BENCH_CONFIGURATION "?"
#endif

static uint32_t rng = 20261109u;

static uint32_t rnd(uint32_t n)
{
	rng = rng * 1664525u + 1013904223u;
	return (rng >> 8) % n;
}

static double now_us(void)
{
	LARGE_INTEGER c, f;

	QueryPerformanceCounter(&c);
	QueryPerformanceFrequency(&f);
	return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
}

/* Names that look like the real thing: words joined by _ - . or nothing, some
 * camelCase, numbers, an extension; a tenth are Japanese. */
static void make_name(wchar_t *out, size_t cap)
{
	static const wchar_t *const words[] = {L"config", L"file", L"main",
	        L"test", L"cache", L"generator", L"server", L"http", L"read",
	        L"me", L"index", L"build", L"output", L"report", L"image",
	        L"photo", L"backup", L"final", L"draft", L"notes", L"log",
	        L"data", L"user", L"admin", L"setup", L"install", L"driver",
	        L"module", L"parser", L"render", L"buffer", L"stream"};
	static const wchar_t *const jp[] = {L"設定", L"ファイル", L"報告書",
	        L"写真", L"最終版", L"メモ", L"資料", L"会議"};
	static const wchar_t *const ext[] = {L".c", L".h", L".txt", L".md",
	        L".json", L".png", L".go", L".dat", L".log", L".cpp"};
	static const wchar_t sep[] = {L'_', L'-', L'.', L' ', 0, 0};
	size_t n = 0;
	uint32_t parts = 1 + rnd(4);

	out[0] = L'\0';
	for (uint32_t p = 0; p < parts; p++) {
		const wchar_t *w = rnd(10) == 0 ? jp[rnd(8)] : words[rnd(32)];
		wchar_t sp = sep[rnd(6)];

		if (p && sp) {
			out[n++] = sp;
		}
		for (size_t i = 0; w[i] && n + 8 < cap; i++) {
			wchar_t c = w[i];

			if (0 == i && p && 0 == sp && c >= L'a' && c <= L'z') {
				c = (wchar_t)(c - 32); /* camelCase */
			}
			out[n++] = c;
		}
		if (rnd(3) == 0) {
			n += (size_t)swprintf_s(
			        out + n, cap - n, L"%u", rnd(1000));
		}
	}
	wcscpy_s(out + n, cap - n, ext[rnd(10)]);
}

typedef struct
{
	WfDir *dir;
	size_t *vis;
	size_t n;
} Corpus;

static Corpus corpus_make(size_t n)
{
	Corpus c = {wf_dir_new(), malloc(n * sizeof(size_t)), n};

	for (size_t i = 0; i < n; i++) {
		wchar_t name[96];

		make_name(name, 96);
		if (WF_OK != wf_dir_add(c.dir, name, wcslen(name), 0, 0, 0)) {
			fprintf(stderr, "out of memory\n");
			exit(2);
		}
		c.vis[i] = i;
	}
	return c;
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

typedef struct
{
	double p50, p95, max, mean;
	size_t passed, matched;
} Result;

static Result run_query(const Corpus *c, const wchar_t *qs, size_t k, int runs)
{
	WfFzQuery q;
	WfFzScratch *x = wf_fz_scratch_new();
	WfFzTopK top;
	WfFzStats st = {0};
	double *t = malloc((size_t)runs * sizeof *t), sum = 0;
	Result r = {0};

	wf_fz_query_set(&q, qs, wcslen(qs));
	wf_fz_topk_init(&top, k);
	for (int i = 0; i < runs + 3; i++) {
		double t0 = now_us(), dt;

		top.n = 0;
		wf_fz_search(
		        &q, c->dir->e, c->vis, c->n, x, &top, NULL, NULL, &st);
		dt = now_us() - t0;
		if (i >= 3) { /* the first runs warm the caches */
			t[i - 3] = dt;
			sum += dt;
		}
	}
	qsort(t, (size_t)runs, sizeof *t, cmp_d);
	r.p50 = t[runs / 2];
	r.p95 = t[(size_t)((double)runs * 0.95)];
	r.max = t[runs - 1];
	r.mean = sum / runs;
	r.passed = st.passed;
	r.matched = st.matched;
	wf_fz_topk_free(&top);
	wf_fz_scratch_free(x);
	free(t);
	return r;
}

/* The alternative that Top-K replaces: score everything that matches, sort all. */
static double run_fullsort(const Corpus *c, const wchar_t *qs, int runs)
{
	WfFzQuery q;
	WfFzScratch *x = wf_fz_scratch_new();
	WfFzHit *all = malloc(c->n * sizeof *all);
	double best = 1e18;

	wf_fz_query_set(&q, qs, wcslen(qs));
	for (int r = 0; r < runs; r++) {
		double t0 = now_us(), dt;
		size_t m = 0;

		for (size_t i = 0; i < c->n; i++) {
			const wchar_t *nm = c->dir->e[c->vis[i]].name;
			int32_t s = wf_fz_score(&q, nm, wcslen(nm), x);

			if (WF_FZ_NO_MATCH != s) {
				all[m++] = (WfFzHit){s, (uint32_t)i};
			}
		}
		/* sort by the same order */
		{
			WfFzTopK t;

			wf_fz_topk_init(&t, m ? m : 1);
			for (size_t i = 0; i < m; i++) {
				wf_fz_topk_offer(&t, all[i]);
			}
			wf_fz_topk_finish(&t);
			wf_fz_topk_free(&t);
		}
		dt = now_us() - t0;
		if (dt < best) {
			best = dt;
		}
	}
	free(all);
	wf_fz_scratch_free(x);
	return best;
}

static double cpu_ms(void)
{
	FILETIME c, e, k, u;

	GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
	return (double)(((ULARGE_INTEGER){.LowPart = k.dwLowDateTime,
	                         .HighPart = k.dwHighDateTime}
	                                .QuadPart +
	                        ((ULARGE_INTEGER){.LowPart = u.dwLowDateTime,
	                                .HighPart = u.dwHighDateTime}
	                                        .QuadPart)) /
	        10000);
}

static SIZE_T private_bytes(void)
{
	PROCESS_MEMORY_COUNTERS_EX m;

	memset(&m, 0, sizeof m);
	m.cb = sizeof m;

	GetProcessMemoryInfo(
	        GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof m);
	return m.PrivateUsage;
}

int main(int argc, char **argv)
{
	static const size_t counts[] = {1000, 10000, 50000, 100000};
	static const struct
	{
		const wchar_t *q;
		const char *what;
	} queries[] = {{L"cfg", "selective 3"}, {L"ma", "2 letters"},
	        {L"e", "1 letter, nearly all pass"},
	        {L"ae", "2 letters, nearly all pass"}, {L"conf", "prefix word"},
	        {L"srvrlg", "6 letters sparse"},
	        {L"gen_cache_file_x", "long 16, rarely"}, {L"zqx", "no match"},
	        {L"設定", "Japanese"}, {L"gS", "camel"}};
	int runs = argc > 1 ? atoi(argv[1]) : 60;

	printf("fuzzy search benchmark: %s build, K=64, %d runs per cell, "
	       "microseconds\n\n",
	        WF_BENCH_CONFIGURATION, runs);
	printf("%-8s %-28s %9s %9s %9s %9s %8s %8s\n", "names", "query", "p50",
	        "p95", "max", "mean", "passed", "matched");
	for (size_t ci = 0; ci < sizeof counts / sizeof *counts; ci++) {
		Corpus c = corpus_make(counts[ci]);

		for (size_t qi = 0; qi < sizeof queries / sizeof *queries;
		        qi++) {
			Result r = run_query(&c, queries[qi].q, 64, runs);
			char q[64];

			WideCharToMultiByte(CP_UTF8, 0, queries[qi].q, -1, q,
			        sizeof q, NULL, NULL);
			printf("%-8zu %-28s %9.0f %9.0f %9.0f %9.0f %8zu "
			       "%8zu\n",
			        counts[ci], q, r.p50, r.p95, r.max, r.mean,
			        r.passed, r.matched);
		}
		wf_dir_free(c.dir);
		free(c.vis);
		printf("\n");
	}

	/* Top-K against scoring and sorting every match */
	{
		Corpus c = corpus_make(100000);
		Result a = run_query(&c, L"ae", 64, 15);
		double b = run_fullsort(&c, L"ae", 15);

		printf("Top-K (heap, K=64) against sorting every match, 100000 "
		       "names, query \"ae\" (%zu match): %.0f us against %.0f "
		       "us "
		       "(best of 15)\n\n",
		        a.matched, a.p50, b);
		/* memory and CPU across many searches */
		{
			WfFzQuery q;
			WfFzScratch *x = wf_fz_scratch_new();
			WfFzTopK top;
			SIZE_T before, after;
			double cpu0, t0;
			const int reps = 300;

			wf_fz_query_set(&q, L"ae", 2);
			wf_fz_topk_init(&top, 64);
			wf_fz_search(&q, c.dir->e, c.vis, c.n, x, &top, NULL,
			        NULL, NULL);
			before = private_bytes();
			cpu0 = cpu_ms();
			t0 = now_us();
			for (int i = 0; i < reps; i++) {
				top.n = 0;
				wf_fz_search(&q, c.dir->e, c.vis, c.n, x, &top,
				        NULL, NULL, NULL);
			}
			after = private_bytes();
			printf("%d searches of 100000 names: private bytes "
			       "%+lld "
			       "(%zu before), CPU %.0f ms of %.0f ms wall, the "
			       "search itself allocates nothing\n\n",
			        reps, (long long)after - (long long)before,
			        (size_t)before, cpu_ms() - cpu0,
			        (now_us() - t0) / 1000);
			wf_fz_topk_free(&top);
			wf_fz_scratch_free(x);
		}
		/* positions (the highlight) of one result */
		{
			WfFzQuery q;
			WfFzScratch *x = wf_fz_scratch_new();
			uint32_t u[WF_FZ_QUERY_MAX];
			double t0 = now_us();
			const int reps = 20000;

			size_t found = 0, at = 0;

			/* names that do match, so that the whole matrix is built */
			wf_fz_query_set(&q, L"ae", 2);
			for (int i = 0; i < reps; i++) {
				const wchar_t *nm = NULL;

				while (at < c.n) {
					nm = c.dir->e[at++].name;
					if (wf_fz_prefilter(
					            &q, nm, wcslen(nm))) {
						break;
					}
					nm = NULL;
				}
				if (!nm) {
					at = 0;
					continue;
				}
				found++;
				wf_fz_positions(&q, nm, wcslen(nm), x, u, NULL);
			}
			printf("positions of a result (full matrix, %zu "
			       "names): "
			       "%.2f us each (60 rows on screen: %.2f ms)\n\n",
			        found, (now_us() - t0) / (double)found,
			        (now_us() - t0) / (double)found * 60 / 1000);
			wf_fz_scratch_free(x);
		}
		/* the worker: the round trip of a tiny request, and the cancel */
		{
			WfFuzzyWorker *w;
			WfFuzzyPort port;
			WfFzJob j;
			double sum = 0, worst = 0;
			const int reps = 500;

			wf_fuzzyworker_start(&w);
			port = wf_fuzzyworker_port(w);
			j.e = c.dir->e;
			j.vis = c.vis;
			j.nvis =
			        50; /* a tiny search: the round trip is the cost */
			j.k = 64;
			wf_fz_query_set(&j.query, L"ma", 2);
			for (int i = 0; i < reps; i++) {
				WfFzAnswer ans;
				double t = now_us(), dt;

				port.start(port.ctx, (uint32_t)i + 1, &j);
				WaitForSingleObject(
				        wf_fuzzyworker_event(w), INFINITE);
				wf_fuzzyworker_poll(w, &ans);
				wf_fz_answer_free(&ans);
				dt = now_us() - t;
				sum += dt;
				if (dt > worst) {
					worst = dt;
				}
			}
			printf("worker round trip of a 50-name request (start, "
			       "answer polled): %.0f us on average, %.0f us at "
			       "most\n",
			        sum / reps, worst);
			/* cancel a search of 100000 names that passes everywhere */
			j.nvis = c.n;
			wf_fz_query_set(&j.query, L"ae", 2);
			sum = worst = 0;
			for (int i = 0; i < 100; i++) {
				double t;

				port.start(port.ctx, 1000 + (uint32_t)i, &j);
				Sleep((DWORD)(i % 7));
				t = now_us();
				port.release(port.ctx);
				t = now_us() - t;
				sum += t;
				if (t > worst) {
					worst = t;
				}
			}
			printf("release() of a running 100000-name search: "
			       "%.0f us "
			       "on average, %.0f us at most (100 tries)\n",
			        sum / 100, worst);
			wf_fuzzyworker_stop(w);
		}
		wf_dir_free(c.dir);
		free(c.vis);
	}
	/* the foreground cost of small folders, to place the threshold */
	{
		static const size_t sizes[] = {500, 1000, 2000, 4096, 8000};

		printf("\nforeground search time of small folders (query "
		       "\"ae\", "
		       "nearly all pass):\n");
		for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
			Corpus c = corpus_make(sizes[i]);
			Result r = run_query(&c, L"ae", 64, 200);

			printf("  %5zu names: p50 %.0f us, p95 %.0f us\n",
			        sizes[i], r.p50, r.p95);
			wf_dir_free(c.dir);
			free(c.vis);
		}
	}
	return 0;
}

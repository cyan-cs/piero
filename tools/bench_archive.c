/*
 * Measures what browsing an archive costs. Not a test: it prints a table.
 *
 *     bench_archive.exe <folder with the archives of tools/make_archive_bench.py>
 *
 * For every archive: the first open (the whole archive read, the index built),
 * then the same folder listed from the cached index many times (the cost of a
 * cursor move or of going into a folder, microseconds, with p50/p95/p99), and
 * the first bytes of one file in the middle of the archive.
 */
#include <windows.h>

#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archive/wf_archive.h"

static double freq_ms;

static double now_ms(void)
{
	LARGE_INTEGER t;

	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / freq_ms;
}

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

static void percentiles(
        double *v, size_t n, double *p50, double *p95, double *p99)
{
	qsort(v, n, sizeof *v, cmp);
	*p50 = v[n / 2];
	*p95 = v[n * 95 / 100];
	*p99 = v[n * 99 / 100];
}

static size_t working_set_kib(void)
{
	PROCESS_MEMORY_COUNTERS pmc;

	GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
	return pmc.WorkingSetSize / 1024;
}

static void one(const wchar_t *folder,
        const wchar_t *name,
        const wchar_t *inside,
        const wchar_t *file)
{
	enum
	{
		N = 5000
	};
	wchar_t path[1100], v[2200];
	static double lap[N];
	WfArchive *a = NULL;
	WfDir *d;
	WfArchiveData data;
	double t0, first, p50, p95, p99, readms;
	size_t i, before, after;
	WfStatus st;

	swprintf(path, 1100, L"%ls\\%ls", folder, name);
	if (INVALID_FILE_ATTRIBUTES == GetFileAttributesW(path)) {
		wprintf(L"%-16ls missing\n", name);
		return;
	}
	wf_archive_cache_clear();
	before = working_set_kib();
	t0 = now_ms();
	st = wf_archive_open(path, NULL, &a);
	first = now_ms() - t0;
	if (WF_OK != st) {
		wprintf(L"%-16ls open failed (%d)\n", name, (int)st);
		return;
	}
	after = working_set_kib();
	{
		WfArchiveInfo info = wf_archive_info(a);

		wprintf(L"%-16ls first open %9.1f ms  entries %6zu  index %6zu "
		        L"KiB  (+%zu KiB working set)\n",
		        name, first, info.entries, info.bytes / 1024,
		        after > before ? after - before : 0);
	}
	for (i = 0; i < N; i++) {
		t0 = now_ms();
		st = wf_archive_list(a, inside, 0, &d);
		lap[i] = (now_ms() - t0) * 1000.0;
		if (WF_OK == st) {
			wf_dir_free(d);
		}
	}
	percentiles(lap, N, &p50, &p95, &p99);
	wprintf(L"%-16ls list \"%ls\": p50 %7.1f us  p95 %7.1f us  p99 %7.1f "
	        L"us\n",
	        L"", *inside ? inside : L"(top)", p50, p95, p99);
	for (i = 0; i < N; i++) {
		WfArchive *again = NULL;

		t0 = now_ms();
		st = wf_archive_open(path, NULL, &again);
		lap[i] = (now_ms() - t0) * 1000.0;
		wf_archive_release(again);
	}
	percentiles(lap, N, &p50, &p95, &p99);
	wprintf(L"%-16ls re-open (cache hit, file re-checked): p50 %6.1f us  "
	        L"p95 %6.1f us  p99 %6.1f us\n",
	        L"", p50, p95, p99);
	wf_archive_release(a);
	swprintf(v, 2200, L"%ls|\\%ls", path, file);
	t0 = now_ms();
	st = wf_vpath_read(v, 256u * 1024u, NULL, &data);
	readms = now_ms() - t0;
	wprintf(L"%-16ls first 256 KiB of \"%ls\": %hs %7.2f ms (%zu bytes)\n",
	        L"", file, WF_OK == st ? "ok" : "FAILED", readms,
	        WF_OK == st ? data.n : 0);
	wf_archive_data_free(&data);
}

int main(int argc, char **argv)
{
	wchar_t folder[1024];
	LARGE_INTEGER f;

	if (argc < 2) {
		puts("usage: bench_archive <folder>");
		return 2;
	}
	QueryPerformanceFrequency(&f);
	freq_ms = (double)f.QuadPart;
	MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, folder, 1024);
	wf_archive_set_codepage(932);
	for (wchar_t *c = folder; *c; c++) {
		if (47 == *c) {
			*c = 92;
		}
	}
	wprintf(L"working set at start: %zu KiB\n\n", working_set_kib());
	one(folder, L"many.zip", L"dir100", L"dir100\\file050.txt");
	one(folder, L"many.7z", L"dir100", L"dir100\\file050.txt");
	one(folder, L"many.tar.gz", L"dir100", L"dir100\\file050.txt");
	one(folder, L"many.tar.xz", L"dir100", L"dir100\\file050.txt");
	one(folder, L"many.tar.zst", L"dir100", L"dir100\\file050.txt");
	one(folder, L"big.tar.gz", L"", L"big.txt");
	one(folder, L"big.tar.zst", L"", L"big.txt");
	wprintf(L"\nworking set at end: %zu KiB\n", working_set_kib());
	return 0;
}

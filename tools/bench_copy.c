/* Opt-in Copy throughput through the real engine (no mocks), into a fresh
 * owned fixture; verification policy as the product's: SHA-256 read-back, and
 * none for comparison. Reports MiB/s per run and the median, and the bytes
 * the independent hash of source and copy agree on. Nothing is deleted: the
 * fixture is retained like the other probes. Disk and AV decide the numbers;
 * there is no threshold. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "p0/p0_lab.h"
#include "p0/p0_common.h"
#include "xfer/wf_probe_win.h"
#include "xfer/wf_copyengine_win.h"

static bool write_source(P0Lab *lab, const wchar_t *relative, unsigned mib)
{
	wchar_t path[1024];
	unsigned char *block = malloc(1u << 20);
	HANDLE h;
	bool ok;

	if (!block || !p0_lab_path(lab, relative, path, 1024)) {
		free(block);
		return false;
	}
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		free(block);
		return false;
	}
	ok = p0_area_track(lab->area, relative, 0) != 0;
	for (unsigned m = 0; ok && m < mib; m++) {
		DWORD put = 0;

		for (size_t i = 0; i < (1u << 20); i++) {
			block[i] = (unsigned char)(i * 7 + (i >> 9) + m * 31);
		}
		ok = WriteFile(h, block, 1u << 20, &put, NULL) &&
		        put == 1u << 20;
	}
	ok = ok && FlushFileBuffers(h);
	CloseHandle(h);
	free(block);
	return ok;
}

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

static double now_s(void)
{
	LARGE_INTEGER t, f;

	QueryPerformanceCounter(&t);
	QueryPerformanceFrequency(&f);
	return (double)t.QuadPart / (double)f.QuadPart;
}

static bool run(P0Lab *lab,
        WfProbeOps *ops,
        WfCopyJournal *journal,
        unsigned run_index,
        WfCopyVerify verify,
        unsigned mib,
        double *mib_per_s)
{
	wchar_t rel[64], dest[1024], source[1024];
	const wchar_t *names[] = {L"big.bin"};
	WfCopySpec spec = {0};
	WfPreflightReport report = {0};
	WfCopyResult result = {0};
	WfNodeInfo node;
	WfStatus st;
	double t0, t1;
	bool ok = false;

	swprintf(rel, 64, L"out%u_%d", run_index, (int)verify);
	if (!p0_area_mkdir(lab->area, rel) ||
	        !p0_lab_path(lab, rel, dest, 1024) ||
	        !p0_lab_path(lab, L"source", source, 1024)) {
		return false;
	}
	st = wf_xfer_capture(
	        ops, WF_XFER_COPY, source, names, 1, 0, &spec.slot);
	if (WF_OK == st) {
		st = wf_preflight_run(ops, &spec.slot, dest,
		        &WF_PREFLIGHT_DEFAULT, NULL, &report);
	}
	if (WF_OK != st || !wf_preflight_can_start(st, &report, 0) ||
	        WF_OK != ops->node(NULL, dest, &node)) {
		wf_copyspec_free(&spec);
		return false;
	}
	spec.dest = _wcsdup(dest);
	spec.dest_id = node.id;
	spec.kind = WF_CJ_COPY;
	spec.collide = WF_COLLIDE_STOP;
	spec.verify = verify;
	spec.total_files = report.tree_files;
	spec.total_bytes = report.total_bytes;
	t0 = now_s();
	st = wf_copyengine_run(&spec, NULL, journal, &result);
	t1 = now_s();
	if (WF_OK == st && WF_CS_DONE == result.state &&
	        !result.failure_total) {
		wchar_t a[1100], b[1100];
		char ha[P0_HASH_HEX], hb[P0_HASH_HEX];

		swprintf(a, 1100, L"%ls\\big.bin", source);
		swprintf(b, 1100, L"%ls\\big.bin", dest);
		ok = p0_sha256_file(a, ha) && p0_sha256_file(b, hb) &&
		        !strcmp(ha, hb);
		*mib_per_s = (double)mib / (t1 - t0);
	}
	wf_copyresult_free(&result);
	wf_copyspec_free(&spec);
	return ok;
}

int wmain(int argc, wchar_t **argv)
{
	unsigned mib = 256, runs = 5;
	P0Lab lab = {0};
	WfProbeOps ops;
	WfCopyJournal *journal;
	wchar_t jpath[1024];
	const struct
	{
		WfCopyVerify method;
		const char *name;
	} modes[] = {{WF_VERIFY_NONE, "verify_none"},
	        {WF_VERIFY_SHA256, "verify_sha256"}};

	for (int i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--mib") && i + 1 < argc) {
			mib = (unsigned)_wtoi(argv[++i]);
		} else if (!wcscmp(argv[i], L"--runs") && i + 1 < argc) {
			runs = (unsigned)_wtoi(argv[++i]);
		} else {
			fputs("usage: bench_copy [--mib N] [--runs N]\n",
			        stderr);
			return 2;
		}
	}
	if (!mib || mib > 4096 || !runs || runs > 20) {
		return 2;
	}
	if (!p0_lab_open(&lab, "bench-copy", 0)) {
		printf("fixture_error=%lu\n", lab.error);
		return 2;
	}
	if (!p0_area_mkdir(lab.area, L"source") ||
	        !p0_area_mkdir(lab.area, L"receipts") ||
	        !write_source(&lab, L"source\\big.bin", mib) ||
	        !p0_lab_path(&lab, L"receipts\\journal.txt", jpath, 1024)) {
		p0_lab_close(&lab, 1);
		fputs("fixture_setup=FAIL\n", stderr);
		return 2;
	}
	ops = wf_probe_win();
	journal = wf_cj_open(jpath);
	if (!journal) {
		p0_lab_close(&lab, 1);
		return 2;
	}
	printf("size_mib=%u runs=%u\n", mib, runs);
	for (size_t m = 0; m < 2; m++) {
		double v[20];
		unsigned good = 0;

		for (unsigned r = 0; r < runs; r++) {
			double x = 0;

			if (run(&lab, &ops, journal, r, modes[m].method, mib,
			            &x)) {
				v[good++] = x;
				printf("%s run=%u mib_per_s=%.1f "
				       "content=identical\n",
				        modes[m].name, r, x);
			} else {
				printf("%s run=%u FAILED\n", modes[m].name, r);
			}
		}
		if (good) {
			qsort(v, good, sizeof *v, cmp);
			printf("%s median_mib_per_s=%.1f min=%.1f max=%.1f "
			       "ok=%u/%u\n",
			        modes[m].name, v[good / 2], v[0], v[good - 1],
			        good, runs);
		}
	}
	wf_cj_close(journal);
	p0_lab_close(&lab, 1); /* never sweep fixtures or recorded targets */
	puts("fixture_retained=1");
	return 0;
}

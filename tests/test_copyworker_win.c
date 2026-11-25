/*
 * The copy worker: jobs run in order off the calling thread, can be paused,
 * resumed and cancelled, every accepted job yields exactly one result, and
 * stopping cancels cleanly. Real files in a scratch area.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "wf_test.h"
#include "xfer/wf_copyworker.h"
#include "xfer/wf_probe_win.h"
#include "base/wf_macro.h"

static P0Lab lab;
static wchar_t root[1024], src_dir[1024], dst_dir[1024], journal[1100];
static WfProbeOps ops;

static bool put(const wchar_t *path, size_t n)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	char *buf = malloc(n ? n : 1);
	DWORD w = 0;
	size_t i;
	bool ok;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	for (i = 0; i < n; i++) {
		buf[i] = (char)(i * 7 + (i >> 9));
	}
	ok = WriteFile(h, buf, (DWORD)n, &w, NULL) && w == n;
	CloseHandle(h);
	free(buf);
	return ok;
}

static bool exists(const wchar_t *p)
{
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(p);
}

static size_t temporaries(void)
{
	wchar_t pattern[1100];
	WIN32_FIND_DATAW fd;
	HANDLE f;
	size_t n = 0;

	swprintf(pattern, 1100, L"%ls\\.wfcopy-*.tmp", dst_dir);
	f = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE == f) {
		return 0;
	}
	do {
		n++;
	} while (FindNextFileW(f, &fd));
	FindClose(f);
	return n;
}

static WfCopySpec job_for(const wchar_t *name)
{
	const wchar_t *names[1];
	WfCopySpec s;
	WfNodeInfo d;

	names[0] = name;
	memset(&s, 0, sizeof s);
	wf_slot_init(&s.slot);
	CHECK(wf_xfer_capture(&ops, WF_XFER_COPY, src_dir, names, 1, 0,
	              &s.slot) == WF_OK);
	s.dest = _wcsdup(dst_dir);
	CHECK(WF_OK == ops.node(NULL, dst_dir, &d));
	s.dest_id = d.id;
	s.kind = WF_CJ_COPY;
	s.verify = WF_VERIFY_SHA256;
	s.collide = WF_COLLIDE_STOP;
	s.total_files = 1;
	s.total_bytes = s.slot.src[0].snap.size;
	return s;
}

static bool wait_for(WfCopyWorker *w, bool (*pred)(WfCopyWorker *), unsigned ms)
{
	ULONGLONG until = GetTickCount64() + ms;

	while (GetTickCount64() < until) {
		if (pred(w)) {
			return true;
		}
		Sleep(10);
	}
	return pred(w);
}

static bool is_paused_with_a_job(WfCopyWorker *w)
{
	WfCopyProgress p;

	return wf_copyworker_progress(w, &p) && WF_CS_PAUSED == p.state;
}

static bool is_idle(WfCopyWorker *w)
{
	WfCopyProgress p;

	return !wf_copyworker_progress(w, &p);
}

/* Waits for `n` results and returns them in order of arrival. */
static size_t collect(WfCopyWorker *w, WfCopyResult *out, size_t n, unsigned ms)
{
	size_t got = 0;
	ULONGLONG until = GetTickCount64() + ms;

	while (got < n && GetTickCount64() < until) {
		if (wf_copyworker_poll(w, &out[got])) {
			got++;
		} else {
			WaitForSingleObject(wf_copyworker_event(w), 50);
		}
	}
	return got;
}

static void jobs_run_in_order_and_report_progress(void)
{
	WfCopyWorker *w;
	WfCopySpec a, b;
	WfCopyResult r[2];
	wchar_t p[1100];
	uint32_t ja, jb;

	CHECK(WF_OK == wf_copyworker_start(&w, journal));
	swprintf(p, 1100, L"%ls\\one.bin", src_dir);
	CHECK(put(p, 2u << 20));
	swprintf(p, 1100, L"%ls\\two.bin", src_dir);
	CHECK(put(p, 1000));
	a = job_for(L"one.bin");
	b = job_for(L"two.bin");
	CHECK(WF_OK == wf_copyworker_submit(w, &a, &ja));
	CHECK(WF_OK == wf_copyworker_submit(w, &b, &jb));
	CHECK(ja != jb);
	CHECK(2 == collect(w, r, 2, 20000));
	CHECK(r[0].job == ja && r[1].job == jb);
	CHECK(WF_CS_DONE == r[0].state && 1 == r[0].files_copied &&
	        WF_CS_DONE == r[1].state);
	swprintf(p, 1100, L"%ls\\one.bin", dst_dir);
	CHECK(exists(p));
	swprintf(p, 1100, L"%ls\\two.bin", dst_dir);
	CHECK(exists(p));
	CHECK(is_idle(w) && 0 == temporaries());
	wf_copyresult_free(&r[0]);
	wf_copyresult_free(&r[1]);
	wf_copyspec_free(&a);
	wf_copyspec_free(&b);
	CHECK(WF_OK == wf_copyworker_stop(w).status);
}

static void pause_holds_the_job_and_resume_finishes_it(void)
{
	WfCopyWorker *w;
	WfCopySpec a;
	WfCopyResult r;
	WfCopyProgress p;
	wchar_t d[1100];

	CHECK(WF_OK == wf_copyworker_start(&w, journal));
	swprintf(d, 1100, L"%ls\\one.bin", dst_dir);
	{
		wchar_t x[1100];

		swprintf(x, 1100, L"%ls\\one.bin", dst_dir);
		DeleteFileW(x);
	}
	wf_copyworker_set_paused(w, true);
	a = job_for(L"one.bin");
	CHECK(WF_OK == wf_copyworker_submit(w, &a, NULL));
	CHECK(wait_for(w, is_paused_with_a_job, 5000));
	Sleep(300);
	CHECK(wf_copyworker_progress(w, &p) && WF_CS_PAUSED == p.state &&
	        0 == p.bytes_done);
	CHECK(!exists(d)); /* nothing is given a name while paused */
	CHECK(wf_copyworker_paused(w));
	wf_copyworker_set_paused(w, false);
	CHECK(1 == collect(w, &r, 1, 20000) && WF_CS_DONE == r.state &&
	        1 == r.files_copied);
	CHECK(exists(d) && 0 == temporaries());
	wf_copyresult_free(&r);
	wf_copyspec_free(&a);
	CHECK(WF_OK == wf_copyworker_stop(w).status);
}

static void cancel_ends_the_running_job_and_drops_the_queue(void)
{
	WfCopyWorker *w;
	WfCopySpec a, b;
	WfCopyResult r[2];
	wchar_t d[1100];

	CHECK(WF_OK == wf_copyworker_start(&w, journal));
	swprintf(d, 1100, L"%ls\\one.bin", dst_dir);
	DeleteFileW(d);
	swprintf(d, 1100, L"%ls\\two.bin", dst_dir);
	DeleteFileW(d);
	wf_copyworker_set_paused(w, true);
	a = job_for(L"one.bin");
	b = job_for(L"two.bin");
	CHECK(WF_OK == wf_copyworker_submit(w, &a, NULL));
	CHECK(WF_OK == wf_copyworker_submit(w, &b, NULL));
	CHECK(wait_for(w, is_paused_with_a_job, 5000));
	wf_copyworker_cancel(w, true);
	CHECK(2 == collect(w, r, 2, 10000));
	CHECK(WF_CS_CANCELLED == r[0].state && WF_CS_CANCELLED == r[1].state);
	CHECK(!exists(d)); /* two.bin never started */
	swprintf(d, 1100, L"%ls\\one.bin", dst_dir);
	CHECK(!exists(d) && 0 == temporaries());
	CHECK(is_idle(w));
	wf_copyresult_free(&r[0]);
	wf_copyresult_free(&r[1]);
	wf_copyspec_free(&a);
	wf_copyspec_free(&b);
	CHECK(WF_OK == wf_copyworker_stop(w).status);
}

static void the_queue_is_bounded(void)
{
	WfCopyWorker *w;
	WfCopySpec a;
	WfCopyResult r;
	size_t accepted = 0, i, results = 0;

	CHECK(WF_OK == wf_copyworker_start(&w, journal));
	wf_copyworker_set_paused(w, true);
	a = job_for(L"two.bin");
	for (i = 0; i < WF_COPY_QUEUE_MAX + 4; i++) {
		WfStatus st = wf_copyworker_submit(w, &a, NULL);

		if (WF_OK == st) {
			accepted++;
		} else {
			CHECK(WF_E_RANGE == st);
		}
		if (0 == i) {
			CHECK(wait_for(w, is_paused_with_a_job, 5000));
		}
	}
	CHECK(accepted ==
	        WF_COPY_QUEUE_MAX + 1); /* one running, the queue full */
	wf_copyworker_cancel(w, true);
	while (results < accepted && 1 == collect(w, &r, 1, 10000)) {
		CHECK(WF_CS_CANCELLED == r.state);
		wf_copyresult_free(&r);
		results++;
	}
	CHECK(results == accepted); /* exactly one result per accepted job */
	wf_copyspec_free(&a);
	CHECK(WF_OK == wf_copyworker_stop(w).status);
}

static void cancel_all_never_drops_unread_results(void)
{
	WfCopyWorker *w;
	WfCopySpec a;
	WfCopyResult r;
	size_t accepted = 0, results = 0, round, i;

	CHECK(WF_OK == wf_copyworker_start(&w, journal));
	a = job_for(L"two.bin");
	/* two rounds of cancelled jobs, none read in between: more results than
	 * the store holds, so the second round has to wait for room */
	for (round = 0; round < 2; round++) {
		wf_copyworker_set_paused(w, true);
		for (i = 0; i < WF_COPY_QUEUE_MAX + 1; i++) {
			if (WF_OK == wf_copyworker_submit(w, &a, NULL)) {
				accepted++;
			}
			if (0 == i) {
				CHECK(wait_for(w, is_paused_with_a_job, 5000));
			}
		}
		wf_copyworker_cancel(w, true);
		Sleep(300);
	}
	CHECK(accepted > 16);
	while (results < accepted && 1 == collect(w, &r, 1, 10000)) {
		CHECK(WF_CS_CANCELLED == r.state);
		wf_copyresult_free(&r);
		results++;
	}
	CHECK(results == accepted); /* none of them was thrown away */
	wf_copyspec_free(&a);
	CHECK(WF_OK == wf_copyworker_stop(w).status);
}

static void stopping_in_the_middle_cleans_up(void)
{
	WfCopyWorker *w;
	WfCopySpec a;
	wchar_t d[1100];

	CHECK(WF_OK == wf_copyworker_start(&w, journal));
	swprintf(d, 1100, L"%ls\\one.bin", dst_dir);
	DeleteFileW(d);
	wf_copyworker_set_paused(w, true);
	a = job_for(L"one.bin");
	CHECK(WF_OK == wf_copyworker_submit(w, &a, NULL));
	CHECK(wait_for(w, is_paused_with_a_job, 5000));
	CHECK(temporaries() <= 1);
	CHECK(WF_OK ==
	        wf_copyworker_stop(w).status); /* cancels, cleans, joins */
	CHECK(!exists(d) && 0 == temporaries());
	wf_copyspec_free(&a);
}

int main(void)
{
	ops = wf_probe_win();
	CHECK(p0_lab_open(&lab, "copyworker", 0));
	if (!lab.area) {
		return TEST_RESULT();
	}
	wcscpy_s(root, 1024, lab.root);
	while (wcslen(root) > 3 && L'\\' == root[wcslen(root) - 1]) {
		root[wcslen(root) - 1] = 0;
	}
	CHECK(p0_area_mkdir(lab.area, L"s"));
	CHECK(p0_area_mkdir(lab.area, L"d"));
	swprintf(src_dir, 1024, L"%ls\\s", root);
	swprintf(dst_dir, 1024, L"%ls\\d", root);
	swprintf(journal, 1100, L"%ls\\journal.txt", root);
	jobs_run_in_order_and_report_progress();
	pause_holds_the_job_and_resume_finishes_it();
	cancel_ends_the_running_job_and_drops_the_queue();
	the_queue_is_bounded();
	cancel_all_never_drops_unread_results();
	stopping_in_the_middle_cleans_up();
	/* the fixture's own cleanup */
	{
		static const wchar_t *const files[] = {L"s\\one.bin",
		        L"s\\two.bin", L"d\\one.bin", L"d\\two.bin",
		        L"journal.txt"};
		size_t i;

		for (i = 0; i < WF_ARRAY_LEN(files); i++) {
			wchar_t path[1100];

			swprintf(path, 1100, L"%ls\\%ls", root, files[i]);
			SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
			DeleteFileW(path);
		}
	}
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}

/* Integration test for the FS worker thread and the directory watcher. */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "fs/wf_fsworker.h"
#include "fs/wf_watch_win.h"
#include "wf_test.h"
#include "base/wf_macro.h"

static int wait_result(WfFsWorker *w, WfFsResult *r, DWORD ms)
{
	if (WAIT_OBJECT_0 != WaitForSingleObject(wf_fsworker_event(w), ms)) {
		return 0;
	}
	return wf_fsworker_poll(w, r);
}

static void make_file(const wchar_t *dir, const wchar_t *name)
{
	wchar_t p[MAX_PATH * 2];
	HANDLE h;

	swprintf(p, MAX_PATH * 2, L"%ls\\%ls", dir, name);
	h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
}

static void test_worker(const wchar_t *dir)
{
	WfFsWorker *w = NULL;
	WfFsPort port;
	WfFsResult r;
	wchar_t missing[MAX_PATH * 2];
	int i, got_new = 0;

	CHECK(WF_OK == wf_fsworker_start(&w));
	port = wf_fsworker_port(w);

	/* no result before any request */
	CHECK(!wf_fsworker_poll(w, &r));
	CHECK(WAIT_TIMEOUT == WaitForSingleObject(wf_fsworker_event(w), 0));

	/* a normal request round-trips, with the parent listing alongside */
	CHECK(WF_OK == port.submit(port.ctx, 7, dir, L"C:\\"));
	CHECK(wait_result(w, &r, 5000));
	CHECK(7 == r.gen && WF_OK == r.st && r.dir && 2 == r.dir->n);
	CHECK(NULL != r.parent);
	wf_dir_free(r.dir);
	wf_dir_free(r.parent);
	CHECK(WAIT_TIMEOUT ==
	        WaitForSingleObject(
	                wf_fsworker_event(w), 0)); /* reset when drained */

	/* errors are reported, with no dirs attached */
	swprintf(missing, MAX_PATH * 2, L"%ls\\nope", dir);
	CHECK(WF_OK == port.submit(port.ctx, 8, missing, NULL));
	CHECK(wait_result(w, &r, 5000));
	CHECK(8 == r.gen && WF_E_NOENT == r.st && !r.dir && !r.parent);

	/* a burst of requests: the newest is always answered; older ones may
	 * or may not be, but nothing leaks or crashes */
	for (i = 100; i < 200; i++) {
		CHECK(WF_OK == port.submit(port.ctx, (uint32_t)i, dir, NULL));
	}
	while (wait_result(w, &r, 2000)) {
		got_new |= r.gen == 199;
		wf_dir_free(r.dir);
		wf_dir_free(r.parent);
		if (got_new) {
			break;
		}
	}
	CHECK(got_new);

	/* stop with a request still queued must not leak or hang */
	CHECK(WF_OK == port.submit(port.ctx, 300, dir, NULL));
	{
		WfWorkerStopReport report = wf_fsworker_stop(w);
		CHECK(WF_OK == report.status && !report.deferred);
		if (report.completion) {
			CloseHandle(report.completion);
		}
	}
	CHECK(WF_OK == wf_fsworker_stop(NULL).status);
}

static void test_watch(const wchar_t *dir)
{
	WfWatch *w = NULL;

	CHECK(WF_OK == wf_watch_open(dir, &w));
	CHECK(WAIT_TIMEOUT == WaitForSingleObject(wf_watch_event(w), 100));

	make_file(dir, L"created.txt");
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(wf_watch_event(w), 5000));
	CHECK(1 == wf_watch_consume(w));

	/* it re-armed itself: a second change is seen without reopening */
	Sleep(50);
	make_file(dir, L"created2.txt");
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(wf_watch_event(w), 5000));
	CHECK(1 == wf_watch_consume(w));

	/* a call with nothing pending must not block */
	(void)wf_watch_consume(w);
	wf_watch_close(
	        w); /* closing with a request outstanding must be clean */
	wf_watch_close(NULL);

	CHECK(WF_OK != wf_watch_open(L"C:\\definitely\\not\\here", &w));
	CHECK(WF_E_INVAL == wf_watch_open(L"relative", &w));
}

int main(void)
{
	wchar_t tmp[MAX_PATH], dir[MAX_PATH * 2], p[MAX_PATH * 2];

	GetTempPathW(MAX_PATH, tmp);
	swprintf(dir, MAX_PATH * 2, L"%lswfile-async-%lu", tmp,
	        GetCurrentProcessId());
	CHECK(CreateDirectoryW(dir, NULL));
	make_file(dir, L"a.txt");
	make_file(dir, L"b.txt");

	test_worker(dir);
	test_watch(dir);

	{
		const wchar_t *names[] = {
		        L"a.txt", L"b.txt", L"created.txt", L"created2.txt"};
		size_t i;

		for (i = 0; i < WF_ARRAY_LEN(names); i++) {
			swprintf(p, MAX_PATH * 2, L"%ls\\%ls", dir, names[i]);
			DeleteFileW(p);
		}
		RemoveDirectoryW(dir);
	}
	return TEST_RESULT();
}

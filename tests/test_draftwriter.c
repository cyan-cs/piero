/* Worker ordering is controlled by events; all draft I/O uses real files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "base/wf_macro.h"
#include "edit/wf_edraftw_win.h"
#include "wf_draft_write_probe.h"
#include "wf_test.h"
#undef wf_draft_write
#undef SleepConditionVariableSRW

static const WfEncoding UTF8 = {WF_ENC_UTF8, 0};
static HANDLE write_entered, write_release, settle_waiting;
static volatile LONG write_calls, settle_thread_id;
static bool block_first_write;

WfStatus wf_test_draft_write(
        const wchar_t *path, WfEncoding enc, const char *text, size_t n)
{
	LONG call = InterlockedIncrement(&write_calls);

	if (block_first_write && call == 1) {
		SetEvent(write_entered);
		CHECK(WAIT_OBJECT_0 ==
		        WaitForSingleObject(write_release, 10000));
	}
	return wf_draft_write(path, enc, text, n);
}

BOOL WINAPI wf_test_draft_wait(
        PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD timeout, ULONG flags)
{
	if (GetCurrentThreadId() ==
	        (DWORD)InterlockedCompareExchange(&settle_thread_id, 0, 0)) {
		/* The settle thread has dropped queued text and reached its wait
		 * with the writer lock held. The active write can now be released. */
		SetEvent(settle_waiting);
	}
	return SleepConditionVariableSRW(cv, lock, timeout, flags);
}

static WfDraftWriter *start_writer(bool block)
{
	WfDraftWriter *w = NULL;

	ResetEvent(write_entered);
	ResetEvent(write_release);
	ResetEvent(settle_waiting);
	InterlockedExchange(&write_calls, 0);
	InterlockedExchange(&settle_thread_id, 0);
	block_first_write = block;
	CHECK(WF_OK == wf_draftw_start(&w));
	return w;
}

static void stop_writer(WfDraftWriter *w)
{
	WfWorkerStopReport r = wf_draftw_stop(w, 5000);
	bool joined = WF_OK == r.status && !r.deferred;

	CHECK(joined);
	if (!joined) {
		exit(1);
	}
}

static bool draft_is(const wchar_t *path, const char *want)
{
	WfEncoding enc;
	char *text = NULL;
	size_t n = 0;
	bool same = WF_OK == wf_draft_read(path, &enc, &text, &n) &&
	        n == strlen(want) && !memcmp(text, want, n);

	free(text);
	return same;
}

typedef struct
{
	WfDraftWriter *writer;
	const wchar_t *path;
	HANDLE done;
	WfStatus status;
} Call;

static DWORD WINAPI submit_text(void *ctx)
{
	Call *call = ctx;

	call->status =
	        wf_draftw_submit(call->writer, call->path, UTF8, "one", 3);
	SetEvent(call->done);
	return 0;
}

static DWORD WINAPI drop_and_settle(void *ctx)
{
	Call *call = ctx;

	InterlockedExchange(&settle_thread_id, (LONG)GetCurrentThreadId());
	wf_draftw_settle(call->writer, call->path, true);
	SetEvent(call->done);
	return 0;
}

static void join_call(HANDLE thread, HANDLE done)
{
	DWORD result = WaitForSingleObject(thread, 5000);

	CHECK(WAIT_OBJECT_0 == result);
	/* A helper borrows a stack Call. Do not return while it still uses it. */
	if (WAIT_OBJECT_0 != result) {
		exit(1);
	}
	CloseHandle(thread);
	CloseHandle(done);
}

static void submission_does_not_wait_for_disk(void)
{
	const wchar_t *path = L"C:\\Users\\me\\slow.txt";
	WfDraftWriter *w = start_writer(true);
	Call call = {w, path, CreateEventW(NULL, TRUE, FALSE, NULL), WF_E_OS};
	HANDLE thread;

	if (!w || !call.done) {
		CHECK_FAIL("cannot start submission test");
		if (call.done) {
			CloseHandle(call.done);
		}
		if (w) {
			stop_writer(w);
		}
		return;
	}
	thread = CreateThread(NULL, 0, submit_text, &call, 0, NULL);
	if (!thread) {
		CHECK_FAIL("cannot start submission helper");
		CloseHandle(call.done);
		stop_writer(w);
		return;
	}
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(write_entered, 5000));
	/* The write remains blocked until after submission has returned. */
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(call.done, 5000));
	SetEvent(write_release);
	join_call(thread, call.done);
	CHECK(WF_OK == call.status);
	wf_draftw_settle(w, path, false);
	CHECK(draft_is(path, "one"));
	stop_writer(w);
	wf_draft_remove(path);
}

static void queued_text_is_replaced(void)
{
	const wchar_t *path = L"C:\\Users\\me\\latest.txt";
	WfDraftWriter *w = start_writer(true);

	if (!w) {
		return;
	}
	CHECK(WF_OK == wf_draftw_submit(w, path, UTF8, "first", 5));
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(write_entered, 5000));
	CHECK(WF_OK == wf_draftw_submit(w, path, UTF8, "second", 6));
	CHECK(WF_OK == wf_draftw_submit(w, path, UTF8, "third", 5));
	SetEvent(write_release);
	wf_draftw_settle(w, path, false);
	CHECK(draft_is(path, "third"));
	stop_writer(w);
	CHECK(2 == InterlockedCompareExchange(&write_calls, 0, 0));
	wf_draft_remove(path);
}

static void queue_identity_preserves_case(void)
{
	const wchar_t *big = L"C:\\Users\\me\\Foo.txt";
	const wchar_t *small = L"C:\\Users\\me\\foo.txt";
	WfDraftWriter *w = start_writer(true);

	if (!w) {
		return;
	}
	CHECK(WF_OK == wf_draftw_submit(w, big, UTF8, "under way", 9));
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(write_entered, 5000));
	CHECK(WF_OK == wf_draftw_submit(w, big, UTF8, "Foo", 3));
	CHECK(WF_OK == wf_draftw_submit(w, small, UTF8, "foo", 3));
	SetEvent(write_release);
	stop_writer(w);
	CHECK(3 == InterlockedCompareExchange(&write_calls, 0, 0));
	wf_draft_remove(big);
	wf_draft_remove(small);
}

static void removal_waits_and_drops_queued_text(void)
{
	const wchar_t *path = L"C:\\Users\\me\\gone.txt";
	WfDraftWriter *w = start_writer(true);
	Call call = {w, path, CreateEventW(NULL, TRUE, FALSE, NULL), WF_OK};
	HANDLE thread;
	WfEncoding enc;
	char *text = NULL;
	size_t n = 0;

	if (!w || !call.done) {
		CHECK_FAIL("cannot start settle test");
		if (call.done) {
			CloseHandle(call.done);
		}
		if (w) {
			stop_writer(w);
		}
		return;
	}
	CHECK(WF_OK == wf_draftw_submit(w, path, UTF8, "under way", 9));
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(write_entered, 5000));
	CHECK(WF_OK == wf_draftw_submit(w, path, UTF8, "waiting", 7));
	thread = CreateThread(NULL, 0, drop_and_settle, &call, 0, NULL);
	if (!thread) {
		CHECK_FAIL("cannot start settle helper");
		SetEvent(write_release);
		CloseHandle(call.done);
		stop_writer(w);
		wf_draft_remove(path);
		return;
	}
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(settle_waiting, 5000));
	CHECK(WAIT_TIMEOUT == WaitForSingleObject(call.done, 0));
	SetEvent(write_release);
	join_call(thread, call.done);
	wf_draft_remove(path);
	stop_writer(w);
	CHECK(1 == InterlockedCompareExchange(&write_calls, 0, 0));
	CHECK(WF_E_NOENT == wf_draft_read(path, &enc, &text, &n));
	free(text);
}

static void failure_is_reported_once(void)
{
	const wchar_t *refused = L"C:\\Users\\me\\.ssh\\config";
	const wchar_t *fine = L"C:\\Users\\me\\fine.txt";
	WfDraftWriter *w = start_writer(false);

	if (!w) {
		return;
	}
	CHECK(!wf_draftw_take_failure(w, refused));
	CHECK(WF_OK == wf_draftw_submit(w, refused, UTF8, "x", 1));
	wf_draftw_settle(w, refused, false);
	CHECK(wf_draftw_take_failure(w, refused));
	CHECK(!wf_draftw_take_failure(w, refused));
	CHECK(WF_OK == wf_draftw_submit(w, fine, UTF8, "ok", 2));
	wf_draftw_settle(w, fine, false);
	CHECK(!wf_draftw_take_failure(w, fine));
	stop_writer(w);
	wf_draft_remove(fine);
}

static void stop_flushes_pending_text(void)
{
	const wchar_t *a = L"C:\\Users\\me\\end-a.txt";
	const wchar_t *b = L"C:\\Users\\me\\end-b.txt";
	WfDraftWriter *w = start_writer(false);

	if (!w) {
		return;
	}
	CHECK(WF_OK == wf_draftw_submit(w, a, UTF8, "A", 1));
	CHECK(WF_OK == wf_draftw_submit(w, b, UTF8, "B", 1));
	stop_writer(w);
	CHECK(draft_is(a, "A") && draft_is(b, "B"));
	wf_draft_remove(a);
	wf_draft_remove(b);
}

int main(void)
{
	wchar_t temp[MAX_PATH], dir[MAX_PATH + 80];
	DWORD length = GetTempPathW(WF_ARRAY_LEN(temp), temp);
	int written;

	if (!length || length >= WF_ARRAY_LEN(temp)) {
		CHECK_FAIL("cannot locate temporary directory");
		return TEST_RESULT();
	}
	written = swprintf(dir, WF_ARRAY_LEN(dir), L"%lswf-draftw-%lu-%llu",
	        temp, (unsigned long)GetCurrentProcessId(),
	        (unsigned long long)GetTickCount64());
	if (written <= 0 || (size_t)written >= WF_ARRAY_LEN(dir) ||
	        !CreateDirectoryW(dir, NULL)) {
		CHECK_FAIL("cannot create an owned draft fixture");
		return TEST_RESULT();
	}
	write_entered = CreateEventW(NULL, TRUE, FALSE, NULL);
	write_release = CreateEventW(NULL, TRUE, FALSE, NULL);
	settle_waiting = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!write_entered || !write_release || !settle_waiting) {
		CHECK_FAIL("cannot create synchronization events");
	} else {
		wf_draft_set_dir(dir);
		submission_does_not_wait_for_disk();
		queued_text_is_replaced();
		queue_identity_preserves_case();
		removal_waits_and_drops_queued_text();
		failure_is_reported_once();
		stop_flushes_pending_text();
		wf_draft_set_dir(NULL);
	}
	if (write_entered) {
		CloseHandle(write_entered);
	}
	if (write_release) {
		CloseHandle(write_release);
	}
	if (settle_waiting) {
		CloseHandle(settle_waiting);
	}
	CHECK(RemoveDirectoryW(dir));
	return TEST_RESULT();
}

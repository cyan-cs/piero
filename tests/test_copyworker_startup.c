/* Deterministic readiness test: no sleeps and no user journal is touched. */
#include <windows.h>
#include <stdbool.h>
#include <string.h>
#include "../tools/p0/p0_lab.h"
#include "xfer/wf_copyworker.h"
#include "wf_test.h"

static const wchar_t *expected_path;
static unsigned creations;
static bool header_ready;

static HANDLE WINAPI checked_thread(LPSECURITY_ATTRIBUTES attributes,
        SIZE_T stack,
        LPTHREAD_START_ROUTINE start,
        LPVOID argument,
        DWORD flags,
        LPDWORD id)
{
	WfCopyJournalInfo info;
	creations++;
	header_ready = wf_cj_inspect(expected_path, &info) &&
	        info.format == WF_CJ_RECEIPTS;
	return CreateThread(attributes, stack, start, argument, flags, id);
}
#define CreateThread checked_thread
#include "../src/xfer/wf_copyworker.c"
#include "../src/async/wf_workcore.c" /* its CreateThread is the one checked */
#undef CreateThread

int main(void)
{
	P0Lab lab = {0};
	wchar_t path[1024];
	const char header[] = "PIERO-COPY-RECEIPTS 2\n";
	CHECK(p0_lab_open(&lab, "copy-worker-start", 0));
	if (!lab.area) {
		return TEST_RESULT();
	}
	CHECK(p0_lab_path(&lab, L"journal", path, 1024));
	expected_path = path;
	HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_NEW, 0, NULL);
	CHECK(file != INVALID_HANDLE_VALUE);
	if (file == INVALID_HANDLE_VALUE) {
		p0_lab_close(&lab, 1);
		return TEST_RESULT();
	}
	DWORD written = 0;
	CHECK(WriteFile(file, header, sizeof header - 1, &written, NULL) &&
	        written == sizeof header - 1);
	OVERLAPPED lock = {0};
	CHECK(LockFileEx(file,
	        LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
	        MAXDWORD, 0, &lock));
	WfCopyWorker *worker = NULL;
	WfStatus status = wf_copyworker_start(&worker, path);
	CHECK(status != WF_OK && worker == NULL && creations == 0);
	if (worker) {
		wf_copyworker_stop(worker);
		worker = NULL;
	}
	CHECK(UnlockFileEx(file, 0, MAXDWORD, 0, &lock));
	CloseHandle(file);
	creations = 0;
	CHECK(wf_copyworker_start(&worker, path) == WF_OK);
	CHECK(worker && creations == 1 && header_ready);
	if (worker) {
		wf_copyworker_stop(worker);
	}
	/* Failed startup and a successful idle worker do not alter prior receipts. */
	file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	CHECK(file != INVALID_HANDLE_VALUE);
	if (file != INVALID_HANDLE_VALUE) {
		char bytes[sizeof header] = {0};
		DWORD got = 0;
		CHECK(ReadFile(file, bytes, sizeof bytes, &got, NULL));
		CHECK(got == sizeof header - 1 && !memcmp(bytes, header, got));
		CloseHandle(file);
	}
	CHECK(p0_lab_close(&lab, 1) == 0);
	return TEST_RESULT();
}

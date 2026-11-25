/* Real startup/checkpoint/shutdown code and session files, with fake navigation.
 * A checkpoint must not replace damaged, unreadable or unsupported saved data. */
#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "main_loop.h"
#include "base/wf_macro.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

static wchar_t state_dir[MAX_PATH + 64];

static void slot_path(const wchar_t *name, wchar_t *path, size_t cap)
{
	CHECK(swprintf(path, cap, L"%ls\\%ls", state_dir, name) > 0);
}

static void put_slot(const wchar_t *name, const char *text)
{
	wchar_t path[MAX_PATH + 80];
	DWORD written = 0;

	slot_path(name, path, WF_ARRAY_LEN(path));
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(file != INVALID_HANDLE_VALUE);
	if (file != INVALID_HANDLE_VALUE) {
		CHECK(WriteFile(
		        file, text, (DWORD)strlen(text), &written, NULL));
		CHECK(written == strlen(text));
		CloseHandle(file);
	}
}

static void expect_slot(const wchar_t *name, const char *text)
{
	wchar_t path[MAX_PATH + 80];
	char bytes[256] = {0};
	DWORD got = 0;
	LARGE_INTEGER size = {0};

	slot_path(name, path, WF_ARRAY_LEN(path));
	HANDLE file = CreateFileW(path, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(file != INVALID_HANDLE_VALUE);
	if (file != INVALID_HANDLE_VALUE) {
		CHECK(GetFileSizeEx(file, &size));
		CHECK(size.QuadPart == (LONGLONG)strlen(text));
		CHECK(ReadFile(file, bytes, sizeof bytes - 1, &got, NULL));
		CHECK(got == strlen(text) && !strcmp(bytes, text));
		CloseHandle(file);
	}
}

static void checkpoint(bool continue_session, bool blocked_header)
{
	Loop *lp = calloc(1, sizeof *lp);
	WfConfig config;
	WfSession capture;
	wchar_t path[MAX_PATH + 80];
	HANDLE blocker = INVALID_HANDLE_VALUE;

	CHECK(lp != NULL);
	if (!lp) {
		return;
	}
	wf_config_init(&config);
	config.session_persist = 1;
	lp->config = &config;
	FakePort fs = {.app = &lp->app};
	WfStatus status = wf_app_init(
	        &lp->app, 120, 30, fake_port(&fs), L"C:\\Users\\dev");
	CHECK(WF_OK == status);
	if (WF_OK != status) {
		free(lp);
		return;
	}
	/* Prove that the test reaches saving, rather than skipping capture. */
	CHECK(wf_app_session_capture(&lp->app, &capture));
	wf_session_free(&capture);
	if (blocked_header) {
		slot_path(L"session.a", path, WF_ARRAY_LEN(path));
		blocker = CreateFileW(path, GENERIC_READ, 0, NULL,
		        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		CHECK(blocker != INVALID_HANDLE_VALUE);
	}
	wf_main_session_start(lp, state_dir, continue_session);
	CHECK(lp->session_writer != NULL);
	if (blocker != INVALID_HANDLE_VALUE) {
		CloseHandle(blocker);
	}
	lp->session_due = 1;
	wf_main_session_tick(lp);
	wf_main_session_stop(lp);
	CHECK(!lp->session_writer && !lp->session_store);
	wf_app_destroy(&lp->app);
	free(lp);
}

static void saved_data_survives_startup_and_checkpoints(void)
{
	static const struct
	{
		const char *a, *b;
		bool blocked_header;
	} cases[] = {
	        {"piero-session 1\nseq 1\nsize 99\n--\ncut", "bad header\n",
	                false},
	        {"piero-session 1\nseq 1\nsize 11\n--\nentry only\n", "",
	                false},
	        {"piero-session 9\nunsupported data\n", "", false},
	        {"piero-session 9999999999999999\n", "", false},
	        {"unreadable header\n", "", true},
	};

	for (size_t i = 0; i < WF_ARRAY_LEN(cases); i++) {
		for (int restore = 0; restore < 2; restore++) {
			put_slot(L"session.a", cases[i].a);
			put_slot(L"session.b", cases[i].b);
			checkpoint(restore != 0, cases[i].blocked_header);
			expect_slot(L"session.a", cases[i].a);
			expect_slot(L"session.b", cases[i].b);
		}
	}
}

static void forgetting_allows_new_checkpoints(void)
{
	WfSessionStore *store;
	WfSession saved;

	CHECK(WF_OK == wf_session_store_open(state_dir, &store));
	CHECK(wf_session_store_lock(store));
	CHECK(WF_OK == wf_session_store_forget(store));
	wf_session_store_close(store);
	checkpoint(false, false);
	CHECK(WF_OK == wf_session_store_open(state_dir, &store));
	CHECK(WF_SESSION_FOUND == wf_session_store_read(store, &saved, NULL));
	CHECK(saved.dir && !wcscmp(saved.dir, L"C:\\Users\\dev"));
	wf_session_free(&saved);
	wf_session_store_close(store);
}

int main(void)
{
	wchar_t temp[MAX_PATH], path[MAX_PATH + 80];
	static const wchar_t *const names[] = {
	        L"session.a", L"session.b", L"session.lock"};

	DWORD units = GetTempPathW(WF_ARRAY_LEN(temp), temp);
	if (!units || units >= WF_ARRAY_LEN(temp)) {
		CHECK_FAIL("temporary path is unavailable or too long");
		return TEST_RESULT();
	}
	CHECK(swprintf(state_dir, WF_ARRAY_LEN(state_dir),
	              L"%lspiero-main-session-%lu-%llu", temp,
	              GetCurrentProcessId(),
	              (unsigned long long)GetTickCount64()) > 0);
	if (!CreateDirectoryW(state_dir, NULL)) {
		CHECK_FAIL("could not create the session fixture");
		return TEST_RESULT();
	}
	saved_data_survives_startup_and_checkpoints();
	forgetting_allows_new_checkpoints();
	for (size_t i = 0; i < WF_ARRAY_LEN(names); i++) {
		slot_path(names[i], path, WF_ARRAY_LEN(path));
		CHECK(DeleteFileW(path));
	}
	CHECK(RemoveDirectoryW(state_dir));
	return TEST_RESULT();
}

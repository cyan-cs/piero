/* Development runtime files and loader only. This is not AppContainer proof. */
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tools/p0/p0_lab.h"
#include "preview/wf_sanitizer_stage_win.h"
#include "preview/wf_dechost_client_win.h"
#include "wf_test.h"

static bool put(const wchar_t *path, const char *text)
{
	HANDLE h =
	        CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	DWORD n = (DWORD)strlen(text), written = 0;
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}
	bool ok = WriteFile(h, text, n, &written, NULL) && written == n;
	CloseHandle(h);
	return ok;
}

static void files(P0Lab *lab)
{
	wchar_t directory[1024], manifest[1200], sentinel[1024];
	DWORD error = 0;
	CHECK(p0_lab_path(lab, L"bundle", directory, 1024));
	CHECK(CreateDirectoryW(directory, NULL));
	CHECK(wf_sanitizer_stage(directory, &error));
	CHECK(wf_sanitizer_stage(directory, &error));
	swprintf(manifest, 1200,
	        L"%ls\\piero.asan.runtime\\piero.asan.runtime.manifest",
	        directory);
	CHECK(p0_lab_path(lab, L"sentinel", sentinel, 1024));
	CHECK(put(sentinel, "KEEP"));
	CHECK(DeleteFileW(
	        manifest)); /* Only this test's freshly generated fixture. */
	CHECK(CreateHardLinkW(manifest, sentinel, NULL));
	CHECK(!wf_sanitizer_stage(directory, &error));
	HANDLE h = CreateFileW(sentinel, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	char text[5] = {0};
	DWORD read = 0;
	CHECK(h != INVALID_HANDLE_VALUE);
	if (h != INVALID_HANDLE_VALUE) {
		CHECK(ReadFile(h, text, 4, &read, NULL) && read == 4);
		CHECK(!memcmp(text, "KEEP", 4));
		CloseHandle(h);
	}
}

static void loader(P0Lab *lab, const wchar_t *product)
{
	wchar_t *old = NULL, staged[1024], command[1200], system[32768];
	DWORD needed = GetEnvironmentVariableW(L"LOCALAPPDATA", NULL, 0),
	      error = 0;
	if (needed) {
		old = malloc((size_t)needed * sizeof *old);
		CHECK(old &&
		        GetEnvironmentVariableW(L"LOCALAPPDATA", old, needed));
		if (!old) {
			return;
		}
	}
	CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA", lab->root));
	WfStatus status = wf_dechost_stage(product, staged, 1024, &error);
	printf("product_stage_status=%d win32=%lu\n", (int)status, error);
	CHECK(status == WF_OK);
	if (status == WF_OK) {
		wchar_t repeated[1024];
		CHECK(wf_dechost_stage(product, repeated, 1024, &error) ==
		        WF_OK);
		CHECK(!wcscmp(staged, repeated));
		DWORD n = GetSystemDirectoryW(system, 32768);
		CHECK(n && n < 32768);
		/* The child cannot accidentally find compiler-rt on the developer PATH. */
		CHECK(SetEnvironmentVariableW(L"PATH", system));
		STARTUPINFOW si = {0};
		PROCESS_INFORMATION pi = {0};
		si.cb = sizeof si;
		swprintf(command, 1200, L"\"%ls\" --version", staged);
		BOOL created = CreateProcessW(staged, command, NULL, NULL,
		        FALSE, CREATE_NO_WINDOW, NULL, lab->root, &si, &pi);
		printf("staged_loader_created=%d win32=%lu\n", (int)created,
		        created ? 0 : GetLastError());
		CHECK(created);
		if (created) {
			DWORD code = 99;
			CHECK(WaitForSingleObject(pi.hProcess, 15000) ==
			        WAIT_OBJECT_0);
			CHECK(GetExitCodeProcess(pi.hProcess, &code) &&
			        code == 0);
			if (code == STILL_ACTIVE) {
				TerminateProcess(pi.hProcess, 1);
				WaitForSingleObject(pi.hProcess, 5000);
			}
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}
	}
	CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA", old));
	free(old);
}

int wmain(int argc, wchar_t **argv)
{
	P0Lab lab = {0};
	CHECK(p0_lab_open(&lab, "sanitizer-stage", 0));
	if (!lab.area) {
		return TEST_RESULT();
	}
	if (argc == 2) {
		loader(&lab, argv[1]);
	} else {
		files(&lab);
	}
	/* Preserve the independently owned fixture, including rejected hardlinks. */
	CHECK(p0_lab_close(&lab, 1) == 0);
	return TEST_RESULT();
}

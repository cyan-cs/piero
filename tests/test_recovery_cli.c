#include <stdio.h>
#include <string.h>

#include "../tools/p0/p0_common.h"
#include "../tools/p0/p0_lab.h"
#include "recovery/wf_recovery_store.h"
#include "wf_test.h"

static int run(const wchar_t *exe,
        const wchar_t *id,
        const char *confirmation,
        const wchar_t *log)
{
	HANDLE input, write, output;
	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
	STARTUPINFOW si = {0};
	PROCESS_INFORMATION pi = {0};
	wchar_t command[4096];
	DWORD code = 99, wrote;
	int ok;
	if (!CreatePipe(&input, &write, &sa, 0)) {
		return 99;
	}
	SetHandleInformation(write, HANDLE_FLAG_INHERIT, 0);
	output = CreateFileW(
	        log, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW, 0, NULL);
	if (INVALID_HANDLE_VALUE == output) {
		CloseHandle(input);
		CloseHandle(write);
		return 99;
	}
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = input;
	si.hStdOutput = si.hStdError = output;
	swprintf(command, 4096, L"\"%ls\" --abandon-recovery %ls", exe, id);
	ok = CreateProcessW(exe, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
	             NULL, NULL, &si, &pi) != 0;
	CloseHandle(input);
	CloseHandle(output);
	if (ok) {
		WriteFile(write, confirmation, (DWORD)strlen(confirmation),
		        &wrote, NULL);
		CloseHandle(write);
		if (WAIT_OBJECT_0 != WaitForSingleObject(pi.hProcess, 5000)) {
			TerminateProcess(pi.hProcess, 99);
			WaitForSingleObject(pi.hProcess, 5000);
		}
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	} else {
		CloseHandle(write);
	}
	return (int)code;
}
int wmain(int argc, wchar_t **argv)
{
	const wchar_t *first = L"11111111-1111-1111-1111-111111111111",
	              *second = L"22222222-2222-2222-2222-222222222222";
	P0Lab lab;
	WfRecoveryStore *store = NULL;
	WfRecoveryLocalInfo info;
	DWORD error, need;
	wchar_t root[1024], state[1024], log1[1024], log2[1024], object[1024],
	        *saved = NULL;
	char hash[65], after[65];
	if (2 != argc) {
		return 2;
	}
	CHECK(p0_lab_open(&lab, "recovery-cli", 1));
	if (!lab.area) {
		return 1;
	}
	CHECK(p0_area_mkdir(lab.area, L"wfile"));
	CHECK(p0_lab_path(&lab, L"wfile\\journal", root, 1024));
	CHECK(wf_recovery_store_open(root, 1, &store, &error));
	if (!store) {
		p0_lab_close(&lab, 1);
		return 1;
	}
	CHECK(wf_recovery_store_put(store, first,
	        L"\\\\unavailable-wfile-test.invalid\\share", "first", 5,
	        &error));
	CHECK(wf_recovery_store_put(
	        store, second, L"REMOVED-TEST-VOLUME", "second", 6, &error));
	swprintf(object, 1024, L"%ls\\%ls.wjr", root, second);
	CHECK(p0_sha256_file(object, hash));
	wf_recovery_store_close(store);
	store = NULL;
	need = GetEnvironmentVariableW(L"LOCALAPPDATA", NULL, 0);
	if (need) {
		saved = malloc((size_t)need * sizeof *saved);
		CHECK(NULL != saved);
		if (saved) {
			GetEnvironmentVariableW(L"LOCALAPPDATA", saved, need);
		}
	}
	wcscpy_s(state, 1024, lab.root);
	CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA", state));
	CHECK(p0_lab_path(&lab, L"wrong.log", log1, 1024) &&
	        p0_lab_path(&lab, L"right.log", log2, 1024));
	CHECK(3 == run(argv[1], first, "wrong\n", log1));
	swprintf(object, 1024, L"%ls\\%ls.wjr", root, first);
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(object));
	CHECK(run(argv[1], first, "11111111-1111-1111-1111-111111111111\n",
	              log2) == 0);
	CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(object));
	CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA", saved));
	CHECK(wf_recovery_store_open(root, 0, &store, &error));
	if (store) {
		CHECK(1 == wf_recovery_store_get(store, 0, &info, &error));
		CHECK(!wcscmp(info.recovery_id, second));
		CHECK(0 == wf_recovery_store_get(store, 1, &info, &error));
	}
	wf_recovery_store_close(store);
	swprintf(object, 1024, L"%ls\\%ls.wjr", root, second);
	CHECK(p0_sha256_file(object, after) && !strcmp(hash, after));
	/*
	 * Both folders now exist (DR-DRAFT-017): "wfile" still holds `second`,
	 * "piero" holds `third`. The command must find the older record when
	 * the active store does not have the ID, and leave the other store's
	 * records alone.
	 */
	{
		const wchar_t *third = L"33333333-3333-3333-3333-333333333333";
		wchar_t piero_root[1024], old_object[1024], new_object[1024];
		char old_hash[65], new_hash[65];

		CHECK(p0_area_mkdir(lab.area, L"piero"));
		CHECK(p0_lab_path(&lab, L"piero\\journal", piero_root, 1024));
		CHECK(wf_recovery_store_open(piero_root, 1, &store, &error));
		if (store) {
			CHECK(wf_recovery_store_put(store, third,
			        L"REMOVED-TEST-VOLUME", "third", 5, &error));
		}
		wf_recovery_store_close(store);
		store = NULL;
		swprintf(old_object, 1024, L"%ls\\%ls.wjr", root, second);
		swprintf(new_object, 1024, L"%ls\\%ls.wjr", piero_root, third);
		CHECK(p0_sha256_file(new_object, new_hash));
		CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA", state));
		CHECK(p0_lab_path(&lab, L"both1.log", log1, 1024) &&
		        p0_lab_path(&lab, L"both2.log", log2, 1024));
		/* an ID that neither store has changes nothing and says so */
		CHECK(3 ==
		        run(argv[1], L"99999999-9999-9999-9999-999999999999",
		                "99999999-9999-9999-9999-999999999999\n",
		                log1));
		CHECK(INVALID_FILE_ATTRIBUTES !=
		                GetFileAttributesW(old_object) &&
		        INVALID_FILE_ATTRIBUTES !=
		                GetFileAttributesW(new_object));
		/* the older record is found and a wrong confirmation keeps it */
		CHECK(p0_lab_path(&lab, L"both3.log", log1, 1024));
		CHECK(3 == run(argv[1], second, "wrong\n", log1));
		CHECK(INVALID_FILE_ATTRIBUTES !=
		        GetFileAttributesW(old_object));
		/* the exact ID abandons it, and only it */
		CHECK(0 ==
		        run(argv[1], second,
		                "22222222-2222-2222-2222-222222222222\n",
		                log2));
		CHECK(INVALID_FILE_ATTRIBUTES ==
		        GetFileAttributesW(old_object));
		CHECK(p0_sha256_file(new_object, old_hash) &&
		        !strcmp(new_hash, old_hash));
		CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA", saved));
	}
	free(saved);
	printf("recovery CLI: exact-ID refusal/acceptance, other journal "
	       "SHA256 preserved; fixture=%ls\n",
	        p0_area_root(lab.area));
	CHECK(0 == p0_lab_close(&lab, 1));
	return TEST_RESULT();
}

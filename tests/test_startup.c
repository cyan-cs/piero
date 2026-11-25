#include "../tools/p0/p0_lab.h"
#include "path/cwd_neutralize.h"
#include "wf_test.h"
int main(void)
{
	P0Lab lab;
	wchar_t cwd[32768], path[1024];
	HANDLE pin;
	DWORD error;
	CHECK(GetCurrentDirectoryW(32768, cwd));
	CHECK(p0_lab_open(&lab, "startup", 1));
	if (!lab.area) {
		return 1;
	}
	CHECK(p0_lab_path(&lab, L"private", path, 1024));
	CHECK(wf_private_directory_open(path, 1, &pin, &error));
	if (INVALID_HANDLE_VALUE != pin) {
		CloseHandle(pin);
	}
	CHECK(p0_area_track(lab.area, L"private", 1));
	CHECK(wf_cwd_neutralize(path, &pin, &error));
	CHECK(SetCurrentDirectoryW(cwd));
	if (INVALID_HANDLE_VALUE != pin) {
		CloseHandle(pin);
	}
	{
		wchar_t child[1024];
		HANDLE file;
		DWORD wrote;
		CHECK(p0_lab_path(&lab, L"private\\unrelated", child, 1024));
		file = CreateFileW(
		        child, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
		CHECK(INVALID_HANDLE_VALUE != file);
		CHECK(WriteFile(file, "USER", 4, &wrote, NULL));
		CloseHandle(file);
		CHECK(p0_area_track(lab.area, L"private\\unrelated", 0));
		CHECK(!wf_cwd_neutralize(path, &pin, &error));
		/* said as what it is: not an access denial */
		CHECK(ERROR_DIR_NOT_EMPTY == error);
		CHECK(INVALID_HANDLE_VALUE == pin);
		CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(child));
	}
	CHECK(p0_area_mkdir(lab.area, L"untrusted"));
	CHECK(p0_lab_path(&lab, L"untrusted", path, 1024));
	CHECK(!wf_private_directory_open(path, 1, &pin, &error));
	/* a folder with inherited rights is "not private", not "access denied" */
	CHECK(WF_PRIVATE_E_NOT_PRIVATE == error);
	CHECK(INVALID_HANDLE_VALUE == pin);
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(path));
	/* a file where the folder should be */
	{
		wchar_t file[1024];
		HANDLE h;

		CHECK(p0_lab_path(&lab, L"not-a-folder", file, 1024));
		h = CreateFileW(
		        file, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
		CHECK(INVALID_HANDLE_VALUE != h);
		if (INVALID_HANDLE_VALUE != h) {
			CloseHandle(h);
		}
		CHECK(p0_area_track(lab.area, L"not-a-folder", 0));
		CHECK(!wf_private_directory_open(file, 0, &pin, &error));
		CHECK(ERROR_DIRECTORY == error);
		CHECK(INVALID_HANDLE_VALUE == pin);
	}
	/* a link where the folder should be is not followed */
	{
		wchar_t target[1024], link[1024], cmd[2300];
		STARTUPINFOW si = {0};
		PROCESS_INFORMATION pi = {0};
		DWORD code = 99;

		CHECK(p0_area_mkdir(lab.area, L"link-target"));
		CHECK(p0_lab_path(&lab, L"link-target", target, 1024));
		CHECK(p0_lab_path(&lab, L"linked", link, 1024));
		swprintf(cmd, 2300, L"cmd.exe /c mklink /J \"%ls\" \"%ls\"",
		        link, target);
		si.cb = sizeof si;
		if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
		            CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
			WaitForSingleObject(pi.hProcess, 10000);
			GetExitCodeProcess(pi.hProcess, &code);
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}
		CHECK(0 == code);
		CHECK(!wf_private_directory_open(link, 0, &pin, &error));
		CHECK(ERROR_REPARSE_POINT_ENCOUNTERED == error);
		CHECK(INVALID_HANDLE_VALUE == pin);
		/* the link goes before its target; the harness removes both */
		CHECK(RemoveDirectoryW(link));
	}
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}

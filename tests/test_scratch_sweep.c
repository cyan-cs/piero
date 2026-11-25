/*
 * The sweep of stale test areas: it removes what a killed run left, and only
 * that. Everything here happens in a base directory this test creates and
 * removes; no other location is read or written.
 *
 *   test_scratch_sweep <p0_scratch_sweep.exe> <a directory for this test>
 *
 * Checked: a listing changes nothing; --reap removes the large test files and
 * the area of a dead run; a live run's area, a directory without a valid
 * marker, a decoy file and a hard-linked "big.bin" are all left alone.
 */
#include <windows.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "p0_area.h"
#include "wf_test.h"

static bool exists(const wchar_t *p)
{
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(p);
}

static bool write_file(const wchar_t *p, size_t bytes)
{
	static char block[65536];
	HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	size_t done = 0;
	bool ok = INVALID_HANDLE_VALUE != h;

	while (ok && done < bytes) {
		DWORD put = 0,
		      want = (DWORD)(bytes - done < sizeof block
		                      ? bytes - done
		                      : sizeof block);

		ok = WriteFile(h, block, want, &put, NULL) && put == want;
		done += put;
	}
	if (INVALID_HANDLE_VALUE != h) {
		ok = CloseHandle(h) && ok;
	}
	return ok;
}

static int run(const wchar_t *tool, const wchar_t *base, const wchar_t *flag)
{
	wchar_t cmd[2200];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	DWORD code = 99;

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	swprintf(cmd, 2200, L"\"%ls\" \"%ls\"%ls%ls", tool, base,
	        flag ? L" " : L"", flag ? flag : L"");
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = INVALID_HANDLE_VALUE;
	si.hStdError = INVALID_HANDLE_VALUE;
	si.hStdInput = INVALID_HANDLE_VALUE;
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	            NULL, NULL, &si, &pi)) {
		return -1;
	}
	WaitForSingleObject(pi.hProcess, 60000);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return (int)code;
}

int wmain(int argc, wchar_t **argv)
{
	wchar_t base[1100], big[1100], tmp[1100], decoy[1100], link2[1100],
	        live_big[1100], plain[1100], notours[1100];
	P0Area *dead, *live;
	char id[96];

	if (argc != 3) {
		fputs("usage: test_scratch_sweep <tool> <directory>\n", stderr);
		return 2;
	}
	swprintf(base, 1100, L"%ls\\sweep-%lu", argv[2], GetCurrentProcessId());
	for (wchar_t *c = base; *c; c++) {
		if (L'/' == *c) {
			*c = L'\\'; /* an extended path takes no forward slash */
		}
	}
	CHECK(CreateDirectoryW(base, NULL));

	/* a dead run (a pid no process has) with large files and a decoy */
	snprintf(id, sizeof id, "sweeptest-2000000000-00112233445566ff");
	dead = p0_area_create(base, id);
	CHECK(NULL != dead);
	CHECK(p0_area_mkdir(dead, L"bigsrc") && p0_area_mkdir(dead, L"dest"));
	CHECK(p0_area_path(dead, L"bigsrc\\big.bin", big, 1100));
	CHECK(p0_area_path(dead, L"dest\\.wfcopy-aa11bb22.tmp", tmp, 1100));
	CHECK(p0_area_path(dead, L"bigsrc\\keep.txt", decoy, 1100));
	CHECK(write_file(big, 3 << 20) && write_file(tmp, 2 << 20) &&
	        write_file(decoy, 100));
	/* a "big.bin" with a second name is not a plain fixture: kept */
	CHECK(p0_area_path(dead, L"dest\\big.bin", plain, 1100));
	CHECK(p0_area_path(dead, L"dest\\other-name", link2, 1100));
	CHECK(write_file(link2, 1 << 20) &&
	        CreateHardLinkW(plain, link2, NULL));

	/* a live run: this very process's id */
	snprintf(id, sizeof id, "sweeptest-%lu-aabbccddeeff0011",
	        GetCurrentProcessId());
	live = p0_area_create(base, id);
	CHECK(NULL != live);
	CHECK(p0_area_mkdir(live, L"bigsrc"));
	CHECK(p0_area_path(live, L"bigsrc\\big.bin", live_big, 1100));
	CHECK(write_file(live_big, 1 << 20));

	/* a directory of the right name shape but with no marker */
	swprintf(notours, 1100,
	        L"%ls\\wfile-p0-sweeptest-2000000001-1122334455667788", base);
	CHECK(CreateDirectoryW(notours, NULL));

	/* listing only: nothing changes */
	CHECK(0 == run(argv[1], base, NULL));
	CHECK(exists(big) && exists(tmp) && exists(decoy) && exists(live_big) &&
	        exists(notours));

	/* reap */
	CHECK(0 == run(argv[1], base, L"--reap"));
	CHECK(!exists(big) &&
	        !exists(tmp)); /* the known large files are gone */
	CHECK(exists(decoy));  /* anything else stays */
	CHECK(exists(plain) && exists(link2)); /* a hard-linked file stays */
	CHECK(exists(live_big));               /* a live run keeps its area */
	CHECK(exists(notours)); /* not provably ours: left alone */

	/* our own cleanup, with the same proofs the area itself enforces */
	{
		wchar_t report[1024] = L"";

		DeleteFileW(decoy);
		DeleteFileW(plain);
		DeleteFileW(link2);
		p0_area_destroy(dead, report, 1024);
		DeleteFileW(live_big);
		p0_area_destroy(live, report, 1024);
		RemoveDirectoryW(notours);
		RemoveDirectoryW(base);
	}
	return TEST_RESULT();
}

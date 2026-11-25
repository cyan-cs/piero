/*
 * Finds the scratch areas that earlier test runs left behind, and optionally
 * removes them. A run that is killed (a CTest timeout, a crash) cannot clean
 * up after itself, and its large fixtures then pile up on the disk.
 *
 *   p0_scratch_sweep <base directory> [--reap]
 *
 * Without --reap nothing is changed: every candidate is listed with what it
 * holds. A directory is touched only if ALL of these hold:
 *   - its name is wfile-p0-<label>-<pid>-<id> (the shape p0_area creates),
 *   - no process with that pid is running (a live run keeps its area),
 *   - its .p0-owner marker and the directory's File ID verify (p0_area_open),
 *   - it is a real directory, not a link.
 * Anything else is named and left alone: the sweep never deletes what it
 * cannot prove a test created.
 *
 * With --reap an owned, stale area is cleaned in two steps:
 *   1. the known large test files, by exact place, each proven to be a plain
 *      file (not a link) inside the area: big.bin in the fixture folders of
 *      the transfer scenarios, and the copy engine's staging files
 *      (".wfcopy-*.tmp") in the destination folders of those scenarios.
 *      Nothing else is removed from an area by name or by pattern.
 *   2. p0_area_destroy(): removes what the run recorded in its manifest, in
 *      reverse order, then the marker, manifest and root; anything unexpected
 *      is named and kept, so the directory may remain.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p0_area.h"

/* Folders of the transfer scenarios that hold large files, relative to an area. */
static const wchar_t *const FIXTURE_DIRS[] = {
        L"bigsrc", L"dest", L"dest2", L"mid"};

static bool parse_name(const wchar_t *name, DWORD *pid)
{
	const wchar_t *last, *prev;
	wchar_t *end;
	unsigned long v;

	if (wcsncmp(name, L"wfile-p0-", 9)) {
		return false;
	}
	last = wcsrchr(name, L'-');
	if (!last || last == name || wcslen(last + 1) < 8) {
		return false;
	}
	prev = last - 1;
	while (prev > name && *prev != L'-') {
		prev--;
	}
	if (*prev != L'-') {
		return false;
	}
	v = wcstoul(prev + 1, &end, 10);
	if (end != last || v == 0) {
		return false;
	}
	*pid = (DWORD)v;
	return true;
}

static bool pid_running(DWORD pid)
{
	HANDLE h = OpenProcess(
	        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	bool running;

	if (!h) {
		/* access denied means a process exists that we may not open */
		return ERROR_INVALID_PARAMETER != GetLastError();
	}
	running = WAIT_TIMEOUT == WaitForSingleObject(h, 0);
	CloseHandle(h);
	return running;
}

/* A plain file (no link, no directory) whose final path is inside `root`. */
static bool plain_file_inside(
        const wchar_t *path, const wchar_t *root, ULONGLONG *bytes)
{
	HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	BY_HANDLE_FILE_INFORMATION info;
	wchar_t final_path[2048];
	DWORD n;
	size_t root_len = wcslen(root);
	bool ok = false;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	if (GetFileInformationByHandle(h, &info) &&
	        !(info.dwFileAttributes &
	                (FILE_ATTRIBUTE_DIRECTORY |
	                        FILE_ATTRIBUTE_REPARSE_POINT)) &&
	        1 == info.nNumberOfLinks) {
		n = GetFinalPathNameByHandleW(
		        h, final_path, 2048, FILE_NAME_NORMALIZED);
		/* "\\?\C:\..." versus the root as given: compare the tails */
		if (n > 0 && n < 2048) {
			const wchar_t *fp = final_path;

			if (!wcsncmp(fp, L"\\\\?\\", 4)) {
				fp += 4;
			}
			ok = !_wcsnicmp(fp, root, root_len) &&
			        (L'\\' == fp[root_len]);
		}
		if (ok && bytes) {
			*bytes = ((ULONGLONG)info.nFileSizeHigh << 32) |
			        info.nFileSizeLow;
		}
	}
	CloseHandle(h);
	return ok;
}

/* Lists or removes the known large files of one owned area. */
static ULONGLONG large_files(P0Area *area, bool reap)
{
	const wchar_t *root = p0_area_root(area);
	ULONGLONG total = 0;
	size_t i;

	for (i = 0; i < sizeof FIXTURE_DIRS / sizeof *FIXTURE_DIRS; i++) {
		wchar_t dir[1100], pattern[1100], path[1100];
		WIN32_FIND_DATAW fd;
		HANDLE find;

		if (!p0_area_path(area, FIXTURE_DIRS[i], dir, 1100)) {
			continue;
		}
		swprintf(pattern, 1100, L"%ls\\*", dir);
		find = FindFirstFileExW(pattern, FindExInfoBasic, &fd,
		        FindExSearchNameMatch, NULL, 0);
		if (INVALID_HANDLE_VALUE == find) {
			continue;
		}
		do {
			ULONGLONG bytes = 0;
			bool known = !wcscmp(fd.cFileName, L"big.bin") ||
			        (!wcsncmp(fd.cFileName, L".wfcopy-", 8) &&
			                wcslen(fd.cFileName) > 12 &&
			                !wcscmp(fd.cFileName +
			                                wcslen(fd.cFileName) -
			                                4,
			                        L".tmp"));

			if (!known) {
				continue;
			}
			swprintf(path, 1100, L"%ls\\%ls", dir, fd.cFileName);
			if (!plain_file_inside(path, root, &bytes)) {
				wprintf(L"  kept (not a plain file inside the "
				        L"area): %ls\n",
				        path);
				continue;
			}
			total += bytes;
			if (reap) {
				if (DeleteFileW(path)) {
					wprintf(L"  removed %ls (%llu MiB)\n",
					        path, bytes >> 20);
				} else {
					wprintf(L"  could not remove %ls "
					        L"(error %lu)\n",
					        path, GetLastError());
				}
			} else {
				wprintf(L"  large: %ls (%llu MiB)\n", path,
				        bytes >> 20);
			}
		} while (FindNextFileW(find, &fd));
		FindClose(find);
	}
	return total;
}

int wmain(int argc, wchar_t **argv)
{
	wchar_t pattern[1100];
	WIN32_FIND_DATAW fd;
	HANDLE find;
	bool reap = argc > 2 && !wcscmp(argv[2], L"--reap");
	ULONGLONG total = 0;
	unsigned areas = 0, live = 0, refused = 0, gone = 0;

	if (argc < 2 || (argc > 2 && !reap) || argc > 3) {
		fputs("usage: p0_scratch_sweep <base directory> [--reap]\n",
		        stderr);
		return 2;
	}
	for (wchar_t *c = argv[1]; *c; c++) {
		if (L'/' == *c) {
			*c = L'\\'; /* extended paths below take no forward slash */
		}
	}
	swprintf(pattern, 1100, L"%ls\\wfile-p0-*", argv[1]);
	find = FindFirstFileExW(
	        pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
	if (INVALID_HANDLE_VALUE == find) {
		puts("no scratch areas");
		return 0;
	}
	do {
		wchar_t root[1100];
		DWORD pid = 0;
		P0Area *area;

		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
		        (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
		        !parse_name(fd.cFileName, &pid)) {
			continue;
		}
		areas++;
		swprintf(root, 1100, L"%ls\\%ls", argv[1], fd.cFileName);
		if (pid_running(pid)) {
			wprintf(L"live (pid %lu), left alone: %ls\n", pid,
			        root);
			live++;
			continue;
		}
		area = p0_area_open(root);
		if (!area) {
			wprintf(L"not provably ours (marker or identity), left "
			        L"alone: "
			        L"%ls\n",
			        root);
			refused++;
			continue;
		}
		wprintf(L"stale and owned: %ls\n", root);
		total += large_files(area, reap);
		if (reap) {
			wchar_t report[2048] = L"";
			int left = p0_area_destroy(area, report, 2048);

			if (left) {
				wprintf(L"  %d item(s) kept: %ls\n", left,
				        report);
			} else {
				gone++;
			}
		} else {
			p0_area_forget(area);
		}
	} while (FindNextFileW(find, &fd));
	FindClose(find);
	printf("%u area(s): %u live, %u not provably ours, %u removed; large "
	       "files %s %llu MiB\n",
	        areas, live, refused, gone, reap ? "removed" : "found",
	        total >> 20);
	return 0;
}

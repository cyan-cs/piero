/*
 * Listing, choosing and opening a file on a FAT volume, with the same
 * unsupported answers all the way. FAT refuses FileIdInfo,
 * FileIdExtdDirectoryInfo and FileAttributeTagInfo, and the volume says FAT32;
 * here a real folder answers like that. The listing must then give weak
 * identities, and opening a listed file must go through on its weak identity
 * (its attributes are read the older way too), and never on a wrong one.
 * The shell is not started: a call to it is only counted.
 */
#include <windows.h>

#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "wf_test.h"

static int fat_mode, shell_calls;
static wchar_t shell_file[1100];

static BOOL mock_info(
        HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID buffer, DWORD bytes)
{
	if (fat_mode &&
	        (FileIdInfo == cls || FileAttributeTagInfo == cls ||
	                FileIdExtdDirectoryInfo == cls)) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return GetFileInformationByHandleEx(h, cls, buffer, bytes);
}

static BOOL mock_volume(HANDLE h,
        LPWSTR name,
        DWORD name_n,
        LPDWORD serial,
        LPDWORD component,
        LPDWORD flags,
        LPWSTR fs,
        DWORD fs_n)
{
	BOOL ok = GetVolumeInformationByHandleW(
	        h, name, name_n, serial, component, flags, fs, fs_n);

	if (ok && fat_mode && fs && fs_n >= 6) {
		wcscpy_s(fs, fs_n, L"FAT32");
	}
	return ok;
}

static BOOL mock_shell(SHELLEXECUTEINFOW *sei)
{
	shell_calls++;
	wcsncpy_s(shell_file, 1100, sei->lpFile, _TRUNCATE);
	return TRUE;
}

#define GetFileInformationByHandleEx mock_info
#define GetVolumeInformationByHandleW mock_volume
#define ShellExecuteExW mock_shell
#include "../src/fs/wf_enum_win.c"
#include "../src/fs/wf_launch_win.c"
#undef ShellExecuteExW
#undef GetVolumeInformationByHandleW
#undef GetFileInformationByHandleEx

static const WfEntry *find(const WfDir *d, const wchar_t *name)
{
	for (size_t i = 0; i < d->n; i++) {
		if (!wcscmp(d->e[i].name, name)) {
			return &d->e[i];
		}
	}
	return NULL;
}

static bool make(const wchar_t *path)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD put;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	WriteFile(h, "x", 1, &put, NULL);
	CloseHandle(h);
	return true;
}

int main(void)
{
	wchar_t tmp[MAX_PATH], dir[MAX_PATH + 40], a[MAX_PATH + 60],
	        b[MAX_PATH + 60], sub[MAX_PATH + 60];
	WfDir *d = NULL;
	const WfEntry *ea, *eb, *es;
	WfFileId strong;
	DWORD err = 0;

	GetTempPathW(MAX_PATH, tmp);
	swprintf(dir, MAX_PATH + 40, L"%lspiero-launchfat-%lu", tmp,
	        GetCurrentProcessId());
	swprintf(a, MAX_PATH + 60, L"%ls\\a.txt", dir);
	swprintf(b, MAX_PATH + 60, L"%ls\\b.txt", dir);
	swprintf(sub, MAX_PATH + 60, L"%ls\\sub", dir);
	CHECK(CreateDirectoryW(dir, NULL));
	CHECK(make(a) && make(b));
	CHECK(CreateDirectoryW(sub, NULL));

	/* a FAT volume: listed with weak identities, opened on them */
	fat_mode = 1;
	CHECK(WF_OK == wf_enum_dir_win(dir, &d) && d);
	CHECK(d && wf_fileid_is_weak(&d->identity) &&
	        !(d->identity.kind == WF_ID_STRONG));
	ea = d ? find(d, L"a.txt") : NULL;
	eb = d ? find(d, L"b.txt") : NULL;
	es = d ? find(d, L"sub") : NULL;
	CHECK(ea && eb && es);
	if (ea && eb && es) {
		CHECK(wf_fileid_is_weak(&ea->identity) &&
		        !(ea->identity.kind == WF_ID_STRONG));
		CHECK(WF_OK == wf_launch_checked(a, &ea->identity, dir, &err));
		CHECK(1 == shell_calls && !wcscmp(shell_file, a));
		/* the weak identity of another file is not this file's */
		CHECK(WF_E_INVAL == wf_launch_checked(a, &eb->identity, dir, &err));
		/* nor is a proven identity claimed for a volume that has none */
		strong = ea->identity;
		strong.kind = WF_ID_STRONG;
		CHECK(WF_E_INVAL == wf_launch_checked(a, &strong, dir, &err));
		/* a folder is not opened */
		CHECK(WF_OK != wf_launch_checked(sub, &es->identity, dir, &err));
		CHECK(1 == shell_calls);
	}
	wf_dir_free(d);
	d = NULL;

	/* an ordinary volume: proven identities as before, and a weak one is
	 * never taken for them */
	fat_mode = 0;
	CHECK(WF_OK == wf_enum_dir_win(dir, &d) && d);
	CHECK(d && (d->identity.kind == WF_ID_STRONG) &&
	        !wf_fileid_is_weak(&d->identity));
	ea = d ? find(d, L"a.txt") : NULL;
	if (ea) {
		WfFileId weak = ea->identity;

		CHECK(WF_OK == wf_launch_checked(a, &ea->identity, dir, &err));
		CHECK(2 == shell_calls);
		wf_weak_fill(&weak, ea->identity.volume, 1);
		CHECK(WF_E_INVAL == wf_launch_checked(a, &weak, dir, &err));
		CHECK(2 == shell_calls);
	}
	wf_dir_free(d);

	/* clean up what was made, by exact name */
	CHECK(DeleteFileW(a) && DeleteFileW(b));
	CHECK(RemoveDirectoryW(sub));
	CHECK(RemoveDirectoryW(dir));
	return TEST_RESULT();
}

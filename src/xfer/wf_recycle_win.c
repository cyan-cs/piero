#define COBJMACROS
#include "wf_recycle_win.h"

#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../path/wf_path.h"
#include "../base/wf_macro.h"

/* FOFX_* flags are in newer SDK headers; spelled out so older ones build. */
#ifndef FOFX_RECYCLEONDELETE
#	define FOFX_RECYCLEONDELETE 0x00080000
#endif
#ifndef FOFX_EARLYFAILURE
#	define FOFX_EARLYFAILURE 0x00100000
#endif

static bool com_started;

void (*wf_recycle_test_before_shell)(const wchar_t *path);

bool wf_recycle_thread_init(void)
{
	HRESULT hr = CoInitializeEx(
	        NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

	com_started = SUCCEEDED(hr);
	return com_started;
}

void wf_recycle_thread_done(void)
{
	if (com_started) {
		CoUninitialize();
	}
	com_started = false;
}

bool wf_recycle_fits(
        bool nuke_on_delete, uint64_t capacity_bytes, uint64_t size)
{
	return !nuke_on_delete && 0 != capacity_bytes && size <= capacity_bytes;
}

/* Bytes below `path` (links are not followed and count as nothing). False
 * when the tree is too big to measure here. */
static bool tree_bytes(
        const wchar_t *path, size_t depth, size_t *budget, uint64_t *total)
{
	wchar_t *pattern = NULL;
	WIN32_FIND_DATAW fd;
	HANDLE f;
	bool ok = true;
	size_t n = wcslen(path);

	if (depth > 64) {
		return false;
	}
	pattern = malloc((n + 3) * sizeof *pattern);
	if (!pattern) {
		return false;
	}
	memcpy(pattern, path, n * sizeof *pattern);
	memcpy(pattern + n, L"\\*", 3 * sizeof *pattern);
	f = FindFirstFileExW(pattern, FindExInfoBasic, &fd,
	        FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
	free(pattern);
	if (INVALID_HANDLE_VALUE == f) {
		return ERROR_FILE_NOT_FOUND == GetLastError();
	}
	do {
		if (!wcscmp(fd.cFileName, L".") ||
		        !wcscmp(fd.cFileName, L"..")) {
			continue;
		}
		if (0 == *budget) {
			ok = false;
			break;
		}
		(*budget)--;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
			continue;
		}
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			wchar_t *sub = NULL;

			if (WF_OK != wf_path_join(path, fd.cFileName, &sub)) {
				ok = false;
				break;
			}
			ok = tree_bytes(sub, depth + 1, budget, total);
			free(sub);
			if (!ok) {
				break;
			}
		} else {
			uint64_t bytes = ((uint64_t)fd.nFileSizeHigh << 32) |
			        fd.nFileSizeLow;
			if (bytes > UINT64_MAX - *total) {
				ok = false;
				break;
			}
			*total += bytes;
		}
	} while (FindNextFileW(f, &fd));
	if (ok && ERROR_NO_MORE_FILES != GetLastError()) {
		ok = false;
	}
	FindClose(f);
	return ok;
}

/* What the user's settings let the bin of `root` take: false if the bin is
 * set to delete immediately. Without a per-volume setting Windows' default
 * (a tenth of the volume) is assumed. */
static bool bin_policy(
        const wchar_t *root, bool *nuke, uint64_t *capacity, DWORD *error)
{
	wchar_t name[64], key[160];
	const wchar_t *open, *close;
	DWORD value = 0, size = sizeof value;
	ULARGE_INTEGER total;
	LSTATUS st;
	*error = ERROR_SUCCESS;

	*nuke = false;
	if (!GetDiskFreeSpaceExW(root, NULL, &total, NULL)) {
		*error = GetLastError();
		return false;
	}
	*capacity = total.QuadPart / 10;
	if (!GetVolumeNameForVolumeMountPointW(root, name, 64)) {
		*error = GetLastError();
		return false;
	}
	open = wcschr(name, L'{');
	close = wcschr(name, L'}');
	if (!open || !close || close < open || (size_t)(close - open) > 60) {
		*error = ERROR_INVALID_DATA;
		return false;
	}
	swprintf(key, 160,
	        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\BitBu"
	        L"cket\\"
	        L"Volume\\%.*ls",
	        (int)(close - open + 1), open);
	st = RegGetValueW(HKEY_CURRENT_USER, key, L"NukeOnDelete", RRF_RT_DWORD,
	        NULL, &value, &size);
	if (ERROR_SUCCESS == st) {
		*nuke = 0 != value;
	} else if (ERROR_FILE_NOT_FOUND != st && ERROR_PATH_NOT_FOUND != st) {
		*error = (DWORD)st;
		return false;
	}
	size = sizeof value;
	st = RegGetValueW(HKEY_CURRENT_USER, key, L"MaxCapacity", RRF_RT_DWORD,
	        NULL, &value, &size);
	if (ERROR_SUCCESS == st) {
		*capacity = (uint64_t)value * 1024u * 1024u;
	} else if (ERROR_FILE_NOT_FOUND != st && ERROR_PATH_NOT_FOUND != st) {
		*error = (DWORD)st;
		return false;
	}
	return true;
}

static bool same_file_id(const WfFileId *want, const FILE_ID_INFO *id)
{
	return want && (want->kind == WF_ID_STRONG) &&
	        want->volume == id->VolumeSerialNumber &&
	        !memcmp(want->id, id->FileId.Identifier, sizeof want->id);
}

/* The object at `path` is `expected` and sits on a local fixed disk. */
WfStatus wf_recycle_preflight(
        const wchar_t *path, const WfFileId *expected, WfRecycleReport *report)
{
	wchar_t *ext = NULL, root[MAX_PATH];
	HANDLE h;
	FILE_ID_INFO id;
	FILE_STANDARD_INFO std;
	WfStatus st;

	WfRecycleReport local;
	if (!report) {
		report = &local;
	}
	memset(report, 0, sizeof *report);
	report->phase = WF_RP_IDENTITY;
	if (!path || !expected || !(expected->kind == WF_ID_STRONG)) {
		return WF_E_INVAL;
	}
	st = wf_path_to_extended(path, &ext);
	if (WF_OK != st) {
		return st;
	}
	h = CreateFileW(ext, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	free(ext);
	if (INVALID_HANDLE_VALUE == h) {
		report->error = GetLastError();
		return ERROR_FILE_NOT_FOUND == report->error ? WF_E_NOENT
		                                             : WF_E_OS;
	}
	if (!GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id) ||
	        !GetFileInformationByHandleEx(
	                h, FileStandardInfo, &std, sizeof std)) {
		report->error = GetLastError();
		CloseHandle(h);
		return WF_E_OS;
	}
	CloseHandle(h);
	if (!same_file_id(expected, &id)) {
		return WF_E_INVAL;
	}
	report->phase = WF_RP_VOLUME;
	if (!GetVolumePathNameW(path, root, MAX_PATH)) {
		report->error = GetLastError();
		return WF_E_ACCESS;
	}
	if (DRIVE_FIXED != GetDriveTypeW(root)) {
		return WF_E_ACCESS;
	}
	/* A drive without a Recycle Bin would make Windows offer to delete for
	 * good; that is never what Delete means here, so it is refused up front. */
	{
		SHQUERYRBINFO bin;

		memset(&bin, 0, sizeof bin);
		bin.cbSize = sizeof bin;
		report->phase = WF_RP_BIN_QUERY;
		report->hresult = SHQueryRecycleBinW(root, &bin);
		if (FAILED(report->hresult)) {
			return WF_E_ACCESS;
		}
	}
	{
		bool nuke = false;
		uint64_t capacity = 0, bytes = (uint64_t)std.EndOfFile.QuadPart;

		report->phase = WF_RP_BIN_POLICY;
		if (!bin_policy(root, &nuke, &capacity, &report->error)) {
			return WF_E_ACCESS;
		}
		if (std.Directory) {
			size_t budget = 1000000;

			bytes = 0;
			if (!tree_bytes(path, 0, &budget, &bytes)) {
				return WF_E_ACCESS; /* cannot show it fits */
			}
		}
		if (!wf_recycle_fits(nuke, capacity, bytes)) {
			return WF_E_ACCESS;
		}
	}
	return WF_OK;
}

/* The folder, held without delete sharing, and checked to be `want`. */
static HANDLE hold_folder(const wchar_t *path, const WfFileId *want)
{
	wchar_t *parent = NULL, *x = NULL;
	size_t n = wf_path_parent_len(path);
	HANDLE h = INVALID_HANDLE_VALUE;
	FILE_ID_INFO id;
	FILE_BASIC_INFO b;

	if (!n || !(parent = wf_path_dup_n(path, n)) ||
	        WF_OK != wf_path_to_extended(parent, &x)) {
		free(parent);
		return INVALID_HANDLE_VALUE;
	}
	free(parent);
	h = CreateFileW(x, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	free(x);
	if (INVALID_HANDLE_VALUE == h) {
		return h;
	}
	if (!GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id) ||
	        !GetFileInformationByHandleEx(h, FileBasicInfo, &b, sizeof b) ||
	        !(b.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
	        (b.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
	        !same_file_id(want, &id)) {
		CloseHandle(h);
		return INVALID_HANDLE_VALUE;
	}
	return h;
}

/* The object, held (the shell may still move it), checked to be `want`. */
static HANDLE hold_object(const wchar_t *path, const WfFileId *want)
{
	wchar_t *x = NULL;
	HANDLE h;
	FILE_ID_INFO id;

	if (WF_OK != wf_path_to_extended(path, &x)) {
		return INVALID_HANDLE_VALUE;
	}
	h = CreateFileW(x, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	free(x);
	if (INVALID_HANDLE_VALUE == h) {
		return h;
	}
	if (!GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id) ||
	        !same_file_id(want, &id)) {
		CloseHandle(h);
		return INVALID_HANDLE_VALUE;
	}
	return h;
}

/* Whether the held object now lives in a Recycle Bin. */
static int in_the_bin(HANDLE h, DWORD *error)
{
	wchar_t now[MAX_PATH * 4];
	DWORD n = GetFinalPathNameByHandleW(
	        h, now, (DWORD)(WF_ARRAY_LEN(now)), VOLUME_NAME_GUID);

	if (!n || n >= WF_ARRAY_LEN(now)) {
		*error = !n ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
		return -1;
	}
	/* A nested directory named $Recycle.Bin is not a recycling receipt. */
	const wchar_t *end = wcschr(now, L'}');
	return !_wcsnicmp(now, L"\\\\?\\Volume{", 11) && end &&
	        !_wcsnicmp(end + 1, L"\\$Recycle.Bin\\", 14);
}

WfStatus wf_recycle_guarded(const wchar_t *path,
        const WfFileId *expected,
        const WfFileId *parent,
        bool (*ready)(void *),
        void *context,
        WfRecycleReport *report)
{
	HANDLE folder = INVALID_HANDLE_VALUE, held = INVALID_HANDLE_VALUE;
	IFileOperation *op = NULL;
	IShellItem *item = NULL;
	WfRecycleReport local;
	HRESULT hr;
	WfStatus st;
	BOOL aborted = FALSE;

	if (!report) {
		report = &local;
	}
	memset(report, 0, sizeof *report);
	if (!path || !com_started || !expected ||
	        !(expected->kind == WF_ID_STRONG) || !parent ||
	        !(parent->kind == WF_ID_STRONG)) {
		return WF_E_INVAL;
	}
	folder = hold_folder(path, parent);
	if (INVALID_HANDLE_VALUE == folder) {
		return WF_E_INVAL;
	}
	held = hold_object(path, expected);
	if (INVALID_HANDLE_VALUE == held) {
		st = WF_E_INVAL;
		goto out_close_folder;
	}
	st = wf_recycle_preflight(path, expected, report);
	if (WF_OK != st) {
		goto out_close_held;
	}
	if (wf_recycle_test_before_shell) {
		wf_recycle_test_before_shell(path);
	}
	report->phase = WF_RP_SHELL;
	hr = CoCreateInstance(&CLSID_FileOperation, NULL, CLSCTX_ALL,
	        &IID_IFileOperation, (void **)&op);
	if (FAILED(hr)) {
		report->hresult = hr;
		st = WF_E_OS;
		goto out_close_held;
	}
	/* Recycle with an undo record; no NOCONFIRMATION or SILENT, so a delete
	 * the bin cannot take is never answered on the user's behalf.
	 * NOERRORUI: an error (access denied, in use) comes back as an HRESULT
	 * and is reported by the caller. Without it the Shell opens a dialog
	 * ("Folder Access Denied") that this console program has no window for,
	 * that waits for a person for as long as it takes and that offers to
	 * carry on as administrator: no elevation is ever offered. */
	hr = IFileOperation_SetOperationFlags(op,
	        FOF_ALLOWUNDO | FOF_WANTNUKEWARNING | FOF_NOCONFIRMMKDIR |
	                FOF_NOERRORUI | FOFX_EARLYFAILURE |
	                FOFX_RECYCLEONDELETE);
	if (SUCCEEDED(hr)) {
		hr = SHCreateItemFromParsingName(
		        path, NULL, &IID_IShellItem, (void **)&item);
	}
	if (SUCCEEDED(hr)) {
		hr = IFileOperation_DeleteItem(op, item, NULL);
	}
	if (SUCCEEDED(hr) && ready && !ready(context)) {
		report->error = ERROR_RETRY;
		hr = HRESULT_FROM_WIN32(ERROR_RETRY);
	}
	if (SUCCEEDED(hr)) {
		HRESULT abort_hr;
		report->attempted = true;
		hr = IFileOperation_PerformOperations(op);
		abort_hr = IFileOperation_GetAnyOperationsAborted(op, &aborted);
		if (SUCCEEDED(hr)) {
			hr = abort_hr;
		}
	}
	report->aborted = FALSE != aborted;
	if (item) {
		IShellItem_Release(item);
	}
	IFileOperation_Release(op);
	report->phase = WF_RP_RECEIPT;
	report->still_there =
	        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(path);
	if (!report->still_there) {
		DWORD error = GetLastError();
		if (ERROR_FILE_NOT_FOUND != error &&
		        ERROR_PATH_NOT_FOUND != error) {
			report->error = error;
			if (SUCCEEDED(hr)) {
				hr = HRESULT_FROM_WIN32(error);
			}
		}
	}
	report->hresult = hr;
	if (SUCCEEDED(hr) && !aborted && !report->still_there) {
		int receipt = in_the_bin(held, &report->error);
		if (1 != receipt) {
			/* Unknown receipt is not evidence that another object was taken. */
			report->swapped = 0 == receipt;
			st = receipt < 0 ? WF_E_OS : WF_E_INVAL;
			goto out_close_held;
		}
	}
	st = FAILED(hr) || aborted || report->still_there ? WF_E_OS : WF_OK;
out_close_held:
	CloseHandle(held);
out_close_folder:
	CloseHandle(folder);
	return st;
}

WfStatus wf_recycle(const wchar_t *path,
        const WfFileId *expected,
        const WfFileId *parent,
        WfRecycleReport *report)
{
	return wf_recycle_guarded(path, expected, parent, NULL, NULL, report);
}

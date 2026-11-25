/* Real owned files/IDs and a deterministic shell model. No real Recycle Bin
 * or user registry mutation; this does not replace the shell integration test. */
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "../tools/p0/p0_lab.h"
#include "wf_test.h"
#include "xfer/wf_recycle_win.h"

static P0Lab lab;
static wchar_t source[1200], moved[1200], original[1200];
static WfFileId moved_id;
static HRESULT bin_hr, perform_hr, fake_abort_hr;
static LSTATUS policy_error;
static unsigned shell_calls, abort_calls;
static bool fake_aborted, receipt_error, nested_receipt, attributes_error,
        enumerate_error;
static unsigned policy_mode;

static WfFileId file_id(const wchar_t *path)
{
	WfFileId result = {0};
	FILE_ID_INFO info;
	HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		CHECK(GetFileInformationByHandleEx(
		        h, FileIdInfo, &info, sizeof info));
		result.volume = info.VolumeSerialNumber;
		memcpy(result.id, info.FileId.Identifier, sizeof result.id);
		result.kind = WF_ID_STRONG;
		CloseHandle(h);
	}
	return result;
}
static void put(const wchar_t *path)
{
	DWORD n;
	HANDLE h =
	        CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		CHECK(WriteFile(h, "owned", 5, &n, NULL) && 5 == n);
		CloseHandle(h);
	}
}
static HRESULT WINAPI query_bin(LPCWSTR root, LPSHQUERYRBINFO info)
{
	(void)root;
	(void)info;
	return bin_hr;
}
static LSTATUS WINAPI policy(HKEY key,
        LPCWSTR sub,
        LPCWSTR value,
        DWORD flags,
        LPDWORD type,
        PVOID data,
        LPDWORD size)
{
	(void)key;
	(void)sub;
	(void)flags;
	(void)type;
	(void)size;
	if (policy_mode) {
		*(DWORD *)data =
		        1 == policy_mode && !wcscmp(value, L"NukeOnDelete") ? 1
		                                                            : 0;
		return ERROR_SUCCESS;
	}
	return policy_error;
}
static DWORD WINAPI attributes(LPCWSTR path)
{
	if (attributes_error) {
		SetLastError(ERROR_ACCESS_DENIED);
		return INVALID_FILE_ATTRIBUTES;
	}
	return GetFileAttributesW(path);
}
static BOOL WINAPI find_next(HANDLE search, LPWIN32_FIND_DATAW data)
{
	if (enumerate_error) {
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	return FindNextFileW(search, data);
}
static ULONG STDMETHODCALLTYPE release_op(IFileOperation *op)
{
	(void)op;
	return 0;
}
static HRESULT STDMETHODCALLTYPE set_flags(IFileOperation *op, DWORD flags)
{
	(void)op;
	CHECK((flags & FOF_ALLOWUNDO) &&
	        !(flags & (FOF_NOCONFIRMATION | FOF_SILENT)));
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE delete_item(
        IFileOperation *op, IShellItem *item, IFileOperationProgressSink *sink)
{
	(void)op;
	(void)item;
	(void)sink;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE perform(IFileOperation *op)
{
	(void)op;
	shell_calls++;
	if (FAILED(perform_hr) || fake_aborted) {
		return perform_hr;
	}
	moved_id = file_id(source);
	CHECK(MoveFileW(source, moved));
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE get_aborted(IFileOperation *op, BOOL *value)
{
	(void)op;
	abort_calls++;
	*value = fake_aborted;
	return fake_abort_hr;
}
static IFileOperationVtbl vtable = {.Release = release_op,
        .SetOperationFlags = set_flags,
        .DeleteItem = delete_item,
        .PerformOperations = perform,
        .GetAnyOperationsAborted = get_aborted};
static IFileOperation operation = {&vtable};
static HRESULT WINAPI create_op(REFCLSID cls,
        LPUNKNOWN outer,
        DWORD context,
        REFIID iid,
        LPVOID *result)
{
	(void)cls;
	(void)outer;
	(void)context;
	(void)iid;
	*result = &operation;
	return S_OK;
}
static DWORD WINAPI final_path(HANDLE h, LPWSTR out, DWORD cap, DWORD flags)
{
	FILE_ID_INFO id;
	CHECK(VOLUME_NAME_GUID == flags);
	if (receipt_error) {
		SetLastError(ERROR_ACCESS_DENIED);
		return 0;
	}
	CHECK(GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id));
	if ((moved_id.kind == WF_ID_STRONG) &&
	        moved_id.volume == id.VolumeSerialNumber &&
	        !memcmp(moved_id.id, id.FileId.Identifier,
	                sizeof moved_id.id)) {
		const wchar_t *mock = nested_receipt
		        ? L"\\\\?\\Volume{mock}\\nested\\$Recycle.Bin\\item"
		        : L"\\\\?\\Volume{mock}\\$Recycle.Bin\\sid\\item";
		CHECK(wcslen(mock) + 1 <= cap);
		wcscpy_s(out, cap, mock);
		return (DWORD)wcslen(mock);
	}
	return GetFinalPathNameByHandleW(h, out, cap, flags);
}
#define SHQueryRecycleBinW query_bin
#define RegGetValueW policy
#define CoCreateInstance create_op
#define GetFinalPathNameByHandleW final_path
#define GetFileAttributesW attributes
#define FindNextFileW find_next
#include "../src/xfer/wf_recycle_win.c"
#undef GetFileAttributesW
#undef FindNextFileW

static void swap_before_shell(const wchar_t *path)
{
	CHECK(MoveFileW(path, original));
	put(path); /* impostor at the exact checked name, without a timing race */
}
static void scenario(unsigned number)
{
	wchar_t rel[64];
	swprintf(rel, 64, L"source-%u", number);
	CHECK(p0_lab_path(&lab, rel, source, 1200));
	put(source);
	CHECK(p0_area_track(lab.area, rel, 0));
	swprintf(rel, 64, L"moved-%u", number);
	CHECK(p0_lab_path(&lab, rel, moved, 1200));
	swprintf(rel, 64, L"original-%u", number);
	CHECK(p0_lab_path(&lab, rel, original, 1200));
	bin_hr = perform_hr = fake_abort_hr = S_OK;
	policy_error = ERROR_FILE_NOT_FOUND;
	shell_calls = abort_calls = 0;
	fake_aborted = receipt_error = nested_receipt = attributes_error =
	        false;
	policy_mode = 0;
	memset(&moved_id, 0, sizeof moved_id);
	wf_recycle_test_before_shell = NULL;
}
static void track_results(unsigned number)
{
	wchar_t rel[64];
	if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(moved)) {
		swprintf(rel, 64, L"moved-%u", number);
		CHECK(p0_area_track(lab.area, rel, 0));
	}
	if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(original)) {
		swprintf(rel, 64, L"original-%u", number);
		CHECK(p0_area_track(lab.area, rel, 0));
	}
}
static bool reject_proof(void *ctx)
{
	(void)ctx;
	return false;
}

int main(void)
{
	WfRecycleReport r;
	CHECK(p0_lab_open(&lab, "recycle-faults", 0));
	if (!lab.area) {
		return 1;
	}
	CHECK(wf_recycle_thread_init());
	WfFileId parent = file_id(lab.root);
	for (unsigned i = 0; i < 15; i++) {
		scenario(i);
		WfFileId id = file_id(source);
		if (0 == i) {
			bin_hr = E_ACCESSDENIED;
		}
		if (1 == i) {
			policy_error = ERROR_ACCESS_DENIED;
		}
		if (2 == i) {
			id.id[0] ^= 1;
		}
		if (3 == i) {
			wf_recycle_test_before_shell = swap_before_shell;
		}
		if (4 == i) {
			nested_receipt = true;
		}
		if (5 == i) {
			receipt_error = true;
		}
		if (6 == i) {
			fake_aborted = true;
		}
		if (7 == i) {
			perform_hr = E_FAIL;
			fake_abort_hr = E_ACCESSDENIED;
		}
		if (9 == i) {
			fake_abort_hr = E_ACCESSDENIED;
		}
		if (10 == i) {
			attributes_error = true;
		}
		if (11 == i) {
			policy_mode = 1; /* configured permanent deletion */
		}
		if (12 == i) {
			policy_mode = 2; /* explicit zero capacity */
		}
		WfFileId folder = parent;
		if (13 == i) {
			folder.id[0] ^= 1;
		}
		WfStatus status = i == 14
		        ? wf_recycle_guarded(
		                  source, &id, &folder, reject_proof, NULL, &r)
		        : wf_recycle(source, &id, &folder, &r);
		if (i == 14) {
			CHECK(!r.attempted && r.error == ERROR_RETRY &&
			        !shell_calls);
		}
		if (i <= 2) {
			CHECK(WF_OK != status && !shell_calls);
		}
		if (0 == i) {
			CHECK(WF_RP_BIN_QUERY == r.phase &&
			        E_ACCESSDENIED == r.hresult);
		}
		if (1 == i) {
			CHECK(WF_RP_BIN_POLICY == r.phase &&
			        ERROR_ACCESS_DENIED == r.error);
		}
		if (3 == i) {
			CHECK(WF_E_INVAL == status && r.swapped &&
			        1 == shell_calls);
			WfFileId kept = file_id(original);
			CHECK(wf_fileid_equal(&id, &kept));
		}
		if (4 == i) {
			CHECK(WF_E_INVAL == status && r.swapped);
		}
		if (5 == i) {
			CHECK(WF_E_OS == status && !r.swapped &&
			        ERROR_ACCESS_DENIED == r.error);
		}
		if (6 == i) {
			CHECK(WF_E_OS == status && r.aborted && r.still_there);
		}
		if (7 == i) {
			CHECK(WF_E_OS == status && E_FAIL == r.hresult &&
			        1 == abort_calls);
		}
		if (8 == i) {
			CHECK(WF_OK == status && !r.swapped && !r.still_there &&
			        1 == abort_calls);
		}
		if (9 == i) {
			CHECK(WF_E_OS == status &&
			        E_ACCESSDENIED == r.hresult &&
			        1 == abort_calls);
		}
		if (10 == i) {
			CHECK(WF_E_OS == status &&
			        ERROR_ACCESS_DENIED == r.error && !r.swapped);
		}
		if (i >= 11) {
			CHECK(WF_OK != status && !shell_calls);
		}
		track_results(i);
	}
	size_t budget = 1000000;
	uint64_t bytes = 0;
	enumerate_error = true;
	CHECK(!tree_bytes(lab.root, 0, &budget, &bytes));
	enumerate_error = false;
	wf_recycle_test_before_shell = NULL;
	wf_recycle_thread_done();
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}

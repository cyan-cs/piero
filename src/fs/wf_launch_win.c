#include "wf_launch_win.h"

#include <shellapi.h>
#include <string.h>

#include "../xfer/wf_xfer.h"
#include "wf_weakid_win.h"

static WfStatus error_status(DWORD error)
{
	return ERROR_ACCESS_DENIED == error || ERROR_SHARING_VIOLATION == error
	        ? WF_E_ACCESS
	        : ERROR_FILE_NOT_FOUND == error ||
	                ERROR_PATH_NOT_FOUND == error || SE_ERR_FNF == error ||
	                SE_ERR_PNF == error
	        ? WF_E_NOENT
	        : ERROR_NOT_ENOUGH_MEMORY == error || ERROR_OUTOFMEMORY == error
	        ? WF_E_NOMEM
	        : WF_E_OS;
}

/* The identity of the open file (the weak one on a FAT volume, where the
 * listing could give no other), or false with *error set. Opening changes
 * nothing that exists, and the pin below keeps the file from being replaced
 * while the handler takes it. */
static bool handle_identity(HANDLE h, WfFileId *id, DWORD *error)
{
	return wf_handle_identity_any(h, id, error);
}

/* A plain local file whose identity is the expected one. */
static WfStatus check_object(HANDLE h, const WfFileId *expected, DWORD *error)
{
	DWORD attributes;
	WfFileId actual;

	memset(&actual, 0, sizeof actual);
	if (!handle_identity(h, &actual, error)) {
		return error_status(*error);
	}
	if (!wf_fileid_equal_or_weak(&actual, expected)) {
		return WF_E_INVAL; /* stale: not the object that was listed */
	}
	/* FAT has no FileAttributeTagInfo: the same attributes, the older way */
	if (!wf_handle_attributes(h, &attributes, error)) {
		return error_status(*error);
	}
	if (attributes &
	        (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
		return WF_E_INVAL;
	}
	return WF_OK;
}

WfStatus wf_launch_checked(const wchar_t *path,
        const WfFileId *expected,
        const wchar_t *directory,
        DWORD *error)
{
	SHELLEXECUTEINFOW sei;
	WfStatus st;
	HANDLE pin;
	DWORD ignored = 0;

	if (!error) {
		error = &ignored;
	}
	*error = 0;
	if (!path || !expected || !directory ||
	        !wf_fileid_usable_to_read(expected) || L'\0' == path[0] ||
	        L'\0' == directory[0]) {
		return WF_E_INVAL;
	}
	pin = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (INVALID_HANDLE_VALUE == pin) {
		*error = GetLastError();
		return error_status(*error);
	}
	st = check_object(pin, expected, error);
	if (WF_OK != st) {
		CloseHandle(pin);
		return st;
	}
	memset(&sei, 0, sizeof sei);
	sei.cbSize = sizeof sei;
	/* NOASYNC: the call returns after the handler took the file, so the
	 * pin covers the whole launch. No SEE_MASK_FLAG_NO_UI: the shell's own
	 * security prompts must stay visible. */
	sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_DDEWAIT;
	sei.lpVerb = NULL;
	sei.lpFile = path;
	sei.lpDirectory = directory;
	sei.nShow = SW_SHOWNORMAL;
	if (!ShellExecuteExW(&sei)) {
		*error = GetLastError();
		if (0 == *error) {
			*error = (DWORD)(UINT_PTR)sei.hInstApp;
		}
		st = error_status(*error);
	}
	CloseHandle(pin);
	return st;
}

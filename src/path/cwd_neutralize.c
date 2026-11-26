#include <windows.h>

#include "cwd_neutralize.h"

#include <aclapi.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wf_canonical_win.h"
#include "wf_path.h"

static bool private_acl(HANDLE h, PSID user)
{
	PSID owner = NULL;
	PACL dacl = NULL;
	PSECURITY_DESCRIPTOR sd = NULL;
	SECURITY_DESCRIPTOR_CONTROL control;
	DWORD revision;
	ACCESS_ALLOWED_ACE *ace = NULL;
	DWORD error = WF_PRIVATE_E_NOT_PRIVATE, query;
	bool ok = false;
	query = GetSecurityInfo(h, SE_FILE_OBJECT,
	        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
	        NULL, &dacl, NULL, &sd);
	if (ERROR_SUCCESS != query) {
		error = query;
		goto done;
	}
	if (!GetSecurityDescriptorControl(sd, &control, &revision)) {
		error = GetLastError();
		goto done;
	}
	if (!owner || !EqualSid(owner, user) || !dacl || 1 != dacl->AceCount ||
	        !(control & SE_DACL_PROTECTED)) {
		goto done;
	}
	if (!GetAce(dacl, 0, (void **)&ace)) {
		error = GetLastError();
		goto done;
	}
	if (ACCESS_ALLOWED_ACE_TYPE != ace->Header.AceType ||
	        !EqualSid(&ace->SidStart, user) ||
	        FILE_ALL_ACCESS != (ace->Mask & FILE_ALL_ACCESS)) {
		goto done;
	}
	ok = true;
done:
	if (sd) {
		LocalFree(sd);
	}
	if (!ok) {
		SetLastError(error);
	}
	return ok;
}

static bool directory_empty(const wchar_t *root)
{
	wchar_t *pattern = NULL;
	WIN32_FIND_DATAW entry;
	HANDLE search;
	bool empty = true;
	if (WF_OK != wf_path_join(root, L"*", &pattern)) {
		return 0;
	}
	search = FindFirstFileW(pattern, &entry);
	free(pattern);
	if (INVALID_HANDLE_VALUE == search) {
		return ERROR_FILE_NOT_FOUND == GetLastError();
	}
	do {
		if (wcscmp(entry.cFileName, L".") &&
		        wcscmp(entry.cFileName, L"..")) {
			empty = false;
			break;
		}
	} while (FindNextFileW(search, &entry));
	if (empty && ERROR_NO_MORE_FILES != GetLastError()) {
		empty = false;
	}
	FindClose(search);
	return empty;
}

bool wf_private_directory_open(
        const wchar_t *path, bool empty, HANDLE *pin, DWORD *error)
{
	HANDLE token = NULL, h = INVALID_HANDLE_VALUE;
	TOKEN_USER *user = NULL;
	DWORD bytes = 0;
	wchar_t *sid = NULL, *extended = NULL, sddl[512];
	PSECURITY_DESCRIPTOR sd = NULL;
	SECURITY_ATTRIBUTES security = {sizeof security, NULL, FALSE};
	FILE_ATTRIBUTE_TAG_INFO attributes;
	WfCanonicalPath canonical = {0};
	bool ok = false;
	if (!path || !pin || !error) {
		return 0;
	}
	*pin = INVALID_HANDLE_VALUE;
	*error = ERROR_ACCESS_DENIED;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		goto os_error;
	}
	GetTokenInformation(token, TokenUser, NULL, 0, &bytes);
	if (!bytes) {
		goto os_error;
	}
	if (NULL == (user = malloc(bytes))) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		goto done;
	}
	if (!GetTokenInformation(token, TokenUser, user, bytes, &bytes)) {
		goto os_error;
	}
	if (!IsValidSid(user->User.Sid)) {
		*error = ERROR_INVALID_SID;
		goto done;
	}
	if (!ConvertSidToStringSidW(user->User.Sid, &sid)) {
		goto os_error;
	}
	/* TokenOwner can be an administrators group. New state belongs to the
	 * actual user; existing objects are only inspected, never repaired. */
	if (swprintf(sddl, sizeof sddl / sizeof *sddl, L"O:%lsD:P(A;;FA;;;%ls)",
	            sid, sid) < 0) {
		*error = ERROR_INSUFFICIENT_BUFFER;
		goto done;
	}
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
	            sddl, SDDL_REVISION_1, &sd, NULL)) {
		goto os_error;
	}
	security.lpSecurityDescriptor = sd;
	if (WF_OK != wf_path_to_extended(path, &extended)) {
		goto done;
	}
	if (!CreateDirectoryW(extended, &security) &&
	        ERROR_ALREADY_EXISTS != GetLastError()) {
		goto os_error;
	}
	h = CreateFileW(extended,
	        READ_CONTROL | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == h) {
		goto os_error;
	}
	if (!GetFileInformationByHandleEx(
	            h, FileAttributeTagInfo, &attributes, sizeof attributes)) {
		goto os_error;
	}
	if (!(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
		*error = ERROR_DIRECTORY;
		goto done;
	}
	if (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		*error = ERROR_REPARSE_POINT_ENCOUNTERED;
		goto done;
	}
	if (!private_acl(h, user->User.Sid)) {
		*error = GetLastError();
		goto done;
	}
	if (!wf_canonical_from_handle(h, &canonical, error)) {
		goto done;
	}
	if (empty && !directory_empty(canonical.path)) {
		*error = ERROR_DIR_NOT_EMPTY;
		goto done;
	}
	*pin = h;
	h = INVALID_HANDLE_VALUE;
	*error = 0;
	ok = true;
	goto done;
os_error:
	*error = GetLastError();
done:
	wf_canonical_free(&canonical);
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	if (token) {
		CloseHandle(token);
	}
	if (sd) {
		LocalFree(sd);
	}
	if (sid) {
		LocalFree(sid);
	}
	free(user);
	free(extended);
	return ok;
}
bool wf_cwd_neutralize(const wchar_t *path, HANDLE *pin, DWORD *error)
{
	WfCanonicalPath canonical = {0};
	if (!wf_private_directory_open(path, true, pin, error)) {
		return 0;
	}
	if (!wf_canonical_from_handle(*pin, &canonical, error) ||
	        !canonical.path) {
		goto fail;
	}
	if (!SetCurrentDirectoryW(canonical.path)) {
		*error = GetLastError();
		goto fail;
	}
	wf_canonical_free(&canonical);
	return 1;
fail:
	wf_canonical_free(&canonical);
	CloseHandle(*pin);
	*pin = INVALID_HANDLE_VALUE;
	return 0;
}

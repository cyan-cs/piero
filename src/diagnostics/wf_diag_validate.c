#include <windows.h>

#include <aclapi.h>
#include <stdlib.h>
#include <string.h>

#include "wf_diag_internal.h"

/* Operands may be members of the packed on-disk structs (unaligned): copy out.
 */
bool wf_diag_equal_id(const void *a, const void *b)
{
	FILE_ID_INFO x, y;
	memcpy(&x, a, sizeof x);
	memcpy(&y, b, sizeof y);
	return x.VolumeSerialNumber == y.VolumeSerialNumber &&
	        !memcmp(x.FileId.Identifier, y.FileId.Identifier, 16);
}

bool wf_diag_nonzero(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (p[i]) {
			return 1;
		}
	}
	return 0;
}

bool wf_diag_plain(HANDLE h,
        bool directory,
        uint64_t *bytes,
        FILE_ID_INFO *id,
        DWORD *error)
{
	FILE_STANDARD_INFO standard;
	FILE_ATTRIBUTE_TAG_INFO tag;
	if (!GetFileInformationByHandleEx(h, FileIdInfo, id, sizeof *id) ||
	        !GetFileInformationByHandleEx(
	                h, FileStandardInfo, &standard, sizeof standard) ||
	        !GetFileInformationByHandleEx(
	                h, FileAttributeTagInfo, &tag, sizeof tag)) {
		*error = GetLastError();
		return 0;
	}
	if (!!standard.Directory != directory || standard.DeletePending ||
	        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
	        (!directory &&
	                (1 != standard.NumberOfLinks ||
	                        standard.EndOfFile.QuadPart < 0))) {
		*error = ERROR_INVALID_DATA;
		return 0;
	}
	if (bytes) {
		*bytes = (uint64_t)standard.EndOfFile.QuadPart;
	}
	*error = 0;
	return 1;
}

static bool private_acl(HANDLE directory, DWORD *error)
{
	HANDLE token = NULL;
	TOKEN_USER *user = NULL;
	DWORD count = 0;
	PSECURITY_DESCRIPTOR sd = NULL;
	PSID owner = NULL;
	PACL dacl = NULL;
	SECURITY_DESCRIPTOR_CONTROL control;
	DWORD revision;
	ACCESS_ALLOWED_ACE *ace = NULL;
	bool ok = false;
	DWORD status;
	*error = ERROR_ACCESS_DENIED;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		*error = GetLastError();
		goto done;
	}
	if (!GetTokenInformation(token, TokenUser, NULL, 0, &count)) {
		status = GetLastError();
		if (ERROR_INSUFFICIENT_BUFFER != status) {
			*error = status;
			goto done;
		}
	}
	if (!count || !(user = malloc(count))) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		goto done;
	}
	if (!GetTokenInformation(token, TokenUser, user, count, &count)) {
		*error = GetLastError();
		goto done;
	}
	status = GetSecurityInfo(directory, SE_FILE_OBJECT,
	        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
	        NULL, &dacl, NULL, &sd);
	if (ERROR_SUCCESS != status) {
		*error = status;
		goto done;
	}
	if (!owner || !EqualSid(owner, user->User.Sid) || !dacl ||
	        1 != dacl->AceCount) {
		goto done;
	}
	if (!GetSecurityDescriptorControl(sd, &control, &revision)) {
		*error = GetLastError();
		goto done;
	}
	if (!(control & SE_DACL_PROTECTED)) {
		goto done;
	}
	if (!GetAce(dacl, 0, (void **)&ace)) {
		*error = GetLastError();
		goto done;
	}
	if (ACCESS_ALLOWED_ACE_TYPE != ace->Header.AceType ||
	        !EqualSid(&ace->SidStart, user->User.Sid) ||
	        FILE_ALL_ACCESS != (ace->Mask & FILE_ALL_ACCESS)) {
		goto done;
	}
	ok = true;
	*error = 0;
done:
	if (sd) {
		LocalFree(sd);
	}
	if (token) {
		CloseHandle(token);
	}
	free(user);
	return ok;
}

bool wf_diag_exact_path(HANDLE h, const wchar_t *expected, DWORD *error)
{
	WfCanonicalPath actual = {0};
	bool ok = wf_canonical_from_handle(h, &actual, error);
	if (ok && (!actual.path || wcscmp(actual.path, expected))) {
		*error = ERROR_INVALID_DATA;
		ok = false;
	}
	wf_canonical_free(&actual);
	return ok;
}

bool wf_diag_known_entries(WfDiagnostics *log, bool empty, DWORD *error)
{
	wchar_t *pattern = wf_diag_path_of(log, L"*");
	WIN32_FIND_DATAW entry;
	HANDLE search;
	bool ok = true;
	DWORD saved;
	if (!pattern) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	search = FindFirstFileW(pattern, &entry);
	saved = GetLastError();
	free(pattern);
	if (INVALID_HANDLE_VALUE == search) {
		*error = ERROR_FILE_NOT_FOUND == saved ? 0 : saved;
		return ERROR_FILE_NOT_FOUND == saved;
	}
	do {
		if (!wcscmp(entry.cFileName, L".") ||
		        !wcscmp(entry.cFileName, L"..")) {
			continue;
		}
		if (empty ||
		        (wcscmp(entry.cFileName, WF_DIAG_MARKER_NAME) &&
		                wcscmp(entry.cFileName, L"log0.wfl") &&
		                wcscmp(entry.cFileName, L"log1.wfl") &&
		                wcscmp(entry.cFileName, L"log2.wfl"))) {
			ok = false;
			*error = ERROR_INVALID_DATA;
			break;
		}
	} while (FindNextFileW(search, &entry));
	if (ok) {
		saved = GetLastError();
		if (ERROR_NO_MORE_FILES != saved) {
			*error = saved;
			ok = false;
		}
	}
	FindClose(search);
	if (ok) {
		*error = 0;
	}
	return ok;
}

bool wf_diag_root_valid(WfDiagnostics *log, DWORD *error)
{
	FILE_ID_INFO id;
	uint64_t bytes;
	if (!wf_diag_plain(log->root, true, NULL, &id, error)) {
		return 0;
	}
	if (!wf_diag_equal_id(&id, &log->root_id)) {
		*error = ERROR_INVALID_DATA;
		return 0;
	}
	if (!private_acl(log->root, error) ||
	        !wf_diag_exact_path(log->root, log->canonical.path, error) ||
	        !wf_diag_plain(log->marker, false, &bytes, &id, error)) {
		return 0;
	}
	if (!wf_diag_equal_id(&id, &log->marker_id) ||
	        bytes !=
	                sizeof(WfDiagDiskMarker) +
	                        wcslen(log->canonical.path) * sizeof(wchar_t)) {
		*error = ERROR_INVALID_DATA;
		return 0;
	}
	return wf_diag_known_entries(log, false, error);
}

bool wf_diag_read_file(
        HANDLE h, unsigned char **out, size_t *count, DWORD *error)
{
	FILE_ID_INFO id;
	uint64_t n;
	DWORD got;
	LARGE_INTEGER zero = {0};
	*out = NULL;
	if (!wf_diag_plain(h, false, &n, &id, error)) {
		return 0;
	}
	if (n > WF_DIAG_FILE_BYTES) {
		*error = ERROR_INVALID_DATA;
		return 0;
	}
	*out = malloc(n ? (size_t)n : 1);
	if (!*out) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	if (!SetFilePointerEx(h, zero, NULL, FILE_BEGIN) ||
	        !ReadFile(h, *out, (DWORD)n, &got, NULL)) {
		*error = GetLastError();
		free(*out);
		*out = NULL;
		return 0;
	}
	if (got != n) {
		*error = ERROR_READ_FAULT;
		free(*out);
		*out = NULL;
		return 0;
	}
	*count = (size_t)n;
	*error = 0;
	return 1;
}

static bool records_valid(const unsigned char *bytes, size_t size, DWORD *error)
{
	if (!wf_diag_records_valid(bytes, size)) {
		*error = ERROR_INVALID_DATA;
		return 0;
	}
	while (size) {
		WfDiagDiskRecord record;
		memcpy(&record, bytes, sizeof record);
		bytes += sizeof record;
		size -= sizeof record;
		if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		            (const char *)bytes, (int)record.bytes, NULL, 0)) {
			*error = GetLastError();
			return 0;
		}
		for (size_t i = 0; i + 1 < record.bytes; i++) {
			if (bytes[i] < 32 || 127 == bytes[i]) {
				*error = ERROR_INVALID_DATA;
				return 0;
			}
		}
		bytes += record.bytes;
		size -= record.bytes;
	}
	return 1;
}

bool wf_diag_header_read(WfDiagnostics *log,
        HANDLE file,
        unsigned slot,
        LogSlot *result,
        DWORD *error)
{
	unsigned char *bytes = NULL;
	size_t n = 0, header_bytes;
	FILE_ID_INFO id;
	WfDiagDiskHeader h;
	bool ok = false;
	wchar_t *path = wf_diag_slot_path(log, slot);
	if (!path) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	if (!wf_diag_read_file(file, &bytes, &n, error)) {
		goto done;
	}
	if (n < sizeof h) {
		*error = ERROR_INVALID_DATA;
		goto done;
	}
	memcpy(&h, bytes, sizeof h);
	header_bytes = sizeof h + (size_t)h.path_units * sizeof(wchar_t);
	if (!wf_diag_plain(file, false, NULL, &id, error) ||
	        !wf_diag_exact_path(file, path, error)) {
		goto done;
	}
	if (memcmp(h.magic, "WFDLOG01", 8) || h.reserved || !h.generation ||
	        h.slot != slot || slot != (h.generation - 1) % WF_DIAG_SLOTS ||
	        !wf_diag_equal_id(&h.root, &log->root_id) ||
	        !wf_diag_equal_id(&h.marker, &log->marker_id) ||
	        !wf_diag_equal_id(&h.object, &id) ||
	        memcmp(h.root_nonce, log->nonce, 32) ||
	        !wf_diag_nonzero(h.object_nonce, 32) ||
	        h.path_units != wcslen(log->canonical.path) ||
	        h.path_units > WF_DIAG_PATH_UNITS || header_bytes > n ||
	        memcmp(bytes + sizeof h, log->canonical.path,
	                (size_t)h.path_units * sizeof(wchar_t)) ||
	        h.crc32 != wf_diag_header_crc(&h, log->canonical.path)) {
		*error = ERROR_INVALID_DATA;
		goto done;
	}
	if (!records_valid(bytes + header_bytes, n - header_bytes, error)) {
		goto done;
	}
	result->present = true;
	result->header = h;
	result->bytes = n;
	ok = true;
done:
	if (!ok && !*error) {
		*error = ERROR_INVALID_DATA;
	}
	free(bytes);
	free(path);
	return ok;
}

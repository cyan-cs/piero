#include "p0_source_guard.h"

#include <stdlib.h>
#include <string.h>

#include "../../src/path/wf_path.h"

P0GuardResult p0_source_guard_acquire(const wchar_t *path,
        const WfNodeInfo *snapshot,
        P0SourceGuard *out,
        DWORD *error)
{
	FILE_ID_INFO id;
	FILE_STANDARD_INFO standard;
	FILE_BASIC_INFO basic;
	HANDLE handle = INVALID_HANDLE_VALUE;
	wchar_t *extended = NULL;
	const uint8_t zero[16] = {0};
	P0GuardResult result = P0_SOURCE_BUSY;
	WfStatus status;
	if (!out || !error) {
		return P0_SOURCE_BUSY;
	}
	out->handle = INVALID_HANDLE_VALUE;
	memset(&out->identity, 0, sizeof out->identity);
	*error = ERROR_INVALID_PARAMETER;
	if (!snapshot || !path) {
		return result;
	}
	if (snapshot->flags & (WF_ENT_DIR | WF_ENT_LINK | WF_ENT_CLOUD)) {
		*error = ERROR_NOT_SUPPORTED;
		return P0_SOURCE_UNSUPPORTED;
	}
	if (!(snapshot->id.kind == WF_ID_STRONG) ||
	        !memcmp(snapshot->id.id, zero, 16)) {
		*error = ERROR_NOT_SUPPORTED;
		return result;
	}
	status = wf_path_to_extended(path, &extended);
	if (WF_OK != status) {
		*error = WF_E_NOMEM == status ? ERROR_NOT_ENOUGH_MEMORY
		                              : ERROR_INVALID_NAME;
		goto done;
	}
	handle = CreateFileW(extended, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (INVALID_HANDLE_VALUE == handle) {
		*error = GetLastError();
		goto done;
	}
	if (!GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof id) ||
	        !GetFileInformationByHandleEx(
	                handle, FileStandardInfo, &standard, sizeof standard) ||
	        !GetFileInformationByHandleEx(
	                handle, FileBasicInfo, &basic, sizeof basic)) {
		*error = GetLastError();
		goto done;
	}
	*error = 0;
	result = P0_SOURCE_CHANGED;
	if (id.VolumeSerialNumber != snapshot->id.volume ||
	        memcmp(id.FileId.Identifier, snapshot->id.id, 16) ||
	        standard.Directory || standard.DeletePending ||
	        standard.EndOfFile.QuadPart < 0 ||
	        (basic.FileAttributes &
	                (FILE_ATTRIBUTE_DIRECTORY |
	                        FILE_ATTRIBUTE_REPARSE_POINT |
	                        FILE_ATTRIBUTE_OFFLINE |
	                        FILE_ATTRIBUTE_RECALL_ON_OPEN |
	                        FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) ||
	        (uint64_t)standard.EndOfFile.QuadPart != snapshot->size ||
	        (uint64_t)basic.LastWriteTime.QuadPart != snapshot->mtime) {
		goto done;
	}
	out->handle = handle;
	handle = INVALID_HANDLE_VALUE;
	out->identity = snapshot->id;
	result = P0_GUARD_OK;
done:
	if (INVALID_HANDLE_VALUE != handle) {
		CloseHandle(handle);
	}
	free(extended);
	return result;
}
void p0_source_guard_release(P0SourceGuard *guard)
{
	if (!guard) {
		return;
	}
	if (INVALID_HANDLE_VALUE != guard->handle) {
		CloseHandle(guard->handle);
	}
	guard->handle = INVALID_HANDLE_VALUE;
	memset(&guard->identity, 0, sizeof guard->identity);
}
const char *p0_source_guard_reason(P0GuardResult result)
{
	switch (result) {
	case P0_GUARD_OK:
		return "source guarded until release; pathname TOCTOU remains";
	case P0_SOURCE_BUSY:
		return "SOURCE_BUSY: cannot acquire/query the read-only "
		       "sharing guard";
	case P0_SOURCE_CHANGED:
		return "SOURCE_CHANGED: identity, type, size or timestamp "
		       "differs from Transfer Slot";
	case P0_SOURCE_UNSUPPORTED:
		return "source kind requires a separate "
		       "directory/cloud/reparse policy";
	}
	return "SOURCE_BUSY: unknown guard result";
}

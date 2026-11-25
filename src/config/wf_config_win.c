#include "wf_config_win.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"
#include "../base/wf_mem.h"

static WfStatus io_status(DWORD error)
{
	return ERROR_FILE_NOT_FOUND == error || ERROR_PATH_NOT_FOUND == error
	        ? WF_E_NOENT
	        : ERROR_ACCESS_DENIED == error ||
	                ERROR_SHARING_VIOLATION == error
	        ? WF_E_ACCESS
	        : WF_E_OS;
}

WfStatus wf_config_read(WfConfig *config,
        const wchar_t *path,
        bool optional,
        WfConfigDiagnostic diagnostic,
        void *context,
        WfConfigReport *report,
        DWORD *error)
{
	HANDLE file = INVALID_HANDLE_VALUE;
	FILE_STANDARD_INFO size;
	FILE_ATTRIBUTE_TAG_INFO tag;
	wchar_t *extended = NULL;
	BYTE *bytes = NULL;
	DWORD got;
	WfStatus status;
	if (!config || !path || !report || !error) {
		return WF_E_INVAL;
	}
	wf_config_dispose(config);
	memset(report, 0, sizeof *report);
	*error = 0;
	status = wf_path_to_extended(path, &extended);
	if (WF_OK != status) {
		goto done;
	}
	file = CreateFileW(extended, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, NULL);
	if (INVALID_HANDLE_VALUE == file) {
		*error = GetLastError();
		status = io_status(*error);
		if (optional && WF_E_NOENT == status) {
			status = WF_OK;
			*error = 0;
		}
		goto done;
	}
	if (FILE_TYPE_DISK != GetFileType(file)) {
		status = WF_E_INVAL;
		goto done;
	}
	if (!GetFileInformationByHandleEx(
	            file, FileStandardInfo, &size, sizeof size) ||
	        !GetFileInformationByHandleEx(
	                file, FileAttributeTagInfo, &tag, sizeof tag)) {
		*error = GetLastError();
		status = io_status(*error);
		goto done;
	}
	if (size.Directory || size.DeletePending ||
	        (tag.FileAttributes &
	                (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
	                        FILE_ATTRIBUTE_RECALL_ON_OPEN |
	                        FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))) {
		status = WF_E_ACCESS;
		goto done;
	}
	if (size.EndOfFile.QuadPart < 0 ||
	        (uint64_t)size.EndOfFile.QuadPart > WF_CONFIG_MAX_BYTES) {
		status = WF_E_RANGE;
		goto done;
	}
	bytes = malloc((size_t)size.EndOfFile.QuadPart + 1);
	if (!bytes) {
		status = WF_E_NOMEM;
		goto done;
	}
	if (!ReadFile(
	            file, bytes, (DWORD)size.EndOfFile.QuadPart, &got, NULL)) {
		*error = GetLastError();
		status = io_status(*error);
		goto done;
	}
	if (got != (DWORD)size.EndOfFile.QuadPart) {
		*error = ERROR_READ_FAULT;
		status = WF_E_OS;
		goto done;
	}
	bytes[got] = 0;
	status = wf_config_parse(
	        config, bytes, got, diagnostic, context, report);
done:
	if (INVALID_HANDLE_VALUE != file) {
		CloseHandle(file);
	}
	free(bytes);
	free(extended);
	return status;
}
WfStatus wf_startup_absolute(const wchar_t *path, wchar_t **out, DWORD *error)
{
	DWORD need, got;
	wchar_t *absolute, *extended = NULL;
	WfStatus status;
	if (!out || !error) {
		return WF_E_INVAL;
	}
	*out = NULL;
	*error = 0;
	if (!path || !*path) {
		return WF_E_INVAL;
	}
	need = GetFullPathNameW(path, 0, NULL, NULL);
	if (!need) {
		*error = GetLastError();
		return io_status(*error);
	}
	if (need > 32768) {
		return WF_E_RANGE;
	}
	absolute = wf_malloc_array((size_t)need, sizeof *absolute);
	if (!absolute) {
		return WF_E_NOMEM;
	}
	got = GetFullPathNameW(path, need, absolute, NULL);
	if (!got || got >= need) {
		*error = got ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
		free(absolute);
		return WF_E_OS;
	}
	status = wf_path_to_extended(absolute, &extended);
	free(extended);
	if (WF_OK == status) {
		*out = absolute;
	} else {
		free(absolute);
	}
	return status;
}
WfStatus wf_startup_env_path(const wchar_t *variable,
        const wchar_t *suffix,
        wchar_t **out,
        DWORD *error)
{
	DWORD need, got;
	size_t tail;
	wchar_t *path;
	WfStatus status;
	if (!out || !error || !variable || !suffix) {
		return WF_E_INVAL;
	}
	*out = NULL;
	*error = 0;
	need = GetEnvironmentVariableW(variable, NULL, 0);
	if (!need) {
		*error = GetLastError();
		return WF_E_NOENT;
	}
	tail = wcslen(suffix);
	if (need >= 32768 || tail > 32767u - need) {
		return WF_E_RANGE;
	}
	path = wf_malloc_array(((size_t)need + tail + 1), sizeof *path);
	if (!path) {
		return WF_E_NOMEM;
	}
	got = GetEnvironmentVariableW(variable, path, need);
	if (!got || got >= need) {
		*error = GetLastError();
		free(path);
		return WF_E_OS;
	}
	if (tail) {
		path[got++] = L'\\';
		memcpy(path + got, suffix, (tail + 1) * sizeof *suffix);
	}
	status = wf_startup_absolute(path, out, error);
	free(path);
	return status;
}

static WfStatus state_suffix(
        const wchar_t *name, const wchar_t *rest, wchar_t **suffix)
{
	size_t n = wcslen(name), m = rest ? wcslen(rest) : 0;
	wchar_t *s;

	if (n > 64 || m > 32000u) {
		return WF_E_RANGE;
	}
	s = wf_malloc_array(n + m + 2, sizeof *s);
	if (!s) {
		return WF_E_NOMEM;
	}
	memcpy(s, name, n * sizeof *s);
	if (m) {
		s[n++] = L'\\';
		memcpy(s + n, rest, m * sizeof *s);
	}
	s[n + m] = L'\0';
	*suffix = s;
	return WF_OK;
}

WfStatus wf_startup_state_path(const wchar_t *variable,
        const wchar_t *rest,
        wchar_t **out,
        WfStateKind *kind,
        DWORD *error)
{
	wchar_t *base = NULL, *suffix = NULL;
	WfStateKind found;
	WfStatus status;

	if (!out || !error || !variable) {
		return WF_E_INVAL;
	}
	*out = NULL;
	status = wf_startup_env_path(variable, L"", &base, error);
	if (WF_OK != status) {
		return status;
	}
	status = wf_state_classify(base, &found, error);
	free(base);
	if (WF_OK != status) {
		return status;
	}
	status = state_suffix(wf_state_name(found), rest, &suffix);
	if (WF_OK != status) {
		return status;
	}
	status = wf_startup_env_path(variable, suffix, out, error);
	free(suffix);
	if (WF_OK == status && kind) {
		*kind = found;
	}
	return status;
}

WfStatus wf_startup_legacy_path(const wchar_t *variable,
        const wchar_t *rest,
        wchar_t **out,
        DWORD *error)
{
	wchar_t *suffix = NULL;
	WfStatus status = state_suffix(wf_state_legacy_name(), rest, &suffix);

	if (WF_OK != status) {
		return status;
	}
	status = wf_startup_env_path(variable, suffix, out, error);
	free(suffix);
	return status;
}

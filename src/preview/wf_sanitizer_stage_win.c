#include "wf_sanitizer_stage_win.h"
#include "wf_sanitizer_stage_config.h"
#include <bcrypt.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RUNTIME_MAX_BYTES (64u << 20)
#define PATH_UNITS (MAX_PATH * 2)

static HANDLE pin_directory(const wchar_t *path)
{
	HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	BY_HANDLE_FILE_INFORMATION info;
	if (h == INVALID_HANDLE_VALUE) {
		return h;
	}
	if (!GetFileInformationByHandle(h, &info) ||
	        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
	        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
		CloseHandle(h);
		SetLastError(ERROR_ACCESS_DENIED);
		return INVALID_HANDLE_VALUE;
	}
	return h;
}

static bool regular_file(HANDLE h, uint64_t *size)
{
	BY_HANDLE_FILE_INFORMATION info;
	if (GetFileType(h) != FILE_TYPE_DISK ||
	        !GetFileInformationByHandle(h, &info) ||
	        (info.dwFileAttributes &
	                (FILE_ATTRIBUTE_DIRECTORY |
	                        FILE_ATTRIBUTE_REPARSE_POINT)) ||
	        info.nNumberOfLinks != 1) {
		SetLastError(ERROR_ACCESS_DENIED);
		return false;
	}
	*size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
	return true;
}

static bool content_matches(HANDLE h, const unsigned char *data, DWORD size)
{
	unsigned char buffer[4096];
	DWORD offset = 0;
	uint64_t actual;
	LARGE_INTEGER zero = {0};
	if (!regular_file(h, &actual) || actual != size ||
	        !SetFilePointerEx(h, zero, NULL, FILE_BEGIN)) {
		SetLastError(ERROR_INVALID_DATA);
		return false;
	}
	while (offset < size) {
		DWORD got = 0, want = size - offset;
		if (want > sizeof buffer) {
			want = sizeof buffer;
		}
		if (!ReadFile(h, buffer, want, &got, NULL)) {
			return false;
		}
		if (got != want || memcmp(buffer, data + offset, got)) {
			SetLastError(ERROR_INVALID_DATA);
			return false;
		}
		offset += got;
	}
	return regular_file(h, &actual) && actual == size;
}

static bool ensure_file(
        const wchar_t *path, const unsigned char *data, DWORD size)
{
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	bool ok;
	if (h != INVALID_HANDLE_VALUE) {
		ok = content_matches(h, data, size);
	} else {
		if (GetLastError() != ERROR_FILE_NOT_FOUND) {
			return false;
		}
		h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
		        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		if (h == INVALID_HANDLE_VALUE) {
			return false;
		}
		DWORD put = 0;
		ok = WriteFile(h, data, size, &put, NULL) && put == size &&
		        FlushFileBuffers(h) && content_matches(h, data, size);
	}
	DWORD error = ok ? 0 : GetLastError();
	CloseHandle(h);
	if (!ok) {
		/* Partial/foreign files are retained; no retry may truncate them. */
		SetLastError(error ? error : ERROR_WRITE_FAULT);
	}
	return ok;
}

static unsigned char *runtime_bytes(DWORD *size)
{
	wchar_t path[PATH_UNITS];
	HMODULE module = GetModuleHandleW(WF_SANITIZER_DLL);
	DWORD n = module ? GetModuleFileNameW(module, path, PATH_UNITS) : 0;
	if (!n || n >= PATH_UNITS) {
		SetLastError(ERROR_BAD_ENVIRONMENT);
		return NULL;
	}
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		return NULL;
	}
	uint64_t length;
	unsigned char *data = NULL;
	BCRYPT_ALG_HANDLE algorithm = NULL;
	DWORD error = ERROR_INVALID_DATA, got = 0;
	if (!regular_file(h, &length) || !length ||
	        length > RUNTIME_MAX_BYTES) {
		goto done;
	}
	*size = (DWORD)length;
	data = malloc(*size);
	if (!data) {
		error = ERROR_NOT_ENOUGH_MEMORY;
		goto done;
	}
	if (!ReadFile(h, data, *size, &got, NULL)) {
		error = GetLastError();
		goto done;
	}
	if (got != *size) {
		error = ERROR_READ_FAULT;
		goto done;
	}
	unsigned char digest[32];
	char hex[65];
	if (BCryptOpenAlgorithmProvider(
	            &algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0 ||
	        BCryptHash(algorithm, NULL, 0, data, *size, digest,
	                sizeof digest) != 0) {
		goto done;
	}
	for (size_t i = 0; i < sizeof digest; i++) {
		snprintf(hex + 2 * i, 3, "%02x", digest[i]);
	}
	if (!strcmp(hex, WF_SANITIZER_SHA256)) {
		error = ERROR_SUCCESS;
	}
done:
	if (algorithm) {
		BCryptCloseAlgorithmProvider(algorithm, 0);
	}
	CloseHandle(h);
	if (error) {
		free(data);
		data = NULL;
		SetLastError(error);
	}
	return data;
}

bool wf_sanitizer_stage(const wchar_t *directory, DWORD *error)
{
	wchar_t assembly[PATH_UNITS], dll[PATH_UNITS], manifest[PATH_UNITS];
	HANDLE parent = INVALID_HANDLE_VALUE, child = INVALID_HANDLE_VALUE;
	unsigned char *data = NULL;
	DWORD size = 0, failure = 0;
	bool ok = false;
	if (error) {
		*error = 0;
	}
	if (!directory || wcslen(directory) + 128 >= PATH_UNITS) {
		SetLastError(ERROR_FILENAME_EXCED_RANGE);
		goto done;
	}
	parent = pin_directory(directory);
	if (parent == INVALID_HANDLE_VALUE) {
		goto done;
	}
	swprintf(assembly, PATH_UNITS, L"%ls\\piero.asan.runtime", directory);
	swprintf(dll, PATH_UNITS, L"%ls\\%ls", assembly, WF_SANITIZER_DLL);
	swprintf(manifest, PATH_UNITS, L"%ls\\piero.asan.runtime.manifest",
	        assembly);
	if (!CreateDirectoryW(assembly, NULL) &&
	        GetLastError() != ERROR_ALREADY_EXISTS) {
		goto done;
	}
	child = pin_directory(assembly);
	if (child == INVALID_HANDLE_VALUE) {
		goto done;
	}
	data = runtime_bytes(&size);
	if (!data || !ensure_file(dll, data, size) ||
	        !ensure_file(manifest,
	                (const unsigned char *)wf_sanitizer_manifest,
	                sizeof wf_sanitizer_manifest - 1)) {
		goto done;
	}
	ok = true;
done:
	failure = ok ? 0 : GetLastError();
	if (!ok && !failure) {
		failure = ERROR_GEN_FAILURE;
	}
	free(data);
	if (child != INVALID_HANDLE_VALUE) {
		CloseHandle(child);
	}
	if (parent != INVALID_HANDLE_VALUE) {
		CloseHandle(parent);
	}
	if (error) {
		*error = failure;
	}
	if (!ok) {
		SetLastError(failure);
	}
	return ok;
}

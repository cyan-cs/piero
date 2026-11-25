#ifndef WF_FINAL_PATH_FAULT_H
#define WF_FINAL_PATH_FAULT_H

#include <windows.h>
#include <wchar.h>

typedef enum
{
	FINAL_PATH_REAL,
	FINAL_PATH_OK,
	FINAL_PATH_QUERY_FAILED,
	FINAL_PATH_READ_FAILED,
	FINAL_PATH_GREW
} FinalPathFault;

static FinalPathFault final_path_fault;

static DWORD WINAPI fault_final_path(
        HANDLE file, LPWSTR path, DWORD capacity, DWORD flags)
{
	static const wchar_t value[] = L"\\\\?\\Volume{test}\\file.txt";
	DWORD length = (DWORD)(sizeof value / sizeof *value - 1);

	if (FINAL_PATH_REAL == final_path_fault) {
		return GetFinalPathNameByHandleW(file, path, capacity, flags);
	}
	if (!path) {
		if (FINAL_PATH_QUERY_FAILED == final_path_fault) {
			SetLastError(ERROR_ACCESS_DENIED);
			return 0;
		}
		return length + 1;
	}
	if (FINAL_PATH_GREW == final_path_fault) {
		SetLastError(
		        ERROR_CRC); /* a successful size result must ignore this */
		return capacity + 1;
	}
	if (capacity <= length) {
		return length + 1;
	}
	wmemcpy(path, value, length + 1);
	if (FINAL_PATH_READ_FAILED == final_path_fault) {
		/* Failure leaves no usable string, even if the buffer looks valid. */
		SetLastError(ERROR_CRC);
		return 0;
	}
	return length;
}

#endif

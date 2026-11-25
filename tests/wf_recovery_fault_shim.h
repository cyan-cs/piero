#ifndef WF_RECOVERY_FAULT_SHIM_H
#define WF_RECOVERY_FAULT_SHIM_H
#include <windows.h>

#include <stdlib.h>
HANDLE WINAPI fault_open(
        LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
BOOL WINAPI fault_write(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
BOOL WINAPI fault_read(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
BOOL WINAPI fault_query(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
BOOL WINAPI fault_flush(HANDLE);
BOOL WINAPI fault_disposition(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
#define CreateFileW fault_open
#define WriteFile fault_write
#define ReadFile fault_read
#define GetFileInformationByHandleEx fault_query
#define FlushFileBuffers fault_flush
#define SetFileInformationByHandle fault_disposition
#endif

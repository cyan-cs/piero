#ifndef WF_OWNED_FAULT_SHIM_H
#define WF_OWNED_FAULT_SHIM_H
#include <windows.h>

#include <stdlib.h>
/* Test-only forced include for separately compiled proof modules. The driver
 * implements these functions before enabling its own Win32 interception. */
BOOL test_setinfo(HANDLE, FILE_INFO_BY_HANDLE_CLASS, void *, DWORD);
BOOL test_getinfo(HANDLE, FILE_INFO_BY_HANDLE_CLASS, void *, DWORD);
DWORD test_finalpath(HANDLE, wchar_t *, DWORD, DWORD);
BOOL test_volume(
        HANDLE, wchar_t *, DWORD, DWORD *, DWORD *, DWORD *, wchar_t *, DWORD);
HANDLE test_createfile(const wchar_t *,
        DWORD,
        DWORD,
        SECURITY_ATTRIBUTES *,
        DWORD,
        DWORD,
        HANDLE);
void test_sleep(DWORD);
void *test_malloc(size_t);
void *test_calloc(size_t, size_t);
#define SetFileInformationByHandle test_setinfo
#define GetFileInformationByHandleEx test_getinfo
#define GetFinalPathNameByHandleW test_finalpath
#define GetVolumeInformationByHandleW test_volume
#define CreateFileW test_createfile
#define Sleep test_sleep
#define malloc test_malloc
#define calloc test_calloc
#endif

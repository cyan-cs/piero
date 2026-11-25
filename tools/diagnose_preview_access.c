/* Diagnostic-only builds of the existing access boundaries, with API logging.
 * No policy checks are bypassed. Stage mode uses an explicit private LOCALAPPDATA
 * supplied by the caller; it never changes workspace or repository ACLs. */
#include <windows.h>
#include <aclapi.h>
#ifdef WF_DIAG_STAGE
#	include "p0/p0_lab.h"
#	include <userenv.h>
#endif
#include <stdio.h>

#ifdef WF_DIAG_CLIENT_ONLY
#	include <stdarg.h>
static void trace_bytes(const char *data)
{
	wchar_t path[1024];
	FILE *file = NULL;
	DWORD n = GetEnvironmentVariableW(L"WF_SIXEL_ACCESS_LOG", path, 1024);
	if (n && n < 1024 && !_wfopen_s(&file, path, L"ab") && file) {
		fputs(data, file);
		fclose(file);
	}
}
#	if defined(__clang__)
static int trace_printf(const char *format, ...)
        __attribute__((format(printf, 1, 2)));
#	endif
static int trace_printf(const char *format, ...)
{
	char data[4096];
	va_list ap;
	int n;
	va_start(ap, format);
	n = vsnprintf(data, sizeof data, format, ap);
	va_end(ap);
	if (n >= 0) {
		trace_bytes(data);
	}
	return n;
}
static int trace_wprintf(const wchar_t *format, ...)
{
	wchar_t wide[2048];
	char data[8192];
	va_list ap;
	int n;
	va_start(ap, format);
	n = vswprintf(wide, 2048, format, ap);
	va_end(ap);
	if (n >= 0 &&
	        WideCharToMultiByte(
	                CP_UTF8, 0, wide, -1, data, sizeof data, NULL, NULL)) {
		trace_bytes(data);
	}
	return n;
}
#	define printf trace_printf
#	define wprintf trace_wprintf
#endif

static HANDLE trace_open(LPCWSTR path,
        DWORD access,
        DWORD share,
        LPSECURITY_ATTRIBUTES sa,
        DWORD creation,
        DWORD flags,
        HANDLE template_file)
{
	HANDLE h = CreateFileW(
	        path, access, share, sa, creation, flags, template_file);
	DWORD err = GetLastError();
	wprintf(L"CreateFile path=%ls access=0x%08lx share=0x%lx flags=0x%08lx "
	        L"result=%ls error=%lu\n",
	        path, access, share, flags,
	        h == INVALID_HANDLE_VALUE ? L"FAIL" : L"OK",
	        h == INVALID_HANDLE_VALUE ? err : 0);
	SetLastError(err);
	return h;
}

#ifdef WF_DIAG_STAGE
static BOOL trace_mkdir(LPCWSTR path, LPSECURITY_ATTRIBUTES sa)
{
	BOOL ok = CreateDirectoryW(path, sa);
	DWORD err = GetLastError();
	wprintf(L"CreateDirectory path=%ls result=%ls error=%lu\n", path,
	        ok ? L"OK" : L"FAIL", ok ? 0 : err);
	SetLastError(err);
	return ok;
}
static DWORD trace_get_security(LPCWSTR name,
        SE_OBJECT_TYPE type,
        SECURITY_INFORMATION info,
        PSID *owner,
        PSID *group,
        PACL *dacl,
        PACL *sacl,
        PSECURITY_DESCRIPTOR *sd)
{
	DWORD result = GetNamedSecurityInfoW(
	        name, type, info, owner, group, dacl, sacl, sd);
	DWORD err = GetLastError();
	wprintf(L"GetNamedSecurityInfo path=%ls result=%lu\n", name, result);
	SetLastError(err);
	return result;
}
static DWORD trace_set_security(LPWSTR name,
        SE_OBJECT_TYPE type,
        SECURITY_INFORMATION info,
        PSID owner,
        PSID group,
        PACL dacl,
        PACL sacl)
{
	DWORD result = SetNamedSecurityInfoW(
	        name, type, info, owner, group, dacl, sacl);
	DWORD err = GetLastError();
	wprintf(L"SetNamedSecurityInfo private_stage=%ls result=%lu\n", name,
	        result);
	SetLastError(err);
	return result;
}
static HRESULT trace_container(PCWSTR name,
        PCWSTR display,
        PCWSTR description,
        PSID_AND_ATTRIBUTES capabilities,
        DWORD count,
        PSID *sid)
{
	HRESULT hr = CreateAppContainerProfile(
	        name, display, description, capabilities, count, sid);
	DWORD err = GetLastError();
	printf("CreateAppContainerProfile HRESULT=0x%08lx\n",
	        (unsigned long)hr);
	SetLastError(err);
	return hr;
}
static BOOL trace_process(LPCWSTR application,
        LPWSTR command,
        LPSECURITY_ATTRIBUTES process_attributes,
        LPSECURITY_ATTRIBUTES thread_attributes,
        BOOL inherit,
        DWORD flags,
        LPVOID environment,
        LPCWSTR directory,
        LPSTARTUPINFOW startup,
        LPPROCESS_INFORMATION process)
{
	BOOL ok = CreateProcessW(application, command, process_attributes,
	        thread_attributes, inherit, flags, environment, directory,
	        startup, process);
	DWORD err = GetLastError();
	wprintf(L"CreateProcess executable=%ls flags=0x%08lx result=%ls "
	        L"error=%lu\n",
	        application, flags, ok ? L"OK" : L"FAIL", ok ? 0 : err);
	SetLastError(err);
	return ok;
}
#	define CreateAppContainerProfile trace_container
#	define CreateProcessW trace_process
#	define CreateDirectoryW trace_mkdir
#	define GetNamedSecurityInfoW trace_get_security
#	define SetNamedSecurityInfoW trace_set_security
#else
static BOOL trace_info(
        HANDLE h, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID data, DWORD size)
{
	BOOL ok = GetFileInformationByHandleEx(h, kind, data, size);
	DWORD err = GetLastError();
	wprintf(L"GetFileInformationByHandleEx class=%d result=%ls error=%lu\n",
	        (int)kind, ok ? L"OK" : L"FAIL", ok ? 0 : err);
	SetLastError(err);
	return ok;
}
#	define GetFileInformationByHandleEx trace_info
#endif
#define CreateFileW trace_open
#ifdef WF_DIAG_STAGE
#	include "../src/preview/wf_dechost_client_win.c"
#else
#	include "../src/preview/wf_imageworker.c"
#endif

#ifndef WF_DIAG_CLIENT_ONLY
int wmain(int argc, wchar_t **argv)
{
	DWORD err = 0;
	setvbuf(stdout, NULL, _IONBF, 0);
#	ifdef WF_DIAG_STAGE
	wchar_t staged[1024];
	WfStatus st = WF_E_OS;
	bool failed = false;
	P0Lab lab = {0};
	bool use_lab;
	if (argc != 3) {
		puts("usage: diagnose_stage source.exe "
		     "private-localappdata|--lab");
		return 2;
	}
	use_lab = !wcscmp(argv[2], L"--lab");
	if (use_lab && !p0_lab_open(&lab, "sixel-stage-access", 1)) {
		printf("lab_open_error=%lu\n", GetLastError());
		return 2;
	}
	for (int i = 0; i < (use_lab ? 2 : 1); i++) {
		const wchar_t *root = use_lab
		        ? (i ? p0_area_root(lab.area) : lab.root)
		        : argv[2];
		wprintf(L"STAGE CASE %d root=%ls\n", i, root);
		if (!SetEnvironmentVariableW(L"LOCALAPPDATA", root) ||
		        !SetEnvironmentVariableW(L"APPDATA", root)) {
			return 2;
		}
		err = 0;
		st = wf_dechost_stage(argv[1], staged, 1024, &err);
		printf("stage_status=%d reported_error=%lu\n", (int)st, err);
		if (st == WF_OK) {
			WfDecHostOptions options = {0};
			WfDecHost *host = NULL;
			err = 0;
			options.sandbox = 1;
			options.exe = staged;
			st = wf_dechost_start(&options, &host, &err);
			printf("sandbox_start_status=%d os_error=%lu\n",
			        (int)st, err);
			wf_dechost_stop(host);
		}
		failed |= st != WF_OK;
	}
	if (use_lab) {
		(void)p0_lab_close(&lab, 1);
	}
	return failed ? 1 : 0;
#	else
	HANDLE file = INVALID_HANDLE_VALUE;
	FileKey key = {0};
	OpenResult result;
	if (argc != 2) {
		puts("usage: diagnose_open absolute-fixture-path");
		return 2;
	}
	result = open_input(argv[1], &file, &key, &err);
	printf("open_result=%d os_error=%lu\n", (int)result, err);
	if (file != INVALID_HANDLE_VALUE) {
		CloseHandle(file);
	}
	return result == OPEN_OK ? 0 : 1;
#	endif
}

#endif

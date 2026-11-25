/*
 * Prototype, not product code: run a program inside an AppContainer with no
 * capabilities and report what it printed and how it ended.
 *
 *   appc_run <staging_dir> <exe_in_staging> [args...]
 *
 * ALL APPLICATION PACKAGES is granted read+execute on <staging_dir> (the exe
 * and its inputs must live there; an AppContainer cannot read the user's
 * folders). The profile is created if missing and left in place.
 */
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <stdio.h>
#include <userenv.h>

static int grant(const wchar_t *dir)
{
	PSID all_packages = NULL;
	PACL old = NULL, acl = NULL;
	PSECURITY_DESCRIPTOR sd = NULL;
	EXPLICIT_ACCESSW ea = {0};
	DWORD st;

	if (!ConvertStringSidToSidW(L"S-1-15-2-1", &all_packages)) {
		return 0;
	}
	st = GetNamedSecurityInfoW(dir, SE_FILE_OBJECT,
	        DACL_SECURITY_INFORMATION, NULL, NULL, &old, NULL, &sd);
	if (st) {
		return 0;
	}
	ea.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
	ea.grfAccessMode = GRANT_ACCESS;
	ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
	ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
	ea.Trustee.ptstrName = (LPWSTR)all_packages;
	st = SetEntriesInAclW(1, &ea, old, &acl);
	if (!st) {
		st = SetNamedSecurityInfoW((LPWSTR)dir, SE_FILE_OBJECT,
		        DACL_SECURITY_INFORMATION, NULL, NULL, acl, NULL);
	}
	LocalFree(acl);
	LocalFree(sd);
	LocalFree(all_packages);
	return 0 == st;
}

int wmain(int argc, wchar_t **argv)
{
	const wchar_t *name = L"wfile.proto.appc";
	PSID sid = NULL;
	SECURITY_CAPABILITIES caps = {0};
	STARTUPINFOEXW si = {0};
	PROCESS_INFORMATION pi = {0};
	SIZE_T sz = 0;
	HANDLE rd, wr, nul;
	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
	wchar_t cmd[2048];
	HRESULT hr;
	DWORD code = 0, n;
	char buf[4096];
	int i;

	if (argc < 3) {
		fputs("usage: appc_run staging_dir exe [args]\n", stderr);
		return 2;
	}
	hr = CreateAppContainerProfile(
	        name, name, L"wfile prototype", NULL, 0, &sid);
	if (HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) == hr) {
		hr = DeriveAppContainerSidFromAppContainerName(name, &sid);
	}
	printf("profile hr=0x%08lx\n", (unsigned long)hr);
	if (FAILED(hr)) {
		return 1;
	}
	printf("grant staging dir: %s\n", grant(argv[1]) ? "ok" : "FAILED");

	CreatePipe(&rd, &wr, &sa, 0);
	nul = CreateFileW(L"NUL", GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
	        NULL);
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	caps.AppContainerSid = sid;
	InitializeProcThreadAttributeList(NULL, 2, 0, &sz);
	si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, sz);
	InitializeProcThreadAttributeList(si.lpAttributeList, 2, 0, &sz);
	UpdateProcThreadAttribute(si.lpAttributeList, 0,
	        PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &caps, sizeof caps,
	        NULL, NULL);
	{
		HANDLE inherit[2] = {wr, nul};

		UpdateProcThreadAttribute(si.lpAttributeList, 0,
		        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
		        sizeof inherit, NULL, NULL);
	}
	si.StartupInfo.cb = sizeof si;
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = wr;
	si.StartupInfo.hStdInput = nul;
	swprintf(cmd, 2048, L"\"%ls\"", argv[2]);
	for (i = 3; i < argc; i++) {
		wcscat_s(cmd, 2048, L" \"");
		wcscat_s(cmd, 2048, argv[i]);
		wcscat_s(cmd, 2048, L"\"");
	}
	if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE,
	            EXTENDED_STARTUPINFO_PRESENT, NULL, argv[1],
	            &si.StartupInfo, &pi)) {
		printf("CreateProcess failed: %lu\n", GetLastError());
		return 1;
	}
	CloseHandle(wr);
	while (ReadFile(rd, buf, sizeof buf - 1, &n, NULL) && n) {
		buf[n] = '\0';
		fputs(buf, stdout);
	}
	WaitForSingleObject(pi.hProcess, 60000);
	GetExitCodeProcess(pi.hProcess, &code);
	printf("exit code 0x%08lx\n", (unsigned long)code);
	return 0;
}

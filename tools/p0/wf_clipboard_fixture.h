#ifndef WF_CLIPBOARD_FIXTURE_H
#define WF_CLIPBOARD_FIXTURE_H

#include <windows.h>

#include <bcrypt.h>
#include <sddl.h>
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

/* Call before creating windows or worker threads. Only a newly created,
 * current-user station may hold test clipboard data. Its station/desktop
 * handles live until process exit, including any deferred clipboard worker.
 * Child processes must use the returned STARTUPINFO.lpDesktop explicitly. */
static bool p0_clipboard_fixture(
        wchar_t *child_desktop, size_t cap, DWORD *error)
{
	union
	{
		TOKEN_USER info;
		BYTE bytes[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
	} user;
	HANDLE token = NULL;
	LPWSTR sid = NULL;
	PSECURITY_DESCRIPTOR descriptor = NULL;
	SECURITY_ATTRIBUTES attributes = {sizeof attributes, NULL, FALSE};
	HWINSTA original = NULL, station = NULL;
	HDESK desktop = NULL;
	wchar_t name[64], actual[64], sddl[512];
	BYTE nonce[16];
	DWORD got = 0, saved = ERROR_GEN_FAILURE;
	bool station_selected = false, ok = false;
	static const wchar_t hex[] = L"0123456789abcdef";

	*error = ERROR_INVALID_PARAMETER;
	if (!child_desktop || cap < 64) {
		return false;
	}
	child_desktop[0] = 0;
	if (BCryptGenRandom(NULL, nonce, sizeof nonce,
	            BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
		goto done;
	}
	wcscpy_s(name, 64, L"piero-clip-");
	for (size_t i = 0; i < sizeof nonce; ++i) {
		name[11 + 2 * i] = hex[nonce[i] >> 4];
		name[12 + 2 * i] = hex[nonce[i] & 15];
	}
	name[43] = 0;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) ||
	        !GetTokenInformation(
	                token, TokenUser, &user, sizeof user, &got) ||
	        !ConvertSidToStringSidW(user.info.User.Sid, &sid)) {
		saved = GetLastError();
		goto done;
	}
	if (swprintf(sddl, 512, L"O:%lsD:P(A;;GA;;;%ls)", sid, sid) < 0 ||
	        swprintf(child_desktop, cap, L"%ls\\Default", name) < 0) {
		saved = ERROR_INSUFFICIENT_BUFFER;
		goto done;
	}
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
	            sddl, SDDL_REVISION_1, &descriptor, NULL)) {
		saved = GetLastError();
		goto done;
	}
	attributes.lpSecurityDescriptor = descriptor;
	original = GetProcessWindowStation();
	if (!original) {
		saved = GetLastError();
		if (!saved) {
			saved = ERROR_INVALID_HANDLE;
		}
		goto done;
	}
	station = CreateWindowStationW(
	        name, CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, &attributes);
	if (!station || !SetProcessWindowStation(station)) {
		saved = GetLastError();
		goto done;
	}
	station_selected = true;
	if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME,
	            actual, sizeof actual, &got)) {
		saved = GetLastError();
		goto done;
	}
	if (wcscmp(name, actual)) {
		saved = ERROR_INVALID_DATA;
		goto done;
	}
	desktop = CreateDesktopW(
	        L"Default", NULL, NULL, 0, GENERIC_ALL, &attributes);
	if (!desktop || !SetThreadDesktop(desktop)) {
		saved = GetLastError();
		goto done;
	}
	ok = true;
done:
	if (!ok) {
		child_desktop[0] = 0;
		if (station_selected) {
			(void)SetProcessWindowStation(original);
		}
		if (desktop) {
			CloseDesktop(desktop);
		}
		if (station) {
			CloseWindowStation(station);
		}
	}
	if (descriptor) {
		LocalFree(descriptor);
	}
	if (sid) {
		LocalFree(sid);
	}
	if (token) {
		CloseHandle(token);
	}
	*error = ok ? ERROR_SUCCESS : saved;
	return ok;
}

#endif

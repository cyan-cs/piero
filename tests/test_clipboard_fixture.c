/* Mock GUI APIs only: no clipboard reads/writes or real station changes. */
#include <windows.h>

#include <aclapi.h>
#include <stdint.h>
#include <wchar.h>

#include "wf_test.h"

typedef enum
{
	FIX_OK,
	FIX_NO_ORIGINAL,
	FIX_CREATE_FAILED,
	FIX_SELECT_FAILED,
	FIX_QUERY_FAILED,
	FIX_WRONG_STATION,
	FIX_DESKTOP_FAILED,
	FIX_THREAD_FAILED
} FixtureCase;
static FixtureCase fixture_case;
static HWINSTA selected;
static wchar_t station_name[64];
static unsigned creates, station_closes, desktop_closes, thread_selects;

#define OLD_STATION ((HWINSTA)(uintptr_t)1)
#define NEW_STATION ((HWINSTA)(uintptr_t)2)
#define NEW_DESKTOP ((HDESK)(uintptr_t)3)

static void private_descriptor(const SECURITY_ATTRIBUTES *attributes)
{
	PSID owner = NULL;
	PACL dacl = NULL;
	BOOL present = FALSE, defaulted = FALSE;
	SECURITY_DESCRIPTOR_CONTROL control;
	DWORD revision;
	ACCESS_ALLOWED_ACE *ace = NULL;

	CHECK(attributes && attributes->lpSecurityDescriptor &&
	        !attributes->bInheritHandle);
	if (!attributes || !attributes->lpSecurityDescriptor) {
		return;
	}
	CHECK(GetSecurityDescriptorOwner(
	        attributes->lpSecurityDescriptor, &owner, &defaulted));
	CHECK(GetSecurityDescriptorDacl(
	        attributes->lpSecurityDescriptor, &present, &dacl, &defaulted));
	CHECK(GetSecurityDescriptorControl(
	        attributes->lpSecurityDescriptor, &control, &revision));
	CHECK(present && dacl && dacl->AceCount == 1 &&
	        (control & SE_DACL_PROTECTED));
	if (!dacl || dacl->AceCount != 1) {
		return;
	}
	CHECK(GetAce(dacl, 0, (void **)&ace));
	CHECK(ace && ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
	        ace->Mask == GENERIC_ALL && EqualSid(owner, &ace->SidStart));
}

static HWINSTA WINAPI current_station(void)
{
	return fixture_case == FIX_NO_ORIGINAL ? NULL : selected;
}
static HWINSTA WINAPI create_station(LPCWSTR name,
        DWORD flags,
        ACCESS_MASK access,
        LPSECURITY_ATTRIBUTES attributes)
{
	creates++;
	CHECK(flags == CWF_CREATE_ONLY && access == WINSTA_ALL_ACCESS);
	CHECK(wcslen(name) == 43 && !wcsncmp(name, L"piero-clip-", 11));
	private_descriptor(attributes);
	wcscpy_s(station_name, 64, name);
	SetLastError(ERROR_ACCESS_DENIED);
	return fixture_case == FIX_CREATE_FAILED ? NULL : NEW_STATION;
}
static BOOL WINAPI select_station(HWINSTA station)
{
	CHECK(station == OLD_STATION || station == NEW_STATION);
	SetLastError(ERROR_ACCESS_DENIED);
	if (station == NEW_STATION && fixture_case == FIX_SELECT_FAILED) {
		return FALSE;
	}
	selected = station;
	return TRUE;
}
static BOOL WINAPI station_info(
        HANDLE station, int index, PVOID out, DWORD bytes, LPDWORD needed)
{
	const wchar_t *name =
	        fixture_case == FIX_WRONG_STATION ? L"unrelated" : station_name;
	size_t size = (wcslen(name) + 1) * sizeof(wchar_t);
	CHECK(station == NEW_STATION && index == UOI_NAME && bytes >= size);
	*needed = (DWORD)size;
	SetLastError(ERROR_ACCESS_DENIED);
	if (fixture_case == FIX_QUERY_FAILED) {
		return FALSE;
	}
	memcpy(out, name, size);
	return TRUE;
}
static HDESK WINAPI create_desktop(LPCWSTR name,
        LPCWSTR device,
        DEVMODEW *mode,
        DWORD flags,
        ACCESS_MASK access,
        LPSECURITY_ATTRIBUTES attributes)
{
	CHECK(!wcscmp(name, L"Default") && !device && !mode && !flags &&
	        access == GENERIC_ALL);
	private_descriptor(attributes);
	SetLastError(ERROR_ACCESS_DENIED);
	return fixture_case == FIX_DESKTOP_FAILED ? NULL : NEW_DESKTOP;
}
static BOOL WINAPI select_desktop(HDESK desktop)
{
	CHECK(desktop == NEW_DESKTOP);
	thread_selects++;
	SetLastError(ERROR_ACCESS_DENIED);
	return fixture_case == FIX_THREAD_FAILED ? FALSE : TRUE;
}
static BOOL WINAPI close_station(HWINSTA station)
{
	CHECK(station == NEW_STATION);
	station_closes++;
	return TRUE;
}
static BOOL WINAPI close_desktop(HDESK desktop)
{
	CHECK(desktop == NEW_DESKTOP);
	desktop_closes++;
	return TRUE;
}

#define GetProcessWindowStation current_station
#define CreateWindowStationW create_station
#define SetProcessWindowStation select_station
#define GetUserObjectInformationW station_info
#define CreateDesktopW create_desktop
#define SetThreadDesktop select_desktop
#define CloseWindowStation close_station
#define CloseDesktop close_desktop
#ifndef WF_CLIPBOARD_FIXTURE_SOURCE
#	define WF_CLIPBOARD_FIXTURE_SOURCE "../tools/p0/wf_clipboard_fixture.h"
#endif
#include WF_CLIPBOARD_FIXTURE_SOURCE

int main(void)
{
	for (FixtureCase mode = FIX_OK; mode <= FIX_THREAD_FAILED; ++mode) {
		wchar_t child[64];
		DWORD error = 0;
		fixture_case = mode;
		selected = OLD_STATION;
		creates = station_closes = desktop_closes = thread_selects = 0;
		SetLastError(ERROR_SUCCESS);
		bool ok = p0_clipboard_fixture(child, 64, &error);
		CHECK(ok == (mode == FIX_OK));
		CHECK(ok ? error == 0 : error != 0);
		if (ok) {
			wchar_t expected[64];
			swprintf(expected, 64, L"%ls\\Default", station_name);
			CHECK(!wcscmp(child, expected) &&
			        selected == NEW_STATION && thread_selects == 1);
			CHECK(!station_closes && !desktop_closes);
		} else {
			CHECK(!child[0] && selected == OLD_STATION);
			CHECK(station_closes ==
			        (mode > FIX_CREATE_FAILED ? 1u : 0u));
			CHECK(desktop_closes ==
			        (mode == FIX_THREAD_FAILED ? 1u : 0u));
		}
		CHECK(creates == (mode == FIX_NO_ORIGINAL ? 0u : 1u));
	}
	return TEST_RESULT();
}

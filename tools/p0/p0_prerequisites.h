/* Independent OS probes, before product assertions. Only known unavailable
 * capabilities produce SKIP; unexpected probe errors remain failures. */
#define COBJMACROS
#include <restartmanager.h>
#include <userenv.h>

static int prerequisite_result(
        Ev *e, const char *name, DWORD error, bool unavailable)
{
	char text[256];

	snprintf(text, sizeof text, "%s: %s (Win32/HRESULT 0x%08lx)",
	        unavailable ? "SKIP" : "FAIL", name, (unsigned long)error);
	puts(text);
	ev_write_text(e, L"artifacts\\prerequisite.txt", text);
	if (!unavailable) {
		ev_check(e, name, 0, "OS capability available", text, false);
	}
	return unavailable ? 77 : 1;
}

static int require_appcontainer(Ev *e)
{
	wchar_t name[96];
	PSID sid = NULL;
	HRESULT hr;

	/* Use a fresh profile: an old product profile cannot decide this probe. */
	swprintf(name, 96, L"piero.test.%lu.%llu", GetCurrentProcessId(),
	        (unsigned long long)GetTickCount64());
	hr = CreateAppContainerProfile(name, name, name, NULL, 0, &sid);
	if (SUCCEEDED(hr)) {
		FreeSid(sid);
		hr = DeleteAppContainerProfile(name);
		if (SUCCEEDED(hr)) {
			return 0;
		}
		return prerequisite_result(
		        e, "AppContainer profile cleanup", (DWORD)hr, false);
	}
	return prerequisite_result(e, "CreateAppContainerProfile", (DWORD)hr,
	        hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
	                hr == E_ACCESSDENIED || hr == E_NOTIMPL ||
	                hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
}

static int require_restart_manager(Ev *e)
{
	DWORD session = 0, error;
	wchar_t key[CCH_RM_SESSION_KEY + 1] = L"";

	error = RmStartSession(&session, 0, key);
	if (ERROR_SUCCESS == error) {
		error = RmEndSession(session);
		return ERROR_SUCCESS == error
		        ? 0
		        : prerequisite_result(e, "RmEndSession", error, false);
	}
	return prerequisite_result(e, "RmStartSession", error,
	        ERROR_ACCESS_DENIED == error || ERROR_WRITE_FAULT == error ||
	                ERROR_NOT_SUPPORTED == error);
}

/* Copy preflight must inspect every real ancestor, without a drive alias. */
static int require_readable_ancestors(Ev *e, const wchar_t *directory)
{
	wchar_t path[1200];
	size_t len = wcslen(directory);

	if (len >= 1200 || len < 3 || directory[1] != L':') {
		return prerequisite_result(
		        e, "ancestor probe path", ERROR_INVALID_NAME, false);
	}
	wcscpy_s(path, 1200, directory);
	for (;;) {
		HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		        NULL, OPEN_EXISTING,
		        FILE_FLAG_BACKUP_SEMANTICS |
		                FILE_FLAG_OPEN_REPARSE_POINT,
		        NULL);
		if (INVALID_HANDLE_VALUE == h) {
			DWORD error = GetLastError();
			wprintf(L"Ancestor probe: %ls\n", path);
			return prerequisite_result(e,
			        "read real copy ancestors", error,
			        ERROR_ACCESS_DENIED == error);
		}
		CloseHandle(h);
		if (len <= 3) {
			return 0;
		}
		while (len > 3 && path[len - 1] != L'\\') {
			len--;
		}
		if (len > 3) {
			len--;
		}
		path[len] = L'\0';
	}
}

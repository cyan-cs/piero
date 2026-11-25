#include "wf_update_win.h"
#include <winhttp.h>
#include <stdlib.h>
#include <string.h>
#include <sddl.h>
#include <stdio.h>

struct WfUpdateWorker
{
	volatile LONG refs, stop;
	HANDLE done;
	bool manual;
	char current[64];
	WfUpdateResult result;
};
static void drop(WfUpdateWorker *w)
{
	if (InterlockedDecrement(&w->refs) == 0) {
		CloseHandle(w->done);
		free(w);
	}
}
static uint64_t seconds(void)
{
	FILETIME f;
	GetSystemTimeAsFileTime(&f);
	return (((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) /
	        10000000;
}
static bool claim(bool manual, WfUpdateState *state)
{
	/* The registry stores attempts, including offline/404 results. Failure to
	 * establish throttling refuses automatic I/O. No file journal is touched. */
	HANDLE token = NULL, mutex = NULL;
	union
	{
		TOKEN_USER alignment;
		BYTE bytes[1024];
	} user;
	DWORD n = sizeof user;
	LPWSTR sid = NULL;
	wchar_t name[256];
	HKEY key = NULL;
	bool ok = false;
	DWORD wait;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		return false;
	}
	if (!GetTokenInformation(token, TokenUser, &user, n, &n) ||
	        !ConvertSidToStringSidW(user.alignment.User.Sid, &sid)) {
		goto done;
	}
	if (swprintf(name, 256, L"Local\\piero.update.%ls", sid) < 0) {
		goto done;
	}
	mutex = CreateMutexW(NULL, FALSE, name);
	wait = mutex ? WaitForSingleObject(mutex, 0) : WAIT_FAILED;
	if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
		*state = WF_UPDATE_THROTTLED;
		goto done;
	}
	if (RegCreateKeyExW(HKEY_CURRENT_USER,
	            L"Software\\cyan-cs\\piero\\Updates", 0, NULL, 0,
	            KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key,
	            NULL) == ERROR_SUCCESS) {
		uint64_t previous = 0, now = seconds();
		DWORD type = 0, bytes = sizeof previous;
		LSTATUS st = RegQueryValueExW(key, L"LastAttempt", NULL, &type,
		        (BYTE *)&previous, &bytes);
		bool valid = st == ERROR_FILE_NOT_FOUND ||
		        (st == ERROR_SUCCESS && type == REG_QWORD &&
		                bytes == sizeof previous);
		if (manual || wf_update_due(true, true, valid, now, previous)) {
			ok = RegSetValueExW(key, L"LastAttempt", 0, REG_QWORD,
			             (BYTE *)&now, sizeof now) == ERROR_SUCCESS;
		} else {
			*state = WF_UPDATE_THROTTLED;
		}
	}
	ReleaseMutex(mutex);
done:
	if (key) {
		RegCloseKey(key);
	}
	if (mutex) {
		CloseHandle(mutex);
	}
	if (sid) {
		LocalFree(sid);
	}
	if (token) {
		CloseHandle(token);
	}
	return ok;
}
static WfUpdateResult fetch(WfUpdateWorker *w)
{
	WfUpdateResult r = {WF_UPDATE_UNAVAILABLE, {0}};
	HINTERNET session = NULL, connection = NULL, request = NULL;
	char *body = NULL;
	size_t used = 0;
	uint64_t deadline = GetTickCount64() + 15000;
	DWORD disable = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES |
	        WINHTTP_DISABLE_AUTHENTICATION;
	DWORD status = 0, bytes = sizeof status;
	if (!claim(w->manual, &r.state) ||
	        InterlockedCompareExchange(&w->stop, 0, 0)) {
		return r;
	}
	session = WinHttpOpen(L"piero update check",
	        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
	        WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session || !WinHttpSetTimeouts(session, 3000, 3000, 3000, 5000)) {
		goto done;
	}
	connection = WinHttpConnect(
	        session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!connection || InterlockedCompareExchange(&w->stop, 0, 0)) {
		goto done;
	}
	request = WinHttpOpenRequest(connection, L"GET",
	        L"/repos/cyan-cs/piero/releases/latest", NULL,
	        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
	        WINHTTP_FLAG_SECURE);
	if (!request ||
	        !WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE,
	                &disable, sizeof disable)) {
		goto done;
	}
	if (InterlockedCompareExchange(&w->stop, 0, 0) ||
	        !WinHttpSendRequest(request,
	                L"Accept: application/vnd.github+json\r\n", (DWORD)-1L,
	                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	        !WinHttpReceiveResponse(request, NULL)) {
		goto done;
	}
	if (!WinHttpQueryHeaders(request,
	            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
	            WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes,
	            WINHTTP_NO_HEADER_INDEX)) {
		goto done;
	}
	if (status != 200) {
		r = wf_update_response(w->current, status, NULL, 0);
		goto done;
	}
	body = malloc(WF_UPDATE_BODY_MAX + 1);
	if (!body) {
		goto done;
	}
	for (;;) {
		if (InterlockedCompareExchange(&w->stop, 0, 0) ||
		        GetTickCount64() > deadline) {
			goto done;
		}
		DWORD got = 0, cap = (DWORD)(WF_UPDATE_BODY_MAX + 1 - used);
		if (cap > 4096) {
			cap = 4096;
		}
		if (!WinHttpReadData(request, body + used, cap, &got)) {
			goto done;
		}
		used += got;
		if (used > WF_UPDATE_BODY_MAX) {
			r.state = WF_UPDATE_INVALID;
			goto done;
		}
		if (!got) {
			break;
		}
	}
	r = wf_update_response(w->current, status, body, used);
done:
	free(body);
	if (request) {
		WinHttpCloseHandle(request);
	}
	if (connection) {
		WinHttpCloseHandle(connection);
	}
	if (session) {
		WinHttpCloseHandle(session);
	}
	return r;
}
static DWORD WINAPI run(void *p)
{
	WfUpdateWorker *w = p;
	w->result = fetch(w);
	SetEvent(w->done);
	drop(w);
	return 0;
}
WfUpdateWorker *wf_update_start(const char *current, bool manual)
{
	if (!current || strlen(current) >= 64) {
		return NULL;
	}
	WfUpdateWorker *w = calloc(1, sizeof *w);
	if (!w) {
		return NULL;
	}
	w->refs = 2;
	w->manual = manual;
	memcpy(w->current, current, strlen(current) + 1);
	w->done = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!w->done) {
		free(w);
		return NULL;
	}
	HANDLE thread = CreateThread(NULL, 0, run, w, 0, NULL);
	if (!thread) {
		w->refs = 1;
		drop(w);
		return NULL;
	}
	CloseHandle(thread);
	return w;
}
HANDLE wf_update_event(const WfUpdateWorker *w)
{
	return w->done;
}
bool wf_update_poll(WfUpdateWorker *w, WfUpdateResult *r, bool *manual)
{
	if (WaitForSingleObject(w->done, 0) != WAIT_OBJECT_0) {
		return false;
	}
	*r = w->result;
	*manual = w->manual;
	return true;
}
void wf_update_cancel(WfUpdateWorker *w)
{
	if (w) {
		InterlockedExchange(&w->stop, 1);
	}
}
void wf_update_disable_automatic(WfUpdateWorker *w)
{
	if (w && !w->manual) {
		wf_update_cancel(w);
	}
}
void wf_update_release(WfUpdateWorker *w)
{
	if (w) {
		wf_update_cancel(w);
		drop(w);
	}
}

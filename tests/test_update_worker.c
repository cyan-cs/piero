/* Real threads + mocked WinHTTP/registry: no network or host registry writes. */
#include <windows.h>
#include <winhttp.h>
#include <stdint.h>
#include <string.h>
#include "wf_test.h"
#include "update/wf_update_win.h"

static HANDLE gate, joined;
static LONG opened, closed, reads;
static DWORD status_code = 200;
static const char *mock_body =
        "{\"tag_name\":\"v0.0.4\",\"draft\":false,\"prerelease\":false}";
static size_t cursor;
static uint64_t mock_previous;
static bool have_previous;
static HANDLE WINAPI make_thread(LPSECURITY_ATTRIBUTES a,
        SIZE_T s,
        LPTHREAD_START_ROUTINE f,
        LPVOID p,
        DWORD flags,
        LPDWORD id)
{
	HANDLE h = CreateThread(a, s, f, p, flags, id);
	CHECK(h != NULL);
	CHECK(h &&
	        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(),
	                &joined, SYNCHRONIZE, FALSE, 0));
	return h;
}
static LSTATUS WINAPI create_key(HKEY root,
        LPCWSTR sub,
        DWORD reserved,
        LPWSTR cls,
        DWORD options,
        REGSAM access,
        const LPSECURITY_ATTRIBUTES sa,
        PHKEY key,
        LPDWORD disposition)
{
	(void)reserved;
	(void)cls;
	(void)options;
	(void)access;
	(void)sa;
	(void)disposition;
	CHECK(root == HKEY_CURRENT_USER &&
	        !wcscmp(sub, L"Software\\cyan-cs\\piero\\Updates"));
	*key = (HKEY)(uintptr_t)1;
	return ERROR_SUCCESS;
}
static LSTATUS WINAPI query_key(HKEY key,
        LPCWSTR name,
        LPDWORD reserved,
        LPDWORD type,
        LPBYTE data,
        LPDWORD bytes)
{
	(void)key;
	(void)reserved;
	CHECK(!wcscmp(name, L"LastAttempt"));
	if (!have_previous) {
		return ERROR_FILE_NOT_FOUND;
	}
	CHECK(*bytes == sizeof mock_previous);
	*type = REG_QWORD;
	memcpy(data, &mock_previous, sizeof mock_previous);
	return ERROR_SUCCESS;
}
static LSTATUS WINAPI set_key(HKEY key,
        LPCWSTR name,
        DWORD reserved,
        DWORD type,
        const BYTE *data,
        DWORD bytes)
{
	(void)key;
	(void)reserved;
	CHECK(!wcscmp(name, L"LastAttempt") && type == REG_QWORD &&
	        bytes == sizeof mock_previous);
	memcpy(&mock_previous, data, sizeof mock_previous);
	have_previous = true;
	return ERROR_SUCCESS;
}
static LSTATUS WINAPI close_key(HKEY key)
{
	(void)key;
	return ERROR_SUCCESS;
}
static HINTERNET WINAPI open_http(
        LPCWSTR agent, DWORD access, LPCWSTR proxy, LPCWSTR bypass, DWORD flags)
{
	(void)agent;
	(void)access;
	(void)proxy;
	(void)bypass;
	(void)flags;
	InterlockedIncrement(&opened);
	return (HINTERNET)(uintptr_t)1;
}
static BOOL WINAPI timeouts(HINTERNET h, int a, int b, int c, int d)
{
	(void)h;
	CHECK(a > 0 && b > 0 && c > 0 && d > 0);
	return TRUE;
}
static HINTERNET WINAPI connect_http(
        HINTERNET h, LPCWSTR host, INTERNET_PORT port, DWORD reserved)
{
	(void)h;
	(void)reserved;
	CHECK(!wcscmp(host, L"api.github.com") && port == 443);
	return (HINTERNET)(uintptr_t)2;
}
static HINTERNET WINAPI open_request(HINTERNET h,
        LPCWSTR verb,
        LPCWSTR path,
        LPCWSTR version,
        LPCWSTR ref,
        LPCWSTR const *accept,
        DWORD flags)
{
	(void)h;
	(void)version;
	(void)ref;
	(void)accept;
	CHECK(!wcscmp(verb, L"GET") &&
	        !wcscmp(path, L"/repos/cyan-cs/piero/releases/latest") &&
	        (flags & WINHTTP_FLAG_SECURE));
	return (HINTERNET)(uintptr_t)3;
}
static BOOL WINAPI option(HINTERNET h, DWORD name, LPVOID data, DWORD size)
{
	(void)h;
	CHECK(name == WINHTTP_OPTION_DISABLE_FEATURE && size == sizeof(DWORD));
	CHECK((*(DWORD *)data &
	              (WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES |
	                      WINHTTP_DISABLE_AUTHENTICATION)) ==
	        (WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES |
	                WINHTTP_DISABLE_AUTHENTICATION));
	return TRUE;
}
static BOOL WINAPI send_http(HINTERNET h,
        LPCWSTR headers,
        DWORD hn,
        LPVOID data,
        DWORD dn,
        DWORD total,
        DWORD_PTR context)
{
	(void)h;
	(void)headers;
	(void)hn;
	(void)data;
	(void)dn;
	(void)total;
	(void)context;
	CHECK(WaitForSingleObject(gate, 10000) == WAIT_OBJECT_0);
	return TRUE;
}
static BOOL WINAPI receive_http(HINTERNET h, LPVOID reserved)
{
	(void)h;
	(void)reserved;
	return TRUE;
}
static BOOL WINAPI headers_http(HINTERNET h,
        DWORD level,
        LPCWSTR name,
        LPVOID buffer,
        LPDWORD bytes,
        LPDWORD index)
{
	(void)h;
	(void)level;
	(void)name;
	(void)index;
	CHECK(*bytes == sizeof(DWORD));
	*(DWORD *)buffer = status_code;
	return TRUE;
}
static BOOL WINAPI read_http(
        HINTERNET h, LPVOID buffer, DWORD capacity, LPDWORD got)
{
	(void)h;
	size_t remaining = strlen(mock_body) - cursor;
	size_t n = remaining < capacity ? remaining : capacity;
	memcpy(buffer, mock_body + cursor, n);
	cursor += n;
	*got = (DWORD)n;
	InterlockedIncrement(&reads);
	return TRUE;
}
static BOOL WINAPI close_http(HINTERNET h)
{
	(void)h;
	InterlockedIncrement(&closed);
	return TRUE;
}

#define CreateThread make_thread
#define RegCreateKeyExW create_key
#define RegQueryValueExW query_key
#define RegSetValueExW set_key
#define RegCloseKey close_key
#define WinHttpOpen open_http
#define WinHttpSetTimeouts timeouts
#define WinHttpConnect connect_http
#define WinHttpOpenRequest open_request
#define WinHttpSetOption option
#define WinHttpSendRequest send_http
#define WinHttpReceiveResponse receive_http
#define WinHttpQueryHeaders headers_http
#define WinHttpReadData read_http
#define WinHttpCloseHandle close_http
#include "../src/update/wf_update_win.c"

static void begin(void)
{
	opened = closed = reads = 0;
	cursor = 0;
	gate = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(gate != NULL);
}
static void join(void)
{
	CHECK(WaitForSingleObject(joined, 15000) == WAIT_OBJECT_0);
	CloseHandle(joined);
	CloseHandle(gate);
}
int main(void)
{
	begin();
	WfUpdateWorker *worker = wf_update_start("0.0.3-dev", true);
	CHECK(worker != NULL);
	if (!worker) {
		return 1;
	}
	CHECK(WaitForSingleObject(wf_update_event(worker), 0) == WAIT_TIMEOUT);
	wf_update_disable_automatic(
	        worker); /* disabling auto must not cancel this manual request */
	SetEvent(gate);
	CHECK(WaitForSingleObject(wf_update_event(worker), 15000) ==
	        WAIT_OBJECT_0);
	WfUpdateResult result;
	bool manual = false;
	CHECK(wf_update_poll(worker, &result, &manual) && manual &&
	        result.state == WF_UPDATE_AVAILABLE);
	wf_update_release(worker);
	join();
	CHECK(opened == 1 && closed == 3);
	begin();
	status_code = 404;
	worker = wf_update_start("0.0.3-dev", true);
	CHECK(worker != NULL);
	if (!worker) {
		return 1;
	}
	SetEvent(gate);
	CHECK(WaitForSingleObject(wf_update_event(worker), 15000) ==
	        WAIT_OBJECT_0);
	CHECK(wf_update_poll(worker, &result, &manual) &&
	        result.state == WF_UPDATE_NO_RELEASE && reads == 0);
	wf_update_release(worker);
	join();
	begin();
	mock_previous = seconds();
	have_previous = true;
	worker = wf_update_start("0.0.3-dev", false);
	CHECK(worker != NULL);
	if (!worker) {
		return 1;
	}
	CHECK(WaitForSingleObject(wf_update_event(worker), 15000) ==
	        WAIT_OBJECT_0);
	CHECK(wf_update_poll(worker, &result, &manual) &&
	        result.state == WF_UPDATE_THROTTLED && opened == 0);
	wf_update_release(worker);
	SetEvent(gate);
	join();
	begin();
	status_code = 200;
	worker = wf_update_start("0.0.3-dev", true);
	CHECK(worker != NULL);
	if (!worker) {
		return 1;
	}
	wf_update_release(
	        worker); /* creator goes away while a real thread owns the context */
	SetEvent(gate);
	join();
	return TEST_RESULT();
}

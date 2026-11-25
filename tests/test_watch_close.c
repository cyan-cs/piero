/*
 * Closing a watch whose cancelled request does not finish: the close is a short
 * wait, not an unbounded one, and the memory the system still owns is not freed
 * under it; it is released once the request has finished. The request that does
 * not finish is made by a CancelIoEx that does nothing until the test lets it.
 */
#include <windows.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "wf_test.h"

static bool stuck;

static BOOL test_cancel(HANDLE h, LPOVERLAPPED ov)
{
	if (stuck) {
		return TRUE; /* said done, and the request goes on */
	}
	return CancelIoEx(h, ov);
}

#define CancelIoEx test_cancel
#include "../src/fs/wf_watch_win.c"
#undef CancelIoEx

static void make_dir(wchar_t *dir, size_t cap, const wchar_t *tag)
{
	wchar_t tmp[MAX_PATH];

	GetTempPathW(MAX_PATH, tmp);
	swprintf(dir, cap, L"%lspiero-watchclose-%ls-%lu", tmp, tag,
	        GetCurrentProcessId());
	CHECK(CreateDirectoryW(dir, NULL));
}

int main(void)
{
	wchar_t dir[MAX_PATH + 64], other[MAX_PATH + 64];
	WfWatch *w = NULL, *w2 = NULL;
	ULONGLONG t0;
	DWORD n;

	make_dir(dir, MAX_PATH + 64, L"a");
	make_dir(other, MAX_PATH + 64, L"b");

	/* an ordinary close: no wait to speak of, nothing kept */
	CHECK(WF_OK == wf_watch_open(dir, &w));
	t0 = GetTickCount64();
	wf_watch_close(w);
	CHECK(GetTickCount64() - t0 < 2000 && 0 == late_n);

	/* a request that does not finish: the close returns after the wait, the
	 * memory is kept, and the watch is not used again */
	CHECK(WF_OK == wf_watch_open(dir, &w));
	stuck = true;
	t0 = GetTickCount64();
	wf_watch_close(w);
	CHECK(GetTickCount64() - t0 < 5000);
	CHECK(1 == late_n && !HasOverlappedIoCompleted(&w->ov));
	/* later opens and closes do not touch it while it is the system's */
	CHECK(WF_OK == wf_watch_open(other, &w2));
	CHECK(1 == late_n);
	stuck = false;
	wf_watch_close(w2);
	CHECK(1 == late_n);
	/* the system lets go: it is released, by the next open or close */
	CancelIoEx(w->dir, &w->ov);
	GetOverlappedResult(w->dir, &w->ov, &n, TRUE);
	CHECK(HasOverlappedIoCompleted(&w->ov));
	reap();
	CHECK(0 == late_n);

	/* with no room to keep it, a watch that does not finish is not freed */
	stuck = true;
	for (int i = 0; i < WATCH_LATE_MAX + 1; i++) {
		WfWatch *x = NULL;

		CHECK(WF_OK == wf_watch_open(dir, &x));
		wf_watch_close(x);
	}
	CHECK(WATCH_LATE_MAX == late_n);
	stuck = false;
	for (size_t i = 0; i < late_n; i++) {
		CancelIoEx(late[i]->dir, &late[i]->ov);
		GetOverlappedResult(late[i]->dir, &late[i]->ov, &n, TRUE);
	}
	reap();
	CHECK(0 == late_n);
	RemoveDirectoryW(dir);
	RemoveDirectoryW(other);
	return TEST_RESULT();
}

/*
 * A harmless program the lock tests start themselves (DR-DRAFT-024). It opens
 * one file without sharing (so another program cannot rename, delete or
 * overwrite it), tells the test it is ready by creating a file, and then does
 * nothing. It never touches anything else, and ends by itself after 120
 * seconds so that a test that dies cannot leave it behind.
 *
 *   lock_helper hold   <file> <ready>            no window
 *   lock_helper close  <file> <ready>            a window that closes on WM_CLOSE
 *   lock_helper ignore <file> <ready>            a window that ignores WM_CLOSE
 *   lock_helper unsaved <file> <ready>           a window that closes on the
 *                                                second WM_CLOSE (it "asks" first)
 */
#include <windows.h>

#include <wchar.h>

#define LIFETIME_MS 120000u

static int g_mode; /* 0 hold, 1 close, 2 ignore, 3 unsaved */
static int g_closes;

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	if (WM_CLOSE == m) {
		g_closes++;
		if (1 == g_mode || (3 == g_mode && g_closes >= 2)) {
			DestroyWindow(w);
		}
		return 0; /* ignore: stays */
	}
	if (WM_DESTROY == m) {
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(w, m, wp, lp);
}

int wmain(int argc, wchar_t **argv)
{
	HANDLE h, ready;
	MSG msg;
	ULONGLONG end = GetTickCount64() + LIFETIME_MS;

	if (argc < 4) {
		return 2;
	}
	g_mode = !wcscmp(argv[1], L"close")    ? 1
	        : !wcscmp(argv[1], L"ignore")  ? 2
	        : !wcscmp(argv[1], L"unsaved") ? 3
	                                       : 0;
	h = CreateFileW(argv[2], GENERIC_READ | GENERIC_WRITE, 0, NULL,
	        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return 3;
	}
	if (g_mode) {
		WNDCLASSW c = {0};
		HWND w;

		c.lpfnWndProc = proc;
		c.hInstance = GetModuleHandleW(NULL);
		c.lpszClassName = L"PieroLockHelper";
		RegisterClassW(&c);
		w = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		        c.lpszClassName, L"piero lock helper", WS_POPUP, -32000,
		        -32000, 10, 10, NULL, NULL, c.hInstance, NULL);
		if (!w) {
			return 4;
		}
		ShowWindow(w, SW_SHOWNOACTIVATE);
	}
	ready = CreateFileW(argv[3], GENERIC_WRITE, FILE_SHARE_READ, NULL,
	        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE != ready) {
		CloseHandle(ready);
	}
	while (GetTickCount64() < end) {
		if (MsgWaitForMultipleObjects(0, NULL, FALSE, 200,
		            QS_ALLINPUT) == WAIT_OBJECT_0) {
			while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
				if (WM_QUIT == msg.message) {
					CloseHandle(h);
					return 0;
				}
				DispatchMessageW(&msg);
			}
		}
	}
	CloseHandle(h);
	return 0;
}

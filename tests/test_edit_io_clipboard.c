/* Faults in the real editor adapter, without reading or replacing the clipboard. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "base/wf_macro.h"
#include "edit/wf_edraft_win.h"
#include "edit/wf_edraftw_win.h"
#include "edit/wf_esession.h"
#include "wf_test.h"

typedef enum
{
	CLIP_NORMAL,
	PASTE_QUERY_FAIL,
	PASTE_READ_FAIL,
	COPY_QUERY_FAIL,
	COPY_READ_FAIL,
	EMPTY_FAIL,
	WINDOW_FAIL
} ClipboardFault;
static ClipboardFault clipboard_fault;
static wchar_t clipboard_text[8];
static size_t clipboard_bytes;
static unsigned opens, closes, unlocks, frees, publications, empties, windows,
        destroyed;
static HWND opened_owner;
static HGLOBAL published_memory;

static BOOL WINAPI clipboard_open(HWND owner)
{
	opened_owner = owner;
	opens++;
	return TRUE;
}

static BOOL WINAPI clipboard_close(void)
{
	closes++;
	return TRUE;
}

static BOOL WINAPI clipboard_format(UINT format)
{
	return CF_UNICODETEXT == format;
}

static HANDLE WINAPI clipboard_data(UINT format)
{
	CHECK(CF_UNICODETEXT == format);
	return (HANDLE)(uintptr_t)1;
}

static LPVOID WINAPI clipboard_lock(HGLOBAL mem)
{
	return mem == (HGLOBAL)(uintptr_t)1 ? clipboard_text : GlobalLock(mem);
}

static SIZE_T WINAPI clipboard_size(HGLOBAL mem)
{
	return mem == (HGLOBAL)(uintptr_t)1 ? clipboard_bytes : GlobalSize(mem);
}

static BOOL WINAPI clipboard_unlock(HGLOBAL mem)
{
	unlocks++;
	return mem == (HGLOBAL)(uintptr_t)1 ? TRUE : GlobalUnlock(mem);
}

static HGLOBAL WINAPI clipboard_free(HGLOBAL mem)
{
	frees++;
	return GlobalFree(mem);
}

static BOOL WINAPI clipboard_empty(void)
{
	empties++;
	return clipboard_fault != EMPTY_FAIL;
}

static HANDLE WINAPI clipboard_publish(UINT format, HANDLE mem)
{
	CHECK(format == CF_UNICODETEXT);
	publications++;
	/* Model the documented owner requirement. */
	if (!opened_owner) {
		return NULL;
	}
	published_memory = mem;
	return mem;
}

static HWND WINAPI clipboard_window(DWORD exstyle,
        LPCWSTR class_name,
        LPCWSTR title,
        DWORD style,
        int x,
        int y,
        int width,
        int height,
        HWND parent,
        HMENU menu,
        HINSTANCE instance,
        LPVOID param)
{
	(void)exstyle;
	(void)class_name;
	(void)title;
	(void)style;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	(void)menu;
	(void)instance;
	(void)param;
	CHECK(parent == HWND_MESSAGE);
	windows++;
	return clipboard_fault == WINDOW_FAIL ? NULL : (HWND)(uintptr_t)2;
}

static BOOL WINAPI clipboard_destroy(HWND window)
{
	CHECK(window == (HWND)(uintptr_t)2);
	destroyed++;
	return TRUE;
}

static int WINAPI clipboard_from_utf16(UINT cp,
        DWORD flags,
        LPCWCH input,
        int input_n,
        LPSTR out,
        int cap,
        LPCCH fallback,
        LPBOOL used)
{
	if ((clipboard_fault == PASTE_QUERY_FAIL && !out) ||
	        (clipboard_fault == PASTE_READ_FAIL && out)) {
		SetLastError(ERROR_ACCESS_DENIED);
		return 0;
	}
	return WideCharToMultiByte(
	        cp, flags, input, input_n, out, cap, fallback, used);
}

static int WINAPI clipboard_to_utf16(
        UINT cp, DWORD flags, LPCCH input, int input_n, LPWSTR out, int cap)
{
	if ((clipboard_fault == COPY_QUERY_FAIL && !out) ||
	        (clipboard_fault == COPY_READ_FAIL && out)) {
		SetLastError(ERROR_ACCESS_DENIED);
		return 0;
	}
	return MultiByteToWideChar(cp, flags, input, input_n, out, cap);
}

#define OpenClipboard clipboard_open
#define CloseClipboard clipboard_close
#define IsClipboardFormatAvailable clipboard_format
#define GetClipboardData clipboard_data
#define GlobalLock clipboard_lock
#define GlobalSize clipboard_size
#define GlobalUnlock clipboard_unlock
#define GlobalFree clipboard_free
#define EmptyClipboard clipboard_empty
#define SetClipboardData clipboard_publish
#define CreateWindowExW clipboard_window
#define DestroyWindow clipboard_destroy
#define WideCharToMultiByte clipboard_from_utf16
#define MultiByteToWideChar clipboard_to_utf16
#ifndef WF_EDIT_IO_SOURCE
#	define WF_EDIT_IO_SOURCE "../src/edit/wf_eio_win.c"
#endif
#include WF_EDIT_IO_SOURCE
#undef MultiByteToWideChar
#undef WideCharToMultiByte
#undef DestroyWindow
#undef CreateWindowExW
#undef SetClipboardData
#undef EmptyClipboard
#undef GlobalFree
#undef GlobalUnlock
#undef GlobalSize
#undef GlobalLock
#undef GetClipboardData
#undef IsClipboardFormatAvailable
#undef CloseClipboard
#undef OpenClipboard

static void reset_clipboard(ClipboardFault fault)
{
	CHECK(!published_memory);
	clipboard_fault = fault;
	wcscpy_s(clipboard_text, WF_ARRAY_LEN(clipboard_text), L"abc");
	clipboard_bytes = 4 * sizeof *clipboard_text;
	opens = closes = unlocks = frees = publications = empties = windows =
	        destroyed = 0;
	opened_owner = NULL;
}

static void paste_errors_return_no_text(void)
{
	const ClipboardFault faults[] = {PASTE_QUERY_FAIL, PASTE_READ_FAIL};
	WfEditIo io = wf_edit_io_system();
	char *text = NULL;
	size_t n = 0;

	for (size_t i = 0; i < WF_ARRAY_LEN(faults); ++i) {
		reset_clipboard(faults[i]);
		CHECK(WF_E_OS == io.clip_get(io.ctx, &text, &n));
		CHECK(!text && !n && opens == 1 && closes == 1 && unlocks == 1);
	}
	reset_clipboard(CLIP_NORMAL);
	clipboard_text[0] = 0xD800;
	clipboard_text[1] = 0;
	CHECK(WF_E_INVAL == io.clip_get(io.ctx, &text, &n));
	CHECK(!text && !n && closes == 1 && unlocks == 1);
	reset_clipboard(CLIP_NORMAL);
	clipboard_text[0] = 0;
	clipboard_bytes = sizeof *clipboard_text;
	CHECK(WF_OK == io.clip_get(io.ctx, &text, &n));
	CHECK(text && !text[0] && !n);
	free(text);
	reset_clipboard(CLIP_NORMAL);
	clipboard_bytes = 3 * sizeof *clipboard_text;
	CHECK(WF_E_INVAL == io.clip_get(io.ctx, &text, &n));
	CHECK(!text && !n && closes == 1 && unlocks == 1);
}

static void copy_errors_do_not_publish(void)
{
	const ClipboardFault faults[] = {
	        COPY_QUERY_FAIL, COPY_READ_FAIL, EMPTY_FAIL, WINDOW_FAIL};
	WfEditIo io = wf_edit_io_system();

	for (size_t i = 0; i < WF_ARRAY_LEN(faults); ++i) {
		reset_clipboard(faults[i]);
		CHECK(WF_E_OS == io.clip_set(io.ctx, "a\nb", 3));
		CHECK(!published_memory && !publications);
		CHECK(frees == (faults[i] == COPY_QUERY_FAIL ? 0u : 1u));
		CHECK(closes == opens);
		CHECK(destroyed == (faults[i] == EMPTY_FAIL ? 1u : 0u));
	}
	reset_clipboard(CLIP_NORMAL);
	CHECK(WF_E_INVAL == io.clip_set(io.ctx, "\xFF", 1));
	CHECK(!opens && !windows && !published_memory);
}

static void text_round_trip_uses_an_owner(void)
{
	WfEditIo io = wf_edit_io_system();
	char *text = NULL;
	size_t n = 0;

	reset_clipboard(CLIP_NORMAL);
	CHECK(WF_OK == io.clip_get(io.ctx, &text, &n));
	CHECK(text && n == 3 && !memcmp(text, "abc", 3));
	free(text);
	CHECK(opens == 1 && closes == 1 && unlocks == 1 && !windows);
	reset_clipboard(CLIP_NORMAL);
	CHECK(WF_OK == io.clip_set(io.ctx, "a\nb", 3));
	CHECK(publications == 1 && windows == 1 && destroyed == 1 && !frees);
	CHECK(opens == 1 && closes == 1 && opened_owner);
	if (published_memory) {
		const wchar_t *value = GlobalLock(published_memory);
		CHECK(value && !wcscmp(value, L"a\r\nb"));
		if (value) {
			GlobalUnlock(published_memory);
		}
		GlobalFree(published_memory);
		published_memory = NULL;
	}
}

int main(void)
{
	paste_errors_return_no_text();
	copy_errors_do_not_publish();
	text_round_trip_uses_an_owner();
	return TEST_RESULT();
}

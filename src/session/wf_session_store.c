#include "wf_session_store.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../base/wf_slotfile.h"

#define MAGIC "piero-session 1"
#define MAGIC_PREFIX "piero-session "
#define PATH_MAX_UNITS 520

struct WfSessionStore
{
	wchar_t slot[2][PATH_MAX_UNITS];
	wchar_t lock_file[PATH_MAX_UNITS];
	HANDLE lock; /* INVALID_HANDLE_VALUE until locked */
	WfStatus
	        write_error; /* WF_OK, or why existing data must be preserved */
};

WfStatus wf_session_store_open(const wchar_t *state_dir, WfSessionStore **out)
{
	WfSessionStore *s;
	static const wchar_t *const name[3] = {
	        L"session.a", L"session.b", L"session.lock"};
	wchar_t *dest[3];

	*out = NULL;
	if (!state_dir || !*state_dir) {
		return WF_E_INVAL;
	}
	s = calloc(1, sizeof *s);
	if (!s) {
		return WF_E_NOMEM;
	}
	s->lock = INVALID_HANDLE_VALUE;
	dest[0] = s->slot[0];
	dest[1] = s->slot[1];
	dest[2] = s->lock_file;
	for (int i = 0; i < 3; i++) {
		if (swprintf(dest[i], PATH_MAX_UNITS, L"%ls\\%ls", state_dir,
		            name[i]) < 0) {
			free(s);
			return WF_E_RANGE;
		}
	}
	*out = s;
	return WF_OK;
}

void wf_session_store_close(WfSessionStore *s)
{
	if (!s) {
		return;
	}
	if (INVALID_HANDLE_VALUE != s->lock) {
		CloseHandle(s->lock);
	}
	free(s);
}

/* Metadata failure is unsafe, just as a reparse point is. */
static bool is_link(HANDLE h)
{
	BY_HANDLE_FILE_INFORMATION info;

	return !GetFileInformationByHandle(h, &info) ||
	        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool wf_session_store_lock(WfSessionStore *s)
{
	HANDLE h;
	WfSession saved;

	if (INVALID_HANDLE_VALUE != s->lock) {
		return true;
	}
	/* sharing read only: a second opener that asks to write is refused for
	 * as long as this handle is open, and no longer */
	h = CreateFileW(s->lock_file, GENERIC_WRITE, FILE_SHARE_READ, NULL,
	        OPEN_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
	                FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	if (is_link(h)) { /* a name that was swapped for a link is not ours */
		CloseHandle(h);
		return false;
	}
	s->lock = h;
	/* Persistence without --continue must also preserve damaged/unsupported
	 * data. Recheck after acquiring the writer lock, before any checkpoint. */
	(void)wf_session_store_read(s, &saved, NULL);
	wf_session_free(&saved);
	return true;
}

bool wf_session_store_locked(const WfSessionStore *s)
{
	return INVALID_HANDLE_VALUE != s->lock;
}

typedef enum
{
	PEEK_ABSENT, /* missing or empty */
	PEEK_FAILED, /* an existing header could not be opened/read */
	PEEK_OURS,   /* starts with this version's magic */
	PEEK_NEWER, /* "piero-session N" with another N, including older versions */
	PEEK_OTHER /* something else */
} Peek;

/* The first line of a slot file, to tell "newer version" from "damaged". */
static Peek peek(const wchar_t *file, int *version)
{
	HANDLE h = CreateFileW(file, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	char head[32];
	DWORD got = 0;
	size_t prefix = strlen(MAGIC_PREFIX);

	if (INVALID_HANDLE_VALUE == h) {
		DWORD error = GetLastError();

		return ERROR_FILE_NOT_FOUND == error ||
		                ERROR_PATH_NOT_FOUND == error
		        ? PEEK_ABSENT
		        : PEEK_FAILED;
	}
	if (!ReadFile(h, head, sizeof head - 1, &got, NULL)) {
		CloseHandle(h);
		return PEEK_FAILED;
	}
	CloseHandle(h);
	if (!got) {
		return PEEK_ABSENT;
	}
	head[got] = '\0';
	if (!strncmp(head, MAGIC "\n", strlen(MAGIC) + 1)) {
		return PEEK_OURS;
	}
	if (!strncmp(head, MAGIC_PREFIX, prefix) && head[prefix] >= '0' &&
	        head[prefix] <= '9') {
		size_t at = prefix;
		int number = 0;

		while (head[at] >= '0' && head[at] <= '9') {
			int digit = head[at++] - '0';

			if (number > (INT_MAX - digit) / 10) {
				return PEEK_OTHER;
			}
			number = number * 10 + digit;
		}
		if ('\n' != head[at] || number == WF_SESSION_VERSION) {
			return PEEK_OTHER;
		}
		if (version) {
			*version = number;
		}
		return PEEK_NEWER;
	}
	return PEEK_OTHER;
}

WfSessionFound wf_session_store_read(
        WfSessionStore *s, WfSession *out, int *version)
{
	WfSlotFile sl[2];
	Peek p[2];
	int pick;
	WfStatus st;

	memset(out, 0, sizeof *out);
	for (int i = 0; i < 2; i++) {
		p[i] = peek(s->slot[i], version);
	}
	/* An unsupported version in either header blocks decoding and writing,
	 * regardless of what the other slot holds. */
	if (PEEK_NEWER == p[0] || PEEK_NEWER == p[1]) {
		s->write_error = WF_E_FORMAT;
		return WF_SESSION_UNSUPPORTED;
	}
	if (PEEK_FAILED == p[0] || PEEK_FAILED == p[1]) {
		s->write_error = WF_E_CORRUPT;
		return WF_SESSION_DAMAGED;
	}
	if (PEEK_ABSENT == p[0] && PEEK_ABSENT == p[1]) {
		return WF_SESSION_NONE;
	}
	wf_slotfile_read(s->slot[0], MAGIC, WF_SESSION_MAX_BYTES, &sl[0]);
	wf_slotfile_read(s->slot[1], MAGIC, WF_SESSION_MAX_BYTES, &sl[1]);
	pick = wf_slotfile_pick(&sl[0], &sl[1]);
	if (pick < 0) {
		wf_slotfile_free(&sl[0]);
		wf_slotfile_free(&sl[1]);
		s->write_error = WF_E_CORRUPT;
		return WF_SESSION_DAMAGED;
	}
	st = wf_session_decode(sl[pick].body, sl[pick].body_n, out);
	wf_slotfile_free(&sl[0]);
	wf_slotfile_free(&sl[1]);
	if (WF_OK != st) {
		s->write_error = st;
	}
	return WF_OK == st ? WF_SESSION_FOUND : WF_SESSION_DAMAGED;
}

WfStatus wf_session_store_write(WfSessionStore *s, const char *body, size_t n)
{
	const wchar_t *files[2] = {s->slot[0], s->slot[1]};

	if (INVALID_HANDLE_VALUE == s->lock) {
		return WF_E_ACCESS;
	}
	if (WF_OK != s->write_error) {
		return s->write_error;
	}
	return wf_slotfile_write(
	        files, MAGIC, NULL, NULL, body, n, WF_SESSION_MAX_BYTES);
}

WfStatus wf_session_store_forget(WfSessionStore *s)
{
	WfStatus st = WF_OK;

	if (INVALID_HANDLE_VALUE == s->lock) {
		return WF_E_ACCESS;
	}
	for (int i = 0; i < 2; i++) {
		/* the same check as a write makes: nothing a link or a second name
		 * leads to is emptied */
		WfStatus one = wf_slotfile_empty(s->slot[i]);

		if (WF_OK != one) {
			st = one;
		}
	}
	if (WF_OK == st) {
		s->write_error = WF_OK;
	} else if (WF_OK == s->write_error) {
		s->write_error = WF_E_CORRUPT;
	}
	return st;
}

#include "wf_watch_win.h"

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../path/wf_path.h"

#define WATCH_BUFFER_BYTES 4096u
/* How long a close waits for the cancelled request to finish. It ends at once
 * for a local folder; a share or a filter driver that is slow to answer must
 * not hold the screen (the close is made while the user moves between folders). */
#define WATCH_CLOSE_WAIT_MS 250u
/* Watches whose request had not finished when they were closed. */
#define WATCH_LATE_MAX 16

#define WATCH_FILTER                                                           \
	(FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |          \
	        FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |      \
	        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SECURITY)

struct WfWatch
{
	HANDLE dir;
	HANDLE ev;
	OVERLAPPED ov;
	bool armed; /* a request is outstanding and `buf` is in use by the
	               kernel */
	DWORD buf[WATCH_BUFFER_BYTES /
	        sizeof(DWORD)]; /* DWORD-aligned; contents are
     ignored */
};

static void reap(void);

static bool arm(WfWatch *w)
{
	DWORD ignored;

	memset(&w->ov, 0, sizeof w->ov);
	w->ov.hEvent = w->ev;
	if (!ReadDirectoryChangesW(w->dir, w->buf, sizeof w->buf, TRUE,
	            WATCH_FILTER, &ignored, &w->ov, NULL)) {
		return false;
	}
	w->armed = true;
	return true;
}

WfStatus wf_watch_open(const wchar_t *dir, WfWatch **out)
{
	WfWatch *w;
	wchar_t *ext = NULL;
	WfStatus st;

	if (!dir || !out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	reap();
	if (wf_path_host_invalid(dir)) {
		return WF_E_INVAL; /* "This PC", an SFTP place: no folder to open */
	}
	w = calloc(1, sizeof *w);
	st = wf_path_to_extended(dir, &ext);
	if (WF_OK != st) {
		free(w);
		return st;
	}
	if (!w) {
		free(ext);
		return WF_E_NOMEM;
	}
	w->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
	w->dir = CreateFileW(ext, FILE_LIST_DIRECTORY,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
	free(ext);
	if (!w->ev || INVALID_HANDLE_VALUE == w->dir || !arm(w)) {
		if (INVALID_HANDLE_VALUE != w->dir) {
			CloseHandle(w->dir);
		}
		if (w->ev) {
			CloseHandle(w->ev);
		}
		free(w);
		return WF_E_OS;
	}
	*out = w;
	return WF_OK;
}

static void release(WfWatch *w)
{
	CloseHandle(w->dir);
	CloseHandle(w->ev);
	free(w);
}

/* Watches closed while their request was still the kernel's. Their memory (the
 * buffer and the OVERLAPPED) is used until it completes, so it is kept, not
 * freed under it; it is released when the request has finished. Used from the
 * thread that opens and closes watches, which is one. */
static WfWatch *late[WATCH_LATE_MAX];
static size_t late_n;

static void reap(void)
{
	for (size_t i = 0; i < late_n;) {
		if (HasOverlappedIoCompleted(&late[i]->ov)) {
			release(late[i]);
			late[i] = late[--late_n];
		} else {
			i++;
		}
	}
}

void wf_watch_close(WfWatch *w)
{
	if (!w) {
		return;
	}
	reap();
	if (w->armed) {
		CancelIoEx(w->dir, &w->ov);
		/* The kernel still owns buf/ov until the request completes. */
		if (!HasOverlappedIoCompleted(&w->ov)) {
			WaitForSingleObject(w->ev, WATCH_CLOSE_WAIT_MS);
		}
		if (!HasOverlappedIoCompleted(&w->ov)) {
			/* not yet: the wait is not made longer, and the memory is not
			 * freed. If there is no room to keep it, it is kept all the same,
			 * for good: a lost buffer, never a write into freed memory. */
			if (late_n < WATCH_LATE_MAX) {
				late[late_n++] = w;
			}
			return;
		}
	}
	release(w);
}

HANDLE wf_watch_event(const WfWatch *w)
{
	return w->ev;
}

WfWatchChanges wf_watch_classify(
        const void *data, size_t bytes, const wchar_t *selected)
{
	const unsigned char *p = data;
	const size_t head = offsetof(FILE_NOTIFY_INFORMATION, FileName);
	WfWatchChanges change = {false, false};
	if (!data || (uintptr_t)data % sizeof(DWORD) || !bytes ||
	        bytes > sizeof(((WfWatch *)0)->buf)) {
		return (WfWatchChanges){true, true};
	}
	for (;;) {
		DWORD next, action, length;
		if (bytes < head) {
			break;
		}
		memcpy(&next, p, sizeof next);
		memcpy(&action, p + sizeof(DWORD), sizeof action);
		memcpy(&length, p + 2 * sizeof(DWORD), sizeof length);
		if (!length || length % sizeof(wchar_t) ||
		        length > bytes - head || action < FILE_ACTION_ADDED ||
		        action > FILE_ACTION_RENAMED_NEW_NAME) {
			break;
		}
		/* The kernel's byte buffer is not a C wchar_t object. */
		wchar_t name[WATCH_BUFFER_BYTES / sizeof(wchar_t)];
		memcpy(name, p + head, length);
		size_t units = length / sizeof(wchar_t), leaf = 0;
		for (size_t i = 0; i < units; i++) {
			/* An alias cannot safely exclude the selected file. */
			if (!name[i] || name[i] == L'~') {
				return (WfWatchChanges){true, true};
			}
		}
		while (leaf < units && name[leaf] != L'\\' &&
		        name[leaf] != L'/' && name[leaf] != L':') {
			leaf++;
		}
		change.listing |=
		        leaf == units || (leaf < units && name[leaf] == L':');
		if (selected && leaf == wcslen(selected) &&
		        CSTR_EQUAL ==
		                CompareStringOrdinal(name, (int)leaf, selected,
		                        (int)leaf, TRUE)) {
			change.preview = true;
		}
		if (!next) {
			return change;
		}
		if (next % sizeof(DWORD) || next < head + length ||
		        next >= bytes) {
			break;
		}
		p += next;
		bytes -= next;
	}
	return (WfWatchChanges){true, true};
}

bool wf_watch_consume_selected(
        WfWatch *w, const wchar_t *selected, WfWatchChanges *changes)
{
	DWORD n;
	BOOL ok;

	if (!w || !w->armed || !changes) {
		return false;
	}
	ok = GetOverlappedResult(w->dir, &w->ov, &n, FALSE);
	if (!ok && ERROR_IO_INCOMPLETE == GetLastError()) {
		return false; /* spurious wake */
	}
	w->armed = false;
	if (!ok && ERROR_NOTIFY_ENUM_DIR != GetLastError()) {
		return false; /* folder gone or handle dead: stop watching */
	}
	*changes = wf_watch_classify(ok ? w->buf : NULL, ok ? n : 0, selected);
	(void)arm(w); /* ok, or overflow (n == 0): both mean "changed"; a failed
	               * re-arm leaves armed == false, so the next consume
	               * reports the death */
	return true;
}

bool wf_watch_active(const WfWatch *w)
{
	return w && w->armed;
}

bool wf_watch_consume(WfWatch *w)
{
	WfWatchChanges changes;
	return wf_watch_consume_selected(w, NULL, &changes);
}

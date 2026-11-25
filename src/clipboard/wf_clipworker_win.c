#define COBJMACROS
#include "../async/wf_workcore.h"
#include "wf_clipworker_win.h"
#include "wf_dataobj_files_win.h"

#include <objidl.h>
#include <ole2.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>

#define STOP_WAIT_MS 500

struct WfClipWorker
{
	WfWorkCore core;
	/* The apartment's thread sleeps in MsgWaitForMultipleObjects, not on the
	 * core's condition variable: an STA that does not pump messages does not get
	 * the clipboard's own notifications, and its reads begin to fail. This event
	 * is what wakes it for a request or a stop. */
	HANDLE wake;
	bool has_req;
	uint32_t req_gen;
	wchar_t **req_path; /* a publish: the names (owned); NULL: a read */
	size_t req_n;
	IDataObject *
	        owned; /* what this worker put on the clipboard (one reference) */
	bool has_result;
	WfClipFiles result; /* owned until polled */
};

static void worker_free(void *owner)
{
	WfClipWorker *w = owner;

	wf_clipfiles_free(&w->result);
	for (size_t i = 0; w->req_path && i < w->req_n; i++) {
		free(w->req_path[i]);
	}
	free(w->req_path);
	if (w->wake) {
		CloseHandle(w->wake);
	}
	free(w);
}

/*
 * The classic clipboard calls, not OleGetClipboard. OLE keeps its own view of the
 * clipboard and learns of a change through a message, so a read just after a change
 * (or from a thread that has not served that message) could return the data of the
 * one before: that was seen in testing, and it is the one failure a paste must not
 * have. OpenClipboard / GetClipboardData always show what is on the clipboard now,
 * and Explorer's OLE data is rendered to them by the system. The clipboard is open
 * only while its blocks are copied out, and a change during the copy (the sequence
 * number moved) makes the read start again.
 */

static CLIPFORMAT format_of(const wchar_t *name)
{
	return (CLIPFORMAT)RegisterClipboardFormatW(name);
}

/* The first bytes of one block of the open clipboard, copied out (at most
 * `want`), or NULL. A block larger than `limit` is not copied: *too_big says so. */
static void *block_of(
        UINT format, size_t want, size_t limit, size_t *bytes, bool *too_big)
{
	HANDLE h = GetClipboardData(format);
	SIZE_T size;
	const void *src;
	void *copy = NULL;

	*bytes = 0;
	if (!h || 0 == (size = GlobalSize(h))) {
		return NULL;
	}
	if (size > limit) {
		*too_big = true;
		return NULL;
	}
	if (size > want) {
		size = want;
	}
	src = GlobalLock(h);
	if (src) {
		copy = malloc(size);
		if (copy) {
			memcpy(copy, src, size);
			*bytes = size;
		}
		GlobalUnlock(h);
	}
	return copy;
}

/* Opens the clipboard, trying for a short while (another program may have it). */
static bool open_for_reading(void)
{
	for (int i = 0; i < 25; i++) {
		if (OpenClipboard(NULL)) {
			return true;
		}
		Sleep(20);
	}
	return false;
}

/* Reads the clipboard once into `out` (gen is set by the caller). */
static void read_once(WfClipFiles *out)
{
	DWORD seq;
	void *list = NULL, *effect = NULL;
	size_t list_n = 0, effect_n = 0;
	bool has_files, has_virtual, too_big = false;
	CLIPFORMAT cf_effect = format_of(CFSTR_PREFERREDDROPEFFECT);

	memset(out, 0, sizeof *out);
	if (!open_for_reading()) {
		out->why = WF_CLIP_BUSY;
		return;
	}
	seq = GetClipboardSequenceNumber();
	has_files = IsClipboardFormatAvailable(CF_HDROP);
	has_virtual =
	        IsClipboardFormatAvailable(format_of(CFSTR_FILEDESCRIPTORW));
	if (has_files) {
		list = block_of(CF_HDROP, SIZE_MAX, WF_CLIP_HDROP_MAX_BYTES,
		        &list_n, &too_big);
		if (list && IsClipboardFormatAvailable(cf_effect)) {
			effect = block_of(cf_effect, sizeof(DWORD), SIZE_MAX,
			        &effect_n, &too_big);
		}
	}
	CloseClipboard();
	if (GetClipboardSequenceNumber() != seq) {
		free(list);
		free(effect);
		out->why =
		        WF_CLIP_FAILED; /* it changed while it was read: again */
		return;
	}
	if (!has_files) {
		out->why = has_virtual ? WF_CLIP_UNSUPPORTED : WF_CLIP_NONE;
		return;
	}
	if (!list) {
		out->why = too_big ? WF_CLIP_BAD : WF_CLIP_FAILED;
		free(effect);
		return;
	}
	out->why = wf_clip_parse_hdrop(list, list_n, out);
	if (WF_CLIP_OK == out->why && effect) {
		out->effect = wf_clip_parse_effect(effect, effect_n);
	}
	free(list);
	free(effect);
}

/* Serves the apartment's pending messages without waiting. */
static void pump_pending(void)
{
	MSG m;

	while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&m);
		DispatchMessageW(&m);
	}
}

/* Just after the clipboard changes, OLE's own view of it is still catching up
 * (it learns of the change through a message), and a read in that moment can
 * fail or find it busy. That is retried a few times, a short while apart, and
 * only for those two outcomes: an answer that is a verdict (no files, damaged,
 * too many) is never retried, and nothing is retried when nobody asked. */
#define READ_TRIES 6
#define READ_PAUSE_MS 40

static void read_clipboard(WfClipFiles *out)
{
	for (int tries = 1;; tries++) {
		pump_pending();
		read_once(out);
		if ((WF_CLIP_BUSY != out->why && WF_CLIP_FAILED != out->why) ||
		        tries >= READ_TRIES) {
			return;
		}
		wf_clipfiles_free(out);
		Sleep(READ_PAUSE_MS);
	}
}

/* Waits for the wake event with no timeout (no polling), serving the thread's
 * messages as they come. */
static void pump_until_woken(WfClipWorker *w)
{
	for (;;) {
		DWORD r = MsgWaitForMultipleObjects(
		        1, &w->wake, FALSE, INFINITE, QS_ALLINPUT);
		MSG m;

		if (WAIT_OBJECT_0 == r || WAIT_FAILED == r) {
			return;
		}
		while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
	}
}

/* Puts `n` names on the clipboard as a copy. OLE keeps the object and asks it for
 * the formats when a receiver wants them, from this thread's apartment (which is
 * why the thread serves messages). Names only; nothing of any file is held. A
 * clipboard that is open elsewhere is tried a few times. */
static void publish_files(
        WfClipWorker *w, WfClipFiles *out, const wchar_t *const *path, size_t n)
{
	IDataObject *pdo = NULL;
	HRESULT hr = E_FAIL;

	memset(out, 0, sizeof *out);
	out->published = true;
	if (WF_OK != wf_dataobj_files_create(path, n, &pdo)) {
		out->why = WF_CLIP_BAD;
		return;
	}
	for (int tries = 0; tries < 6; tries++) {
		hr = OleSetClipboard(pdo);
		if (S_OK == hr || 0x800401D0ul != (unsigned long)hr) {
			break;
		}
		Sleep(40); /* CLIPBRD_E_CANT_OPEN: another program has it open */
	}
	if (S_OK != hr) {
		IDataObject_Release(pdo);
		out->why = 0x800401D0ul == (unsigned long)hr ? WF_CLIP_BUSY
		                                             : WF_CLIP_FAILED;
		return;
	}
	/* the one reference this worker keeps, to flush at the end and then release */
	if (w->owned) {
		IDataObject_Release(w->owned);
	}
	w->owned = pdo;
	out->why = WF_CLIP_OK;
	out->n = n;
}

/* At the end, if what this worker put there is still on the clipboard, the Shell
 * is given the data itself (OleFlushClipboard renders every format into the
 * clipboard) so that a paste after piero has ended still works. The cost is the
 * names, a few kilobytes; if another program has since put something else there,
 * nothing is flushed and nothing of the other program's is touched. */
static void flush_owned(WfClipWorker *w)
{
	if (!w->owned) {
		return;
	}
	if (S_OK == OleIsCurrentClipboard(w->owned)) {
		(void)OleFlushClipboard();
	}
	IDataObject_Release(w->owned);
	w->owned = NULL;
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	WfClipWorker *w = arg;
	bool ole = SUCCEEDED(OleInitialize(NULL));

	for (;;) {
		uint32_t gen;
		WfClipFiles files;
		wchar_t **names;
		size_t names_n;

		AcquireSRWLockExclusive(&w->core.lock);
		while (!w->core.stop && !w->has_req) {
			ReleaseSRWLockExclusive(&w->core.lock);
			pump_until_woken(w);
			AcquireSRWLockExclusive(&w->core.lock);
		}
		if (w->core.stop) {
			ReleaseSRWLockExclusive(&w->core.lock);
			break;
		}
		gen = w->req_gen;
		names = w->req_path;
		names_n = w->req_n;
		w->req_path = NULL;
		w->req_n = 0;
		w->has_req = false;
		w->core.busy = true;
		ReleaseSRWLockExclusive(&w->core.lock);

		if (names) {
			if (ole) {
				publish_files(w, &files,
				        (const wchar_t *const *)names, names_n);
			} else {
				memset(&files, 0, sizeof files);
				files.published = true;
				files.why = WF_CLIP_FAILED;
			}
			for (size_t i = 0; i < names_n; i++) {
				free(names[i]);
			}
			free(names);
		} else {
			read_clipboard(&files);
		}
		files.gen = gen;

		AcquireSRWLockExclusive(&w->core.lock);
		w->core.busy = false;
		if (w->has_result) {
			wf_clipfiles_free(&w->result); /* a newer answer wins */
			if (UINT64_MAX != w->core.stats.discarded_results) {
				w->core.stats.discarded_results++;
			}
		}
		w->result = files;
		w->has_result = true;
		if (UINT64_MAX != w->core.stats.completed_requests) {
			w->core.stats.completed_requests++;
		}
		SetEvent(w->core.ready);
		ReleaseSRWLockExclusive(&w->core.lock);
	}
	flush_owned(w);
	if (ole) {
		OleUninitialize();
	}
	wf_wc_release(&w->core, worker_free, w);
	return 0;
}

WfStatus wf_clipworker_start(WfClipWorker **out)
{
	WfClipWorker *w;

	*out = NULL;
	w = calloc(1, sizeof *w);
	if (!w) {
		return WF_E_NOMEM;
	}
	w->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!w->wake || WF_OK != wf_wc_start(&w->core, thread_main, w)) {
		if (w->wake) {
			CloseHandle(w->wake);
		}
		free(w);
		return WF_E_OS;
	}
	*out = w;
	return WF_OK;
}

WfWorkerStopReport wf_clipworker_stop(WfClipWorker *w)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	/* the stop is set before the thread is woken, so it cannot go back to sleep */
	AcquireSRWLockExclusive(&w->core.lock);
	w->core.stop = true;
	ReleaseSRWLockExclusive(&w->core.lock);
	SetEvent(w->wake);
	return wf_wc_stop(&w->core, STOP_WAIT_MS, false, worker_free, w);
}

HANDLE wf_clipworker_event(const WfClipWorker *w)
{
	return w->core.ready;
}

bool wf_clipworker_idle(WfClipWorker *w)
{
	bool idle;

	if (!w) {
		return true;
	}
	AcquireSRWLockShared(&w->core.lock);
	idle = !w->core.busy && !w->has_req && !w->has_result && !w->owned;
	ReleaseSRWLockShared(&w->core.lock);
	return idle;
}

bool wf_clipworker_poll(WfClipWorker *w, WfClipFiles *out)
{
	bool got = false;

	if (!w || !out) {
		return false;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->has_result) {
		*out = w->result;
		memset(&w->result, 0, sizeof w->result);
		w->has_result = false;
		got = true;
	}
	ResetEvent(w->core.ready);
	ReleaseSRWLockExclusive(&w->core.lock);
	return got;
}

static WfStatus port_read(void *ctx, uint32_t gen)
{
	WfClipWorker *w = ctx;

	if (!w || !gen) {
		return WF_E_INVAL;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		return WF_E_INVAL;
	}
	if (w->has_req && UINT64_MAX != w->core.stats.superseded_requests) {
		w->core.stats.superseded_requests++;
	}
	for (size_t i = 0; w->req_path && i < w->req_n; i++) {
		free(w->req_path[i]);
	}
	free(w->req_path);
	w->req_path = NULL;
	w->req_n = 0;
	w->has_req = true;
	w->req_gen = gen;
	ReleaseSRWLockExclusive(&w->core.lock);
	SetEvent(w->wake);
	return WF_OK;
}

static WfStatus port_publish(
        void *ctx, uint32_t gen, const wchar_t *const *path, size_t n)
{
	WfClipWorker *w = ctx;
	wchar_t **copy;

	if (!w || !gen || !path || !n || n > WF_CLIPFILES_MAX) {
		return WF_E_INVAL;
	}
	copy = calloc(n, sizeof *copy);
	if (!copy) {
		return WF_E_NOMEM;
	}
	for (size_t i = 0; i < n; i++) {
		copy[i] = path[i] ? _wcsdup(path[i]) : NULL;
		if (!copy[i]) {
			for (size_t k = 0; k < i; k++) {
				free(copy[k]);
			}
			free(copy);
			return WF_E_NOMEM;
		}
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		for (size_t i = 0; i < n; i++) {
			free(copy[i]);
		}
		free(copy);
		return WF_E_INVAL;
	}
	for (size_t i = 0; w->req_path && i < w->req_n; i++) {
		free(w->req_path[i]);
	}
	free(w->req_path);
	w->req_path = copy;
	w->req_n = n;
	w->has_req = true;
	w->req_gen = gen;
	ReleaseSRWLockExclusive(&w->core.lock);
	SetEvent(w->wake);
	return WF_OK;
}

WfClipFilesPort wf_clipworker_port(WfClipWorker *w)
{
	WfClipFilesPort p = {w, port_read, port_publish};

	return p;
}

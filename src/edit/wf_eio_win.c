/* The editor's real file and clipboard access (see wf_esession.h). */
#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "../async/wf_blocking.h"
#include "wf_edraft_win.h"
#include "wf_edraftw_win.h"
#include "wf_esession.h"

#define CLIP_MAX (8u << 20)
#define BUSY_AFTER_MS 120u
#define BUSY_TICK_MS 50u

/* The one place the screen is told that a file job is taking long. */
static struct
{
	WfEditBusy fn;
	void *ctx;
} g_busy;

void wf_edit_io_set_busy(WfEditBusy fn, void *ctx)
{
	g_busy.fn = fn;
	g_busy.ctx = ctx;
}

static bool busy_tick(void *ctx, unsigned elapsed_ms)
{
	const bool *saving = ctx;

	return g_busy.fn(g_busy.ctx, *saving, elapsed_ms);
}

static WfStatus run_waiting(WfBlockingFn fn, void *arg, bool saving)
{
	WfBlockingWait w = {BUSY_AFTER_MS, BUSY_TICK_MS, NULL, &saving};

	if (g_busy.fn) {
		w.tick = busy_tick;
	}
	return wf_blocking_run(fn, arg, &w);
}

typedef struct
{
	const wchar_t *path;
	size_t max_bytes;
	unsigned cp;
	const WfEncoding *force;
	WfLoaded *out;
} LoadJob;

static WfStatus load_run(void *p)
{
	LoadJob *j = p;

	return wf_efile_load(j->path, j->max_bytes, j->cp, j->force, j->out);
}

typedef struct
{
	const wchar_t *path;
	const WfFileStamp *was;
	const char *utf8;
	size_t n;
	WfEncoding enc;
	const WfSaveOptions *opt;
	WfSaveResult *out;
} SaveJob;

static WfStatus save_over_run(void *p)
{
	SaveJob *j = p;

	return wf_efile_save_over(
	        j->path, j->was, j->utf8, j->n, j->enc, j->opt, j->out);
}

static WfStatus save_new_run(void *p)
{
	SaveJob *j = p;

	return wf_efile_save_new(j->path, j->utf8, j->n, j->enc, j->out);
}

static WfStatus load(void *ctx,
        const wchar_t *path,
        size_t max_bytes,
        unsigned cp,
        const WfEncoding *force,
        WfLoaded *out)
{
	LoadJob j = {path, max_bytes, cp, force, out};

	(void)ctx;
	return run_waiting(load_run, &j, false);
}

static WfStatus save_over(void *ctx,
        const wchar_t *path,
        const WfFileStamp *was,
        const char *utf8,
        size_t n,
        WfEncoding enc,
        const WfSaveOptions *opt,
        WfSaveResult *out)
{
	SaveJob j = {path, was, utf8, n, enc, opt, out};

	(void)ctx;
	return run_waiting(save_over_run, &j, true);
}

static WfStatus save_new(void *ctx,
        const wchar_t *path,
        const char *utf8,
        size_t n,
        WfEncoding enc,
        WfSaveResult *out)
{
	SaveJob j = {path, NULL, utf8, n, enc, NULL, out};

	(void)ctx;
	return run_waiting(save_new_run, &j, true);
}

/* The clipboard may be held by another program for a moment. */
static bool open_clipboard(HWND owner)
{
	int i;

	for (i = 0; i < 10; i++) {
		if (OpenClipboard(owner)) {
			return true;
		}
		Sleep(15);
	}
	return false;
}

static WfStatus clip_get(void *ctx, char **utf8, size_t *n)
{
	HANDLE h;
	const wchar_t *w;
	size_t units, cap;
	int bytes;
	char *out = NULL;
	WfStatus st = WF_OK;

	(void)ctx;
	*utf8 = NULL;
	*n = 0;
	if (!open_clipboard(NULL)) {
		return WF_E_OS;
	}
	if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
		CloseClipboard();
		return WF_E_NOENT;
	}
	h = GetClipboardData(CF_UNICODETEXT);
	w = h ? GlobalLock(h) : NULL;
	if (!w) {
		CloseClipboard();
		return WF_E_OS;
	}
	cap = GlobalSize(h) / sizeof *w;
	for (units = 0; units < cap && units <= CLIP_MAX / 3 && w[units];
	        units++) {
	}
	if (units > CLIP_MAX / 3) {
		st = WF_E_RANGE;
		goto done;
	}
	if (units == cap) {
		st = WF_E_INVAL;
		goto done;
	}
	bytes = units ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w,
	                        (int)units, NULL, 0, NULL, NULL)
	              : 0;
	if (units && bytes <= 0) {
		st = ERROR_NO_UNICODE_TRANSLATION == GetLastError() ? WF_E_INVAL
		                                                    : WF_E_OS;
		goto done;
	}
	out = malloc((size_t)bytes + 1);
	if (!out) {
		st = WF_E_NOMEM;
		goto done;
	}
	if (bytes &&
	        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w,
	                (int)units, out, bytes, NULL, NULL) != bytes) {
		st = ERROR_NO_UNICODE_TRANSLATION == GetLastError() ? WF_E_INVAL
		                                                    : WF_E_OS;
		goto done;
	}
	out[bytes] = '\0';
	*utf8 = out;
	*n = (size_t)bytes;
	out = NULL;
done:
	free(out);
	GlobalUnlock(h);
	CloseClipboard();
	return st;
}

static WfStatus clip_set(void *ctx, const char *utf8, size_t n)
{
	/* Windows programs expect CRLF in clipboard text */
	size_t i, extra = 0, k = 0;
	char *crlf;
	int units;
	HGLOBAL mem;
	wchar_t *dst;
	HWND owner;
	bool published = false;

	(void)ctx;
	if (n > CLIP_MAX) {
		return WF_E_RANGE;
	}
	for (i = 0; i < n; i++) {
		if ('\n' == utf8[i] && (0 == i || '\r' != utf8[i - 1])) {
			extra++;
		}
	}
	crlf = malloc(n + extra + 1);
	if (!crlf) {
		return WF_E_NOMEM;
	}
	for (i = 0; i < n; i++) {
		if ('\n' == utf8[i] && (0 == i || '\r' != utf8[i - 1])) {
			crlf[k++] = '\r';
		}
		crlf[k++] = utf8[i];
	}
	units = k ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, crlf,
	                    (int)k, NULL, 0)
	          : 0;
	if (k && units <= 0) {
		WfStatus st = ERROR_NO_UNICODE_TRANSLATION == GetLastError()
		        ? WF_E_INVAL
		        : WF_E_OS;
		free(crlf);
		return st;
	}
	mem = GlobalAlloc(GMEM_MOVEABLE, ((size_t)units + 1) * sizeof *dst);
	dst = mem ? GlobalLock(mem) : NULL;
	if (!dst) {
		if (mem) {
			GlobalFree(mem);
		}
		free(crlf);
		return WF_E_NOMEM;
	}
	if (k) {
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, crlf,
		            (int)k, dst, units) != units) {
			GlobalUnlock(mem);
			GlobalFree(mem);
			free(crlf);
			return WF_E_OS;
		}
	}
	dst[units] = L'\0';
	GlobalUnlock(mem);
	free(crlf);
	/* EmptyClipboard needs a non-NULL owner for SetClipboardData to succeed.
	 * Immediate rendering lets this message-only window be destroyed below. */
	owner = CreateWindowExW(0, L"STATIC", L"piero clipboard", 0, 0, 0, 0, 0,
	        HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
	if (!owner) {
		GlobalFree(mem);
		return WF_E_OS;
	}
	if (!open_clipboard(owner)) {
		DestroyWindow(owner);
		GlobalFree(mem);
		return WF_E_OS;
	}
	if (EmptyClipboard()) {
		published = NULL != SetClipboardData(CF_UNICODETEXT, mem);
	}
	CloseClipboard();
	DestroyWindow(owner);
	if (!published) {
		GlobalFree(mem);
		return WF_E_OS;
	}
	/* SetClipboardData transferred mem to the system. */
	return WF_OK;
}

static bool draft_allowed(void *ctx, const wchar_t *path, size_t bytes)
{
	(void)ctx;
	return wf_draft_allowed(path, bytes);
}

/* Start once on the first draft submission. A failed start is reported without
 * moving the disk flush onto the UI thread; later submissions keep its error. */
static WfDraftWriter *g_drafts;
static bool g_drafts_tried;
static WfStatus g_drafts_error = WF_E_OS;

static WfDraftWriter *drafts_worker(void)
{
	if (!g_drafts && !g_drafts_tried) {
		g_drafts_tried = true;
		g_drafts_error = wf_draftw_start(&g_drafts);
		if (WF_OK != g_drafts_error) {
			g_drafts = NULL;
		}
	}
	return g_drafts;
}

static WfStatus draft_write(void *ctx,
        const wchar_t *path,
        WfEncoding enc,
        const char *utf8,
        size_t n)
{
	WfDraftWriter *w = drafts_worker();

	(void)ctx;
	if (!w) {
		return g_drafts_error;
	}
	return wf_draftw_submit(w, path, enc, utf8, n);
}

static bool draft_failed(void *ctx, const wchar_t *path)
{
	(void)ctx;
	return wf_draftw_take_failure(g_drafts, path);
}

static WfStatus draft_read(
        void *ctx, const wchar_t *path, WfEncoding *enc, char **utf8, size_t *n)
{
	(void)ctx;
	wf_draftw_settle(
	        g_drafts, path, false); /* what waits is written first */
	return wf_draft_read(path, enc, utf8, n);
}

static void draft_remove(void *ctx, const wchar_t *path)
{
	(void)ctx;
	/* what waits for the disk is dropped and a write under way is waited for,
	 * or the draft would be written again after it was removed */
	wf_draftw_settle(g_drafts, path, true);
	wf_draft_remove(path);
}

void wf_edit_io_drafts_stop(unsigned wait_ms)
{
	WfDraftWriter *w = g_drafts;

	g_drafts = NULL;
	g_drafts_tried = true; /* nothing is started again after the end */
	g_drafts_error = WF_E_INVAL;
	(void)wf_draftw_stop(w, wait_ms);
}

WfEditIo wf_edit_io_system(void)
{
	WfEditIo io = {NULL, load, save_over, save_new, clip_get, clip_set,
	        draft_allowed, draft_write, draft_read, draft_remove,
	        draft_failed};

	return io;
}

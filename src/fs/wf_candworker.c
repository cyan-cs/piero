#include "../async/wf_workcore.h"
#include "wf_candworker.h"

#include <stdlib.h>
#include <string.h>

#include "../base/wf_win32status.h"
#include "../path/wf_path.h"

#define RESULT_CAP 16
#define STOP_WAIT_MS 1000

typedef enum
{
	REQ_NONE,
	REQ_START,
	REQ_MORE
} ReqKind;

struct WfCandWorker
{
	WfWorkCore core;

	ReqKind req;
	uint32_t req_gen;
	wchar_t *req_dir, *req_prefix;
	bool req_hidden, req_dirs_only;

	/* the search the thread is on (read by others only through the lock) */
	volatile LONG
	        live_gen; /* 0: none; the loop reads it without the lock */
	volatile LONG stopping; /* likewise */
	LONG open_handles;

	WfCandBatch q[RESULT_CAP];
	size_t qhead, qn;
};

static void worker_free(void *owner)
{
	WfCandWorker *w = owner;

	free(w->req_dir);
	free(w->req_prefix);
	free(w);
}

/* Caller holds the lock. Only the newest batches matter if the consumer is far
 * behind; a search that is no longer the live one never delivers. */
static void push_batch(WfCandWorker *w, const WfCandBatch *b)
{
	if ((LONG)b->gen != w->live_gen) {
		return;
	}
	if (RESULT_CAP == w->qn) {
		w->qhead = (w->qhead + 1) % RESULT_CAP;
		w->qn--;
		if (UINT64_MAX != w->core.stats.discarded_results) {
			w->core.stats.discarded_results++;
		}
	}
	w->q[(w->qhead + w->qn++) % RESULT_CAP] = *b;
	SetEvent(w->core.ready);
}

/* Search state owned by the worker thread. */

typedef struct
{
	HANDLE find; /* INVALID_HANDLE_VALUE: none */
	uint32_t gen;
	wchar_t prefix[260];
	size_t prefix_n;
	bool include_hidden, dirs_only;
	WIN32_FIND_DATAW
	pending; /* the entry FindFirst returned, not yet looked at */
	bool has_pending;
	bool ended;
} Search;

static void search_close(WfCandWorker *w, Search *s)
{
	if (INVALID_HANDLE_VALUE != s->find) {
		FindClose(s->find);
		InterlockedDecrement(&w->open_handles);
	}
	s->find = INVALID_HANDLE_VALUE;
	s->has_pending = false;
	s->ended = true;
}

/* The shared classes, and a path that is not a folder is "not there" too. */
static WfStatus map_error(DWORD e)
{
	return ERROR_DIRECTORY == e ? WF_E_NOENT : wf_status_from_win32(e);
}

static bool matches(const Search *s, const WIN32_FIND_DATAW *fd)
{
	size_t n;

	if (!wcscmp(fd->cFileName, L".") || !wcscmp(fd->cFileName, L"..")) {
		return false;
	}
	if ((fd->dwFileAttributes &
	            (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) &&
	        !s->include_hidden) {
		return false;
	}
	if (s->dirs_only &&
	        !(fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
		return false;
	}
	if (!s->prefix_n) {
		return true;
	}
	n = wcslen(fd->cFileName);
	if (n < s->prefix_n) {
		return false;
	}
	return CompareStringOrdinal(fd->cFileName, (int)s->prefix_n, s->prefix,
	               (int)s->prefix_n, TRUE) == CSTR_EQUAL;
}

static void fill(WfCandItem *it, const WIN32_FIND_DATAW *fd)
{
	wcsncpy_s(it->name, WF_CAND_NAME, fd->cFileName, _TRUNCATE);
	it->flags = 0;
	if (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
		it->flags |= 1u << 0; /* WF_ENT_DIR */
	}
	if (fd->dwFileAttributes &
	        (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) {
		it->flags |= 1u << 1; /* WF_ENT_HIDDEN */
	}
	if (fd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		it->flags |= 1u << 2; /* WF_ENT_LINK */
	}
}

/* Fills `b` with up to WF_CAND_BATCH matches. Returns false if the search was ended
 * from outside before it finished (nothing is then delivered). */
static bool collect(WfCandWorker *w, Search *s, WfCandBatch *b)
{
	while (b->n < WF_CAND_BATCH) {
		WIN32_FIND_DATAW fd;

		if (s->has_pending) {
			fd = s->pending;
			s->has_pending = false;
		} else if (!FindNextFileW(s->find, &fd)) {
			DWORD e = GetLastError();

			s->ended = true;
			if (ERROR_NO_MORE_FILES != e) {
				b->st = map_error(e);
				b->error = e;
			}
			break;
		}
		if ((LONG)s->gen != w->live_gen || w->stopping) {
			return false;
		}
		if (matches(s, &fd)) {
			fill(&b->items[b->n++], &fd);
		}
	}
	b->more = !s->ended;
	return (LONG)s->gen == w->live_gen && !w->stopping;
}

static void run_start(WfCandWorker *w,
        Search *s,
        uint32_t gen,
        const wchar_t *dir,
        const wchar_t *prefix,
        bool hidden,
        bool dirs_only)
{
	WfCandBatch b;
	wchar_t *pattern = NULL, *x = NULL;
	size_t pn = wcslen(prefix);

	search_close(w, s);
	memset(&b, 0, sizeof b);
	b.gen = gen;
	b.first = true;
	s->gen = gen;
	s->ended = false;
	s->prefix_n = pn < 259 ? pn : 259;
	memcpy(s->prefix, prefix, s->prefix_n * sizeof *prefix);
	s->prefix[s->prefix_n] = L'\0';
	s->include_hidden = hidden;
	s->dirs_only = dirs_only;
	if (WF_OK != wf_path_join(dir, L"*", &pattern) ||
	        WF_OK != wf_path_to_extended(pattern, &x)) {
		b.st = WF_E_NOMEM;
		goto deliver;
	}
	s->find = FindFirstFileExW(x, FindExInfoBasic, &s->pending,
	        FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
	if (INVALID_HANDLE_VALUE == s->find) {
		DWORD e = GetLastError();

		s->ended = true;
		if (ERROR_FILE_NOT_FOUND != e) {
			b.st = map_error(e);
			b.error = e;
		}
		goto deliver;
	}
	InterlockedIncrement(&w->open_handles);
	s->has_pending = true;
	if (!collect(w, s, &b)) {
		search_close(w, s);
		goto out;
	}
deliver:
	if (WF_OK != b.st || s->ended) {
		search_close(w, s);
	}
	AcquireSRWLockExclusive(&w->core.lock);
	push_batch(w, &b);
	ReleaseSRWLockExclusive(&w->core.lock);
out:
	free(pattern);
	free(x);
}

static void run_more(WfCandWorker *w, Search *s, uint32_t gen)
{
	WfCandBatch b;

	if (gen != s->gen || INVALID_HANDLE_VALUE == s->find) {
		return; /* not the search this thread is on */
	}
	memset(&b, 0, sizeof b);
	b.gen = gen;
	if (!collect(w, s, &b)) {
		search_close(w, s);
		return;
	}
	if (WF_OK != b.st || s->ended) {
		search_close(w, s);
	}
	AcquireSRWLockExclusive(&w->core.lock);
	push_batch(w, &b);
	ReleaseSRWLockExclusive(&w->core.lock);
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	WfCandWorker *w = arg;
	Search s;

	memset(&s, 0, sizeof s);
	s.find = INVALID_HANDLE_VALUE;
	s.ended = true;
	for (;;) {
		ReqKind kind;
		uint32_t gen;
		wchar_t *dir, *prefix;
		bool hidden, dirs_only;

		AcquireSRWLockExclusive(&w->core.lock);
		while (!w->core.stop && REQ_NONE == w->req) {
			/* idle: a cancelled search gives its handle back now */
			if (INVALID_HANDLE_VALUE != s.find &&
			        (LONG)s.gen != w->live_gen) {
				ReleaseSRWLockExclusive(&w->core.lock);
				search_close(w, &s);
				AcquireSRWLockExclusive(&w->core.lock);
				continue;
			}
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, INFINITE, 0);
		}
		if (w->core.stop) {
			ReleaseSRWLockExclusive(&w->core.lock);
			break;
		}
		kind = w->req;
		gen = w->req_gen;
		dir = w->req_dir;
		prefix = w->req_prefix;
		hidden = w->req_hidden;
		dirs_only = w->req_dirs_only;
		w->req_dir = w->req_prefix = NULL;
		w->req = REQ_NONE;
		w->core.busy = true;
		ReleaseSRWLockExclusive(&w->core.lock);

		if (REQ_START == kind) {
			run_start(w, &s, gen, dir, prefix, hidden, dirs_only);
		} else {
			run_more(w, &s, gen);
		}
		free(dir);
		free(prefix);

		AcquireSRWLockExclusive(&w->core.lock);
		w->core.busy = false;
		if (UINT64_MAX != w->core.stats.completed_requests) {
			w->core.stats.completed_requests++;
		}
		ReleaseSRWLockExclusive(&w->core.lock);
	}
	search_close(w, &s);
	wf_wc_release(&w->core, worker_free, w);
	return 0;
}

WfStatus wf_candworker_start(WfCandWorker **out)
{
	WfCandWorker *w;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	w = calloc(1, sizeof *w);
	if (!w) {
		return WF_E_NOMEM;
	}
	if (WF_OK != wf_wc_start(&w->core, thread_main, w)) {
		free(w);
		return WF_E_OS;
	}
	*out = w;
	return WF_OK;
}

WfWorkerStopReport wf_candworker_stop(WfCandWorker *w)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	return wf_wc_stop(&w->core, STOP_WAIT_MS, true, worker_free, w);
}

HANDLE wf_candworker_event(const WfCandWorker *w)
{
	return w->core.ready;
}

bool wf_candworker_poll(WfCandWorker *w, WfCandBatch *out)
{
	bool got = false;

	if (!w || !out) {
		return false;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	/* a batch of a search that has since been replaced is not worth showing */
	while (w->qn > 0 && (LONG)w->q[w->qhead].gen != w->live_gen) {
		w->qhead = (w->qhead + 1) % RESULT_CAP;
		w->qn--;
		if (UINT64_MAX != w->core.stats.discarded_results) {
			w->core.stats.discarded_results++;
		}
	}
	if (w->qn > 0) {
		*out = w->q[w->qhead];
		w->qhead = (w->qhead + 1) % RESULT_CAP;
		w->qn--;
		got = true;
	}
	if (0 == w->qn) {
		ResetEvent(w->core.ready);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	return got;
}

static WfStatus port_start(void *ctx,
        uint32_t gen,
        const wchar_t *dir,
        const wchar_t *prefix,
        bool include_hidden,
        bool dirs_only)
{
	WfCandWorker *w = ctx;
	wchar_t *d, *p;

	if (!w || !dir || !prefix || !gen) {
		return WF_E_INVAL;
	}
	d = wf_path_dup(dir);
	p = wf_path_dup(prefix);
	if (!d || !p) {
		free(d);
		free(p);
		return WF_E_NOMEM;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		free(d);
		free(p);
		return WF_E_INVAL;
	}
	if (REQ_NONE != w->req &&
	        UINT64_MAX != w->core.stats.superseded_requests) {
		w->core.stats.superseded_requests++;
	}
	free(w->req_dir);
	free(w->req_prefix);
	w->req = REQ_START;
	w->req_gen = gen;
	w->req_dir = d;
	w->req_prefix = p;
	w->req_hidden = include_hidden;
	w->req_dirs_only = dirs_only;
	InterlockedExchange(&w->live_gen, (LONG)gen);
	if (w->core.busy) {
		CancelSynchronousIo(w->core.thread);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

static WfStatus port_more(void *ctx, uint32_t gen)
{
	WfCandWorker *w = ctx;

	if (!w || !gen) {
		return WF_E_INVAL;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop || (LONG)gen != w->live_gen) {
		ReleaseSRWLockExclusive(&w->core.lock);
		return WF_E_INVAL;
	}
	if (REQ_NONE ==
	        w->req) { /* a pending START is never replaced by MORE */
		w->req = REQ_MORE;
		w->req_gen = gen;
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

static void port_cancel(void *ctx)
{
	WfCandWorker *w = ctx;

	if (!w) {
		return;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	InterlockedExchange(&w->live_gen, 0);
	if (REQ_NONE != w->req) {
		free(w->req_dir);
		free(w->req_prefix);
		w->req_dir = w->req_prefix = NULL;
		w->req = REQ_NONE;
	}
	if (w->core.busy) {
		CancelSynchronousIo(w->core.thread);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
}

WfCandPort wf_candworker_port(WfCandWorker *w)
{
	WfCandPort p = {w, port_start, port_more, port_cancel};

	return p;
}

int wf_candworker_open_handles(WfCandWorker *w)
{
	return (int)w->open_handles;
}

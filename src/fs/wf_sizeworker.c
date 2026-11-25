#include "../async/wf_workcore.h"
#include "wf_sizeworker.h"

#include <stdlib.h>
#include <string.h>

#include "../base/wf_win32status.h"
#include "../path/wf_path.h"

#define STOP_WAIT_MS 1000
#define PROGRESS_MS 250u /* how often running totals are handed over */
#define CHECK_EVERY 64u  /* entries between looks at the clock and the flag */
#define MAX_PENDING 100000u /* folders waiting to be listed, at most */
/* The paths of those folders, in bytes: a count alone does not bound the
 * memory (100000 paths of 32000 units are 6 GB). The same budget as the search
 * walk (wf_treewalk.h). */
#define MAX_PENDING_BYTES (64u << 20)
#define MAX_PATH_UNITS 32000u /* the longest path entered, as in the walk */

struct WfSizeWorker
{
	WfWorkCore core;

	/* the request, until the thread takes it */
	bool has_req;
	uint32_t req_gen;
	wchar_t *req_dir;
	wchar_t **req_names;
	size_t req_n;

	volatile LONG live_gen; /* 0: none; read without the lock by the walk */
	volatile LONG stopping;
	LONG open_handles;

	/* the newest totals of the live request, one slot per folder */
	WfSizeUpdate *slots;
	bool *dirty;
	size_t n, scan;
	size_t pending; /* slots that are dirty: poll has nothing to look for at 0 */
};

static void names_free(wchar_t **names, size_t n)
{
	for (size_t i = 0; names && i < n; i++) {
		free(names[i]);
	}
	free(names);
}

static void worker_free(void *owner)
{
	WfSizeWorker *w = owner;

	free(w->req_dir);
	names_free(w->req_names, w->req_n);
	free(w->slots);
	free(w->dirty);
	free(w);
}

/* True while the walk of `gen` should go on. */
static bool wanted(const WfSizeWorker *w, uint32_t gen)
{
	return (LONG)gen == w->live_gen && !w->stopping;
}

/* Gives the newest totals of folder `index` to the consumer. A request that
 * is no longer the live one delivers nothing. */
static void publish(WfSizeWorker *w, const WfSizeUpdate *u)
{
	AcquireSRWLockExclusive(&w->core.lock);
	if ((LONG)u->gen == w->live_gen && u->index < w->n) {
		w->slots[u->index] = *u;
		if (!w->dirty[u->index]) {
			w->dirty[u->index] = true;
			w->pending++;
		}
		SetEvent(w->core.ready);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
}

/* The shared classes, and a path that is not a folder is "not there" too. */
static WfStatus map_error(DWORD e)
{
	return ERROR_DIRECTORY == e ? WF_E_NOENT : wf_status_from_win32(e);
}

typedef struct
{
	wchar_t **path; /* folders still to list; the last is listed next */
	size_t n, cap;
	size_t bytes; /* of the paths held */
} Pending;

typedef enum
{
	PUSH_OK,
	PUSH_FULL, /* over a bound: the folder is not listed, and the total says so */
	PUSH_NOMEM
} Push;

/* Takes `path` only on PUSH_OK; on any other answer the caller still owns it. */
static Push pending_push(Pending *p, wchar_t *path)
{
	size_t units = wcslen(path), bytes = (units + 1) * sizeof *path;

	if (p->n >= MAX_PENDING || units > MAX_PATH_UNITS ||
	        bytes > MAX_PENDING_BYTES - p->bytes) {
		return PUSH_FULL;
	}
	if (p->n == p->cap) {
		size_t cap = p->cap ? p->cap * 2 : 64;
		wchar_t **grown = realloc(p->path, cap * sizeof *grown);

		if (!grown) {
			return PUSH_NOMEM;
		}
		p->path = grown;
		p->cap = cap;
	}
	p->path[p->n++] = path;
	p->bytes += bytes;
	return PUSH_OK;
}

/* The folder to list next; the caller frees it. */
static wchar_t *pending_pop(Pending *p)
{
	wchar_t *path = p->path[--p->n];

	p->bytes -= (wcslen(path) + 1) * sizeof *path;
	return path;
}

static void pending_free(Pending *p)
{
	names_free(p->path, p->n);
	memset(p, 0, sizeof *p);
}

/* Lists `dir` and adds what is in it to `u`; sub-folders go to `todo`.
 * Returns false when the walk was ended from outside. */
static bool list_one(WfSizeWorker *w,
        const wchar_t *dir,
        WfSizeUpdate *u,
        Pending *todo,
        ULONGLONG started,
        ULONGLONG *last_push,
        size_t *entries)
{
	wchar_t *pattern = NULL, *x = NULL;
	WIN32_FIND_DATAW fd;
	HANDLE find;
	bool go = true;

	if (WF_OK != wf_path_join(dir, L"*", &pattern) ||
	        WF_OK != wf_path_to_extended(pattern, &x)) {
		free(pattern);
		free(x);
		u->partial = true;
		if (WF_OK == u->st) {
			u->st = WF_E_NOMEM;
		}
		return true;
	}
	find = FindFirstFileExW(x, FindExInfoBasic, &fd, FindExSearchNameMatch,
	        NULL, FIND_FIRST_EX_LARGE_FETCH);
	free(pattern);
	free(x);
	if (INVALID_HANDLE_VALUE == find) {
		DWORD e = GetLastError();

		if (ERROR_FILE_NOT_FOUND !=
		        e) { /* an empty folder is not one */
			u->partial = true;
			if (WF_OK == u->st) {
				u->st = map_error(e);
			}
		}
		return wanted(w, u->gen);
	}
	InterlockedIncrement(&w->open_handles);
	do {
		if (!wcscmp(fd.cFileName, L".") ||
		        !wcscmp(fd.cFileName, L"..")) {
			continue;
		}
		if (0 == (++*entries % CHECK_EVERY)) {
			ULONGLONG now = GetTickCount64();

			if (!wanted(w, u->gen)) {
				go = false;
				break;
			}
			if (*entries >= WF_SIZE_MAX_ENTRIES ||
			        now - started >= WF_SIZE_MAX_MS) {
				u->partial = true;
				break;
			}
			if (now - *last_push >= PROGRESS_MS) {
				*last_push = now;
				publish(w, u);
			}
		}
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			u->folders++;
			if (fd.dwFileAttributes &
			        FILE_ATTRIBUTE_REPARSE_POINT) {
				continue; /* a link is counted, never followed */
			}
			wchar_t *sub = NULL;
			Push pushed = PUSH_NOMEM;

			if (WF_OK == wf_path_join(dir, fd.cFileName, &sub)) {
				pushed = pending_push(todo, sub);
			}
			if (PUSH_OK != pushed) {
				free(sub);
				u->partial = true;
				if (PUSH_NOMEM == pushed && WF_OK == u->st) {
					u->st = WF_E_NOMEM;
				}
			}
		} else {
			u->files++;
			u->bytes += ((uint64_t)fd.nFileSizeHigh << 32) |
			        fd.nFileSizeLow;
		}
	} while (FindNextFileW(find, &fd));
	if (go && GetLastError() != ERROR_NO_MORE_FILES && !u->partial) {
		DWORD e = GetLastError();

		if (ERROR_SUCCESS != e) {
			u->partial = true;
			if (WF_OK == u->st) {
				u->st = map_error(e);
			}
		}
	}
	FindClose(find);
	InterlockedDecrement(&w->open_handles);
	return go;
}

/* The totals of one folder of the request; the last word is published. */
static void count_folder(
        WfSizeWorker *w, uint32_t gen, size_t index, const wchar_t *root)
{
	WfSizeUpdate u;
	Pending todo;
	ULONGLONG started = GetTickCount64(), last_push = started;
	size_t entries = 0;
	DWORD attr = GetFileAttributesW(root);
	bool go = true;
	wchar_t *first;

	memset(&u, 0, sizeof u);
	memset(&todo, 0, sizeof todo);
	u.gen = gen;
	u.index = index;
	if (INVALID_FILE_ATTRIBUTES == attr ||
	        0 == (attr & FILE_ATTRIBUTE_DIRECTORY) ||
	        (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
		/* gone, not a folder, or a link: there is nothing to count */
		u.finished = u.partial = true;
		u.st = INVALID_FILE_ATTRIBUTES == attr ? WF_E_NOENT
		                                       : WF_E_INVAL;
		publish(w, &u);
		return;
	}
	/* the copy and the queueing are two steps: a copy that could not be queued
	 * is still ours to free */
	first = wf_path_dup(root);
	if (first && PUSH_OK == pending_push(&todo, first)) {
		while (go && todo.n) {
			wchar_t *dir = pending_pop(&todo);

			go = list_one(w, dir, &u, &todo, started, &last_push,
			        &entries);
			free(dir);
			if (u.partial &&
			        (entries >= WF_SIZE_MAX_ENTRIES ||
			                GetTickCount64() - started >=
			                        WF_SIZE_MAX_MS)) {
				break;
			}
		}
	} else {
		free(first);
		u.partial = true;
		u.st = WF_E_NOMEM;
	}
	pending_free(&todo);
	if (!go) {
		return; /* ended from outside: nothing is delivered */
	}
	u.finished = true;
	publish(w, &u);
}

static void run_request(WfSizeWorker *w,
        uint32_t gen,
        const wchar_t *dir,
        wchar_t **names,
        size_t n)
{
	for (size_t i = 0; i < n && wanted(w, gen); i++) {
		wchar_t *root = NULL;
		WfStatus joined = wf_path_join(dir, names[i], &root);

		if (WF_OK != joined) {
			/* not a name inside `dir` (a separator in it, say) */
			WfSizeUpdate u = {gen, i, 0, 0, 0, true, true, joined};

			publish(w, &u);
			continue;
		}
		count_folder(w, gen, i, root);
		free(root);
	}
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	WfSizeWorker *w = arg;

	for (;;) {
		uint32_t gen;
		wchar_t *dir, **names;
		size_t n;

		AcquireSRWLockExclusive(&w->core.lock);
		while (!w->core.stop && !w->has_req) {
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, INFINITE, 0);
		}
		if (w->core.stop) {
			ReleaseSRWLockExclusive(&w->core.lock);
			break;
		}
		gen = w->req_gen;
		dir = w->req_dir;
		names = w->req_names;
		n = w->req_n;
		w->req_dir = NULL;
		w->req_names = NULL;
		w->req_n = 0;
		w->has_req = false;
		w->core.busy = true;
		ReleaseSRWLockExclusive(&w->core.lock);

		run_request(w, gen, dir, names, n);
		free(dir);
		names_free(names, n);

		AcquireSRWLockExclusive(&w->core.lock);
		w->core.busy = false;
		if (UINT64_MAX != w->core.stats.completed_requests) {
			w->core.stats.completed_requests++;
		}
		ReleaseSRWLockExclusive(&w->core.lock);
	}
	wf_wc_release(&w->core, worker_free, w);
	return 0;
}

WfStatus wf_sizeworker_start(WfSizeWorker **out)
{
	WfSizeWorker *w;

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

WfWorkerStopReport wf_sizeworker_stop(WfSizeWorker *w)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	InterlockedExchange(&w->stopping, 1);
	return wf_wc_stop(&w->core, STOP_WAIT_MS, true, worker_free, w);
}

HANDLE wf_sizeworker_event(const WfSizeWorker *w)
{
	return w->core.ready;
}

bool wf_sizeworker_poll(WfSizeWorker *w, WfSizeUpdate *out)
{
	bool got = false;

	if (!w || !out) {
		return false;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	for (size_t k = 0; w->pending && k < w->n && !got; k++) {
		size_t i = (w->scan + k) % w->n;

		if (w->dirty[i]) {
			*out = w->slots[i];
			w->dirty[i] = false;
			w->pending--;
			w->scan = i + 1;
			got = true;
		}
	}
	if (!got) {
		ResetEvent(w->core.ready);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	return got;
}

bool wf_sizeworker_idle(WfSizeWorker *w)
{
	bool idle;

	if (!w) {
		return true;
	}
	AcquireSRWLockShared(&w->core.lock);
	idle = !w->core.busy && !w->has_req && !w->pending;
	ReleaseSRWLockShared(&w->core.lock);
	return idle;
}

static WfStatus port_start(void *ctx,
        uint32_t gen,
        const wchar_t *dir,
        const wchar_t *const *names,
        size_t n)
{
	WfSizeWorker *w = ctx;
	wchar_t *d, **copy;
	WfSizeUpdate *slots;
	bool *dirty;

	if (!w || !dir || !names || !gen || !n || n > WF_SIZE_MAX_FOLDERS) {
		return WF_E_INVAL;
	}
	d = wf_path_dup(dir);
	copy = calloc(n, sizeof *copy);
	slots = calloc(n, sizeof *slots);
	dirty = calloc(n, sizeof *dirty);
	for (size_t i = 0; copy && i < n; i++) {
		copy[i] = wf_path_dup(names[i]);
		if (!copy[i]) {
			names_free(copy, n);
			copy = NULL;
		}
	}
	if (!d || !copy || !slots || !dirty) {
		free(d);
		names_free(copy, n);
		free(slots);
		free(dirty);
		return WF_E_NOMEM;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		free(d);
		names_free(copy, n);
		free(slots);
		free(dirty);
		return WF_E_INVAL;
	}
	if (w->has_req && UINT64_MAX != w->core.stats.superseded_requests) {
		w->core.stats.superseded_requests++;
	}
	free(w->req_dir);
	names_free(w->req_names, w->req_n);
	free(w->slots);
	free(w->dirty);
	w->has_req = true;
	w->req_gen = gen;
	w->req_dir = d;
	w->req_names = copy;
	w->req_n = n;
	w->slots = slots;
	w->dirty = dirty;
	w->n = n;
	w->scan = 0;
	w->pending = 0; /* the new slots are all clean */
	InterlockedExchange(&w->live_gen, (LONG)gen);
	ResetEvent(w->core.ready);
	if (w->core.busy) {
		CancelSynchronousIo(w->core.thread);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

static void port_cancel(void *ctx)
{
	WfSizeWorker *w = ctx;

	if (!w) {
		return;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	InterlockedExchange(&w->live_gen, 0);
	if (w->has_req) {
		free(w->req_dir);
		names_free(w->req_names, w->req_n);
		w->req_dir = NULL;
		w->req_names = NULL;
		w->req_n = 0;
		w->has_req = false;
	}
	w->n = 0;
	ResetEvent(w->core.ready);
	if (w->core.busy) {
		CancelSynchronousIo(w->core.thread);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
}

WfSizePort wf_sizeworker_port(WfSizeWorker *w)
{
	WfSizePort p = {w, port_start, port_cancel};

	return p;
}

int wf_sizeworker_open_handles(WfSizeWorker *w)
{
	return (int)w->open_handles;
}

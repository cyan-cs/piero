#include "../async/wf_workcore.h"
#include "wf_searchworker.h"

#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"
#include "wf_dupes.h"

#define STOP_WAIT_MS 1000
#define FLUSH_MS 100u
#define MAX_WAITING 64u /* batches the screen has not taken */

struct WfSearchWorker
{
	WfWorkCore core;
	CONDITION_VARIABLE space; /* signalled when the screen took a batch */
	WfSearchListFn list;
	WfSearchHashFn hash; /* NULL: a duplicate search is refused */

	bool has_req;
	bool req_dupes;
	uint32_t req_gen;
	wchar_t *req_root;
	WfFileId req_root_id;
	WfQuery req_query;
	bool req_hidden;

	volatile LONG live_gen; /* 0: none; read without the lock by the walk */
	volatile LONG stopping;

	WfSearchBatch q[MAX_WAITING]; /* oldest first */
	size_t qn;
};

static void queue_clear(WfSearchWorker *w)
{
	for (size_t i = 0; i < w->qn; i++) {
		wf_search_batch_free(&w->q[i]);
	}
	w->qn = 0;
}

static void worker_free(void *owner)
{
	WfSearchWorker *w = owner;

	queue_clear(w);
	free(w->req_root);
	free(w);
}

typedef struct
{
	WfSearchWorker *w;
	uint32_t gen;
	const WfQuery *query;
	WfHit *hit; /* the batch being filled */
	size_t n, cap;
	WfHitDir *dir; /* the folder of the last hit: its record is shared */
	size_t total;  /* hits made so far */
	ULONGLONG last_flush;
	size_t visits;
	bool limit; /* stopped at WF_SEARCH_MAX_HITS */
	bool nomem;
	/* duplicates */
	bool dupes;
	WfDupFile *file; /* the files collected for comparing */
	wchar_t **fname; /* owned: their names */
	size_t nfile, capfile;
	uint32_t group;
	uint64_t pdone, ptotal;
	bool progress_due;
	size_t groups, files_in_groups, skipped;
} Run;

static bool wanted(const Run *r)
{
	return (LONG)r->gen == r->w->live_gen && !r->w->stopping;
}

static bool run_cancelled(void *ctx)
{
	return !wanted(ctx);
}

static bool cancel_cb(void *ctx)
{
	return !wanted(ctx);
}

static WfStatus list_cb(void *ctx, const wchar_t *dir, WfDir **out)
{
	Run *r = ctx;

	return r->w->list(dir, cancel_cb, r, out);
}

/* Puts the batch on the queue (waiting for room). false: the request is over. */
static bool publish(
        Run *r, bool done, bool at_least, const WfWalkStats *walk, WfStatus st)
{
	WfSearchWorker *w = r->w;
	WfSearchBatch b;

	if (!done && 0 == r->n && !r->progress_due) {
		return true;
	}
	memset(&b, 0, sizeof b);
	b.gen = r->gen;
	b.dupes = r->dupes;
	b.read_done = r->pdone;
	b.read_total = r->ptotal;
	r->progress_due = false;
	if (done) {
		b.groups = r->groups;
		b.files_in_groups = r->files_in_groups;
		b.skipped = r->skipped;
	}
	b.hit = r->hit;
	b.n = r->n;
	b.done = done;
	b.at_least = at_least;
	b.st = st;
	if (walk) {
		b.walk = *walk;
	}
	r->hit = NULL;
	r->n = r->cap = 0;
	AcquireSRWLockExclusive(&w->core.lock);
	while (w->qn >= MAX_WAITING && wanted(r)) {
		SleepConditionVariableSRW(
		        &w->space, &w->core.lock, INFINITE, 0);
	}
	if (!wanted(r)) {
		ReleaseSRWLockExclusive(&w->core.lock);
		wf_search_batch_free(&b);
		return false;
	}
	w->q[w->qn++] = b;
	SetEvent(w->core.ready);
	ReleaseSRWLockExclusive(&w->core.lock);
	r->last_flush = GetTickCount64();
	return true;
}

static bool visit_cb(void *ctx, const WfWalkItem *item)
{
	Run *r = ctx;
	const WfEntry *e = item->e;
	WfHit *h;

	if (!wanted(r)) {
		return false;
	}
	if (!wf_query_match(r->query, e->name, 0 != (e->flags & WF_ENT_DIR),
	            e->size, e->mtime)) {
		if (0 == (++r->visits & 63u) && r->n &&
		        GetTickCount64() - r->last_flush >= FLUSH_MS) {
			return publish(r, false, false, NULL, WF_OK);
		}
		return true;
	}
	if (r->n == r->cap) {
		size_t cap = r->cap ? r->cap * 2 : 16;
		WfHit *grown = realloc(r->hit, cap * sizeof *grown);

		if (!grown) {
			r->nomem = true;
			return false;
		}
		r->hit = grown;
		r->cap = cap;
	}
	if (!r->dir || wcscmp(r->dir->path, item->dir)) {
		WfHitDir *d = wf_hitdir_new(item->dir, item->dir_id);

		if (!d) {
			r->nomem = true;
			return false;
		}
		wf_hitdir_unref(
		        r->dir); /* the worker's own reference to the last */
		r->dir = d;
	}
	h = &r->hit[r->n];
	memset(h, 0, sizeof *h);
	h->dir = wf_hitdir_ref(r->dir);
	h->name = wf_path_dup(e->name);
	if (!h->name) {
		wf_hitdir_unref(h->dir);
		r->nomem = true;
		return false;
	}
	h->id = e->identity;
	h->flags = e->flags;
	h->size = e->size;
	h->mtime = e->mtime;
	h->depth = (uint32_t)item->depth;
	r->n++;
	r->total++;
	if (r->total >= WF_SEARCH_MAX_HITS) {
		r->limit = true;
		return false;
	}
	if (r->n >= WF_SEARCH_BATCH ||
	        (0 == (++r->visits & 63u) &&
	                GetTickCount64() - r->last_flush >= FLUSH_MS)) {
		return publish(r, false, false, NULL, WF_OK);
	}
	return true;
}

static void run_request(WfSearchWorker *w,
        uint32_t gen,
        const wchar_t *root,
        WfFileId root_id,
        const WfQuery *query,
        bool hidden)
{
	Run r;
	WfWalkOps ops = {list_cb, visit_cb, run_cancelled, hidden, 0};
	WfWalkStats stats;
	WfStatus st;
	bool at_least;

	memset(&r, 0, sizeof r);
	r.w = w;
	r.gen = gen;
	r.query = query;
	r.last_flush = GetTickCount64();
	st = wf_walk(&ops, &r, root, root_id, &stats);
	if (r.nomem && WF_OK == st) {
		st = WF_E_NOMEM;
	}
	at_least = r.limit || stats.pending_full || WF_OK != st;
	if (wanted(&r)) {
		(void)publish(&r, true, at_least, &stats, st);
	}
	for (size_t i = 0; i < r.n; i++) {
		wf_hit_clear(&r.hit[i]);
	}
	free(r.hit);
	wf_hitdir_unref(r.dir);
}

/* Collects the files worth comparing: not a folder, not a link, not empty. */
static bool collect_cb(void *ctx, const WfWalkItem *item)
{
	Run *r = ctx;
	const WfEntry *e = item->e;
	wchar_t *name;

	if (!wanted(r)) {
		return false;
	}
	if ((e->flags & (WF_ENT_DIR | WF_ENT_LINK | WF_ENT_CLOUD)) ||
	        0 == e->size) {
		return true;
	}
	if (r->nfile >= WF_DUPE_MAX_FILES) {
		r->limit = true;
		return false;
	}
	if (r->nfile == r->capfile) {
		size_t cap = r->capfile ? r->capfile * 2 : 256;
		WfDupFile *f = realloc(r->file, cap * sizeof *f);
		wchar_t **n;

		if (f) {
			r->file = f;
		}
		n = realloc(r->fname, cap * sizeof *n);
		if (n) {
			r->fname = n;
		}
		if (!f || !n) {
			r->nomem = true;
			return false;
		}
		r->capfile = cap;
	}
	if (!r->dir || wcscmp(r->dir->path, item->dir)) {
		WfHitDir *d = wf_hitdir_new(item->dir, item->dir_id);

		if (!d) {
			r->nomem = true;
			return false;
		}
		wf_hitdir_unref(r->dir);
		r->dir = d;
	}
	name = wf_path_dup(e->name);
	if (!name) {
		r->nomem = true;
		return false;
	}
	r->fname[r->nfile] = name;
	r->file[r->nfile].dir = wf_hitdir_ref(r->dir);
	r->file[r->nfile].name = name;
	r->file[r->nfile].id = e->identity;
	r->file[r->nfile].size = e->size;
	r->file[r->nfile].mtime = e->mtime;
	r->nfile++;
	return true;
}

static WfStatus dupes_hash(
        void *ctx, const WfDupFile *file, uint64_t limit, uint8_t digest[32])
{
	Run *r = ctx;

	if (!r->w->hash) {
		return WF_E_INVAL;
	}
	{
		WfSearchSeen seen;

		seen.size = file->size;
		seen.mtime = file->mtime;
		seen.id = file->id;
		return r->w->hash(file->dir->path, file->name, &seen, limit,
		        cancel_cb, r, digest);
	}
}

static bool dupes_group(void *ctx, const WfDupFile *const *file, size_t n)
{
	Run *r = ctx;
	size_t room = WF_SEARCH_MAX_HITS -
	        r->total; /* total stays below the limit here */

	if (!wanted(r)) {
		return false;
	}
	if (n > room) {
		/* A group that does not fit is cut to what does, before anything is
		 * made for the rest; "at least" says so. Less than two files are not
		 * a group of duplicates, so they are not shown at all. */
		r->limit = true;
		if (room < 2) {
			return false;
		}
		n = room;
	}
	if (r->n + n > r->cap) {
		size_t cap = r->cap ? r->cap : 16;
		WfHit *grown;

		while (cap < r->n + n) {
			cap *= 2;
		}
		grown = realloc(r->hit, cap * sizeof *grown);
		if (!grown) {
			r->nomem = true;
			return false;
		}
		r->hit = grown;
		r->cap = cap;
	}
	r->group++;
	for (size_t i = 0; i < n; i++) {
		WfHit *h = &r->hit[r->n];

		memset(h, 0, sizeof *h);
		h->name = wf_path_dup(file[i]->name);
		if (!h->name) {
			r->nomem = true;
			return false;
		}
		h->dir = wf_hitdir_ref(file[i]->dir);
		h->id = file[i]->id;
		h->size = file[i]->size;
		h->mtime = file[i]->mtime;
		h->group = r->group;
		r->n++;
	}
	r->total += n;
	r->limit = r->limit || r->total >= WF_SEARCH_MAX_HITS;
	if (r->limit) {
		return false;
	}
	if (r->n >= WF_SEARCH_BATCH) {
		return publish(r, false, false, NULL, WF_OK);
	}
	return true;
}

static void dupes_progress(
        void *ctx, WfDupePhase phase, uint64_t done, uint64_t total)
{
	Run *r = ctx;

	(void)phase;
	r->pdone = done;
	r->ptotal = total;
	if (GetTickCount64() - r->last_flush >= FLUSH_MS) {
		r->progress_due = true;
		(void)publish(r, false, false, NULL, WF_OK);
	}
}

static bool dupes_cancelled(void *ctx)
{
	return !wanted(ctx);
}

static void run_dupes(WfSearchWorker *w,
        uint32_t gen,
        const wchar_t *root,
        WfFileId root_id,
        bool hidden)
{
	Run r;
	WfWalkOps ops = {list_cb, collect_cb, run_cancelled, hidden, 0};
	WfWalkStats stats;
	WfDupeOps dops = {
	        dupes_hash, dupes_group, dupes_progress, dupes_cancelled};
	WfDupeStats dst;
	WfStatus st;
	bool at_least;

	memset(&r, 0, sizeof r);
	r.w = w;
	r.gen = gen;
	r.dupes = true;
	r.last_flush = GetTickCount64();
	st = wf_walk(&ops, &r, root, root_id, &stats);
	if (r.nomem && WF_OK == st) {
		st = WF_E_NOMEM;
	}
	memset(&dst, 0, sizeof dst);
	/* at the file limit the files kept are still compared, and "at least" says so */
	if (WF_OK == st && wanted(&r)) {
		st = wf_dupes_run(r.file, r.nfile, &dops, &r, &dst);
	}
	if (r.nomem && WF_OK == st) {
		st = WF_E_NOMEM;
	}
	r.groups = dst.groups;
	r.files_in_groups = dst.files_in_groups;
	r.skipped = dst.skipped;
	at_least = r.limit || stats.pending_full || WF_OK != st;
	if (wanted(&r)) {
		(void)publish(&r, true, at_least, &stats, st);
	}
	for (size_t i = 0; i < r.n; i++) {
		wf_hit_clear(&r.hit[i]);
	}
	free(r.hit);
	for (size_t i = 0; i < r.nfile; i++) {
		wf_hitdir_unref(r.file[i].dir);
		free(r.fname[i]);
	}
	free(r.file);
	free(r.fname);
	wf_hitdir_unref(r.dir);
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	WfSearchWorker *w = arg;

	for (;;) {
		uint32_t gen;
		wchar_t *root;
		WfFileId root_id;
		WfQuery query;
		bool hidden, dupes;

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
		root = w->req_root;
		w->req_root = NULL;
		root_id = w->req_root_id;
		query = w->req_query;
		hidden = w->req_hidden;
		dupes = w->req_dupes;
		w->has_req = false;
		w->core.busy = true;
		ReleaseSRWLockExclusive(&w->core.lock);

		if (dupes) {
			run_dupes(w, gen, root, root_id, hidden);
		} else {
			run_request(w, gen, root, root_id, &query, hidden);
		}
		free(root);

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

WfStatus wf_searchworker_start(WfSearchWorker **out, WfSearchListFn list)
{
	WfSearchWorker *w;

	if (!out || !list) {
		return WF_E_INVAL;
	}
	*out = NULL;
	w = calloc(1, sizeof *w);
	if (!w) {
		return WF_E_NOMEM;
	}
	w->list = list;
	InitializeConditionVariable(&w->space);
	if (WF_OK != wf_wc_start(&w->core, thread_main, w)) {
		free(w);
		return WF_E_OS;
	}
	*out = w;
	return WF_OK;
}

WfWorkerStopReport wf_searchworker_stop(WfSearchWorker *w)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	InterlockedExchange(&w->stopping, 1);
	WakeAllConditionVariable(&w->space);
	return wf_wc_stop(&w->core, STOP_WAIT_MS, true, worker_free, w);
}

void wf_searchworker_set_hash(WfSearchWorker *w, WfSearchHashFn hash)
{
	if (w) {
		AcquireSRWLockExclusive(&w->core.lock);
		w->hash = hash;
		ReleaseSRWLockExclusive(&w->core.lock);
	}
}

HANDLE wf_searchworker_event(const WfSearchWorker *w)
{
	return w->core.ready;
}

bool wf_searchworker_idle(WfSearchWorker *w)
{
	bool idle;

	if (!w) {
		return true;
	}
	AcquireSRWLockShared(&w->core.lock);
	idle = !w->core.busy && !w->has_req && !w->qn;
	ReleaseSRWLockShared(&w->core.lock);
	return idle;
}

bool wf_searchworker_poll(WfSearchWorker *w, WfSearchBatch *out)
{
	bool got = false;

	if (!w || !out) {
		return false;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->qn) {
		*out = w->q[0];
		memmove(&w->q[0], &w->q[1], (w->qn - 1) * sizeof w->q[0]);
		w->qn--;
		got = true;
	}
	if (!w->qn) {
		ResetEvent(w->core.ready);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	if (got) {
		WakeAllConditionVariable(&w->space);
	}
	return got;
}

static WfStatus submit(WfSearchWorker *w,
        uint32_t gen,
        const wchar_t *root,
        WfFileId root_id,
        const WfQuery *query,
        bool hidden,
        bool dupes)
{
	wchar_t *copy;

	if (!w || !root || !*root || (!dupes && !query) || !gen) {
		return WF_E_INVAL;
	}
	copy = wf_path_dup(root);
	if (!copy) {
		return WF_E_NOMEM;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop || (dupes && !w->hash)) {
		ReleaseSRWLockExclusive(&w->core.lock);
		free(copy);
		return WF_E_INVAL;
	}
	if (w->has_req && UINT64_MAX != w->core.stats.superseded_requests) {
		w->core.stats.superseded_requests++;
	}
	free(w->req_root);
	w->req_root = copy;
	w->req_root_id = root_id;
	memset(&w->req_query, 0, sizeof w->req_query);
	if (query) {
		w->req_query = *query;
	}
	w->req_hidden = hidden;
	w->req_dupes = dupes;
	w->req_gen = gen;
	w->has_req = true;
	InterlockedExchange(&w->live_gen, (LONG)gen);
	queue_clear(w);
	ResetEvent(w->core.ready);
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeAllConditionVariable(
	        &w->space); /* a walk waiting for room is over */
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

static WfStatus port_start(void *ctx,
        uint32_t gen,
        const wchar_t *root,
        WfFileId root_id,
        const WfQuery *query,
        bool hidden)
{
	return submit(ctx, gen, root, root_id, query, hidden, false);
}

static WfStatus port_start_dupes(void *ctx,
        uint32_t gen,
        const wchar_t *root,
        WfFileId root_id,
        bool hidden)
{
	return submit(ctx, gen, root, root_id, NULL, hidden, true);
}

static void port_cancel(void *ctx)
{
	WfSearchWorker *w = ctx;

	if (!w) {
		return;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	InterlockedExchange(&w->live_gen, 0);
	free(w->req_root);
	w->req_root = NULL;
	w->has_req = false;
	queue_clear(w);
	ResetEvent(w->core.ready);
	if (w->core.busy) {
		CancelSynchronousIo(w->core.thread);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeAllConditionVariable(&w->space);
	WakeConditionVariable(&w->core.wake);
}

WfSearchPort wf_searchworker_port(WfSearchWorker *w)
{
	WfSearchPort p = {w, port_start, port_cancel, port_start_dupes};

	return p;
}

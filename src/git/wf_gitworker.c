#include "../async/wf_workcore.h"
#include "wf_gitworker.h"

#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"

#define STOP_WAIT_MS 3000
#define DETECT_MS 15000u
#define STATUS_MS 60000u
#define BASE_MS 15000u
#define STATUS_MAX ((size_t)64 << 20)
#define BASE_MAX ((size_t)4 << 20)

enum
{
	K_REPO,
	K_BASE,
	K_DIFF,
	K_N
};

typedef struct
{
	bool has;
	uint32_t gen;
	WfGitEnv env;
	wchar_t *root;
	int mode; /* WF_GIT_REPO_*: with RESOLVE or LOCATE `root` is any folder */
} ReqRepo;

typedef struct
{
	bool has;
	uint32_t gen;
	uint64_t id;
	WfGitEnv env;
	wchar_t *root;
	char *rel, *orig;
	wchar_t *
	        path; /* instead of root and rel: the file, and the worker finds both */
	bool expect_new;
} ReqBase;

typedef struct
{
	bool has;
	uint32_t gen;
	uint64_t id;
	WfGitBlob *base;
	char *text;
	size_t n;
} ReqDiff;

struct WfGitWorker
{
	WfWorkCore core;
	ReqRepo repo;
	ReqBase base;
	ReqDiff diff;
	volatile LONG newest[K_N]; /* the generation that is wanted; 0: none */
	WfGitAnswer ans[K_N];
	bool has_ans[K_N];
};

static void repo_clear(ReqRepo *r)
{
	free(r->root);
	memset(r, 0, sizeof *r);
}

static void base_clear(ReqBase *r)
{
	free(r->path);
	free(r->root);
	free(r->rel);
	free(r->orig);
	memset(r, 0, sizeof *r);
}

static void diff_clear(ReqDiff *r)
{
	wf_gitblob_release(r->base);
	free(r->text);
	memset(r, 0, sizeof *r);
}

static void worker_free(void *owner)
{
	WfGitWorker *w = owner;

	repo_clear(&w->repo);
	base_clear(&w->base);
	diff_clear(&w->diff);
	for (int k = 0; k < K_N; k++) {
		wf_gitanswer_free(&w->ans[k]);
	}
	free(w);
}

typedef struct
{
	WfGitWorker *w;
	int kind;
	uint32_t gen;
} Job;

/* The process of a job is ended when the worker stops or a newer request of its
 * kind (or a cancel) has made its generation not the wanted one. */
static bool job_cancelled(void *ctx)
{
	const Job *j = ctx;

	return j->w->core.stop || (LONG)j->gen != j->w->newest[j->kind];
}

/* Hands an answer over if it is still the wanted one; frees it if not. */
static void publish(Job *j, WfGitAnswer *a)
{
	WfGitWorker *w = j->w;

	AcquireSRWLockExclusive(&w->core.lock);
	if (!w->core.stop && (LONG)a->gen == w->newest[j->kind]) {
		wf_gitanswer_free(&w->ans[j->kind]);
		w->ans[j->kind] = *a;
		w->has_ans[j->kind] = true;
		SetEvent(w->core.ready);
		memset(a, 0, sizeof *a);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	wf_gitanswer_free(a);
}

static void answer_init(
        WfGitAnswer *a, WfGitAnswerKind kind, uint32_t gen, uint64_t id)
{
	memset(a, 0, sizeof *a);
	a->kind = kind;
	a->gen = gen;
	a->id = id;
	a->state = WF_GITR_FAILED;
}

static void repo_job(WfGitWorker *w, ReqRepo *rq)
{
	Job job = {w, K_REPO, rq->gen};
	WfGitAnswer a;
	WfGitRunResult r;
	WfStatus st;
	wchar_t found[MAX_PATH * 4];
	const wchar_t *dir = rq->root;
	static const wchar_t *const inside[] = {
	        L"rev-parse", L"--is-inside-work-tree", NULL};
	static const wchar_t *const status[] = {L"status", L"--porcelain=v2",
	        L"-z", L"--untracked-files=normal", NULL};

	answer_init(&a, WF_GIT_A_REPO, rq->gen, 0);
	memset(&r, 0, sizeof r);
	/* every answer says what it was asked about and, when one was found, which
	 * repository it is for: a generation alone cannot tell two repositories apart
	 * once the screen has gone from one to another */
	a.asked = wf_path_dup(rq->root);
	if (!a.asked) {
		goto done;
	}
	if (rq->mode !=
	        WF_GIT_REPO_PLAIN) { /* the walk the screen did not do: it may wait, here it may */
		if (!wf_git_nearest_dotgit(rq->root, found, ARRAYSIZE(found))) {
			a.state =
			        WF_GITR_NOT_REPO; /* no .git above: no process, no wsl.exe probe */
			goto done;
		}
		dir = found;
	}
	a.root = wf_path_dup(dir);
	if (!a.root) {
		goto done;
	}
	if (WF_GIT_REPO_LOCATE ==
	        rq->mode) { /* only where the .git is: no process */
		a.located = true;
		a.state = WF_GITR_OK;
		goto done;
	}
	if (WF_GIT_UNAVAILABLE ==
	        wf_git_capability(&rq->env, job_cancelled, &job)) {
		a.state = job_cancelled(&job) ? WF_GITR_FAILED
		                              : WF_GITR_UNAVAILABLE;
		goto done;
	}
	st = wf_git_exec(
	        &rq->env, dir, inside, DETECT_MS, 256, job_cancelled, &job, &r);
	if (WF_OK != st) {
		a.state =
		        WF_E_NOENT == st ? WF_GITR_UNAVAILABLE : WF_GITR_FAILED;
		wf_gitproc_result_free(&r);
		goto done;
	}
	if (0 != r.exit_code || !r.out ||
	        0 != strncmp((const char *)r.out, "true", 4)) {
		a.state = WF_GITR_NOT_REPO;
		wf_gitproc_result_free(&r);
		goto done;
	}
	wf_gitproc_result_free(&r);
	st = wf_git_exec(&rq->env, dir, status, STATUS_MS, STATUS_MAX,
	        job_cancelled, &job, &r);
	if (WF_OK == st && 0 == r.exit_code &&
	        WF_OK == wf_gitstatus_parse(r.out, r.out_n, &a.table)) {
		a.state = WF_GITR_OK;
	}
	wf_gitproc_result_free(&r);
done:
	publish(&job, &a);
}

/* After a failed `cat-file`: whether the path is simply not in HEAD. `rev-parse
 * --verify --quiet` exits 1 and says nothing for a name that is not there (and
 * for a repository with no commit yet); any other failure is an error of Git
 * and is not taken for a new file. */
static bool is_new_at_head(
        const WfGitEnv *env, const wchar_t *root, const wchar_t *spec, Job *job)
{
	const wchar_t *args[] = {
	        L"rev-parse", L"--verify", L"--quiet", spec, NULL};
	WfGitRunResult r;
	bool is_new;

	memset(&r, 0, sizeof r);
	is_new = WF_OK ==
	                wf_git_exec(env, root, args, BASE_MS, 256,
	                        job_cancelled, job, &r) &&
	        1 == r.exit_code && '\0' == r.err[0] && 0 == r.out_n;
	wf_gitproc_result_free(&r);
	return is_new;
}

/* Where a file is: its repository's `.git` at or above its folder, and its path
 * from there. False: no `.git` above it (or a path that does not convert). */
static bool locate(const WfGitEnv *env,
        const wchar_t *path,
        wchar_t *root,
        size_t root_cap,
        char *rel,
        size_t rel_cap)
{
	wchar_t dir[MAX_PATH * 4];
	const wchar_t *slash = NULL;

	for (const wchar_t *c = path; *c; c++) {
		slash = L'\\' == *c || L'/' == *c ? c : slash;
	}
	if (!slash || (size_t)(slash - path) >= ARRAYSIZE(dir)) {
		return false;
	}
	memcpy(dir, path, (size_t)(slash - path) * sizeof *dir);
	dir[slash - path] = L'\0';
	return wf_git_nearest_dotgit(dir, root, root_cap) &&
	        wf_gitenv_rel_path(env, root, path, rel, rel_cap);
}

static void base_job(WfGitWorker *w, ReqBase *rq)
{
	Job job = {w, K_BASE, rq->gen};
	WfGitAnswer a;
	WfGitRunResult r;
	wchar_t spec[4200], root_buf[MAX_PATH * 4];
	char rel_buf[4400];
	const wchar_t *args[] = {L"cat-file", L"blob", spec, NULL};
	const wchar_t *root = rq->root;
	const char *rel = rq->rel;
	const char *path;
	WfStatus st;
	int n;

	answer_init(&a, WF_GIT_A_BASE, rq->gen, rq->id);
	memset(&r, 0, sizeof r);
	if (rq->path) { /* the caller does not know where the repository is: find out here */
		if (!locate(&rq->env, rq->path, root_buf, ARRAYSIZE(root_buf),
		            rel_buf, sizeof rel_buf)) {
			a.state = WF_GITR_NOT_REPO;
			goto done;
		}
		root = root_buf;
		rel = rel_buf;
	}
	path = rq->orig ? rq->orig : rel;
	if (rq->expect_new &&
	        !rq->orig) { /* Git said it is new: nothing to fetch */
		a.base = wf_gitblob_new("", 0);
		a.state = a.base ? WF_GITR_OK : WF_GITR_FAILED;
		goto done;
	}
	if (WF_GIT_UNAVAILABLE ==
	        wf_git_capability(&rq->env, job_cancelled, &job)) {
		a.state = job_cancelled(&job) ? WF_GITR_FAILED
		                              : WF_GITR_UNAVAILABLE;
		goto done;
	}
	wcscpy_s(spec, ARRAYSIZE(spec), L"HEAD:");
	n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
	        spec + 5, (int)ARRAYSIZE(spec) - 5);
	if (0 == n) {
		goto done;
	}
	st = wf_git_exec(&rq->env, root, args, BASE_MS, BASE_MAX, job_cancelled,
	        &job, &r);
	if (WF_OK == st && 0 == r.exit_code) {
		a.base = wf_gitblob_new(
		        r.out ? (const void *)r.out : "", r.out_n);
		a.state = a.base ? WF_GITR_OK : WF_GITR_FAILED;
	} else if (WF_OK == st && is_new_at_head(&rq->env, root, spec, &job)) {
		/* Git itself says HEAD has no such path (or no commit at all): the file
		 * is new, whatever the status table knew or did not know yet */
		a.base = wf_gitblob_new("", 0);
		a.state = a.base ? WF_GITR_OK : WF_GITR_FAILED;
	} else if (WF_E_RANGE == st) {
		a.state = WF_GITR_TOO_LARGE;
	} else if (WF_E_NOENT == st) {
		a.state = WF_GITR_UNAVAILABLE;
	}
	wf_gitproc_result_free(&r);
done:
	publish(&job, &a);
}

static void diff_job(WfGitWorker *w, ReqDiff *rq)
{
	Job job = {w, K_DIFF, rq->gen};
	WfGitAnswer a;
	WfStatus st;

	answer_init(&a, WF_GIT_A_DIFF, rq->gen, rq->id);
	st = wf_linediff(rq->base->data, rq->base->n, rq->text, rq->n, &a.diff);
	if (WF_OK == st) {
		a.state = WF_GITR_OK;
	} else if (WF_E_RANGE == st) {
		a.state = WF_GITR_TOO_LARGE;
		a.diff_too_many = true;
	}
	publish(&job, &a);
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	WfGitWorker *w = arg;

	for (;;) {
		ReqRepo repo = {0};
		ReqBase base = {0};
		ReqDiff diff = {0};
		int kind;

		AcquireSRWLockExclusive(&w->core.lock);
		while (!w->core.stop && !w->diff.has && !w->base.has &&
		        !w->repo.has) {
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, INFINITE, 0);
		}
		if (w->core.stop) {
			ReleaseSRWLockExclusive(&w->core.lock);
			break;
		}
		/* what the editor waits for first */
		if (w->diff.has) {
			kind = K_DIFF;
			diff = w->diff;
			memset(&w->diff, 0, sizeof w->diff);
		} else if (w->base.has) {
			kind = K_BASE;
			base = w->base;
			memset(&w->base, 0, sizeof w->base);
		} else {
			kind = K_REPO;
			repo = w->repo;
			memset(&w->repo, 0, sizeof w->repo);
		}
		w->core.busy = true;
		ReleaseSRWLockExclusive(&w->core.lock);

		if (K_DIFF == kind) {
			diff_job(w, &diff);
			diff_clear(&diff);
		} else if (K_BASE == kind) {
			base_job(w, &base);
			base_clear(&base);
		} else {
			repo_job(w, &repo);
			repo_clear(&repo);
		}

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

WfStatus wf_gitworker_start(WfGitWorker **out)
{
	WfGitWorker *w;

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

WfWorkerStopReport wf_gitworker_stop(WfGitWorker *w)
{
	WfWorkerStopReport none;

	memset(&none, 0, sizeof none);
	if (!w) {
		return none;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	w->core.stop = true; /* a running process is ended at the next look */
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return wf_wc_stop(&w->core, STOP_WAIT_MS, false, worker_free, w);
}

bool wf_gitworker_idle(WfGitWorker *w)
{
	bool idle;

	if (!w) {
		return true;
	}
	AcquireSRWLockShared(&w->core.lock);
	idle = !w->core.busy && !w->repo.has && !w->base.has && !w->diff.has;
	for (int k = 0; k < K_N; k++) {
		idle = idle && !w->has_ans[k];
	}
	ReleaseSRWLockShared(&w->core.lock);
	return idle;
}

HANDLE wf_gitworker_event(const WfGitWorker *w)
{
	return w->core.ready;
}

bool wf_gitworker_poll(WfGitWorker *w, WfGitAnswer *out)
{
	bool got = false;

	AcquireSRWLockExclusive(&w->core.lock);
	for (int k = K_DIFF; k >= K_REPO && !got; k--) {
		if (w->has_ans[k]) {
			w->has_ans[k] = false;
			if ((LONG)w->ans[k].gen ==
			        w->newest[k]) { /* still the wanted one */
				*out = w->ans[k];
				memset(&w->ans[k], 0, sizeof w->ans[k]);
				got = true;
			} else {
				wf_gitanswer_free(&w->ans[k]);
			}
		}
	}
	if (!w->has_ans[K_REPO] && !w->has_ans[K_BASE] && !w->has_ans[K_DIFF]) {
		ResetEvent(w->core.ready);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	return got;
}

WfStatus wf_gitworker_repo(WfGitWorker *w,
        uint32_t gen,
        const WfGitEnv *env,
        const wchar_t *root,
        int mode)
{
	ReqRepo r;

	if (!w || !gen || !env || !root || WF_GENV_OTHER == env->kind) {
		return WF_E_INVAL;
	}
	memset(&r, 0, sizeof r);
	r.root = wf_path_dup(root);
	if (!r.root) {
		return WF_E_NOMEM;
	}
	r.has = true;
	r.gen = gen;
	r.env = *env;
	r.mode = mode;
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		repo_clear(&r);
		return WF_E_INVAL;
	}
	repo_clear(&w->repo);
	w->repo = r;
	InterlockedExchange(&w->newest[K_REPO], (LONG)gen);
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

static char *dup_utf8(const char *s)
{
	size_t n = strlen(s) + 1;
	char *c = malloc(n);

	if (c) {
		memcpy(c, s, n);
	}
	return c;
}

WfStatus wf_gitworker_base(WfGitWorker *w,
        uint32_t gen,
        uint64_t id,
        const WfGitEnv *env,
        const wchar_t *root,
        const char *rel,
        const char *orig,
        bool expect_new)
{
	ReqBase r;

	if (!w || !gen || !env || !root || !rel || !rel[0] ||
	        WF_GENV_OTHER == env->kind) {
		return WF_E_INVAL;
	}
	memset(&r, 0, sizeof r);
	r.root = wf_path_dup(root);
	r.rel = dup_utf8(rel);
	r.orig = orig ? dup_utf8(orig) : NULL;
	if (!r.root || !r.rel || (orig && !r.orig)) {
		base_clear(&r);
		return WF_E_NOMEM;
	}
	r.has = true;
	r.gen = gen;
	r.id = id;
	r.env = *env;
	r.expect_new = expect_new;
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		base_clear(&r);
		return WF_E_INVAL;
	}
	base_clear(&w->base);
	w->base = r;
	InterlockedExchange(&w->newest[K_BASE], (LONG)gen);
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

WfStatus wf_gitworker_base_of(WfGitWorker *w,
        uint32_t gen,
        uint64_t id,
        const WfGitEnv *env,
        const wchar_t *path)
{
	ReqBase r;

	if (!w || !gen || !env || !path || !path[0] ||
	        WF_GENV_OTHER == env->kind) {
		return WF_E_INVAL;
	}
	memset(&r, 0, sizeof r);
	r.path = wf_path_dup(path);
	if (!r.path) {
		return WF_E_NOMEM;
	}
	r.has = true;
	r.gen = gen;
	r.id = id;
	r.env = *env;
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		base_clear(&r);
		return WF_E_INVAL;
	}
	base_clear(&w->base);
	w->base = r;
	InterlockedExchange(&w->newest[K_BASE], (LONG)gen);
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

WfStatus wf_gitworker_diff(WfGitWorker *w,
        uint32_t gen,
        uint64_t id,
        WfGitBlob *base,
        const char *text,
        size_t n)
{
	ReqDiff r;

	if (!w || !gen || !base || (!text && n)) {
		return WF_E_INVAL;
	}
	memset(&r, 0, sizeof r);
	r.text = malloc(n + 1);
	if (!r.text) {
		return WF_E_NOMEM;
	}
	if (n) {
		memcpy(r.text, text, n);
	}
	r.text[n] = '\0';
	r.n = n;
	r.base = wf_gitblob_ref(base);
	r.has = true;
	r.gen = gen;
	r.id = id;
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		diff_clear(&r);
		return WF_E_INVAL;
	}
	diff_clear(&w->diff);
	w->diff = r;
	InterlockedExchange(&w->newest[K_DIFF], (LONG)gen);
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

void wf_gitworker_cancel(WfGitWorker *w)
{
	if (!w) {
		return;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	repo_clear(&w->repo);
	base_clear(&w->base);
	diff_clear(&w->diff);
	for (int k = 0; k < K_N; k++) {
		InterlockedExchange(&w->newest[k],
		        0); /* a running job is no longer wanted */
		wf_gitanswer_free(&w->ans[k]);
		w->has_ans[k] = false;
	}
	ResetEvent(w->core.ready);
	ReleaseSRWLockExclusive(&w->core.lock);
}

void wf_gitworker_cancel_editor(WfGitWorker *w)
{
	if (!w) {
		return;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	base_clear(&w->base);
	diff_clear(&w->diff);
	for (int k = K_BASE; k <= K_DIFF; k++) {
		InterlockedExchange(&w->newest[k],
		        0); /* a running job is no longer wanted */
		wf_gitanswer_free(&w->ans[k]);
		w->has_ans[k] = false;
	}
	if (!w->has_ans[K_REPO]) {
		ResetEvent(w->core.ready);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
}

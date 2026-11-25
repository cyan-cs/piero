#include "main_loop.h"
#include "wf_syntax_pin.h"
static bool language_arg(const wchar_t *text, int *setting);

static void start_image_service(Loop *lp, const WfConfig *config);
static bool write_last_directory(
        const wchar_t *output, const wchar_t *directory)
{
	wchar_t *extended = NULL;
	char *bytes = NULL;
	HANDLE file = INVALID_HANDLE_VALUE;
	DWORD wrote;
	int need;
	bool ok = false;
	const BYTE bom[] = {0xef, 0xbb, 0xbf};
	if (!output) {
		return true;
	}
	need = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, directory, -1,
	        NULL, 0, NULL, NULL);
	if (!need || !(bytes = malloc((size_t)need))) {
		goto done;
	}
	if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, directory, -1,
	            bytes, need, NULL, NULL) ||
	        WF_OK != wf_path_to_extended(output, &extended)) {
		goto done;
	}
	/* An explicit output still must not silently overwrite an existing
	 * file. */
	file = CreateFileW(extended, GENERIC_WRITE, FILE_SHARE_READ, NULL,
	        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == file) {
		goto done;
	}
	ok = WriteFile(file, bom, sizeof bom, &wrote, NULL) &&
	        wrote == sizeof bom &&
	        WriteFile(file, bytes, (DWORD)need - 1, &wrote, NULL) &&
	        wrote == (DWORD)need - 1 && FlushFileBuffers(file);
done:
	if (INVALID_HANDLE_VALUE != file) {
		CloseHandle(file);
	}
	free(extended);
	free(bytes);
	if (!ok) {
		fputs("piero: cannot write --print-last-dir output (existing "
		      "files are preserved)\n",
		        stderr);
	}
	return ok;
}

/* The search below a folder reads each folder with the program's own
 * enumerator, for the names only: nothing here is drawn from a listing. */
static WfStatus search_list(
        const wchar_t *dir, bool (*cancel)(void *), void *cctx, WfDir **out)
{
	return wf_enum_names_win(dir, cancel, cctx, out);
}

static void worker_stopped(
        Loop *lp, const char *name, WfWorkerStopReport report)
{
	/* Product probe contexts are static; no callback borrows the
	 * app/config. */
	if (report.completion) {
		CloseHandle(report.completion);
	}
	if (WF_OK != report.status || report.cancel_error) {
		fprintf(stderr,
		        "piero: %s stop %s (Win32 %lu, cancel %lu, "
		        "deferred=%d)\n",
		        name, wf_status_name(report.status), report.error,
		        report.cancel_error, report.deferred);
		wf_main_diagnose(lp, WF_DIAG_STARTUP, WF_DIAG_FAILED, 0,
		        report.error ? report.error : report.cancel_error);
	}
	if (WF_LOG_DEBUG == lp->config->log_level) {
		fprintf(stderr,
		        "piero: %s worker completed=%llu superseded=%llu "
		        "discarded=%llu\n",
		        name,
		        (unsigned long long)report.stats.completed_requests,
		        (unsigned long long)report.stats.superseded_requests,
		        (unsigned long long)report.stats.discarded_results);
	}
}

static WfStatus copy_submit(void *ctx, const WfCopySpec *spec, uint32_t *job)
{
	return wf_copyworker_submit(ctx, spec, job);
}

static void copy_pause(void *ctx, bool paused)
{
	wf_copyworker_set_paused(ctx, paused);
}

static bool copy_paused(void *ctx)
{
	return wf_copyworker_paused(ctx);
}

static void copy_cancel(void *ctx, bool all)
{
	wf_copyworker_cancel(ctx, all);
}

static bool copy_progress(void *ctx, WfCopyProgress *out)
{
	return wf_copyworker_progress(ctx, out);
}

static WfCopyPort copy_port(Loop *lp)
{
	WfCopyPort port = {lp->copy, copy_submit, copy_pause, copy_paused,
	        copy_cancel, copy_progress};

	return port;
}

/* Receipt text is an inventory, never recovery authority. Unknown or legacy
 * state must be reviewed before the copy/editor services append to it. */
static void start_copy(Loop *lp)
{
	wchar_t *dir = NULL, *journal = NULL;
	WfStateKind kind = WF_STATE_FRESH;
	DWORD error = 0;
	WfStatus st;

	wf_efile_set_journal(
	        NULL); /* until there is one, no save replaces a file */
	if (WF_OK !=
	                wf_startup_state_path(
	                        L"LOCALAPPDATA", L"", &dir, &kind, &error) ||
	        wf_startup_state_path(L"LOCALAPPDATA", L"copy-journal.txt",
	                &journal, NULL, &error) != WF_OK) {
		fputs("piero: copying unavailable (no local state folder)\n",
		        stderr);
		wf_efile_set_journal_unavailable(WF_EFJ_NO_STATE, error);
		lp->copy_off_set = true;
		lp->copy_off = MSG_COPY_OFF_STATE_FOLDER;
		goto done;
	}
	if (!CreateDirectoryW(dir, NULL) &&
	        ERROR_ALREADY_EXISTS != (error = GetLastError())) {
		fputs("piero: copying unavailable (cannot create the local "
		      "state folder)\n",
		        stderr);
		wf_efile_set_journal_unavailable(WF_EFJ_NO_STATE, error);
		lp->copy_off_set = true;
		lp->copy_off = MSG_COPY_OFF_STATE_FOLDER;
		goto done;
	}
	if (WF_STATE_BOTH == kind && wf_main_old_state_blocks()) {
		/* records may be waiting in the older folder: nothing that
		 * replaces or removes a file is started until they are dealt
		 * with (DR-DRAFT-017); the files themselves are not touched */
		fputs("piero: copying/replacing saves unavailable: unresolved "
		      "records in the older state folder\n",
		        stderr);
		wf_efile_set_journal_unavailable(WF_EFJ_FORMAT, 0);
		lp->copy_off_set = true;
		lp->copy_off = MSG_COPY_SAFETY_BLOCKED;
		goto done;
	}
	if (!wf_cj_inspect(journal, &lp->copy_recovery) ||
	        lp->copy_recovery.format == WF_CJ_LEGACY) {
		fputs("piero: copying/replacing saves unavailable: receipt "
		      "inventory requires manual review\n",
		        stderr);
		/* an old or damaged record is kept as it is: a gate, not a failure */
		wf_efile_set_journal_unavailable(
		        lp->copy_recovery.format == WF_CJ_LEGACY ||
		                        lp->copy_recovery.format ==
		                                WF_CJ_INVALID
		                ? WF_EFJ_FORMAT
		                : WF_EFJ_INSPECT,
		        lp->copy_recovery.error);
		lp->copy_off_set = true;
		lp->copy_off = MSG_COPY_SAFETY_BLOCKED;
		goto done;
	}
	if (lp->copy_recovery.records) {
		fputs("piero: copy receipts retained; crash recovery is "
		      "manual\n",
		        stderr);
	}
	/* the editor's and the settings' replacing saves record their swap there */
	wf_efile_set_journal(journal);
	st = wf_copyworker_start(&lp->copy, journal);
	if (WF_OK != st) {
		lp->copy = NULL;
		lp->copy_off_set = true;
		lp->copy_off = MSG_COPY_OFF_RECORD;
		fprintf(stderr, "piero: copying unavailable (%s)\n",
		        wf_status_name(st));
	}
done:
	free(dir);
	free(journal);
}

#ifdef WF_PRODUCT_EDIT
/* The bulk rename's thread, with its journals in the batch subfolder of the state
 * folder. Without it M says it is not available; nothing else depends on it. */
static void start_batch(Loop *lp)
{
	wchar_t *state = NULL, *dir = NULL;
	DWORD error = 0;

	lp->batch = NULL;
	if (WF_OK ==
	                wf_startup_state_path(
	                        L"LOCALAPPDATA", L"", &state, NULL, &error) &&
	        WF_OK ==
	                wf_startup_state_path(L"LOCALAPPDATA", L"batch", &dir,
	                        NULL, &error)) {
		/* the engine makes the batch folder; its parent must be there */
		if ((CreateDirectoryW(state, NULL) ||
		            ERROR_ALREADY_EXISTS == GetLastError()) &&
		        WF_OK != wf_batchworker_start(&lp->batch, dir)) {
			lp->batch = NULL;
		}
	}
	free(state);
	free(dir);
}
#endif

/*
 * The workers of the four on-demand features (# D L F) are started by the
 * first request that needs one, not with the program, and are stopped again
 * when they have nothing in flight and nothing to poll (DR-DRAFT-035 and 036).
 * The screen holds a port whose context is the loop; each call makes sure the
 * worker is there and then calls the worker's own port. Only the UI thread
 * makes, polls and stops them, so there is nothing to guard: a NULL pointer is
 * "not running", a start that fails leaves it NULL and says why, and the next
 * request tries again. What a request made is released with the request; what
 * the screen shows (a digest, the sizes, the hits, who has a file) is the
 * screen's, not the worker's, and stays.
 */
#ifdef _DEBUG
static void module_trace(const char *name, const char *change)
{
	char line[96];

	snprintf(line, sizeof line, "[module:%s] %s\n", name, change);
	OutputDebugStringA(line);
}
#else
#	define module_trace(name, change) ((void)(name), (void)(change))
#endif

static WfStatus hash_ready(Loop *lp)
{
	WfStatus st;

	if (lp->hash) {
		return WF_OK;
	}
	st = wf_hashworker_start(&lp->hash);
	if (WF_OK == st) {
		module_trace("hash", "inactive -> active");
	}
	return st;
}

static WfStatus size_ready(Loop *lp)
{
	WfStatus st;

	if (lp->size) {
		return WF_OK;
	}
	st = wf_sizeworker_start(&lp->size);
	if (WF_OK == st) {
		module_trace("dirsize", "inactive -> active");
	}
	return st;
}

static WfStatus lock_ready(Loop *lp)
{
	WfStatus st;

	if (lp->lock) {
		return WF_OK;
	}
	st = wf_lockworker_start(&lp->lock, NULL);
	if (WF_OK == st) {
		module_trace("lock", "inactive -> active");
	}
	return st;
}

static WfStatus search_ready(Loop *lp)
{
	WfStatus st;

	if (lp->search) {
		return WF_OK;
	}
	st = wf_searchworker_start(&lp->search, search_list);
	if (WF_OK == st) {
		wf_searchworker_set_hash(lp->search, wf_dupehash_file);
		module_trace("search", "inactive -> active");
	}
	return st;
}

static WfStatus clip_ready(Loop *lp)
{
	WfStatus st;

	if (lp->clip) {
		return WF_OK;
	}
	st = wf_clipworker_start(&lp->clip);
	if (WF_OK == st) {
		module_trace("clip", "inactive -> active");
	}
	return st;
}

static WfStatus image_ready(Loop *lp)
{
	WfImageWorkerOptions o;
	WfStatus st;

	if (lp->image) {
		return WF_OK;
	}
	memset(&o, 0, sizeof o);
	o.sandbox = 1;
	o.cache_bytes = (size_t)lp->config->image_cache_mib << 20;
	st = wf_imageworker_start(&lp->image, &o);
	if (WF_OK == st) {
		lp->image_needed_ms = GetTickCount64();
		module_trace("image", "inactive -> active");
	}
	return st;
}

static WfStatus launch_ready(Loop *lp)
{
	WfStatus st;

	if (lp->launch) {
		return WF_OK;
	}
	st = wf_launchworker_start(&lp->launch);
	if (WF_OK == st) {
		module_trace("launch", "inactive -> active");
	}
	return st;
}

/* A worker that was stopped while idle: its thread has been asked to leave and
 * has, or is handed over like any other at exit; nothing is waiting on it. */
static void worker_released(
        Loop *lp, const char *name, WfWorkerStopReport report)
{
	if (report.completion) {
		CloseHandle(report.completion);
	}
	if (WF_OK != report.status || report.cancel_error) {
		wf_main_diagnose(lp, WF_DIAG_STARTUP, WF_DIAG_FAILED, 0,
		        report.error ? report.error : report.cancel_error);
	}
	module_trace(name, "active -> inactive");
}

/* The two modules that hold most (DR-DRAFT-037): the decoder with its picture
 * cache, and the indexes of archives. Neither is released at once: what is on
 * screen is in use, and the time counts from the turn it left the screen. */
#define IMAGE_IDLE_MS 120000u
#define ARCHIVE_IDLE_MS 300000u

static bool image_in_view(const Loop *lp)
{
	return WF_IMG_NONE != lp->app.image.state;
}

void wf_main_note_modules_in_use(Loop *lp)
{
	if (lp->image &&
	        (image_in_view(lp) || !wf_imageworker_idle(lp->image))) {
		lp->image_needed_ms = lp->app.now_ms;
	}
	if (wf_app_archive_in_view(&lp->app)) {
		wf_archive_cache_touch(lp->app.now_ms);
	}
}

static void release_heavy_modules(Loop *lp)
{
	uint64_t now = lp->app.now_ms;

	wf_main_note_modules_in_use(lp);
	if (lp->image && now - lp->image_needed_ms >= lp->idle_image_ms) {
		worker_released(lp, "image", wf_imageworker_stop(lp->image));
		lp->image = NULL;
	}
	if (0 == wf_archive_cache_idle_wait(now, lp->idle_archive_ms)) {
		wf_archive_cache_clear();
		module_trace("archive", "index cache released");
	}
}

uint32_t wf_main_idle_timeout(const Loop *lp, uint64_t now_ms)
{
	uint64_t wait = UINT64_MAX, a;

	if (lp->image) {
		uint64_t due = lp->image_needed_ms + lp->idle_image_ms;

		wait = due > now_ms ? due - now_ms : 1;
	}
	a = wf_archive_cache_idle_wait(now_ms, lp->idle_archive_ms);
	if (a < wait) {
		wait = a ? a : 1;
	}
	return wait >= WF_WAIT_FOREVER ? WF_WAIT_FOREVER : (uint32_t)wait;
}

/* Called at the end of every turn, after the answers were delivered: a worker
 * with no request in flight and no answer waiting is stopped, which frees its
 * thread, its handles and everything a request left in it. Nothing is lost: the
 * screen has what it was given, and a request after this starts the worker
 * again (about 0.05 ms). Workers that were never started are skipped without
 * touching a lock. */
void wf_main_release_idle_workers(Loop *lp)
{
	if (lp->hash && wf_hashworker_idle(lp->hash)) {
		worker_released(lp, "hash", wf_hashworker_stop(lp->hash));
		lp->hash = NULL;
	}
	if (lp->size && wf_sizeworker_idle(lp->size)) {
		worker_released(lp, "dirsize", wf_sizeworker_stop(lp->size));
		lp->size = NULL;
	}
	if (lp->lock && wf_lockworker_idle(lp->lock)) {
		worker_released(lp, "lock", wf_lockworker_stop(lp->lock));
		lp->lock = NULL;
	}
	if (lp->search && wf_searchworker_idle(lp->search)) {
		worker_released(lp, "search", wf_searchworker_stop(lp->search));
		lp->search = NULL;
	}
	if (lp->clip && wf_clipworker_idle(lp->clip)) {
		worker_released(lp, "clip", wf_clipworker_stop(lp->clip));
		lp->clip = NULL;
	}
	if (lp->launch && wf_launchworker_idle(lp->launch)) {
		worker_released(lp, "launch", wf_launchworker_stop(lp->launch));
		lp->launch = NULL;
	}
	if (lp->git && wf_gitworker_idle(lp->git)) {
		worker_released(lp, "git", wf_gitworker_stop(lp->git));
		lp->git = NULL;
	}
	release_heavy_modules(lp);
}

static WfStatus lazy_hash_start(void *ctx, uint32_t gen, const wchar_t *path)
{
	Loop *lp = ctx;
	WfStatus st = hash_ready(lp);
	WfHashPort p;

	if (WF_OK != st) {
		return st;
	}
	p = wf_hashworker_port(lp->hash);
	return p.start(p.ctx, gen, path);
}

static void lazy_hash_cancel(void *ctx)
{
	Loop *lp = ctx;

	if (lp->hash) {
		WfHashPort p = wf_hashworker_port(lp->hash);

		p.cancel(p.ctx);
	}
}

static WfStatus lazy_size_start(void *ctx,
        uint32_t gen,
        const wchar_t *dir,
        const wchar_t *const *names,
        size_t n)
{
	Loop *lp = ctx;
	WfStatus st = size_ready(lp);
	WfSizePort p;

	if (WF_OK != st) {
		return st;
	}
	p = wf_sizeworker_port(lp->size);
	return p.start(p.ctx, gen, dir, names, n);
}

static void lazy_size_cancel(void *ctx)
{
	Loop *lp = ctx;

	if (lp->size) {
		WfSizePort p = wf_sizeworker_port(lp->size);

		p.cancel(p.ctx);
	}
}

static WfStatus lazy_lock_query(void *ctx, uint32_t gen, const wchar_t *path)
{
	Loop *lp = ctx;
	WfStatus st = lock_ready(lp);
	WfLockPort p;

	if (WF_OK != st) {
		return st;
	}
	p = wf_lockworker_port(lp->lock);
	return p.query(p.ctx, gen, path);
}

static WfStatus lazy_lock_close(void *ctx,
        uint32_t gen,
        uint32_t pid,
        uint64_t start,
        WfLockAppType type)
{
	Loop *lp = ctx;
	WfStatus st = lock_ready(lp);
	WfLockPort p;

	if (WF_OK != st) {
		return st;
	}
	p = wf_lockworker_port(lp->lock);
	return p.close(p.ctx, gen, pid, start, type);
}

static WfStatus lazy_lock_end(void *ctx,
        uint32_t gen,
        uint32_t pid,
        uint64_t start,
        WfLockAppType type)
{
	Loop *lp = ctx;
	WfStatus st = lock_ready(lp);
	WfLockPort p;

	if (WF_OK != st) {
		return st;
	}
	p = wf_lockworker_port(lp->lock);
	return p.end(p.ctx, gen, pid, start, type);
}

static WfStatus lazy_search_start(void *ctx,
        uint32_t gen,
        const wchar_t *root,
        WfFileId root_id,
        const WfQuery *query,
        bool hidden)
{
	Loop *lp = ctx;
	WfStatus st = search_ready(lp);
	WfSearchPort p;

	if (WF_OK != st) {
		return st;
	}
	p = wf_searchworker_port(lp->search);
	return p.start(p.ctx, gen, root, root_id, query, hidden);
}

static WfStatus lazy_search_dupes(void *ctx,
        uint32_t gen,
        const wchar_t *root,
        WfFileId root_id,
        bool hidden)
{
	Loop *lp = ctx;
	WfStatus st = search_ready(lp);
	WfSearchPort p;

	if (WF_OK != st) {
		return st;
	}
	p = wf_searchworker_port(lp->search);
	return p.start_dupes(p.ctx, gen, root, root_id, hidden);
}

static WfStatus lazy_clip_read(void *ctx, uint32_t gen)
{
	Loop *lp = ctx;
	WfStatus st = clip_ready(lp);
	WfClipFilesPort p;

	if (WF_OK != st) {
		return st;
	}
	p = wf_clipworker_port(lp->clip);
	return p.read(p.ctx, gen);
}

static WfStatus lazy_clip_publish(
        void *ctx, uint32_t gen, const wchar_t *const *path, size_t n)
{
	Loop *lp = ctx;
	WfStatus st = clip_ready(lp);
	WfClipFilesPort p;

	if (WF_OK != st) {
		return st;
	}
	p = wf_clipworker_port(lp->clip);
	return p.publish(p.ctx, gen, path, n);
}

static WfStatus lazy_launch_submit(
        void *ctx, uint32_t gen, const WfLaunchRequest *req)
{
	Loop *lp = ctx;
	WfStatus st = launch_ready(lp);
	WfLaunchPort p;

	if (WF_OK != st) {
		return st;
	}
	p = wf_launchworker_port(lp->launch);
	return p.submit(p.ctx, gen, req);
}

static WfStatus lazy_image_submit(void *ctx, const WfImageRequest *rq)
{
	Loop *lp = ctx;
	WfStatus st = image_ready(lp);
	WfImagePort p;

	if (WF_OK != st) {
		return st;
	}
	p = wf_imageworker_port(lp->image);
	return p.submit(p.ctx, rq);
}

/* A worker that is not there has nothing to cancel. */
static void lazy_image_cancel(void *ctx)
{
	Loop *lp = ctx;

	if (lp->image) {
		WfImagePort p = wf_imageworker_port(lp->image);

		p.cancel(p.ctx);
	}
}

static WfStatus git_ready(Loop *lp)
{
	WfStatus st;

	if (lp->git) {
		return WF_OK;
	}
	st = wf_gitworker_start(&lp->git);
	if (WF_OK == st) {
		module_trace("git", "inactive -> active");
	}
	return st;
}

/* Whether Git is asked about `dir` at all, and where: its environment and the
 * nearest folder above it with a `.git`. File attributes only; a folder that is
 * in no work tree costs nothing else, not even the worker. */
/* Whether looking for a `.git` above a folder is quick enough to be done here: a
 * fixed drive, whose attributes answer at once. A WSL folder is reached through
 * the distribution, a removable drive may be asleep: the walk there waits, and
 * the worker does it. */
static bool walk_is_cheap(const wchar_t *dir)
{
	const wchar_t *d = dir + (!_wcsnicmp(dir, L"\\\\?\\", 4) ? 4 : 0);
	wchar_t drive[4] = {d[0], L':', L'\\', L'\0'};
	UINT type = GetDriveTypeW(drive);

	return DRIVE_FIXED == type || DRIVE_RAMDISK == type;
}

/* What the screen can tell about a folder without waiting: which Git it is for
 * and, on a fixed drive, the nearest folder with a `.git` (file attributes
 * only). Elsewhere it is "later": nothing is touched here that may block, and the
 * worker finds the `.git` (a folder in no work tree costs a thread's moment and
 * no process). */
static WfGitRootKind git_root_of(
        void *ctx, const wchar_t *dir, WfGitEnv *env, wchar_t *root, size_t cap)
{
	(void)ctx;
	if (WF_GENV_OTHER == wf_gitenv_classify(dir, env)) {
		return WF_GIT_ROOT_NONE;
	}
	if (WF_GENV_WINDOWS == env->kind && walk_is_cheap(dir)) {
		return wf_git_nearest_dotgit(dir, root, cap) ? WF_GIT_ROOT_FOUND
		                                             : WF_GIT_ROOT_NONE;
	}
	if (wcslen(dir) + 1 > cap) {
		return WF_GIT_ROOT_NONE;
	}
	wcscpy_s(root, cap, dir);
	return WF_GIT_ROOT_LATER;
}

static WfStatus lazy_git_repo(
        void *ctx, uint32_t gen, const wchar_t *root, int mode)
{
	Loop *lp = ctx;
	WfGitEnv env;
	WfStatus st;

	if (WF_GENV_OTHER == wf_gitenv_classify(root, &env)) {
		return WF_E_INVAL;
	}
	st = git_ready(lp);
	return WF_OK == st ? wf_gitworker_repo(lp->git, gen, &env, root, mode)
	                   : st;
}

/* The base of the file an editor buffer holds. When the repository on screen is
 * the file's, its root is known and so is what the file list says about the file
 * (new, or renamed from). Otherwise the worker finds the repository itself:
 * nothing here walks folders or waits. */
static WfStatus lazy_git_base(
        void *ctx, uint32_t gen, uint64_t id, const wchar_t *path)
{
	Loop *lp = ctx;
	WfGitEnv env;
	const WfApp *app = &lp->app;
	char rel[4400], orig[4400];
	const wchar_t *root;
	bool expect_new;
	WfStatus st;

	if (WF_GENV_OTHER == wf_gitenv_classify(path, &env)) {
		return WF_E_NOENT;
	}
	st = git_ready(lp);
	if (WF_OK != st) {
		return st;
	}
	if (wf_app_git_base_hint(app, &env, path, &root, rel, sizeof rel,
	            &expect_new, orig, sizeof orig)) {
		return wf_gitworker_base(lp->git, gen, id, &env, root, rel,
		        orig[0] ? orig : NULL, expect_new);
	}
	return wf_gitworker_base_of(lp->git, gen, id, &env, path);
}

static WfStatus lazy_git_diff(void *ctx,
        uint32_t gen,
        uint64_t id,
        WfGitBlob *base,
        const char *text,
        size_t n)
{
	Loop *lp = ctx;
	WfStatus st = git_ready(lp);

	return WF_OK == st ? wf_gitworker_diff(lp->git, gen, id, base, text, n)
	                   : st;
}

static void lazy_git_cancel(void *ctx)
{
	Loop *lp = ctx;

	/* Only the editor's requests: the status the file list waits for is not the
	 * editor's to drop (a settings save makes a new editor, and this runs).
	 * NULL-safe: nothing runs when there is no worker. */
	wf_gitworker_cancel_editor(lp->git);
}

WfGitPort wf_main_git_port(Loop *lp)
{
	return (WfGitPort){lp, git_root_of, lazy_git_repo, lazy_git_base,
	        lazy_git_diff, lazy_git_cancel};
}

static void lazy_search_cancel(void *ctx)
{
	Loop *lp = ctx;

	if (lp->search) {
		WfSearchPort p = wf_searchworker_port(lp->search);

		p.cancel(p.ctx);
	}
}

/* Starts the workers the loop waits on. Failures with a plain message happen
 * before the alternate screen opens, so they stay visible on the real one. */
static bool start_services(Loop *lp)
{
	const WfConfig *config = lp->config;
	WfStatus st = wf_fsworker_start(&lp->fs);

	if (WF_OK != st) {
		goto failed;
	}
	st = wf_xferworker_start(
	        &lp->xfer, wf_probe_win(), WF_PREFLIGHT_DEFAULT);
	if (WF_OK != st) {
		goto failed;
	}
	lp->idle = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!lp->idle) {
		return false;
	}
	start_copy(lp);
#ifdef WF_PRODUCT_EDIT
	st = wf_editworker_start(&lp->edit);
	if (WF_OK != st) {
		goto failed;
	}
	start_batch(lp);
#endif
	st = wf_fsworker_start_limited(&lp->dirprev, WF_DIRPREV_MAX_ENTRIES);
	if (WF_OK != st) {
		goto failed;
	}
	/* without it Smart Go still resolves paths; it just offers no folder
	 * entries */
	if (WF_OK != wf_candworker_start(&lp->cand)) {
		lp->cand = NULL;
	}
	/* without it a huge list cannot be searched (the screen says so, and does
	 * not search it on its own thread); a small one is searched there */
	if (WF_OK != wf_fuzzyworker_start(&lp->fuzzy)) {
		lp->fuzzy = NULL;
	}
	/* without it nothing is coloured by a grammar; nothing else depends on it.
	 * It loads nothing until a file that an installed grammar can colour is
	 * shown (DR-DRAFT-033). */
	wf_synhost_configure(NULL, WF_SYNTAX_DLL_SHA256);
	if (WF_OK != wf_synworker_start(&lp->syntax)) {
		lp->syntax = NULL;
	}
	/* names in archives that are not UTF-8 are read in this code page */
	wf_archive_set_codepage((UINT)config->text_fallback_encoding);
	if (config->preview_enabled) {
		st = wf_previewworker_start(&lp->preview,
		        (size_t)config->text_max_kib * 1024u,
		        (UINT)config->text_fallback_encoding);
		if (WF_OK != st) {
			fprintf(stderr,
			        "piero: cannot start preview worker (%s)\n",
			        wf_status_name(st));
			return false;
		}
	} else {
		lp->preview_disabled = CreateEventW(NULL, TRUE, FALSE, NULL);
		if (!lp->preview_disabled) {
			return false;
		}
	}
	start_image_service(lp, config);
	return true;
failed:
	fprintf(stderr, "piero: cannot start worker (%s)\n",
	        wf_status_name(st));
	return false;
}

/* What "default" means for how a picture is drawn: half blocks in Windows
 * Terminal, which draws them and every colour well; plain characters where
 * that is not known. */
static int resolve_image_style(const WfConfig *config)
{
	wchar_t wt[8];

	if (!config->images || WF_IMAGE_OFF == config->image_style) {
		return WF_IMAGE_OFF;
	}
	if (WF_IMAGE_DEFAULT != config->image_style) {
		return config->image_style;
	}
	return GetEnvironmentVariableW(L"WT_SESSION", wt, ARRAYSIZE(wt))
	        ? WF_IMAGE_HALFBLOCK
	        : WF_IMAGE_ASCII;
}

/* Pictures and PDF pages: one worker that owns the sandboxed decoder. It is
 * made by the first picture asked for and released when none has been on screen
 * for a while (DR-DRAFT-037); only the style is decided here. */
static void start_image_service(Loop *lp, const WfConfig *config)
{
	lp->image_style = resolve_image_style(config);
}

static const char *env_text(const char *name, char *buf, DWORD cap)
{
	DWORD n = GetEnvironmentVariableA(name, buf, cap);

	return n > 0 && n < cap ? buf : NULL;
}

/* How many colours the terminal gets, from the configuration and, for "auto",
 * the environment. The older [ui] color_mode = 256 still counts as a choice. */
static WfColorDepth depth_for(const WfConfig *c)
{
	char no_color[8], colorterm[32], wt[8], term[64];
	int policy = c->color_policy;

	if (WF_COLOR_AUTO == policy && WF_COLOR_256 == c->color_mode) {
		policy = WF_COLOR_POLICY_256;
	}
	return wf_color_depth_resolve(policy,
	        env_text("NO_COLOR", no_color, sizeof no_color),
	        env_text("COLORTERM", colorterm, sizeof colorterm),
	        env_text("WT_SESSION", wt, sizeof wt),
	        env_text("TERM", term, sizeof term));
}

/* The theme and colour depth the configuration asks for. A change of depth
 * means the whole screen has to be written again. */
static void apply_appearance(Loop *lp, const WfConfig *c)
{
	WfThemePalette p;
	WfColorDepth depth = depth_for(c);

	wf_config_palette(c, &p);
	wf_theme_activate(&p);
	wf_present_set_color(depth);
	if (depth != lp->depth && lp->prev) {
		for (size_t i = 0;
		        i < (size_t)lp->prev->cols * (size_t)lp->prev->rows;
		        i++) {
			lp->prev->cells[i].flags ^= 64;
		}
	}
	lp->depth = depth;
}

/* The editor session is configured once; with no file open it can be
 * rebuilt, which is how changed editor settings reach the next file. */
static WfStatus settings_save(void *ctx,
        const WfConfigChange *changes,
        size_t n,
        char *why,
        size_t cap)
{
	Loop *lp = ctx;
	WfIniSet *sets;
	WfConfigSaveResult r;
	WfStatus st;
	size_t i;

	if (!lp->config_path) {
		snprintf(why, cap, "%s", wf_msg(MSG_SETTINGS_WHY_NO_CONFIG));
		return WF_E_INVAL;
	}
	if (!n) {
		return WF_OK;
	}
	sets = wf_malloc_array(n, sizeof *sets);
	if (!sets) {
		snprintf(why, cap, "%s", wf_msg(MSG_NAV_NOMEM));
		return WF_E_NOMEM;
	}
	for (i = 0; i < n; i++) {
		sets[i].section = changes[i].section;
		sets[i].key = changes[i].key;
		sets[i].value = changes[i].remove ? NULL : changes[i].value;
	}
	st = wf_config_file_save(&lp->cfgfile, sets, n, &r);
	free(sets);
	if (WF_OK != st) {
		snprintf(why, cap, "%s", wf_msg(MSG_SETTINGS_PREPARE_FAILED));
		return st;
	}
	switch (r.verdict) {
	case WF_CS_OK: return WF_OK;
	case WF_CS_COMMITTED_UNVERIFIED:
		(void)wf_msgf(why, cap, MSG_CFG_SAVE_UNVERIFIED,
		        &WF_ARG_N(r.error), 1);
		return WF_E_OS;
	case WF_CS_CONFLICT:
		(void)wf_msgf(why, cap, MSG_CFG_SAVE_CONFLICT, NULL, 0);
		return WF_E_INVAL;
	case WF_CS_REFUSED:
		snprintf(why, cap, "%s",
		        r.reason ? wf_msg_reason(r.reason)
		                 : wf_msg(MSG_SETTINGS_PREPARE_FAILED));
		return WF_E_ACCESS;
	case WF_CS_IO: break;
	}
	(void)wf_msgf(why, cap, MSG_CFG_SAVE_IO_DETAIL,
	        (WfMsgArg[]){
	                WF_ARG_S(r.reason
	                                ? wf_msg_reason(r.reason)
	                                : wf_msg(MSG_SETTINGS_PREPARE_FAILED)),
	                WF_ARG_N(r.error)},
	        2);
	return WF_E_OS;
}

static void settings_apply(void *ctx, const WfConfig *config, bool committed)
{
	Loop *lp = ctx;
	if (committed && !config->update_checks) {
		wf_update_disable_automatic(lp->updates);
	}
	if (committed && config->update_checks) {
		lp->update_auto_attempted = false;
	}

	/* the screen shows the draft; only a saved configuration is live */
	apply_appearance(lp, config);
	/* the editor borrows the keymap: a new one (saved, or re-read from the
	 * file) means a new editor, as for the other editor settings */
	if (committed || lp->editor_keys != wf_app_keymap(&lp->app)) {
		wf_main_restart_editor(lp);
	}
}

static WfStatus settings_reload(void *ctx, WfConfig *fresh)
{
	Loop *lp = ctx;
	WfConfigReport report;
	DWORD error = 0;
	WfStatus st;

	if (!lp->config_path) {
		return WF_E_INVAL;
	}
	st = wf_config_read(
	        fresh, lp->config_path, true, NULL, NULL, &report, &error);
	if (WF_OK != st) {
		return st;
	}
	{
		wchar_t *path = _wcsdup(lp->config_path);

		if (!path) {
			return WF_E_NOMEM;
		}
		wf_config_file_close(&lp->cfgfile);
		st = wf_config_file_open(&lp->cfgfile, path);
		free(path);
	}
	return st;
}

/* Applies configuration and connects available services. Copy is connected
 * only when its worker was started; otherwise the startup refusal is shown. */
static void configure_app(Loop *lp)
{
	const WfConfig *config = lp->config;

	lp->app.ratios = (WfRatios){config->parent_ratio, config->current_ratio,
	        config->preview_ratio};
	lp->app.confirm_unc_navigation = config->confirm_unc_navigation;
	lp->app.recovery.refresh = wf_main_refresh_recovery;
	lp->app.recovery.context = lp;
	wf_main_refresh_recovery(lp);
	if (lp->app.recovery.unknown) {
		wf_main_diagnose(lp, WF_DIAG_RECOVERY, WF_DIAG_REFUSED, 0, 0);
	}
	wf_view_set_hidden(&lp->app.cur, config->show_hidden);
	wf_view_set_hidden(&lp->app.parent, config->show_hidden);
	wf_app_set_xfer(&lp->app, wf_xferworker_port(lp->xfer));
	if (lp->cand) {
		wf_app_set_candidates(&lp->app, wf_candworker_port(lp->cand));
	}
	/* # D L F: the ports are there from the start, their workers are not */
	wf_app_set_sizes(
	        &lp->app, (WfSizePort){lp, lazy_size_start, lazy_size_cancel});
	if (lp->fuzzy) {
		wf_app_set_fuzzy(&lp->app, wf_fuzzyworker_port(lp->fuzzy));
	}
	wf_app_set_hash(
	        &lp->app, (WfHashPort){lp, lazy_hash_start, lazy_hash_cancel});
	wf_app_set_search(&lp->app,
	        (WfSearchPort){lp, lazy_search_start, lazy_search_cancel,
	                lazy_search_dupes});
	if (lp->syntax) {
		wf_app_set_syntax(&lp->app, wf_synworker_port(lp->syntax));
	}
	wf_app_set_clip_files(&lp->app,
	        (WfClipFilesPort){lp, lazy_clip_read, lazy_clip_publish});
	wf_app_set_lock(&lp->app,
	        (WfLockPort){
	                lp, lazy_lock_query, lazy_lock_close, lazy_lock_end});
	wf_app_set_clipboard(&lp->app, (WfClipPort){NULL, wf_main_clip_get});
	wf_app_set_smartgo_env(&lp->app, wf_smartgo_env_win());
	{
		WfUiLang detected = wf_i18n_detect();

		wf_app_set_detected_language(&lp->app, detected);
		wf_i18n_set_language(wf_i18n_resolve(
		        (WfLangSetting)config->lang_setting, detected));
	}
	wf_app_set_local_time(&lp->app, wf_main_local_time);
	wf_app_set_dirpreview(
	        &lp->app, (WfDirPreviewPort){lp, wf_main_preview_list_submit});
	if (!config->dir_preview) {
		wf_app_dirpreview_enable(&lp->app, false);
	}
	if (lp->copy) {
		wf_app_set_copy(&lp->app, copy_port(lp), config->copy_verify);
	} else if (lp->copy_off_set) {
		wf_app_set_copy_off(&lp->app, lp->copy_off);
	}
	/* Open Terminal Here uses the launch worker too, and needs no private
	 * folder: only Open is held back without the folder */
	wf_app_set_terminal(&lp->app, (WfLaunchPort){lp, lazy_launch_submit});
	wf_app_set_git(&lp->app,
	        wf_main_git_port(lp)); /* only the way to ask: no probe */
	if (!lp->launch_enabled) {
		/* Open only when the private working folder was verified */
		wf_app_set_open_off(&lp->app, MSG_OPEN_OFF_PRIVATE);
	}
	if (lp->launch_enabled &&
	        wf_app_set_launch(&lp->app,
	                (WfLaunchPort){lp, lazy_launch_submit},
	                lp->launch_directory) != WF_OK) {
		fputs("piero: Open unavailable (out of memory)\n", stderr);
	}
	if (lp->preview) {
		wf_app_set_preview(
		        &lp->app, wf_previewworker_port(lp->preview));
	}
	if (WF_IMAGE_OFF != lp->image_style) {
		wf_app_set_image(&lp->app,
		        (WfImagePort){lp, lazy_image_submit, lazy_image_cancel},
		        lp->image_style);
	}
	wf_app_set_version(&lp->app, WF_VERSION);
	lp->app.update_context = lp;
	lp->app.update_request = wf_main_request_update;
	(void)wf_app_set_keys(&lp->app, &config->keys,
	        1 == config->editor_keymap ? WF_KM_NANO : WF_KM_PIERO);
	wf_app_keys_warn(&lp->app);
	wf_main_start_editor(lp);
#ifdef WF_PRODUCT_EDIT
	/* Create/rename. Absent only from a -DWF_PRODUCT_EDIT=OFF (read-only)
	 * build, whose forms say so and change nothing. */
	wf_app_set_edit(&lp->app, wf_editworker_port(lp->edit));
	if (lp->batch) {
		wf_app_set_batch(&lp->app, wf_batchworker_port(lp->batch));
		/* only counts the journals of earlier runs; the question about
		 * them is asked when the screen is free, never answered here */
		wf_app_batch_startup(&lp->app);
	}
#endif
	apply_appearance(lp, config);
	wf_app_set_settings(&lp->app, lp->config,
	        (WfSettingsPort){
	                lp, settings_save, settings_apply, settings_reload});
	if (lp->first_run) {
		wf_app_setup_begin(&lp->app, true);
	}
}

/* Opens the console and sizes the app to it. */
static bool open_console(Loop *lp)
{
	WfStatus st = wf_term_open(&lp->term);

	if (WF_OK != st) {
		fprintf(stderr, "piero: cannot open console (%s)\n",
		        wf_status_name(st));
		return false;
	}
	return WF_OK == wf_term_size(lp->term, &lp->app.cols, &lp->app.rows);
}

/* Stops every worker and releases what the loop owns. Stopping order: the
 * edit worker first, so a started mutation finishes before the rest goes. */
static void stop_services(Loop *lp)
{
	wf_update_release(lp->updates);
	lp->updates = NULL;
	wf_watch_close(lp->watch);
	/* a running job is cancelled and removes its temporary before we go */
	worker_stopped(lp, "copy", wf_copyworker_stop(lp->copy));
	lp->copy = NULL;
#ifdef WF_PRODUCT_EDIT
	worker_stopped(lp, "edit", wf_editworker_stop(lp->edit));
	lp->edit = NULL;
	/* a running batch is asked to stop and puts its names back first */
	worker_stopped(lp, "batch", wf_batchworker_stop(lp->batch));
	lp->batch = NULL;
#endif
	wf_es_free(lp->editor);
	lp->editor = NULL;
	/* the drafts that wait for the disk are written before the run ends */
	wf_edit_io_drafts_stop(3000);
	/* UI teardown can cancel searches and previews through borrowed ports.
	 * Finish it before consuming any of those worker references. */
	if (lp->app_ready) {
		wf_app_destroy(&lp->app);
		lp->app_ready = false;
	}
	worker_stopped(lp, "dirpreview", wf_fsworker_stop(lp->dirprev));
	lp->dirprev = NULL;
	worker_stopped(lp, "candidates", wf_candworker_stop(lp->cand));
	lp->cand = NULL;
	worker_stopped(lp, "foldersizes", wf_sizeworker_stop(lp->size));
	lp->size = NULL;
	worker_stopped(lp, "fuzzysearch", wf_fuzzyworker_stop(lp->fuzzy));
	lp->fuzzy = NULL;
	worker_stopped(lp, "filehash", wf_hashworker_stop(lp->hash));
	lp->hash = NULL;
	worker_stopped(lp, "foldersearch", wf_searchworker_stop(lp->search));
	lp->search = NULL;
	/* the syntax worker is the only thread inside piero-syntax.dll: once it is
	 * joined, the DLL and the grammars can be let go */
	if (lp->syntax) {
		WfWorkerStopReport rep = wf_synworker_stop(lp->syntax);
		bool clean = WF_OK == rep.status && !rep.deferred;

		worker_stopped(lp, "syntax", rep);
		lp->syntax = NULL;
		if (clean) {
			wf_synhost_shutdown();
		}
	}
	worker_stopped(lp, "clipboard", wf_clipworker_stop(lp->clip));
	lp->clip = NULL;
	worker_stopped(lp, "locks", wf_lockworker_stop(lp->lock));
	lp->lock = NULL;
	worker_stopped(lp, "launch", wf_launchworker_stop(lp->launch));
	lp->launch = NULL;
	worker_stopped(lp, "git", wf_gitworker_stop(lp->git));
	lp->git = NULL;
	worker_stopped(lp, "transfer", wf_xferworker_stop(lp->xfer));
	lp->xfer = NULL;
	worker_stopped(lp, "listing", wf_fsworker_stop(lp->fs));
	lp->fs = NULL;
	worker_stopped(lp, "preview", wf_previewworker_stop(lp->preview));
	lp->preview = NULL;
	worker_stopped(lp, "image", wf_imageworker_stop(lp->image));
	lp->image = NULL;
	if (lp->preview_disabled) {
		CloseHandle(lp->preview_disabled);
	}
	if (lp->idle) {
		CloseHandle(lp->idle);
	}
	wf_frame_free(lp->prev);
	wf_frame_free(lp->spare);
	wf_buf_free(&lp->out);
}

/* The times after which the two heavy modules are released. The tests cannot
 * wait minutes: PIERO_IDLE_RELEASE_MS (50 to 3600000) replaces both. */
static void set_idle_limits(Loop *lp)
{
	char buf[16], *end = buf;
	const char *v = env_text("PIERO_IDLE_RELEASE_MS", buf, sizeof buf);
	unsigned long n = v ? strtoul(v, &end, 10) : 0;

	lp->idle_image_ms = IMAGE_IDLE_MS;
	lp->idle_archive_ms = ARCHIVE_IDLE_MS;
	if (v && *v && !*end && n >= 50 && n <= 3600000ul) {
		lp->idle_image_ms = lp->idle_archive_ms = (uint32_t)n;
	}
}

static int run(const wchar_t *start,
        WfConfig *config,
        const wchar_t *config_path,
        bool first_run,
        const wchar_t *last_dir,
        WfDiagnostics *diagnostics,
        const wchar_t *launch_directory,
        bool launch_enabled,
        const wchar_t *state_dir,
        bool continue_session)
{
	Loop lp;
	WfStatus st;
	int rc = 1;

	memset(&lp, 0, sizeof lp);
	lp.config = config;
	lp.config_path = config_path;
	lp.first_run = first_run;
	if (config_path &&
	        WF_OK != wf_config_file_open(&lp.cfgfile, config_path)) {
		lp.config_path = NULL; /* settings then cannot be saved */
	}
	lp.diagnostics = diagnostics;
	lp.launch_directory = launch_directory;
	lp.launch_enabled = launch_enabled;
	set_idle_limits(&lp);
	wf_buf_init(&lp.out);
	wf_debounce_init(&lp.debounce);

	if (!start_services(&lp)) {
		goto cleanup;
	}
	st = wf_app_init(&lp.app, 80, 24, wf_fsworker_port(lp.fs), start);
	if (WF_OK != st) {
		fprintf(stderr, "piero: cannot start (%s)\n",
		        wf_status_name(st));
		goto cleanup;
	}
	lp.app_ready = true;
	configure_app(&lp);
	/* after the start folder is asked for: a saved session replaces it */
	wf_main_session_start(&lp, state_dir, continue_session);
	if (!open_console(&lp)) {
		goto cleanup;
	}
	rc = wf_main_run_loop(&lp);
cleanup:
	wf_term_close(lp.term); /* NULL-safe; restores the console first */
	if (WF_CLOSE_FORCED == wf_term_close_reason()) {
		return rc; /* crash-equivalent: the system ends the process now
		            */
	}
	if (lp.app_ready && lp.app.cur.dir && 0 == rc) {
		/* inside an archive the shell is left in the folder that holds it */
		wchar_t *real = wf_path_dup(lp.app.cwd);

		if (real && wf_vpath_is(real)) {
			size_t file = wf_vpath_archive_len(real);

			real[file] = L'\0';
			real[wf_path_parent_len(real)] = L'\0';
		}
		if (!real || !write_last_directory(last_dir, real)) {
			rc = 1;
		}
		free(real);
	}
	wf_main_session_stop(&lp); /* while the screen's state still exists */
	stop_services(&lp);
	wf_config_file_close(&lp.cfgfile);
	{
		wchar_t trace[2];
		if (GetEnvironmentVariableW(L"WF_PREVIEW_TRACE", trace, 2) ==
		                1 &&
		        trace[0] == L'1') {
			fprintf(stderr,
			        "preview-trace text_requests=%llu "
			        "image_requests=%llu directory_requests=%llu "
			        "cleared_snapshots=%llu "
			        "retained_snapshots=%llu frames=%llu "
			        "output_bytes=%llu sixel_bytes=%llu\n",
			        (unsigned long long)
			                lp.app.preview_stats.text_requests,
			        (unsigned long long)
			                lp.app.preview_stats.image_requests,
			        (unsigned long long)
			                lp.app.preview_stats.directory_requests,
			        (unsigned long long)
			                lp.app.preview_stats.cleared_snapshots,
			        (unsigned long long)
			                lp.app.preview_stats.retained_snapshots,
			        (unsigned long long)lp.rendered_frames,
			        (unsigned long long)lp.output_bytes,
			        (unsigned long long)lp.sixel_bytes);
		}
	}
	if (lp.diagnostic_error) {
		fprintf(stderr,
		        "piero: diagnostics stopped safely (Win32 %lu); "
		        "unproved logs preserved\n",
		        lp.diagnostic_error);
	}
	return rc;
}

static bool harden_dll_search(void)
{
	/* System32 only; the application directory is deliberately excluded. */
	return SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32) &&
	        SetDllDirectoryW(L"");
}

static void config_warning(void *ctx,
        WfConfigWarning warning,
        size_t line,
        const char *section,
        const char *key)
{
	(void)ctx;
	fprintf(stderr, "piero: CONFIG %s line %zu [%s] %s\n",
	        wf_config_warning_name(warning), line, section, key);
}

/* Prints what is recorded for the ID: volume, state and the permitted action.
 * False only when the volume name cannot be made printable. */
/* Everything startup produces and shutdown must release. */
typedef struct
{
	WfCli cli;
	WfConfig config;
	WfConfigReport report;
	wchar_t *start, *config_path, *last, *state, *neutral, *logs;
	WfDiagnostics *diagnostics;
	HANDLE state_pin, cwd_pin;
} Startup;

#ifdef WF_PRODUCT_EDIT
#	define WF_HELP_EDIT                                                   \
		"Create, Rename, same-volume Move, no-clobber Copy and Open "  \
		"are enabled in this preview.\n"
#else
#	define WF_HELP_EDIT                                                   \
		"Create, Rename and same-volume Move are disabled in this "    \
		"build; no-clobber Copy and Open remain available.\n"
#endif

static void print_help(void)
{
	puts("usage: piero [--config <path>] [--print-last-dir "
	     "<new-file>] [--language <code>] [--setup] [path]\n"
	     "       piero --version | --help | --reset-terminal\n"
	     "       piero --abandon-recovery <id> | --purge-state\n"
	     "       piero --continue | --forget-session\n"
	     "Keys: j/k h/l Enter g b/f Space / . r i y/x/p z C u Delete "
	     "v P , ? q\n"
	     "      Ctrl+L go to path (Tab completes)  i details  e edit  "
	     ", settings\n"
	     "--setup repeats language, keyboard and theme setup.\n"
	     "--continue puts back the session saved by the last run (where it "
	     "was: folder, cursor, selection, preview); saving is off unless "
	     "[session] persist = true. --forget-session empties it.\n"
	     "Text preview is bounded and sanitized. Image/PDF preview "
	     "requires AppContainer; it has no unisolated "
	     "fallback.\n" WF_HELP_EDIT
	     "Overwrite Copy, cross-volume Move, Recycle and Undo Copy "
	     "are available with confirmation. Overwrite permanently removes "
	     "the old file; WSL deletion is permanent. Recovery records do not "
	     "authorize automatic deletion.\n"
	     "--purge-state is unavailable; state is not swept automatically.\n"
	     "--print-last-dir creates BOM UTF-8; an existing output "
	     "file is preserved.");
}

/* Commands that never open the browser. Returns true with *result set when
 * the command was handled. */
static bool run_utility_command(const WfCli *cli, int *result)
{
	switch (cli->action) {
	case WF_CLI_VERSION:
		puts("piero " WF_VERSION);
		*result = 0;
		return true;
	case WF_CLI_HELP:
		print_help();
		*result = 0;
		return true;
	case WF_CLI_RESET_TERMINAL:
		fputs(wf_term_reset_sequence(), stdout);
		*result = 0;
		return true;
	case WF_CLI_ABANDON_RECOVERY:
	case WF_CLI_PURGE_STATE:
		*result = wf_main_recovery_command(cli);
		return true;
	case WF_CLI_FORGET_SESSION:
		*result = wf_main_forget_session();
		return true;
	case WF_CLI_IMAGE_WORKER:
		fputs("piero: --image-worker is not a command to run; pictures "
		      "and PDF are decoded by a helper process that piero "
		      "starts itself\n",
		        stderr);
		*result = 3;
		return true;
	case WF_CLI_BROWSE: break;
	}
	return false;
}

/* Resolves the configuration path against the original process CWD, reads
 * the file and validates what the OS must confirm (the fallback code page). */
static bool load_configuration(Startup *su)
{
	DWORD error = 0;
	WfStatus status;

	if (su->cli.config_path) {
		status = wf_startup_absolute(
		        su->cli.config_path, &su->config_path, &error);
	} else {
		status = wf_startup_state_path(L"APPDATA", L"config.ini",
		        &su->config_path, NULL, &error);
	}
	if (WF_OK != status && su->cli.config_path) {
		fprintf(stderr,
		        "piero: invalid configuration path (Win32 %lu)\n",
		        error);
		return false;
	}
	if (su->config_path) {
		status = wf_config_read(&su->config, su->config_path,
		        !su->cli.config_path, config_warning, NULL, &su->report,
		        &error);
		if (WF_OK != status) {
			fprintf(stderr,
			        "piero: configuration refused (%s, Win32 "
			        "%lu)\n",
			        wf_status_name(status), error);
			return false;
		}
	}
	if (su->cli.language) {
		if (!language_arg(su->cli.language, &su->config.lang_setting)) {
			fputs("piero: --language expects auto or an installed "
			      "language code\n",
			        stderr);
			return false;
		}
	}
	if (!IsValidCodePage((UINT)su->config.text_fallback_encoding)) {
		config_warning(NULL, WF_CONFIG_INVALID_VALUE,
		        su->report.text_fallback_encoding_line, "preview",
		        "text_fallback_encoding");
		su->config.text_fallback_encoding = 932;
	}
	return true;
}

/* --language: the value is ASCII; anything else is refused. */
static bool language_arg(const wchar_t *text, int *setting)
{
	char ascii[32];
	size_t i;
	WfLangSetting parsed;

	for (i = 0; text[i] && i + 1 < sizeof ascii; i++) {
		if (text[i] >= 0x80) {
			return false;
		}
		ascii[i] = (char)text[i];
	}
	if (text[i]) {
		return false;
	}
	ascii[i] = '\0';
	if (!wf_i18n_parse_setting(ascii, &parsed)) {
		return false;
	}
	*setting = (int)parsed;
	return true;
}

/* The configured start_directory is UTF-8; the rest of the program is
 * UTF-16. NULL when it is not valid UTF-8 or memory is short. */
static wchar_t *utf8_to_wide(const char *utf8)
{
	wchar_t *wide;
	int n = MultiByteToWideChar(
	        CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);

	if (!n || NULL == (wide = wf_malloc_array((size_t)n, sizeof *wide))) {
		return NULL;
	}
	if (!MultiByteToWideChar(
	            CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, n)) {
		free(wide);
		return NULL;
	}
	return wide;
}

/* Command line first, then the configured directory, then the process CWD.
 * Every user path is resolved while the process CWD still has its original
 * value. */
static bool resolve_start_directory(Startup *su)
{
	const WfConfig *config = &su->config;
	DWORD error = 0;
	WfStatus status;

	if (su->cli.print_last_dir &&
	        wf_startup_absolute(
	                su->cli.print_last_dir, &su->last, &error) != WF_OK) {
		fprintf(stderr,
		        "piero: invalid --print-last-dir path (Win32 %lu)\n",
		        error);
		return false;
	}
	if (su->cli.path) {
		status = wf_startup_absolute(su->cli.path, &su->start, &error);
	} else if (config->start_directory &&
	        !strcmp(config->start_directory, "home")) {
		status = wf_startup_env_path(
		        L"USERPROFILE", L"", &su->start, &error);
	} else if (config->start_directory) {
		wchar_t *wide = utf8_to_wide(config->start_directory);

		if (!wide) {
			fputs("piero: configured start_directory cannot be "
			      "used\n",
			        stderr);
			return false;
		}
		status = wf_startup_absolute(wide, &su->start, &error);
		free(wide);
	} else {
		su->start = wf_main_current_directory();
		status = su->start ? WF_OK : WF_E_OS;
	}
	if (WF_OK != status) {
		fprintf(stderr,
		        "piero: cannot resolve start directory (%s, Win32 "
		        "%lu)\n",
		        wf_status_name(status), error);
		return false;
	}
	return true;
}

/* Why the private state folder could not be used, in words (NULL: only the
 * Win32 code is known). Nothing here relaxes the check; it names the cause. */
static const char *private_reason(DWORD error)
{
	switch (error) {
	case WF_PRIVATE_E_NOT_PRIVATE:
		return "it is not private to your account (owner or "
		       "permissions)";
	case ERROR_DIR_NOT_EMPTY: return "its working folder is not empty";
	case ERROR_REPARSE_POINT_ENCOUNTERED: return "it is a link";
	case ERROR_DIRECTORY:                 return "it is not a folder";
	case ERROR_ACCESS_DENIED:             return "access to it is denied";
	default:                              return NULL;
	}
}

/* Pins the private state directory and moves the process CWD into an empty
 * private directory below it. Failure is not fatal: Open stays blocked. */
static void prepare_runtime_directories(Startup *su)
{
	DWORD error = 0;

	if (wf_startup_state_path(
	            L"LOCALAPPDATA", L"", &su->state, NULL, &error) == WF_OK &&
	        wf_private_directory_open(
	                su->state, false, &su->state_pin, &error) &&
	        WF_OK == wf_path_join(su->state, L"cwd", &su->neutral) &&
	        wf_cwd_neutralize(su->neutral, &su->cwd_pin, &error)) {
		/* Process CWD stays here for the session; browser paths are
		 * absolute. */
		return;
	}
	if (private_reason(error)) {
		fprintf(stderr,
		        "piero: the private state folder cannot be used: %s; "
		        "Open and logs stay off\n",
		        private_reason(error));
	} else {
		fprintf(stderr,
		        "piero: the private state folder cannot be used (Win32 "
		        "%lu); Open and logs stay off\n",
		        error);
	}
}

/* Open needs the private neutral directory unless the configuration asks for
 * the file's own folder (then no private directory is involved). */
static bool launch_enabled(const Startup *su)
{
	return WF_LAUNCH_FILE_DIR == su->config.launch_working_directory ||
	        INVALID_HANDLE_VALUE != su->cwd_pin;
}

static const wchar_t *launch_directory(const Startup *su)
{
	return WF_LAUNCH_FILE_DIR == su->config.launch_working_directory
	        ? NULL
	        : su->neutral;
}

/* Diagnostics need the pinned state directory; without it they stay off. */
static void start_diagnostics(Startup *su)
{
	DWORD error = 0;

	if (INVALID_HANDLE_VALUE != su->state_pin &&
	        WF_OK == wf_path_join(su->state, L"logs", &su->logs) &&
	        wf_diag_open(su->logs, (WfDiagLevel)su->config.log_level,
	                su->config.log_paths, &su->diagnostics, &error)) {
		WfDiagEvent event = {WF_DIAG_INFO, WF_DIAG_STARTUP,
		        WF_DIAG_BEGIN, 0, 0, 0, NULL};

		if (!wf_diag_write(su->diagnostics, &event, &error)) {
			fprintf(stderr,
			        "piero: diagnostics unavailable (Win32 %lu); "
			        "unproved logs preserved\n",
			        error);
			wf_diag_close(su->diagnostics);
			su->diagnostics = NULL;
		}
		return;
	}
	if (INVALID_HANDLE_VALUE == su->state_pin) {
		/* the folder was not verified: said above, with its cause */
		fputs("piero: logs are off: the private state folder is not "
		      "available\n",
		        stderr);
		return;
	}
	fprintf(stderr,
	        "piero: private diagnostics unavailable (Win32 %lu); existing "
	        "files preserved\n",
	        error);
}

static void warn_unsupported_settings(const WfConfig *config)
{
	if (config->schema_version > 1) {
		fputs("piero: CONFIG schema_version is newer than this "
		      "program; settings it does not know are ignored\n",
		        stderr);
	}
	if (config->remote_auto_preview || config->cloud_auto_preview) {
		fputs("piero: CONFIG automatic remote/cloud reads remain "
		      "disabled by the safe preview policy\n",
		        stderr);
	}
}

static void shutdown_startup(Startup *su, int result)
{
	DWORD error = 0;

	if (su->diagnostics) {
		WfDiagEvent event = {result ? WF_DIAG_ERROR : WF_DIAG_INFO,
		        WF_DIAG_STARTUP, result ? WF_DIAG_FAILED : WF_DIAG_OK,
		        0, 0, 0, NULL};

		if (!wf_diag_write(su->diagnostics, &event, &error)) {
			fprintf(stderr,
			        "piero: diagnostic final write refused (Win32 "
			        "%lu)\n",
			        error);
		}
		wf_diag_close(su->diagnostics);
	}
	if (INVALID_HANDLE_VALUE != su->cwd_pin) {
		CloseHandle(su->cwd_pin);
	}
	if (INVALID_HANDLE_VALUE != su->state_pin) {
		CloseHandle(su->state_pin);
	}
	free(su->state);
	free(su->neutral);
	free(su->logs);
	free(su->start);
	free(su->config_path);
	free(su->last);
	wf_config_dispose(&su->config);
	wf_cli_dispose(&su->cli);
}

/* A first run: no configuration file where one is expected, on a console that
 * a person is using (or the person asked with --setup). */
/* Whether a console that a person can use is attached: the standard handle if
 * it is a console, else the console device itself (as the terminal layer does
 * when input or output is redirected). */
static bool console_in_use(DWORD std_id, const wchar_t *device)
{
	DWORD mode;
	HANDLE h = GetStdHandle(std_id);
	bool ok;

	if (h && INVALID_HANDLE_VALUE != h && GetConsoleMode(h, &mode)) {
		return true;
	}
	h = CreateFileW(device, GENERIC_READ | GENERIC_WRITE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
	        NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = 0 != GetConsoleMode(h, &mode);
	CloseHandle(h);
	return ok;
}

/* A first run: no configuration file where one is expected, on a console
 * that a person is using (or the person asked with --setup). */
static bool first_run_wanted(const Startup *su)
{
	DWORD error;

	if (su->cli.setup) {
		return true;
	}
	if (!su->config_path) {
		return false;
	}
	if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(su->config_path)) {
		return false;
	}
	error = GetLastError();
	if (ERROR_FILE_NOT_FOUND != error && ERROR_PATH_NOT_FOUND != error) {
		return false;
	}
	return console_in_use(STD_INPUT_HANDLE, L"CONIN$") &&
	        console_in_use(STD_OUTPUT_HANDLE, L"CONOUT$");
}

int wmain(int argc, wchar_t **argv)
{
	Startup su;
	WfCliReport args;
	int result = 1;

	if (!harden_dll_search()) {
		fputs("piero: required DLL search hardening unavailable\n",
		        stderr);
		return 3;
	}
	{
		/* the sandboxed decoder is this same program started with its own
		 * arguments: it must run nothing else (no settings, no terminal) */
		int host_code = 0;

		if (wf_dechost_from_args(argc, argv, &host_code)) {
			return host_code;
		}
	}
	memset(&su, 0, sizeof su);
	su.state_pin = su.cwd_pin = INVALID_HANDLE_VALUE;
	wf_cli_init(&su.cli);
	wf_config_init(&su.config);
	if (WF_OK !=
	        wf_cli_parse(
	                &su.cli, argc, (const wchar_t *const *)argv, &args)) {
		fprintf(stderr,
		        "piero: invalid arguments (argument %zu, code %d); use "
		        "--help\n",
		        args.argument, args.error);
		result = 2;
	} else if (!run_utility_command(&su.cli, &result) &&
	        load_configuration(&su) && resolve_start_directory(&su)) {
		prepare_runtime_directories(&su);
		start_diagnostics(&su);
		/* before any worker measures text */
		wf_set_ambiguous_width(su.config.ambiguous_width);
		warn_unsupported_settings(&su.config);
		result = run(su.start, &su.config, su.config_path,
		        first_run_wanted(&su), su.last, su.diagnostics,
		        launch_directory(&su), launch_enabled(&su),
		        INVALID_HANDLE_VALUE != su.state_pin ? su.state : NULL,
		        0 != su.cli.continue_session);
	}
	shutdown_startup(&su, result);
	return result;
}

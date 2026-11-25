/* The event loop: what each worker has to say, drawing, and waiting. */
#include "main_loop.h"

WfStatus wf_main_request_update(void *context, bool manual)
{
	Loop *lp = context;
	if (lp->updates) {
		return WF_E_ACCESS;
	}
	if (!wf_update_allowed(lp->config->update_checks,
	            lp->config->update_consent, !lp->config->updates_blocked,
	            manual)) {
		return WF_E_ACCESS;
	}
	lp->updates = wf_update_start(WF_VERSION, manual);
	return lp->updates ? WF_OK : WF_E_NOMEM;
}
static bool pump_update(Loop *lp)
{
	WfUpdateResult result;
	bool manual;
	if (!lp->updates || !wf_update_poll(lp->updates, &result, &manual)) {
		return false;
	}
	wf_update_release(lp->updates);
	lp->updates = NULL;
	if (!manual && !lp->config->update_checks) {
		return false;
	}
	wf_app_update_complete(&lp->app, &result);
	return result.state == WF_UPDATE_AVAILABLE ||
	        lp->app.mode == &WF_MODE_SETTINGS;
}

/* The clipboard for Smart Go's Ctrl+V: the editor's system clipboard access,
 * which already knows how to read text safely. */
WfStatus wf_main_clip_get(void *ctx, char **utf8, size_t *n)
{
	WfEditIo io = wf_edit_io_system();

	(void)ctx;
	return io.clip_get ? io.clip_get(io.ctx, utf8, n) : WF_E_INVAL;
}

/* Details show wall-clock time; 0 tells the formatter to fall back to UTC. */
WfFileTime wf_main_local_time(WfFileTime utc)
{
	FILETIME in, out;
	SYSTEMTIME st, local;

	in.dwLowDateTime = (DWORD)(utc & 0xFFFFFFFFu);
	in.dwHighDateTime = (DWORD)(utc >> 32);
	if (!FileTimeToSystemTime(&in, &st) ||
	        !SystemTimeToTzSpecificLocalTime(NULL, &st, &local) ||
	        !SystemTimeToFileTime(&local, &out)) {
		return 0;
	}
	return ((WfFileTime)out.dwHighDateTime << 32) | out.dwLowDateTime;
}

void wf_main_diagnose(Loop *lp,
        WfDiagStage stage,
        WfDiagResult result,
        uint64_t operation_id,
        DWORD error)
{
	WfDiagEvent event = {
	        WF_DIAG_OK == result ? WF_DIAG_INFO : WF_DIAG_WARNING, stage,
	        result, operation_id, error, 0, NULL};
	if (lp->diagnostics && !lp->diagnostic_error &&
	        !wf_diag_write(
	                lp->diagnostics, &event, &lp->diagnostic_error) &&
	        !lp->diagnostic_error) {
		lp->diagnostic_error = ERROR_WRITE_FAULT;
	}
}

static bool sync_graphics(Loop *lp)
{
	const WfTermCaps *caps = wf_term_capabilities(lp->term);
	wchar_t wt[8];
	bool palette_known =
	        caps->colors || GetEnvironmentVariableW(L"WT_SESSION", wt, 8);
	bool enabled = lp->config->graphics != 2 && caps->cell_w &&
	        caps->cell_h &&
	        (lp->config->graphics == 1 ||
	                (palette_known && wf_term_caps_ready(caps)));
	unsigned cw = enabled ? caps->cell_w : 0,
	         ch = enabled ? caps->cell_h : 0;
	unsigned colors = caps->colors ? caps->colors : 256;
	bool changed = lp->app.image_colors != colors ||
	        lp->app.image_cell_w != cw || lp->app.image_cell_h != ch ||
	        lp->app.image_quality != lp->config->image_quality;
	lp->app.image_cell_w = cw;
	lp->app.image_cell_h = ch;
	lp->app.image_quality = lp->config->image_quality;
	lp->app.image_colors = colors;
	return changed;
}

/* Bound each console write and yield to pending keys between chunks. CAN
 * aborts an unfinished DCS; ST restores the parser before any subsequent UI.
 * The coordinator erases any partially painted image on the retry. */
static WfStatus write_graphics(Loop *lp)
{
	size_t offset = 0;
	while (offset < lp->out.len) {
		DWORD pending = 0;
		size_t n = lp->out.len - offset;
		WfStatus st;
		if (GetNumberOfConsoleInputEvents(
		            wf_term_input_handle(lp->term), &pending) &&
		        pending) {
			st = wf_term_write(lp->term, "\x18\x1b\\", 3);
			return WF_OK == st ? WF_E_CANCELLED : st;
		}
		if (n > 16384) {
			n = wf_utf8_prefix(lp->out.data + offset, n, 16384);
		}
		if (!n) {
			return WF_E_INVAL;
		}
		// VT parsing spans writes; cancellation above terminates an open DCS.
		st = wf_term_write(lp->term, lp->out.data + offset, n);
		if (WF_OK != st) {
			return st;
		}
		offset += n;
	}
	return WF_OK;
}

/* Redraws the whole app via a diff against the previous frame. On failure
 * the previous frame is dropped so the next attempt does a full redraw. */
static WfStatus render(Loop *lp)
{
	WfFrame *next = lp->spare;
	WfStatus st;

	/* Two frames take turns (what is shown, and what is being made), so a redraw
	 * allocates nothing while the size stays. wf_app_draw begins by filling the
	 * whole frame, which is the one place the cells are made blank. */
	lp->spare = NULL;
	if (!wf_frame_reuse(
	            &next, lp->app.cols, lp->app.rows, &WF_STYLE_BASE)) {
		lp->spare = next;
		return WF_E_NOMEM;
	}
	wf_app_sync_counts(&lp->app);
	wf_app_draw(&lp->app, next);
	if (lp->app.mode != &WF_MODE_NORMAL &&
	        lp->app.mode != &WF_MODE_PREVIEW &&
	        lp->app.mode != &WF_MODE_PREVIEW_PANE) {
		memset(&next->graphic, 0, sizeof next->graphic);
	}

	/* Synchronized output only where the terminal answered the query, and
	 * never around graphics: a Sixel frame is written in cancellable chunks
	 * and must not be left inside an open synchronized update. A frame that
	 * changed nothing stays empty (no bytes when idle). */
	bool sync = wf_term_capabilities(lp->term)->sync_output &&
	        !next->graphic.len && !(lp->prev && lp->prev->graphic.len);
	size_t mark = 0;

	wf_buf_clear(&lp->out);
	st = !sync || wf_sync_begin(&lp->out, &mark) ? WF_OK : WF_E_NOMEM;
	if (WF_OK == st) {
		st = wf_graphics_present(lp->prev, next, &lp->out);
	}
	if (WF_OK == st && sync && !wf_sync_end(&lp->out, mark)) {
		st = WF_E_NOMEM;
	}
	if (WF_OK == st) {
		lp->rendered_frames++;
		lp->output_bytes += lp->out.len;
		if (next->graphic.len && lp->out.data &&
		        strstr(lp->out.data, "\x1bP")) {
			lp->sixel_bytes += next->graphic.len;
		}
		st = next->graphic.len
		        ? write_graphics(lp)
		        : wf_term_write(
		                  lp->term, wf_buf_cstr(&lp->out), lp->out.len);
		if (WF_E_CANCELLED == st) {
			lp->graphics_retry = true;
			next->graphic.gen ^= 0x80000000u;
			for (size_t i = 0;
			        i < (size_t)next->cols * (size_t)next->rows;
			        i++) {
				next->cells[i].flags ^= 64;
			}
			st = WF_OK;
		}
	}

	if (WF_OK == st) {
		lp->spare = lp->prev;
		lp->prev = next;
	} else {
		/* the terminal is in a state we do not know: no diff against a frame
		 * that may not be on screen; the next one repaints everything */
		wf_frame_free(lp->prev);
		lp->prev = NULL;
		lp->spare = next;
	}
	return st;
}

/* malloc'd current directory (any length), or NULL. */
wchar_t *wf_main_current_directory(void)
{
	DWORD need = GetCurrentDirectoryW(0, NULL), got;
	wchar_t *p;

	if (0 == need) {
		return NULL;
	}
	p = wf_malloc_array(need, sizeof *p);
	if (!p) {
		return NULL;
	}
	got = GetCurrentDirectoryW(need, p);
	if (0 == got || got >= need) { /* changed under us, or failed */
		free(p);
		return NULL;
	}
	return p;
}

/* Follows the app's directory: reopens the watch when cwd changed. A
 * directory that cannot be watched (some network shares) just goes without. */
static void retarget_watch(Loop *lp)
{
	/* Initial UNC navigation may be waiting for connection consent. */
	if (!lp->app.cur.dir) {
		return;
	}
	if (lp->watch_version == lp->app.cwd_version) {
		return;
	}
	wf_watch_close(lp->watch);
	lp->watch = NULL;
	wf_debounce_init(&lp->debounce);
	lp->watch_listing_dirty = lp->watch_preview_dirty = false;
	if (WF_OK != wf_watch_open(lp->app.cwd, &lp->watch)) {
		lp->watch = NULL;
	}
	wf_app_dircache_enable(&lp->app, lp->watch != NULL);
	lp->watch_version = lp->app.cwd_version;
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

/* Delivers queued listings. True if the screen changed. */
static bool pump_listings(Loop *lp)
{
	WfFsResult r;
	bool changed = false;

	while (wf_fsworker_poll(lp->fs, &r)) {
		if (r.gen != lp->app.nav_gen || !lp->app.want_path) {
			/* The app consumes stale ownership without sorting, logging or
			 * invalidating the displayed frame. */
			wf_app_on_listing(
			        &lp->app, r.gen, r.st, r.dir, r.parent);
			continue;
		}
		if (WF_OK == r.st) {
			WfStatus sorted = r.dir
			        ? wf_dir_sort_with(r.dir,
			                  (WfSortKey)lp->config->sort_by,
			                  lp->config->sort_descending,
			                  0 != lp->config->directories_first)
			        : WF_OK;
			if (WF_OK == sorted && r.parent) {
				sorted = wf_dir_sort_with(r.parent,
				        (WfSortKey)lp->config->sort_by,
				        lp->config->sort_descending,
				        0 != lp->config->directories_first);
			}
			if (WF_OK != sorted) {
				r.st = sorted;
			}
		}
		wf_app_on_listing(&lp->app, r.gen, r.st, r.dir, r.parent);
		wf_main_diagnose(lp, WF_DIAG_LIST,
		        WF_OK == r.st ? WF_DIAG_OK : WF_DIAG_FAILED, r.gen, 0);
		changed = true;
	}
	return changed;
}

/* Delivers capture/preflight answers. True if the screen changed. */
static bool pump_xfer(Loop *lp)
{
	WfXferResult r;
	bool changed = false;

	while (wf_xferworker_poll(lp->xfer, &r)) {
		bool current = r.gen == lp->app.xfer_gen;
		bool accepted = WF_OK == r.st &&
		        (WF_XR_CAPTURE == r.kind ||
		                (WF_PF_OK == r.report.reason &&
		                        r.report.checks_complete));
		WfDiagStage stage =
		        (WF_XR_CAPTURE == r.kind
		                        ? r.slot.mode
		                        : lp->app.slot.mode) == WF_XFER_MOVE
		        ? WF_DIAG_MOVE
		        : WF_DIAG_COPY;
		if (WF_XR_CAPTURE == r.kind) {
			wf_app_on_capture(&lp->app, r.gen, r.st, &r.slot);
		} else {
			wf_app_on_preflight(&lp->app, r.gen, r.st, &r.report);
		}
		/* A successful preflight still does not authorize execution. */
		if (current) {
			wf_main_diagnose(lp, stage,
			        accepted ? WF_DIAG_OK : WF_DIAG_REFUSED, r.gen,
			        0);
		}
		wf_slot_free(
		        &r.slot); /* no-op once the app took the contents */
		changed = true;
	}
	return changed;
}

/* Delivers finished copy / move / recycle jobs. */
static bool pump_copy(Loop *lp)
{
	WfCopyResult r;
	bool changed = false;

	while (lp->copy && wf_copyworker_poll(lp->copy, &r)) {
		wf_app_on_copy_result(&lp->app, &r);
		wf_main_diagnose(lp,
		        WF_CJ_MOVE_ACROSS == r.kind ? WF_DIAG_MOVE
		                                    : WF_DIAG_COPY,
		        WF_CS_DONE == r.state && !r.failure_total
		                ? WF_DIAG_OK
		                : WF_DIAG_FAILED,
		        r.job, 0);
		wf_copyresult_free(&r);
		changed = true;
	}
	return changed;
}

#ifdef WF_PRODUCT_EDIT
/* Delivers create/rename receipts. Every accepted request answers once. */
static bool pump_edit(Loop *lp)
{
	WfEditResult r;
	bool changed = false;

	while (lp->edit && wf_editworker_poll(lp->edit, &r)) {
		wf_app_on_edit(&lp->app, r.gen, &r.outcome, r.new_name);
		changed = true;
	}
	return changed;
}

/* Delivers the result of a batch rename or of a recovery of old journals. */
static bool pump_batch(Loop *lp)
{
	WfBxDone d;
	bool changed = false;

	while (lp->batch && wf_batchworker_poll(lp->batch, &d)) {
		wf_app_on_batch(&lp->app, &d);
		changed = true;
	}
	return changed;
}
#endif

WfStatus wf_main_preview_list_submit(
        void *ctx, uint32_t gen, const wchar_t *path, const wchar_t *parent)
{
	Loop *lp = ctx;
	wf_fsworker_set_sort(lp->dirprev, (WfSortKey)lp->config->sort_by,
	        lp->config->sort_descending);
	wf_fsworker_set_dirs_first(
	        lp->dirprev, 0 != lp->config->directories_first);
	WfFsPort port = wf_fsworker_port(lp->dirprev);
	return port.submit(port.ctx, gen, path, parent);
}

/* Delivers the listings of the directory under the cursor (preview pane). */
static bool pump_dirprev(Loop *lp)
{
	WfFsResult r;
	bool changed = false;

	while (lp->dirprev && wf_fsworker_poll(lp->dirprev, &r)) {
		WfDir *before = lp->app.dirprev.dir;
		WfDirPreviewState state = lp->app.dirprev_state;
		wf_dir_free(r.parent);
		wf_app_on_dirpreview(&lp->app, r.gen, r.st, r.dir);
		changed |= before != lp->app.dirprev.dir ||
		        state != lp->app.dirprev_state;
	}
	return changed;
}

/* Delivers Smart Go's candidates. */
static bool pump_cand(Loop *lp)
{
	WfCandBatch b;
	bool changed = false;

	while (lp->cand && wf_candworker_poll(lp->cand, &b)) {
		wf_app_on_candidates(&lp->app, &b);
		changed = true;
	}
	return changed;
}

/* Delivers the answer of a read of the clipboard's files. */
static bool pump_clip(Loop *lp)
{
	WfClipFiles files;
	bool changed = false;

	while (lp->clip && wf_clipworker_poll(lp->clip, &files)) {
		wf_app_on_clip_files(&lp->app, &files);
		changed = true;
	}
	return changed;
}

/* Delivers the answer of a lock query or of a close or end request. */
static bool pump_lock(Loop *lp)
{
	WfLockAnswer a;
	bool changed = false;

	while (lp->lock && wf_lockworker_poll(lp->lock, &a)) {
		wf_app_on_lock(&lp->app, &a);
		changed = true;
	}
	return changed;
}

/* Delivers the folder totals D is counting. */
static bool pump_size(Loop *lp)
{
	WfSizeUpdate u;
	bool changed = false;

	while (lp->size && wf_sizeworker_poll(lp->size, &u)) {
		wf_app_on_sizes(&lp->app, &u);
		changed = true;
	}
	return changed;
}

/* Delivers the hits of the search below a folder. */
static bool pump_search(Loop *lp)
{
	WfSearchBatch b;
	bool changed = false;

	while (lp->search && wf_searchworker_poll(lp->search, &b)) {
		wf_app_on_search(&lp->app, &b);
		changed = true;
	}
	return changed;
}

/* Delivers the colours of the preview pane and of the editor. */
static bool pump_syntax(Loop *lp)
{
	WfSynAnswer a;
	bool changed = false;

	while (lp->syntax && wf_synworker_poll(lp->syntax, &a)) {
		if (!wf_app_on_syntax(&lp->app, &a)) {
			/* the editor's: its session takes it */
			if (lp->editor) {
				wf_es_on_syntax(lp->editor, &a);
			}
			wf_synanswer_free(&a);
		}
		changed = true;
	}
	return changed;
}

/* Delivers the progress and the end of a file hash (#). */
static bool pump_hash(Loop *lp)
{
	WfHashUpdate u;
	bool changed = false;

	while (lp->hash && wf_hashworker_poll(lp->hash, &u)) {
		wf_app_on_hash(&lp->app, &u);
		changed = true;
	}
	return changed;
}

/* Delivers the answer of a fuzzy search. */
static bool pump_fuzzy(Loop *lp)
{
	WfFzAnswer a;
	bool changed = false;

	while (lp->fuzzy && wf_fuzzyworker_poll(lp->fuzzy, &a)) {
		wf_app_on_fuzzy(&lp->app, &a);
		changed = true;
	}
	return changed;
}

/* Delivers the answers of launch requests. */
static bool pump_launch(Loop *lp)
{
	WfLaunchResult r;
	bool changed = false;

	while (lp->launch && wf_launchworker_poll(lp->launch, &r)) {
		wf_app_on_launch(&lp->app, r.gen, &r.outcome, r.name);
		changed = true;
	}
	return changed;
}

typedef enum
{
	DISPATCH_FATAL = -1,
	DISPATCH_CONTINUE = 0,
	DISPATCH_QUIT = 1
} DispatchResult;

/* Dispatches a batch of input; asks the loop to quit or reports a fatal error.
 */
static DispatchResult dispatch(
        Loop *lp, const WfEvent *ev, size_t n, bool *dirty)
{
	size_t i;

	for (i = 0; i < n; i++) {
		WfAction act;

		if (WF_EV_RESIZE == ev[i].type) {
			if (wf_term_size(lp->term, &lp->app.cols,
			            &lp->app.rows) != WF_OK) {
				return DISPATCH_FATAL;
			}
			*dirty = true;
			continue;
		}
		/* After a modal opened, whatever else was already typed is
		 * type-ahead: drop it, but still deliver key-ups. */
		if (lp->app.flush_input && WF_EV_RELEASE != ev[i].type) {
			continue;
		}

		if (wf_app_window_wheel(&lp->app, &ev[i], &act)) {
			if (WF_ACT_REDRAW == act) {
				*dirty = true;
			}
			continue;
		}
		if (!wf_app_paste_filter(&lp->app, &ev[i])) {
			continue;
		}
		act = lp->app.mode->on_event(&lp->app, &ev[i]);
		if (WF_ACT_QUIT == act || lp->app.quit_now) {
			return DISPATCH_QUIT;
		}
		if (WF_ACT_REDRAW == act) {
			*dirty = true;
		}
	}
	return DISPATCH_CONTINUE;
}

/* Delivers finished previews. True if the screen changed. */
static bool pump_preview(Loop *lp)
{
	WfPreviewResult result;
	bool changed = false;

	while (lp->preview && wf_previewworker_poll(lp->preview, &result)) {
		bool accepted = result.gen == lp->app.preview_gen &&
		        lp->app.preview_busy;
		if (accepted && result.preview &&
		        result.preview->system_error) {
			wf_main_diagnose(lp, WF_DIAG_PREVIEW, WF_DIAG_FAILED,
			        result.gen, result.preview->system_error);
		}
		wf_app_on_preview(&lp->app, result.gen, result.preview);
		changed |= accepted;
	}
	return changed;
}

/* Delivers finished pictures. True if the screen changed. */
static bool pump_image(Loop *lp)
{
	WfImageResult r;
	bool changed = false;

	while (lp->image && wf_imageworker_poll(lp->image, &r)) {
		WfImageAnswer answer;
		bool accepted = r.gen == lp->app.image.gen &&
		        lp->app.image.state == WF_IMG_BUSY;

		memset(&answer, 0, sizeof answer);
		answer.gen = r.gen;
		answer.status = r.status;
		answer.info = r.info;
		answer.pm = r.pm;
		answer.sixel = r.sixel;
		answer.sixel_w = r.sixel_w;
		answer.sixel_h = r.sixel_h;
		answer.sensitive = r.sensitive;
		answer.page = r.page;
		answer.refused = r.refused;
		answer.unavailable = r.unavailable;
		answer.os_error = r.os_error;
		wf_app_on_image(&lp->app, &answer);
		changed |= accepted;
	}
	return changed;
}

/* Delivers what Git answered: the status to the file list, the base and the diff
 * to the editor. True if the screen changed. */
static bool pump_git(Loop *lp)
{
	WfGitAnswer a;
	bool changed = false;

	while (lp->git && wf_gitworker_poll(lp->git, &a)) {
		if (WF_GIT_A_REPO == a.kind) {
			changed |= wf_app_on_git(&lp->app, &a);
		} else if (lp->editor) {
			changed |= wf_es_on_git(lp->editor, &a);
		}
		wf_gitanswer_free(&a);
	}
	return changed;
}

/* Hands every finished worker answer to the app. True if the screen changed. */
static bool pump_results(Loop *lp)
{
	bool changed = pump_listings(lp);

	changed |= pump_xfer(lp);
	changed |= pump_copy(lp);
#ifdef WF_PRODUCT_EDIT
	changed |= pump_edit(lp);
	changed |= pump_batch(lp);
#endif
	changed |= pump_launch(lp);
	changed |= pump_dirprev(lp);
	changed |= pump_cand(lp);
	changed |= pump_size(lp);
	changed |= pump_clip(lp);
	changed |= pump_lock(lp);
	changed |= pump_fuzzy(lp);
	changed |= pump_hash(lp);
	changed |= pump_search(lp);
	changed |= pump_syntax(lp);
	changed |= pump_preview(lp);
	changed |= pump_image(lp);
	changed |= pump_git(lp);
	changed |= pump_update(lp);
	return changed;
}

/* The workers of # D L F, of V, of Open and Open Terminal Here and of pictures
 * are started by the first request (DR-DRAFT-035 to 037), and the picture
 * worker is released when idle, so their events are taken again
 * at the top of every turn: a worker that was made
 * during the last one is waited on from this one, and its answer wakes the loop. */
static void fill_lazy_wait_handles(const Loop *lp, HANDLE waits[W_COUNT])
{
	waits[W_SIZE] = lp->size ? wf_sizeworker_event(lp->size) : lp->idle;
	waits[W_LOCK] = lp->lock ? wf_lockworker_event(lp->lock) : lp->idle;
	waits[W_HASH] = lp->hash ? wf_hashworker_event(lp->hash) : lp->idle;
	waits[W_SEARCH] =
	        lp->search ? wf_searchworker_event(lp->search) : lp->idle;
	waits[W_CLIP] = lp->clip ? wf_clipworker_event(lp->clip) : lp->idle;
	waits[W_LAUNCH] =
	        lp->launch ? wf_launchworker_event(lp->launch) : lp->idle;
	waits[W_IMAGE] = lp->image ? wf_imageworker_event(lp->image) : lp->idle;
	waits[W_GIT] = lp->git ? wf_gitworker_event(lp->git) : lp->idle;
}

/* The handles the loop waits on, in the order of the W_* indexes. The watch
 * handle is appended by the loop because it follows the current folder. */
static void fill_wait_handles(Loop *lp, HANDLE waits[W_COUNT])
{
	waits[W_INPUT] = wf_term_input_handle(lp->term);
	waits[W_SHUTDOWN] = wf_term_shutdown_event(lp->term);
	waits[W_FS] = wf_fsworker_event(lp->fs);
	waits[W_XFER] = wf_xferworker_event(lp->xfer);
	/* No worker is created when previews are disabled by configuration. */
	waits[W_PREVIEW] = lp->preview ? wf_previewworker_event(lp->preview)
	                               : lp->preview_disabled;
#ifdef WF_PRODUCT_EDIT
	waits[W_EDIT] = wf_editworker_event(lp->edit);
#else
	waits[W_EDIT] = lp->idle;
#endif
#ifdef WF_PRODUCT_EDIT
	waits[W_BATCH] = lp->batch ? wf_batchworker_event(lp->batch) : lp->idle;
#else
	waits[W_BATCH] = lp->idle;
#endif
	waits[W_DIRPREV] =
	        lp->dirprev ? wf_fsworker_event(lp->dirprev) : lp->idle;
	waits[W_CAND] = lp->cand ? wf_candworker_event(lp->cand) : lp->idle;
	waits[W_COPY] = lp->copy ? wf_copyworker_event(lp->copy) : lp->idle;
	waits[W_UPDATE] = lp->updates ? wf_update_event(lp->updates) : lp->idle;
	waits[W_FUZZY] = lp->fuzzy ? wf_fuzzyworker_event(lp->fuzzy) : lp->idle;
	fill_lazy_wait_handles(lp, waits);
	waits[W_SYNTAX] =
	        lp->syntax ? wf_synworker_event(lp->syntax) : lp->idle;
}

int wf_main_run_loop(Loop *lp)
{
	HANDLE waits[W_COUNT];

	fill_wait_handles(lp, waits);
	lp->app.now_ms = GetTickCount64();
	if (WF_OK != render(lp)) {
		return 1;
	}
	wf_app_on_presented(&lp->app);

	for (;;) {
		DWORD count = W_WATCH, w;
		uint32_t timeout;
		WfEvent ev[EVENTS_PER_TICK];
		bool dirty = lp->graphics_retry;
		if (lp->app.mode == &WF_MODE_NORMAL &&
		        !lp->app.update_prompted &&
		        !lp->config->update_consent &&
		        !lp->config->updates_blocked) {
			wf_app_update_consent_begin(&lp->app);
			dirty = true;
		}
		if (!lp->update_auto_attempted && lp->config->update_consent &&
		        lp->config->update_checks &&
		        !lp->config->updates_blocked) {
			lp->update_auto_attempted = true;
			(void)wf_main_request_update(lp, false);
		}
		waits[W_UPDATE] =
		        lp->updates ? wf_update_event(lp->updates) : lp->idle;
		fill_lazy_wait_handles(lp, waits);
		lp->graphics_retry = false;
		DispatchResult quit;
		size_t n;

		if (lp->watch) {
			waits[count++] = wf_watch_event(lp->watch);
		}

		timeout = min_u32(wf_app_timeout(&lp->app),
		        wf_debounce_timeout(&lp->debounce, lp->app.now_ms));
		timeout = min_u32(
		        timeout, wf_main_session_timeout(lp, GetTickCount64()));
		timeout = min_u32(
		        timeout, wf_main_idle_timeout(lp, GetTickCount64()));
		if (dirty) {
			timeout = min_u32(timeout, 16);
		}
		if (&WF_MODE_EDITOR == lp->app.mode) {
			timeout = min_u32(timeout,
			        wf_es_timeout(lp->editor, lp->app.now_ms));
		}
		w = WaitForMultipleObjects(count, waits, FALSE,
		        WF_WAIT_FOREVER == timeout ? INFINITE : timeout);
		if (WAIT_FAILED == w) {
			DWORD error = GetLastError();
			wf_main_diagnose(
			        lp, WF_DIAG_STARTUP, WF_DIAG_FAILED, 0, error);
			fprintf(stderr,
			        "piero: event wait failed (Win32 %lu)\n",
			        error);
			return 1;
		}
		lp->app.now_ms = GetTickCount64();
		wf_main_note_modules_in_use(lp);

		if (w == WAIT_OBJECT_0 + W_SHUTDOWN) {
			return 0;
		}
		if (w == WAIT_OBJECT_0 + W_WATCH && lp->watch) {
			WfWatchChanges change;
			const WfEntry *entry = wf_view_current(&lp->app.cur);
			if (wf_watch_consume_selected(lp->watch,
			            entry ? entry->name : NULL, &change)) {
				wf_app_dircache_forget_neighbours(&lp->app);
				if (change.preview &&
				        &WF_MODE_EDITOR == lp->app.mode &&
				        wf_main_editor_disk_check(lp)) {
					dirty = true;
				}
				lp->watch_listing_dirty |= change.listing;
				lp->watch_preview_dirty |= change.preview;
				if (change.listing || change.preview) {
					wf_debounce_note(
					        &lp->debounce, lp->app.now_ms);
				}
			}
		}

		if (lp->watch && !wf_watch_active(lp->watch)) {
			wf_app_dircache_enable(&lp->app, false);
		}

		/* Input is always drained: the handle can be signalled with
		 * nothing readable, and a timeout wake costs one empty read. */
		n = wf_term_read_events(lp->term, ev, EVENTS_PER_TICK);
		dirty |= sync_graphics(lp);
		quit = dispatch(lp, ev, n, &dirty);
		if (DISPATCH_FATAL == quit) {
			return 1;
		}
		if (DISPATCH_QUIT == quit) {
			return 0;
		}

		if (pump_results(lp)) {
			dirty = true;
		}
		wf_main_release_idle_workers(lp);
		wf_main_sync_editing_keys(lp);
		wf_main_session_tick(lp);
		if (WF_ACT_REDRAW == wf_app_tick(&lp->app)) {
			dirty = true;
		}
		if (&WF_MODE_EDITOR == lp->app.mode &&
		        wf_es_tick(lp->editor, lp->app.now_ms)) {
			dirty = true;
		}
		if (lp->editor) {
			wf_es_save_drafts(lp->editor, lp->app.now_ms);
		}
		if (wf_debounce_take(&lp->debounce, lp->app.now_ms)) {
			if (lp->watch_listing_dirty) {
				wf_app_refresh(
				        &lp->app, lp->watch_preview_dirty);
			} else if (lp->watch_preview_dirty) {
				wf_app_preview_sync(&lp->app, true);
			}
			lp->watch_listing_dirty = lp->watch_preview_dirty =
			        false;
			dirty = true;
		}
		retarget_watch(lp);
		/* a refresh above may have changed the list under a search */
		if (wf_app_fuzzy_tick(&lp->app)) {
			dirty = true;
		}

		if (lp->app.flush_input) {
			wf_term_drain_input(lp->term);
			lp->app.flush_input = false;
		}
		if (dirty) {
			wf_main_session_note(lp);
			if (WF_OK != render(lp)) {
				return 1;
			}
			lp->app.now_ms = GetTickCount64();
			wf_app_on_presented(&lp->app);
		}
	}
}

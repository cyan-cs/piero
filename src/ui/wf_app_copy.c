/*
 * Copy, cross-volume move and recycle, as the screen sees them. (Undo is
 * wf_app_undo.c; the log of the last jobs is wf_app_joblog.c.)
 *
 * The work itself is done by the copy worker (another thread); this file
 * builds the job from what the user confirmed, shows how far it has got, and
 * turns the result into a plain statement of what was done and what was not.
 * Nothing here touches the file system.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"

#define COPY_REFRESH_MS 200u

void wf_app_set_copy(WfApp *app, WfCopyPort port, WfCopyVerify verify)
{
	app->copy = port;
	app->copy_verify = verify;
}

void wf_app_set_copy_off(WfApp *app, WfMsg why)
{
	app->copy_off_set = true;
	app->copy_off = why;
}

void wf_app_copy_destroy(WfApp *app)
{
	size_t i;

	for (i = 0; i < app->undo_n; i++) {
		wf_app_undo_free(&app->undo[i]);
	}
	app->undo_n = 0;
	wf_app_undo_free(&app->undo_pending);
	app->undo_pending_set = false;
	wf_slot_free(&app->recycle_slot);
	wf_app_joblog_destroy(app);
	wf_app_backups_destroy(app);
}

static bool spec_for(WfApp *app,
        WfCopySpec *spec,
        WfCopyKind kind,
        const WfTransferSlot *slot,
        const wchar_t *dest,
        const WfFileId *dest_id)
{
	memset(spec, 0, sizeof *spec);
	wf_slot_init(&spec->slot);
	if (WF_OK != wf_slot_clone(&spec->slot, slot)) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_NOMEM));
		return false;
	}
	spec->kind = kind;
	if (dest) {
		spec->dest = wf_path_dup(dest);
		if (!spec->dest) {
			wf_copyspec_free(spec);
			wf_app_set_msg(app, wf_msg(MSG_JOB_NOMEM));
			return false;
		}
		spec->dest_id = *dest_id;
	}
	spec->verify = app->copy_verify;
	return true;
}

static bool submit(WfApp *app, WfCopySpec *spec)
{
	uint32_t job = 0;
	WfStatus st = app->copy.submit(app->copy.ctx, spec, &job);

	if (WF_OK == st) {
		wf_app_retry_note(app, job, spec);
		wf_app_lock_note_job(app, job, spec);
	}
	wf_copyspec_free(spec);
	if (WF_E_RANGE == st) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_QUEUE_FULL));
		return false;
	}
	if (WF_OK != st) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_START_FAILED));
		return false;
	}
	app->copy_jobs++;
	app->last_job = job;
	app->copy_refresh_at = 0;
	return true;
}

bool wf_app_copy_start(WfApp *app, bool hydrate)
{
	WfCopySpec spec;
	const WfPreflightReport *rep = &app->last_report;
	bool cross = app->pending_cross;
	WfCollide collide = app->pending_collide;

	if (!app->copy.submit) {
		if (app->copy_off_set) {
			wf_app_set_msg(app, wf_msg(app->copy_off));
			return true;
		}
		if (app->recovery.unknown) {
			wf_app_set_msg(app, wf_msg(MSG_COPY_SAFETY_BLOCKED));
			return true;
		}
		return false;
	}
	app->pending_cross = false;
	app->pending_collide = WF_COLLIDE_STOP;
	app->slot.collisions_ok = false;
	/* a FAT destination has only a weak identity: a copy may go there (new names
	 * only), a move may not */
	if (!app->cur.dir ||
	        !((app->cur.dir->identity.kind == WF_ID_STRONG) ||
	                (!cross &&
	                        wf_fileid_is_weak(&app->cur.dir->identity)))) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_COPY_DEST_UNPROVEN));
		return true;
	}
	if (wf_fileid_is_weak(&app->cur.dir->identity)) {
		/* a folder is built under a hidden name and renamed, which a FAT
		 * identity does not survive; and nothing is replaced there */
		for (size_t i = 0; i < app->slot.count; i++) {
			if (app->slot.src[i].snap.flags & WF_ENT_DIR) {
				wf_app_set_msg(
				        app, wf_msg(MSG_JOB_COPY_FAT_FOLDER));
				return true;
			}
		}
		if (WF_COLLIDE_OVERWRITE == collide) {
			wf_app_set_msg(app, wf_msg(MSG_JOB_COPY_FAT_OVERWRITE));
			return true;
		}
	}
	if (!spec_for(app, &spec, cross ? WF_CJ_MOVE_ACROSS : WF_CJ_COPY,
	            &app->slot, app->cwd, &app->cur.dir->identity)) {
		return true;
	}
	spec.collide = collide;
	spec.merge_folders = !cross; /* a copy enters a folder that is there */
	spec.hydrate = hydrate;
	spec.total_bytes = rep->total_bytes;
	spec.total_files = rep->tree_files;
	if (submit(app, &spec)) {
		app->msg[0] = '\0';
		if (cross) {
			/* still a move: a second p asks again, and the slot goes
			 * once the originals are in the Bin */
			app->cross_first = app->slot.src[0].snap.id;
			app->slot.mode = WF_XFER_MOVE;
		}
	} else if (cross) {
		app->slot.mode = WF_XFER_MOVE;
	}
	return true;
}

/* The items picked in a ZIP become an EXTRACT job: the archive, the paths in it
 * and the folder on screen. The job reads, checks and writes; nothing is read
 * here. Returns true when it was handed to the worker. */
bool wf_app_extract_start(WfApp *app, WfCollide collide)
{
	WfCopySpec spec;

	if (!app->arc_pick.n) {
		return false;
	}
	if (!app->copy.submit) {
		wf_app_set_msg(app,
		        wf_msg(app->copy_off_set ? app->copy_off
		                        : app->recovery.unknown
		                        ? MSG_COPY_SAFETY_BLOCKED
		                        : MSG_XFER_NOT_AVAILABLE));
		return true;
	}
	if (!app->cur.dir || !(app->cur.dir->identity.kind == WF_ID_STRONG)) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_COPY_DEST_UNPROVEN));
		return true;
	}
	memset(&spec, 0, sizeof spec);
	wf_slot_init(&spec.slot);
	spec.kind = WF_CJ_EXTRACT;
	spec.archive = wf_path_dup(app->arc_pick.archive);
	spec.dest = wf_path_dup(app->cwd);
	spec.dest_id = app->cur.dir->identity;
	spec.inner = calloc(app->arc_pick.n, sizeof *spec.inner);
	if (!spec.archive || !spec.dest || !spec.inner) {
		wf_copyspec_free(&spec);
		wf_app_set_msg(app, wf_msg(MSG_JOB_NOMEM));
		return true;
	}
	for (size_t i = 0; i < app->arc_pick.n; i++) {
		spec.inner[i] = wf_path_dup(app->arc_pick.inner[i]);
		if (!spec.inner[i]) {
			wf_copyspec_free(&spec);
			wf_app_set_msg(app, wf_msg(MSG_JOB_NOMEM));
			return true;
		}
		spec.n_inner = i + 1;
	}
	spec.verify = app->copy_verify;
	spec.collide = collide;
	spec.merge_folders = true;
	if (submit(app, &spec)) {
		app->msg[0] = '\0';
	}
	return true;
}

void wf_app_recycle_confirmed(WfApp *app, void *ctx)
{
	WfCopySpec spec;
	size_t entry;

	(void)ctx;
	if (!app->recycle_slot.count) {
		return;
	}
	/* The entry stays until the job has finished, and the job and the entry
	 * are tied by id. Room for that tie is taken before the job is sent:
	 * with none, nothing is sent. */
	entry = wf_app_undo_entry_of_slot(app, &app->recycle_slot);
	if (SIZE_MAX != entry &&
	        app->recycling_n >=
	                sizeof app->recycling / sizeof app->recycling[0]) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_QUEUE_FULL));
		wf_slot_free(&app->recycle_slot);
		return;
	}
	if (!spec_for(app, &spec, WF_CJ_RECYCLE, &app->recycle_slot, NULL,
	            NULL)) {
		wf_slot_free(&app->recycle_slot);
		return;
	}
	spec.total_files = app->recycle_slot.count;
	if (submit(app, &spec) && SIZE_MAX != entry) {
		app->recycling[app->recycling_n].job = app->last_job;
		app->recycling[app->recycling_n++].uid = app->undo[entry].uid;
	}
	wf_slot_free(&app->recycle_slot);
}

bool wf_app_recycle_send(WfApp *app, WfTransferSlot *s)
{
	size_t before = app->copy_jobs;

	wf_slot_free(&app->recycle_slot);
	app->recycle_slot = *s;
	wf_slot_init(s);
	wf_app_recycle_confirmed(app, NULL);
	return app->copy_jobs > before; /* a job went out */
}

/* Builds recycle_slot from the visible selection (or the cursor entry). */
static bool recycle_capture(WfApp *app)
{
	const WfNameSet *sel = wf_sel_folder(&app->sel, app->cwd);
	size_t i, n = 0, cap;
	WfTransferSlot *s = &app->recycle_slot;
	const WfEntry *cur = wf_view_current(&app->cur);

	wf_slot_free(s);
	wf_slot_init(s);
	cap = sel && sel->n ? app->cur.nvis : 1;
	if (!cap) {
		return false;
	}
	s->src = calloc(cap, sizeof *s->src);
	s->dir = wf_path_dup(app->cwd);
	if (!s->src || !s->dir) {
		wf_slot_free(s);
		wf_app_set_msg(app, wf_msg(MSG_JOB_NOMEM));
		return false;
	}
	s->dir_id = app->cur.dir->identity;
	for (i = 0; i < app->cur.nvis; i++) {
		const WfEntry *e = &app->cur.dir->e[app->cur.vis[i]];

		if (sel && sel->n ? !wf_nameset_has(sel, e->name) : e != cur) {
			continue;
		}
		if (!(e->identity.kind == WF_ID_STRONG)) {
			wf_slot_free(s);
			wf_app_set_msg(
			        app, wf_msg(MSG_JOB_RECYCLE_ID_UNPROVEN));
			return false;
		}
		s->src[n].name = wf_path_dup(e->name);
		if (!s->src[n].name) {
			s->count = n;
			wf_slot_free(s);
			wf_app_set_msg(app, wf_msg(MSG_JOB_NOMEM));
			return false;
		}
		s->src[n].snap.id = e->identity;
		s->src[n].snap.flags = e->flags;
		s->src[n].snap.size = e->size;
		s->src[n].snap.mtime = e->mtime;
		n++;
		s->count = n;
	}
	if (!n) {
		wf_slot_free(s);
		wf_app_set_msg(app, wf_msg(MSG_JOB_RECYCLE_HIDDEN));
		return false;
	}
	return true;
}

static void recycle_ask(WfApp *app, const WfEvent *trigger);

void wf_app_recycle_begin(WfApp *app, const WfEvent *trigger)
{
	if (!wf_copyjob_allowed(WF_CJ_RECYCLE, WF_COLLIDE_STOP)) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_RECYCLE_UNAVAILABLE));
		return;
	}
	if (!app->cur.dir || app->want_path) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_RECYCLE_WAIT));
		return;
	}
	if (!recycle_capture(app)) {
		return;
	}
	recycle_ask(app, trigger);
}

/* Asks the Delete question for what recycle_slot holds. */
static void recycle_ask(WfApp *app, const WfEvent *trigger)
{
	char l1[WF_CONFIRM_LINE_BYTES], l2[WF_CONFIRM_LINE_BYTES];
	const char *lines[5];
	WfBuf name;
	WfStatus st;
	int n = 0;

	(void)wf_msgf(l1, sizeof l1, MSG_JOB_RECYCLE_LINE,
	        &WF_ARG_N(app->recycle_slot.count), 1);
	wf_buf_init(&name);
	if (1 == app->recycle_slot.count) {
		const wchar_t *w = app->recycle_slot.src[0].name;

		wf_sanitize_utf16(w, wcslen(w), &name);
		wf_utf8_elide(l2, sizeof l2, wf_buf_cstr(&name), 50);
	} else {
		(void)wf_msgf(l2, sizeof l2, MSG_JOB_RECYCLE_SELECTED,
		        &WF_ARG_N(app->recycle_slot.count), 1);
	}
	wf_buf_free(&name);
	lines[n++] = wf_msg(MSG_JOB_RECYCLE_TITLE);
	lines[n++] = l1;
	lines[n++] = l2;
	lines[n++] = wf_msg(MSG_JOB_RECYCLE_NOTE);
	st = wf_app_confirm_begin(
	        app, lines, n, wf_app_recycle_confirmed, NULL, trigger);
	if (WF_OK != st) {
		wf_slot_free(&app->recycle_slot);
		wf_app_set_error(app, st);
	}
}

/* The Delete question again for the sources of a recycle that failed in use
 * (takes the slot). Everything is asked as for the first time, and the job
 * checks the identities the sources had. */
bool wf_app_recycle_retry(WfApp *app, WfTransferSlot *take)
{
	if (!wf_copyjob_allowed(WF_CJ_RECYCLE, WF_COLLIDE_STOP) ||
	        !app->copy.submit || app->want_path || !take->count) {
		return false;
	}
	wf_slot_free(&app->recycle_slot);
	app->recycle_slot = *take;
	wf_slot_init(take);
	recycle_ask(app, NULL);
	return true;
}

/* Messages are printf-style; the attribute lets clang check them. */
#if defined(__clang__) || defined(__GNUC__)
#	define CP_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#	define CP_PRINTF(a, b)
#endif
static void line(WfApp *app, const char *fmt, ...) CP_PRINTF(2, 3);

static void line(WfApp *app, const char *fmt, ...)
{
	va_list ap;

	if (app->report_n >= WF_REPORT_LINES) {
		return;
	}
	va_start(ap, fmt);
	vsnprintf(app->report[app->report_n++], WF_REPORT_BYTES, fmt, ap);
	va_end(ap);
}

/* A report line from the catalog. */
static void report_msg(WfApp *app, WfMsg id, const WfMsgArg *args, size_t nargs)
{
	if (app->report_n >= WF_REPORT_LINES) {
		return;
	}
	(void)wf_msgf(
	        app->report[app->report_n++], WF_REPORT_BYTES, id, args, nargs);
}

static const char *verb_of_kind(WfCopyKind k)
{
	return wf_msg(WF_CJ_MOVE_ACROSS == k ? MSG_JOB_VERB_MOVE
	                : WF_CJ_RECYCLE == k ? MSG_JOB_VERB_RECYCLE
	                                     : MSG_JOB_VERB_COPY);
}

static const char *verify_name(WfCopyVerify v)
{
	return wf_msg(WF_VERIFY_NONE == v            ? MSG_JOB_VERIFY_NONE
	                : WF_VERIFY_HMAC_SHA256 == v ? MSG_JOB_VERIFY_HMAC
	                                             : MSG_JOB_VERIFY_SHA);
}

/* The copy engine names why a file failed; the words are chosen here. */
const char *wf_app_copyfail_text(WfCopyFailKind k);
static const char *copyfail_text(WfCopyFailKind k)
{
	return wf_app_copyfail_text(k);
}

const char *wf_app_copyfail_text(WfCopyFailKind k)
{
	switch (k) {
	case WF_CF_SOURCE_CHANGED: return wf_msg(MSG_JOB_CF_SOURCE_CHANGED);
	case WF_CF_SOURCE_UNREADABLE:
		return wf_msg(MSG_JOB_CF_SOURCE_UNREADABLE);
	case WF_CF_EXISTS:            return wf_msg(MSG_JOB_CF_EXISTS);
	case WF_CF_DEST_CHANGED:      return wf_msg(MSG_JOB_CF_DEST_CHANGED);
	case WF_CF_DEST_UNSAFE:       return wf_msg(MSG_JOB_CF_DEST_UNSAFE);
	case WF_CF_NO_SPACE:          return wf_msg(MSG_JOB_CF_NO_SPACE);
	case WF_CF_WRITE_FAILED:      return wf_msg(MSG_JOB_CF_WRITE_FAILED);
	case WF_CF_VERIFY_FAILED:     return wf_msg(MSG_JOB_CF_VERIFY_FAILED);
	case WF_CF_METADATA_FAILED:   return wf_msg(MSG_JOB_CF_METADATA_FAILED);
	case WF_CF_COMMIT_FAILED:     return wf_msg(MSG_JOB_CF_COMMIT_FAILED);
	case WF_CF_CLOUD_SKIPPED:     return wf_msg(MSG_JOB_CF_CLOUD);
	case WF_CF_ENCRYPTED_SKIPPED: return wf_msg(MSG_JOB_CF_ENCRYPTED);
	case WF_CF_LINK_SKIPPED:      return wf_msg(MSG_JOB_CF_LINK);
	case WF_CF_TOO_DEEP:          return wf_msg(MSG_JOB_CF_TOO_DEEP);
	case WF_CF_NOT_REMOVED:       return wf_msg(MSG_JOB_CF_NOT_REMOVED);
	case WF_CF_STAGING_LEFT:      return wf_msg(MSG_JOB_CF_STAGING_LEFT);
	case WF_CF_REPLACED_IN_BIN:   return wf_msg(MSG_JOB_CF_REPLACED_IN_BIN);
	case WF_CF_ARCHIVE_NAME:      return wf_msg(MSG_JOB_CF_ARCHIVE_NAME);
	case WF_CF_ARCHIVE_COLLISION:
		return wf_msg(MSG_JOB_CF_ARCHIVE_COLLISION);
	case WF_CF_ARCHIVE_DATA:  return wf_msg(MSG_JOB_CF_ARCHIVE_DATA);
	case WF_CF_ARCHIVE_LIMIT: return wf_msg(MSG_JOB_CF_ARCHIVE_LIMIT);
	case WF_CF_ARCHIVE_UNSUPPORTED:
		return wf_msg(MSG_JOB_CF_ARCHIVE_UNSUPPORTED);
	case WF_CF_ARCHIVE_CHANGED: return wf_msg(MSG_JOB_CF_ARCHIVE_CHANGED);
	}
	return wf_msg(MSG_JOB_CF_OTHER);
}

/* A checked copy in which a file's read-back differed: the result then says how
 * many files were verified and how many were not (DR-DRAFT-019, F9). */
static bool verify_mismatch(const WfCopyResult *r)
{
	size_t kept = r->failure_total < WF_CJ_FAILURES ? r->failure_total
	                                                : WF_CJ_FAILURES;

	if (WF_VERIFY_NONE == r->verify) {
		return false;
	}
	for (size_t i = 0; i < kept; i++) {
		if (WF_CF_VERIFY_FAILED == r->failures[i].kind) {
			return true;
		}
	}
	return false;
}

void wf_app_on_copy_result(WfApp *app, WfCopyResult *r)
{
	wf_app_dircache_invalidate(app);
	const char *verb = verb_of_kind(r->kind);
	char head[96], size[32];
	bool problems, lock_hint;
	size_t i, shown;
	uint32_t undo_uid = 0;

	if (app->copy_jobs) {
		app->copy_jobs--;
	}
	if (!app->copy_jobs && app->copy_paused) { /* a pause is for the job */
		app->copy_paused = false;
		app->copy.pause(app->copy.ctx, false);
	}
	if (WF_CS_DONE == r->state || WF_CS_CANCELLED == r->state ||
	        WF_CS_FAILED == r->state) {
		undo_uid = wf_app_undo_keep_created(app, r);
	}
	if (WF_CJ_RECYCLE == r->kind) {
		wf_app_undo_recycle_result(app, r);
	}

	lock_hint = wf_app_lock_job_result(app, r);
	problems = r->failure_total || r->links_skipped || r->streams_skipped ||
	        WF_CS_DONE != r->state;
	wf_app_fmt_bytes(r->bytes, size, sizeof size);

	app->report_n = 0;
	{
		char n_copied[24], n_failed[24], n_skipped[24], n_second[24],
		        n_third[24];

		snprintf(n_copied, sizeof n_copied, "%zu", r->files_copied);
		snprintf(n_failed, sizeof n_failed, "%zu", r->files_failed);
		snprintf(n_skipped, sizeof n_skipped, "%zu",
		        r->files_skipped + r->links_skipped);
		snprintf(n_second, sizeof n_second, "%zu", r->sources_recycled);
		snprintf(n_third, sizeof n_third, "%zu", r->sources_kept);
		if (WF_CS_CANCELLED == r->state && WF_CJ_RECYCLE == r->kind) {
			(void)wf_msgf(head, sizeof head,
			        MSG_JOB_RES_RECYCLE_CANCELLED,
			        &WF_ARG_S(n_second), 1);
		} else if (WF_CS_CANCELLED == r->state) {
			(void)wf_msgf(head, sizeof head, MSG_JOB_RES_CANCELLED,
			        (WfMsgArg[]){
			                WF_ARG_S(verb), WF_ARG_S(n_copied)},
			        2);
		} else if (WF_CS_FAILED == r->state) {
			(void)wf_msgf(head, sizeof head, MSG_JOB_RES_FAILED,
			        &WF_ARG_S(verb), 1);
		} else if (WF_CJ_RECYCLE == r->kind && r->sources_kept) {
			(void)wf_msgf(head, sizeof head,
			        MSG_JOB_RES_RECYCLE_KEPT,
			        (WfMsgArg[]){
			                WF_ARG_S(n_second), WF_ARG_S(n_third)},
			        2);
		} else if (WF_CJ_RECYCLE == r->kind) {
			(void)wf_msgf(head, sizeof head,
			        MSG_JOB_RES_RECYCLE_DONE, &WF_ARG_S(n_second),
			        1);
		} else if (problems && verify_mismatch(r)) {
			(void)wf_msgf(head, sizeof head,
			        MSG_JOB_RES_VERIFY_PROBLEMS,
			        (WfMsgArg[]){WF_ARG_S(verb), WF_ARG_S(n_copied),
			                WF_ARG_S(n_failed),
			                WF_ARG_S(n_skipped)},
			        4);
		} else if (problems) {
			(void)wf_msgf(head, sizeof head, MSG_JOB_RES_PROBLEMS,
			        (WfMsgArg[]){WF_ARG_S(verb), WF_ARG_S(n_copied),
			                WF_ARG_S(n_failed),
			                WF_ARG_S(n_skipped)},
			        4);
		} else if (WF_CJ_MOVE_ACROSS == r->kind) {
			(void)wf_msgf(head, sizeof head, MSG_JOB_RES_MOVE_DONE,
			        (WfMsgArg[]){WF_ARG_S(n_copied), WF_ARG_S(size),
			                WF_ARG_S(WF_VERIFY_NONE == r->verify
			                                ? wf_msg(MSG_JOB_NOT_VERIFIED)
			                                : "")},
			        3);
		} else if (r->files_skipped || r->overwritten) {
			snprintf(
			        n_third, sizeof n_third, "%zu", r->overwritten);
			snprintf(n_skipped, sizeof n_skipped, "%zu",
			        r->files_skipped);
			(void)wf_msgf(head, sizeof head,
			        MSG_JOB_RES_COPY_SKIPPED,
			        (WfMsgArg[]){WF_ARG_S(n_copied),
			                WF_ARG_S(n_third), WF_ARG_S(n_skipped),
			                WF_ARG_S(verify_name(r->verify))},
			        4);
		} else {
			(void)wf_msgf(head, sizeof head,
			        r->extracted ? MSG_JOB_RES_EXTRACT_DONE
			                     : MSG_JOB_RES_COPY_DONE,
			        (WfMsgArg[]){WF_ARG_S(n_copied), WF_ARG_S(size),
			                WF_ARG_S(verify_name(r->verify))},
			        3);
		}
	}

	line(app, "%s", head);
	line(app, "");
	if (r->extracted) {
		report_msg(app, MSG_JOB_RES_EXTRACTED,
		        &WF_ARG_N(r->failure_total), 1);
	}
	if (WF_CJ_RECYCLE != r->kind) {
		report_msg(app, MSG_JOB_RES_COPIED,
		        (WfMsgArg[]){WF_ARG_N(r->files_copied), WF_ARG_S(size),
		                WF_ARG_N(r->dirs_created),
		                WF_ARG_N(r->overwritten)},
		        4);
		report_msg(app, MSG_JOB_RES_CHECKING,
		        &WF_ARG_S(verify_name(r->verify)), 1);
		if (r->files_skipped || r->links_skipped) {
			report_msg(app, MSG_JOB_RES_SKIPPED,
			        (WfMsgArg[]){WF_ARG_N(r->files_skipped),
			                WF_ARG_N(r->links_skipped)},
			        2);
		}
		if (r->streams_kept || r->streams_skipped) {
			report_msg(app, MSG_JOB_RES_STREAMS,
			        (WfMsgArg[]){WF_ARG_N(r->streams_kept),
			                WF_ARG_N(r->streams_skipped)},
			        2);
		}
		if (WF_CJ_MOVE_ACROSS == r->kind) {
			report_msg(app, MSG_JOB_RES_ORIGINALS,
			        (WfMsgArg[]){WF_ARG_N(r->sources_recycled),
			                WF_ARG_N(r->sources_kept)},
			        2);
		}
	}
	if (r->sources_untouched || r->sources_uncertain) {
		report_msg(app, MSG_JOB_RES_RECYCLE_DETAIL,
		        (WfMsgArg[]){WF_ARG_N(r->sources_untouched),
		                WF_ARG_N(r->sources_uncertain)},
		        2);
	}
	if (r->sources_uncertain ||
	        (r->sources_kept && !r->sources_untouched)) {
		report_msg(app, MSG_JOB_RES_CHECK_LOCATIONS, NULL, 0);
	}
	shown = r->failure_total < WF_CJ_FAILURES ? r->failure_total
	                                          : WF_CJ_FAILURES;
	for (i = 0; i < shown && app->report_n < WF_REPORT_LINES - 4; i++) {
		WfBuf nm;
		char name[40], code[16];

		wf_buf_init(&nm);
		wf_sanitize_utf16(
		        r->failures[i].name, wcslen(r->failures[i].name), &nm);
		wf_utf8_elide(name, sizeof name, wf_buf_cstr(&nm), 34);
		snprintf(code, sizeof code, "%lu", r->failures[i].error);
		report_msg(app, MSG_JOB_RES_FAILURE,
		        (WfMsgArg[]){WF_ARG_S(name),
		                WF_ARG_S(copyfail_text(r->failures[i].kind)),
		                WF_ARG_S(code)},
		        3);
		wf_buf_free(&nm);
	}
	if (r->failure_total > i) {
		report_msg(app, MSG_JOB_RES_MORE,
		        &WF_ARG_N(r->failure_total - i), 1);
	}
	if (WF_CJ_RECYCLE != r->kind && r->files_copied) {
		report_msg(app, MSG_JOB_RES_NOT_PRESERVED, NULL, 0);
	}
	if (lock_hint) {
		line(app, "");
		line(app, "%s", wf_msg(MSG_LOCK_REPORT_HINT));
	}
	line(app, "");
	line(app, "%s", wf_msg(MSG_HELP_CLOSE));

	if (WF_CJ_MOVE_ACROSS == r->kind && r->sources_recycled &&
	        app->slot.count && WF_XFER_MOVE == app->slot.mode &&
	        wf_fileid_equal_or_weak(
	                &app->slot.src[0].snap.id, &app->cross_first)) {
		wf_app_xfer_drop_slot(
		        app); /* those sources are in the Bin now */
	}
	app->msg_keep_until = app->now_ms + WF_RECEIPT_KEEP_MS;
	/* the row of the history: Undo exists for the new items of a copy, and
	 * for nothing else the copy engine does */
	wf_app_joblog_add(app, head, r, undo_uid,
	        WF_CJ_RECYCLE == r->kind ? MSG_HIST_WHY_RECYCLE
	                : WF_CJ_MOVE_ACROSS == r->kind
	                ? MSG_HIST_WHY_MOVE_ACROSS
	                : WF_CJ_COPY == r->kind && r->created_n
	                ? MSG_HIST_WHY_NOT_KEPT
	                : MSG_HIST_WHY_COPY_NOTHING);
	wf_app_reload(app);
	wf_app_set_msg(app, head);
	if (problems && &WF_MODE_NORMAL == app->mode) {
		wf_app_report_show(app);
	}
}

static void report_draw(const WfApp *app, WfFrame *f)
{
	const char *lines[WF_REPORT_LINES];
	int i;

	int rows;

	for (i = 0; i < app->report_n; i++) {
		lines[i] = app->report[i];
	}
	if (wf_ui_alert(f, lines, app->report_n)) {
		return;
	}
	/* too small for the box: a window that scrolls, every line reachable */
	rows = f->rows - 1;
	wf_frame_fill(f, 0, 0, f->cols, f->rows, ' ', &WF_STYLE_BASE);
	for (i = 0; i < rows && app->report_top + i < app->report_n; i++) {
		wf_app_put_fit(f, 0, i, lines[app->report_top + i],
		        &WF_STYLE_BASE, f->cols);
	}
	if (rows >= 1) {
		wf_app_put_fit(f, 0, rows, wf_msg(MSG_JOBLOG_SCROLL),
		        &wf_app_style_bar, f->cols);
	}
}

static WfAction report_event(WfApp *app, const WfEvent *ev)
{
	int page = app->rows > 2 ? app->rows - 1 : 1; /* the lines shown */
	int limit = app->report_n > page ? app->report_n - page : 0;

	if (WF_EV_RELEASE == ev->type) {
		return WF_ACT_NONE;
	}
	if (WF_EV_CHAR == ev->type && 'L' == ev->ch && app->lock.op) {
		app->mode =
		        &WF_MODE_NORMAL; /* the hint in the report names L */
		wf_app_cmd_locks(app);
		return WF_ACT_REDRAW;
	}
	if (WF_EV_RESIZE == ev->type) {
		if (app->report_top > limit) {
			app->report_top = limit;
		}
		return WF_ACT_NONE;
	}
	if (WF_EV_KEY == ev->type && limit > 0) {
		switch (ev->key) {
		case WF_KEY_DOWN:
			app->report_top = app->report_top < limit
			        ? app->report_top + 1
			        : limit;
			return WF_ACT_REDRAW;
		case WF_KEY_UP:
			app->report_top =
			        app->report_top ? app->report_top - 1 : 0;
			return WF_ACT_REDRAW;
		case WF_KEY_PGDN:
			app->report_top = app->report_top + page < limit
			        ? app->report_top + page
			        : limit;
			return WF_ACT_REDRAW;
		case WF_KEY_PGUP:
			app->report_top = app->report_top > page
			        ? app->report_top - page
			        : 0;
			return WF_ACT_REDRAW;
		case WF_KEY_HOME: app->report_top = 0; return WF_ACT_REDRAW;
		case WF_KEY_END:  app->report_top = limit; return WF_ACT_REDRAW;
		default:          break;
		}
	}
	app->mode = &WF_MODE_NORMAL;
	return WF_ACT_REDRAW;
}

const WfMode WF_MODE_REPORT = {"REPORT", report_draw, report_event};

void wf_app_report_show(WfApp *app)
{
	if (app->report_n) {
		app->report_top = 0;
		app->mode = &WF_MODE_REPORT;
	}
}

/* Cells of a text, for fitting the parts of the line. */
static int cells_of(const WfBuf *b)
{
	return wf_utf8_cells(wf_buf_cstr(b), b->len);
}

/* The running job as one line of at most `room` cells. What is kept first is
 * what tells the person what is going on and how to stop it: the state and the
 * count, the keys, then the bytes and the queue; the name of the file is what
 * gives way, cut to the room that is left (or left out when there is none). */
void wf_app_copy_status(const WfApp *app, WfBuf *s, int room)
{
	WfCopyProgress p;
	WfBuf head, bytes, name, queued, keys;
	char done[32], total[32], count[48], text[96], shown[512];
	int left;

	if (app->copy_paused && !app->copy_jobs) {
		wf_buf_puts(s, wf_msg(MSG_JOB_STATUS_PAUSED));
		return;
	}
	if (!app->copy_jobs || !app->copy.progress ||
	        !app->copy.progress(app->copy.ctx, &p)) {
		return;
	}
	wf_buf_init(&head);
	wf_buf_init(&bytes);
	wf_buf_init(&name);
	wf_buf_init(&queued);
	wf_buf_init(&keys);
	wf_buf_puts(&head,
	        wf_msg(WF_CS_PAUSED == p.state
	                        ? MSG_JOB_STATUS_PAUSED_PREFIX
	                        : MSG_JOB_STATUS_WORKING_PREFIX));
	if (p.files_total) {
		snprintf(count, sizeof count, "%u/%u", (unsigned)p.files_done,
		        (unsigned)p.files_total);
	} else {
		snprintf(count, sizeof count, "%u", (unsigned)p.files_done);
	}
	(void)wf_msgf(
	        text, sizeof text, MSG_JOB_STATUS_FILES, &WF_ARG_S(count), 1);
	wf_buf_puts(&head, text);
	if (p.bytes_total) {
		wf_app_fmt_bytes(p.bytes_done, done, sizeof done);
		wf_app_fmt_bytes(p.bytes_total, total, sizeof total);
		(void)wf_msgf(text, sizeof text, MSG_JOB_STATUS_BYTES,
		        (WfMsgArg[]){WF_ARG_S(done), WF_ARG_S(total)}, 2);
		wf_buf_puts(&bytes, text);
	}
	if (p.current[0]) {
		wf_sanitize_utf16(p.current, wcslen(p.current), &name);
	}
	if (p.queued) {
		(void)wf_msgf(text, sizeof text, MSG_JOB_STATUS_QUEUED,
		        &WF_ARG_N(p.queued), 1);
		wf_buf_puts(&queued, text);
	}
	wf_buf_puts(&keys, wf_msg(MSG_JOB_STATUS_KEYS));

	left = room - cells_of(&head) - cells_of(&keys);
	if (left < 0) {
		/* not even the state and the keys: the state alone, if it fits */
		wf_buf_clear(&keys);
		left = room - cells_of(&head);
	}
	wf_buf_append(s, wf_buf_cstr(&head), head.len);
	if (left >= 0 && bytes.len && cells_of(&bytes) <= left) {
		wf_buf_append(s, wf_buf_cstr(&bytes), bytes.len);
		left -= cells_of(&bytes);
	}
	if (left >= 0 && queued.len && cells_of(&queued) <= left) {
		left -= cells_of(&queued);
	} else {
		wf_buf_clear(&queued);
	}
	if (name.len &&
	        left - 1 >= 6) { /* a space, and at least a stub of it */
		wf_utf8_elide(shown, sizeof shown, wf_buf_cstr(&name),
		        (size_t)(left - 1));
		wf_buf_puts(s, " ");
		wf_buf_puts(s, shown);
	}
	wf_buf_append(s, wf_buf_cstr(&queued), queued.len);
	wf_buf_append(s, wf_buf_cstr(&keys), keys.len);
	wf_buf_free(&head);
	wf_buf_free(&bytes);
	wf_buf_free(&name);
	wf_buf_free(&queued);
	wf_buf_free(&keys);
}

uint32_t wf_app_copy_timeout(const WfApp *app)
{
	if (!app->copy_jobs) {
		return WF_WAIT_FOREVER;
	}
	return app->now_ms >= app->copy_refresh_at
	        ? 0
	        : (uint32_t)(app->copy_refresh_at - app->now_ms);
}

bool wf_app_copy_tick(WfApp *app)
{
	if (!app->copy_jobs || app->now_ms < app->copy_refresh_at) {
		return false;
	}
	app->copy_refresh_at = app->now_ms + COPY_REFRESH_MS;
	return true;
}

void wf_app_cmd_pause(WfApp *app)
{
	if (!app->copy.pause) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_COPY_UNAVAILABLE));
		return;
	}
	app->copy_paused = !app->copy.paused(app->copy.ctx);
	app->copy.pause(app->copy.ctx, app->copy_paused);
	wf_app_set_msg(app,
	        wf_msg(app->copy_paused ? MSG_JOB_PAUSED : MSG_JOB_RESUMED));
}

void wf_app_cmd_cancel_jobs(WfApp *app)
{
	if (wf_app_wsl_cancel(app)) {
		return; /* a WSL copy, move or delete is what is running */
	}
	if (!app->copy.cancel || !app->copy_jobs) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_NOTHING_TO_CANCEL));
		return;
	}
	app->copy_paused = false;
	app->copy.cancel(app->copy.ctx, true);
	wf_app_set_msg(app, wf_msg(MSG_JOB_CANCELLING));
}

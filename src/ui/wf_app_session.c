/*
 * `piero --continue` in the application: what is captured of the screen, and
 * how a saved session is put back (DR-DRAFT-018).
 *
 * Putting back is not a second way of doing anything. The folder is an
 * ordinary navigation (so the listing, the generation that drops an old answer,
 * and the confirmation for a network share are the ones the person always
 * meets); the cursor and the scroll are a position seeded into the Navigation
 * Memory, which the listing reads when it arrives; the selection is the
 * selection, pruned by the listing; the preview's scroll is the preview's own
 * restore after an editor save. A session only ever names places: nothing is
 * opened for writing and no program is started.
 */
#include <stdlib.h>
#include <string.h>

#include "../archive/wf_vpath.h"
#include "../fs/wf_sftpplace_win.h"
#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"

void wf_app_session_destroy(WfApp *app)
{
	free(app->session_restore.edit.path);
	free(app->session_restore.dir);
	free(app->session_restore.entry);
	memset(&app->session_restore, 0, sizeof app->session_restore);
}

void wf_app_notice(WfApp *app, const char *text)
{
	wf_app_set_msg(app, text);
	app->msg_keep_until = app->now_ms + WF_RECEIPT_KEEP_MS;
}

/* The folder to save for the one on screen: an archive is saved as the folder
 * that holds it (what the shell is left in too). NULL: nothing to save. */
static wchar_t *saved_folder(const WfApp *app)
{
	wchar_t *dir;

	if (wf_sftp_place_is(app->cwd)) {
		return NULL; /* a session does not hold an ssh place */
	}
	dir = wf_path_dup(app->cwd);
	if (dir && wf_vpath_is(dir)) {
		dir[wf_vpath_archive_len(dir)] = L'\0';
		dir[wf_path_parent_len(dir)] = L'\0';
	}
	return dir;
}

static WfSessionPane pane_of(const WfApp *app)
{
	return &WF_MODE_PREVIEW_PANE == app->mode ? WF_SESSION_PANE_PREVIEW
	        : &WF_MODE_PREVIEW == app->mode   ? WF_SESSION_PANE_FULL
	                                          : WF_SESSION_PANE_LIST;
}

bool wf_app_session_capture(const WfApp *app, WfSession *out)
{
	const WfEntry *entry = wf_view_current(&app->cur);
	const WfNameSet *picked;
	bool in_archive;
	size_t want;

	memset(out, 0, sizeof *out);
	if (!app->cwd || !app->cur.dir || app->want_path) {
		return false; /* no folder, or one is being replaced right now */
	}
	out->dir = saved_folder(app);
	if (!out->dir) {
		return false;
	}
	in_archive = wf_vpath_is(app->cwd);
	if (entry && !in_archive) {
		out->entry = wf_path_dup(entry->name);
		if (!out->entry) {
			goto fail;
		}
	}
	out->cursor = in_archive ? 0 : app->cur.cursor;
	out->top = in_archive ? 0
	                      : wf_view_top(&app->cur, wf_app_body_height(app));
	picked = in_archive ? NULL : wf_sel_folder(&app->sel, app->cwd);
	want = picked ? picked->n : 0;
	if (want > WF_SESSION_MAX_SELECTED) {
		want = WF_SESSION_MAX_SELECTED;
	}
	if (want) { /* one allocation for the list, not one for every name */
		out->selected = calloc(want, sizeof *out->selected);
		if (!out->selected) {
			goto fail;
		}
	}
	for (size_t i = 0; i < want; i++) {
		out->selected[i] = wf_path_dup(picked->names[i]);
		if (!out->selected[i]) {
			goto fail;
		}
		out->n_selected++; /* what is owned so far, for the free on failure */
	}
	out->pane = in_archive ? WF_SESSION_PANE_LIST : pane_of(app);
	/* the editor had the screen: its file and where the cursor was; the text
	 * is the draft's, never the session's */
	if (&WF_MODE_EDITOR == app->mode && app->editor.place &&
	        app->editor.place(app->editor.ctx, &out->edit)) {
		out->has_edit = true;
	}
	out->pv_top = app->preview_top;
	out->pv_left = app->preview_left;
	return true;
fail:
	wf_session_free(out);
	return false;
}

WfStatus wf_app_session_restore(WfApp *app, const WfSession *s)
{
	WfStatus st;

	if (!s || !s->dir || !wf_session_path_ok(s->dir)) {
		return WF_E_INVAL;
	}
	wf_app_session_destroy(app);
	app->session_restore.dir = wf_path_dup(s->dir);
	app->session_restore.entry = s->entry ? wf_path_dup(s->entry) : NULL;
	if (!app->session_restore.dir ||
	        (s->entry && !app->session_restore.entry)) {
		wf_app_session_destroy(app);
		return WF_E_NOMEM;
	}
	app->session_restore.pane = s->pane;
	if (s->has_edit && s->edit.path) {
		app->session_restore.edit = s->edit;
		app->session_restore.edit.path = wf_path_dup(s->edit.path);
		if (!app->session_restore.edit.path) {
			wf_app_session_destroy(app);
			return WF_E_NOMEM;
		}
		app->session_restore.has_edit = true;
	}
	app->session_restore.pv_top = s->pv_top;
	app->session_restore.pv_left = s->pv_left;
	/* the selection: names for the folder; the listing prunes those gone */
	for (size_t i = 0; i < s->n_selected; i++) {
		if (wf_nameset_has(
		            wf_sel_folder(&app->sel, s->dir), s->selected[i])) {
			continue;
		}
		st = wf_sel_toggle(&app->sel, s->dir, s->selected[i]);
		if (WF_OK != st) {
			wf_app_session_destroy(app);
			return st;
		}
	}
	app->session_restore.cursor = s->cursor;
	app->session_restore.top = s->top;
	app->session_restore.active = true; /* the listing reads it */
	st = wf_app_navigate(app, s->dir, NULL, SIZE_MAX);
	if (WF_OK != st) {
		wf_app_session_destroy(app);
		return st;
	}
	return WF_OK;
}

/* The words for a folder, as the status line says it. */
static void say_folder(WfApp *app, WfMsg msg, const wchar_t *dir)
{
	const wchar_t *leaf;
	size_t n;
	WfBuf shown;
	char text[sizeof app->msg];

	wf_vpath_leaf(dir, &leaf, &n);
	wf_buf_init(&shown);
	if (WF_OK == wf_sanitize_utf16(leaf, n, &shown)) {
		(void)wf_msgf(text, sizeof text, msg,
		        &WF_ARG_S(wf_buf_cstr(&shown)), 1);
		wf_app_set_msg(app, text);
	}
	wf_buf_free(&shown);
}

/* Opens the editor on the saved file, if it is in this folder, by the same
 * call the person's `e` makes (so a link, a cloud file or a folder is refused as
 * always, a draft of unsaved text is offered by the editor's own question, and
 * a file that changed meanwhile is simply read as it is now). Then the cursor
 * and the view go where they were. Nothing is written. */
static void restore_editor(WfApp *app)
{
	const wchar_t *path = app->session_restore.edit.path, *leaf;
	size_t parent = wf_path_parent_len(path), n;

	if (!app->editor.open || parent != wcslen(app->cwd) ||
	        CSTR_EQUAL !=
	                CompareStringOrdinal(path, (int)parent, app->cwd,
	                        (int)parent, TRUE)) {
		wf_app_set_msg(app,
		        wf_msg(MSG_SESSION_RESTORED)); /* the folder only */
		return;
	}
	wf_vpath_leaf(path, &leaf, &n);
	wf_app_edit_begin_named(app, leaf);
	if (&WF_MODE_EDITOR == app->mode && app->editor.goto_place) {
		app->editor.goto_place(
		        app->editor.ctx, &app->session_restore.edit);
	}
}

void wf_app_session_listing_done(WfApp *app)
{
	const WfEntry *entry;
	wchar_t *path;

	if (!app->session_restore.active) {
		return;
	}
	if (!app->cwd || wcscmp(app->cwd, app->session_restore.dir)) {
		/* the person went somewhere else first: nothing more to put back */
		wf_app_session_destroy(app);
		return;
	}
	if (app->session_restore.has_edit) {
		restore_editor(app);
		wf_app_session_destroy(app);
		return;
	}
	entry = wf_view_current(&app->cur);
	/* the preview and who has the keys come back only when the cursor is on
	 * the file they were about */
	if (WF_SESSION_PANE_LIST != app->session_restore.pane && entry &&
	        !(entry->flags & WF_ENT_DIR) && app->session_restore.entry &&
	        !wcscmp(entry->name, app->session_restore.entry) &&
	        WF_OK == wf_path_join(app->cwd, entry->name, &path)) {
		free(app->preview_keep.path);
		app->preview_keep.path = path;
		app->preview_keep.top = app->session_restore.pv_top;
		app->preview_keep.left = app->session_restore.pv_left;
		if (WF_SESSION_PANE_FULL == app->session_restore.pane) {
			app->mode = &WF_MODE_PREVIEW;
		} else {
			wf_app_preview_focus(app);
		}
	}
	if (app->session_restore.fell_back) {
		say_folder(app, MSG_SESSION_FALLBACK, app->cwd);
	} else {
		wf_app_set_msg(app, wf_msg(MSG_SESSION_RESTORED));
	}
	wf_app_session_destroy(app);
}

void wf_app_session_listing_failed(WfApp *app, const wchar_t *failed)
{
	size_t parent;
	wchar_t *up;

	if (!app->session_restore.active) {
		return;
	}
	if (!failed || wcscmp(failed, app->session_restore.dir)) {
		wf_app_session_destroy(app); /* some other navigation failed */
		return;
	}
	/* the saved folder is gone or cannot be read: the nearest one above it,
	 * one asynchronous listing at a time, until the root */
	parent = wf_path_parent_len(failed);
	up = parent ? wf_path_dup_n(failed, parent) : NULL;
	if (!up) {
		wf_app_session_destroy(app);
		return;
	}
	free(app->session_restore.dir);
	app->session_restore.dir = up;
	free(app->session_restore.entry);
	app->session_restore.entry = NULL;
	app->session_restore.pane = WF_SESSION_PANE_LIST;
	app->session_restore.cursor = app->session_restore.top = 0;
	app->session_restore.fell_back = true;
	if (WF_OK != wf_app_navigate(app, up, NULL, SIZE_MAX)) {
		wf_app_session_destroy(app);
	}
}

/*
 * The file manager's side of the built-in editor: `e` opens the file under
 * the cursor, the editor takes the whole screen and every key while it is
 * open, and closing it brings back the three columns untouched: the same
 * folder, cursor, selection and navigation memory, with the listing and the
 * preview refreshed if something was saved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../archive/wf_vpath.h"
#include "../fs/wf_sftpplace_win.h"
#include "../path/wf_path.h"
#include "wf_app_priv.h"

void wf_app_set_editor(WfApp *app, WfEditorPort port)
{
	app->editor = port;
}

bool wf_app_editing(const WfApp *app)
{
	/* the settings screens also take Ctrl chords (Ctrl+S saves) */
	return &WF_MODE_EDITOR == app->mode || &WF_MODE_SETTINGS == app->mode ||
	        &WF_MODE_GO == app->mode;
}

static void edit_entry(WfApp *app, const WfEntry *entry);

void wf_app_edit_begin(WfApp *app)
{
	edit_entry(app, wf_view_current(&app->cur));
}

/* The same, for the entry of the current folder with this name (the backups
 * screen opens a backup this way). */
void wf_app_edit_begin_named(WfApp *app, const wchar_t *leaf)
{
	const WfEntry *entry = NULL;

	for (size_t i = 0; app->cur.dir && i < app->cur.dir->n; i++) {
		if (!wcscmp(app->cur.dir->e[i].name, leaf)) {
			entry = &app->cur.dir->e[i];
			break;
		}
	}
	edit_entry(app, entry);
}

static void edit_entry(WfApp *app, const WfEntry *entry)
{
	wchar_t *path = NULL;

	if (wf_vpath_is(app->cwd)) { /* a WSL file is opened, read-only */
		wf_app_set_msg(app, wf_msg(MSG_AR_READONLY));
		return;
	}
	char why[128];
	WfStatus st;

	if (!app->editor.open) {
		wf_app_set_msg(app, wf_msg(MSG_EDIT_UNAVAILABLE));
		return;
	}
	if (!entry) {
		wf_app_set_msg(app, wf_msg(MSG_EDIT_NOTHING));
		return;
	}
	if (app->want_path) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_STALE_LISTING));
		return;
	}
	if (entry->flags & WF_ENT_DIR) {
		wf_app_set_msg(app, wf_msg(MSG_EDIT_FOLDER));
		return;
	}
	if (entry->flags & WF_ENT_LINK) {
		wf_app_set_msg(app, wf_msg(MSG_EDIT_LINK));
		return;
	}
	if (entry->flags & WF_ENT_CLOUD) {
		wf_app_set_msg(app, wf_msg(MSG_EDIT_CLOUD));
		return;
	}
	st = wf_path_join(app->cwd, entry->name, &path);
	if (WF_OK != st) {
		wf_app_set_error(app, st);
		return;
	}
	why[0] = '\0';
	if (0 != app->editor.open(app->editor.ctx, path, why, sizeof why)) {
		char text[160];

		(void)wf_msgf(text, sizeof text, MSG_EDIT_PREFIX,
		        &WF_ARG_S(why[0] ? why : wf_msg(MSG_EDIT_CANNOT)), 1);
		wf_app_set_msg(app, text);
		free(path);
		return;
	}
	free(path);
	/* closing the editor goes back to the preview it was opened from, at
	 * the place being read; from the lists, to the lists */
	app->edit_from = &WF_MODE_PREVIEW == app->mode ||
	                &WF_MODE_PREVIEW_PANE == app->mode
	        ? app->mode
	        : &WF_MODE_NORMAL;
	app->mode = &WF_MODE_EDITOR;
	app->flush_input = true; /* keys typed ahead belong to the lists */
}

static void leave_editor(WfApp *app)
{
	bool saved = app->editor.take_saved &&
	        app->editor.take_saved(app->editor.ctx);

	free(app->preview_keep.path);
	app->preview_keep.path = NULL;
	if (saved && app->preview_path &&
	        (&WF_MODE_PREVIEW == app->edit_from ||
	                &WF_MODE_PREVIEW_PANE == app->edit_from)) {
		app->preview_keep.path = wf_path_dup(app->preview_path);
		app->preview_keep.top = app->preview_top;
		app->preview_keep.left = app->preview_left;
	}
	app->mode = app->edit_from ? app->edit_from : &WF_MODE_NORMAL;
	app->edit_from = NULL;
	app->flush_input = true;
	if (saved) {
		wf_app_reload(app);
		app->msg_keep_until = app->now_ms + WF_RECEIPT_KEEP_MS;
		wf_app_set_msg(app, wf_msg(MSG_EDIT_CLOSED_SAVED));
	}
}

/* T in the lock window for a save: the editor's own save, from the start. */
void wf_app_editor_save_again(WfApp *app)
{
	int r;

	app->mode = &WF_MODE_EDITOR;
	if (!app->lock.editor_save) {
		return;
	}
	r = app->lock.editor_save(app->editor.ctx);
	if (2 == r) {
		leave_editor(app);
	}
}

static WfAction editor_event(WfApp *app, const WfEvent *ev)
{
	int r;

	if (!app->editor.event) {
		return WF_ACT_NONE;
	}
	r = app->editor.event(app->editor.ctx, ev, app->rows, app->cols);
	if (3 == r) { /* the save found the file in use: look who has it */
		wf_app_editor_lock_look(app);
		return WF_ACT_REDRAW;
	}
	if (2 == r) {
		leave_editor(app);
		return WF_ACT_REDRAW;
	}
	return r ? WF_ACT_REDRAW : WF_ACT_NONE;
}

static void editor_draw(const WfApp *app, WfFrame *f)
{
	if (app->editor.draw) {
		app->editor.draw(app->editor.ctx, f);
	}
}

const WfMode WF_MODE_EDITOR = {"EDIT", editor_draw, editor_event};

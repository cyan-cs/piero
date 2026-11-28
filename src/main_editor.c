#include "main_loop.h"

/* The editor behind the app's port. */
static int editor_open(void *ctx, const wchar_t *path, char *why, size_t cap)
{
	return WF_OPEN_REFUSED == wf_es_open(ctx, path, why, cap);
}

static int editor_event(void *ctx, const WfEvent *ev, int rows, int cols)
{
	wf_es_set_clock(ctx, GetTickCount64()); /* for the timeout of "gg" */
	switch (wf_es_event(ctx, ev, rows, cols)) {
	case WF_ES_NONE: return 0;
	case WF_ES_BUSY:
		return 0; /* pump retains input before the port is called */
	case WF_ES_REDRAW: return wf_es_take_lock_look(ctx) ? 3 : 1;
	case WF_ES_CLOSED: break;
	}
	return 2;
}

static wchar_t *editor_lock_path(void *ctx)
{
	return wf_es_lock_path(ctx);
}

static int editor_save_again(void *ctx)
{
	switch (wf_es_save_again(ctx)) {
	case WF_ES_NONE:   return 0;
	case WF_ES_BUSY:   return 0;
	case WF_ES_REDRAW: return 1;
	case WF_ES_CLOSED: break;
	}
	return 2;
}

/* While a file is being read or written the screen is kept alive: a line at
 * the bottom says what is going on and for how long, a resize is followed,
 * and Esc or Ctrl+C asks the job to stop (it can, until a save begins to
 * replace the file). Everything else typed meanwhile is dropped. */
static bool editor_busy(void *ctx, bool saving, unsigned elapsed_ms)
{
	Loop *lp = ctx;
	char secs[16], text[200], cut[200], seq[320];
	WfEvent ev[16];
	size_t n, got = 0;
	bool keep = true;
	int cols = lp->app.cols, rows = lp->app.rows, pad;

	snprintf(secs, sizeof secs, "%u.%u", elapsed_ms / 1000,
	        (elapsed_ms % 1000) / 100);
	(void)wf_msgf(text, sizeof text, MSG_ED_BUSY,
	        (WfMsgArg[]){WF_ARG_S(wf_msg(saving ? MSG_ED_BUSY_SAVE
	                                            : MSG_ED_BUSY_LOAD)),
	                WF_ARG_S(secs)},
	        2);
	wf_utf8_elide(cut, sizeof cut, text, cols > 1 ? (size_t)cols - 1 : 1);
	pad = cols - wf_utf8_cells(cut, strlen(cut));
	snprintf(seq, sizeof seq, "\x1b[%d;1H\x1b[7m %s%*s\x1b[0m", rows, cut,
	        pad > 1 ? pad - 1 : 0, "");
	(void)wf_term_write(lp->term, seq, strlen(seq));
	/* the screen was written behind the renderer's back: the next frame
	 * must repaint every cell */
	wf_frame_free(lp->spare);
	lp->spare =
	        lp->prev; /* kept to be drawn over; the next frame is a full repaint */
	lp->prev = NULL;

	got = wf_term_read_events(lp->term, ev, 16);
	for (n = 0; n < got; n++) {
		if (WF_EV_RESIZE == ev[n].type) {
			(void)wf_term_size(
			        lp->term, &lp->app.cols, &lp->app.rows);
		} else if ((WF_EV_KEY == ev[n].type &&
		                   WF_KEY_ESC == ev[n].key) ||
		        (WF_EV_CTRL == ev[n].type && 'c' == ev[n].ch)) {
			keep = false;
		}
	}
	return keep;
}

/* The cursor and the view of the current buffer, for the saved session. */
static bool editor_place(void *ctx, WfSessionEdit *out)
{
	WfEsPlace p;

	if (!wf_es_place(ctx, &p)) {
		return false;
	}
	out->path = _wcsdup(p.path);
	out->line = p.line;
	out->col = p.col;
	out->top = p.top;
	out->left = p.left;
	out->modified = p.modified;
	return NULL != out->path;
}

static void editor_goto_place(void *ctx, const WfSessionEdit *in)
{
	wf_es_goto(ctx, in->line, in->col, in->top, in->left);
}

/* The watch says the file under the cursor changed (which is the file the editor
 * opened) while the editor has the screen: if the disk no longer holds what the
 * editor read or wrote, say so once. The message is a hint; the editor's save does
 * its own check of the bytes, whether or not this notice was ever given. */
bool wf_main_editor_disk_check(Loop *lp)
{
	const wchar_t *path;
	WfFileStamp stamp;

	if (!lp->editor || !wf_es_disk_stamp(lp->editor, &path, &stamp) ||
	        !wf_efile_probably_changed(path, &stamp)) {
		return false;
	}
	wf_es_note_external_change(lp->editor);
	return true;
}

static void editor_draw(void *ctx, WfFrame *f)
{
	wf_es_draw(ctx, f);
}

static bool editor_take_saved(void *ctx)
{
	return wf_es_take_saved(ctx);
}

/* The editor is configured from [editor]; the keymap "vim" is not accepted by
 * the parser, so only the two shipped presets can arrive here. */
void wf_main_start_editor(Loop *lp)
{
	const WfConfig *c = lp->config;
	WfEditConfig ec;

	if (!c->editor_enabled) {
		return;
	}
	wf_es_config_default(&ec);
	ec.keymap = 1 == c->editor_keymap ? WF_KM_NANO : WF_KM_PIERO;
	/* A profile that was chosen supplies the editor's keys too; until then
	 * the editor keeps the keys it always had. */
	lp->editor_keys = wf_app_keymap(&lp->app);
	ec.keys = lp->editor_keys &&
	                WF_PROFILE_LEGACY != wf_keymap_profile(lp->editor_keys)
	        ? lp->editor_keys
	        : NULL;
	ec.ed.tab_width = (unsigned)c->editor_tab_width;
	ec.ed.use_spaces = 1 == c->editor_indent;
	ec.ed.auto_indent = 0 != c->editor_auto_indent;
	ec.ed.case_sensitive = 0 != c->editor_case_sensitive;
	ec.ed.max_bytes = (size_t)c->editor_max_mib << 20;
	ec.ed.undo_bytes = (size_t)c->editor_undo_mib << 20;
	ec.line_numbers = 0 != c->editor_line_numbers;
	ec.syntax = 0 != c->editor_syntax;
	ec.soft_wrap = 0 != c->editor_soft_wrap;
	ec.show_whitespace = 0 != c->editor_show_whitespace;
	ec.backup = 0 != c->editor_backup;
	ec.drafts = 0 != c->editor_recovery_draft;
	ec.md_split = c->editor_markdown_split;
	ec.fallback_codepage = (unsigned)c->text_fallback_encoding;
	wf_edit_io_set_busy(editor_busy, lp);
	lp->editor = wf_es_new(&ec, wf_edit_io_system());
	if (!lp->editor) {
		fputs("piero: editor unavailable (out of memory)\n", stderr);
		return;
	}
	if (lp->syntax) {
		wf_es_set_syntax(lp->editor, wf_synworker_port(lp->syntax));
	}
	wf_es_set_git(lp->editor, wf_main_git_port(lp));
	wf_app_set_editor(&lp->app,
	        (WfEditorPort){lp->editor, editor_open, editor_event,
	                editor_draw, editor_take_saved, editor_place,
	                editor_goto_place});
	wf_app_set_editor_lock(&lp->app, editor_lock_path, editor_save_again);
}

/* Select Ctrl+C key handling for editing/capture, and modified-key reporting
 * for the active profile and dialogs. */
void wf_main_sync_editing_keys(Loop *lp)
{
	/* Ctrl+C is a key only where the screen uses it (the editor, Smart Go);
	 * Settings reads Ctrl+S and the like but leaves Ctrl+C the program's
	 * interrupt, as in the file manager. While a key is being chosen for a
	 * command it is a key like any other (and Esc ends the wait) */
	bool capturing = &WF_MODE_SETTINGS == lp->app.mode &&
	        WF_SS_CAPTURE == lp->app.settings.sub;
	bool want = wf_app_editing(&lp->app) &&
	        (&WF_MODE_SETTINGS != lp->app.mode || capturing);
	const WfKeymap *keys = wf_app_keymap(&lp->app);
	bool modified =
	        (keys && WF_PROFILE_LEGACY != wf_keymap_profile(keys)) ||
	        &WF_MODE_SETTINGS == lp->app.mode ||
	        &WF_MODE_HITS == lp->app.mode; /* Ctrl+A marks all results */

	/* Non-Legacy profiles bind Ctrl / Alt chords in the file manager too. */
	if (modified != lp->term_modified &&
	        WF_OK == wf_term_set_modified_keys(lp->term, modified)) {
		lp->term_modified = modified;
	}
	if (want == lp->term_editing) {
		return;
	}
	if (WF_OK == wf_term_set_editing(lp->term, want)) {
		lp->term_editing = want;
	}
}

void wf_main_restart_editor(Loop *lp)
{
	if (wf_app_editing(&lp->app) && &WF_MODE_SETTINGS != lp->app.mode) {
		return;
	}
	wf_app_set_editor(&lp->app, (WfEditorPort){0});
	wf_es_free(lp->editor);
	lp->editor = NULL;
	wf_main_start_editor(lp);
}

#ifndef WF_APP_PRIV_H
#define WF_APP_PRIV_H

#include "wf_dircache.h"

/* Internal seams between app translation units. Not part of the API. */

#include "../base/wf_buf.h"
#include "../text/wf_markdown.h"
#include "wf_app.h"

/* Sets the status-bar message. `text` is sanitized and cut to fit. */
void wf_app_set_msg(WfApp *app, const char *text);

/* Rows available to the file lists at the current terminal size (>= 1). */
int wf_app_body_height(const WfApp *app);

/* Put the selection in the slot to copy / to move; paste the slot here. */
void wf_app_cmd_stage(WfApp *app, bool move);
void wf_app_cmd_paste(WfApp *app);

/* Esc in NORMAL after the selection was already empty: drops the slot.
 * Returns 1 if there was a slot to drop. */
bool wf_app_xfer_drop_slot(WfApp *app);
/* WSL file operations (wf_app_wsl.c). */
/* after c / x; false: nothing was recorded (the earlier mark is kept) */
bool wf_app_wsl_note_clip(WfApp *app, bool move);
bool wf_app_wsl_paste(WfApp *app); /* true: the paste was WSL's */
bool wf_app_wsl_delete(WfApp *app, const WfEvent *ev); /* true: handled */
bool wf_app_wsl_receipt(
        WfApp *app, const WfEditOutcome *outcome, const wchar_t *name);
void wf_app_wsl_clear_clip(WfApp *app);
/* C while a WSL operation runs: it stops at the next chunk or item and says
 * what is already done. True when one was running. */
bool wf_app_wsl_cancel(WfApp *app);
/* The undo that was started has ended: `done` of its items were put back.
 * All of them: the entry goes. Some: it keeps the rest. None: it stays whole,
 * to be tried again. (Accepting the request is not the same as undoing.) */
void wf_app_undo_done(WfApp *app, size_t done);
/* Between the copy results and Undo (wf_app_undo.c). */
size_t wf_app_undo_entry_of_slot(const WfApp *app, const WfTransferSlot *s);
/* Returns the uid of the Undo entry made for the copy; 0: none was made. */
uint32_t wf_app_undo_keep_created(WfApp *app, WfCopyResult *r);
void wf_app_undo_recycle_result(WfApp *app, const WfCopyResult *r);
/* The "yes" of a question that recycles the slot (wf_app_copy.c). */
void wf_app_recycle_confirmed(WfApp *app, void *ctx);
/* Progress of a running WSL copy: true when the line must be redrawn; the
 * time to the next look; the text for the status line. */
bool wf_app_wsl_tick(WfApp *app);
/* Undo of the last WSL create, rename or move. */
void wf_app_wsl_note_form(WfApp *app, bool rename, bool directory);
void wf_app_wsl_edit_done(WfApp *app, const WfEditOutcome *outcome);
bool wf_app_wsl_undo(WfApp *app); /* true: it was WSL's */
uint32_t wf_app_wsl_next_ms(const WfApp *app);
void wf_app_wsl_status(const WfApp *app, WfBuf *s);
void wf_app_wsl_destroy(WfApp *app);

/* Appends " | COPY 3 items" / " | capturing..." style status, if any. */
void wf_app_xfer_status(const WfApp *app, WfBuf *s);

/* Releases the slot. */
void wf_app_xfer_destroy(WfApp *app);

/* Navigation owns pending requests, history and UNC approval state. */
void wf_app_set_error(WfApp *app, WfStatus st);
void wf_app_free_want(WfApp *app);
void wf_app_dircache_forget_neighbours(WfApp *app);
void wf_app_navigation_memory_destroy(WfApp *app);
/* The saved session being put back (wf_app_session.c). */
void wf_app_session_destroy(WfApp *app);
void wf_app_session_listing_done(WfApp *app);
void wf_app_session_listing_failed(WfApp *app, const wchar_t *failed);
WfStatus wf_app_navigate(WfApp *app,
        const wchar_t *path,
        const wchar_t *select,
        size_t history_index);
size_t wf_app_unc_root_length(const wchar_t *path);
bool wf_app_extended_drive(const wchar_t *path);
void wf_app_free_pending_navigation(WfApp *app);
void wf_app_unc_yes(WfApp *app, void *ctx);
void wf_app_go(WfApp *app, const wchar_t *path, const wchar_t *select);
void wf_app_enter_selected(WfApp *app);
void wf_app_toggle_hidden(WfApp *app);
void wf_app_set_hidden(WfApp *app, bool show);
void wf_app_toggle_select(WfApp *app);
/* Quit now, or ask first if work is running. */
WfAction wf_app_quit_request(WfApp *app);
/* The keys of NORMAL mode, and what to do with them (wf_app_keys.c). */
WfAction wf_app_fm_key(WfApp *app, const WfEvent *ev);
/* The pointer in the file list (wf_app_mouse.c). */
WfAction wf_app_mouse(WfApp *app, const WfEvent *ev);
/* Whether a screen is taking typed text right now (a settings field). Each
 * screen answers for itself; the paste filter asks. */
bool wf_app_settings_takes_text(const WfApp *app);
/* Scrolls the preview by `delta` lines (negative: up), within its limits. */
void wf_app_preview_scroll(WfApp *app, int delta);
/* Feeds one key to the sequence matcher of `ctx`; the state restarts when
 * the context is not the one of the last key. */
WfKeyResult wf_app_feed(WfApp *app, WfKeyContext ctx, WfStroke stroke);
/* Time until a half-typed sequence is dropped, and the dropping. */
uint32_t wf_app_keys_timeout(const WfApp *app);
void wf_app_binding_hint(const WfApp *app, bool preview, char *out, size_t cap);
bool wf_app_keys_tick(WfApp *app);
/* A count that was refused, or a command line that could not run. */
void wf_app_help_begin(WfApp *app);
void wf_app_sort_begin(WfApp *app);
void wf_app_cmdline_begin(WfApp *app);
void wf_app_set_sort(WfApp *app, WfSortKey key, bool descending);
/* The name of a sort order, in the language in use. */
const char *wf_app_sort_name(WfSortKey key);
void wf_app_keys_destroy(WfApp *app);
/* Inside an archive: says it is read only and returns true. Every key that
 * would create, change, move, copy, open or edit something asks first. */
bool wf_app_archive_guard(WfApp *app);
void wf_app_history_go(WfApp *app, bool forward);
void wf_app_go_parent(WfApp *app);

/* Move executor (wf_app_xfer.c): called for every edit receipt; true when the
 * receipt belonged to a move and was consumed. */
bool wf_app_move_receipt(
        WfApp *app, const WfEditOutcome *outcome, const wchar_t *name);
/* The failure sentence for a receipt that did not succeed. */
void wf_app_edit_failure_text(const WfEditOutcome *outcome,
        const char *shown_name,
        char *out,
        size_t cap);
/* Enter on a file (wf_app_open.c). */
void wf_app_open_selected(WfApp *app);
/* Open Terminal Here: a new terminal in the folder on screen (wf_app_open.c). */
void wf_app_cmd_terminal(WfApp *app);
void wf_app_open_destroy(WfApp *app);
/* Smart Go (wf_app_go.c): `g` opens it empty; Ctrl+L with the shown folder
 * selected. */
void wf_app_path_prompt_begin(WfApp *app);
/* Ctrl+L: edit the current path, pre-selected, ending in a separator. */
void wf_app_path_bar_begin(WfApp *app);
void wf_app_path_state_destroy(WfApp *app);
/* Navigation tells Smart Go how the listing it asked for turned out (it keeps
 * the typed text and says why when it did not). The first returns true when
 * the listing is to be dropped (the typed file is not in its folder). */
bool wf_app_go_listing_vetoed(WfApp *app, uint32_t gen, const WfDir *dir);
void wf_app_go_listing_failed(WfApp *app, uint32_t gen, WfStatus st);
void wf_app_go_listing_done(WfApp *app, uint32_t gen);
/* i: snapshot the details of the entry under the cursor and show them. */
void wf_app_info_begin(WfApp *app);
/* Read-only operation-history details for Settings > About. */
void wf_app_safety_info(const WfApp *app, WfInfo *info);
/* Create (the form that asks file or folder), create a file / a folder at
 * once, rename, send to the Recycle Bin. Execution always goes through the
 * checked worker or the release gate. `ev` is the key that asked, so that a
 * question can tell when it is released. */
void wf_app_cmd_create(WfApp *app);
void wf_app_cmd_create_kind(WfApp *app, bool file);
void wf_app_cmd_rename(WfApp *app);
void wf_app_cmd_recycle(WfApp *app, const WfEvent *ev);
void wf_app_preview_clear(WfApp *app);

/* Copy, recycle and undo (wf_app_copy.c). */
void wf_app_fmt_bytes(uint64_t b, char *out, size_t cap);
/* Pause / resume the queue, cancel it, undo the last rename, move or copy. */
void wf_app_cmd_pause(WfApp *app);
void wf_app_cmd_cancel_jobs(WfApp *app);
void wf_app_cmd_undo(WfApp *app);
/* Starts the job the confirmed preflight stands for. False when there is no
 * copy engine (the caller then says so). */
bool wf_app_copy_start(WfApp *app, bool hydrate);
/* Extracting items picked in a ZIP (wf_app_extract.c): y inside the archive,
 * p in a folder; the job itself is started by wf_app_extract_start. */
void wf_app_extract_pick(WfApp *app);
bool wf_app_extract_paste(WfApp *app);
void wf_app_extract_drop(WfApp *app);
bool wf_app_extract_start(WfApp *app, WfCollide collide);
/* The running copy as a line of at most `room` cells: state, count, keys and
 * sizes first, the file name in what is left. */
void wf_app_copy_status(const WfApp *app, WfBuf *s, int room);
uint32_t wf_app_copy_timeout(const WfApp *app);
bool wf_app_copy_tick(WfApp *app);
void wf_app_copy_destroy(WfApp *app);
/* Delete key: confirm, then recycle the selection (or the current entry). */
void wf_app_recycle_begin(WfApp *app, const WfEvent *trigger);
void wf_app_undo_free(WfUndoEntry *u);
/* Returns the uid the entry was given (never 0). */
uint32_t wf_app_undo_push(WfApp *app, WfUndoEntry *u);
/* The entry with this uid was undone whole: it goes (no entry: nothing). */
void wf_app_undo_retire(WfApp *app, uint32_t uid);
/* The Undo of a bulk rename (wf_app_batch.c): the same engine, every name the
 * other way round, all or nothing. False (with a word) when it cannot start. */
bool wf_app_batch_undo_begin(WfApp *app, const WfUndoEntry *u);
/* The history (wf_app_joblog.c). A row for a change that was not a job of the
 * copy engine; `undo_uid` 0: it cannot be undone and `why` (a WfMsg) says why. */
void wf_app_hist_add(WfApp *app, const char *head, uint32_t undo_uid, int why);
/* The Undo entry `uid` is gone: undone, or pushed out by newer ones. */
void wf_app_hist_undo_gone(WfApp *app, uint32_t uid, bool undone);
/* A same-volume move of the slot's items into `dest`; also how undo moves
 * items back. Returns false (changing nothing) when it cannot start. */
bool wf_app_move_begin(WfApp *app,
        const WfTransferSlot *slot,
        const wchar_t *dest,
        const WfFileId *dest_id,
        bool is_undo);
/* Theme screen and settings (wf_app_theme.c, wf_app_settings.c). */
void wf_app_onboarding_done(WfApp *app, WfThemeId id);
void wf_app_settings_preview(WfApp *app);
const char *wf_app_name_reason_text(int verdict);
void wf_app_language_text(const WfApp *app, int setting, char *out, size_t cap);
bool wf_app_settings_commit(WfApp *app,
        const WfConfig *draft,
        bool force_theme,
        char *why,
        size_t cap);
/* Shows the lines in app->report in a window that any key closes. */
void wf_app_report_show(WfApp *app);
/* Opens the entry of the current folder with this name in the editor. */
void wf_app_edit_begin_named(WfApp *app, const wchar_t *leaf);
/* The backups of the file under the cursor (<name>~ ... <name>~9): a window
 * that lists them, opens one in the editor, or sends one to the Recycle Bin
 * through the same question and job as Delete. */
void wf_app_backups_begin(WfApp *app);
void wf_app_backups_destroy(WfApp *app);

/* The names `y` takes: the selection of the folder on screen, else the entry under
 * the cursor. False (with a message) when there is none. `names` needs room for the
 * visible entries; they point into the listing. */
bool wf_app_collect_targets(
        WfApp *app, const wchar_t **names, size_t *n, size_t *hidden);

/* Files from the Windows clipboard (V) and to it (Y) */
void wf_app_cmd_clip_paste(WfApp *app);
void wf_app_cmd_clip_copy(WfApp *app);

/* Who has a file open (L, wf_app_lock.c; DR-DRAFT-024) */
typedef enum
{
	WF_LOCKOP_NONE, /* asked by hand: there is nothing to try again */
	WF_LOCKOP_COPY,
	WF_LOCKOP_MOVE, /* to another volume (copy, then the originals) */
	WF_LOCKOP_RECYCLE,
	WF_LOCKOP_RENAME,
	WF_LOCKOP_MOVE_SAME, /* on one volume, item by item */
	WF_LOCKOP_EDITOR
} WfLockOp;
void wf_app_cmd_locks(WfApp *app);
/* The search below the folder (Tab in the filter prompt; wf_app_hits.c). */
void wf_app_search_begin(WfApp *app);
void wf_app_dupes_begin(WfApp *app); /* F: the duplicates below this folder */
void wf_app_hits_destroy(WfApp *app);
void wf_app_git_destroy(WfApp *app);
/* A reload that came too soon after the last request is not lost: it is made when
 * the gap has passed (the last change of a burst is always shown). */
uint32_t wf_app_git_timeout(const WfApp *app);
bool wf_app_git_tick(WfApp *app);
/* Stages `names` of the folder `dir` for copy or move as `y`/`x` do for the
 * listing (the capture is the program's own, by name, in `dir`). False: nothing
 * was started, and the screen says why. */
bool wf_app_stage_names(WfApp *app,
        WfTransferMode mode,
        const wchar_t *dir,
        const wchar_t **names,
        size_t n);
/* Sends the Recycle job of `s` as the answer yes to the Delete question does for
 * the folder on screen (takes the slot on success and on failure). */
bool wf_app_recycle_send(WfApp *app, WfTransferSlot *s);
/* Tabs and places (wf_app_tabs.c). */
void wf_app_cmd_tab_new(WfApp *app);
void wf_app_cmd_tab_close(WfApp *app);
void wf_app_cmd_tab_move(WfApp *app, int by); /* -1 previous, +1 next */
void wf_app_cmd_place_set(WfApp *app);
void wf_app_cmd_place_go(WfApp *app);
void wf_app_tabs_destroy(WfApp *app);
void wf_app_tabs_arrived(WfApp *app);
size_t wf_app_tab_count(const WfApp *app);
/* Draws the strip at (x, 0) in `width` cells: nothing (0) with one tab. Returns
 * the cells used. */
int wf_app_tabs_strip(const WfApp *app, WfFrame *f, int x, int width);
void wf_app_tabs_text(const WfApp *app, char *out, size_t cap);
/* Syntax colours of the preview (wf_app_syntax.c). */
void wf_app_syntax_preview_reset(WfApp *app);
void wf_app_syntax_preview_start(WfApp *app);
/* The classes of bytes [from, from + n) of a preview line: true when coloured. */
bool wf_app_syntax_preview_line(
        const WfApp *app, size_t line, size_t from, size_t n, uint8_t *out);
/* The SHA-256 of the file under the cursor (#; wf_app_hash.c). */
void wf_app_cmd_hash(WfApp *app);
void wf_app_hash_destroy(WfApp *app);
/* Esc: true when a hash was running (and is stopped). */
bool wf_app_hash_stop(WfApp *app);
/* The text for the details window: the digest of `e` if it holds, "computing"
 * while it runs, else false. `out` needs 65 bytes. */
bool wf_app_hash_line(
        const WfApp *app, const WfEntry *e, char *out, size_t cap);
/* The text "SHA-256 <hex>" for the last row of a text preview: true when a
 * digest holds for the file under the cursor (wf_app_render.c). */
bool wf_app_preview_sha_line(const WfApp *app, char *out, size_t cap);
void wf_app_lock_destroy(WfApp *app);
/* A job was accepted: keep what it was told, for a retry. */
void wf_app_lock_note_job(WfApp *app, uint32_t job, const WfCopySpec *spec);
/* A job ended: true when a failure in it was "in use" (the failed operation is
 * then what L opens). The job's note is dropped either way. */
bool wf_app_lock_job_result(WfApp *app, const WfCopyResult *r);
/* A rename failed in use (undo_pending still holds what it was), or a move on
 * one volume stopped at item `next` of the slot. */
void wf_app_lock_rename_failed(WfApp *app, const WfEditOutcome *outcome);
void wf_app_lock_move_failed(
        WfApp *app, const WfEditOutcome *outcome, size_t next);
/* The editor: its save failed in use, and the person chose to look. */
void wf_app_editor_lock_look(WfApp *app);
void wf_app_editor_save_again(WfApp *app);
/* Entry points the lock window uses to put an operation back (each checks
 * everything again; none performs anything by itself). */
bool wf_app_retry_stage_slot(WfApp *app,
        const WfTransferSlot *src,
        const wchar_t *dest,
        WfCopyKind kind,
        WfMsg said);
bool wf_app_path_in_source(
        const wchar_t *path, const wchar_t *dir, const wchar_t *name);
bool wf_app_recycle_retry(WfApp *app, WfTransferSlot *take);
bool wf_app_rename_again(WfApp *app,
        const wchar_t *dir,
        const wchar_t *old_name,
        const wchar_t *new_name,
        const WfFileId *id);
/* Fuzzy search of the names on screen (s; wf_app_fuzzy.c). */
void wf_app_cmd_fuzzy(WfApp *app);
void wf_app_fuzzy_destroy(WfApp *app);
/* The view's guard: the listing is about to change (see wf_view.h). */
void wf_app_fuzzy_guard(void *app);
bool wf_app_fuzzy_redo_due(const WfApp *app);

/* Bulk rename (M; wf_app_batch.c; DR-DRAFT-022) */
void wf_app_cmd_bulk_rename(WfApp *app);
void wf_app_batch_destroy(WfApp *app);
bool wf_app_batch_tick(WfApp *app);
uint32_t wf_app_batch_timeout(const WfApp *app);
/* The planner's word for row `i` in the current language ("" for a row that is
 * fine); `buf` holds it when it has to be made. */
const char *wf_app_batch_row_text(
        const WfBatchPlan *plan, size_t i, char *buf, size_t cap);

/* Folder sizes (D) */
void wf_app_cmd_dirsize(WfApp *app);
void wf_app_dirsize_destroy(WfApp *app);
/* True while a count runs. */
bool wf_app_dirsize_running(const WfApp *app);
/* Esc: stops a running count and keeps what it found; false if none ran. */
bool wf_app_dirsize_stop(WfApp *app);
/* The counted totals are dropped (r, a new count). */
void wf_app_dirsize_forget(WfApp *app);
/* The total counted for a folder entry of this listing. */
bool wf_app_dirsize_total(
        const WfApp *app, const WfEntry *entry, WfSizeUpdate *out);
/* The same in words for the details window: "12,345 bytes (12.05 KiB), 3
 * files, 2 folders", "at least" while it is partial. False: not counted. */
bool wf_app_dirsize_describe(
        const WfApp *app, const WfEntry *entry, char *out, size_t cap);
/* The log of the last jobs (newest first, every failure with its full path),
 * as a window that scrolls. */
void wf_app_joblog_begin(WfApp *app);
void wf_app_retry_note(WfApp *app, uint32_t job, const WfCopySpec *spec);
void wf_app_retry_take(WfApp *app, const WfCopyResult *r, WfJobLogEntry *e);
void wf_app_retry_stage(WfApp *app);
void wf_app_retry_free(WfApp *app);
void wf_app_joblog_add(WfApp *app,
        const char *head,
        WfCopyResult *r,
        uint32_t undo_uid,
        int why);
void wf_app_joblog_destroy(WfApp *app);
const char *wf_app_copyfail_text(WfCopyFailKind k);
/* `e`: edit the file under the cursor (wf_app_editor.c). */
void wf_app_edit_begin(WfApp *app);
/* Debounced directory-preview request: ms until it is due, and the tick that
 * sends it. The tick returns true when the screen has to be redrawn. */
extern const WfMode WF_MODE_IMAGE_PASSWORD;
bool wf_app_image_wanted(const WfApp *app, const wchar_t *name);
void wf_app_image_begin(WfApp *app, const wchar_t *name);
void wf_app_image_clear(WfApp *app);
bool wf_app_image_active(const WfApp *app);
/* True when the picture used the key; false lets the text preview have it. */
bool wf_app_image_command(
        WfApp *app, uint16_t cmd, uint32_t count, bool counted);
bool wf_app_image_draw(
        const WfApp *app, WfFrame *f, int x, int y, int w, int h);
uint32_t wf_app_image_timeout(const WfApp *app);
bool wf_app_image_tick(WfApp *app);

uint32_t wf_app_dirprev_timeout(const WfApp *app);
bool wf_app_dirprev_tick(WfApp *app);
void wf_app_preview_sync(WfApp *app, bool explicit_load);
void wf_app_preview_focus(WfApp *app);
size_t wf_app_preview_scroll_limit(const WfPreview *preview, int height);
/* Columns of the text area (after the line-number gutter) in a view `width`
 * cells wide, and how far the text can be panned in it. */
int wf_app_preview_text_width(const WfPreview *preview, int width);
size_t wf_app_preview_pan_limit(const WfPreview *preview, int width);
/* Markdown preview (DR-DRAFT-025). The layout of the previewed file for a
 * view `width` cells wide, or NULL when the preview is not text of a .md /
 * .markdown file (or there was no memory): the plain text view is shown then.
 * Cached; the cache is dropped when the preview changes. */
struct WfAppMd
{
	WfMdDoc *doc;
	int width;
};
void wf_app_md_create(WfApp *app);
void wf_app_md_destroy(WfApp *app);
void wf_app_md_invalidate(WfApp *app);
const WfMdDoc *wf_app_md_doc(const WfApp *app, int width);
extern const WfMode WF_MODE_PREVIEW_PANE;
void wf_app_draw_preview(
        const WfApp *app, WfFrame *f, int x, int y, int w, int h);

/* Drawing primitives shared by the modal and base-screen renderers. */
#define wf_app_style_bar (wf_theme_styles()->bar)
/* The rule between two columns, the same in the file lists and in settings:
 * one `|` cell wide in the theme's border colour, the columns touching it on
 * both sides. h cells down from (x, y). */
void wf_app_vrule(WfFrame *f, int x, int y, int h);
void wf_app_put_str(WfFrame *f,
        int x,
        int y,
        const char *s,
        const WfStyle *st,
        int max_cells);
int wf_app_put_fit(WfFrame *f,
        int x,
        int y,
        const char *s,
        const WfStyle *st,
        int max_cells);
/* Text being typed (cursor at its end) in `width` cells: shown from its end
 * with "<" in front when it is too long; returns the cell for the cursor. */
int wf_app_put_input_tail(WfFrame *f,
        int x,
        int y,
        const char *s,
        size_t n,
        int width,
        const WfStyle *st);

#endif

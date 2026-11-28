#ifndef WF_EDITOR_H
#define WF_EDITOR_H

#include "wf_eenc.h"
#include "wf_esearch.h"
#include "wf_etext.h"
#include "wf_eundo.h"

/*
 * One text buffer being edited: the text, the cursor and selection, the undo
 * history and the editing operations. This is the whole editing model; keys,
 * prompts and drawing live elsewhere and call into it, so every key binding
 * preset runs the very same operations.
 *
 * Every operation that changes the text goes through one primitive
 * (replace a range by a text) that records exactly what was removed and what
 * was added. That is what makes undo exact, "modified" exact, and failure
 * safe: an operation that cannot complete leaves the text as it was.
 */

typedef struct
{
	unsigned tab_width;
	bool use_spaces;
	bool auto_indent;
	bool case_sensitive; /* default for searches */
	size_t max_bytes;    /* the largest text that may be edited */
	size_t undo_bytes;   /* history budget */
	WfEol new_eol; /* line break of a NEW file; a file that is read keeps
	                  its own, line by line */
} WfEditorOptions;

#define WF_ED_FIND_MAX 256
#define WF_ED_MSG_MAX 96

typedef struct
{
	WfEText text;
	WfUndo undo;
	WfEditorOptions opt;
	WfEncoding enc;
	bool read_only;

	WfPos cur;
	WfPos anchor;
	bool has_anchor; /* a selection exists when the anchor differs */
	bool mark; /* selection started with the mark key: arrows extend */
	size_t want_cells; /* display column kept across vertical moves */
	bool want_valid; /* want_cells is up to date; otherwise measured on use */
	size_t wrap_cells; /* soft-wrap width in cells; 0 = no wrapping */

	char *clip; /* internal clipboard (raw text), owned */
	size_t clip_n;
	bool clip_line; /* the clip is whole lines: paste goes above the cursor */

	/* word completion state */
	char *comp_prefix;
	size_t comp_index;
	size_t comp_start; /* column where the word being completed begins */
	WfPos comp_end;
	size_t comp_len;
	uint64_t comp_gen;

	size_t dirty_from; /* first line changed since the view last looked */

	char msg[WF_ED_MSG_MAX]; /* result of the last operation, for the status */
} WfEditor;

void wf_ed_options_default(WfEditorOptions *o);
WfStatus wf_ed_init(WfEditor *e, const WfEditorOptions *o);
void wf_ed_free(WfEditor *e);

/* Replaces the whole text (the file just read), clears undo and selection and
 * puts the cursor at the start. `utf8` must be valid. */
WfStatus wf_ed_set_text(
        WfEditor *e, const char *utf8, size_t n, WfEncoding enc);
bool wf_ed_modified(const WfEditor *e);
void wf_ed_mark_saved(WfEditor *e);

/* Selection. */
bool wf_ed_has_selection(const WfEditor *e);
void wf_ed_selection(const WfEditor *e, WfPos *from, WfPos *to);
void wf_ed_clear_selection(WfEditor *e);
void wf_ed_select_all(WfEditor *e);
/* Nano's mark: starts a selection at the cursor, or drops it. */
void wf_ed_toggle_mark(WfEditor *e);

typedef enum
{
	WF_MV_LEFT,
	WF_MV_RIGHT,
	WF_MV_UP,
	WF_MV_DOWN,
	WF_MV_LINE_START,
	WF_MV_LINE_END,
	WF_MV_DOC_START,
	WF_MV_DOC_END,
	WF_MV_WORD_LEFT,
	WF_MV_WORD_RIGHT,
	WF_MV_PAGE_UP,
	WF_MV_PAGE_DOWN
} WfMove;

/* `extend`: the selection grows (Shift). `page` is the page size in lines. */
void wf_ed_move(WfEditor *e, WfMove m, bool extend, size_t page);
/* Resumable word scan. Each step examines at most `budget` bytes plus one
 * UTF-8 scalar. It owns no text; callers must discard it when the line changes.
 * The column is provisional until step returns true (including long runs of
 * combining marks, which must never expose a cursor inside a cluster). */
typedef struct
{
	size_t col;
	int kind, phase;
	bool right, cluster;
} WfWordScan;
void wf_ed_word_scan_begin(WfWordScan *scan, size_t col, bool right);
bool wf_ed_word_scan_step(const WfELine *line, WfWordScan *scan, size_t budget);
/* Apply a completed motion, preserving Shift/mark selection semantics. */
void wf_ed_move_to(WfEditor *e, WfPos to, bool extend);
/* 1-based line and column (in characters); clamped to the text. */
void wf_ed_goto(WfEditor *e, size_t line, size_t column);

/* Edits. All fail with WF_E_ACCESS on a read-only buffer, WF_E_RANGE when the
 * text would exceed the size limit, and change nothing when they fail. */
WfStatus wf_ed_type(WfEditor *e, uint32_t cp);
/* Pasted or inserted text: control characters are dropped, breaks follow the
 * file. Replaces the selection. One undo step. */
WfStatus wf_ed_insert_text(WfEditor *e, const char *utf8, size_t n);
/* Explicit range edit. Success clears selection; failure preserves cursor,
 * selection, clipboard and history. Used by modal operators after resolving
 * their range without temporarily moving the real cursor. */
WfStatus wf_ed_replace_range(
        WfEditor *e, WfPos from, WfPos to, const char *utf8, size_t n);
/* Puts a saved text in place of the whole text, byte for byte: control
 * characters and every kind of line break stay as they were saved. One undo
 * step. */
WfStatus wf_ed_restore_text(WfEditor *e, const char *utf8, size_t n);
WfStatus wf_ed_newline(WfEditor *e);
WfStatus wf_ed_backspace(WfEditor *e);
WfStatus wf_ed_delete(WfEditor *e);
WfStatus wf_ed_tab(WfEditor *e, bool back);
WfStatus wf_ed_delete_line(WfEditor *e);
/* Cut/copy the selection, or the current line when nothing is selected. */
WfStatus wf_ed_cut(WfEditor *e);
WfStatus wf_ed_copy(WfEditor *e);
/* Pastes the internal clipboard. */
WfStatus wf_ed_paste(WfEditor *e);
/* Sets the internal clipboard (e.g. from the system clipboard). */
WfStatus wf_ed_set_clip(WfEditor *e, const char *utf8, size_t n, bool lines);
WfStatus wf_ed_undo(WfEditor *e);
WfStatus wf_ed_redo(WfEditor *e);
/* Comments or uncomments the selected lines with `prefix` (e.g. "//"). */
WfStatus wf_ed_toggle_comment(WfEditor *e, const char *prefix);
WfStatus wf_ed_complete_word(WfEditor *e);

/* Search. find selects the match and puts the cursor at its end. */
bool wf_ed_find(WfEditor *e, const WfSearch *s, bool forward, bool *wrapped);
/* Replaces the selection if it is a match of `s`. */
WfStatus wf_ed_replace_selection(
        WfEditor *e, const WfSearch *s, const char *repl, size_t n);
/* Replaces every match in one undo step; *count receives how many. */
WfStatus wf_ed_replace_all(WfEditor *e,
        const WfSearch *s,
        const char *repl,
        size_t n,
        size_t *count);

/* The bracket matching the one at or before the cursor. */
bool wf_ed_match_bracket(const WfEditor *e, WfPos *open, WfPos *close);

/* The cursor's display column and line, 1-based, for the status line. */
size_t wf_ed_cursor_col(const WfEditor *e);

#endif

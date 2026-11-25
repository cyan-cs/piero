#ifndef WF_ETEXT_H
#define WF_ETEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"

/*
 * The editor's text: an array of lines of valid UTF-8, each with the line
 * terminator that ended it in the file.
 *
 * Why lines: the text is edited at the cursor and shown a screenful at a
 * time. Within-line insertions move that line's bytes; inserting line breaks
 * also shifts subsequent line records. Original terminators (LF, CRLF, CR,
 * even mixed) are preserved rather than normalized on save.
 *
 * Invariants (structure and content are checked by wf_et_check):
 *  - n >= 1;
 *  - every line but the last has an eol != NONE; the last always has NONE.
 *    A file ending with a terminator has an extra empty last line
 *    (so "a\n" is {"a" LF} {"" NONE});
 *  - `bytes` is the sum of all content and terminator bytes;
 *  - every line's content is valid UTF-8 without NUL (the caller guarantees
 *    this for what it inserts; positions are on scalar boundaries).
 * An edit that would push `bytes` over `max_bytes` fails with WF_E_RANGE and
 * changes nothing.
 */
typedef enum
{
	WF_EOL_NONE = 0,
	WF_EOL_LF,
	WF_EOL_CRLF,
	WF_EOL_CR
} WfEol;

typedef struct
{
	size_t line; /* 0-based */
	size_t col;  /* byte offset into the line, on a scalar boundary */
} WfPos;

typedef struct
{
	char *s; /* not NUL-terminated */
	size_t len, cap;
	WfEol eol;
} WfELine;

typedef struct
{
	WfELine *lines;
	size_t n, cap;
	size_t bytes;
	size_t max_bytes; /* 0 = unlimited */
	WfEol default_eol; /* for a break typed where the line has no terminator */
	uint64_t gen;      /* bumped by every successful change */
	/* bumped when lines come, go or change their line end: not by an edit
	 * inside one line. What depends only on the lines and their ends (the
	 * LF / CRLF label) is kept for as long as this stays. */
	uint64_t shape_gen;
} WfEText;

size_t wf_eol_len(WfEol e);
const char *wf_eol_bytes(WfEol e);

WfStatus wf_et_init(WfEText *t, WfEol default_eol, size_t max_bytes);
void wf_et_free(WfEText *t);

/* Replaces the content with `s` (valid UTF-8). Each CRLF, LF and lone CR is
 * one line break and is remembered as such. */
WfStatus wf_et_load(WfEText *t, const char *s, size_t n);

int wf_pos_cmp(WfPos a, WfPos b);
WfPos wf_et_end(const WfEText *t);
bool wf_et_valid(const WfEText *t, WfPos p);

/* Inserts `s` at `at`; *end (optional) is where the inserted text ends.
 * normalize = false: line breaks in `s` keep their own terminators (undo,
 * file insert). normalize = true: every break in `s` (CRLF, LF, CR) becomes
 * one break with the terminator of the line it splits, or the default one. */
WfStatus wf_et_insert(WfEText *t,
        WfPos at,
        const char *s,
        size_t n,
        bool normalize,
        WfPos *end);

/* Allocation for the editor model (text, undo). wf_et_fail_nth_alloc(n) makes
 * the n-th allocation from now (0 = the next) and all after it fail, for tests; -1 = off. */
void *wf_et_malloc(size_t n);
void *wf_et_calloc(size_t count, size_t size);
void *wf_et_realloc(void *p, size_t n);
void wf_et_fail_nth_alloc(long n);

/*
 * A copy of lines [first, last], taken before an edit that might fail half
 * way, so the edit can be taken back without allocating (a rollback that
 * itself needs memory is no rollback). wf_et_snap_restore puts the copy back in
 * place of the lines [snap->first, cur_last] that are there now; it cannot
 * fail, because the line array only ever grows and the text it restores has
 * exactly as many lines as it had when the copy was taken. It consumes the
 * copy; wf_et_snap_free drops a copy that is not needed.
 */
typedef struct
{
	WfELine *lines;
	size_t first, n, bytes;
} WfETSnap;

WfStatus wf_et_snap_take(
        const WfEText *t, size_t first, size_t last, WfETSnap *out);
void wf_et_snap_restore(WfEText *t, WfETSnap *snap, size_t cur_last);
void wf_et_snap_free(WfETSnap *snap);

/* Removes [from, to). When `out` is given it receives the removed text as raw
 * bytes (terminators included) in a malloc'd buffer the caller frees. */
WfStatus wf_et_delete(
        WfEText *t, WfPos from, WfPos to, char **out, size_t *out_n);

/* Raw bytes of [from, to) in a malloc'd buffer (free by the caller). */
WfStatus wf_et_read(
        const WfEText *t, WfPos from, WfPos to, char **out, size_t *out_n);

/* Raw size of [from, to) in bytes. */
size_t wf_et_size(const WfEText *t, WfPos from, WfPos to);

/* Cursor steps inside one line: a base scalar plus following width-zero
 * scalars and skin-tone modifiers. This is not full grapheme segmentation;
 * visible special scalars such as ZWJ remain separate steps. */
size_t wf_et_col_prev(const WfELine *l, size_t col);
size_t wf_et_col_next(const WfELine *l, size_t col);
/* A scalar the editor shows as text that replaces it (a control, a bidi or
 * zero-width character: "^A", "<ZWJ>") and not as itself. */
bool wf_et_special_scalar(uint32_t cp);

/* Display columns of l->s[0..col). Tabs advance to the next multiple of `tab`
 * (tab == 0: one cell). Special scalars use their visible replacement's width,
 * such as ^X or <ZWJ>. */
size_t wf_et_cells(const WfELine *l, size_t col, unsigned tab);
/* The byte offset whose display column is the largest not above `cells`. */
size_t wf_et_col_at_cells(const WfELine *l, size_t cells, unsigned tab);

/* Soft wrapping: the byte offsets at which each display row of the line
 * starts, for rows `width` cells wide (a scalar that does not fit moves to
 * the next row; a tab counts from the start of its row). *starts is a
 * malloc'd array the caller frees; the return value is the row count (>= 1),
 * or 0 on allocation failure. width == 0 is treated as 1. A full final row
 * adds an empty display row for the insertion cursor. */
size_t wf_et_wrap(
        const WfELine *l, size_t width, unsigned tab, size_t **starts);

/* True when every invariant holds. For tests and debugging. */
bool wf_et_check(const WfEText *t);

#endif

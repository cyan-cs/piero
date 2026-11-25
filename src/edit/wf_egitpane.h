#ifndef WF_EGITPANE_H
#define WF_EGITPANE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../git/wf_gitport.h"

/*
 * The right-hand pane of the editor that shows what the buffer changes against
 * HEAD (DR-DRAFT-038): read-only, beside the text, never in front of it.
 *
 *  - The base (the file at HEAD) is asked for once, when a file is opened, and
 *    kept; the lines are compared against the buffer's current text, saved or
 *    not.
 *  - A change of the text only arms a deadline. When it has not moved for
 *    WF_GP_DEBOUNCE_MS the idle wake-up posts a copy of the text with the text's
 *    own generation; the answer is shown only if it is for the newest request.
 *    No key waits for any of it.
 *  - The pane exists only when the base is known, the terminal is wide enough
 *    (WF_GP_MIN_COLS) and no Markdown preview takes the place. Anything else, a
 *    failure of Git included, leaves the editor exactly as it is without Git.
 */
#define WF_GP_DEBOUNCE_MS 150u
#define WF_GP_RETRY_MS                                                         \
	1000u /* a diff request that could not be made is made again */
#define WF_GP_MIN_COLS 100
#define WF_GP_TEXT_MAX ((size_t)4 << 20) /* the largest text compared */

typedef enum
{
	WF_GP_NONE, /* not asked (no Git, no file, or not a place Git is asked about) */
	WF_GP_ASKED, /* the base was asked for */
	WF_GP_READY, /* the base is here: the pane is shown */
	WF_GP_OFF /* the answer said there is none: no pane, nothing more asked */
} WfGpState;

typedef struct
{
	uint8_t kind; /* 'H' hunk header, 'C' context, '-' removed, '+' added */
	uint32_t hunk;
	size_t a, b; /* the line of the old / the new text the row shows */
} WfGpRow;

typedef struct
{
	WfGpState state;
	uint64_t id;       /* this buffer, for the answers */
	uint32_t base_gen; /* of the newest request for the base */
	WfGitBlob *base;   /* the file at HEAD (counted) */
	uint32_t diff_gen; /* of the newest request for a diff */
	uint64_t
	        diff_text_gen; /* the text generation that request was made for */
	bool has_diff;
	WfLineDiff diff;
	bool too_many; /* the last diff was past the bounds */
	uint64_t seen; /* the newest text generation noticed */
	bool seen_valid;
	uint64_t due_ms; /* when to post a diff; 0: nothing waits */
	WfGpRow *rows;   /* what the pane shows, built from `diff` */
	size_t nrows;
} WfGitPane;

void wf_gp_free(WfGitPane *p); /* empties it; safe to repeat */

/* The rows of the pane for a diff: each hunk with `context` lines of the new
 * text around it, hunks that are closer than twice that shown as one. WF_E_NOMEM
 * leaves no rows. */
WfStatus wf_gp_build_rows(WfGitPane *p, size_t context);

/* The row to put at the top so that the line `cursor` (0-based, in the new text)
 * is in view: the first row at or after it, with a third of the pane above it. */
size_t wf_gp_top_for(const WfGitPane *p, size_t cursor, size_t height);

#endif

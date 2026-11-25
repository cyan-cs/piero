#ifndef WF_SYNOVERLAY_H
#define WF_SYNOVERLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wf_syntax_abi.h"

/*
 * The colours of a text, as the screen keeps them (DR-DRAFT-033, section 7):
 * the runs the syntax worker answered with, and the text they are of. When the
 * text on screen has changed since (the editor: a key was typed, the answer
 * for it is on its way), the runs still describe the old text. retarget()
 * finds a common byte prefix and suffix; fill() reuses their old colours,
 * shifting suffix offsets and leaving the changed span plain. Multiple edits
 * are treated as one span between that prefix and suffix. No parsing occurs here.
 *
 * Display data only: nothing in here changes a text.
 */
typedef struct
{
	WfSynRun *runs; /* owned */
	uint32_t n;
	char *base; /* owned reference text; NULL: use run offsets without retargeting */
	size_t base_len;
	size_t pre, suf, cur_len; /* the mapping, set by retarget */
	bool active;
} WfSynOverlay;

void wf_synov_clear(WfSynOverlay *ov);
/* Consumes the runs and base text, including on rejection. Both are malloc'd;
 * `runs` may be NULL when n == 0, and `base` is optional.
 * `text_len` is the length of the text the runs are of. Runs that are not what
 * a worker promises (unsorted, overlapping, empty, wrapping, or beyond the text)
 * are not kept: the overlay is then empty and the text is drawn plain. Returns
 * whether the runs were accepted, including an empty set. */
bool wf_synov_set(WfSynOverlay *ov,
        WfSynRun *runs,
        uint32_t n,
        char *base,
        size_t base_len,
        size_t text_len);
/* The text on screen is now `cur`. A no-op for an overlay without a base. */
void wf_synov_retarget(WfSynOverlay *ov, const char *cur, size_t cur_len);
/* The classes of bytes [from, from + len) of the current text into out. */
void wf_synov_fill(
        const WfSynOverlay *ov, size_t from, size_t len, uint8_t *out);

#endif

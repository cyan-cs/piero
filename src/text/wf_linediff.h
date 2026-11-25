#ifndef WF_LINEDIFF_H
#define WF_LINEDIFF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"

/*
 * A line diff of two texts (DR-DRAFT-038), the same for every backend. Text
 * only, no I/O.
 *
 * Lines end at LF. Comparison ignores a trailing CR on each line, including
 * the final line without LF, so CRLF and LF versions compare equally. A
 * last line without a line break is a line like the others (whether the text
 * ends with a break is not reported).
 *
 * The method is Myers' O(ND) greedy with the lines that begin and end both
 * texts taken off first. Each complete input is limited to WF_LD_LINES_MAX
 * lines, checked before prefix/suffix trimming. More than that, or more than
 * WF_LD_EDITS_MAX added plus removed lines, returns WF_E_RANGE.
 */
#define WF_LD_LINES_MAX 200000u
#define WF_LD_EDITS_MAX 1000u

typedef struct
{
	size_t a, an; /* first line and count in the old text (0-based) */
	size_t b, bn; /* first line and count in the new text */
} WfDiffHunk;

typedef struct
{
	size_t off, len; /* bytes of the line, without LF; CR is still in it */
} WfDiffLine;

typedef struct
{
	char *a_text, *b_text; /* the copies the lines point into */
	WfDiffLine *a_line, *b_line;
	size_t a_n, b_n; /* lines in each */
	WfDiffHunk *
	        h; /* in order; a pure insertion has an == 0, a deletion bn == 0 */
	size_t n;
} WfLineDiff;

/* Compares `a` (the old text) with `b`. The texts are copied. WF_E_RANGE as
 * above; WF_E_NOMEM on allocation failure. With valid arguments, a failure
 * leaves an empty result. Invalid arguments return WF_E_INVAL before clearing out. */
WfStatus wf_linediff(
        const char *a, size_t an, const char *b, size_t bn, WfLineDiff *out);
void wf_linediff_free(WfLineDiff *d); /* empties it; safe to repeat */

#endif

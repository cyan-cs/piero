#ifndef WF_BULKNAME_H
#define WF_BULKNAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"

/*
 * The new names of a batch rename, from a pattern (DR-DRAFT-022). Pure.
 *
 * The extension is the part after the last dot of a file (a name that only starts
 * with a dot, and every folder, has none). The pattern makes the stem and the
 * extension is kept, with its case changed if asked. In a template:
 *
 *     {name}    the stem as it is (after find and replace)
 *     {n}       the number, no padding;  {n:4} padded with zeros to 4 digits
 *     {{  }}    a brace
 *
 * Anything else in braces is an error, with the position. Find and replace are
 * literal, case sensitive, and run on the stem before the template.
 */
typedef enum
{
	WF_BULK_CASE_KEEP,
	WF_BULK_CASE_LOWER,
	WF_BULK_CASE_UPPER
} WfBulkCase;

typedef struct
{
	const wchar_t *tmpl; /* NULL: "{name}" */
	const wchar_t *find; /* NULL or empty: nothing is replaced */
	const wchar_t *replace;
	int64_t start; /* the first number */
	int64_t step;  /* added for each item; may be negative */
	WfBulkCase stem_case;
	WfBulkCase ext_case;
} WfBulkPattern;

#define WF_BULK_NAME_MAX 4096u /* longer results are refused, not cut */

/* WF_OK, or WF_E_INVAL with *pos at the unit of the template that is wrong. */
WfStatus wf_bulk_check(const WfBulkPattern *p, size_t *pos);

/* The name for the item at `index` (0-based). `dir`: no extension. A result that
 * cannot be written (a number out of range, a name over WF_BULK_NAME_MAX) is
 * WF_E_RANGE. Validity as a file name is the planner's judgement, not this. */
WfStatus wf_bulk_apply(const WfBulkPattern *p,
        const wchar_t *old_name,
        bool dir,
        size_t index,
        wchar_t **out);

#endif

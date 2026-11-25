#ifndef WF_MDINLINE_H
#define WF_MDINLINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"
#include "wf_markdown.h"

/*
 * The inline half of the Markdown module (DR-DRAFT-025): the text of one
 * block (a paragraph, a heading, a table cell) becomes a run of display
 * units, one per scalar, each with a width and a style. Internal to
 * src/text; wf_markdown.h is the interface.
 */

enum
{
	WF_MDU_SPACE = 1u
	        << 0, /* a space: a place to break, dropped at a break */
	WF_MDU_HARD = 1u << 1 /* a forced line break (not shown) */
};

typedef struct
{
	uint32_t cp;
	uint16_t style;
	uint8_t w; /* display cells, 0..2 */
	uint8_t flags;
} WfMdUnit;

typedef struct
{
	WfMdUnit *u;
	size_t n, cap;
} WfMdUnits;

void wf_mdu_free(WfMdUnits *u);
/* Shrinks capacity to the units in use, or frees an empty list. A failed
 * reallocation leaves the list unchanged. */
void wf_mdu_trim(WfMdUnits *u);
WfStatus wf_mdu_push(WfMdUnits *u, uint32_t cp, uint16_t style, uint8_t flags);
/* Every scalar of the UTF-8 text as one unit (no collapsing of spaces). */
WfStatus wf_mdu_push_text(
        WfMdUnits *u, const char *s, size_t n, uint16_t style);

/* Finds a link reference definition by label (the text between the
 * brackets); true with the address. */
typedef bool (*WfMdRefFn)(
        void *ctx, const char *label, size_t n, const char **dest, size_t *dn);

/* Appends the inline text s[0..n) (lines joined with '\n') as display units.
 * `base` is OR-ed into every style. Once tokenization reaches WF_MD_NODES
 * nodes, the remaining source is plain text. On allocation failure, units
 * already appended to `out` remain there. */
WfStatus wf_md_inline(const char *s,
        size_t n,
        uint16_t base,
        WfMdRefFn ref,
        void *ref_ctx,
        WfMdUnits *out);

/* Reference label comparison form: ASCII lower case with space, tab, CR and
 * LF runs collapsed. Writes at most cap - 1 bytes plus NUL when
 * cap > 0; returns the bytes written. Truncation may split UTF-8. */
size_t wf_md_ref_normalize(const char *s, size_t n, char *out, size_t cap);

#endif

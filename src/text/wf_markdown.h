#ifndef WF_MARKDOWN_H
#define WF_MARKDOWN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "../base/wf_status.h"

/*
 * Markdown as it reads (DR-DRAFT-025): the text of a .md file in, styled and
 * wrapped lines out. A hand-written CommonMark subset (headings, paragraphs,
 * emphasis, code, quotes, lists, rules, links, GFM tables), a pure function:
 * it performs no I/O or command execution. Unrecognized syntax is shown as
 * text. Source size, nesting, references and output have the limits below.
 *
 * The result carries meanings (bold, link, heading level 3...), not colours;
 * src/render/wf_mddraw.c turns them into the active theme's styles.
 */

/* Render limits; wf_md_render also caps input at WF_MD_SRC_MAX bytes. */
#define WF_MD_SRC_MAX (256u * 1024u)
#define WF_MD_SRC_LINES 65536u
#define WF_MD_NEST_MAX 8u
#define WF_MD_TABLE_COLS 32u
#define WF_MD_REFS 256u
#define WF_MD_NODES 65536u
#define WF_MD_OUT_LINES 20000u
#define WF_MD_OUT_BYTES (4u * 1024u * 1024u)
/* Cumulative raw reference destinations resolved before inline expansion.
 * Further references stay literal; this budget also spans table cells. */
#define WF_MD_REF_BYTES (4u * 1024u * 1024u)
#define WF_MD_PREFIX_CELLS 40

enum
{
	WF_MDS_BOLD = 1u << 0,
	WF_MDS_ITALIC = 1u << 1,
	WF_MDS_STRIKE = 1u << 2,
	WF_MDS_CODE = 1u << 3, /* inline code */
	WF_MDS_LINK = 1u << 4, /* the words of a link */
	WF_MDS_URL = 1u << 5,  /* an address shown after them */
	WF_MDS_MARKER = 1u
	        << 6,           /* list bullet, quote bar, table border, rule */
	WF_MDS_QUOTE = 1u << 7, /* inside a block quote */
	WF_MDS_BLOCK = 1u << 8, /* a line of a code block */
	WF_MDS_IMAGE = 1u << 9, /* "[image: alt]" */
	WF_MDS_HEAD_SHIFT = 10  /* three bits: heading level 0..6 */
};
#define WF_MDS_HEAD(style) (((style) >> WF_MDS_HEAD_SHIFT) & 7u)

enum
{
	WF_MDL_CODE = 1u << 0 /* a code block row: the code colour fills it */
};

typedef struct
{
	uint32_t off, len; /* bytes of WfMdDoc.text */
	uint16_t style;
} WfMdRun;

typedef struct
{
	uint32_t run, nruns;
	uint32_t src; /* the line of the source it comes from, 0-based */
	uint16_t flags;
} WfMdLine;

typedef struct
{
	char *text;
	size_t text_len;
	WfMdRun *runs;
	size_t nruns;
	WfMdLine *lines;
	size_t nlines;
	bool truncated; /* a limit omitted source text or reference expansion */
	int width;      /* the width it was laid out for, in cells */
} WfMdDoc;

/* Lays the Markdown in src[0..n) out for `width` cells (clamped to 8..400).
 * Input beyond WF_MD_SRC_MAX is ignored and sets `truncated`. A line break is
 * LF, CRLF or CR; control characters are made visible; bad UTF-8 becomes
 * U+FFFD. The caller owns the result and frees it with wf_md_free.
 * WF_E_INVAL for invalid arguments; WF_E_NOMEM on allocation failure.
 * With a valid `out`, failures leave *out NULL. */
WfStatus wf_md_render(const char *src, size_t n, int width, WfMdDoc **out);
void wf_md_free(WfMdDoc *doc);

/* The first line of the document that comes from source line `src` or a later
 * one (the document's last line when none does); for scrolling two panes
 * together. Lines are in source order, so this is a binary search. */
size_t wf_md_line_for_source(const WfMdDoc *doc, size_t src);

/* Text of line `i` without styles: at most cap - 1 bytes, NUL-terminated
 * when cap > 0. Truncation is by byte and may split UTF-8. Returns the bytes
 * written, or 0 for an absent line or zero capacity. */
size_t wf_md_line_text(const WfMdDoc *doc, size_t i, char *out, size_t cap);

/* True for a file named *.md or *.markdown in any case. */
bool wf_md_name_is_markdown(const wchar_t *name);

#endif

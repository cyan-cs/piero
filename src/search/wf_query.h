#ifndef WF_QUERY_H
#define WF_QUERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"
#include "../fs/wf_dir.h"

/*
 * What to look for below a folder (F4 of DR-DRAFT-019, DR-DRAFT-031). Pure: text in,
 * a verdict per name out; nothing here reads a folder.
 *
 * The text is words and terms separated by spaces (a word in double quotes may
 * hold spaces):
 *
 *     report          the name holds "report", whatever the case (all words must)
 *     "annual report" a word with a space
 *     ext:txt         a file whose extension is txt (also ext:.txt; up to 4: any of them)
 *     size>10k        a file larger than 10 KiB (k, m, g, t, also KiB..; plain bytes)
 *     size<2m         a file smaller than 2 MiB
 *     newer:2026-10-01  modified on or after that day, UTC
 *     older:2026-01-01  modified before that day, UTC
 *
 * Extension, size and the file-only terms never match a folder; words and dates do.
 * Case is folded for the characters where Windows folds it simply (the whole BMP
 * through the system's lower-casing); no normalization.
 */
#define WF_QUERY_WORDS 8
#define WF_QUERY_EXTS 4
#define WF_QUERY_WORD_UNITS 128

typedef struct
{
	wchar_t word[WF_QUERY_WORDS][WF_QUERY_WORD_UNITS]; /* folded */
	size_t n_word;
	wchar_t ext[WF_QUERY_EXTS][16]; /* folded, no dot */
	size_t n_ext;
	bool has_gt, has_lt, has_newer, has_older;
	uint64_t gt, lt;
	WfFileTime newer, older;
} WfQuery;

/* WF_OK, or WF_E_INVAL with *pos at the unit of `text` where the term that is
 * wrong starts. `*q` is only meaningful on WF_OK. */
WfStatus wf_query_parse(const wchar_t *text, WfQuery *q, size_t *pos);

/* No word and no term: it would match everything. */
bool wf_query_empty(const WfQuery *q);

bool wf_query_match(const WfQuery *q,
        const wchar_t *name,
        bool dir,
        uint64_t size,
        WfFileTime mtime);

#endif

#ifndef WF_ESEARCH_H
#define WF_ESEARCH_H

#include "wf_etext.h"

/*
 * Plain-text search inside lines. A pattern never contains a line break, so a
 * match is always inside one line. Case folding is ASCII only: other
 * characters match exactly (a deliberate limit, not a Unicode collation).
 * "Whole word" means the characters just outside the match are not word
 * characters (ASCII letters and digits, '_', and everything outside ASCII).
 * There is no regular-expression engine: patterns are literal text.
 */
typedef struct
{
	const char *pat; /* valid UTF-8, not empty, no CR/LF */
	size_t pat_n;
	bool match_case;
	bool whole_word;
} WfSearch;

typedef struct
{
	WfPos start, end;
} WfMatch;

/* The first match at or after `from` (forward), or the last match that starts
 * before `from` (backward). With `wrap`, the search continues from the other
 * end of the text; *wrapped says whether it did. */
bool wf_es_find(const WfEText *t,
        const WfSearch *s,
        WfPos from,
        bool forward,
        bool wrap,
        WfMatch *out,
        bool *wrapped);

/* Where the non-overlapping matches start, in text order, at most `cap` of them
 * (`out` holds that many). Returns how many. */
size_t wf_es_positions(
        const WfEText *t, const WfSearch *s, size_t cap, WfPos *out);

/* The number of non-overlapping matches (counting stops at `cap`), and the
 * 1-based index of the match that starts at `at` (0 if none does). */
size_t wf_es_count(const WfEText *t,
        const WfSearch *s,
        size_t cap,
        WfPos at,
        size_t *index);

/* The first match in the line that starts at or after `from_col`. The drawing
 * walks a line's matches one after the other with this, so there is no limit on
 * how many a line may have and no array to hold them. */
bool wf_es_next_match(
        const WfELine *l, const WfSearch *s, size_t from_col, size_t *at);
/* The same, but `left` is taken as the byte before `from_col` (-1: none) in
 * place of the line's own: for a whole-word test of a line that is being made
 * again, where what stands before `from_col` is the text that was put there. */
bool wf_es_next_match_after(const WfELine *l,
        const WfSearch *s,
        size_t from_col,
        int left,
        size_t *at);
/* The start offsets of the matches inside one line (at most `cap`). Returns how
 * many were stored. */
size_t wf_es_line_matches(
        const WfELine *l, const WfSearch *s, size_t *starts, size_t cap);

/* True when the pattern is usable (non-empty, no line breaks). */
bool wf_es_ok(const WfSearch *s);

#endif

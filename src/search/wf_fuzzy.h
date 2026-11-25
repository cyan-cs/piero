#ifndef WF_FUZZY_H
#define WF_FUZZY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"
#include "../fs/wf_dir.h"

/*
 * Fuzzy search of the names the listing already holds (DR-DRAFT-023).
 *
 * Pure C17, no Win32 and no threads: the worker and the screen call it.
 * Platform assumption as in the text layer: wchar_t is a UTF-16 unit.
 *
 * Ranking uses a linear subsequence prefilter, dynamic programming in the
 * fzf V2 family, and a bounded heap for the best K matches. Ranking allocates
 * nothing after scratch and result initialization; position reconstruction
 * lazily allocates full matrices on its first call.
 */

#define WF_FZ_QUERY_MAX 64u /* characters (code points) of a query */
#define WF_FZ_NAME_MAX 256u /* characters of a name that are scored */
#define WF_FZ_K_DEFAULT 64u
#define WF_FZ_NO_MATCH INT32_MIN

/* A query, folded once: `cp` holds the characters as they are compared. */
typedef struct
{
	uint32_t cp[WF_FZ_QUERY_MAX];
	size_t n;
} WfFzQuery;

/* False (the query is left empty) when `s` has no characters or more than
 * WF_FZ_QUERY_MAX of them. A lone surrogate is a character of its own. */
bool wf_fz_query_set(WfFzQuery *q, const wchar_t *s, size_t len);

/* The comparison form of one character: simple case folding of ASCII,
 * Latin-1, Latin Extended-A, Greek, Cyrillic, full-width Latin letters and
 * digits (to ASCII) and Katakana (to Hiragana). Everything else is itself.
 * Not Unicode case folding, not normalization: see the decision record. */
uint32_t wf_fz_fold(uint32_t cp);

/* The units (1 or 2) of the character that starts at `at`: 2 only for a
 * complete surrogate pair. */
size_t wf_fz_cp_units(const wchar_t *s, size_t len, size_t at);

/* Scratch for scoring (about 5 KB) and, once a position is asked for, the
 * matrices (80 KB). One scratch serves one thread at a time. */
typedef struct WfFzScratch WfFzScratch;
WfFzScratch *wf_fz_scratch_new(void);
void wf_fz_scratch_free(WfFzScratch *x); /* NULL-safe */

/* Step 1: do the query's characters appear in `s`, in order? */
bool wf_fz_prefilter(const WfFzQuery *q, const wchar_t *s, size_t len);

/* Steps 1 and 2: the score of `s` for `q` (higher is better), or
 * WF_FZ_NO_MATCH. Rolling rows only: O(window) space. */
int32_t wf_fz_score(
        const WfFzQuery *q, const wchar_t *s, size_t len, WfFzScratch *x);

/* The best path for `s`: units[i] is the UTF-16 offset of the start of the
 * character that q->cp[i] matched. Uses the full matrix (allocated on the first
 * call; false if that fails). The score is the one wf_fz_score gives. */
bool wf_fz_positions(const WfFzQuery *q,
        const wchar_t *s,
        size_t len,
        WfFzScratch *x,
        uint32_t *units,
        int32_t *score);

/* The score of one given path, worked out by the plain rules (no matrix): the
 * reference the tests hold the programme to. NO_MATCH when the path is not a
 * valid match (a character that differs, positions not rising). */
int32_t wf_fz_path_score(const WfFzQuery *q,
        const wchar_t *s,
        size_t len,
        WfFzScratch *x,
        const uint32_t *units);

typedef struct
{
	int32_t score;
	uint32_t index; /* the row in the list the search was asked about */
} WfFzHit;

/* The order of results: higher score first; equal scores keep list order. */
bool wf_fz_better(WfFzHit a, WfFzHit b);

typedef struct
{
	WfFzHit *hit; /* owned; a heap whose root is the worst kept hit */
	size_t n, k;
} WfFzTopK;

WfStatus wf_fz_topk_init(WfFzTopK *t, size_t k);
void wf_fz_topk_free(WfFzTopK *t);
/* Keeps the hit if it is among the best K so far. */
void wf_fz_topk_offer(WfFzTopK *t, WfFzHit h);
/* Puts the kept hits in result order (best first). */
void wf_fz_topk_finish(WfFzTopK *t);

typedef bool (*WfFzCancel)(void *ctx); /* true: stop at once */

typedef struct
{
	size_t scanned; /* names looked at */
	size_t passed;  /* ... that passed the prefilter */
	size_t matched; /* ... that scored (the number of matches) */
	bool cancelled;
} WfFzStats;

/* Searches the names of e[vis[0..nvis)] and keeps the best of `top` (which the
 * caller made with its K and which is empty). Asks `cancel` every 32 names. A
 * cancelled search returns WF_OK with stats->cancelled set: what it kept is
 * only the part it had seen. */
WfStatus wf_fz_search(const WfFzQuery *q,
        const WfEntry *e,
        const size_t *vis,
        size_t nvis,
        WfFzScratch *x,
        WfFzTopK *top,
        WfFzCancel cancel,
        void *cancel_ctx,
        WfFzStats *stats);

/* What a request is about. The pointers are borrowed from the view on screen
 * and stay valid until release() returns (the view's guard sees to it). */
typedef struct
{
	const WfEntry *e;
	const size_t *vis;
	size_t nvis;
	WfFzQuery query;
	size_t k;
} WfFzJob;

typedef struct
{
	uint32_t gen; /* the request this answers */
	WfStatus st;
	size_t matched;
	size_t scanned;
	WfFzHit *hit; /* owned, best first */
	size_t nhit;
} WfFzAnswer;

void wf_fz_answer_free(WfFzAnswer *a); /* NULL-safe; frees `hit` */

/*
 * start() asks for a search and replaces a request that has not begun; the
 * running one is cancelled. release() ends every request and returns only
 * when the worker no longer reads the job's memory. The answer comes back
 * through wf_app_on_fuzzy() with the request's generation. A zeroed port
 * means: always search on the screen's thread.
 */
typedef struct
{
	void *ctx;
	WfStatus (*start)(void *ctx, uint32_t gen, const WfFzJob *job);
	void (*release)(void *ctx);
} WfFuzzyPort;

#endif

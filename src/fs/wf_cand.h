#ifndef WF_CAND_H
#define WF_CAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"

/*
 * Candidates for Smart Go: the entries of one folder that start with what was
 * typed, handed over in batches of up to WF_CAND_BATCH. These
 * are names for the screen to show and for Tab to insert.
 */
#define WF_CAND_BATCH 24
#define WF_CAND_NAME 260

typedef struct
{
	wchar_t name[WF_CAND_NAME]; /* the entry's own name, NUL-terminated */
	uint32_t flags;             /* WF_ENT_DIR / _HIDDEN / _LINK */
} WfCandItem;

typedef struct
{
	uint32_t gen; /* the request this answers */
	WfStatus st;
	unsigned long
	        error; /* captured Win32 error when available; 0 otherwise */
	size_t n;      /* items[0..n) */
	WfCandItem items[WF_CAND_BATCH];
	/* The folder has not been read to its end: asking for more may find
	 * more. False once the end was seen. */
	bool more;
	bool first; /* the first batch of the request */
} WfCandBatch;

/*
 * Smart Go (`g`, Ctrl+L). Candidates come in WF_CAND_BATCH-sized batches:
 * start() begins a search of `dir` for names starting with `prefix` and ends
 * any earlier one; more() asks the same search for another batch; cancel()
 * ends it. With `dirs_only`, non-directory entries are filtered before the
 * batch is filled, so each batch contains up to WF_CAND_BATCH folders.
 * Answers come back through wf_app_on_candidates() carrying the
 * generation of the request; the app drops any that are not the newest. A
 * zeroed port means "no folder candidates" (recent folders and aliases still
 * work).
 */
typedef struct
{
	void *ctx;
	WfStatus (*start)(void *ctx,
	        uint32_t gen,
	        const wchar_t *dir,
	        const wchar_t *prefix,
	        bool include_hidden,
	        bool dirs_only);
	WfStatus (*more)(void *ctx, uint32_t gen);
	void (*cancel)(void *ctx);
} WfCandPort;

#endif

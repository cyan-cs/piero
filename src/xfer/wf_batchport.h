#ifndef WF_BATCHPORT_H
#define WF_BATCHPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "../base/wf_status.h"
#include "../fs/wf_dir.h"
#include "wf_batchplan.h"

/*
 * What the screen of the batch rename (wf_app_batch.c) knows of the engine and
 * of the thread that runs it (DR-DRAFT-022, UI). These plain types isolate the
 * port from engine types; wf_batchworker.c converts between them. An absent
 * port (NULL functions) means the key says "not available" and changes nothing.
 */

/* The same values as WfBatchResult and WfBatchPre (the glue checks it at
 * compile time). */
typedef enum
{
	WF_BX_SUCCESS,
	WF_BX_ROLLED_BACK,
	WF_BX_RECOVERY_REQUIRED,
	WF_BX_FAILED_BEFORE
} WfBxResult;

typedef enum
{
	WF_BX_PRE_READY,
	WF_BX_PRE_MISSING,
	WF_BX_PRE_CHANGED,
	WF_BX_PRE_ACCESS,
	WF_BX_PRE_PLAN,
	WF_BX_PRE_LIMITS,
	WF_BX_PRE_UNSUPPORTED,
	WF_BX_PRE_NOMEM
} WfBxPre;

typedef struct
{
	WfBxResult result;
	WfBxPre pre;
	size_t pre_item;
	size_t steps_total, steps_done;
	size_t rolled_back, not_restored;
	size_t failed_step;
	unsigned long error;
	struct
	{
		size_t opens, id_queries, renames, enumerations;
	} calls;
	size_t log_writes, log_flushes;
	uint64_t ms_prepare, ms_journal, ms_rename, ms_rollback;
} WfBxReport;

typedef struct
{
	const wchar_t *path; /* absolute */
	WfFileId expected;   /* what the listing showed */
	const wchar_t *to_name;
} WfBxSource;

struct WfBatchJob; /* the engine's job, opaque here */

/* One finished request of the worker. */
typedef struct
{
	uint32_t gen;
	bool recover; /* a recovery of old journals, not a run */
	WfBxReport rep;
	/* recovery: journals looked at, closed as done, taken back, kept open */
	size_t rec_total, rec_done, rec_rolled_back, rec_open;
} WfBxDone;

typedef struct
{
	void *ctx;
	/* Plan and preflight: reads only, on the caller's thread. A job comes back
	 * whenever memory allows; rep->pre says whether it is ready. */
	struct WfBatchJob *(*prepare)(
	        void *ctx, const WfBxSource *src, size_t n, WfBxReport *rep);
	void (*job_free)(void *ctx, struct WfBatchJob *job);
	bool (*ready)(void *ctx, const struct WfBatchJob *job);
	const WfBatchPlan *(*plan)(void *ctx, const struct WfBatchJob *job);
	/* Runs the job on the worker. WF_OK: the worker owns the job and one
	 * WfBxDone with `gen` will be polled. Anything else: the caller keeps it. */
	WfStatus (*run)(void *ctx, uint32_t gen, struct WfBatchJob *job);
	/* Recovers every unfinished journal of the state folder. */
	WfStatus (*recover)(void *ctx, uint32_t gen);
	void (*cancel)(void *ctx); /* the running job stops at its next step */
	size_t (*progress)(
	        void *ctx); /* steps done so far by the running job */
	size_t (*unfinished)(void *ctx); /* journals that need a look */
} WfBatchPort;

#endif

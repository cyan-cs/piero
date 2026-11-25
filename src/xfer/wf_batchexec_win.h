#ifndef WF_BATCHEXEC_WIN_H
#define WF_BATCHEXEC_WIN_H

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wf_batchlog_win.h"
#include "wf_batchplan.h"

/*
 * The batch rename and same-volume batch move on real folders (DR-DRAFT-022):
 * prepare (the plan and the preflight, reads only), execute (journal, then one
 * rename by handle per step, with rollback), recover (after a crash).
 *
 * No overwrite, no copy, no delete, no other volume. Every rename is
 * `SetFileInformationByHandle(FileRenameInfo)` with ReplaceIfExists = FALSE on a
 * handle whose FileId was compared with the planned one just before and just after.
 */

typedef enum
{
	WF_BATCH_SUCCESS,
	WF_BATCH_ROLLED_BACK,
	WF_BATCH_RECOVERY_REQUIRED,
	WF_BATCH_FAILED_BEFORE_EXECUTION
} WfBatchResult;

/* Why a batch was not made ready (the first item that failed, if one did). */
typedef enum
{
	WF_BATCH_READY,
	WF_BATCH_PRE_MISSING, /* a source or folder is not there */
	WF_BATCH_PRE_CHANGED, /* a source is not the file the listing showed */
	WF_BATCH_PRE_ACCESS, /* it cannot be opened, or the volume is read-only */
	WF_BATCH_PRE_PLAN,   /* the plan refused something: see the rows */
	WF_BATCH_PRE_LIMITS, /* too many items or folders, or an unreadable folder */
	WF_BATCH_PRE_UNSUPPORTED, /* not a local, same-volume, FileId volume */
	WF_BATCH_PRE_NOMEM
} WfBatchPre;

typedef struct
{
	const wchar_t *path; /* the absolute path of the file or folder */
	WfFileId expected; /* the identity the listing showed (must be valid) */
	const wchar_t
	        *to_name; /* rename: the new name; move: NULL keeps the name */
} WfBatchSource;

/* What happened, with the counts of the calls that were made. */
typedef struct
{
	WfBatchResult result;
	WfBatchPre pre;
	size_t pre_item; /* the item behind `pre` */
	size_t steps_total, steps_done;
	size_t rolled_back, not_restored;
	size_t failed_step;
	DWORD error;
	struct
	{
		size_t opens, id_queries, renames, enumerations;
	} calls;
	WfBatchLogStats log;
	uint64_t ms_prepare, ms_journal, ms_rename, ms_rollback;
} WfBatchReport;

/* Test points: called with a word and a step number. A hook may end the process
 * (the crash tests do), or change the disk (the replacement tests do). */
typedef struct
{
	void (*at)(void *ctx, const char *point, size_t step);
	void *ctx;
} WfBatchHooks;

typedef struct WfBatchJob WfBatchJob;

/* Builds the plan and checks the sources and destination without changing them.
 * A job comes back whenever memory allows; `rep->pre` says whether it is ready.
 * `dest_dir`: NULL renames in place; else every item is moved into
 * that folder (it must already exist, on the same volume). */
WfBatchJob *wf_batch_prepare(const WfBatchSource *src,
        size_t n,
        const wchar_t *dest_dir,
        WfBatchReport *rep);
void wf_batch_job_free(WfBatchJob *job);

/* For tests: the next listings of a folder stop with an access error after this
 * many entries (-1: never). */
void wf_batch_fail_listing_after(long names);

bool wf_batch_job_ready(const WfBatchJob *job);
const WfBatchPlan *wf_batch_job_plan(const WfBatchJob *job);
size_t wf_batch_job_items(const WfBatchJob *job);
const wchar_t *wf_batch_job_old_name(const WfBatchJob *job, size_t item);
const wchar_t *wf_batch_job_new_name(const WfBatchJob *job, size_t item);

/* Executes the prepared job. `journal_dir` is the folder for the .pbj file.
 * `cancel` (may be NULL) is checked before each step. Only SUCCESS means the
 * requested changes completed. */
void wf_batch_execute(WfBatchJob *job,
        const wchar_t *journal_dir,
        const WfBatchHooks *hooks,
        const volatile LONG *cancel,
        WfBatchReport *rep);

/* After a crash: decides every step from the disk, rolls back what was done (or
 * closes the journal when all was done) and writes the end. A journal that is
 * damaged or of another version is kept and reported RECOVERY_REQUIRED. */
void wf_batch_recover(const wchar_t *journal_file,
        const WfBatchHooks *hooks,
        WfBatchReport *rep);

/* How many .pbj files of `dir` are unfinished (a plan, no end) or damaged. */
size_t wf_batch_count_unfinished(const wchar_t *dir);

#endif

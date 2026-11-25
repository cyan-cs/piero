#ifndef WF_BATCHLOG_WIN_H
#define WF_BATCHLOG_WIN_H

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"

/*
 * The journal of one batch rename or move (DR-DRAFT-022): an
 * append-only file of lines, each closed by a CRC-32, flushed to the disk at the
 * points the record names. Its format is separate from the copy journal.
 *
 *   PBJ1 <id> <items> <steps>      I <n> <vol> <fileid> <d|f> <path>
 *   S <n> <item> <to path> <0|1>   G   B <n>   D <n>   R <n>   E <state>
 *
 * Paths are written one UTF-16 unit at a time (%uXXXX outside a safe set), so any
 * name an NTFS folder can hold, a lone surrogate included, comes back exactly.
 */

/* What a reader accepts (the planner allows fewer items; steps are at most two per item). */
#define WF_BJ_MAX_ITEMS 20000u
#define WF_BJ_MAX_STEPS 40000u

typedef enum
{
	WF_BJ_SUCCESS,
	WF_BJ_ROLLED_BACK,
	WF_BJ_RECOVERY_REQUIRED
} WfBatchEnd;

typedef struct
{
	uint64_t volume;
	uint8_t id[16];
	bool dir;
	wchar_t *path; /* where it was when the batch was planned */
} WfBjItem;

typedef struct
{
	uint32_t item;
	bool temp;
	wchar_t *to; /* the full path the step renames it to */
} WfBjStep;

typedef enum
{
	WF_BJ_STEP_PLANNED,
	WF_BJ_STEP_BEGUN, /* B written, no D: the disk says whether it happened */
	WF_BJ_STEP_DONE,
	WF_BJ_STEP_RETURNED /* taken back */
} WfBjStepState;

typedef enum
{
	WF_BJ_READ_OK,
	WF_BJ_READ_NO_PLAN, /* no G: nothing was ever changed under it */
	WF_BJ_READ_DAMAGED, /* a bad line with more after it, or an unknown form */
	WF_BJ_READ_UNSUPPORTED /* another version */
} WfBjReadResult;

typedef struct
{
	uint8_t batch[16];
	size_t n_items, n_steps;
	WfBjItem *item;
	WfBjStep *step;
	WfBjStepState *state;
	bool ended;
	WfBatchEnd end;
	bool torn;            /* the last line was cut short and was ignored */
	uint64_t valid_bytes; /* the file up to the end of the last good line */
} WfBatchJournal;

typedef struct WfBatchLog WfBatchLog;

/* Creates `<dir>\<hex id>.pbj`, CREATE_NEW (an existing name is an error). */
WfStatus wf_batchlog_create(
        const wchar_t *dir, const uint8_t batch[16], WfBatchLog **out);
void wf_batchlog_close(WfBatchLog *log);

/* Counts of what the log did, for the measurement. */
typedef struct
{
	size_t writes, flushes;
} WfBatchLogStats;
WfBatchLogStats wf_batchlog_stats(const WfBatchLog *log);

/* The header, every item and step and the G line, then one flush. Nothing may be
 * renamed before this has returned WF_OK. */
WfStatus wf_batchlog_plan(WfBatchLog *log,
        const WfBjItem *item,
        size_t n_item,
        const WfBjStep *step,
        size_t n_step);

/* Opens an existing journal to go on writing: the file is cut at `valid_bytes`
 * (a torn last line goes) and the next line follows the last good one. */
WfStatus wf_batchlog_resume(
        const wchar_t *file, uint64_t valid_bytes, WfBatchLog **out);

/* One line of 'B', 'D' or 'R' for a step, flushed. */
WfStatus wf_batchlog_mark(WfBatchLog *log, char what, size_t step);
/* The same line, written but not flushed. For D and R, which the disk can
 * always restate (recovery looks at the names, not at these lines): the next
 * flush, the next B or the E line, carries them to the disk. */
WfStatus wf_batchlog_mark_lazy(WfBatchLog *log, char what, size_t step);
/* The E line, flushed. */
WfStatus wf_batchlog_end(WfBatchLog *log, WfBatchEnd end);

/* Reads a journal file (the valid lines; see WfBjReadResult). `out` is empty
 * unless the result is OK. */
WfBjReadResult wf_batchlog_read(const wchar_t *file, WfBatchJournal *out);
void wf_batchjournal_free(WfBatchJournal *j);

uint32_t wf_batchlog_crc32(const void *data, size_t n);

#endif

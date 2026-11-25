#ifndef WF_BATCH_UI_H
#define WF_BATCH_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "../fs/wf_dir.h"
#include "../xfer/wf_batchport.h"
#include "../xfer/wf_bulkname.h"

/*
 * The screens of the batch rename (M; DR-DRAFT-022, UI): the pattern, the
 * before/after list, the one question, the run and its result, and the question
 * about journals an earlier run left behind (wf_app_batch.c).
 */

typedef enum
{
	WF_BS_IDLE,
	WF_BS_PATTERN, /* the fields */
	WF_BS_PREVIEW, /* before -> after, with a verdict for every row */
	WF_BS_RUNNING, /* on the worker; Esc asks it to stop */
	WF_BS_RESULT   /* what happened, in words that are true */
} WfBatchStage;

enum
{
	WF_BF_TEMPLATE,
	WF_BF_FIND,
	WF_BF_REPLACE,
	WF_BF_START,
	WF_BF_STEP,
	WF_BF_STEM_CASE,
	WF_BF_EXT_CASE,
	WF_BF_COUNT
};

#define WF_BF_TEXT_MAX 96 /* UTF-16 units of a text field, with the NUL */
#define WF_BF_NUM_MAX 24

/* One item of the batch, as the listing showed it. All of it is owned. */
typedef struct
{
	wchar_t *name; /* the old name */
	wchar_t *path; /* the full path */
	WfFileId id;
	bool is_dir;
	wchar_t *to; /* the new name; NULL for a row that cannot be made */
} WfBatchUiRow;

typedef struct
{
	WfBatchPort port;
	WfBatchStage stage;

	/* The items, as the listing showed them when M was pressed. Owned. */
	size_t n;
	wchar_t *dir;    /* the folder (== cwd then) */
	WfFileId dir_id; /* its identity */
	WfBatchUiRow *item;
	uint32_t nav_gen, cwd_version; /* the listing they were taken from */

	/* The pattern fields. */
	wchar_t f_text[WF_BF_COUNT][WF_BF_TEXT_MAX]; /* the text fields */
	int stem_case, ext_case;                     /* WfBulkCase */
	int field;                                   /* the one being edited */
	int pat_err; /* 0 fine, 1 template, 2 start, 3 step */
	size_t pat_pos;

	/* The preview. */
	struct WfBatchJob *job; /* owned until it is handed to the worker */
	WfBxReport pre;
	size_t top;
	size_t n_renames; /* items that change name */
	size_t n_bad;     /* rows whose name could not be made at all */
	size_t prepares;  /* how many times the plan was made (for tests) */

	/* The run. */
	uint32_t gen;
	bool busy;    /* a request is out to the worker */
	bool recover; /* ... and it is a recovery */
	bool cancelling;
	size_t progress, shown_progress;
	size_t unfinished; /* journals counted at the start */
	bool ask_pending; /* ... and the question about them is still to be asked */
	uint32_t
	        undo_uid; /* the run that is out is the Undo of this entry; 0: no */

	/* The result. */
	WfBxDone done;
	char result[6][160];
	int result_n;
} WfBatch;

#endif

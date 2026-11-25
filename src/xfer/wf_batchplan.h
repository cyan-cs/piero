#ifndef WF_BATCHPLAN_H
#define WF_BATCHPLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"
#include "../fs/wf_dir.h"
#include "../path/wf_name.h"

/*
 * The plan of a batch rename or same-volume batch move (DR-DRAFT-022).
 *
 * Pure: names and identities in, a verdict for every item and an ordered list of
 * steps out. It touches no file and lists no folder; what exists in a folder is
 * given to it (read once by the preflight). Nothing here is permission to act: the
 * execution checks every item again at its own step.
 */

#define WF_BATCH_MAX_ITEMS 20000u
#define WF_BATCH_MAX_PARENTS 64u /* folders held open for the run */
#define WF_BATCH_PATH_MAX                                                      \
	32766u /* the longest target path, as the single rename */

/* A folder that the batch reads names from or writes names into. */
typedef struct
{
	const wchar_t *path; /* absolute, no trailing backslash except a root */
	WfFileId id;
	bool case_sensitive; /* false when it is not known (refuses more, not less) */
	bool link;           /* the folder itself is a reparse point */
	uint32_t component_max;      /* the volume's component limit */
	const wchar_t *const *names; /* every name that exists in it now */
	size_t n_names;
} WfBatchParent;

typedef struct
{
	uint32_t parent;     /* index into the parents: where the item is */
	const wchar_t *name; /* its name now */
	bool dir;
	bool link;   /* a reparse point: refused, as by the single rename */
	WfFileId id; /* the identity the listing showed */
	uint32_t to_parent; /* where it is to be (== parent for a rename) */
	const wchar_t *to_name;
} WfBatchItem;

typedef enum
{
	WF_BP_OK,
	WF_BP_UNCHANGED, /* the new name is the old one: skipped, not an error */
	WF_BP_NAME,      /* see WfBatchRow.name */
	WF_BP_PATH_LONG, /* the target path is longer than WF_BATCH_PATH_MAX */
	WF_BP_DUP_TARGET, /* another item gets the same name in the same folder */
	WF_BP_EXISTS, /* that name exists and nothing in the batch vacates it */
	WF_BP_DUP_ITEM, /* the same file is in the batch twice */
	WF_BP_NESTED, /* inside another item of the batch, or moved into itself */
	WF_BP_VOLUME, /* not on the volume of the others */
	WF_BP_NO_ID,  /* the identity is missing: never authorises a change */
	WF_BP_LINK_PARENT, /* a folder that is a reparse point */
	WF_BP_LINK_ITEM /* a reparse point: the single rename refuses it too */
} WfBatchWhy;

typedef struct
{
	WfBatchWhy why;
	WfNameResult name; /* meaningful with WF_BP_NAME */
	bool temp; /* a cycle goes through this item: it has a temp step */
} WfBatchRow;

/* One name change. A cycle is broken by a step to a temporary name (temp) and a
 * later step of the same item to its final name. */
typedef struct
{
	uint32_t item;
	uint32_t to_parent;
	wchar_t *to_name; /* owned */
	bool temp;
} WfBatchStep;

typedef struct
{
	WfBatchRow *row; /* one per item, owned */
	size_t n;
	WfBatchStep
	        *step; /* owned; empty unless every row is OK or UNCHANGED */
	size_t n_steps;
	size_t n_refused; /* rows that are neither OK nor UNCHANGED */
	size_t n_skipped; /* UNCHANGED */
	size_t n_cycles;  /* cycles broken with a temporary name */
} WfBatchPlan;

/* `batch_id` names the temporary files (".piero-batch-<hex>-<item>"). WF_OK means
 * the plan was made (look at n_refused); an error is memory or a misuse. The
 * input strings are only read, and must outlive the plan's use of them. */
WfStatus wf_batch_plan(const WfBatchParent *parent,
        size_t n_parent,
        const WfBatchItem *item,
        size_t n_item,
        const uint8_t batch_id[16],
        WfBatchPlan *out);
void wf_batch_plan_free(WfBatchPlan *p);

const char *wf_batch_why_text(WfBatchWhy why);

#endif

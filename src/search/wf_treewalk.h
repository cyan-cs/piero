#ifndef WF_TREEWALK_H
#define WF_TREEWALK_H

#include <stdbool.h>
#include <stddef.h>

#include "../base/wf_status.h"
#include "../fs/wf_dir.h"

/*
 * A walk of the folders below one folder, nearest first (DR-DRAFT-031, and F6's
 * duplicate search). Pure: it asks `list` for each folder and tells `visit` about
 * every entry; it holds no file and no handle.
 *
 * It never follows a link (a reparse point is shown as an entry and not entered),
 * never enters a folder whose content is not local (a cloud placeholder), reads each
 * folder once, and is bounded: how many folders wait to be read, how long a path
 * may get. What it could not read is counted, never hidden.
 */
#define WF_WALK_MAX_PENDING 200000u /* folders waiting to be read */
/* The paths of those folders, in bytes: a count alone does not bound the memory
 * (200000 paths of 32000 units are 12 GB). */
#define WF_WALK_MAX_PENDING_BYTES (64u << 20)
#define WF_WALK_MAX_PATH 32000u /* UTF-16 units of the longest path entered */

typedef struct
{
	const wchar_t *dir; /* absolute path of the folder the entry is in */
	WfFileId dir_id; /* its identity, as its parent's listing showed it */
	size_t depth;    /* 0: the root itself */
	const WfEntry *e;
} WfWalkItem;

typedef struct
{
	/* Lists `dir`; WF_OK with *out owned by the walk (it frees it). Any other
	 * answer is that folder not read (counted in `unreadable`) and the walk goes
	 * on, except WF_E_NOMEM, which ends the walk with that status, and
	 * WF_E_CANCELLED, which ends it as cancelled. */
	WfStatus (*list)(void *ctx, const wchar_t *dir, WfDir **out);
	/* One entry. false: stop the walk now (a limit). */
	bool (*visit)(void *ctx, const WfWalkItem *item);
	/* true: stop the walk now (asked from outside). May be NULL. */
	bool (*cancelled)(void *ctx);
	bool hidden; /* hidden and system entries are visited too */
	size_t max_pending_bytes; /* 0: WF_WALK_MAX_PENDING_BYTES; a test may lower it */
} WfWalkOps;

typedef struct
{
	size_t folders;     /* folders read */
	size_t entries;     /* entries visited */
	size_t unreadable;  /* folders that could not be listed */
	size_t partial;     /* folders listed in part (too many entries) */
	size_t not_entered; /* links, cloud folders and folders too deep */
	bool pending_full; /* folders were left unread: the walk is not complete */
	bool cancelled;    /* stopped from outside */
	bool stopped;      /* stopped by visit() */
} WfWalkStats;

/* WF_OK when the walk ran (look at the stats for what it did not do); WF_E_NOMEM
 * (the walk's own, or a listing's: the walk stops there) or WF_E_INVAL otherwise. `root_id` is the identity of the root (may be invalid). */
WfStatus wf_walk(const WfWalkOps *ops,
        void *ctx,
        const wchar_t *root,
        WfFileId root_id,
        WfWalkStats *stats);

#endif

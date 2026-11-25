#ifndef WF_SEARCH_H
#define WF_SEARCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"
#include "../fs/wf_dir.h"
#include "wf_query.h"
#include "wf_treewalk.h"

/*
 * The search below a folder (F4 of DR-DRAFT-019, DR-DRAFT-031): what the worker
 * hands the screen. The UI consumes these result types without calling the
 * enumerator itself.
 *
 * A hit is one entry the query matched, with the folder it is in and the identity
 * the listing showed for it, so that Copy, Move and the Recycle Bin can act on the
 * real path and are refused if the object is no longer that one.
 */
#define WF_SEARCH_MAX_HITS 200000u /* then "at least" */
#define WF_SEARCH_BATCH 256u       /* hits handed over at a time, at most */

/* One folder that holds hits. Shared by its hits: the worker still makes hits of it
 * while the screen is letting go of earlier ones, so the count is atomic. */
typedef struct
{
	wchar_t *path; /* owned: absolute */
	WfFileId id;   /* as the listing of its parent showed it */
	volatile long refs;
} WfHitDir;

typedef struct
{
	WfHitDir *dir; /* a reference */
	wchar_t *name; /* owned: the leaf, raw */
	WfFileId id;
	uint32_t flags; /* WF_ENT_* */
	uint64_t size;
	WfFileTime mtime;
	uint32_t group; /* duplicates: the group (1 up); search: 0 */
	uint32_t depth; /* folders below the root */
} WfHit;

/* A new folder record with one reference (the caller's). NULL: no memory. */
WfHitDir *wf_hitdir_new(const wchar_t *path, WfFileId id);
WfHitDir *wf_hitdir_ref(WfHitDir *d);
void wf_hitdir_unref(WfHitDir *d); /* NULL-safe; the last one frees it */

/* Frees what the hit owns and drops its reference. */
void wf_hit_clear(WfHit *h);

/* One delivery of the worker: hits (in the order found), and at the end the walk's
 * account. */
typedef struct
{
	uint32_t gen;
	WfHit *hit; /* owned */
	size_t n;
	bool done; /* the last batch of this request */
	bool at_least; /* done, and the walk did not finish: limit, a stop or too many folders */
	WfWalkStats walk; /* with done */
	WfStatus st;      /* with done: WF_OK, or why it could not go on */
	/* Duplicates only (the request of start_dupes): how far the reading is, and
	 * with `done` what the comparing came to. */
	bool dupes;
	uint64_t read_done, read_total; /* files read of files to read */
	size_t groups, files_in_groups, skipped;
} WfSearchBatch;

void wf_search_batch_free(WfSearchBatch *b); /* the hits and the batch's own */

/* What the screen asks of the worker. */
typedef struct
{
	void *ctx;
	/* Walks below `root` (an absolute path whose identity is `root_id`) for names that
	 * match `query`; ends any earlier request. */
	WfStatus (*start)(void *ctx,
	        uint32_t gen,
	        const wchar_t *root,
	        WfFileId root_id,
	        const WfQuery *query,
	        bool hidden);
	void (*cancel)(void *ctx);
	/* Walks below `root` for files that hold the same bytes (DR-DRAFT-032); the
	 * hits come in groups (WfHit.group), whole groups at a time; ends any earlier
	 * request. NULL: not offered. */
	WfStatus (*start_dupes)(void *ctx,
	        uint32_t gen,
	        const wchar_t *root,
	        WfFileId root_id,
	        bool hidden);
} WfSearchPort;

/* What the listing saw of a file: the hash is asked for that file, and is only
 * the hash of it if the handle that is read still says so. */
typedef struct
{
	uint64_t size;
	WfFileTime mtime;
	WfFileId id;
} WfSearchSeen;

/* The SHA-256 of the first `limit` bytes of a file (0: all of it) for the duplicate
 * search; `cancel` is asked while it reads. The program backs this with the file
 * hash, a test with its own. A file that is no longer what `seen` says (its
 * identity, size or time) is WF_E_CHANGED: the size alone is not the file. */
typedef WfStatus (*WfSearchHashFn)(const wchar_t *dir,
        const wchar_t *name,
        const WfSearchSeen *seen,
        uint64_t limit,
        bool (*cancel)(void *),
        void *cctx,
        uint8_t digest[32]);

/* How the worker lists one folder (a test stands in for the disk). `cancel` is asked
 * while a big folder is read. */
typedef WfStatus (*WfSearchListFn)(
        const wchar_t *dir, bool (*cancel)(void *), void *cctx, WfDir **out);

#endif

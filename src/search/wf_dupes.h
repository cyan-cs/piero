#ifndef WF_DUPES_H
#define WF_DUPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"
#include "wf_search.h"

/*
 * Files that hold the same bytes (F6 of DR-DRAFT-019, DR-DRAFT-032). Pure: files in,
 * groups out; the bytes are read through `hash`, which the program backs with
 * SHA-256 of a file.
 *
 * Three steps, each only for what the one before could not tell apart: by size (no
 * file is read), by the hash of the first WF_DUPE_PREFIX bytes, by the hash of the
 * whole file (a file no longer than the prefix is already whole). Two files are in
 * one group when their sizes and their SHA-256 are equal. Nothing is chosen for the
 * person: a group is a list of files, never a verdict about which to keep.
 */
#define WF_DUPE_PREFIX 65536u /* bytes read first */
#define WF_DUPE_MAX_FILES                                                      \
	500000u /* files kept for comparing; the walk says it stopped */

typedef struct
{
	WfHitDir *dir; /* a reference, not owned here */
	const wchar_t *name;
	WfFileId id;
	uint64_t
	        size; /* > 0: an empty file is not a duplicate of anything worth keeping */
	WfFileTime mtime;
} WfDupFile;

typedef enum
{
	WF_DUPE_SIZES,       /* grouping by size */
	WF_DUPE_PREFIX_HASH, /* reading the first bytes */
	WF_DUPE_FULL_HASH    /* reading the rest */
} WfDupePhase;

typedef struct
{
	/* The SHA-256 of the first `limit` bytes of the file (0: all of it). WF_OK, or
	 * why not (gone, in use, changed while read, no longer file->size bytes): the
	 * file is then left out. */
	WfStatus (*hash)(void *ctx,
	        const WfDupFile *file,
	        uint64_t limit,
	        uint8_t digest[32]);
	/* A group of equal files (`n` >= 2, in the order given). false: stop now. */
	bool (*group)(void *ctx, const WfDupFile *const *file, size_t n);
	/* How far it is: files read of files to read. May be NULL. */
	void (*progress)(
	        void *ctx, WfDupePhase phase, uint64_t done, uint64_t total);
	bool (*cancelled)(void *ctx); /* May be NULL. */
} WfDupeOps;

typedef struct
{
	size_t candidates; /* files with another file of their size */
	size_t hashed; /* reads made (a file read in two steps counts twice) */
	size_t groups;
	size_t files_in_groups;
	size_t skipped; /* files left out: they could not be read, or changed */
	bool cancelled;
	bool stopped; /* the group callback said stop */
} WfDupeStats;

/* WF_OK when it ran (see the stats); WF_E_NOMEM or WF_E_INVAL otherwise. The files
 * and their strings must stay valid for the call. */
WfStatus wf_dupes_run(const WfDupFile *files,
        size_t n,
        const WfDupeOps *ops,
        void *ctx,
        WfDupeStats *stats);

#endif

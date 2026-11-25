#ifndef WF_COPYJOB_H
#define WF_COPYJOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../config/wf_config.h"
#include "wf_xfer.h"

/*
 * The copy engine's vocabulary: what a job is, how far it has got, and what
 * came of it. Plain data, shared by the engine (wf_copyengine_win.c), the worker
 * that runs it off the UI thread, and the UI that shows it.
 *
 * Product execution permits Copy, cross-volume Move (copy, verify, then the
 * originals go to the Recycle Bin) and Recycle, with the collision choices
 * stop, skip, overwrite (files only; the old file is moved aside by handle and
 * kept until the new one is in place) and keep both. The one thing they share
 * that cannot be closed is the Shell's work on names; see DR-DRAFT-015.
 *  - new files use a fresh temporary and no-clobber publication; configured
 *    verification compares supported content before publication;
 *  - source content is read, never retired by product Copy;
 *  - whatever cannot be carried over (links, online-only files without
 *    consent, encrypted files, large alternate streams) is skipped and said
 *    so, never dropped silently;
 *  - uncertain originals, backups and temporaries are retained for review;
 *    receipt text never authorizes automatic deletion or restoration.
 * What it does not promise: survival of a power cut (the S14 trials were not
 * run), and copying of security descriptors, extended attributes, encryption
 * or compression state (reported as not preserved).
 */

typedef enum
{
	WF_CJ_COPY,
	WF_CJ_MOVE_ACROSS, /* copy, verify, then recycle the sources */
	WF_CJ_RECYCLE,     /* only recycle the items; `dest` is unused */
	WF_CJ_EXTRACT      /* items of a ZIP, written below `dest` as a copy */
} WfCopyKind;

typedef enum
{
	WF_COLLIDE_STOP,      /* an existing name is a failure for that item */
	WF_COLLIDE_SKIP,      /* an existing name is left alone */
	WF_COLLIDE_OVERWRITE, /* an existing file is replaced; folders merge only
	                         with merge_folders */
	WF_COLLIDE_RENAME     /* keep both: the new one gets the next free name,
	                         "name (2).ext"; files and folders alike */
} WfCollide;

/* Product policy, independent of snapshots and operation confirmations. */
bool wf_copyjob_allowed(WfCopyKind kind, WfCollide collide);

typedef struct
{
	WfCopyKind kind;
	WfTransferSlot slot; /* owned clone: the sources as captured */
	wchar_t *dest;       /* owned: absolute folder */
	WfFileId dest_id;
	WfCopyVerify verify;
	WfCollide collide;
	/* COPY with skip, overwrite or keep both: a folder that is already at the
	 * destination is entered and filled, never refused, skipped or renamed
	 * (the files in it follow `collide`, name by name). A file where a folder
	 * goes, or a folder where a file goes, follows `collide` as for any name.
	 * A link at the destination is not entered. Off: folders never merge. */
	bool merge_folders;
	bool hydrate; /* online-only files may be downloaded */
	uint64_t total_bytes;
	size_t total_files;
	/* EXTRACT: the ZIP file (a real path) and the paths inside it that are the
	 * items ("" is the whole archive); the slot is not used. */
	wchar_t *archive;
	wchar_t **inner;
	size_t n_inner;
} WfCopySpec;

void wf_copyspec_free(WfCopySpec *s);
/* Deep copy (the worker keeps its own). */
WfStatus wf_copyspec_clone(WfCopySpec *dst, const WfCopySpec *src);

typedef enum
{
	WF_CS_IDLE,
	WF_CS_RUNNING,
	WF_CS_PAUSED,
	WF_CS_DONE,
	WF_CS_CANCELLED,
	WF_CS_FAILED /* the job could not start or had to stop */
} WfCopyState;

typedef struct
{
	WfCopyState state;
	uint32_t job;
	size_t queued; /* jobs waiting behind this one */
	size_t files_done, files_total;
	uint64_t bytes_done, bytes_total;
	wchar_t current[64]; /* leaf being copied, cut for display */
} WfCopyProgress;

typedef enum
{
	WF_CF_SOURCE_CHANGED,
	WF_CF_SOURCE_UNREADABLE,
	WF_CF_EXISTS,       /* the name is taken (policy STOP, or a folder) */
	WF_CF_DEST_CHANGED, /* the file to replace changed meanwhile */
	WF_CF_DEST_UNSAFE,  /* replacing it would lose something */
	WF_CF_NO_SPACE,
	WF_CF_WRITE_FAILED,
	WF_CF_VERIFY_FAILED, /* read-back differed or could not be done */
	WF_CF_COMMIT_FAILED,
	WF_CF_CLOUD_SKIPPED,
	WF_CF_ENCRYPTED_SKIPPED,
	WF_CF_LINK_SKIPPED,
	WF_CF_TOO_DEEP,
	WF_CF_NOT_REMOVED,    /* MOVE: the source was kept (see error) */
	WF_CF_STAGING_LEFT,   /* an unfinished folder copy could not be fully
	                         taken apart: it stays under a .wfcopy-*.dir name */
	WF_CF_REPLACED_IN_BIN /* the name was taken by another object while the
	                         shell recycled it: that object is in the Recycle
	                         Bin, and the one chosen was not removed */
	        ,
	WF_CF_METADATA_FAILED,
	/* EXTRACT: an entry that was not taken, and why */
	WF_CF_ARCHIVE_NAME,      /* its name is not safe to write */
	WF_CF_ARCHIVE_COLLISION, /* same name as another once case is ignored */
	WF_CF_ARCHIVE_DATA,      /* damaged data, wrong checksum or size */
	WF_CF_ARCHIVE_LIMIT,     /* over a limit of extraction */
	WF_CF_ARCHIVE_UNSUPPORTED, /* a link or a password-protected entry */
	WF_CF_ARCHIVE_CHANGED      /* the archive changed while it was read */
} WfCopyFailKind;

typedef struct
{
	wchar_t name[96]; /* the item's path below the source folder, cut */
	WfCopyFailKind kind;
	unsigned long error;
} WfCopyFailure;

#define WF_CJ_FAILURES 12
/* How many failures a result keeps in full (path, kind, code); the count of
 * the rest is failure_total. */
#define WF_CJ_FAIL_ALL_MAX 4000

typedef struct
{
	wchar_t *path; /* owned: the source folder and the item below it */
	WfCopyFailKind kind;
	unsigned long error;
} WfCopyFailFull;

typedef struct
{
	wchar_t *path; /* owned: what was created at the destination */
	WfFileId id;
	WfFileId
	        parent; /* the folder it is in, as the job held it: a merge makes
		           items at several levels, and Undo proves each parent */
	bool dir;
} WfCopyCreated;

typedef struct
{
	uint32_t job;
	WfCopyKind kind;
	WfCopyVerify verify; /* what THIS job was told to check, not the setting
	                        as it is when the result is read */
	WfCopyState state;
	size_t files_copied, files_skipped, files_failed, dirs_created;
	size_t overwritten;
	uint64_t bytes;
	size_t links_skipped;
	size_t streams_kept, streams_skipped;
	/* Kept is the historical counter name: no verified recycling receipt.
	 * The shell may have mutated paths; it does not prove source survival. */
	size_t sources_recycled, sources_kept;
	size_t sources_untouched, sources_uncertain;
	size_t failure_total;
	WfCopyFailure failures[WF_CJ_FAILURES]; /* the first ones */
	WfCopyCreated *created;                 /* top-level items, for undo */
	size_t created_n;
	/* RECYCLE: item_done[i] is 1 when source i was taken (proven), for the
	 * item_n sources of the job in order; 0 for one that was not, failed or
	 * never reached. item_n is 0 when this could not be kept. */
	unsigned char *item_done;
	size_t item_n;
	WfCopyFailFull *fail_all; /* the first WF_CJ_FAIL_ALL_MAX failures */
	size_t fail_all_n;
	WfFileId dest_id; /* the folder the job wrote into, as it was checked */
	bool extracted;   /* the items came out of an archive (kind is COPY) */
} WfCopyResult;

void wf_copyresult_free(WfCopyResult *r);
const char *wf_copyfail_text(WfCopyFailKind k);

/*
 * Copy, cross-volume Move, Recycle and ZIP extraction jobs.
 * submit() copies and queues the spec (WF_E_RANGE when the queue is full).
 * During normal service every accepted job yields a result; shutdown follows
 * the worker's cancellation and result-lifetime contract. progress() fills the
 * running job's state and returns false when nothing is running or queued.
 * A zeroed port means "no copy engine": the transfer stops after preflight.
 */
typedef struct
{
	void *ctx;
	WfStatus (*submit)(void *ctx, const WfCopySpec *spec, uint32_t *job);
	void (*pause)(void *ctx, bool paused);
	bool (*paused)(void *ctx);
	void (*cancel)(void *ctx, bool all);
	bool (*progress)(void *ctx, WfCopyProgress *out);
} WfCopyPort;

#endif

#ifndef WF_XFER_H
#define WF_XFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"
#include "../fs/wf_dir.h"

/*
 * Transfer Slot (spec 2.4) and batch preflight (spec 7.4, P0-S08/S09/S10).
 *
 * Nothing here mutates the filesystem. All OS access goes through
 * WfProbeOps, so the policy below is plain C17 and is tested against an
 * in-memory fake as well as the real Win32 implementation.
 */

/* Equal only if both are valid and identical. */
bool wf_fileid_equal(const WfFileId *a, const WfFileId *b);
/* The same object by the strong identity, or by the weak one of a FAT volume
 * when both are weak. A strong and a weak identity are never the same. */
bool wf_fileid_equal_or_weak(const WfFileId *a, const WfFileId *b);
/* True when `id` can be relied on for an operation that changes nothing that
 * exists: proven, or weak. */
bool wf_fileid_usable_to_read(const WfFileId *id);

/* lstat-like: describes the object itself, never what a reparse point
 * resolves to. */
typedef struct
{
	WfFileId id;
	uint32_t flags;       /* WF_ENT_* */
	uint32_t reparse_tag; /* with WF_ENT_LINK */
	uint64_t size;
	WfFileTime mtime;
	uint32_t nlinks;
} WfNodeInfo;

typedef struct
{
	wchar_t root[260];   /* volume mount point containing the path */
	wchar_t fs_name[16]; /* "NTFS", "ReFS", "exFAT", ... */
	uint64_t serial; /* fallback volume identity when file IDs are absent */
	uint32_t max_component;
	bool is_local;
	bool read_only;
	bool supports_open_by_id;
	uint64_t free_bytes; /* available to the calling user */
} WfVolInfo;

typedef struct
{
	void *ctx;
	/* WF_E_NOENT if absent; any other error means "unknown" and callers
	 * must fail closed. */
	WfStatus (*node)(void *ctx, const wchar_t *path, WfNodeInfo *out);
	WfStatus (*volume)(void *ctx, const wchar_t *path, WfVolInfo *out);
	/* Identity of the object `path` finally resolves to, FOLLOWING links.
	 * node() describes a link itself, so its ID differs from its target's;
	 * placement checks (destination inside a source, same folder) must use
	 * this, or a junction alias hides the overlap. */
	WfStatus (*resolve_id)(void *ctx, const wchar_t *path, WfFileId *out);
	WfStatus (*list)(void *ctx, const wchar_t *dir, WfDir **out);
	/* Succeeds iff files and subdirectories could be created in `dir`;
	 * must not create anything. */
	WfStatus (*dest_access)(void *ctx, const wchar_t *dir);
} WfProbeOps;

typedef enum
{
	WF_REPARSE_NONE,
	WF_REPARSE_TRANSPARENT, /* CLOUD*, WOF, DEDUP: behave as ordinary files
	                         */
	WF_REPARSE_REDIRECT,    /* symlink, mount point */
	WF_REPARSE_UNKNOWN
} WfReparseClass;

/* The allowlist is fixed in code. Whether each transparent kind actually
 * copies as an ordinary file is a P0 gate (spec 6.3), not assumed here. */
WfReparseClass wf_reparse_classify(uint32_t flags, uint32_t tag);

typedef enum
{
	WF_XFER_NONE,
	WF_XFER_COPY,
	WF_XFER_MOVE
} WfTransferMode;

typedef struct
{
	wchar_t *name;   /* owned; leaf name inside slot.dir */
	WfNodeInfo snap; /* identity and attributes captured at y/x */
} WfSourceSnap;

typedef struct
{
	WfTransferMode mode;
	wchar_t *dir; /* owned; folder the sources live in */
	WfFileId dir_id;
	WfSourceSnap *src; /* owned */
	size_t count;
	size_t hidden_excluded; /* selected but not visible at capture time */
	bool collisions_ok; /* the user accepted names that already exist at the
	                       destination (skip or overwrite): preflight then
	                       does not stop at them */
} WfTransferSlot;

void wf_slot_init(WfTransferSlot *s);
void wf_slot_free(WfTransferSlot *s); /* leaves it empty */
/* `dst` is overwritten, not freed: it must be empty (init'ed or freed). */
WfStatus wf_slot_clone(WfTransferSlot *dst, const WfTransferSlot *src);

/*
 * Snapshots `names` inside `dir`. Fails as a whole if any source cannot be
 * described; `out` is left empty then. Requires n > 0.
 */
WfStatus wf_xfer_capture(const WfProbeOps *ops,
        WfTransferMode mode,
        const wchar_t *dir,
        const wchar_t *const *names,
        size_t n,
        size_t hidden_excluded,
        WfTransferSlot *out);

typedef enum
{
	WF_PF_OK,
	WF_PF_CROSS_VOLUME, /* MOVE only: not a failure, offers Copy instead */
	WF_PF_EMPTY_SLOT,
	WF_PF_SOURCE_MISSING,
	WF_PF_SOURCE_CHANGED,
	WF_PF_SAME_FOLDER,
	WF_PF_COLLISION,
	WF_PF_DEST_INSIDE_SOURCE,
	WF_PF_DEST_NOT_DIR,
	WF_PF_DEST_REPARSE, /* a link on the destination path: unverified
	                       (H-007) */
	WF_PF_DEST_ACCESS,
	WF_PF_VOLUME_UNSUPPORTED,
	WF_PF_REPARSE,
	WF_PF_PATH_TOO_LONG,
	WF_PF_COMPONENT_TOO_LONG,
	WF_PF_NO_SPACE,
	WF_PF_TREE_TOO_LARGE,
	WF_PF_IO_ERROR,
	WF_PF_CANCELLED
} WfPfReason;

typedef struct
{
	/* PROVISIONAL: spec 7.4 says P0-S09 measures the default. Until that
	 * evidence exists this is a deliberately conservative placeholder. */
	uint64_t reserve_bytes;
	/* Longest path (UTF-16 units, extended form, excluding the NUL) the
	 * volume layer will be asked to handle. */
	size_t max_path_units;
} WfPreflightPolicy;

extern const WfPreflightPolicy WF_PREFLIGHT_DEFAULT;

/* Namespace shared with the S04-S06 journal fixture. A final top-level name
 * is replaced by .wfile-txn-<UUID>\payload.bin; descendants keep their tail.
 * All counts exclude NUL and count UTF-16 code units on Windows. */
#define WF_TXN_NAME_UNITS 47u
#define WF_TXN_PAYLOAD_NAME L"payload.bin"
typedef struct
{
	size_t final_units, transaction_units;
} WfTransferPaths;
WfStatus wf_xfer_path_lengths(size_t dest_units,
        size_t relative_units,
        size_t top_name_units,
        WfTransferPaths *out);

typedef struct
{
	WfPfReason reason;
	bool checks_complete; /* only set after every batch check succeeds */
	size_t bad_index;     /* top-level source involved, if any */
	wchar_t bad_name[64]; /* its (possibly truncated) name or the offending
	                         entry */

	size_t top_files, top_dirs,
	        top_links; /* the selected items themselves */
	size_t tree_files, tree_dirs, tree_links; /* whole tree (COPY only) */
	uint64_t total_bytes;                     /* COPY only */
	size_t cloud_files;
	uint64_t cloud_bytes;

	size_t longest_final, longest_txn; /* COPY only; extended-form units */
	size_t max_path_units, max_component; /* limits applied */
	uint64_t free_bytes, need_bytes,
	        reserve_bytes; /* observed free vs configured margin */

	wchar_t fs_name[16];
	bool auto_recovery; /* volume capability only, NEVER ownership proof */
} WfPreflightReport;

typedef struct
{
	void *ctx;
	bool (*is_set)(void *ctx);
} WfCancel;

/*
 * Runs every check of spec 7.4 against `dest` and fills `rep`. Returns
 * WF_OK when a verdict was reached (read rep->reason; only WF_PF_OK means
 * "go"); other statuses are infrastructure failures such as out-of-memory.
 * Any check that cannot be answered counts as a refusal (fail closed).
 * `cancel` may be NULL.
 */
WfStatus wf_preflight_run(const WfProbeOps *ops,
        const WfTransferSlot *slot,
        const wchar_t *dest,
        const WfPreflightPolicy *policy,
        const WfCancel *cancel,
        WfPreflightReport *rep);

const char *wf_pf_reason_text(WfPfReason r);

/* Admission only, not an execution guarantee or a filesystem snapshot.
 * Hydration consent applies to this batch; preflight itself reads no data. */
/* hydration_allowed is deliberately an int: only exactly 1 is consent, so a
 * stray non-zero value can never authorise a download. */
bool wf_preflight_can_start(WfStatus status,
        const WfPreflightReport *report,
        int hydration_allowed);

/*
 * Slot capture and preflight run off the UI thread (they open many files).
 * Both calls queue and return; answers come back through wf_app_on_capture()
 * and wf_app_on_preflight() with the same `gen`. The port copies whatever it
 * needs from its arguments. A zeroed port means "transfers unavailable".
 */
typedef struct
{
	void *ctx;
	WfStatus (*capture)(void *ctx,
	        uint32_t gen,
	        WfTransferMode mode,
	        const wchar_t *dir,
	        const wchar_t *const *names,
	        size_t n,
	        size_t hidden_excluded);
	WfStatus (*preflight)(void *ctx,
	        uint32_t gen,
	        const WfTransferSlot *slot,
	        const wchar_t *dest);
} WfXferPort;

#endif

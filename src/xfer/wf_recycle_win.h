#ifndef WF_RECYCLE_WIN_H
#define WF_RECYCLE_WIN_H

#include <windows.h>

#include "wf_xfer.h"

/*
 * The Shell executor of the Recycle job (Delete, and the removal of the copies
 * an Undo of a copy takes back) and of the source a cross-volume Move retires.
 * wf_copyjob_allowed enables Recycle with the STOP policy and Move
 * (DR-DRAFT-015). The residual risk is recorded there: a path-based IShellItem cannot be
 * atomically bound to the previously checked FILE_ID, so the checks narrow the
 * window and the postchecks can only report a swap after the Shell has acted;
 * they do not prevent it. A success receipt alone is not proof that nothing
 * else was moved.
 *
 * Requests recycling of one file or folder. The request is made with the shell's "recycle, with undo record" flags and
 * without any "answer yes to everything" flag, so if Windows cannot recycle an
 * item (for instance it is larger than the bin allows) it asks the user in its
 * own dialog; the application does not authorize a permanent-delete fallback.
 *
 * Before asking, the object at `path` must still be the one the listing
 * showed (same FILE_ID), it must be on a local fixed disk with a Recycle Bin
 * that is not set to delete immediately, and (measured) not larger than that
 * bin takes; otherwise it is refused (WF_E_ACCESS) and nothing is asked. The calling thread needs COM in a single-threaded
 * apartment (wf_recycle_thread_init).
 */
typedef struct
{
	HRESULT hresult;
	DWORD error; /* captured at the failing Win32 call, never a stale last-error */
	enum
	{
		WF_RP_IDENTITY,
		WF_RP_VOLUME,
		WF_RP_BIN_QUERY,
		WF_RP_BIN_POLICY,
		WF_RP_SHELL,
		WF_RP_RECEIPT
	} phase;
	bool aborted;     /* the shell reports that it did not finish */
	bool still_there; /* the object exists after the call */
	bool swapped; /* the checked object is outside the bin after apparent
	                 shell success; another object's location is unknown */
	bool attempted; /* PerformOperations was called; failure may have mutated paths */
} WfRecycleReport;

/* Non-mutating capability and policy check; failure is not permission to delete.
 * The executor repeats it after pinning the identities. */
WfStatus wf_recycle_preflight(
        const wchar_t *path, const WfFileId *expected, WfRecycleReport *report);

/* Test seam, off in the product: called after the object and its folder were
 * checked and held, right before the shell is asked, so a test can swap the
 * object at that moment. */
extern void (*wf_recycle_test_before_shell)(const wchar_t *path);

/* The Recycle Bin of a volume can take an item of `size` bytes: it is not set
 * to "delete immediately" and the item is not over its capacity. Windows
 * would otherwise offer to delete for good; the application refuses first. */
bool wf_recycle_fits(
        bool nuke_on_delete, uint64_t capacity_bytes, uint64_t size);

bool wf_recycle_thread_init(void);
void wf_recycle_thread_done(void);

/* WF_OK: the object that was checked is in the bin. WF_E_INVAL: not the
 * object that was listed (or its folder is not `parent`), or the shell took a
 * different object than the one checked (report->swapped). The other object's
 * location is unknown. WF_E_ACCESS: capability/policy check refused. Other
 * errors: no verified receipt; the shell may have changed the file system.
 *
 * Both ids are required. The folder is held open without delete sharing, so it
 * cannot itself be renamed or deleted; ancestors are not pinned here. The object is held while the
 * shell works, and afterwards the held object must be the one that is in the
 * bin. The shell takes a name, so this cannot stop a swap of the object inside
 * its folder, only notice it. */
WfStatus wf_recycle(const wchar_t *path,
        const WfFileId *expected,
        const WfFileId *parent,
        WfRecycleReport *report);

/* Rechecks a caller-owned content proof immediately before PerformOperations.
 * The callback and its context are borrowed for this synchronous call only.
 * A false result prevents the shell operation; this is not an atomic path lock. */
WfStatus wf_recycle_guarded(const wchar_t *path,
        const WfFileId *expected,
        const WfFileId *parent,
        bool (*ready)(void *),
        void *context,
        WfRecycleReport *report);

#endif

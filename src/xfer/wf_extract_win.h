#ifndef WF_EXTRACT_WIN_H
#define WF_EXTRACT_WIN_H

#include <windows.h>

#include "wf_copyengine_win.h"

/* Tests only: asked before each block is written; true makes that write fail
 * as a full disk does. */
extern bool (*wf_extract_test_fail_write)(void);

/* Tests only: called with the path of a file that is about to be removed
 * because its data could not be trusted (before the removal). */
extern void (*wf_extract_test_before_discard)(const wchar_t *path_x);

/*
 * Runs one EXTRACT job on the calling thread: the items of a ZIP written below
 * spec->dest as a copy. The archive is read once, front to back, into a hidden
 * staging folder inside the destination (.wfextract-*.dir), by the checked
 * extractor of src/archive: nothing is written at a name the archive chose
 * until it has been checked there. While the archive is read, writes are
 * confined to the staging folder; final output names are unchanged. The staged items are then copied into place by the
 * copy engine itself (spec->collide, folders merged, verified, journalled,
 * registered for Undo as any copy), and the staging folder is taken apart again
 * object by object on the FileIds it was made with.
 *
 * The ZIP is only read. Entries that were not taken (unsafe name, name that
 * differs only in case, link, wrong checksum, over a limit) are in the result
 * as failures; the job is DONE with failures, CANCELLED, or FAILED.
 *
 * Two stages, two guarantees. While the archive is read into the staging folder
 * final output names are unchanged: a cancel, a failure (the archive could not be
 * read, changed, went over a limit of the whole job) or anything in the staging
 * folder that is not whole and could not be removed prevents publication.
 * Staging cleanup is attempted; any leftovers are reported. Once the staged items are
 * being put in place the guarantees are those of any copy: an item is published
 * whole or not at all, one that was published before a cancel stays (and is in
 * the result, registered for Undo), a file that was replaced is not put back by
 * itself, and a copy that was started is finished or cleaned up item by item.
 *
 * The destination is opened and its identity proven before anything is made in
 * it, and held; every folder made in the staging folder is held while files go
 * below it, so that none can be swapped for a link meanwhile.
 *
 * A crash while the archive is read leaves the hidden staging folder in the
 * destination (it is not journalled, and it is never removed without the
 * identity it was made with): the next extraction into that folder names every
 * such folder in its result as a failure. A staging folder that cannot be taken
 * apart in a run that did not crash is named in the same way.
 * Staged files and published copies coexist; the declared size is used for
 * an early disk-space estimate.
 */
WfStatus wf_extract_run(const WfCopySpec *spec,
        const WfCopyControl *control,
        WfCopyJournal *journal,
        WfCopyResult *out);

#endif

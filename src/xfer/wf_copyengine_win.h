#ifndef WF_COPYENGINE_WIN_H
#define WF_COPYENGINE_WIN_H

#include <windows.h>

#include "wf_copyjob.h"
#include "wf_copyjournal_win.h"

/*
 * Runs one copy job to its end on the calling thread. The caller supplies two
 * hooks: checkpoint(), called between chunks, returns false when the job was
 * cancelled (and may block while it is paused); progress() is told how far
 * things have got. Both are optional.
 *
 * The result records job state and per-item outcomes; the
 * return value only says whether the engine itself could run (a failure here
 * means nothing was attempted).
 */
typedef struct
{
	void *ctx;
	bool (*checkpoint)(void *ctx);
	void (*progress)(void *ctx,
	        size_t files_done,
	        uint64_t bytes_done,
	        const wchar_t *current);
} WfCopyControl;

/* Tests only: called in an overwrite after the old file has been moved to its
 * aside name and before the new one takes the name. A crash here can leave the
 * old file at the aside name; journal inventory supports manual recovery. */
extern void (*wf_copyengine_test_after_aside)(
        const wchar_t *dest, const wchar_t *aside);

/* Tests only: when it returns true the identity of a temporary just created
 * is taken as unreadable (the case the engine cannot journal the file in). */
extern bool (*wf_copyengine_test_tmp_id_unreadable)(void);

WfStatus wf_copyengine_run(const WfCopySpec *spec,
        const WfCopyControl *control,
        WfCopyJournal *journal,
        WfCopyResult *out);

#endif

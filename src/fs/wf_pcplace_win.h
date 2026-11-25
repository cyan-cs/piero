#ifndef WF_PCPLACE_WIN_H
#define WF_PCPLACE_WIN_H

#include <stdbool.h>
#include <stddef.h>

#include "../base/wf_status.h"
#include "wf_dir.h"

/*
 * "This PC" as a folder in the file list (path WF_PCROOT, see wf_path.h):
 * one entry for each drive letter in use ("C:") and one for each registered WSL
 * distribution ("wsl:Ubuntu"), all of them folders. It only asks Windows which
 * letters exist and reads the registry list of distributions: no drive is
 * opened, no distribution or wsl.exe is started. The entries carry no file
 * identity, so no operation that changes files is ever allowed here.
 */
WfStatus wf_pcplace_list(size_t limit, WfDir **out);

/* Tests: use this bit mask of drive letters (bit 0 is A:) instead of
 * GetLogicalDrives, and `names` (NULL-terminated) instead of the registry. NULL
 * and 0 with `use` false put the real ones back. */
void wf_pcplace_test_source(
        bool use, unsigned long drives, const wchar_t *const *names);

#endif

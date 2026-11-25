#ifndef WF_PROBE_WIN_H
#define WF_PROBE_WIN_H

#include "wf_xfer.h"

/*
 * The real WfProbeOps. Everything is read-only:
 *  - node():  opens the object itself (FILE_FLAG_OPEN_REPARSE_POINT, attribute
 *             access only, full sharing) so a link is described, not followed.
 *  - volume(): GetVolumePathNameW + GetVolumeInformationW +
 * GetDiskFreeSpaceExW.
 *  - list():  wf_enum_dir_win.
 *  - dest_access(): opens the directory asking for FILE_ADD_FILE and
 *             FILE_ADD_SUBDIRECTORY; success proves the rights, nothing is
 * created. Stateless and thread-safe.
 */
WfProbeOps wf_probe_win(void);

#endif

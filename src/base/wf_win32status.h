#ifndef WF_WIN32STATUS_H
#define WF_WIN32STATUS_H

#include <windows.h>

#include "wf_status.h"

/*
 * The classes a listing, a probe or a walk tells apart, from the Win32 error
 * of the call that failed: refused (access, a file in use), not there (a name,
 * a path, a network name, a drive), and anything else. Out of memory is not
 * mapped here: it is a different failure, reported where it is seen.
 *
 * A module that needs another class keeps its own mapping, and says why (the
 * editor and the launcher read more codes; the workers that list a path that
 * must be a folder also take ERROR_DIRECTORY as "not there"). Changing what a
 * code maps to changes the error a person is shown.
 */
static inline WfStatus wf_status_from_win32(DWORD error)
{
	switch (error) {
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION: return WF_E_ACCESS;
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_BAD_NETPATH:
	case ERROR_BAD_NET_NAME:
	case ERROR_INVALID_DRIVE:     return WF_E_NOENT;
	}
	return WF_E_OS;
}

#endif

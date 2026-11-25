#ifndef WF_STATE_DIR_H
#define WF_STATE_DIR_H

#include <windows.h>

#include "wf_status.h"

/*
 * Which folder holds the program's state under %APPDATA% or %LOCALAPPDATA%.
 * Rule (DR-DRAFT-017): "piero" if it exists, else the older "wfile" if it
 * exists, else "piero" is to be created. Classifying only looks; nothing is
 * copied, moved, created or deleted. A name that exists but is not a folder,
 * or cannot be examined, is an error: the caller must not guess.
 */
typedef enum
{
	WF_STATE_FRESH,  /* neither exists: use piero (to be created) */
	WF_STATE_PIERO,  /* only piero exists */
	WF_STATE_LEGACY, /* only wfile exists: keep using it */
	WF_STATE_BOTH /* both exist: piero is used, wfile may hold old records */
} WfStateKind;

/* `base` is the absolute path of the folder that holds both candidates.
 * On failure *error is the Win32 error (0 when the cause is not Win32). */
WfStatus wf_state_classify(
        const wchar_t *base, WfStateKind *kind, DWORD *error);

/* The folder name to use for `kind`: L"wfile" for WF_STATE_LEGACY, else
 * L"piero". */
const wchar_t *wf_state_name(WfStateKind kind);

/* The older folder's name, for reading records left there. */
const wchar_t *wf_state_legacy_name(void);

#endif

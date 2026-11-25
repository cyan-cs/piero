#ifndef WF_SMARTGO_ENV_WIN_H
#define WF_SMARTGO_ENV_WIN_H

#include "wf_smartgo_path.h"

/*
 * Smart Go's view of the machine: the user's profile folder
 * (FOLDERID_Profile), the known folders behind the @aliases (Desktop,
 * Documents and the others move with OneDrive, so they are asked of the
 * shell, never built from strings), and environment variables. Answers are
 * looked up each time; the shell is only asked for a folder the user typed.
 */
WfSmartEnv wf_smartgo_env_win(void);

#endif

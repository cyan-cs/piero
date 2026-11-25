#ifndef WF_LAUNCH_H
#define WF_LAUNCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wf_dir.h"

/*
 * Opening a file with its associated program (design 8.3). Pure data and the
 * extension classification; the Win32 side is wf_launch_win.c.
 */

/* One open request. Strings are borrowed by submit(), which copies them. */
typedef struct
{
	const wchar_t *path; /* absolute path of the file */
	WfFileId id;         /* identity the listing observed for it */
	const wchar_t
	        *directory; /* lpDirectory handed to the shell: the neutral
		               directory or the file's folder */
	bool terminal;      /* not a file: a new terminal in `directory` (then
	                       `path` and `id` are not used), wf_terminal_win.h */
} WfLaunchRequest;

typedef struct
{
	WfStatus st;
	unsigned long error; /* GetLastError() / SE_ERR_* at the failure, 0 if
	                        none */
} WfLaunchOutcome;

typedef struct
{
	void *ctx;
	/* Accepts one request at a time; the answer arrives through
	 * wf_app_on_launch() with the same `gen`. */
	WfStatus (*submit)(void *ctx, uint32_t gen, const WfLaunchRequest *req);
} WfLaunchPort;

/* Whether the extension policy requires confirmation. Trailing dots and
 * spaces are stripped before checking the last extension against QUIET in
 * wf_launch_class.c. Missing/unknown extensions require confirmation.
 * The policy does not inspect content or the registered handler. */
bool wf_launch_needs_confirm(const wchar_t *name);

#endif

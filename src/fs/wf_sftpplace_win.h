#ifndef WF_SFTPPLACE_WIN_H
#define WF_SFTPPLACE_WIN_H

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"
#include "wf_dir.h"

/*
 * An SFTP place as a folder in the file list, read-only.
 *
 * Its path is  \\sftp.invalid\<ssh host name>\a\b  : a name Windows can never
 * resolve (so no path function that is handed it by mistake can reach a real
 * machine), the host name exactly as the user's own ssh configuration knows
 * it, and the remote POSIX path with '\' for '/'. The host name root alone is
 * the server's "/".
 *
 * One session (one ssh.exe child) is kept for the host last listed and
 * dropped for another host, on any protocol fault, or at wf_sftp_place_close.
 * The caller is the one listing worker thread; calls are serialised by a lock
 * anyway. Every wait has a time limit; a failed listing makes at most ONE
 * fresh connection attempt (no retry loop). Nothing is ever written; a host
 * key that is unknown or changed stops the connection (see wf_sshcmd.h).
 *
 * Remote names that are not valid UTF-8, or that contain a backslash, cannot
 * be shown as a Windows path component: they are left out and counted in
 * wf_sftp_place_note().
 */
bool wf_sftp_place_is(const wchar_t *path);

/* `cancelled` (may be NULL) is asked while it connects and while it waits for
 * the server, and ends the listing with WF_E_CANCELLED: a request that has been
 * replaced does not hold the worker for a server that is slow or silent. The
 * session is dropped then (a reply that was not read leaves it out of step) and
 * made again by the next listing. */
WfStatus wf_sftp_place_list(const wchar_t *path,
        size_t limit,
        bool (*cancelled)(void *),
        void *cctx,
        WfDir **out);

/* Why the last listing failed (English, one line) or what was left out;
 * empty when there is nothing to say. */
void wf_sftp_place_note(char *out, size_t cap);

/* Reads the whole regular file at `path` (at most `max_bytes` when that is
 * non-zero: WF_E_RANGE above it; WF_E_INVAL for anything but a regular file;
 * a link is not followed). *mtime is whole seconds since 1970. */
WfStatus wf_sftp_place_read(const wchar_t *path,
        size_t max_bytes,
        uint8_t **out,
        size_t *n,
        uint32_t *mtime);

void wf_sftp_place_close(void);

/* Tests: use this program and command line instead of ssh.exe. NULL clears. */
void wf_sftp_place_test_command(const wchar_t *exe, const wchar_t *cmdline);
/* Tests: the limit of every wait, in ms (0: back to the default). */
void wf_sftp_place_test_wait(unsigned ms);

#endif

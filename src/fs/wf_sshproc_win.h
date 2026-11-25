#ifndef WF_SSHPROC_WIN_H
#define WF_SSHPROC_WIN_H

#include <windows.h>

#include <stdbool.h>

#include "../base/wf_status.h"
#include "wf_sftpc.h"

/*
 * The child process behind an SFTP session: ssh.exe (or, in tests, a stand-in)
 * with its standard input and output as the byte stream of wf_sftpc.
 *
 *  - The child gets exactly three handles (stdin, stdout, stderr), nothing
 *    else of piero's; no console window; it is in a job object that kills it
 *    when piero goes away or the session is freed.
 *  - Reads and writes have a time limit and can be cancelled from another
 *    thread (wf_sshproc_cancel), so a hung connection never holds a worker.
 *  - stderr is captured (a bounded amount) for wf_ssh_classify; it is never
 *    shown unfiltered.
 *
 * One thread uses the stream at a time; cancel may come from any thread.
 */
typedef struct WfSshProc WfSshProc;

/* `exe` is the full path of the program, `cmdline` the whole command line
 * (as wf_ssh_command builds it). Nothing is searched for on PATH. */
WfStatus wf_sshproc_start(
        const wchar_t *exe, const wchar_t *cmdline, WfSshProc **out);

/* The stream for wf_sftpc_open; valid until wf_sshproc_free. */
WfSftpStream wf_sshproc_stream(WfSshProc *p);

/* Makes every current and later read/write return WF_E_CANCELLED. */
void wf_sshproc_cancel(WfSshProc *p);

/* Waits up to `ms` for the child to end. True and *code when it has. */
bool wf_sshproc_wait(WfSshProc *p, uint32_t ms, int *code);

/* What the child wrote to stderr so far (NUL-terminated, at most 4 KiB,
 * control characters other than CR/LF/TAB replaced). */
void wf_sshproc_stderr(WfSshProc *p, char *out, size_t cap);

/* Ends the child if it still runs and releases everything. NULL-safe. */
void wf_sshproc_free(WfSshProc *p);

#endif

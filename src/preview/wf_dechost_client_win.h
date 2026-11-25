#ifndef WF_DECHOST_CLIENT_WIN_H
#define WF_DECHOST_CLIENT_WIN_H

#include <windows.h>

#include "../base/wf_status.h"
#include "wf_decode_win.h"

/*
 * The application's side of the decoder host: starts the sandboxed process,
 * sends it one request at a time and brings back the pixels.
 *
 * The host is an AppContainer process with no capabilities; Windows ACLs and
 * explicitly inherited handles still determine accessible objects. Its job
 * kills it when the job handle closes, caps its memory and allows no second
 * process. It is given
 * the one file it must read as a handle duplicated into it. If an AppContainer
 * cannot be had, starting FAILS: there is no weaker host (design I-008).
 *
 * One WfDecHost serves one request at a time and is used from one thread.
 * A host that times out, answers wrongly or dies is marked unusable. The
 * client requests termination and waits up to 5 seconds for process exit;
 * that wait can expire without confirming exit. The image worker stops the
 * old host and starts a fresh one for a later request; decode does not restart it.
 */
typedef struct WfDecHost WfDecHost;

typedef struct
{
	/* The program to run as host (a staged copy of piero.exe). NULL: the
	 * running program, in place; only for tests. */
	const wchar_t *exe;
	/* 1: AppContainer, required in the product. 0: an ordinary process in the
	 * job; for tests that run where an AppContainer cannot be made. */
	int sandbox;
	uint32_t memory_mib; /* job memory limit; 0 = 512 */
} WfDecHostOptions;

typedef enum
{
	WF_DEC_START_VALIDATE,
	WF_DEC_START_PROFILE_CREATE,
	WF_DEC_START_PROFILE_DERIVE,
	WF_DEC_START_PIPES,
	WF_DEC_START_ATTRIBUTES,
	WF_DEC_START_PROCESS,
	WF_DEC_START_JOB,
	WF_DEC_START_RESUME,
	WF_DEC_START_READY,
	WF_DEC_START_TOKEN /* appended: preserve existing diagnostic values */
} WfDecStartPhase;

/* Profile errors are HRESULTs; other startup errors are Win32 codes. */
WfStatus wf_dechost_start(
        const WfDecHostOptions *opt, WfDecHost **out, DWORD *os_error);
/* Same startup path with optional phase diagnostics, no fallback or repair. */
WfStatus wf_dechost_start_with_phase(const WfDecHostOptions *opt,
        WfDecHost **out,
        DWORD *os_error,
        WfDecStartPhase *phase);

/* Ends the host (closing its input is how it learns to stop; a host that
 * does not stop is killed) and frees everything. NULL is fine. */
void wf_dechost_stop(WfDecHost *h);

/* Cached usability flag; 0 after termination is requested or exit is observed.
 * A nonzero value does not itself check whether the process is still running. */
int wf_dechost_alive(const WfDecHost *h);
/* Whether the host's token really is an AppContainer token (asked of the
 * system, not remembered from how it was started). */
int wf_dechost_sandboxed(const WfDecHost *h);
/* The host's process id, for logs and for tests that kill it; 0 if none. */
DWORD wf_dechost_pid(const WfDecHost *h);
/* How the host ended (STILL_ACTIVE while it runs): evidence for a log. */
DWORD wf_dechost_exit_code(const WfDecHost *h);

/*
 * Decodes the file open as `file` (a read handle the caller keeps; it is
 * duplicated into the host, never handed over). `timeout_ms` covers the whole
 * exchange. WF_DSTAT_HOST_LOST and WF_DSTAT_TIMEOUT mean no valid reply came
 * and the host was killed. `box_*` are bounded by WF_DWIRE_BOX_MAX and the
 * reply limit. On WF_DSTAT_OK `*pm` is the picture, owned by the caller.
 *
 * `abandon` (may be NULL) is an event the caller sets when the answer is no
 * longer wanted. The host is then given `abandon_grace_ms` to finish: a reply
 * that arrives in that time is returned as usual (the caller drops it) and the
 * host stays usable; past it the host is killed and the result is
 * WF_DSTAT_ABORTED. Killing at once would restart a host for every cursor
 * move; waiting out the whole timeout would make the next picture queue behind
 * the last one.
 */
WfDecodeStatus wf_dechost_decode(WfDecHost *h,
        WfDecodeKind kind,
        HANDLE file,
        uint32_t box_w,
        uint32_t box_h,
        uint32_t page,
        const wchar_t *password,
        uint32_t timeout_ms,
        HANDLE abandon,
        uint32_t abandon_grace_ms,
        WfPixmap **pm,
        WfDecodeInfo *info);

/*
 * The same, for a file that has no handle: its bytes (`n` of them, at most
 * WF_DWIRE_INPUT_MAX) are sent to the host over the pipe. This is how an
 * archive entry is shown without ever being written to disk.
 */
WfDecodeStatus wf_dechost_decode_bytes(WfDecHost *h,
        WfDecodeKind kind,
        const void *bytes,
        size_t n,
        uint32_t box_w,
        uint32_t box_h,
        uint32_t page,
        const wchar_t *password,
        uint32_t timeout_ms,
        HANDLE abandon,
        uint32_t abandon_grace_ms,
        WfPixmap **pm,
        WfDecodeInfo *info);

/*
 * Copies `source` (NULL: the running program) to %LOCALAPPDATA%\<state folder>\worker\<sha256>\piero.exe
 * (temporary file, flush, hash check, rename that never replaces) and lets
 * ALL APPLICATION PACKAGES read and execute it, so an AppContainer can start
 * it. An existing copy is verified by hash, never overwritten. `out` receives
 * the path.
 */
WfStatus wf_dechost_stage(
        const wchar_t *source, wchar_t *out, size_t cap, DWORD *os_error);

/* The wait left until `deadline` at the time `now` (both GetTickCount64 values):
 * 0 once it has passed. Never a number that wraps (to INFINITE, say). */
DWORD wf_dechost_remaining_ms(ULONGLONG deadline, ULONGLONG now);

#endif

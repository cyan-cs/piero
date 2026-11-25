#ifndef WF_IMAGEWORKER_H
#define WF_IMAGEWORKER_H

#include <windows.h>

#include "../async/wf_worker.h"
#include "wf_image.h"
#include "wf_dechost_client_win.h"

/*
 * The service behind WfImagePort: one thread that owns the sandboxed decoder
 * host, takes the NEWEST request, answers from a small cache when it can and
 * from the host when it cannot.
 *
 *  - latest wins: a newer request abandons the one in flight (see
 *    wf_dechost_decode for the grace), and replaces the one waiting;
 *  - results carry the request's generation, so the UI drops stale ones;
 *  - the file is opened here with the same policy as the text preview (local
 *    volume, no reparse point or cloud placeholder, no redirected ancestor)
 *    and handed to the host as a handle, never as a path;
 *  - the cache key includes the file's identity, size and write time, the
 *    page and the pixel box, so a replaced file or another size never hits;
 *  - a prefetch request fills the cache and delivers nothing, and only runs
 *    when nothing else is waiting.
 *
 * Counters saturate, like every worker here.
 */
typedef struct WfImageWorker WfImageWorker;

typedef struct
{
	uint32_t gen;
	WfDecodeStatus status;
	WfDecodeInfo info;
	WfPixmap *pm; /* owned by whoever polls; NULL unless status is OK */
	uint32_t page, box_w, box_h;
	WfBuf sixel;
	uint32_t sixel_w, sixel_h;
	bool from_cache;
	bool sensitive; /* PDF pixels and encoded bytes are erased before release */
	/* the file was refused by the open policy (a link, a placeholder, a
	 * remote or moved file) rather than failing to open: `os_error` is then
	 * 0. Otherwise a failed open has the Win32 error here. */
	bool refused;
	DWORD os_error;
	/* the host could not be started at all: the reason is isolation, not the
	 * file (shown differently) */
	bool unavailable;
} WfImageResult;

typedef struct
{
	/* The host program: a staged copy of piero.exe. NULL: this program
	 * (tests). */
	const wchar_t *host_exe;
	int sandbox;         /* 1 in the product; 0 only in tests */
	size_t cache_bytes;  /* 0 = 64 MiB */
	uint32_t timeout_ms; /* per request; 0 = 12000 */
	uint32_t grace_ms;   /* given to an overtaken request; 0 = 150 */
} WfImageWorkerOptions;

typedef struct
{
	WfWorkerStats worker;
	uint64_t cache_hits, cache_misses, host_starts, host_kills;
	uint64_t decode_ms_total; /* time spent waiting on the host */
} WfImageWorkerStats;

WfStatus wf_imageworker_start(
        WfImageWorker **out, const WfImageWorkerOptions *options);
WfWorkerStopReport wf_imageworker_stop(WfImageWorker *w);
/* True when nothing is in flight or waiting and no answer waits to be polled:
 * the worker can be stopped without losing an answer. Stopping it ends the
 * decoder host, drops the picture cache and a PDF opened in the host
 * (DR-DRAFT-037). NULL: true. */
bool wf_imageworker_idle(WfImageWorker *w);
WfImageWorkerStats wf_imageworker_stats(WfImageWorker *w);
HANDLE wf_imageworker_event(const WfImageWorker *w);
bool wf_imageworker_poll(WfImageWorker *w, WfImageResult *out);
WfImagePort wf_imageworker_port(WfImageWorker *w);

#endif

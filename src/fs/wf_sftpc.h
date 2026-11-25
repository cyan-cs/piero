#ifndef WF_SFTPC_H
#define WF_SFTPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"
#include "wf_sftp.h"

/*
 * A read-only SFTP v3 client over a byte stream. The stream is anything that
 * can move bytes both ways with a time limit: in the product the pipes of an
 * ssh.exe child (wf_sshproc_win.h), in tests a scripted server. Every call is
 * synchronous, on one thread; list and range-read operations may exchange
 * several requests and replies. The caller runs them on a worker so the
 * screen never waits.
 *
 * After any failure of the stream or of the protocol (a timeout, a short
 * read, a reply that does not parse or answers another request) the client is
 * BROKEN: every later call fails at once and the session has to be dropped
 * and made again, which for ssh means connecting again and checking the host
 * key again. A server error for one request (no such file, permission
 * denied) is not that: the client stays usable and the status is kept in
 * wf_sftpc_server_status().
 *
 * Only reading exists here: list, stat/lstat, read a range and realpath.
 * There is no way to ask this client to write, rename or remove.
 */

typedef struct
{
	void *ctx;
	/* All of `b` is sent, or an error. WF_E_TIMEOUT when the limit passes,
	 * WF_E_CANCELLED when the caller stopped the session. */
	WfStatus (*write)(void *ctx, const uint8_t *b, size_t n, uint32_t ms);
	/* At least one byte, at most `cap`, into *got. *got = 0 with WF_OK is the
	 * end of the stream. WF_E_TIMEOUT / WF_E_CANCELLED as above. */
	WfStatus (*read)(
	        void *ctx, uint8_t *b, size_t cap, size_t *got, uint32_t ms);
} WfSftpStream;

typedef struct WfSftpClient WfSftpClient;

/* Sends INIT and checks VERSION. `ms` limits each complete packet read and
 * each stream write, rather than the whole list or range-read operation. */
WfStatus wf_sftpc_open(
        const WfSftpStream *stream, uint32_t ms, WfSftpClient **out);
/* The same, and `cancelled` (may be NULL) is asked for the life of the client,
 * until wf_sftpc_set_cancel changes it: before each request, and every
 * WF_SFTP_CANCEL_SLICE_MS of a wait for a reply. True ends the call with
 * WF_E_CANCELLED and the client is broken (a reply that was not read leaves the
 * stream out of step, so the session is not used again). */
WfStatus wf_sftpc_open_ex(const WfSftpStream *stream,
        uint32_t ms,
        bool (*cancelled)(void *ctx),
        void *cctx,
        WfSftpClient **out);
void wf_sftpc_set_cancel(
        WfSftpClient *c, bool (*cancelled)(void *ctx), void *cctx);
#define WF_SFTP_CANCEL_SLICE_MS 100u
void wf_sftpc_close(WfSftpClient *c); /* NULL-safe; never closes the stream */

bool wf_sftpc_broken(const WfSftpClient *c);
uint32_t wf_sftpc_server_status(const WfSftpClient *c);
bool wf_sftpc_has_posix_rename(const WfSftpClient *c);
bool wf_sftpc_has_fsync(const WfSftpClient *c);

typedef struct
{
	uint8_t *name; /* owned, raw bytes, valid wf_posix_name_ok */
	size_t name_n;
	WfSftpAttrs attrs;
} WfSftpEntry;

typedef struct
{
	WfSftpEntry *e; /* owned */
	size_t n;
	bool truncated; /* more existed than `limit` */
} WfSftpListing;

void wf_sftp_listing_free(WfSftpListing *l); /* NULL-safe */

/* Lists a directory (no "." or ".."), at most `limit` entries. */
WfStatus wf_sftpc_list(WfSftpClient *c,
        const uint8_t *path,
        size_t path_n,
        size_t limit,
        WfSftpListing *out);

/* lstat (follow = false, a link is reported as a link) or stat. */
WfStatus wf_sftpc_stat(WfSftpClient *c,
        const uint8_t *path,
        size_t path_n,
        bool follow,
        WfSftpAttrs *out);

/* Reads up to `len` bytes at `offset` from a regular file into `buf`; *got
 * may be less at the end of the file. Opens read-only, reads in pieces,
 * closes. The file must be a regular file (checked with lstat first: a link,
 * a device, a pipe or a directory is refused with WF_E_INVAL). */
WfStatus wf_sftpc_read_range(WfSftpClient *c,
        const uint8_t *path,
        size_t path_n,
        uint64_t offset,
        size_t len,
        uint8_t *buf,
        size_t *got);

/* The server's own canonical form of a path (it resolves ".."), into out. */
WfStatus wf_sftpc_realpath(WfSftpClient *c,
        const uint8_t *path,
        size_t path_n,
        uint8_t *out,
        size_t cap,
        size_t *out_n);

#endif

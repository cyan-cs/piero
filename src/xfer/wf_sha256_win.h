#ifndef WF_SHA256_WIN_H
#define WF_SHA256_WIN_H

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>

#include "../fs/wf_dir.h"
#include "wf_hash.h"

/*
 * Streaming SHA-256 (BCrypt), and the hash of a file (DR-DRAFT-027). Reads 256 KiB
 * at a time from one handle opened for reading with full sharing and a sequential
 * hint; never writes; never follows a link; a folder is refused. `cancel` (may be
 * NULL) is asked between reads; `progress` (may be NULL) gets the bytes read so
 * far after each read.
 */
typedef bool (*WfShaCancel)(void *ctx);
typedef void (*WfShaProgress)(void *ctx, uint64_t done, uint64_t total);

#define WF_SHA_BUFFER (256u * 1024u)

/* One hash in pieces (for the tests, and for the file function). */
typedef struct WfSha WfSha;
WfSha *wf_sha_new(void);
bool wf_sha_update(WfSha *s, const void *bytes, size_t n);
bool wf_sha_finish(WfSha *s, uint8_t digest[32]); /* frees `s` in every case */
void wf_sha_free(WfSha *s);

/* The SHA-256 of the first `limit` bytes (0: all; also all when shorter).
 * Uses the whole-file checks; `out->size` is still the size of the file. */
void wf_sha256_file_prefix(const wchar_t *path,
        uint64_t limit,
        WfShaCancel cancel,
        void *cancel_ctx,
        WfShaResult *out);

/* What a listing saw of the file that is now to be read: the file is the one it
 * showed only if the handle that is opened says so. */
typedef struct
{
	uint64_t size;
	uint64_t last_write; /* FILETIME ticks, as the listing gave them */
	WfFileId id; /* neither proven nor weak: none is known, and size and
	                time alone are compared */
} WfShaSeen;

/* The same as wf_sha256_file_prefix, and the file must be what `seen` says: on
 * the handle that is read, the identity, the size and the last write time are
 * compared before one byte is read, and WF_E_CHANGED is the answer when any of
 * them differs. A size that happens to match is not the same file. */
void wf_sha256_file_seen(const wchar_t *path,
        uint64_t limit,
        const WfShaSeen *seen,
        WfShaCancel cancel,
        void *cancel_ctx,
        WfShaResult *out);

void wf_sha256_file(const wchar_t *path,
        WfShaCancel cancel,
        void *cancel_ctx,
        WfShaProgress progress,
        void *progress_ctx,
        WfShaResult *out);

#endif

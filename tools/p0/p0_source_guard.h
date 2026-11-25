#ifndef P0_SOURCE_GUARD_H
#define P0_SOURCE_GUARD_H

#include <windows.h>

#include "../../src/xfer/wf_xfer.h"

typedef enum
{
	P0_GUARD_OK,
	P0_SOURCE_BUSY,
	P0_SOURCE_CHANGED,
	P0_SOURCE_UNSUPPORTED
} P0GuardResult;
typedef struct
{
	HANDLE handle;
	WfFileId identity;
} P0SourceGuard;

/* Independent PoC, never called by the production engine. On success the
 * caller owns a GENERIC_READ/FILE_SHARE_READ handle through copy completion.
 * No directory snapshot, hydration, metadata immutability, or protection of
 * CopyFileExW's later pathname lookup is promised. Close before reusing out. */
P0GuardResult p0_source_guard_acquire(const wchar_t *path,
        const WfNodeInfo *snapshot,
        P0SourceGuard *out,
        DWORD *error);
void p0_source_guard_release(P0SourceGuard *guard);
const char *p0_source_guard_reason(P0GuardResult result);

#endif

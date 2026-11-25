#ifndef WF_FSPORT_H
#define WF_FSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"

/* The producer copies paths before returning. Submission queues work without
 * waiting for enumeration; results carry gen and may be omitted when superseded.
 * parent_path is optional. Production delivers results through its event loop;
 * a test fake may answer synchronously. A zeroed port means unavailable. */
typedef struct
{
	void *ctx;
	WfStatus (*submit)(void *ctx,
	        uint32_t gen,
	        const wchar_t *path,
	        const wchar_t *parent_path);
} WfFsPort;

#endif

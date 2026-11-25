#ifndef WF_SIZE_H
#define WF_SIZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"

/*
 * How much a folder holds (`D`). The totals are names for the screen to show;
 * nothing here opens a file's content or changes anything.
 *
 * Directory entries marked as reparse points are counted as folders without
 * descending into them. Other entries contribute to file count and byte size
 * from the listing. Sizes are logical lengths, not allocated disk space;
 * hard links count once per name. File contents are not read.
 */

/* Entry and elapsed-time limits for one root's count. Checked periodically
 * during enumeration; blocking system calls can delay them. Reaching either
 * limit publishes the totals so far with `partial` set. */
#define WF_SIZE_MAX_ENTRIES 5000000u
#define WF_SIZE_MAX_MS 60000u
/* The most folders one request can ask about. */
#define WF_SIZE_MAX_FOLDERS 100000u

typedef struct
{
	uint32_t gen;                   /* the request this answers */
	size_t index;                   /* which folder of the request */
	uint64_t bytes, files, folders; /* so far, or in all when `finished` */
	bool finished; /* this folder is done; otherwise running totals */
	/* Incomplete: a limit, unreadable subtree or resource failure. Superseded
	 * and cancelled requests stop publishing rather than delivering a final count. */
	bool partial;
	WfStatus
	        st; /* first recorded error; a reached limit may still have WF_OK */
} WfSizeUpdate;

/*
 * start() asks for the totals of `n` folders named `names` inside `dir`, and
 * ends any earlier request; cancel() ends the request. Answers come back
 * through wf_app_on_sizes() carrying the request's generation; the app drops
 * any that are not the newest. A zeroed port means "no folder sizes".
 */
typedef struct
{
	void *ctx;
	WfStatus (*start)(void *ctx,
	        uint32_t gen,
	        const wchar_t *dir,
	        const wchar_t *const *names,
	        size_t n);
	void (*cancel)(void *ctx);
} WfSizePort;

#endif

#ifndef WF_HASH_H
#define WF_HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"

/*
 * The SHA-256 of a file on request (DR-DRAFT-027, `#`). The types here are the
 * ones the screen and the worker share; the hashing is wf_sha256_win.c and the
 * worker is wf_hashworker.c.
 *
 * The digest is a claim about the bytes that were read, so it is only handed out
 * when the file is, by size and last write time, the same before and after it was
 * read, and the number of bytes read is its size. Otherwise there is no digest.
 */
typedef struct
{
	WfStatus st;    /* WF_OK; WF_E_CHANGED: not the same file by the end;
	                   WF_E_CANCELLED; WF_E_ACCESS (a link, no right);
	                   WF_E_NOENT; WF_E_INVAL (a folder); WF_E_OS */
	uint32_t error; /* captured Win32 error when available; 0 for policy failures */
	uint8_t digest[32];  /* meaningful only with WF_OK */
	uint64_t size;       /* the size the file had when it was opened */
	uint64_t last_write; /* its last write time (FILETIME ticks) then */
} WfShaResult;

/* One answer of the worker: progress (finished false) or the end. */
typedef struct
{
	uint32_t gen;
	bool finished;
	uint64_t done, total; /* bytes read so far / the size at the start */
	WfShaResult res;      /* with `finished` */
} WfHashUpdate;

/* start() asks for the hash of `path` and ends any earlier request; cancel() ends
 * the request. A zeroed port means "no hashing". */
typedef struct
{
	void *ctx;
	WfStatus (*start)(void *ctx, uint32_t gen, const wchar_t *path);
	void (*cancel)(void *ctx);
} WfHashPort;

/* 64 lowercase hex digits and a NUL. */
void wf_sha256_hex(const uint8_t digest[32], char out[65]);

#endif

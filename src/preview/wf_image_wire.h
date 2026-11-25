#ifndef WF_IMAGE_WIRE_H
#define WF_IMAGE_WIRE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WF_IMAGE_WIRE_HEADER 40u
#define WF_IMAGE_WIRE_RASTER (16u * 1024u * 1024u)
/* Candidate P0 wire format only. No decoder or product worker uses it yet.
 * Little-endian: WFIM / u32 version / u64 request / u32 width,height,stride,
 * u32 format(1=PBGRA) / u64 payload bytes, followed by exact raster bytes. */
typedef struct
{
	uint64_t request;
	bool accepted;
} WfImageReply;
bool wf_image_reply_accept(
        WfImageReply *state, const void *message, size_t actual_bytes);
#endif

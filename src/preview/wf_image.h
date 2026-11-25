#ifndef WF_IMAGE_H
#define WF_IMAGE_H

#include "wf_decode_wire.h"
#include "wf_pixmap.h"
#include "../base/wf_buf.h"

#define WF_IMAGE_PASSWORD_MAX 256u

/*
 * One picture, or one page of a PDF, asked for in pixels. The service copies
 * `path` and `password` before submit returns (and wipes its copy of the
 * password once used), delivers at most one answer per request through
 * wf_app_on_image() carrying the same `gen`, and drops what a newer request
 * overtakes. A `prefetch` request only warms the service's cache: no answer
 * comes back. A zeroed port means "no picture preview".
 */
typedef struct
{
	uint32_t gen;
	const wchar_t *path;
	WfDecodeKind kind;
	uint64_t
	        session; /* viewing lifetime, distinct from each request generation */
	uint32_t box_w, box_h; /* the picture must fit this many pixels */
	uint32_t page;         /* PDF: 0-based */
	const wchar_t *password;
	bool prefetch;
	uint32_t view_w, view_h, pan_x, pan_y, bg_rgb;
	int quality;
	unsigned max_colors;
	bool sixel;
} WfImageRequest;

typedef struct
{
	void *ctx;
	WfStatus (*submit)(void *ctx, const WfImageRequest *rq);
	void (*cancel)(void *ctx);
} WfImagePort;

/* A delivered answer transfers pm and sixel to its consumer, including when
 * the consumer rejects its generation. Request strings are not borrowed here. */
typedef struct
{
	uint32_t gen;
	WfDecodeStatus status;
	WfDecodeInfo info;
	WfPixmap *pm; /* NULL unless status is WF_DSTAT_OK */
	uint32_t page;
	WfBuf sixel; /* owned encoded viewport, may be empty on safe fallback */
	uint32_t sixel_w, sixel_h;
	/* The file was refused by the open policy, or could not be opened (then
	 * os_error is the Win32 error), or the isolated decoder could not be
	 * started at all: three different things to tell the user. */
	bool refused, unavailable;
	bool sensitive;
	uint32_t os_error;
} WfImageAnswer;

#endif

#ifndef WF_PIXMAP_H
#define WF_PIXMAP_H

#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"

/*
 * A decoded picture as the preview pipeline carries it: one 32-bit pixel per
 * point, premultiplied alpha, laid out 0xAARRGGBB (the memory order B,G,R,A
 * of WIC's 32bppPBGRA and of the worker's wire format). Rows are contiguous
 * (stride = w * 4), so a pixmap is one allocation: header and pixels are not
 * separate objects, and a pixmap never points into someone else's memory.
 *
 * The size is bounded here so that no later size computation can overflow
 * and a hostile header cannot ask for gigabytes: WF_PIXMAP_MAX_PIXELS pixels
 * (64 MiB) at most, either side at most WF_PIXMAP_MAX_SIDE.
 *
 * Ownership: wf_pixmap_new / wf_pixmap_scale return a pixmap the caller frees
 * with wf_pixmap_free (NULL is fine). Nothing here is thread-safe; a pixmap
 * is handed from the worker to the UI thread whole.
 */
#define WF_PIXMAP_MAX_SIDE 16384u
#define WF_PIXMAP_MAX_PIXELS (16u * 1024u * 1024u)

typedef struct
{
	uint32_t w, h;
	uint32_t px[]; /* w * h, row-major, premultiplied 0xAARRGGBB */
} WfPixmap;

/* Zero-filled (transparent). WF_E_INVAL for a zero or oversized side. */
WfStatus wf_pixmap_new(uint32_t w, uint32_t h, WfPixmap **out);
void wf_pixmap_free(WfPixmap *p);
/* Bytes the pixel area of a w x h pixmap takes; 0 if the size is refused. */
size_t wf_pixmap_bytes(uint32_t w, uint32_t h);

/* A copy of `rows` x `cols` premultiplied BGRA pixels read with `stride`
 * bytes between rows (stride >= cols * 4). */
WfStatus wf_pixmap_from_bgra(const void *data,
        uint32_t cols,
        uint32_t rows,
        size_t stride,
        WfPixmap **out);

/*
 * The largest w x h with the aspect ratio of sw x sh that fits box_w x
 * box_h, at least 1 x 1. Without `upscale` a picture smaller than the box
 * keeps its size. Rounds to nearest, never beyond the box.
 */
void wf_fit_box(uint32_t sw,
        uint32_t sh,
        uint32_t box_w,
        uint32_t box_h,
        int upscale,
        uint32_t *w,
        uint32_t *h);

/*
 * `src` resampled to w x h: an exact area average when shrinking (so thin
 * lines and text keep their weight instead of aliasing away), linear when
 * enlarging. Averages premultiplied values, which is the correct way to mix
 * transparent pixels. Same size returns a copy.
 */
WfStatus wf_pixmap_scale(
        const WfPixmap *src, uint32_t w, uint32_t h, WfPixmap **out);

/*
 * The picture turned and/or mirrored so that it shows the way its camera
 * meant, from an Exif orientation value 1..8 (1 as stored, 2 mirrored,
 * 3 upside down, 4 flipped, 5 transposed, 6 turned 90 degrees clockwise,
 * 7 transversed, 8 turned 90 degrees counter-clockwise). 5..8 swap width and
 * height. Done on the already reduced picture, never on the full-size source:
 * WIC's flip/rotator over a 2400x1600 JPEG took 22 s. Any other value is
 * WF_E_INVAL.
 */
WfStatus wf_pixmap_orient(const WfPixmap *src, int orientation, WfPixmap **out);

/* The pixel as opaque 0x00RRGGBB over the background colour 0x00RRGGBB. */
uint32_t wf_px_over(uint32_t premultiplied, uint32_t background_rgb);

#endif

/* libFuzzer harness: the pixel pipeline on arbitrary sizes and contents:
 * shrink/enlarge, turn, composite, and encode as Sixel. Sizes are taken from
 * the input and kept small enough to run fast; contents are the input. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/wf_buf.h"
#include "preview/wf_pixmap.h"
#include "render/wf_sixel.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint32_t w, h, tw, th;
	int orient;
	WfPixmap *src = NULL, *a = NULL, *b = NULL;

	if (size < 6) {
		return 0;
	}
	w = 1 + data[0] % 48;
	h = 1 + data[1] % 48;
	tw = 1 + data[2] % 64;
	th = 1 + data[3] % 64;
	orient = 1 + data[4] % 9; /* 9 is invalid on purpose */
	if (WF_OK != wf_pixmap_new(w, h, &src)) {
		return 0;
	}
	for (size_t i = 0; i < (size_t)w * h; i++) {
		size_t at = 5 + i * 4;
		uint32_t px = 0;

		for (int k = 0; k < 4; k++) {
			px = px << 8 |
			        (at + (size_t)k < size ? data[at + (size_t)k]
			                               : 0);
		}
		src->px[i] = px;
	}
	if (WF_OK == wf_pixmap_scale(src, tw, th, &a)) {
		if (WF_OK == wf_pixmap_orient(a, orient, &b)) {
			WfBuf out;

			wf_buf_init(&out);
			(void)wf_sixel_encode(
			        b, 0x101010, 2u + data[5] % 255, &out);
			wf_buf_free(&out);
		}
	}
	(void)wf_px_over(src->px[0], 0x336699);
	wf_pixmap_free(b);
	wf_pixmap_free(a);
	wf_pixmap_free(src);
	return 0;
}

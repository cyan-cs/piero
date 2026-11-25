#include "wf_pixmap.h"

#include <stdlib.h>
#include <string.h>

/* The widest horizontal pass buffer we agree to build: 8 bytes a pixel. A
 * source this large never arrives (the decoder host reduces first); it is a
 * limit, not a feature. */
#define SCALE_TEMP_MAX (64u * 1024u * 1024u)

size_t wf_pixmap_bytes(uint32_t w, uint32_t h)
{
	if (0 == w || 0 == h || w > WF_PIXMAP_MAX_SIDE ||
	        h > WF_PIXMAP_MAX_SIDE) {
		return 0;
	}
	/* both sides <= 16384, so the product fits 32 bits before the cap */
	if ((uint64_t)w * h > WF_PIXMAP_MAX_PIXELS) {
		return 0;
	}
	return (size_t)w * h * sizeof(uint32_t);
}

WfStatus wf_pixmap_new(uint32_t w, uint32_t h, WfPixmap **out)
{
	size_t bytes = wf_pixmap_bytes(w, h);
	WfPixmap *p;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	if (0 == bytes) {
		return WF_E_INVAL;
	}
	p = calloc(1, sizeof *p + bytes);
	if (!p) {
		return WF_E_NOMEM;
	}
	p->w = w;
	p->h = h;
	*out = p;
	return WF_OK;
}

void wf_pixmap_free(WfPixmap *p)
{
	free(p);
}

WfStatus wf_pixmap_from_bgra(const void *data,
        uint32_t cols,
        uint32_t rows,
        size_t stride,
        WfPixmap **out)
{
	const unsigned char *src = data;
	WfPixmap *p;
	WfStatus st;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	if (!data || stride < (size_t)cols * 4) {
		return WF_E_INVAL;
	}
	st = wf_pixmap_new(cols, rows, &p);
	if (WF_OK != st) {
		return st;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy(p->px + (size_t)y * cols, src + (size_t)y * stride,
		        (size_t)cols * 4);
	}
	*out = p;
	return WF_OK;
}

void wf_fit_box(uint32_t sw,
        uint32_t sh,
        uint32_t box_w,
        uint32_t box_h,
        int upscale,
        uint32_t *w,
        uint32_t *h)
{
	uint64_t nw, nh;

	if (0 == box_w) {
		box_w = 1;
	}
	if (0 == box_h) {
		box_h = 1;
	}
	if (0 == sw || 0 == sh) {
		*w = 1;
		*h = 1;
		return;
	}
	if (!upscale && sw <= box_w && sh <= box_h) {
		*w = sw;
		*h = sh;
		return;
	}
	/* width-limited when box_w / sw <= box_h / sh, compared without division */
	if ((uint64_t)box_w * sh <= (uint64_t)box_h * sw) {
		nw = box_w;
		nh = ((uint64_t)sh * box_w + sw / 2) / sw;
	} else {
		nh = box_h;
		nw = ((uint64_t)sw * box_h + sh / 2) / sh;
	}
	if (nw < 1) {
		nw = 1;
	}
	if (nh < 1) {
		nh = 1;
	}
	if (nw > box_w) {
		nw = box_w;
	}
	if (nh > box_h) {
		nh = box_h;
	}
	*w = (uint32_t)nw;
	*h = (uint32_t)nh;
}

/*
 * One axis of the resampling, as data: output index i reads `n[i]` input
 * samples starting at `first[i]`, each with a weight out of 65536 (the weights
 * of one output sum to exactly 65536). Both the horizontal and the vertical
 * pass are then the same loop over a plan, and the shrink (area average) and
 * the enlarge (linear) differ only in how the plan is made.
 */
typedef struct
{
	uint32_t *first;
	uint32_t *n;
	uint32_t *weight; /* taps entries per output */
	uint32_t taps;
} Plan;

static void plan_free(Plan *p)
{
	free(p->first);
	free(p->n);
	free(p->weight);
	memset(p, 0, sizeof *p);
}

static WfStatus plan_make(uint32_t in, uint32_t out, Plan *p)
{
	memset(p, 0, sizeof *p);
	p->taps = out <= in ? in / out + 2 : 2;
	p->first = calloc(out, sizeof *p->first);
	p->n = calloc(out, sizeof *p->n);
	p->weight = calloc((size_t)out * p->taps, sizeof *p->weight);
	if (!p->first || !p->n || !p->weight) {
		plan_free(p);
		return WF_E_NOMEM;
	}
	for (uint32_t i = 0; i < out; i++) {
		uint32_t *wt = p->weight + (size_t)i * p->taps;

		if (out <= in) {
			/* output i covers [i*in, (i+1)*in) in units of 1/out input
			 * samples; input j covers [j*out, (j+1)*out) */
			uint64_t lo = (uint64_t)i * in, hi = lo + in;
			uint32_t j0 = (uint32_t)(lo / out), j = j0, k = 0;
			uint32_t sum = 0;

			for (; (uint64_t)j * out < hi && j < in; j++, k++) {
				uint64_t a = (uint64_t)j * out, b = a + out;
				uint64_t from = a > lo ? a : lo;
				uint64_t to = b < hi ? b : hi;

				wt[k] = (uint32_t)((to - from) * 65536 / in);
				sum += wt[k];
			}
			/* the division dropped fractions: give them to the widest tap */
			if (k > 0) {
				wt[k / 2] += 65536 - sum;
			}
			p->first[i] = j0;
			p->n[i] = k;
		} else {
			/* sample centre of output i in input coordinates, 16.16 */
			int64_t pos = (int64_t)(2 * (uint64_t)i + 1) * in *
			                32768 / out -
			        32768;
			int64_t j0 = pos >> 16;
			uint32_t frac = (uint32_t)(pos & 0xFFFF);

			if (j0 < 0) {
				j0 = 0;
				frac = 0;
			}
			if ((uint64_t)j0 + 1 >= in) {
				j0 = (int64_t)in - 1;
				frac = 0;
			}
			p->first[i] = (uint32_t)j0;
			if (0 == frac) {
				wt[0] = 65536;
				p->n[i] = 1;
			} else {
				wt[0] = 65536 - frac;
				wt[1] = frac;
				p->n[i] = 2;
			}
		}
	}
	return WF_OK;
}

WfStatus wf_pixmap_scale(
        const WfPixmap *src, uint32_t w, uint32_t h, WfPixmap **out)
{
	Plan hp = {0}, vp = {0};
	uint16_t *tmp = NULL;
	WfPixmap *dst = NULL;
	WfStatus st;
	size_t tmp_bytes;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	if (!src || !wf_pixmap_bytes(src->w, src->h)) {
		return WF_E_INVAL;
	}
	st = wf_pixmap_new(w, h, &dst);
	if (WF_OK != st) {
		return st;
	}
	if (w == src->w && h == src->h) {
		memcpy(dst->px, src->px, wf_pixmap_bytes(w, h));
		*out = dst;
		return WF_OK;
	}
	tmp_bytes = (size_t)w * src->h * 4 * sizeof *tmp;
	if ((uint64_t)w * src->h > SCALE_TEMP_MAX / 8) {
		wf_pixmap_free(dst);
		return WF_E_RANGE;
	}
	tmp = malloc(tmp_bytes);
	st = tmp ? plan_make(src->w, w, &hp) : WF_E_NOMEM;
	if (WF_OK != st) {
		free(tmp);
		wf_pixmap_free(dst);
		return st;
	}
	st = plan_make(src->h, h, &vp);
	if (WF_OK != st) {
		plan_free(&hp);
		free(tmp);
		wf_pixmap_free(dst);
		return st;
	}

	/* horizontal: 8-bit channels in, 16-bit (value * 256) out */
	for (uint32_t y = 0; y < src->h; y++) {
		const uint32_t *row = src->px + (size_t)y * src->w;

		for (uint32_t x = 0; x < w; x++) {
			const uint32_t *wt = hp.weight + (size_t)x * hp.taps;
			uint64_t acc[4] = {0, 0, 0, 0};
			uint16_t *o = tmp + ((size_t)y * w + x) * 4;

			for (uint32_t k = 0; k < hp.n[x]; k++) {
				uint32_t px = row[hp.first[x] + k];

				acc[0] += (uint64_t)(px >> 24) * wt[k];
				acc[1] += (uint64_t)((px >> 16) & 255) * wt[k];
				acc[2] += (uint64_t)((px >> 8) & 255) * wt[k];
				acc[3] += (uint64_t)(px & 255) * wt[k];
			}
			for (int c = 0; c < 4; c++) {
				o[c] = (uint16_t)((acc[c] + 128) >> 8);
			}
		}
	}
	/* vertical: 16-bit in, 8-bit out */
	for (uint32_t y = 0; y < h; y++) {
		const uint32_t *wt = vp.weight + (size_t)y * vp.taps;

		for (uint32_t x = 0; x < w; x++) {
			uint64_t acc[4] = {0, 0, 0, 0};
			uint32_t v[4];

			for (uint32_t k = 0; k < vp.n[y]; k++) {
				const uint16_t *s = tmp +
				        ((size_t)(vp.first[y] + k) * w + x) * 4;

				for (int c = 0; c < 4; c++) {
					acc[c] += (uint64_t)s[c] * wt[k];
				}
			}
			for (int c = 0; c < 4; c++) {
				uint64_t r =
				        (acc[c] + ((uint64_t)1 << 23)) >> 24;

				v[c] = r > 255 ? 255 : (uint32_t)r;
			}
			dst->px[(size_t)y * w + x] =
			        v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3];
		}
	}
	plan_free(&hp);
	plan_free(&vp);
	free(tmp);
	*out = dst;
	return WF_OK;
}

WfStatus wf_pixmap_orient(const WfPixmap *src, int orientation, WfPixmap **out)
{
	WfPixmap *dst;
	WfStatus st;
	uint32_t w, h, dw, dh;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	if (!src || orientation < 1 || orientation > 8) {
		return WF_E_INVAL;
	}
	w = src->w;
	h = src->h;
	dw = orientation >= 5 ? h : w;
	dh = orientation >= 5 ? w : h;
	st = wf_pixmap_new(dw, dh, &dst);
	if (WF_OK != st) {
		return st;
	}
	for (uint32_t dy = 0; dy < dh; dy++) {
		for (uint32_t dx = 0; dx < dw; dx++) {
			uint32_t sx, sy;

			switch (orientation) {
			case 2:
				sx = w - 1 - dx;
				sy = dy;
				break;
			case 3:
				sx = w - 1 - dx;
				sy = h - 1 - dy;
				break;
			case 4:
				sx = dx;
				sy = h - 1 - dy;
				break;
			case 5:
				sx = dy;
				sy = dx;
				break;
			case 6:
				sx = dy;
				sy = h - 1 - dx;
				break;
			case 7:
				sx = w - 1 - dy;
				sy = h - 1 - dx;
				break;
			case 8:
				sx = w - 1 - dy;
				sy = dx;
				break;
			default:
				sx = dx;
				sy = dy;
				break;
			}
			dst->px[(size_t)dy * dw + dx] =
			        src->px[(size_t)sy * w + sx];
		}
	}
	*out = dst;
	return WF_OK;
}

uint32_t wf_px_over(uint32_t premultiplied, uint32_t background_rgb)
{
	uint32_t a = premultiplied >> 24, inv = 255 - a, out = 0;

	for (int shift = 16; shift >= 0; shift -= 8) {
		uint32_t s = (premultiplied >> shift) & 255;
		uint32_t b = (background_rgb >> shift) & 255;
		uint32_t c = s + (b * inv + 127) / 255;

		out |= (c > 255 ? 255 : c) << shift;
	}
	return out;
}

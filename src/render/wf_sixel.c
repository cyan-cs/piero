#include "wf_sixel.h"

#include <stdlib.h>
#include <string.h>
#include "../base/wf_mem.h"

/*
 * Quantiser: a histogram of 5-bit-per-channel bins (32768 of them), then
 * median cut over the occupied bins. Working on bins instead of pixels keeps
 * the cost independent of the picture size after one pass over it. Each bin
 * belongs to exactly one box, so mapping a pixel to a palette entry is one
 * table lookup, with no nearest-colour search.
 */
#define BINS 32768u

typedef struct
{
	uint32_t count;  /* pixels in the bin */
	uint64_t sum[3]; /* of 8-bit r, g, b: the bin's true mean colour */
} Bin;

typedef struct
{
	uint16_t *bin; /* indices into the histogram, a slice of one array */
	uint32_t n;
	uint64_t pixels;
} Box;

static unsigned bin_of(uint32_t rgb)
{
	return (((rgb >> 19) & 31) << 10) | (((rgb >> 11) & 31) << 5) |
	        ((rgb >> 3) & 31);
}

/* the channel (0 r, 1 g, 2 b) of a bin index, 0..31 */
static unsigned bin_channel(unsigned bin, int ch)
{
	return (bin >> (10 - 5 * ch)) & 31;
}

/* The channel with the widest spread among the box's bins. */
static int widest_channel(const Box *b)
{
	unsigned lo[3] = {31, 31, 31}, hi[3] = {0, 0, 0};
	int best = 0;

	for (uint32_t i = 0; i < b->n; i++) {
		for (int c = 0; c < 3; c++) {
			unsigned v = bin_channel(b->bin[i], c);

			if (v < lo[c]) {
				lo[c] = v;
			}
			if (v > hi[c]) {
				hi[c] = v;
			}
		}
	}
	for (int c = 1; c < 3; c++) {
		if (hi[c] - lo[c] > hi[best] - lo[best]) {
			best = c;
		}
	}
	return best;
}

/* Splits the box at the pixel-count median of its widest channel. */
static void split_box(const Bin *hist, Box *b, Box *other)
{
	uint64_t half = b->pixels / 2, run = 0;
	uint32_t cut = 0;

	/* In-place partition into 32 channel buckets. No global qsort context:
	 * encoders may run concurrently. */
	unsigned counts[32] = {0}, start[32], next[32];
	int channel = widest_channel(b);
	unsigned offset = 0;
	for (uint32_t i = 0; i < b->n; i++) {
		counts[bin_channel(b->bin[i], channel)]++;
	}
	for (unsigned k = 0; k < 32; k++) {
		start[k] = next[k] = offset;
		offset += counts[k];
	}
	for (unsigned k = 0; k < 32; k++) {
		while (next[k] < start[k] + counts[k]) {
			unsigned dest = bin_channel(b->bin[next[k]], channel);
			if (dest == k) {
				next[k]++;
			} else {
				uint16_t tmp = b->bin[next[k]];
				b->bin[next[k]] = b->bin[next[dest]];
				b->bin[next[dest]++] = tmp;
			}
		}
	}
	for (; cut + 1 < b->n; cut++) {
		run += hist[b->bin[cut]].count;
		if (run >= half) {
			cut++;
			break;
		}
	}
	if (0 == cut) {
		cut = 1;
	}
	other->bin = b->bin + cut;
	other->n = b->n - cut;
	other->pixels = 0;
	for (uint32_t i = 0; i < other->n; i++) {
		other->pixels += hist[other->bin[i]].count;
	}
	b->n = cut;
	b->pixels -= other->pixels;
}

/* A command letter and its number: "#12", "!40". */
static WfStatus put_tagged(WfBuf *out, char tag, unsigned n)
{
	WfStatus st = wf_buf_putc(out, tag);

	return WF_OK == st ? wf_buf_put_uint(out, n) : st;
}

static WfStatus put_run(WfBuf *out, unsigned count, char ch)
{
	WfStatus st;

	if (out->len > WF_SIXEL_MAX_BYTES - 16u) {
		return WF_E_RANGE;
	}
	if (count > 3) {
		st = put_tagged(out, '!', count);
		return WF_OK == st ? wf_buf_putc(out, ch) : st;
	}
	while (count--) {
		st = wf_buf_putc(out, ch);
		if (WF_OK != st) {
			return st;
		}
	}
	return WF_OK;
}

static unsigned percent(unsigned v)
{
	return (v * 100 + 127) / 255;
}

/* Emits the colour definitions, then the pixel bands. `idx` holds, per pixel,
 * the palette index or 0xFFFF for "not painted". */
static WfStatus emit(WfBuf *out,
        const WfPixmap *pm,
        const uint16_t *idx,
        const uint8_t (*palette)[3],
        unsigned ncolors,
        WfSixelCancelled cancelled,
        void *ctx)
{
	uint8_t *mask = NULL;  /* ncolors x w column masks of the band */
	uint16_t *used = NULL; /* colours present in the band */
	uint8_t *seen = NULL;
	WfStatus st = WF_OK;
	size_t w = pm->w;

	st = WF_E_NOMEM;
	mask = calloc((size_t)ncolors * w, 1);
	used = wf_malloc_array(ncolors, sizeof *used);
	seen = calloc(ncolors, 1);
	if (!mask || !used || !seen) {
		goto done;
	}
	st = wf_buf_puts(out, "\x1bP0;1;0q\"1;1;");
	if (WF_OK == st) {
		st = wf_buf_put_uint(out, pm->w);
	}
	if (WF_OK == st) {
		st = wf_buf_putc(out, ';');
	}
	if (WF_OK == st) {
		st = wf_buf_put_uint(out, pm->h);
	}
	for (unsigned c = 0; WF_OK == st && c < ncolors; c++) {
		st = put_tagged(out, '#', c);
		for (int k = 0; WF_OK == st && k < 3; k++) {
			/* "#n;2;r;g;b": colour n, RGB, each as a percentage */
			st = wf_buf_putc(out, ';');
			if (WF_OK == st && 0 == k) {
				st = wf_buf_puts(out, "2;");
			}
			if (WF_OK == st) {
				st = wf_buf_put_uint(
				        out, percent(palette[c][k]));
			}
		}
	}
	for (uint32_t y0 = 0; WF_OK == st && y0 < pm->h; y0 += 6) {
		unsigned nused = 0;
		if (cancelled && cancelled(ctx)) {
			st = WF_E_CANCELLED;
			break;
		}

		for (uint32_t r = 0; r < 6 && y0 + r < pm->h; r++) {
			const uint16_t *row = idx + (size_t)(y0 + r) * w;

			for (size_t x = 0; x < w; x++) {
				unsigned c = row[x];

				if (0xFFFFu == c) {
					continue;
				}
				if (!seen[c]) {
					seen[c] = 1;
					used[nused++] = (uint16_t)c;
				}
				mask[(size_t)c * w + x] |= (uint8_t)(1u << r);
			}
		}
		for (unsigned u = 0; WF_OK == st && u < nused; u++) {
			unsigned c = used[u];
			uint8_t *m = mask + (size_t)c * w;
			size_t last = w;

			while (last > 0 && 0 == m[last - 1]) {
				last--;
			}
			st = put_tagged(out, '#', c);
			for (size_t x = 0; WF_OK == st && x < last;) {
				size_t run = 1;

				while (x + run < last && m[x + run] == m[x]) {
					run++;
				}
				st = put_run(
				        out, (unsigned)run, (char)(63 + m[x]));
				x += run;
			}
			memset(m, 0, w);
			seen[c] = 0;
			if (WF_OK == st) {
				/* '$' back to the band's left edge for the next colour;
				 * after the last colour '-' starts the next band */
				st = wf_buf_putc(out, '$');
			}
		}
		if (WF_OK == st && y0 + 6 < pm->h) {
			st = wf_buf_putc(out, '-');
		}
	}
	if (WF_OK == st && out->len > WF_SIXEL_MAX_BYTES - 2u) {
		st = WF_E_RANGE;
	}
	if (WF_OK == st) {
		st = wf_buf_puts(out, "\x1b\\");
	}
done:
	free(mask);
	free(used);
	free(seen);
	return st;
}

WfStatus wf_sixel_encode_ex(const WfPixmap *pm,
        uint32_t bg_rgb,
        unsigned max_colors,
        bool dither,
        WfSixelCancelled cancelled,
        void *ctx,
        WfBuf *out)
{
	Bin *hist = NULL;
	uint16_t *bins = NULL, *idx = NULL, *lut = NULL;
	int32_t *errors = NULL, *current = NULL, *next = NULL;
	Box *boxes = NULL;
	uint8_t (*palette)[3] = NULL;

	size_t npix, old_len;
	unsigned nbins = 0, nboxes = 1;
	WfStatus st = WF_E_NOMEM;

	if (!pm || !out || !wf_pixmap_bytes(pm->w, pm->h)) {
		return WF_E_INVAL;
	}
	if (max_colors < 2) {
		max_colors = 2;
	}
	if (max_colors > WF_SIXEL_MAX_COLORS) {
		max_colors = WF_SIXEL_MAX_COLORS;
	}
	if ((uint64_t)pm->w * pm->h > WF_SIXEL_MAX_PIXELS || out->len > 4096) {
		return WF_E_RANGE;
	}
	npix = (size_t)pm->w * pm->h;
	old_len = out->len;
	hist = calloc(BINS, sizeof *hist);
	bins = malloc(BINS * sizeof *bins);
	idx = malloc(npix * sizeof *idx);

	lut = malloc(BINS * sizeof *lut);
	boxes = calloc(max_colors, sizeof *boxes);
	palette = calloc(max_colors, sizeof *palette);
	if (!hist || !bins || !idx || !lut || !boxes || !palette) {
		goto done;
	}

	for (size_t i = 0; i < npix; i++) {
		uint32_t c = wf_px_over(pm->px[i], bg_rgb);
		Bin *b = &hist[bin_of(c)];

		if ((i & 16383u) == 0 && cancelled && cancelled(ctx)) {
			st = WF_E_CANCELLED;
			goto done;
		}
		if (c == (bg_rgb & 0xFFFFFF)) {
			continue; /* not painted: not part of the palette either */
		}
		b->count++;
		b->sum[0] += (c >> 16) & 255;
		b->sum[1] += (c >> 8) & 255;
		b->sum[2] += c & 255;
	}
	for (unsigned i = 0; i < BINS; i++) {
		if (hist[i].count) {
			bins[nbins++] = (uint16_t)i;
		}
	}
	boxes[0].bin = bins;
	boxes[0].n = nbins;
	for (unsigned i = 0; i < nbins; i++) {
		boxes[0].pixels += hist[bins[i]].count;
	}
	while (nboxes < max_colors) {
		unsigned pick = nboxes;
		uint64_t best = 0;

		for (unsigned i = 0; i < nboxes; i++) {
			if (boxes[i].n > 1 && boxes[i].pixels > best) {
				best = boxes[i].pixels;
				pick = i;
			}
		}
		if (pick == nboxes) {
			break; /* every box is a single bin */
		}
		split_box(hist, &boxes[pick], &boxes[nboxes]);
		nboxes++;
	}
	if (0 == nbins) {
		nboxes = 1; /* one entry so the sequence is well formed */
	}
	for (unsigned b = 0; b < nboxes; b++) {
		uint64_t sum[3] = {0, 0, 0}, n = 0;

		for (uint32_t i = 0; i < boxes[b].n; i++) {
			const Bin *h = &hist[boxes[b].bin[i]];

			sum[0] += h->sum[0];
			sum[1] += h->sum[1];
			sum[2] += h->sum[2];
			n += h->count;
			lut[boxes[b].bin[i]] = (uint16_t)b;
		}
		for (int k = 0; k < 3; k++) {
			palette[b][k] = n ? (uint8_t)((sum[k] + n / 2) / n) : 0;
		}
	}
	/* Only a picture with more colours than the palette, and dithered, reads
	 * a bin that no pixel was counted in (the error moves a pixel to any bin):
	 * for it, the nearest palette entry of each of the 5-bit bins, one bounded
	 * lookup and no per-pixel palette search. Otherwise every pixel is in a bin
	 * the box split gave its entry, and the search would be for nothing. */
	if (dither && nbins > max_colors) {
		for (unsigned i = 0; i < BINS; i++) {
			unsigned best = 0, distance = UINT32_MAX;

			if ((i & 1023u) == 0 && cancelled && cancelled(ctx)) {
				st = WF_E_CANCELLED;
				goto done;
			}
			for (unsigned j = 0; j < nboxes; j++) {
				unsigned d = 0;

				for (int k = 0; k < 3; k++) {
					int delta =
					        (int)(bin_channel(i, k) * 8 +
					                4) -
					        palette[j][k];
					d += (unsigned)(delta * delta);
				}
				if (d < distance) {
					distance = d;
					best = j;
				}
			}
			lut[i] = (uint16_t)best;
		}
	}
	if (dither && nbins > max_colors) {
		errors = calloc(((size_t)pm->w + 2) * 6, sizeof *errors);
		if (!errors) {
			st = WF_E_NOMEM;
			goto done;
		}
		current = errors;
		next = errors + ((size_t)pm->w + 2) * 3;
	}
	for (uint32_t y = 0; y < pm->h; y++) {
		if (cancelled && cancelled(ctx)) {
			st = WF_E_CANCELLED;
			goto done;
		}
		for (uint32_t x = 0; x < pm->w; x++) {
			size_t i = (size_t)y * pm->w + x,
			       e = ((size_t)x + 1) * 3;
			uint32_t rgb = wf_px_over(pm->px[i], bg_rgb),
			         mapped = 0;
			int adjusted[3];
			if (rgb == (bg_rgb & 0xffffff)) {
				idx[i] = 0xffffu;
				continue;
			}
			for (int k = 0; k < 3; k++) {
				int v = (int)((rgb >> (16 - k * 8)) & 255);
				if (errors) {
					v += current[e + (size_t)k] / 16;
				}
				if (v < 0) {
					v = 0;
				}
				if (v > 255) {
					v = 255;
				}
				adjusted[k] = v;
				mapped |= (uint32_t)v << (16 - k * 8);
			}
			idx[i] = lut[bin_of(mapped)];
			if (errors) {
				for (int k = 0; k < 3; k++) {
					size_t at = e + (size_t)k;
					int quantized =
					        (int)((percent(palette[idx[i]]
					                              [k]) *
					                              255 +
					                      50) /
					                100);
					int delta = adjusted[k] - quantized;
					current[at + 3] += delta * 7;
					next[at - 3] += delta * 3;
					next[at] += delta * 5;
					next[at + 3] += delta;
				}
			}
		}
		if (errors) {
			int32_t *tmp = current;
			current = next;
			next = tmp;
			memset(next, 0, ((size_t)pm->w + 2) * 3 * sizeof *next);
		}
	}
	st = emit(out, pm, idx, (const uint8_t (*)[3])palette, nboxes,
	        cancelled, ctx);
done:
	if (WF_OK != st && out->data) {
		out->len = old_len; /* leave the buffer as found */
		out->data[old_len] = '\0';
	}
	free(errors);
	free(hist);
	free(bins);
	free(idx);

	free(lut);
	free(boxes);
	free(palette);
	return st;
}

WfStatus wf_sixel_encode(
        const WfPixmap *pm, uint32_t bg_rgb, unsigned max_colors, WfBuf *out)
{
	return wf_sixel_encode_ex(
	        pm, bg_rgb, max_colors, false, NULL, NULL, out);
}

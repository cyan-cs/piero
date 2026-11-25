#ifndef WF_IMGFIX_H
#define WF_IMGFIX_H

/*
 * Hostile and huge picture files, made without decoding or encoding anything:
 * a PNG whose pixels are all zero is a few megabytes of deflate even when it
 * claims hundreds of millions of pixels (a decompression bomb that is also a
 * valid file), and a header that lies costs a few bytes. Header-only, for tests
 * and the probe tool; every function returns malloc'd bytes the caller frees.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
	uint8_t *p;
	size_t n, cap;
} FixBuf;

static inline void fix_put(FixBuf *b, const void *d, size_t n)
{
	if (b->n + n > b->cap) {
		size_t c = b->cap ? b->cap : 4096;

		while (c < b->n + n) {
			c *= 2;
		}
		b->p = realloc(b->p, c);
		b->cap = c;
	}
	memcpy(b->p + b->n, d, n);
	b->n += n;
}

static inline void fix_put8(FixBuf *b, unsigned v)
{
	uint8_t c = (uint8_t)v;

	fix_put(b, &c, 1);
}

static inline void fix_be32(FixBuf *b, uint32_t v)
{
	uint8_t c[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16),
	        (uint8_t)(v >> 8), (uint8_t)v};

	fix_put(b, c, 4);
}

static inline void fix_le32(FixBuf *b, uint32_t v)
{
	uint8_t c[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
	        (uint8_t)(v >> 24)};

	fix_put(b, c, 4);
}

static inline uint32_t fix_crc(const uint8_t *d, size_t n, uint32_t crc)
{
	static uint32_t table[256];

	if (!table[1]) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;

			for (int k = 0; k < 8; k++) {
				c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			}
			table[i] = c;
		}
	}
	crc = ~crc;
	for (size_t i = 0; i < n; i++) {
		crc = table[(crc ^ d[i]) & 255] ^ (crc >> 8);
	}
	return ~crc;
}

typedef struct
{
	FixBuf *b;
	uint32_t acc;
	int nbits;
} FixBits;

static inline void fix_bits(FixBits *w, uint32_t v, int n) /* LSB first */
{
	for (int i = 0; i < n; i++) {
		w->acc |= ((v >> i) & 1u) << w->nbits;
		if (8 == ++w->nbits) {
			fix_put8(w->b, w->acc);
			w->acc = 0;
			w->nbits = 0;
		}
	}
}

static inline void fix_huff(FixBits *w, uint32_t code, int n) /* MSB first */
{
	for (int i = n - 1; i >= 0; i--) {
		fix_bits(w, (code >> i) & 1u, 1);
	}
}

static inline void fix_lit_len(FixBits *w, unsigned v)
{
	if (v < 144) {
		fix_huff(w, 0x30u + v, 8);
	} else if (v < 256) {
		fix_huff(w, 0x190u + (v - 144), 9);
	} else if (v < 280) {
		fix_huff(w, v - 256, 7);
	} else {
		fix_huff(w, 0xC0u + (v - 280), 8);
	}
}

static inline void fix_match(FixBits *w, unsigned len) /* distance 1 */
{
	static const unsigned base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15,
	        17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163,
	        195, 227, 258};
	static const int extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2,
	        2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
	int k = 28;

	while (base[k] > len) {
		k--;
	}
	fix_lit_len(w, 257u + (unsigned)k);
	fix_bits(w, len - base[k], extra[k]);
	fix_huff(w, 0, 5);
}

/* A zlib stream that inflates to `n` zero bytes. */
static inline void fix_zlib_zeros(FixBuf *out, uint64_t n)
{
	FixBits w = {out, 0, 0};
	uint64_t left = n;
	uint32_t adler;

	fix_put8(out, 0x78);
	fix_put8(out, 0x01);
	fix_bits(&w, 1, 1); /* final */
	fix_bits(&w, 1, 2); /* fixed Huffman */
	if (left) {
		fix_lit_len(&w, 0);
		left--;
	}
	while (left >= 258) {
		fix_match(&w, 258);
		left -= 258;
	}
	if (left >= 3) {
		fix_match(&w, (unsigned)left);
		left = 0;
	}
	while (left--) {
		fix_lit_len(&w, 0);
	}
	fix_lit_len(&w, 256);
	if (w.nbits) {
		fix_bits(&w, 0, 8 - w.nbits);
	}
	adler = (uint32_t)(((n % 65521u) << 16) | 1u);
	fix_be32(out, adler);
}

static inline void fix_chunk(
        FixBuf *out, const char *type, const void *d, size_t n)
{
	uint32_t crc = fix_crc((const uint8_t *)type, 4, 0);

	fix_be32(out, (uint32_t)n);
	fix_put(out, type, 4);
	if (n) {
		fix_put(out, d, n);
		crc = fix_crc(d, n, crc);
	}
	fix_be32(out, crc);
}

/* The bytes of a PNG of all-zero rows: `color` 0 (gray) or 2 (RGB) at 8 bits
 * a sample, optionally interlaced, with a valid IHDR, IDAT, IEND and CRCs. The
 * dimensions may be anything: nothing of the size is allocated. */
static inline uint8_t *fix_png_zeros(
        uint32_t w, uint32_t h, int color, int interlace, size_t *n)
{
	static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
	FixBuf b = {0}, ihdr = {0}, z = {0};
	uint64_t raw = 0;
	unsigned bpp = 2 == color ? 3 : 1;

	fix_put(&b, sig, 8);
	fix_be32(&ihdr, w);
	fix_be32(&ihdr, h);
	fix_put8(&ihdr, 8);
	fix_put8(&ihdr, (unsigned)color);
	fix_put8(&ihdr, 0);
	fix_put8(&ihdr, 0);
	fix_put8(&ihdr, interlace ? 1 : 0);
	fix_chunk(&b, "IHDR", ihdr.p, ihdr.n);
	if (!interlace) {
		raw = (uint64_t)h * ((uint64_t)w * bpp + 1);
	} else {
		static const unsigned x0[7] = {0, 4, 0, 2, 0, 1, 0},
		                      y0[7] = {0, 0, 4, 0, 2, 0, 1},
		                      dx[7] = {8, 8, 4, 4, 2, 2, 1},
		                      dy[7] = {8, 8, 8, 4, 4, 2, 2};

		for (int p = 0; p < 7; p++) {
			uint64_t pw =
			        w > x0[p] ? (w - x0[p] + dx[p] - 1) / dx[p] : 0;
			uint64_t ph =
			        h > y0[p] ? (h - y0[p] + dy[p] - 1) / dy[p] : 0;

			if (pw && ph) {
				raw += ph * (pw * bpp + 1);
			}
		}
	}
	/* a header may claim more pixels than any file could hold: the stream then
	 * stops short of what the header says, as a hostile file's does */
	fix_zlib_zeros(&z, raw > (600ull << 20) ? (600ull << 20) : raw);
	fix_chunk(&b, "IDAT", z.p, z.n);
	fix_chunk(&b, "IEND", NULL, 0);
	free(ihdr.p);
	free(z.p);
	*n = b.n;
	return b.p;
}

/* A BMP header (BITMAPINFOHEADER) that says what it likes, then `body` zero
 * bytes (the file need not hold the pixels it describes). */
static inline uint8_t *fix_bmp(
        int32_t w, int32_t h, unsigned bits, uint64_t body, size_t *n)
{
	FixBuf b = {0};
	uint8_t zero[4096] = {0};

	fix_put(&b, "BM", 2);
	fix_le32(&b, 54);
	fix_le32(&b, 0);
	fix_le32(&b, 54);
	fix_le32(&b, 40);
	fix_le32(&b, (uint32_t)w);
	fix_le32(&b, (uint32_t)h);
	fix_put8(&b, 1);
	fix_put8(&b, 0);
	fix_put8(&b, bits);
	fix_put8(&b, 0);
	for (int i = 0; i < 6; i++) {
		fix_le32(&b, 0);
	}
	while (body) {
		size_t k = body < sizeof zero ? (size_t)body : sizeof zero;

		fix_put(&b, zero, k);
		body -= k;
	}
	*n = b.n;
	return b.p;
}

#endif

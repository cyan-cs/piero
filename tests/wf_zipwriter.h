/*
 * A ZIP writer for tests: entries are stored (not compressed) so that every
 * byte of a hostile archive is in the test that makes it. `claimed` is the
 * size the headers say, normally the length; zip_crc_xor writes a wrong
 * checksum. Include once, in a test that uses it.
 */
#ifndef WF_ZIPWRITER_H
#define WF_ZIPWRITER_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
	uint8_t *d;
	size_t n, cap;
	uint32_t central_n;
	uint8_t *c;
	size_t cn, ccap;
} ZipOut;

static void zput(
        uint8_t **buf, size_t *n, size_t *cap, const void *p, size_t len)
{
	if (*n + len > *cap) {
		*cap = (*n + len) * 2 + 1024;
		*buf = realloc(*buf, *cap);
	}
	memcpy(*buf + *n, p, len);
	*n += len;
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	size_t i;
	int k;

	for (i = 0; i < n; i++) {
		c ^= p[i];
		for (k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
		}
	}
	return ~c;
}

static void le16(uint8_t *o, unsigned v)
{
	o[0] = (uint8_t)v;
	o[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *o, uint32_t v)
{
	le16(o, v & 0xFFFF);
	le16(o + 2, v >> 16);
}

/* A stored entry. `claimed` is the size the headers say (normally len);
 * flags bit 11 marks the name as UTF-8. */
static uint32_t zip_crc_xor; /* a test sets it to write a wrong checksum */

static void zip_add(ZipOut *z,
        const char *name,
        size_t namelen,
        const void *data,
        size_t len,
        size_t claimed,
        unsigned flags)
{
	uint8_t h[30], ch[46];
	uint32_t crc = crc32_of(data, len) ^ zip_crc_xor, off = (uint32_t)z->n;

	memset(h, 0, sizeof h);
	le32(h, 0x04034b50);
	le16(h + 4, 20);
	le16(h + 6, flags);
	le32(h + 14, crc);
	le32(h + 18, (uint32_t)claimed);
	le32(h + 22, (uint32_t)claimed);
	le16(h + 26, (unsigned)namelen);
	zput(&z->d, &z->n, &z->cap, h, sizeof h);
	zput(&z->d, &z->n, &z->cap, name, namelen);
	zput(&z->d, &z->n, &z->cap, data, len);
	memset(ch, 0, sizeof ch);
	le32(ch, 0x02014b50);
	le16(ch + 4, 20);
	le16(ch + 6, 20);
	le16(ch + 8, flags);
	le32(ch + 16, crc);
	le32(ch + 20, (uint32_t)claimed);
	le32(ch + 24, (uint32_t)claimed);
	le16(ch + 28, (unsigned)namelen);
	le32(ch + 42, off);
	zput(&z->c, &z->cn, &z->ccap, ch, sizeof ch);
	zput(&z->c, &z->cn, &z->ccap, name, namelen);
	z->central_n++;
}

static void zip_add_s(ZipOut *z, const char *name, const char *data)
{
	zip_add(z, name, strlen(name), data, strlen(data), strlen(data), 0x800);
}

/* An entry stored as a raw deflate stream that `comp` already is. `claimed` is
 * the size the headers say the data has when inflated, and `crc` its checksum:
 * both may be lies, which is the point. */
static inline void zip_add_deflated(ZipOut *z,
        const char *name,
        const void *comp,
        size_t comp_len,
        uint64_t claimed,
        uint32_t crc)
{
	uint8_t h[30], ch[46];
	size_t namelen = strlen(name);
	uint32_t off = (uint32_t)z->n;

	memset(h, 0, sizeof h);
	le32(h, 0x04034b50);
	le16(h + 4, 20);
	le16(h + 6, 0x800);
	le16(h + 8, 8);
	le32(h + 14, crc);
	le32(h + 18, (uint32_t)comp_len);
	le32(h + 22, (uint32_t)claimed);
	le16(h + 26, (unsigned)namelen);
	zput(&z->d, &z->n, &z->cap, h, sizeof h);
	zput(&z->d, &z->n, &z->cap, name, namelen);
	zput(&z->d, &z->n, &z->cap, comp, comp_len);
	memset(ch, 0, sizeof ch);
	le32(ch, 0x02014b50);
	le16(ch + 4, 20);
	le16(ch + 6, 20);
	le16(ch + 8, 0x800);
	le16(ch + 10, 8);
	le32(ch + 16, crc);
	le32(ch + 20, (uint32_t)comp_len);
	le32(ch + 24, (uint32_t)claimed);
	le16(ch + 28, (unsigned)namelen);
	le32(ch + 42, off);
	zput(&z->c, &z->cn, &z->ccap, ch, sizeof ch);
	zput(&z->c, &z->cn, &z->ccap, name, namelen);
	z->central_n++;
}

/* A raw deflate stream that inflates to `1 + 258 * matches` bytes of 'a': one
 * literal, then matches of length 258 at distance 1 in a fixed-code block, at
 * about 13 bits for each 258 bytes (a ratio near 160 to 1). *out is malloc'd. */
static inline void deflate_run_of_a(size_t matches, uint8_t **out, size_t *n)
{
	size_t cap = matches * 2 + 16, len = 0;
	uint8_t *b = calloc(cap, 1);
	uint64_t acc = 0;
	unsigned bits = 0;

#define PUT_BITS_LSB(v, k)                                                     \
	do {                                                                   \
		acc |= (uint64_t)(v) << bits;                                  \
		bits += (k);                                                   \
		while (bits >= 8) {                                            \
			b[len++] = (uint8_t)acc;                               \
			acc >>= 8;                                             \
			bits -= 8;                                             \
		}                                                              \
	} while (0)
#define PUT_CODE_MSB(code, k)                                                  \
	do {                                                                   \
		for (int i_ = (int)(k) - 1; i_ >= 0; i_--) {                   \
			PUT_BITS_LSB(((code) >> i_) & 1u, 1);                  \
		}                                                              \
	} while (0)
	PUT_BITS_LSB(1, 1); /* final block */
	PUT_BITS_LSB(1, 2); /* fixed codes */
	PUT_CODE_MSB(0x30 + 'a', 8);
	for (size_t i = 0; i < matches; i++) {
		PUT_CODE_MSB(0xC0 + (285 - 280), 8); /* length 258 */
		PUT_CODE_MSB(0, 5);                  /* distance 1 */
	}
	PUT_CODE_MSB(0, 7); /* end of block */
	if (bits) {
		b[len++] = (uint8_t)acc;
	}
#undef PUT_CODE_MSB
#undef PUT_BITS_LSB
	*out = b;
	*n = len;
}

/* The finished archive: central directory and end record appended, the
 * writer emptied. The caller frees *data. */
static void zip_finish(ZipOut *z, uint8_t **data, size_t *n)
{
	uint8_t e[22];
	uint32_t start = (uint32_t)z->n;

	zput(&z->d, &z->n, &z->cap, z->c, z->cn);
	memset(e, 0, sizeof e);
	le32(e, 0x06054b50);
	le16(e + 8, z->central_n > 0xFFFF ? 0xFFFF : z->central_n);
	le16(e + 10, z->central_n > 0xFFFF ? 0xFFFF : z->central_n);
	le32(e + 12, (uint32_t)z->cn);
	le32(e + 16, start);
	zput(&z->d, &z->n, &z->cap, e, sizeof e);
	*data = z->d;
	*n = z->n;
	free(z->c);
	memset(z, 0, sizeof *z);
}

#endif

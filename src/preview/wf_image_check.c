#include "wf_image_check.h"

#include <windows.h>

#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	        (uint32_t)p[2] << 8 | p[3];
}

static uint32_t le16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	        (uint32_t)p[3] << 24;
}

/* The CRC-32 of the PNG specification (reflected, polynomial 0xEDB88320). */
static uint32_t crc_table[256];
static INIT_ONCE crc_once = INIT_ONCE_STATIC_INIT;

/* Filled once, by exactly one thread; the others wait for it. */
static BOOL CALLBACK crc_table_fill(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	(void)once;
	(void)param;
	(void)ctx;
	for (uint32_t k = 0; k < 256; k++) {
		uint32_t c = k;

		for (int bit = 0; bit < 8; bit++) {
			c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
		}
		crc_table[k] = c;
	}
	return TRUE;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
	const uint32_t *table = crc_table;
	size_t i;

	(void)InitOnceExecuteOnce(&crc_once, crc_table_fill, NULL, NULL);
	for (i = 0; i < n; i++) {
		crc = table[(crc ^ p[i]) & 255u] ^ (crc >> 8);
	}
	return crc;
}

/* What the PNG specification allows in IHDR: a size that is a number of pixels
 * a decoder can hold (1 to 2^31-1 a side), the one compression and filter
 * method there are, and only the sample depths each colour type has. A decoder
 * that takes a header outside it is being lenient about a file nobody made on
 * purpose. */
static bool png_ihdr_ok(const uint8_t *h)
{
	uint32_t w = be32(h), ht = be32(h + 4);
	unsigned depth = h[8], color = h[9];
	bool depth_ok;

	if (0 == w || 0 == ht || w > 0x7FFFFFFFu || ht > 0x7FFFFFFFu) {
		return false;
	}
	switch (color) {
	case 0:
		depth_ok = 1 == depth || 2 == depth || 4 == depth ||
		        8 == depth || 16 == depth;
		break;
	case 3:
		depth_ok = 1 == depth || 2 == depth || 4 == depth || 8 == depth;
		break;
	case 2:
	case 4:
	case 6:  depth_ok = 8 == depth || 16 == depth; break;
	default: depth_ok = false; break;
	}
	return depth_ok && 0 == h[10] && 0 == h[11] && h[12] <= 1;
}

static bool refuse(const char **why, const char *reason)
{
	if (why) {
		*why = reason;
	}
	return false;
}

bool wf_png_structure_ok(const uint8_t *data, size_t size, const char **why)
{
	static const uint8_t signature[8] = {
	        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	size_t at = 8;
	bool first = true, idat = false;

	if (why) {
		*why = "";
	}
	if (!data || size < sizeof signature ||
	        memcmp(data, signature, sizeof signature)) {
		return refuse(why, "not a PNG signature");
	}
	while (at < size) {
		const uint8_t *type;
		uint32_t length, crc;
		bool is_iend;

		if (size - at < 12) {
			return refuse(why, "truncated chunk header");
		}
		length = be32(data + at);
		if (length > 0x7FFFFFFFu || length > size - at - 12) {
			return refuse(why, "chunk length exceeds the file");
		}
		type = data + at + 4;
		crc = be32(data + at + 8 + length);
		if ((crc32_update(0xFFFFFFFFu, type, (size_t)length + 4) ^
		            0xFFFFFFFFu) != crc) {
			return refuse(why, "chunk CRC mismatch");
		}
		if (first && (memcmp(type, "IHDR", 4) || 13 != length)) {
			return refuse(why, "IHDR is not the first chunk");
		}
		if (first && !png_ihdr_ok(data + at + 8)) {
			return refuse(
			        why, "IHDR holds values PNG does not allow");
		}
		if (!first && !memcmp(type, "IHDR", 4)) {
			return refuse(why, "second IHDR");
		}
		first = false;
		if (!memcmp(type, "IDAT", 4)) {
			idat = true;
		}
		is_iend = !memcmp(type, "IEND", 4);
		at += 12 + (size_t)length;
		if (is_iend) {
			if (0 != length) {
				return refuse(why, "IEND has data");
			}
			if (!idat) {
				return refuse(why, "no image data");
			}
			if (at != size) {
				return refuse(why, "data after IEND");
			}
			return true;
		}
	}
	return refuse(why, "missing IEND (truncated)");
}

/* An entry's picture: where it is, and the entry that named it first. */
typedef struct
{
	uint32_t offset, bytes, index;
} IcoRef;

static int ico_ref_order(const void *a, const void *b)
{
	const IcoRef *x = a, *y = b;

	if (x->offset != y->offset) {
		return x->offset < y->offset ? -1 : 1;
	}
	if (x->bytes != y->bytes) {
		return x->bytes < y->bytes ? -1 : 1;
	}
	return x->index < y->index ? -1 : x->index > y->index;
}

/*
 * Entries of one size often name the same picture, and a file can name one
 * picture from thousands of entries. The picture is checked once, by the first
 * entry that names it (the same place, the same length: the same bytes). With
 * the entries ordered by place, `first[i]` is that first entry, and the others
 * are skipped; the verdict, and the reason given, are what checking every entry
 * in turn would give.
 */
bool wf_ico_structure_ok(const uint8_t *data, size_t size, const char **why)
{
	uint32_t count, i;
	IcoRef *ref;
	uint32_t *first;

	if (why) {
		*why = "";
	}
	if (!data || size < 6 || 0 != le16(data) || 1 != le16(data + 2)) {
		return refuse(why, "not an ICO header");
	}
	count = le16(data + 4);
	if (0 == count || size < 6 + (size_t)count * 16) {
		return refuse(why, "ICO directory outside the file");
	}
	ref = malloc((size_t)count * sizeof *ref);
	first = malloc((size_t)count * sizeof *first);
	if (!ref || !first) {
		free(ref);
		free(first);
		return refuse(why, "out of memory");
	}
	for (i = 0; i < count; i++) {
		const uint8_t *e = data + 6 + (size_t)i * 16;

		ref[i].bytes = le32(e + 8);
		ref[i].offset = le32(e + 12);
		ref[i].index = i;
	}
	qsort(ref, count, sizeof *ref, ico_ref_order);
	for (i = 0; i < count; i++) {
		bool again = i && ref[i].offset == ref[i - 1].offset &&
		        ref[i].bytes == ref[i - 1].bytes;

		first[ref[i].index] =
		        again ? first[ref[i - 1].index] : ref[i].index;
	}
	free(ref);
	for (i = 0; i < count; i++) {
		const uint8_t *e = data + 6 + (size_t)i * 16;
		uint32_t bytes = le32(e + 8), offset = le32(e + 12);

		if (0 == bytes || offset > size || bytes > size - offset) {
			free(first);
			return refuse(why, "ICO image outside the file");
		}
		if (first[i] == i && bytes >= 8 &&
		        !memcmp(data + offset, "\x89PNG", 4) &&
		        !wf_png_structure_ok(data + offset, bytes, why)) {
			free(first);
			return false;
		}
	}
	free(first);
	return true;
}

static uint32_t be16(const uint8_t *p)
{
	return (uint32_t)p[0] << 8 | p[1];
}

/*
 * Walks the JPEG marker structure without decoding any of it. The decoder
 * shows a file that is cut off as a picture with the missing part filled in
 * (measured: cut to a fifth of its size it still "decodes"), so the one thing
 * to establish before it runs is that the stream really ends in EOI.
 */
bool wf_jpeg_structure_ok(const uint8_t *data, size_t size, const char **why)
{
	size_t p = 2;
	bool frame = false, scan = false;

	if (why) {
		*why = "";
	}
	if (!data || size < 4 || 0xFF != data[0] || 0xD8 != data[1]) {
		return refuse(why, "not a JPEG header");
	}
	for (;;) {
		uint8_t m;
		size_t len;

		if (p >= size || 0xFF != data[p]) {
			return refuse(why, "JPEG has no end of image");
		}
		while (p < size && 0xFF == data[p]) {
			p++; /* any number of fill bytes may precede a marker */
		}
		if (p >= size) {
			return refuse(why, "JPEG has no end of image");
		}
		m = data[p++];
		if (0xD9 == m) {
			return frame && scan
			        ? true
			        : refuse(why, "JPEG has no picture");
		}
		if (0x00 == m || 0xD8 == m) {
			return refuse(why, "JPEG marker out of place");
		}
		if (0x01 == m || (m >= 0xD0 && m <= 0xD7)) {
			continue; /* markers without a length */
		}
		if (p + 2 > size) {
			return refuse(why, "JPEG segment outside the file");
		}
		len = be16(data + p);
		if (len < 2 || len > size - p) {
			return refuse(why, "JPEG segment outside the file");
		}
		if (m >= 0xC0 && m <= 0xCF && 0xC4 != m && 0xC8 != m &&
		        0xCC != m) {
			frame = true;
		}
		p += len;
		if (0xDA == m) { /* start of scan: entropy-coded data follows */
			scan = true;
			while (p < size) {
				if (0xFF != data[p]) {
					p++;
				} else if (p + 1 >= size) {
					return refuse(why,
					        "JPEG has no end of image");
				} else if (0x00 == data[p + 1] ||
				        (data[p + 1] >= 0xD0 &&
				                data[p + 1] <= 0xD7)) {
					p += 2; /* a stuffed byte or a restart marker */
				} else if (0xFF == data[p + 1]) {
					p++; /* fill before a marker */
				} else {
					break; /* a real marker ends the scan data */
				}
			}
		}
	}
}

/* GIF blocks, to the trailer. A file cut inside the image data would otherwise
 * show the rows that arrived. */
bool wf_gif_structure_ok(const uint8_t *data, size_t size, const char **why)
{
	size_t p = 13;
	bool image = false;

	if (why) {
		*why = "";
	}
	if (!data || size < 14 ||
	        (memcmp(data, "GIF87a", 6) && memcmp(data, "GIF89a", 6))) {
		return refuse(why, "not a GIF header");
	}
	if (data[10] & 0x80) {
		p += (size_t)3 << ((data[10] & 7) + 1);
	}
	while (p < size) {
		uint8_t b = data[p++];

		if (0x3B == b) {
			return image ? true : refuse(why, "GIF has no picture");
		}
		if (0x2C == b) {
			uint8_t flags;

			if (p + 9 > size) {
				return refuse(why, "GIF has no trailer");
			}
			flags = data[p + 8];
			p += 9;
			if (flags & 0x80) {
				p += (size_t)3 << ((flags & 7) + 1);
			}
			p++; /* LZW minimum code size */
			image = true;
		} else if (0x21 == b) {
			p++; /* extension label */
		} else {
			return refuse(why, "GIF block out of place");
		}
		/* data sub-blocks: length-prefixed, ended by a zero length */
		for (;;) {
			size_t n;

			if (p >= size) {
				return refuse(why, "GIF has no trailer");
			}
			n = data[p++];
			if (0 == n) {
				break;
			}
			if (n > size - p) {
				return refuse(why, "GIF has no trailer");
			}
			p += n;
		}
	}
	return refuse(why, "GIF has no trailer");
}

/* RIFF container size against the file: a WebP cut short declares more than
 * it has. Chunk contents are the decoder's business. */
bool wf_webp_structure_ok(const uint8_t *data, size_t size, const char **why)
{
	uint64_t riff;

	if (why) {
		*why = "";
	}
	if (!data || size < 20 || memcmp(data, "RIFF", 4) ||
	        memcmp(data + 8, "WEBP", 4)) {
		return refuse(why, "not a WebP header");
	}
	riff = (uint64_t)le32(data + 4) + 8;
	if (riff < 20 || riff > size) {
		return refuse(why, "WebP is shorter than it says");
	}
	return true;
}

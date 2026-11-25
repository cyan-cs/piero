#ifndef WF_IMAGE_CHECK_H
#define WF_IMAGE_CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Structural checks run BEFORE bytes reach an image decoder.
 *
 * Measured (P0-A03): the Windows PNG decoder accepts a PNG cut to 100 bytes and
 * a PNG with two damaged bytes in the middle, decoding the rows it can and
 * reporting success. A decoder that accepts truncated or corrupted input is not
 * a bounded failure, so a damaged file must be refused here instead of showing
 * a partial picture as if it were the file.
 *
 * Functions over a byte range: no decoder, no I/O, and no allocation except in
 * wf_ico_structure_ok (below). They say nothing about pixel content or
 * dimensions limits (the decoder host enforces the raster limit separately).
 */

/* A PNG is accepted only if: the signature matches; IHDR is the first chunk
 * (13 bytes); every chunk's length fits the buffer and its CRC-32 (over type
 * and data) matches; at least one IDAT is present; and IEND (length 0) is the
 * last chunk with nothing after it. *why is a static text for a refusal. */
bool wf_png_structure_ok(const uint8_t *data, size_t size, const char **why);

/* An ICO container: header, directory entries inside the buffer, every image
 * payload inside the buffer. A payload that is a PNG must itself pass
 * wf_png_structure_ok(). It allocates two arrays of one slot per directory entry
 * (at most 65535 entries, about 1 MiB in all), freed before it returns. When
 * they cannot be had it returns false with *why "out of memory": a good file is
 * then refused like a damaged one (nothing is shown), and the caller cannot tell
 * the two apart except by that text. */
bool wf_ico_structure_ok(const uint8_t *data, size_t size, const char **why);

/* JPEG: SOI, a frame header and a scan, every marker segment inside the
 * buffer, and EOI reached (bytes after EOI are tolerated: some cameras append
 * data). Nothing of the entropy-coded data is decoded. */
bool wf_jpeg_structure_ok(const uint8_t *data, size_t size, const char **why);

/* GIF: header, colour tables and every data sub-block inside the buffer, at
 * least one image, and the trailer reached. */
bool wf_gif_structure_ok(const uint8_t *data, size_t size, const char **why);

/* WebP: RIFF/WEBP header whose declared size the buffer actually holds. */
bool wf_webp_structure_ok(const uint8_t *data, size_t size, const char **why);

#endif

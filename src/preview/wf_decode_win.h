#ifndef WF_DECODE_WIN_H
#define WF_DECODE_WIN_H

#include <windows.h>

#include <stddef.h>
#include <stdint.h>

#include "wf_decode_wire.h"
#include "wf_pixmap.h"

/*
 * The decoders, in-process: Windows Imaging Component for pictures and
 * Windows.Data.Pdf for PDF pages. Product decoding runs inside the sandboxed
 * host (D-007, D-026). Both decoders receive the whole file as bytes, rather
 * than opening a path, and return a pixmap no larger than the requested box.
 *
 * On anything but WF_DSTAT_OK `*out` is NULL. `info` is filled as far as it
 * is known (a PDF refused for want of a password still says nothing about
 * its pages; a page number out of range still says how many there are).
 * Thread-safe: each call initialises COM (multithreaded) for itself.
 */
/* Pixels the source may have before it is refused as a decompression bomb;
 * the output is small whatever this is, the time to produce it is not. */
#define WF_DECODE_SOURCE_PIXELS (256ull * 1000 * 1000)

/* The status for "WIC would not open these bytes" (exposed for its test). */
WfDecodeStatus wf_decode_open_failure(HRESULT hr, const void *data, size_t n);

WfDecodeStatus wf_decode_image(const void *data,
        size_t n,
        uint32_t box_w,
        uint32_t box_h,
        WfPixmap **out,
        WfDecodeInfo *info);

/* `password` is NULL or a NUL-terminated UTF-16 string. */
WfDecodeStatus wf_decode_pdf(const void *data,
        size_t n,
        uint32_t page,
        uint32_t box_w,
        uint32_t box_h,
        const wchar_t *password,
        WfPixmap **out,
        WfDecodeInfo *info);

/*
 * Renders one page to PNG bytes through Windows.Data.Pdf (malloc'd, the caller
 * frees). The page is rendered at the largest size that fits the box, so a
 * larger box means a sharper page, not a scaled one.
 */
WfDecodeStatus wf_pdf_render_png(const void *data,
        size_t n,
        uint32_t page,
        uint32_t box_w,
        uint32_t box_h,
        const wchar_t *password,
        void **png,
        size_t *png_n,
        WfDecodeInfo *info);

#endif

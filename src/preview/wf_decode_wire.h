#ifndef WF_DECODE_WIRE_H
#define WF_DECODE_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The conversation between the app and the decoder host (the sandboxed
 * process that turns an image or a PDF page into pixels). Pure byte layout
 * and validation, no I/O: both sides read a fixed-size header first, check
 * it, and only then read exactly the payload the header promises, so neither
 * ever allocates or reads on the strength of an unchecked length.
 *
 * All integers little-endian. One request, one reply. For a file on disk,
 * the parent duplicates a read-only handle INTO the host process and names
 * it in the request. An archive entry uses INLINE: its bytes follow the
 * password on the pipe, without a temporary file. Neither form sends a path.
 *
 *   request, 56 bytes + the password:
 *      0 "WFRQ"  4 u32 version  8 u64 request id
 *     16 u32 kind (1 image, 2 pdf)  20 u32 box_w  24 u32 box_h (pixels)
 *     28 u32 page (0-based, pdf)    32 u64 input size in bytes
 *     40 u32 password length in UTF-16 units (pdf, may be 0)  44 u32 flags
 *        bit 0 INLINE: the file's bytes follow the password on this pipe
 *        (an archive entry has no file to open); the handle field is then 0
 *     48 u64 the input file's handle, as numbered in the HOST's handle table
 *     then password_units * 2 bytes UTF-16LE
 *
 *   reply, 56 bytes + the raster:
 *      0 "WFRS"  4 u32 version  8 u64 request id  16 u32 status
 *     20 u32 page count (pdf; 1 for an image)
 *     24 u32 source width  28 u32 source height (page size in pixels at 96 dpi)
 *     32 u32 width  36 u32 height of the raster that follows
 *     40 u32 flags (bit 0: the source has transparency)  44 u32 0
 *     48 u64 payload bytes = width * height * 4 for OK, 0 otherwise
 *     then the raster, premultiplied BGRA, rows contiguous
 */
#define WF_DWIRE_VERSION 1u
#define WF_DWIRE_REQUEST_HEADER 56u
#define WF_DWIRE_REPLY_HEADER 56u
#define WF_DWIRE_PASSWORD_MAX 256u        /* UTF-16 units */
#define WF_DWIRE_RASTER_MAX (16u << 20)   /* bytes of pixels in a reply */
#define WF_DWIRE_INPUT_MAX (128ull << 20) /* bytes of file the host will take */
#define WF_DWIRE_BOX_MAX 8192u            /* either side of the requested box */

typedef enum
{
	WF_DKIND_IMAGE = 1,
	WF_DKIND_PDF = 2
} WfDecodeKind;

typedef enum
{
	WF_DSTAT_OK = 0,
	WF_DSTAT_UNSUPPORTED,       /* not a format we decode */
	WF_DSTAT_CORRUPT,           /* damaged or truncated */
	WF_DSTAT_PASSWORD_REQUIRED, /* encrypted PDF, none given */
	WF_DSTAT_PASSWORD_WRONG,
	WF_DSTAT_TOO_LARGE,  /* beyond the pixel or byte limits */
	WF_DSTAT_PAGE_RANGE, /* the page does not exist */
	WF_DSTAT_TIMEOUT,    /* the host gave up on it */
	WF_DSTAT_INTERNAL,
	WF_DSTAT_HOST_LOST, /* made up by the app: no reply came */
	WF_DSTAT_ABORTED, /* made up by the app: overtaken by a newer request */
	WF_DSTAT_COUNT
} WfDecodeStatus;

enum
{
	WF_DFLAG_ALPHA = 1u << 0 /* reply: the source has transparency */
};

enum
{
	WF_DREQ_INLINE = 1u << 0 /* request: the file's bytes follow */
};

/* What a decoder tells about the file besides the pixels. */
typedef struct
{
	uint32_t page_count, src_w, src_h, flags;
} WfDecodeInfo;

typedef struct
{
	uint64_t request;
	uint32_t kind, box_w, box_h, page;
	uint64_t input_size;
	uint32_t password_units;
	uint32_t flags;        /* WF_DREQ_* */
	uint64_t input_handle; /* valid in the host process only; 0 if INLINE */
} WfDecodeRequest;

typedef struct
{
	uint64_t request;
	uint32_t status, page_count, src_w, src_h, out_w, out_h, flags;
	uint64_t payload;
} WfDecodeReply;

/* Bytes the encoded request takes, or 0 if it is not valid. */
size_t wf_dwire_request_size(const WfDecodeRequest *r);
/* Writes header and password (UTF-16 units, `r->password_units` of them,
 * may be NULL when 0); returns the bytes written, 0 on invalid input or a
 * buffer that is too small. */
size_t wf_dwire_request_encode(const WfDecodeRequest *r,
        const uint16_t *password,
        void *out,
        size_t cap);
/* Validates the 56-byte header alone; fills `r`. The password bytes that
 * follow are `r->password_units * 2` long. */
bool wf_dwire_request_header(const void *hdr, WfDecodeRequest *r);

void wf_dwire_reply_encode(const WfDecodeReply *r, void *out56);
/*
 * Validates a 56-byte reply header against the request it must answer.
 * OK needs a raster no larger than WF_DWIRE_RASTER_MAX that exactly fills
 * width x height and fits the requested box; every other status must carry
 * no raster. Fills `r` only on success.
 */
bool wf_dwire_reply_header(
        const void *hdr, const WfDecodeRequest *asked, WfDecodeReply *r);

#endif

#ifndef WF_EENC_H
#define WF_EENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"

/*
 * What encoding a file is in, how to turn it into the UTF-8 the editor holds,
 * and how to turn it back byte for byte on save.
 *
 * Detection proposes TEXT for a byte order mark or strict UTF-8. BOM-less
 * UTF-16 and fallback code pages are AMBIGUOUS and require an explicit choice;
 * recognized binary signatures, NUL patterns and control density can yield
 * BINARY. Decoding validates the contents after detection.
 * Code-page decoding also requires encoding the result back to the same bytes.
 */
typedef enum
{
	WF_ENC_UTF8,
	WF_ENC_UTF8_BOM,
	WF_ENC_UTF16LE, /* with its byte order mark, as Windows writes it */
	WF_ENC_UTF16BE,
	WF_ENC_UTF16LE_RAW, /* no byte order mark: only by explicit choice */
	WF_ENC_UTF16BE_RAW,
	WF_ENC_CODEPAGE /* a Windows code page the configuration allows */
} WfEncKind;

typedef struct
{
	WfEncKind kind;
	unsigned codepage; /* WF_ENC_CODEPAGE only */
} WfEncoding;

typedef enum
{
	WF_DETECT_TEXT,
	WF_DETECT_BINARY,
	WF_DETECT_AMBIGUOUS
} WfDetectKind;

typedef struct
{
	WfDetectKind kind;
	WfEncoding enc;     /* TEXT: the encoding; AMBIGUOUS: the candidate */
	bool candidate_ok;  /* AMBIGUOUS: opening as `enc` is offered */
	const char *reason; /* static text for BINARY and AMBIGUOUS */
} WfDetect;

/* Judges the whole file. `fallback_codepage` (0 = none) is the one code page
 * the user allowed for text that is not UTF-8; it is only ever a candidate. */
WfDetect wf_enc_detect(
        const uint8_t *bytes, size_t n, unsigned fallback_codepage);

/* Decodes to malloc'd, NUL-terminated UTF-8 without embedded NUL. The caller
 * frees *utf8. For invalid encoded input, *bad (optional) receives a source
 * byte offset when available; code-page failures may report 0. Allocation
 * failures and some invalid arguments do not set *bad. */
WfStatus wf_enc_decode(const uint8_t *bytes,
        size_t n,
        WfEncoding enc,
        char **utf8,
        size_t *utf8_n,
        size_t *bad);

/* Encodes UTF-8 into malloc'd bytes for saving; the caller frees *bytes and
 * uses *bytes_n (the result is not necessarily NUL-terminated). A scalar with
 * no code-page mapping returns WF_E_INVAL and a UTF-8 offset in optional *bad,
 * rather than substituting a default character. */
WfStatus wf_enc_encode(const char *utf8,
        size_t n,
        WfEncoding enc,
        uint8_t **bytes,
        size_t *bytes_n,
        size_t *bad);

/* Strict UTF-8 validation: no overlongs, surrogates, > U+10FFFF or NUL. */
bool wf_utf8_strict(const char *s, size_t n, size_t *bad);

/* "UTF-8", "UTF-8 BOM", "UTF-16 LE", "UTF-16 BE", "CP932" ... */
void wf_enc_name(WfEncoding enc, char *out, size_t cap);

#endif

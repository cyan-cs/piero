/* libFuzzer harness: encoding detection, decode and the encode round trip.
 * Invariants: decode output is strict UTF-8 without NUL and NUL-terminated;
 * encoding that output again succeeds for every Unicode encoding and decodes
 * back to the same text. Build: see tools/fuzz.ps1. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "edit/wf_eenc.h"
#include "base/wf_macro.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const WfEncKind kinds[] = {WF_ENC_UTF8, WF_ENC_UTF8_BOM,
	        WF_ENC_UTF16LE, WF_ENC_UTF16BE, WF_ENC_UTF16LE_RAW,
	        WF_ENC_UTF16BE_RAW};
	WfDetect d = wf_enc_detect(data, size, 932);

	(void)d;
	for (size_t k = 0; k < WF_ARRAY_LEN(kinds); k++) {
		WfEncoding enc = {kinds[k], 0};
		char *u = NULL;
		size_t un = 0, bad = 0;

		if (WF_OK != wf_enc_decode(data, size, enc, &u, &un, &bad)) {
			continue;
		}
		if ('\0' != u[un] || memchr(u, 0, un) ||
		        !wf_utf8_strict(u, un, &bad)) {
			abort();
		}
		{
			uint8_t *b = NULL;
			size_t bn = 0;
			char *u2 = NULL;
			size_t u2n = 0;

			if (WF_OK != wf_enc_encode(u, un, enc, &b, &bn, &bad)) {
				abort();
			}
			if (WF_OK !=
			                wf_enc_decode(
			                        b, bn, enc, &u2, &u2n, &bad) ||
			        u2n != un || memcmp(u, u2, un)) {
				abort();
			}
			free(b);
			free(u2);
		}
		free(u);
	}
	return 0;
}

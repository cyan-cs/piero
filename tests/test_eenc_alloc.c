/* A conversion either completes or reports allocation failure, including the
 * reverse conversion used to validate legacy code pages. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "base/wf_macro.h"
#include "wf_test.h"

static size_t attempts, fail_at, owned;

static void *conversion_malloc(size_t n)
{
	void *p;

	if (++attempts == fail_at) {
		return NULL;
	}
	p = malloc(n);
	if (p) {
		owned++;
	}
	return p;
}

static void conversion_free(void *p)
{
	if (p) {
		CHECK(owned > 0);
		owned--;
	}
	free(p);
}

#define malloc conversion_malloc
#define free conversion_free
#ifndef WF_EENC_SOURCE
#	define WF_EENC_SOURCE "../src/edit/wf_eenc.c"
#endif
#include WF_EENC_SOURCE
#undef free
#undef malloc

static void conversion_failures(WfEncoding enc)
{
	static const char text[] = "text \xE3\x81\x82";
	uint8_t *encoded = NULL;
	char *decoded = NULL;
	size_t encoded_n = 0, decoded_n = 0;

	attempts = 0;
	fail_at = 0;
	CHECK(WF_OK ==
	        wf_enc_encode(text, sizeof text - 1, enc, &encoded, &encoded_n,
	                NULL));
	size_t encode_attempts = attempts;
	if (!encoded) {
		return;
	}
	for (size_t fail = 1; fail <= encode_attempts; ++fail) {
		uint8_t *out = NULL;
		size_t n = 0;

		attempts = 0;
		fail_at = fail;
		CHECK(WF_E_NOMEM ==
		        wf_enc_encode(
		                text, sizeof text - 1, enc, &out, &n, NULL));
		CHECK(!out && !n && owned == 1);
		conversion_free(out);
	}
	attempts = 0;
	fail_at = 0;
	CHECK(WF_OK ==
	        wf_enc_decode(
	                encoded, encoded_n, enc, &decoded, &decoded_n, NULL));
	size_t decode_attempts = attempts;
	CHECK(decoded && decoded_n == sizeof text - 1 &&
	        !memcmp(decoded, text, decoded_n));
	conversion_free(decoded);
	for (size_t fail = 1; fail <= decode_attempts; ++fail) {
		char *out = NULL;
		size_t n = 0;

		attempts = 0;
		fail_at = fail;
		CHECK(WF_E_NOMEM ==
		        wf_enc_decode(encoded, encoded_n, enc, &out, &n, NULL));
		CHECK(!out && !n && owned == 1);
		conversion_free(out);
	}
	conversion_free(encoded);
	CHECK(!owned);
}

int main(void)
{
	const WfEncoding encodings[] = {{WF_ENC_UTF8, 0}, {WF_ENC_UTF8_BOM, 0},
	        {WF_ENC_UTF16LE, 0}, {WF_ENC_UTF16BE, 0},
	        {WF_ENC_UTF16LE_RAW, 0}, {WF_ENC_UTF16BE_RAW, 0},
	        {WF_ENC_CODEPAGE, 932}};

	for (size_t i = 0; i < WF_ARRAY_LEN(encodings); ++i) {
		conversion_failures(encodings[i]);
	}
	return TEST_RESULT();
}

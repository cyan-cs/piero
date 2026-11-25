/* Encoding detection, strict decoding and byte-exact re-encoding. */
#include <stdlib.h>
#include <string.h>

#include "edit/wf_eenc.h"
#include "wf_test.h"

#define BYTES(s) ((const uint8_t *)(s))

static WfEncoding enc(WfEncKind k)
{
	WfEncoding e = {k, 0};

	return e;
}

static void strict_utf8_rejects_what_is_not_text(void)
{
	size_t bad = 99;

	CHECK(wf_utf8_strict("plain ascii", 11, NULL));
	CHECK(wf_utf8_strict("\xE6\x97\xA5\xF0\x9F\x98\x80", 7, NULL));
	CHECK(!wf_utf8_strict("\xC0\x80", 2, &bad) && 0 == bad); /* overlong */
	CHECK(!wf_utf8_strict("ab\xED\xA0\x80", 5, &bad) &&
	        2 == bad);                                   /* surrogate */
	CHECK(!wf_utf8_strict("\xF4\x90\x80\x80", 4, NULL)); /* > U+10FFFF */
	CHECK(!wf_utf8_strict("ab\xE6\x97", 4, &bad) &&
	        2 == bad);                                   /* truncated */
	CHECK(!wf_utf8_strict("a\0b", 3, &bad) && 1 == bad); /* NUL */
	CHECK(!wf_utf8_strict("\xFF", 1, NULL));
}

static void decode_utf8_and_bom(void)
{
	char *u;
	size_t n;

	CHECK(wf_enc_decode(BYTES("\xEF\xBB\xBF"
	                          "hi"),
	              5, enc(WF_ENC_UTF8_BOM), &u, &n, NULL) == WF_OK &&
	        2 == n && !memcmp(u, "hi", 2));
	free(u);
	/* the kind says BOM but there is none */
	CHECK(wf_enc_decode(BYTES("hi"), 2, enc(WF_ENC_UTF8_BOM), &u, &n,
	              NULL) == WF_E_INVAL);
}

static void empty_text_and_size_limits(void)
{
	char *text = NULL;
	uint8_t *bytes = NULL;
	size_t n = 0;

	CHECK(WF_OK ==
	        wf_enc_decode(NULL, 0, enc(WF_ENC_UTF8), &text, &n, NULL));
	CHECK(text && !text[0] && !n);
	free(text);
	CHECK(WF_OK ==
	        wf_enc_encode(NULL, 0, enc(WF_ENC_UTF8), &bytes, &n, NULL));
	CHECK(bytes && !n);
	free(bytes);
	CHECK(WF_E_RANGE ==
	        wf_enc_decode(BYTES(""), SIZE_MAX, enc(WF_ENC_UTF8), &text, &n,
	                NULL));
	CHECK(!text && !n);
	CHECK(WF_E_RANGE ==
	        wf_enc_encode("", SIZE_MAX - 2, enc(WF_ENC_UTF8_BOM), &bytes,
	                &n, NULL));
	CHECK(!bytes && !n);
}

static void utf16_round_trips_with_pairs_and_rejects_lone_surrogates(void)
{
	/* "a" U+1F600 "b", little endian with BOM */
	static const uint8_t LE[] = {
	        0xFF, 0xFE, 'a', 0, 0x3D, 0xD8, 0x00, 0xDE, 'b', 0};
	static const uint8_t BE[] = {
	        0xFE, 0xFF, 0, 'a', 0xD8, 0x3D, 0xDE, 0x00, 0, 'b'};
	static const uint8_t LONE[] = {0xFF, 0xFE, 'a', 0, 0x3D, 0xD8, 'b', 0};
	static const uint8_t ODD[] = {0xFF, 0xFE, 'a', 0, 'b'};
	static const uint8_t NUL16[] = {0xFF, 0xFE, 'a', 0, 0, 0};
	char *u;
	uint8_t *b;
	size_t n, bn, bad = 99;

	CHECK(WF_OK ==
	        wf_enc_decode(
	                LE, sizeof LE, enc(WF_ENC_UTF16LE), &u, &n, NULL));
	CHECK(6 == n &&
	        !memcmp(u,
	                "a\xF0\x9F\x98\x80"
	                "b",
	                6));
	CHECK(WF_OK ==
	                wf_enc_encode(
	                        u, n, enc(WF_ENC_UTF16LE), &b, &bn, NULL) &&
	        bn == sizeof LE && !memcmp(b, LE, bn));
	free(b);
	CHECK(WF_OK ==
	                wf_enc_encode(
	                        u, n, enc(WF_ENC_UTF16BE), &b, &bn, NULL) &&
	        bn == sizeof BE && !memcmp(b, BE, bn));
	free(b);
	free(u);
	CHECK(WF_OK ==
	                wf_enc_decode(BE, sizeof BE, enc(WF_ENC_UTF16BE), &u,
	                        &n, NULL) &&
	        6 == n);
	free(u);
	CHECK(wf_enc_decode(LONE, sizeof LONE, enc(WF_ENC_UTF16LE), &u, &n,
	              &bad) == WF_E_INVAL &&
	        4 == bad);
	CHECK(wf_enc_decode(ODD, sizeof ODD, enc(WF_ENC_UTF16LE), &u, &n,
	              NULL) == WF_E_INVAL);
	CHECK(wf_enc_decode(NUL16, sizeof NUL16, enc(WF_ENC_UTF16LE), &u, &n,
	              NULL) == WF_E_INVAL);
	/* a file in the wrong encoding is refused rather than reinterpreted */
	CHECK(WF_OK !=
	                wf_enc_decode(BE, sizeof BE, enc(WF_ENC_UTF16LE), &u,
	                        &n, NULL) ||
	        6 != n);
}

static void detection_follows_the_rules(void)
{
	static const uint8_t LE_NOBOM[] = {
	        'h', 0, 'e', 0, 'l', 0, 'l', 0, 'o', 0, ' ', 0, 'w', 0, 'o', 0};
	static const uint8_t BIN[] = {'a', 0, 0, 0xFF, 1, 2, 3, 0, 0, 0};
	WfDetect d;

	d = wf_enc_detect(BYTES("hello\n"), 6, 0);
	CHECK(WF_DETECT_TEXT == d.kind && WF_ENC_UTF8 == d.enc.kind);
	d = wf_enc_detect(BYTES(""), 0, 0);
	CHECK(WF_DETECT_TEXT == d.kind);
	d = wf_enc_detect(BYTES("\xEF\xBB\xBF"
	                        "x"),
	        4, 0);
	CHECK(WF_DETECT_TEXT == d.kind && WF_ENC_UTF8_BOM == d.enc.kind);
	d = wf_enc_detect(BYTES("\xFF\xFE"
	                        "a\0"),
	        4, 0);
	CHECK(WF_DETECT_TEXT == d.kind && WF_ENC_UTF16LE == d.enc.kind);
	d = wf_enc_detect(BYTES("\xFE\xFF\0"
	                        "a"),
	        4, 0);
	CHECK(WF_DETECT_TEXT == d.kind && WF_ENC_UTF16BE == d.enc.kind);

	/* NUL bytes alone do not make UTF-16 binary; but without a BOM it is
	 * never opened without being asked */
	d = wf_enc_detect(LE_NOBOM, sizeof LE_NOBOM, 0);
	CHECK(WF_DETECT_AMBIGUOUS == d.kind &&
	        WF_ENC_UTF16LE_RAW == d.enc.kind && d.candidate_ok);
	d = wf_enc_detect(BIN, sizeof BIN, 0);
	CHECK(WF_DETECT_BINARY == d.kind && d.reason);

	/* known signatures win over a text-looking extension */
	d = wf_enc_detect(BYTES("\x89PNG\r\n\x1A\n"), 8, 0);
	CHECK(WF_DETECT_BINARY == d.kind && !strcmp(d.reason, "PNG image"));
	d = wf_enc_detect(BYTES("%PDF-1.7\n"), 9, 0);
	CHECK(WF_DETECT_BINARY == d.kind);
	d = wf_enc_detect(BYTES("MZ\x90\0\x03\0\0\0"), 8, 0);
	CHECK(WF_DETECT_BINARY == d.kind);

	/* control characters */
	d = wf_enc_detect(BYTES("a\x01"
	                        "b"),
	        3, 0);
	CHECK(WF_DETECT_BINARY == d.kind);
	d = wf_enc_detect(BYTES("tab\there\r\n\x1B[31mred\x1B[0m\n"), 23, 0);
	CHECK(WF_DETECT_TEXT == d.kind);

	/* not UTF-8: ambiguous, offered only when a code page is allowed */
	d = wf_enc_detect(BYTES("caf\xE9"), 4, 0);
	CHECK(WF_DETECT_AMBIGUOUS == d.kind && !d.candidate_ok);
	d = wf_enc_detect(BYTES("caf\xE9"), 4, 1252);
	CHECK(WF_DETECT_AMBIGUOUS == d.kind && d.candidate_ok &&
	        WF_ENC_CODEPAGE == d.enc.kind && 1252 == d.enc.codepage);
}

static void code_pages_open_only_when_they_round_trip(void)
{
	WfEncoding cp932 = {WF_ENC_CODEPAGE, 932};
	char *u;
	uint8_t *b;
	size_t n, bn, bad = 99;

	/* "あ" in Shift_JIS */
	CHECK(WF_OK ==
	                wf_enc_decode(
	                        BYTES("\x82\xA0"), 2, cp932, &u, &n, NULL) &&
	        3 == n && !memcmp(u, "\xE3\x81\x82", 3));
	CHECK(WF_OK == wf_enc_encode(u, n, cp932, &b, &bn, NULL) && 2 == bn &&
	        !memcmp(b, "\x82\xA0", 2));
	free(b);
	free(u);
	/* a lead byte without its trail byte is not text in this code page */
	CHECK(WF_OK != wf_enc_decode(BYTES("ab\x82"), 3, cp932, &u, &n, NULL));
	/* nothing is substituted: an emoji has no CP932 form */
	CHECK(WF_E_INVAL ==
	        wf_enc_encode("ab\xF0\x9F\x98\x80", 6, cp932, &b, &bn, &bad));
	CHECK(2 == bad);
	/* Latin-1 era text */
	{
		WfEncoding w1252 = {WF_ENC_CODEPAGE, 1252};

		CHECK(WF_OK ==
		                wf_enc_decode(BYTES("caf\xE9"), 4, w1252, &u,
		                        &n, NULL) &&
		        5 == n && !memcmp(u, "caf\xC3\xA9", 5));
		free(u);
		CHECK(WF_E_INVAL ==
		                wf_enc_encode("\xE6\x97\xA5", 3, w1252, &b, &bn,
		                        &bad) &&
		        0 == bad);
	}
}

static void names_are_readable(void)
{
	char name[32];
	WfEncoding e = {WF_ENC_CODEPAGE, 932};

	wf_enc_name(e, name, sizeof name);
	CHECK(!strcmp(name, "CP932"));
	wf_enc_name(enc(WF_ENC_UTF16LE), name, sizeof name);
	CHECK(!strcmp(name, "UTF-16 LE"));
}

int main(void)
{
	strict_utf8_rejects_what_is_not_text();
	decode_utf8_and_bom();
	empty_text_and_size_limits();
	utf16_round_trips_with_pairs_and_rejects_lone_surrogates();
	detection_follows_the_rules();
	code_pages_open_only_when_they_round_trip();
	names_are_readable();
	return TEST_RESULT();
}

/* Structural refusal of damaged PNG / ICO input (P0-A03 root cause). The
 * reference file is the well-known 67-byte 1x1 PNG, whose chunk CRCs come from
 * the specification, not from the code under test. */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "preview/wf_image_check.h"
#include "wf_test.h"

static const uint8_t PNG1x1[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
        0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63,
        0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00,
        0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

static void valid_png_is_accepted(void)
{
	const char *why = NULL;

	CHECK(sizeof PNG1x1 == 67);
	CHECK(wf_png_structure_ok(PNG1x1, sizeof PNG1x1, &why));
	CHECK(wf_png_structure_ok(PNG1x1, sizeof PNG1x1, NULL));
}

/* Every proper prefix is a truncated file: none may be accepted. This is the
 * measured WIC behaviour (a 100-byte prefix "decodes"). */
static void every_truncation_is_refused(void)
{
	for (size_t n = 0; n < sizeof PNG1x1; n++) {
		const char *why = "";

		CHECK(!wf_png_structure_ok(PNG1x1, n, &why));
		CHECK(why && why[0]);
	}
}

/* Damage to any single byte after the signature is caught by a CRC, a length
 * or the chunk order: nothing is silently accepted. */
static void every_single_byte_flip_is_refused(void)
{
	uint8_t copy[sizeof PNG1x1];

	for (size_t i = 0; i < sizeof PNG1x1; i++) {
		memcpy(copy, PNG1x1, sizeof copy);
		copy[i] ^= 0x01;
		CHECK(!wf_png_structure_ok(copy, sizeof copy, NULL));
		copy[i] = PNG1x1[i] ^ 0xFF;
		CHECK(!wf_png_structure_ok(copy, sizeof copy, NULL));
	}
}

static void structural_rules(void)
{
	uint8_t buf[sizeof PNG1x1 + 16];
	const char *why = "";

	/* data after IEND */
	memcpy(buf, PNG1x1, sizeof PNG1x1);
	buf[sizeof PNG1x1] = 0;
	CHECK(!wf_png_structure_ok(buf, sizeof PNG1x1 + 1, &why));
	CHECK(strstr(why, "after IEND"));
	/* the damage the sandbox corpus uses: two bytes in the middle */
	memcpy(buf, PNG1x1, sizeof PNG1x1);
	buf[33] ^= 0xFF;
	buf[34] ^= 0x55;
	CHECK(!wf_png_structure_ok(buf, sizeof PNG1x1, &why));
	/* NULL / empty */
	CHECK(!wf_png_structure_ok(NULL, 0, &why));
	CHECK(!wf_png_structure_ok(PNG1x1, 0, &why));
	/* a chunk length that points past the end must not be followed */
	memcpy(buf, PNG1x1, sizeof PNG1x1);
	buf[8] = 0x7F;
	buf[9] = 0xFF;
	CHECK(!wf_png_structure_ok(buf, sizeof PNG1x1, &why));
	CHECK(strstr(why, "exceeds"));
}

/* ICO: a container whose embedded PNG is damaged is refused; a plain payload
 * only has to lie inside the file. */
static void ico_container(void)
{
	uint8_t ico[6 + 16 + sizeof PNG1x1];
	const char *why = "";

	memset(ico, 0, sizeof ico);
	ico[2] = 1;
	ico[4] = 1;
	ico[6 + 8] = (uint8_t)sizeof PNG1x1;
	ico[6 + 12] = 22;
	memcpy(ico + 22, PNG1x1, sizeof PNG1x1);
	CHECK(wf_ico_structure_ok(ico, sizeof ico, &why));
	ico[22 + 40] ^= 0x01; /* inside the IDAT data */
	CHECK(!wf_ico_structure_ok(ico, sizeof ico, &why));
	CHECK(strstr(why, "CRC"));
	ico[22 + 40] ^= 0x01;
	CHECK(!wf_ico_structure_ok(ico, sizeof ico - 1, &why)); /* cut short */
	ico[6 + 12] = 200; /* offset beyond the file */
	CHECK(!wf_ico_structure_ok(ico, sizeof ico, &why));
	CHECK(!wf_ico_structure_ok(ico, 4, &why));
	CHECK(!wf_ico_structure_ok(NULL, 0, &why));
	/* a non-PNG payload (a DIB) is only bounds-checked */
	ico[6 + 12] = 22;
	memcpy(ico + 22, "BMPDATA-NOT-A-PNG-AT-ALL-BUT-INSIDE-THE-FILE-xxxxxxx",
	        sizeof PNG1x1 < 52 ? sizeof PNG1x1 : 52);
	CHECK(wf_ico_structure_ok(ico, sizeof ico, &why));
}

/* SOI, APP0, DQT-ish, SOF0, DHT-ish, SOS with entropy data that contains a
 * stuffed byte, a restart marker and a fill byte, then EOI. */
static const uint8_t JPEG[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x4A, 0x46,
        0xFF, 0xDB, 0x00, 0x03, 0x01, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x08,
        0x00, 0x08, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xC4, 0x00, 0x02, 0xFF, 0xDA,
        0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00, 0x12, 0x34, 0xFF, 0x00,
        0x56, 0xFF, 0xD0, 0x78, 0x9A, 0xFF, 0xFF, 0xD9};

static void jpeg_structure(void)
{
	uint8_t buf[sizeof JPEG + 16];
	const char *why = NULL;

	CHECK(wf_jpeg_structure_ok(JPEG, sizeof JPEG, &why));
	/* bytes after EOI are tolerated */
	memcpy(buf, JPEG, sizeof JPEG);
	memset(buf + sizeof JPEG, 0x5A, 16);
	CHECK(wf_jpeg_structure_ok(buf, sizeof buf, NULL));
	/* every proper prefix is a cut-off file */
	for (size_t n = 0; n < sizeof JPEG; n++) {
		why = "";
		if (wf_jpeg_structure_ok(JPEG, n, &why)) {
			printf("  prefix %zu of a JPEG accepted\n", n);
			CHECK_FAIL("truncated JPEG accepted");
		}
	}
	/* not a JPEG, a stray start marker, no frame, no scan */
	CHECK(!wf_jpeg_structure_ok(NULL, 10, &why));
	memcpy(buf, JPEG, sizeof JPEG);
	buf[0] = 0xFE;
	CHECK(!wf_jpeg_structure_ok(buf, sizeof JPEG, &why));
	memcpy(buf, JPEG, sizeof JPEG);
	buf[14] = 0xC4; /* the frame header becomes a table: no SOF */
	CHECK(!wf_jpeg_structure_ok(buf, sizeof JPEG, &why));
	memcpy(buf, JPEG, sizeof JPEG);
	buf[sizeof JPEG - 3] =
	        0xFF; /* EOI right after the table, before a scan */
	{
		uint8_t no_scan[] = {
		        0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x03, 0x00, 0xFF, 0xD9};

		CHECK(!wf_jpeg_structure_ok(no_scan, sizeof no_scan, &why));
	}
	/* a segment whose length runs past the end */
	memcpy(buf, JPEG, sizeof JPEG);
	buf[5] = 0xF0;
	CHECK(!wf_jpeg_structure_ok(buf, sizeof JPEG, &why));
	/* a length of 0 or 1 is not a length */
	memcpy(buf, JPEG, sizeof JPEG);
	buf[5] = 0x01;
	CHECK(!wf_jpeg_structure_ok(buf, sizeof JPEG, &why));
}

/* the classic transparent 1x1 GIF */
static const uint8_t GIF[] = {'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0,
        0, 0xFF, 0xFF, 0xFF, 0, 0, 0, '!', 0xF9, 4, 1, 0, 0, 0, 0, ',', 0, 0, 0,
        0, 1, 0, 1, 0, 0, 2, 2, 0x44, 1, 0, ';'};

static void gif_structure(void)
{
	uint8_t buf[sizeof GIF + 4];
	const char *why = NULL;

	CHECK(wf_gif_structure_ok(GIF, sizeof GIF, &why));
	memcpy(buf, GIF, sizeof GIF);
	memset(buf + sizeof GIF, 0, 4);
	CHECK(wf_gif_structure_ok(buf, sizeof buf, NULL));
	for (size_t n = 0; n < sizeof GIF; n++) {
		if (wf_gif_structure_ok(GIF, n, &why)) {
			printf("  prefix %zu of a GIF accepted\n", n);
			CHECK_FAIL("truncated GIF accepted");
		}
	}
	memcpy(buf, GIF, sizeof GIF);
	buf[0] = 'X';
	CHECK(!wf_gif_structure_ok(buf, sizeof GIF, &why));
	memcpy(buf, GIF, sizeof GIF);
	buf[19] = 0x3F; /* the extension introducer becomes junk */
	CHECK(!wf_gif_structure_ok(buf, sizeof GIF, &why));
	/* a trailer with no picture before it */
	{
		uint8_t none[] = {
		        'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0, 0, 0, ';'};

		CHECK(!wf_gif_structure_ok(none, sizeof none, &why));
	}
}

static void webp_structure(void)
{
	uint8_t w[32] = {'R', 'I', 'F', 'F', 24, 0, 0, 0, 'W', 'E', 'B', 'P',
	        'V', 'P', '8', ' ', 12, 0, 0, 0};
	const char *why = NULL;

	CHECK(wf_webp_structure_ok(w, 32, &why));
	CHECK(wf_webp_structure_ok(w, 32, NULL));
	for (size_t n = 0; n < 32; n++) {
		if (n >= 32 && wf_webp_structure_ok(w, n, &why)) {
			CHECK_FAIL("unreachable");
		}
		if (n < 32) {
			CHECK(!wf_webp_structure_ok(w, n, &why));
		}
	}
	/* trailing bytes after the declared size are tolerated */
	CHECK(wf_webp_structure_ok(w, 32, &why));
	w[4] = 0xFF; /* declares more than the buffer has */
	CHECK(!wf_webp_structure_ok(w, 32, &why));
	w[4] = 24;
	w[8] = 'X';
	CHECK(!wf_webp_structure_ok(w, 32, &why));
}

/* The CRC-32 of the specification, written out here on its own. */
static uint32_t test_crc(const uint8_t *p, size_t n)
{
	uint32_t crc = 0xFFFFFFFFu;

	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++) {
			crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
		}
	}
	return crc ^ 0xFFFFFFFFu;
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* One chunk at `at`: length, type, data, CRC. Returns the end. */
static size_t put_chunk(
        uint8_t *at, const char *type, const uint8_t *data, size_t n)
{
	put_be32(at, (uint32_t)n);
	memcpy(at + 4, type, 4);
	if (n) {
		memcpy(at + 8, data, n);
	}
	put_be32(at + 8 + n, test_crc(at + 4, n + 4));
	return n + 12;
}

/* A PNG whose IDAT holds `idat_n` bytes (the data is not a picture; only the
 * structure is looked at). Returns its size. */
static size_t make_png(uint8_t *out, size_t idat_n)
{
	static const uint8_t head[8] = {
	        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	uint8_t ihdr[13] = {0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0};
	uint8_t *data = calloc(idat_n ? idat_n : 1, 1);
	size_t at = 8;

	memcpy(out, head, 8);
	at += put_chunk(out + at, "IHDR", ihdr, sizeof ihdr);
	at += put_chunk(out + at, "IDAT", data, idat_n);
	at += put_chunk(out + at, "IEND", NULL, 0);
	free(data);
	return at;
}

/* An ICO whose `count` entries all name the same PNG, which is `png_n` bytes,
 * as the pictures of one size often do. */
static void ico_many_entries_for_one_png(size_t count, size_t idat_n)
{
	size_t dir = 6 + 16 * count;
	uint8_t *ico = malloc(dir + idat_n + 64);
	size_t png_n = make_png(ico + dir, idat_n), total = dir + png_n;
	const char *why = "";
	clock_t t0;
	double sec;

	CHECK(ico != NULL);
	memset(ico, 0, dir);
	ico[2] = 1;
	ico[4] = (uint8_t)count;
	ico[5] = (uint8_t)(count >> 8);
	for (size_t i = 0; i < count; i++) {
		uint8_t *e = ico + 6 + 16 * i;

		e[8] = (uint8_t)png_n;
		e[9] = (uint8_t)(png_n >> 8);
		e[10] = (uint8_t)(png_n >> 16);
		e[11] = (uint8_t)(png_n >> 24);
		e[12] = (uint8_t)dir;
		e[13] = (uint8_t)(dir >> 8);
		e[14] = (uint8_t)(dir >> 16);
		e[15] = (uint8_t)(dir >> 24);
	}
	t0 = clock();
	CHECK(wf_ico_structure_ok(ico, total, &why));
	sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
	printf("info: ICO of %zu entries for one %zu-byte PNG: %.3f s\n", count,
	        png_n, sec);
	CHECK(sec < 5.0);
	ico[dir + png_n - 20] ^=
	        1; /* inside the IDAT or its CRC: every entry is refused */
	CHECK(!wf_ico_structure_ok(ico, total, &why));
	CHECK(strstr(why, "CRC") != NULL);
	free(ico);
}

static void ico_entries_sharing_pictures(void)
{
	/* the picture is checked once, however many entries name it */
	ico_many_entries_for_one_png(3000, 8u << 20);
	/* a bad picture among good ones is still found, by the first entry that names it */
	{
		size_t dir = 6 + 16 * 3, a, b, total;
		uint8_t *ico = calloc(1, dir + 2 * 200);
		const char *why = "";

		CHECK(ico != NULL);
		ico[2] = 1;
		ico[4] = 3;
		a = make_png(ico + dir, 20);
		b = make_png(ico + dir + a, 30);
		ico[dir + a + 20] ^= 1; /* the second picture is damaged */
		total = dir + a + b;
		for (int i = 0; i < 3;
		        i++) { /* entries: first, second, first again */
			uint8_t *e = ico + 6 + 16 * i;
			size_t off = 1 == i ? dir + a : dir,
			       len = 1 == i ? b : a;

			e[8] = (uint8_t)len;
			e[12] = (uint8_t)off;
		}
		CHECK(!wf_ico_structure_ok(ico, total, &why));
		ico[dir + a + 20] ^= 1; /* repaired */
		CHECK(wf_ico_structure_ok(ico, total, &why));
		free(ico);
	}
}

/* The checksum table is made on first use. Many threads arriving together at the
 * first use must all get right answers (and the table is written by one of
 * them, once). */
#define RACERS 16

static HANDLE g_go;
static volatile LONG g_bad;

static DWORD WINAPI racer(LPVOID unused)
{
	const char *why;

	(void)unused;
	WaitForSingleObject(g_go, INFINITE);
	for (int i = 0; i < 200; i++) {
		if (!wf_png_structure_ok(PNG1x1, sizeof PNG1x1, &why)) {
			InterlockedIncrement(&g_bad);
		}
	}
	return 0;
}

static void many_threads_at_the_first_use_agree(void)
{
	HANDLE t[RACERS];

	g_go = CreateEventW(NULL, TRUE, FALSE, NULL);
	for (int i = 0; i < RACERS; i++) {
		t[i] = CreateThread(NULL, 0, racer, NULL, 0, NULL);
	}
	SetEvent(g_go); /* all at once, before anything else has used the table */
	WaitForMultipleObjects(RACERS, t, TRUE, 30000);
	for (int i = 0; i < RACERS; i++) {
		CloseHandle(t[i]);
	}
	CloseHandle(g_go);
	CHECK(0 == g_bad);
}

int main(void)
{
	many_threads_at_the_first_use_agree(); /* first: the table is not made yet */
	valid_png_is_accepted();
	every_truncation_is_refused();
	every_single_byte_flip_is_refused();
	structural_rules();
	ico_container();
	ico_entries_sharing_pictures();
	jpeg_structure();
	gif_structure();
	webp_structure();
	return TEST_RESULT();
}

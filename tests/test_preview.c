#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "preview/wf_preview.h"
#include "wf_test.h"

static void test_strict_encoding_and_terminal_injection(void)
{
	WfPreview *p = NULL;
	const unsigned char le[] = {0xff, 0xfe, 'a', 0, 0x3d, 0xd8, 0, 0xde};
	const unsigned char be[] = {0xfe, 0xff, 0, 'b'};
	const unsigned char bad[][4] = {{0xc0, 0xaf, 0, 0},
	        {0xff, 0xfe, 0, 0xd8}, {0xfe, 0xff, 0, 0x61}};
	CHECK(wf_preview_decode("a\033[31m\n\tend\r\nx",
	              sizeof "a\033[31m\n\tend\r\nx" - 1, 0, &p) == WF_OK);
	CHECK(p && WF_PREVIEW_TEXT == p->kind);
	CHECK(p && 3 == p->nlines);
	if (p && 3 == p->nlines) {
		CHECK(!strcmp(p->text.data + p->lines[0], "a^[[31m"));
		CHECK(!strcmp(p->text.data + p->lines[1], "    end"));
		CHECK(!strcmp(p->text.data + p->lines[2], "x"));
		CHECK(NULL == strchr(p->text.data, '\033'));
	}
	wf_preview_free(p);
	p = NULL;
	CHECK(WF_OK == wf_preview_decode(le, sizeof le, 0, &p));
	CHECK(p && WF_PREVIEW_TEXT == p->kind &&
	        !strcmp(p->text.data, "a\xf0\x9f\x98\x80"));
	wf_preview_free(p);
	p = NULL;
	CHECK(WF_OK == wf_preview_decode(be, sizeof be, 0, &p));
	CHECK(p && WF_PREVIEW_TEXT == p->kind && !strcmp(p->text.data, "b"));
	wf_preview_free(p);
	p = NULL;
	CHECK(WF_OK == wf_preview_decode(bad[0], 2, 0, &p));
	CHECK(p && WF_PREVIEW_INVALID == p->kind);
	wf_preview_free(p);
	p = NULL;
	CHECK(WF_OK == wf_preview_decode(bad[1], 4, 0, &p));
	CHECK(p && WF_PREVIEW_INVALID == p->kind);
	wf_preview_free(p);
	p = NULL;
	CHECK(WF_OK == wf_preview_decode(bad[2], 3, 0, &p));
	CHECK(p && WF_PREVIEW_INVALID == p->kind);
	wf_preview_free(p);
}

static void test_bounds_and_policy(void)
{
	WfPreview *p = NULL;
	char *many = malloc(WF_PREVIEW_BYTES);
	CHECK(NULL != many);
	if (!many) {
		return;
	}
	memset(many, 'x', WF_PREVIEW_BYTES);
	CHECK(WF_OK == wf_preview_decode(many, WF_PREVIEW_BYTES, 1, &p));
	CHECK(p && p->truncated && WF_PREVIEW_TEXT == p->kind);
	CHECK(p && p->text.len <= WF_PREVIEW_LINE_BYTES + 1u);
	wf_preview_free(p);
	p = NULL;
	memset(many, '\n', WF_PREVIEW_BYTES);
	CHECK(WF_OK == wf_preview_decode(many, WF_PREVIEW_BYTES, 1, &p));
	CHECK(p && p->truncated && WF_PREVIEW_LINES == p->nlines);
	wf_preview_free(p);
	p = NULL;
	CHECK(WF_OK == wf_preview_decode("a\0b", 3, 0, &p));
	CHECK(p && WF_PREVIEW_BINARY == p->kind);
	wf_preview_free(p);
	p = NULL;
	CHECK(WF_OK == wf_preview_decode("\xe2\x82", 2, 1, &p));
	CHECK(p && WF_PREVIEW_TEXT == p->kind && p->truncated);
	wf_preview_free(p);
	p = NULL;
	CHECK(WF_OK == wf_preview_decode("\xe0\x80", 2, 1, &p));
	CHECK(p && WF_PREVIEW_INVALID == p->kind);
	wf_preview_free(p);
	p = NULL;
	CHECK(WF_OK == wf_preview_decode("\xed\xa0", 2, 1, &p));
	CHECK(p && WF_PREVIEW_INVALID == p->kind);
	wf_preview_free(p);
	p = NULL;
	CHECK(WF_E_INVAL ==
	        wf_preview_decode(many, WF_PREVIEW_HARD_BYTES + 1u, 0, &p));
	CHECK(NULL == p);
	CHECK(wf_preview_auto_allowed(L"C:\\local\\a.txt", 0));
	CHECK(!wf_preview_auto_allowed(L"\\\\server\\share\\a.txt", 0));
	CHECK(!wf_preview_auto_allowed(L"\\\\?\\UNC\\server\\share\\a.txt", 0));
	CHECK(!wf_preview_auto_allowed(L"C:\\cloud.txt", WF_ENT_CLOUD));
	CHECK(!wf_preview_auto_allowed(L"C:\\link.txt", WF_ENT_LINK));
	CHECK(wf_preview_is_image(L"X.PNG"));
	CHECK(!wf_preview_is_image(L"X.png.txt"));
	free(many);
}

/* What decoding a text of the largest size costs: 1 MiB of lines with a tab and
 * a control character in some, the same bytes every time, five runs. The check
 * is the result (the text is the same whatever the cost); the time is printed. */
static void test_decode_cost_of_a_large_text(void)
{
	enum
	{
		RUNS = 5,
		SIZE = 1024 * 1024
	};
	char *text = malloc(SIZE);
	size_t n = 0;
	double best = 1e9, worst = 0;

	CHECK(NULL != text);
	for (unsigned k = 0; text && n + 40 < SIZE; k++) {
		n += (size_t)snprintf(
		        text + n, 40, "line %u\tof \033text\n", k);
	}
	for (int r = 0; text && r < RUNS; r++) {
		WfPreview *p = NULL;
		LARGE_INTEGER a, b, f;
		double ms;

		QueryPerformanceFrequency(&f);
		QueryPerformanceCounter(&a);
		CHECK(WF_OK == wf_preview_decode(text, n, false, &p));
		QueryPerformanceCounter(&b);
		ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 /
		        (double)f.QuadPart;
		best = ms < best ? ms : best;
		worst = ms > worst ? ms : worst;
		CHECK(p && WF_PREVIEW_TEXT == p->kind && p->nlines > 1000);
		CHECK(p && NULL == strchr(p->text.data, '\033'));
		wf_preview_free(p);
	}
	printf("info: decode of %zu bytes: best %.1f ms, worst %.1f ms (%d "
	       "runs)\n",
	        n, best, worst, RUNS);
	free(text);
}

int main(void)
{
	test_strict_encoding_and_terminal_injection();
	test_bounds_and_policy();
	test_decode_cost_of_a_large_text();
	return TEST_RESULT();
}

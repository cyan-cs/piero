/* A refused allocation while the Markdown renderer builds a paragraph is an error,
 * never a document with some of its text missing (CODING_RULES section 6).
 * wf_buf.c is compiled into this test with its realloc replaced, so that the
 * Nth growth of any buffer the renderer uses can be refused; the library's own
 * copy of wf_buf.c is not linked because this object already defines it. */
#include <stdlib.h>
#include <string.h>

#include "wf_test.h"

static long calls;
static long refuse_at = -1;

static void *refusing_realloc(void *p, size_t n)
{
	if (calls++ == refuse_at) {
		return NULL;
	}
	return realloc(p, n);
}

#define realloc refusing_realloc
#include "../src/base/wf_buf.c"
#undef realloc

#include "../src/text/wf_markdown.h"

/* Every row of the document, joined by '\n'. */
static char *dump(const WfMdDoc *d)
{
	size_t cap = d->text_len + d->nlines * 2 + 16, k = 0;
	char *out = malloc(cap);

	if (!out) {
		return NULL;
	}
	for (size_t i = 0; i < d->nlines; i++) {
		k += wf_md_line_text(d, i, out + k, cap - k);
		out[k++] = '\n';
	}
	out[k] = '\0';
	return out;
}

/* Renders `src` with the `at`th buffer growth refused (-1: none). */
static WfStatus render_refusing(const char *src, long at, WfMdDoc **d)
{
	calls = 0;
	refuse_at = at;
	*d = NULL;
	return wf_md_render(src, strlen(src), 60, d);
}

static void every_refusal_is_an_error_or_the_whole_text(const char *src)
{
	WfMdDoc *d;
	char *want;
	long total;
	int refused = 0;

	CHECK(WF_OK == render_refusing(src, -1, &d));
	if (!d) {
		return;
	}
	want = dump(d);
	wf_md_free(d);
	total = calls;
	CHECK(want && total > 0);
	for (long at = 0; want && at < total; at++) {
		WfStatus st = render_refusing(src, at, &d);

		if (WF_OK != st) {
			refused++;
			CHECK(WF_E_NOMEM == st);
			CHECK(!d);
			continue;
		}
		CHECK(d);
		if (d) {
			char *got = dump(d);

			CHECK(got && !strcmp(got, want));
			free(got);
		}
		wf_md_free(d);
	}
	/* the test must have reached the failure path, or it proves nothing */
	CHECK(refused > 0);
	free(want);
}

int main(void)
{
	/* a paragraph of several lines, longer than the buffer's first 64 bytes */
	every_refusal_is_an_error_or_the_whole_text(
	        "first line of a paragraph that goes on for a while\n"
	        "second line of the same paragraph, also fairly long\n"
	        "third line\n");
	/* a setext heading joins its lines with a space */
	every_refusal_is_an_error_or_the_whole_text(
	        "heading line one\nheading line two\n=====\n");
	every_refusal_is_an_error_or_the_whole_text(
	        "&#x202e; &#8205; &amp; &#x202e; &#8205; &amp; "
	        "&#x202e; &#8205; &amp; &#x202e; &#8205; &amp;\n");
	return TEST_RESULT();
}

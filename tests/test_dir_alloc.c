/* Count actual owned allocations, including sanitizer buffer capacity. */
#include <stdlib.h>
#include <string.h>
#include "fs/wf_dir.h"
#include "base/wf_buf.h"
#include "text/wf_text.h"
#include "wf_test.h"

static void *ptrs[512];
static size_t sizes[512], count, bytes;
static long until_failure;
static bool refuse(void)
{
	return until_failure-- == 0;
}
static void remember(void *p, size_t n)
{
	if (!p) {
		return;
	}
	CHECK(count < 512);
	if (count < 512) {
		ptrs[count] = p;
		sizes[count++] = n;
		bytes += n;
	}
}
static void *allocate(size_t n)
{
	if (refuse()) {
		return NULL;
	}
	void *p = malloc(n);
	remember(p, n);
	return p;
}
static void *allocate_zero(size_t n, size_t k)
{
	if (refuse()) {
		return NULL;
	}
	void *p = calloc(n, k);
	remember(p, n * k);
	return p;
}
static void forget(void *p)
{
	for (size_t i = 0; i < count; i++) {
		if (ptrs[i] == p) {
			bytes -= sizes[i];
			--count;
			ptrs[i] = ptrs[count];
			sizes[i] = sizes[count];
			return;
		}
	}
}
static void release(void *p)
{
	forget(p);
	free(p);
}
static void *resize(void *p, size_t n)
{
	if (refuse()) {
		return NULL;
	}
	size_t i;
	for (i = 0; i < count && ptrs[i] != p; i++) {
	}
	void *q = realloc(p, n);
	if (q) {
		if (i < count) {
			bytes -= sizes[i];
			bytes += n;
			ptrs[i] = q;
			sizes[i] = n;
		} else {
			remember(q, n);
		}
	}
	return q;
}
static WfStatus sanitize(const wchar_t *s, size_t n, WfBuf *out)
{
	if (refuse()) {
		return WF_E_NOMEM;
	}
	WfStatus st = wf_sanitize_utf16(s, n, out);
	if (st == WF_OK && out->data) {
		remember(out->data, out->cap);
	}
	return st;
}
#define malloc allocate
#define calloc allocate_zero
#define realloc resize
#define free release
#define wf_sanitize_utf16 sanitize
#include "../src/fs/wf_dir.c"
#undef wf_sanitize_utf16
#undef free
#undef realloc
#undef calloc
#undef malloc

int main(void)
{
	for (long fail = 0; fail < 210; fail++) {
		until_failure = fail;
		WfDir *d = wf_dir_new();
		if (d) {
			for (unsigned i = 0; i < 70; i++) {
				WfStatus st = wf_dir_add(
				        d, L"name\x1b\x65e5", 6, 0, 0, 0);
				CHECK(d->owned_bytes == bytes);
				if (st != WF_OK) {
					CHECK(st == WF_E_NOMEM);
					break;
				}
			}
			wf_dir_free(d);
		}
		CHECK(count == 0 && bytes == 0);
	}
	/* An unsorted list still reports OOM without changing its order; an
	 * already ordered list needs no sort allocation at all. */
	until_failure = -1;
	WfDir *d = wf_dir_new();
	CHECK(d != NULL);
	if (d) {
		CHECK(wf_dir_add(d, L"b", 1, 0, 0, 0) == WF_OK);
		CHECK(wf_dir_add(d, L"a", 1, 0, 0, 0) == WF_OK);
		until_failure = 0;
		CHECK(wf_dir_sort(d, WF_SORT_NAME, false) == WF_E_NOMEM);
		CHECK(!wcscmp(d->e[0].name, L"b"));
		until_failure = -1;
		CHECK(wf_dir_sort(d, WF_SORT_NAME, false) == WF_OK);
		until_failure = 0;
		CHECK(wf_dir_sort(d, WF_SORT_NAME, false) == WF_OK);
		CHECK(until_failure == 0 && !wcscmp(d->e[0].name, L"a"));
		wf_dir_free(d);
	}
	CHECK(count == 0 && bytes == 0);
	return TEST_RESULT();
}

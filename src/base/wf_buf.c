#include "wf_buf.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void wf_buf_init(WfBuf *b)
{
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

void wf_buf_free(WfBuf *b)
{
	if (!b) {
		return;
	}
	free(b->data);
	wf_buf_init(b);
}

void wf_buf_clear(WfBuf *b)
{
	if (!b) {
		return;
	}
	b->len = 0;
	if (b->data) {
		b->data[0] = '\0';
	}
}

static WfStatus reserve(WfBuf *b, size_t extra)
{
	size_t need, cap;
	char *p;

	need = b->len + extra + 1; /* room for the NUL */
	if (need <= b->cap) {
		return WF_OK;
	}

	cap = b->cap ? b->cap : 64;
	while (cap < need) {
		cap = cap > SIZE_MAX / 2 ? need : cap * 2;
	}

	p = realloc(b->data, cap);
	if (!p) {
		return WF_E_NOMEM;
	}
	b->data = p;
	b->cap = cap;
	return WF_OK;
}

WfStatus wf_buf_append(WfBuf *b, const void *p, size_t n)
{
	WfStatus st;

	if (!b) {
		return WF_E_INVAL;
	}
	/* Keep range errors independent of p, without allocating for invalid input. */
	if (n > SIZE_MAX - b->len - 1) {
		return WF_E_RANGE;
	}
	if (n && !p) {
		return WF_E_INVAL;
	}
	st = reserve(b, n);
	if (WF_OK != st) {
		return st;
	}
	if (n) {
		memcpy(b->data + b->len, p, n);
	}
	b->len += n;
	b->data[b->len] = '\0';
	return WF_OK;
}

WfStatus wf_buf_puts(WfBuf *b, const char *s)
{
	if (!s) {
		return WF_E_INVAL;
	}
	return wf_buf_append(b, s, strlen(s));
}

WfStatus wf_buf_putc(WfBuf *b, char c)
{
	return wf_buf_append(b, &c, 1);
}

WfStatus wf_buf_put_uint(WfBuf *b, unsigned v)
{
	char tmp[12]; /* UINT_MAX has at most 10 digits on supported targets */
	size_t i = sizeof tmp;

	if (!b) {
		return WF_E_INVAL;
	}
	do {
		tmp[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	return wf_buf_append(b, tmp + i, sizeof tmp - i);
}

const char *wf_buf_cstr(const WfBuf *b)
{
	return b && b->data ? b->data : "";
}

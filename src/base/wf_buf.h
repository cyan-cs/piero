#ifndef WF_BUF_H
#define WF_BUF_H

#include <stddef.h>

#include "wf_status.h"

/*
 * Growable byte buffer. The buffer owns `data`.
 * When allocated, len < cap. Successful appends set data[len] to '\0'.
 * A failed append leaves the buffer unchanged. Append sources must not overlap
 * the buffer's storage (growth may relocate it).
 */
typedef struct
{
	char *data;
	size_t len;
	size_t cap;
} WfBuf;

void wf_buf_init(WfBuf *b);
void wf_buf_free(WfBuf *b);
void wf_buf_clear(WfBuf *b); /* keeps capacity */

WfStatus wf_buf_append(WfBuf *b, const void *p, size_t n);
WfStatus wf_buf_puts(WfBuf *b, const char *s);
WfStatus wf_buf_putc(WfBuf *b, char c);
WfStatus wf_buf_put_uint(WfBuf *b, unsigned v);

/* Never NULL: returns "" for an empty buffer. */
const char *wf_buf_cstr(const WfBuf *b);

#endif

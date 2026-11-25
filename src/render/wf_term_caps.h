#ifndef WF_TERM_CAPS_H
#define WF_TERM_CAPS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "../base/wf_buf.h"

/* Only synthetic console response records reach this parser. A query has a
 * fixed deadline; late replies are swallowed but never enable graphics. */
typedef struct
{
	uint64_t deadline;
	uint32_t version;
	unsigned cell_w, cell_h, colors;
	unsigned pixel_w, pixel_h, cols, rows;
	bool da, sixel, rectangular;
	bool sync_output; /* DECRQM answered that mode 2026 can be set and reset */
	char reply[128];
	size_t len;
	bool dropping;
} WfTermCaps;

/* Synchronized output (DEC private mode 2026): a frame written between these
 * is shown whole. Only ever used when the terminal answered the query. */
#define WF_SYNC_BEGIN "\x1b[?2026h"
#define WF_SYNC_END "\x1b[?2026l"

/* Frames that go out as one synchronized update. begin appends WF_SYNC_BEGIN
 * and returns the length to give end; once the frame has been appended, end
 * closes it, or takes the opening back out when the frame added nothing, so
 * an idle frame stays empty. */
bool wf_sync_begin(WfBuf *out, size_t *mark);
bool wf_sync_end(WfBuf *out, size_t mark);

void wf_term_caps_begin(WfTermCaps *c, uint64_t now);
void wf_term_caps_feed(WfTermCaps *c, unsigned ch, uint64_t now);
bool wf_term_caps_ready(const WfTermCaps *c);
const char *wf_term_caps_query(void);
#endif

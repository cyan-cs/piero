#ifndef WF_GRAPHICS_H
#define WF_GRAPHICS_H
#include "wf_frame.h"
#include "../base/wf_buf.h"
/* Erase old pixel content, repair affected cells, then paint the new image.
 * Previous graphic bytes are never read (their owner may already be gone). */
WfStatus wf_graphics_present(WfFrame *prev, const WfFrame *next, WfBuf *out);
#endif

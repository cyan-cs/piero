#include "render/wf_graphics.h"
#include "render/wf_theme.h"
#include "wf_test.h"
#include <string.h>

int main(void)
{
	const WfStyle blank = {0xffffff, 0, 0};
	WfFrame *p = wf_frame_new(80, 24, &blank);
	WfFrame *n = wf_frame_new(80, 24, &blank);
	WfBuf out = {0};
	static const char payload[] =
	        "\x1bP0;1;0q\"1;1;10;6#0;2;100;0;0#0!10~\x1b\\";
	CHECK(p && n);
	n->graphic.data = payload;
	n->graphic.len = sizeof payload - 1;
	n->graphic.x = 50;
	n->graphic.y = 3;
	n->graphic.w = 2;
	n->graphic.h = 1;
	n->graphic.gen = 10;
	CHECK(WF_OK == wf_graphics_present(p, n, &out));
	CHECK(strstr(out.data, payload) && strstr(out.data, "\x1b[4;51H"));
	p->graphic = n->graphic;
	p->graphic.data = NULL; /* previous bytes may already be freed */
	wf_buf_clear(&out);
	CHECK(WF_OK == wf_graphics_present(p, n, &out));
	CHECK(!out.len); /* idle: zero output, zero encoding */
	wf_frame_fill(n, 0, 23, 80, 1, '!', &blank);
	wf_buf_clear(&out);
	CHECK(WF_OK == wf_graphics_present(p, n, &out));
	CHECK(out.len && !strstr(out.data, payload) && !strstr(out.data, "$z"));
	wf_frame_fill(n, 0, 23, 80, 1, ' ', &blank);
	memset(&n->graphic, 0,
	        sizeof n->graphic); /* dialog, text, hide, leave */
	wf_buf_clear(&out);
	CHECK(WF_OK == wf_graphics_present(p, n, &out));
	CHECK(strstr(out.data, "\x1b[4;51;4;52$z") != NULL);
	CHECK(!strstr(out.data, "\x1b[2J"));
	CHECK(!strstr(out.data, "\x1bP"));
	n->graphic = p->graphic;
	n->graphic.data = payload;
	n->graphic.gen++;
	wf_buf_clear(&out);
	CHECK(WF_OK == wf_graphics_present(p, n, &out));
	CHECK(strstr(out.data, "$z") && strstr(out.data, payload));
	n->graphic.x = 80;
	CHECK(WF_E_INVAL == wf_graphics_present(p, n, &out));
	wf_frame_free(n);
	wf_frame_free(p);
	wf_buf_free(&out);
	return TEST_RESULT();
}

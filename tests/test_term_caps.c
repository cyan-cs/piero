#include "render/wf_term_caps.h"
#include "base/wf_buf.h"
#include "wf_test.h"
#include <string.h>

static void feed(WfTermCaps *c, const char *s, uint64_t now)
{
	while (*s) {
		wf_term_caps_feed(c, (unsigned char)*s++, now);
	}
}
int main(void)
{
	WfTermCaps c = {0};
	wf_term_caps_begin(&c, 100);
	CHECK(!wf_term_caps_ready(&c)); /* no reply */
	feed(&c, "\x1b[?61;4;28c", 110);
	CHECK(!wf_term_caps_ready(&c)); /* DA1 alone is insufficient */
	feed(&c, "\x1b[6;20;10t", 120);
	CHECK(wf_term_caps_ready(&c) && c.cell_w == 10 && c.cell_h == 20);
	wf_term_caps_begin(&c, 200);
	feed(&c, "\x1b[?61;4;28c\x1b[4;800;1200t\x1b[8;40;120t", 210);
	CHECK(wf_term_caps_ready(&c) && c.cell_w == 10 && c.cell_h == 20);
	wf_term_caps_begin(&c, 300);
	feed(&c, "\x1b[?1;2c\x1b[6;20;10t", 310);
	CHECK(c.da && !wf_term_caps_ready(&c));
	wf_term_caps_begin(&c, 400);
	feed(&c, "\x1b[?61;4;28c\x1b[6;20;10t", 1151);
	CHECK(!wf_term_caps_ready(&c)); /* late */
	wf_term_caps_begin(&c, 500);
	feed(&c, "\x1b[?61;4x\x1b[?61;;4;28c\x1b[6;999999999;10t", 510);
	CHECK(!c.da && !c.cell_w);
	feed(&c, "\x1b[", 520);
	for (unsigned i = 0; i < 512; i++) {
		wf_term_caps_feed(&c, '1', 520);
	}
	wf_term_caps_feed(&c, 't', 520);
	CHECK(!c.cell_w && c.len <= sizeof c.reply && !c.dropping);
	feed(&c, "\x1b[?61;4;28c\x1b[6;20;10t", 530);
	CHECK(wf_term_caps_ready(&c)); /* recovers after bad reply */
	wf_term_caps_begin(&c, 600);
	feed(&c, "\x1b[?61;4;28c\x1b[6;0;10t", 610);
	CHECK(!wf_term_caps_ready(&c));
	feed(&c, "\x1b[6;20;10", 620);
	CHECK(!wf_term_caps_ready(&c));
	feed(&c, "t", 630);
	CHECK(wf_term_caps_ready(&c)); /* split asynchronous reply */
	feed(&c, "\x1b[?1;0;16S", 640);
	CHECK(c.colors == 16);

	/* synchronized output: only a terminal that answers, in time, enables it */
	CHECK(strstr(wf_term_caps_query(), "\x1b[?2026$p"));
	wf_term_caps_begin(&c, 700);
	CHECK(!c.sync_output); /* no answer: never enabled */
	feed(&c, "\x1b[?2026;2$y", 710);
	CHECK(c.sync_output); /* known, currently reset */
	wf_term_caps_begin(&c, 720);
	CHECK(!c.sync_output); /* a new query starts from "no" */
	feed(&c, "\x1b[?2026;1$y", 730);
	CHECK(c.sync_output); /* known, currently set */
	wf_term_caps_begin(&c, 740);
	feed(&c, "\x1b[?2026;0$y", 750);
	CHECK(!c.sync_output); /* not recognised */
	wf_term_caps_begin(&c, 760);
	feed(&c, "\x1b[?2026;3$y\x1b[?2026;4$y", 770);
	CHECK(!c.sync_output); /* permanently fixed: nothing to switch */
	wf_term_caps_begin(&c, 780);
	feed(&c, "\x1b[?2026;9$y\x1b[?2027;2$y\x1b[?2026;2$x\x1b[?2026;22$y",
	        790);
	CHECK(!c.sync_output); /* other modes, bad values, wrong final byte */
	wf_term_caps_begin(&c, 800);
	feed(&c, "\x1b[?2026;2$y", 1551);
	CHECK(!c.sync_output); /* late */
	wf_term_caps_begin(&c, 900);
	feed(&c, "\x1b[?2026;2", 910);
	CHECK(!c.sync_output);
	feed(&c, "$y", 920);
	CHECK(c.sync_output); /* split reply */
	wf_term_caps_begin(&c, 1000);
	feed(&c, "\x1b[?61;4;28c\x1b[6;20;10t\x1b[?2026;2$y", 1010);
	CHECK(wf_term_caps_ready(&c) && c.sync_output); /* beside graphics */

	/* the wrapper: a drawn frame is opened and closed, an empty one is not sent */
	{
		WfBuf b;
		size_t mark = 0;

		wf_buf_init(&b);
		CHECK(wf_sync_begin(&b, &mark));
		CHECK(WF_OK == wf_buf_puts(&b, "frame"));
		CHECK(wf_sync_end(&b, mark));
		CHECK(!strcmp(
		        wf_buf_cstr(&b), WF_SYNC_BEGIN "frame" WF_SYNC_END));

		wf_buf_clear(&b);
		CHECK(wf_sync_begin(&b, &mark));
		CHECK(wf_sync_end(&b, mark)); /* nothing drawn */
		CHECK(0 == b.len && !strcmp(wf_buf_cstr(&b), ""));

		wf_buf_clear(&b);
		CHECK(wf_sync_begin(&b, &mark));
		CHECK(WF_OK == wf_buf_puts(&b, "x"));
		CHECK(!wf_sync_end(
		        &b, mark + 100)); /* a mark that was never given */
		CHECK(!wf_sync_end(&b, 0));
		CHECK(!wf_sync_begin(NULL, &mark) && !wf_sync_begin(&b, NULL));
		wf_buf_free(&b);
	}
	return TEST_RESULT();
}

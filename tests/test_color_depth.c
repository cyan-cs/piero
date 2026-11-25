/* Colour fallback: the nearest-colour tables, what the environment decides,
 * and what the presenter writes at each depth. */
#include <string.h>

#include "render/wf_frame.h"
#include "render/wf_present.h"
#include "render/wf_theme.h"
#include "wf_test.h"

static bool has(const WfBuf *b, const char *needle)
{
	return NULL != strstr(wf_buf_cstr(b), needle);
}

static void the_nearest_colour_is_chosen(void)
{
	/* the cube corners and the grey ramp */
	CHECK(16 == wf_color_to_256(0x000000));
	CHECK(231 == wf_color_to_256(0xFFFFFF));
	CHECK(196 == wf_color_to_256(0xFF0000));
	CHECK(46 == wf_color_to_256(0x00FF00));
	CHECK(21 == wf_color_to_256(0x0000FF));
	CHECK(wf_color_to_256(0x808080) >= 232 ||
	        244 == wf_color_to_256(0x808080));
	/* a grey the ramp holds exactly beats the cube */
	CHECK(232 == wf_color_to_256(0x080808));
	CHECK(255 == wf_color_to_256(0xEEEEEE));
	/* every result is a valid index */
	for (uint32_t v = 0; v <= 0xFFFFFF; v += 0x010F0F + 0x3) {
		CHECK(wf_color_to_256(v) >= 16 && wf_color_to_256(v) <= 255);
	}

	CHECK(0 == wf_color_to_16(0x0C0C0C));
	CHECK(0 == wf_color_to_16(0x000000));
	CHECK(15 == wf_color_to_16(0xFFFFFF));
	CHECK(7 == wf_color_to_16(0xCCCCCC));
	CHECK(9 == wf_color_to_16(0xE74856));
	CHECK(10 == wf_color_to_16(0x16C60C));
	for (uint32_t v = 0; v <= 0xFFFFFF; v += 0x01FF11) {
		CHECK(wf_color_to_16(v) < 16);
	}
}

static void the_environment_decides_in_auto(void)
{
	/* configuration beats the environment, whatever it says */
	CHECK(WF_DEPTH_TRUECOLOR ==
	        wf_color_depth_resolve(1, "1", NULL, NULL, "dumb"));
	CHECK(WF_DEPTH_256 ==
	        wf_color_depth_resolve(2, NULL, "truecolor", NULL, NULL));
	CHECK(WF_DEPTH_16 == wf_color_depth_resolve(3, NULL, NULL, "x", NULL));
	CHECK(WF_DEPTH_NONE ==
	        wf_color_depth_resolve(4, NULL, "truecolor", "x", NULL));
	/* auto */
	CHECK(WF_DEPTH_TRUECOLOR ==
	        wf_color_depth_resolve(0, NULL, NULL, NULL, NULL));
	CHECK(WF_DEPTH_NONE ==
	        wf_color_depth_resolve(0, "1", "truecolor", "x",
	                NULL)); /* NO_COLOR wins over a good terminal */
	CHECK(WF_DEPTH_TRUECOLOR ==
	        wf_color_depth_resolve(0, "", NULL, NULL,
	                NULL)); /* an empty NO_COLOR is not set */
	CHECK(WF_DEPTH_NONE ==
	        wf_color_depth_resolve(0, NULL, NULL, NULL, "dumb"));
	CHECK(WF_DEPTH_TRUECOLOR ==
	        wf_color_depth_resolve(0, NULL, "24bit", NULL, "xterm"));
	CHECK(wf_color_depth_resolve(0, NULL, NULL, "{guid}",
	              "xterm-256color") == WF_DEPTH_TRUECOLOR);
	CHECK(WF_DEPTH_256 ==
	        wf_color_depth_resolve(0, NULL, NULL, NULL, "xterm-256color"));
	CHECK(WF_DEPTH_TRUECOLOR ==
	        wf_color_depth_resolve(0, NULL, NULL, NULL, "xterm"));
	CHECK(WF_DEPTH_TRUECOLOR ==
	        wf_color_depth_resolve(7, NULL, NULL, NULL,
	                NULL)); /* an unknown policy is auto-like, not none */
}

static WfBuf paint(WfColorDepth d, uint32_t highlight, uint16_t attr)
{
	WfStyle st = {0x336699, 0x112233, attr};
	WfFrame *f = wf_frame_new(10, 3, &st);
	WfBuf out;

	CHECK(NULL != f);
	wf_buf_init(&out);
	{
		WfThemePalette p = wf_theme_preset(WF_THEME_SPADE)->palette;

		p.selection_background = highlight;
		wf_theme_activate(&p);
	}
	wf_present_set_color(d);
	CHECK(WF_OK == wf_present_diff(NULL, f, &out));
	wf_present_set_color(WF_DEPTH_TRUECOLOR);
	wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
	wf_frame_free(f);
	return out;
}

static void each_depth_writes_its_own_codes(void)
{
	WfBuf b;

	b = paint(WF_DEPTH_TRUECOLOR, 0, 0);
	CHECK(has(&b, ";38;2;51;102;153") && has(&b, ";48;2;17;34;51"));
	wf_buf_free(&b);

	b = paint(WF_DEPTH_256, 0, 0);
	CHECK(has(&b, ";38;5;") && has(&b, ";48;5;") && !has(&b, ";38;2;"));
	wf_buf_free(&b);

	b = paint(WF_DEPTH_16, 0, 0);
	CHECK(!has(&b, ";38;2;") && !has(&b, ";38;5;"));
	CHECK((has(&b, ";3") || has(&b, ";9")) &&
	        (has(&b, ";4") || has(&b, ";10")));
	wf_buf_free(&b);

	/* no colours at all: nothing but attributes */
	b = paint(WF_DEPTH_NONE, 0, WF_ATTR_BOLD);
	CHECK(!has(&b, ";38") && !has(&b, ";48") && has(&b, "\x1b[0;1m"));
	wf_buf_free(&b);

	/* the selected row (its background is the highlight) is reversed */
	b = paint(WF_DEPTH_NONE, 0x112233, 0);
	CHECK(has(&b, "\x1b[0;7m"));
	wf_buf_free(&b);
	b = paint(WF_DEPTH_NONE, 0x999999, 0);
	CHECK(!has(&b, ";7"));
	wf_buf_free(&b);
	/* the highlight only matters without colours */
	b = paint(WF_DEPTH_TRUECOLOR, 0x112233, 0);
	CHECK(!has(&b, ";7"));
	wf_buf_free(&b);
	CHECK(WF_DEPTH_TRUECOLOR == wf_present_color_depth());
}

int main(void)
{
	the_nearest_colour_is_chosen();
	the_environment_decides_in_auto();
	each_depth_writes_its_own_codes();
	return TEST_RESULT();
}

/*
 * The eight themes, their exact colours, IDs and aliases, the colour helpers,
 * the Custom theme, and the contrast of every pair that carries text. The
 * expected colours are written out here again, from the owner's table, rather
 * than read back from the code under test.
 */
#include <stdio.h>
#include <string.h>

#include "render/wf_theme.h"
#include "wf_test.h"
#include "base/wf_macro.h"

typedef struct
{
	const char *id, *name;
	uint32_t background, surface, foreground, accent;
	uint32_t muted, selection_background, selection_foreground, border;
} Expected;

static const Expected table[WF_THEME_PRESET_COUNT] = {
        {"spade", "Spade", 0x0F172A, 0x1E293B, 0xE2E8F0, 0x60A5FA, 0x94A3B8,
                0x1E40AF, 0xF8FAFC, 0x334155},
        {"heart", "Heart", 0x21141E, 0x352433, 0xFCE7F3, 0xF9A8D4, 0xD6ACC7,
                0x831843, 0xFDF2F8, 0x64425B},
        {"diamond", "Diamond", 0xF8FAFC, 0xE2E8F0, 0x1E293B, 0x1D4ED8, 0x475569,
                0xBFDBFE, 0x172554, 0xCBD5E1},
        {"club", "Club", 0x0E1B16, 0x192B22, 0xE1F0E6, 0x6EE7B7, 0x9FB7A8,
                0x065F46, 0xF0FDF4, 0x345548},
        {"joker", "Joker", 0x100E18, 0x201827, 0xF3E8FF, 0xD946EF, 0xB4A4C2,
                0x701A75, 0xFFFFFF, 0x4A3157},
        {"king", "King", 0x241610, 0x35231C, 0xFFEDD5, 0xFBBF24, 0xD1B2A0,
                0x92400E, 0xFFF7ED, 0x6E4635},
        {"queen", "Queen", 0x1A1426, 0x2A203B, 0xF3E8FF, 0xC4B5FD, 0xC0B3CD,
                0x5B21B6, 0xF5F3FF, 0x514265},
        {"ace", "Ace", 0x18181B, 0x27272A, 0xF4F4F5, 0xA1A1AA, 0xA1A1AA,
                0x3F3F46, 0xF4F4F5, 0x52525B}};

static void presets_have_the_owners_colours(void)
{
	for (int i = 0; i < WF_THEME_PRESET_COUNT; i++) {
		const WfThemeDefinition *d = wf_theme_preset((WfThemeId)i);
		const Expected *e = &table[i];

		CHECK(NULL != d);
		if (!d) {
			continue;
		}
		CHECK(!strcmp(d->id, e->id) &&
		        !strcmp(d->display_name, e->name));
		CHECK(d->palette.background == e->background &&
		        d->palette.surface == e->surface &&
		        d->palette.foreground == e->foreground &&
		        d->palette.accent == e->accent);
		CHECK(d->palette.muted_foreground == e->muted &&
		        d->palette.selection_background ==
		                e->selection_background &&
		        d->palette.selection_foreground ==
		                e->selection_foreground &&
		        d->palette.border == e->border);
		/* stable lower-case IDs, display names start with a capital */
		CHECK(wf_theme_id_text((WfThemeId)i) == d->id);
		CHECK(d->display_name[0] >= 'A' && d->display_name[0] <= 'Z');
		CHECK(strlen(d->id) == strlen(d->display_name));
		for (const char *c = d->id; *c; c++) {
			CHECK(*c >= 'a' && *c <= 'z');
		}
		/* no suit symbols or emoji: ASCII letters only */
		for (const char *c = d->display_name; *c; c++) {
			CHECK((*c >= 'A' && *c <= 'Z') ||
			        (*c >= 'a' && *c <= 'z'));
		}
	}
	CHECK(NULL == wf_theme_preset(WF_THEME_CUSTOM));
	CHECK(NULL == wf_theme_preset((WfThemeId)99));
	CHECK(WF_THEME_DEFAULT == WF_THEME_SPADE);
	CHECK(!strcmp(wf_theme_display_name(WF_THEME_CUSTOM), "Custom"));
}

static void ids_and_old_names_are_read_exactly(void)
{
	static const struct
	{
		const char *text;
		WfThemeId id;
	} ok[] = {{"spade", WF_THEME_SPADE}, {"ace", WF_THEME_ACE},
	        {"custom", WF_THEME_CUSTOM}, {"midnight", WF_THEME_SPADE},
	        {"sakura", WF_THEME_HEART}, {"snow", WF_THEME_DIAMOND},
	        {"forest", WF_THEME_CLUB}, {"sunset", WF_THEME_KING},
	        {"amethyst", WF_THEME_QUEEN}, {"graphite", WF_THEME_ACE}};
	static const char *const bad[] = {"", "Spade", "SPADE", " spade",
	        "spade ", "spades", "paper", "joker2", "\xE2\x99\xA0"};
	WfThemeId id = WF_THEME_HEART;

	for (size_t i = 0; i < WF_ARRAY_LEN(ok); i++) {
		id = WF_THEME_JOKER;
		CHECK(wf_theme_id_parse(ok[i].text, &id) && id == ok[i].id);
	}
	/* "joker" is new: it has no old name */
	CHECK(wf_theme_id_parse("joker", &id) && WF_THEME_JOKER == id);
	id = WF_THEME_HEART;
	for (size_t i = 0; i < WF_ARRAY_LEN(bad); i++) {
		CHECK(!wf_theme_id_parse(bad[i], &id) && WF_THEME_HEART == id);
	}
	CHECK(!wf_theme_id_parse(NULL, &id));
}

static void hex_colours_are_strict(void)
{
	uint32_t v = 7;
	char text[8];

	CHECK(wf_color_parse_hex("#0F172A", &v) && 0x0F172A == v);
	CHECK(wf_color_parse_hex("#abcdef", &v) && 0xABCDEF == v);
	CHECK(wf_color_parse_hex("#000000", &v) && 0 == v);
	CHECK(wf_color_parse_hex("#FFFFFF", &v) && 0xFFFFFF == v);
	v = 7;
	static const char *const bad[] = {"", "#", "#FFF", "#12345", "#1234567",
	        "0F172A", "#0F172G", "# 0F172", "#0F172A ", "#+F172A",
	        "#0x1234",
	        ("\xEF\xBC\x83"
	         "0F172A"),
	        "#0F17\n2A"};
	for (size_t i = 0; i < WF_ARRAY_LEN(bad); i++) {
		CHECK(!wf_color_parse_hex(bad[i], &v) && 7 == v);
	}
	CHECK(!wf_color_parse_hex(NULL, &v));
	wf_color_format_hex(0x0F172A, text);
	CHECK(!strcmp(text, "#0F172A"));
	wf_color_format_hex(0, text);
	CHECK(!strcmp(text, "#000000"));
	wf_color_format_hex(0xFFFFFF, text);
	CHECK(!strcmp(text, "#FFFFFF"));
}

static void contrast_and_mixing_follow_the_definitions(void)
{
	double c = wf_color_contrast(0x000000, 0xFFFFFF);

	CHECK(c > 20.99 && c < 21.01); /* black on white */
	CHECK(wf_color_contrast(0x777777, 0x777777) < 1.001);
	CHECK(wf_color_contrast(0xFFFFFF, 0x000000) == c); /* symmetric */
	CHECK(0x000000 == wf_color_mix(0x000000, 0xFFFFFF, 0));
	CHECK(0xFFFFFF == wf_color_mix(0x000000, 0xFFFFFF, 100));
	CHECK(0x808080 == wf_color_mix(0x000000, 0xFFFFFF, 50));
	CHECK(0x102030 == wf_color_mix(0x102030, 0x102030, 37));
}

static void custom_is_a_preset_with_some_colours_replaced(void)
{
	WfCustomTheme c;
	WfThemePalette p, base;

	wf_custom_init(&c);
	wf_theme_build_custom(&c, &p);
	CHECK(!memcmp(&p, &wf_theme_preset(WF_THEME_SPADE)->palette, sizeof p));

	c.base = WF_THEME_KING;
	c.color[WF_CUSTOM_BACKGROUND] = 0x112233;
	c.color[WF_CUSTOM_ACCENT] = 0xAABBCC;
	wf_theme_build_custom(&c, &p);
	base = wf_theme_preset(WF_THEME_KING)->palette;
	CHECK(0x112233 == p.background && 0xAABBCC == p.accent);
	CHECK(p.foreground == base.foreground && p.surface == base.surface &&
	        p.border == base.border);
	/* what follows from them follows */
	CHECK(0xAABBCC == p.directory_foreground);
	CHECK(p.file_foreground == p.foreground);
	CHECK(p.search_background == p.selection_background);
	CHECK(p.status_background == p.surface);
	/* out-of-range values are ignored, not truncated */
	c.color[WF_CUSTOM_FOREGROUND] = 0x1000000;
	wf_theme_build_custom(&c, &p);
	CHECK(p.foreground == base.foreground);
	c.base = (WfThemeId)50; /* an invalid base falls back to the default */
	wf_theme_build_custom(&c, &p);
	CHECK(p.border == wf_theme_preset(WF_THEME_SPADE)->palette.border);

	for (int i = 0; i < WF_CUSTOM_COUNT; i++) {
		CHECK(wf_custom_slot_key((WfCustomSlot)i) &&
		        wf_custom_slot_label((WfCustomSlot)i));
	}
	CHECK(!strcmp(wf_custom_slot_key(WF_CUSTOM_SELECTION_BACKGROUND),
	        "selection_background"));
	wf_theme_palette_of(WF_THEME_HEART, &c, &p);
	CHECK(0x21141E == p.background);
	wf_theme_palette_of(WF_THEME_CUSTOM, &c, &p);
	CHECK(0x112233 == p.background);
}

/* Measured, not assumed: every pair that carries text, in every theme. */
static void every_theme_reads_well(void)
{
	double worst = 99.0;

	for (int i = 0; i < WF_THEME_PRESET_COUNT; i++) {
		const WfThemePalette *p =
		        &wf_theme_preset((WfThemeId)i)->palette;
		WfContrastIssue issues[16];
		size_t n = wf_theme_check_contrast(p, issues, 16);

		printf("  %-8s text %5.2f  muted %5.2f  accent %5.2f  "
		       "selected %5.2f  ok %5.2f  warn %5.2f  err %5.2f\n",
		        table[i].name,
		        wf_color_contrast(p->foreground, p->background),
		        wf_color_contrast(p->muted_foreground, p->background),
		        wf_color_contrast(p->accent, p->background),
		        wf_color_contrast(p->selection_foreground,
		                p->selection_background),
		        wf_color_contrast(p->success_foreground, p->background),
		        wf_color_contrast(p->warning_foreground, p->background),
		        wf_color_contrast(p->error_foreground, p->background));
		for (size_t k = 0; k < n; k++) {
			printf("    ISSUE %s: %.2f < %.1f\n", issues[k].what,
			        issues[k].ratio, issues[k].minimum);
		}
		CHECK(0 == n);
		double sel = wf_color_contrast(
		        p->selection_foreground, p->selection_background);
		if (sel < worst) {
			worst = sel;
		}
	}
	printf("  lowest selected-row contrast: %.2f\n", worst);
}

static void a_bad_custom_palette_is_reported(void)
{
	WfThemePalette p = wf_theme_preset(WF_THEME_SPADE)->palette;
	WfContrastIssue issues[16];

	p.foreground = p.background; /* invisible text */
	CHECK(wf_theme_check_contrast(&p, issues, 16) >= 2);
	CHECK(issues[0].ratio < issues[0].minimum);
	CHECK(0 ==
	        wf_theme_check_contrast(&p, issues, 0)); /* no room: no write */

	/* between "invisible" and "fine": under 4.5:1 is reported, over is not */
	p = wf_theme_preset(WF_THEME_SPADE)->palette;
	p.foreground = 0x6B7280; /* grey on the navy: about 3.5:1 */
	CHECK(wf_color_contrast(p.foreground, p.background) < 4.5 &&
	        wf_color_contrast(p.foreground, p.background) > 3.0);
	{
		size_t n = wf_theme_check_contrast(&p, issues, 16);
		bool text_flagged = false;

		for (size_t i = 0; i < n; i++) {
			if (!strcmp(issues[i].what, "text on background")) {
				text_flagged = true;
			}
		}
		CHECK(text_flagged);
	}
	p.foreground = 0xA0A8B4; /* well above 4.5:1 */
	{
		size_t n = wf_theme_check_contrast(&p, issues, 16);

		for (size_t i = 0; i < n; i++) {
			CHECK(0 !=
			        strcmp(issues[i].what, "text on background"));
		}
	}
}

static void styles_come_from_the_active_palette(void)
{
	const WfThemePalette *joker = &wf_theme_preset(WF_THEME_JOKER)->palette;
	uint32_t before = wf_theme_generation();
	const WfThemeStyles *s;

	/* nothing active yet means the default, never black on black */
	s = wf_theme_styles();
	CHECK(0xE2E8F0 == s->base.fg_rgb && 0x0F172A == s->base.bg_rgb);
	before = wf_theme_generation();
	wf_theme_activate(joker);
	CHECK(wf_theme_generation() == before + 1);
	s = wf_theme_styles();
	CHECK(0xF3E8FF == s->base.fg_rgb && 0x100E18 == s->base.bg_rgb);
	CHECK(0xFFFFFF == s->selection.fg_rgb &&
	        0x701A75 == s->selection.bg_rgb);
	CHECK(0xD946EF == s->directory.fg_rgb &&
	        (s->directory.attributes & WF_ATTR_BOLD));
	CHECK(s->bar.bg_rgb == joker->surface);
	CHECK(s->syntax[5] == joker->accent &&
	        s->syntax[0] == joker->foreground);
	CHECK(0xD946EF == wf_theme_active()->accent);
	wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
}

int main(void)
{
	presets_have_the_owners_colours();
	ids_and_old_names_are_read_exactly();
	hex_colours_are_strict();
	contrast_and_mixing_follow_the_definitions();
	custom_is_a_preset_with_some_colours_replaced();
	every_theme_reads_well();
	a_bad_custom_palette_is_reported();
	styles_come_from_the_active_palette();
	return TEST_RESULT();
}

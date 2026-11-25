#include "wf_theme.h"
#include "../base/wf_macro.h"

#include <math.h>
#include <string.h>

/*
 * Base and accent colours are the owner's table. The tokens the table does
 * not give (success/warning/error) are chosen per theme so that each reads on
 * its background; the rest are derived from the table in one place
 * (derive()). The contrast of every pair is measured by tests/test_theme.c.
 */
#define PRESET(id_text, display, bg, surface, fg, accent, muted, sel_bg,       \
        sel_fg, border, ok, warn, err)                                         \
	{                                                                      \
		id_text, display,                                              \
		{                                                              \
			bg, surface, fg, muted, accent, sel_bg, sel_fg,        \
			        border, accent, fg, ok, warn, err, sel_bg,     \
			        sel_fg, surface, fg                            \
		}                                                              \
	}

static const WfThemeDefinition presets[] = {PRESET("spade",
                                                    "Spade",
                                                    0x0F172A,
                                                    0x1E293B,
                                                    0xE2E8F0,
                                                    0x60A5FA,
                                                    0x94A3B8,
                                                    0x1E40AF,
                                                    0xF8FAFC,
                                                    0x334155,
                                                    0x86EFAC,
                                                    0xFCD34D,
                                                    0xF87171),
        PRESET("heart",
                "Heart",
                0x21141E,
                0x352433,
                0xFCE7F3,
                0xF9A8D4,
                0xD6ACC7,
                0x831843,
                0xFDF2F8,
                0x64425B,
                0x86EFAC,
                0xFCD34D,
                0xFCA5A5),
        PRESET("diamond",
                "Diamond",
                0xF8FAFC,
                0xE2E8F0,
                0x1E293B,
                0x1D4ED8,
                0x475569,
                0xBFDBFE,
                0x172554,
                0xCBD5E1,
                0x15803D,
                0xB45309,
                0xB91C1C),
        PRESET("club",
                "Club",
                0x0E1B16,
                0x192B22,
                0xE1F0E6,
                0x6EE7B7,
                0x9FB7A8,
                0x065F46,
                0xF0FDF4,
                0x345548,
                0x86EFAC,
                0xFCD34D,
                0xF87171),
        PRESET("joker",
                "Joker",
                0x100E18,
                0x201827,
                0xF3E8FF,
                0xD946EF,
                0xB4A4C2,
                0x701A75,
                0xFFFFFF,
                0x4A3157,
                0x86EFAC,
                0xFCD34D,
                0xFCA5A5),
        PRESET("king",
                "King",
                0x241610,
                0x35231C,
                0xFFEDD5,
                0xFBBF24,
                0xD1B2A0,
                0x92400E,
                0xFFF7ED,
                0x6E4635,
                0x86EFAC,
                0xFDBA74,
                0xFCA5A5),
        PRESET("queen",
                "Queen",
                0x1A1426,
                0x2A203B,
                0xF3E8FF,
                0xC4B5FD,
                0xC0B3CD,
                0x5B21B6,
                0xF5F3FF,
                0x514265,
                0x86EFAC,
                0xFCD34D,
                0xFCA5A5),
        PRESET("ace",
                "Ace",
                0x18181B,
                0x27272A,
                0xF4F4F5,
                0xA1A1AA,
                0xA1A1AA,
                0x3F3F46,
                0xF4F4F5,
                0x52525B,
                0x86EFAC,
                0xFCD34D,
                0xF87171)};
#undef PRESET
_Static_assert(WF_ARRAY_LEN(presets) == WF_THEME_PRESET_COUNT,
        "WF_THEME_PRESET_COUNT and the preset table disagree");

/* Retired names of an earlier set and the preset each one became. */
static const struct
{
	const char *old_id;
	WfThemeId id;
} aliases[] = {{"midnight", WF_THEME_SPADE}, {"sakura", WF_THEME_HEART},
        {"snow", WF_THEME_DIAMOND}, {"forest", WF_THEME_CLUB},
        {"sunset", WF_THEME_KING}, {"amethyst", WF_THEME_QUEEN},
        {"graphite", WF_THEME_ACE}};

const WfThemeDefinition *wf_theme_preset(WfThemeId id)
{
	return (unsigned)id < WF_THEME_PRESET_COUNT ? &presets[id] : NULL;
}

const char *wf_theme_id_text(WfThemeId id)
{
	if (WF_THEME_CUSTOM == id) {
		return "custom";
	}
	return (unsigned)id < WF_THEME_PRESET_COUNT ? presets[id].id : NULL;
}

const char *wf_theme_display_name(WfThemeId id)
{
	if (WF_THEME_CUSTOM == id) {
		return "Custom";
	}
	return (unsigned)id < WF_THEME_PRESET_COUNT ? presets[id].display_name
	                                            : NULL;
}

bool wf_theme_id_parse(const char *text, WfThemeId *out)
{
	size_t i;

	if (!text || !out) {
		return false;
	}
	if (!strcmp(text, "custom")) {
		*out = WF_THEME_CUSTOM;
		return true;
	}
	for (i = 0; i < WF_THEME_PRESET_COUNT; i++) {
		if (!strcmp(text, presets[i].id)) {
			*out = (WfThemeId)i;
			return true;
		}
	}
	for (i = 0; i < WF_ARRAY_LEN(aliases); i++) {
		if (!strcmp(text, aliases[i].old_id)) {
			*out = aliases[i].id;
			return true;
		}
	}
	return false;
}

bool wf_color_parse_hex(const char *text, uint32_t *rgb)
{
	uint32_t v = 0;
	size_t i;

	if (!text || !rgb || '#' != text[0] || 7 != strlen(text)) {
		return false;
	}
	for (i = 1; i < 7; i++) {
		char c = text[i];
		uint32_t d;

		if (c >= '0' && c <= '9') {
			d = (uint32_t)(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			d = (uint32_t)(c - 'a') + 10u;
		} else if (c >= 'A' && c <= 'F') {
			d = (uint32_t)(c - 'A') + 10u;
		} else {
			return false;
		}
		v = v << 4 | d;
	}
	*rgb = v;
	return true;
}

void wf_color_format_hex(uint32_t rgb, char out[8])
{
	static const char digits[] = "0123456789ABCDEF";
	int shift;
	size_t i = 1;

	out[0] = '#';
	for (shift = 20; shift >= 0; shift -= 4) {
		out[i++] = digits[(rgb >> shift) & 0xFu];
	}
	out[7] = '\0';
}

static double linear(uint32_t channel)
{
	double c = (double)channel / 255.0;

	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double luminance(uint32_t rgb)
{
	return 0.2126 * linear((rgb >> 16) & 0xFF) +
	        0.7152 * linear((rgb >> 8) & 0xFF) +
	        0.0722 * linear(rgb & 0xFF);
}

double wf_color_contrast(uint32_t a, uint32_t b)
{
	double la = luminance(a), lb = luminance(b);

	if (la < lb) {
		double t = la;

		la = lb;
		lb = t;
	}
	return (la + 0.05) / (lb + 0.05);
}

uint32_t wf_color_mix(uint32_t a, uint32_t b, unsigned percent)
{
	uint32_t out = 0;
	int shift;

	if (percent > 100) {
		percent = 100;
	}
	for (shift = 16; shift >= 0; shift -= 8) {
		uint32_t ca = (a >> shift) & 0xFF, cb = (b >> shift) & 0xFF;
		uint32_t c =
		        (ca * (100u - percent) + cb * percent + 50u) / 100u;

		out |= c << shift;
	}
	return out;
}

static const struct
{
	const char *key, *label;
} slots[] = {{"background", "Background"}, {"surface", "Surface"},
        {"foreground", "Foreground"}, {"accent", "Accent"}, {"muted", "Muted"},
        {"selection_background", "Selection background"},
        {"selection_foreground", "Selection foreground"}, {"border", "Border"},
        {"error", "Error"}, {"warning", "Warning"}, {"success", "Success"}};
_Static_assert(WF_ARRAY_LEN(slots) == WF_CUSTOM_COUNT,
        "every custom colour slot needs a key and a label");

const char *wf_custom_slot_key(WfCustomSlot slot)
{
	return (unsigned)slot < WF_CUSTOM_COUNT ? slots[slot].key : NULL;
}

const char *wf_custom_slot_label(WfCustomSlot slot)
{
	return (unsigned)slot < WF_CUSTOM_COUNT ? slots[slot].label : NULL;
}

void wf_custom_init(WfCustomTheme *c)
{
	size_t i;

	c->base = WF_THEME_DEFAULT;
	for (i = 0; i < WF_CUSTOM_COUNT; i++) {
		c->color[i] = -1;
	}
}

/* Tokens derived from the base colour overrides. */
static void derive(WfThemePalette *p)
{
	p->directory_foreground = p->accent;
	p->file_foreground = p->foreground;
	p->search_background = p->selection_background;
	p->search_foreground = p->selection_foreground;
	p->status_background = p->surface;
	p->status_foreground = p->foreground;
}

void wf_theme_build_custom(const WfCustomTheme *c, WfThemePalette *out)
{
	const WfThemeDefinition *base = wf_theme_preset(c->base);
	uint32_t *field[WF_CUSTOM_COUNT];
	size_t i;

	if (!base) {
		base = wf_theme_preset(WF_THEME_DEFAULT);
	}
	*out = base->palette;
	field[WF_CUSTOM_BACKGROUND] = &out->background;
	field[WF_CUSTOM_SURFACE] = &out->surface;
	field[WF_CUSTOM_FOREGROUND] = &out->foreground;
	field[WF_CUSTOM_ACCENT] = &out->accent;
	field[WF_CUSTOM_MUTED] = &out->muted_foreground;
	field[WF_CUSTOM_SELECTION_BACKGROUND] = &out->selection_background;
	field[WF_CUSTOM_SELECTION_FOREGROUND] = &out->selection_foreground;
	field[WF_CUSTOM_BORDER] = &out->border;
	field[WF_CUSTOM_ERROR] = &out->error_foreground;
	field[WF_CUSTOM_WARNING] = &out->warning_foreground;
	field[WF_CUSTOM_SUCCESS] = &out->success_foreground;
	for (i = 0; i < WF_CUSTOM_ERROR; i++) {
		if (c->color[i] >= 0 && c->color[i] <= 0xFFFFFF) {
			*field[i] = (uint32_t)c->color[i];
		}
	}
	derive(out);
	/* the three that nothing is derived from are applied last */
	for (i = WF_CUSTOM_ERROR; i < WF_CUSTOM_COUNT; i++) {
		if (c->color[i] >= 0 && c->color[i] <= 0xFFFFFF) {
			*field[i] = (uint32_t)c->color[i];
		}
	}
}

bool wf_custom_id_valid(const char *id)
{
	size_t i;

	if (!id || !id[0]) {
		return false;
	}
	for (i = 0; id[i]; i++) {
		char ch = id[i];

		if (i + 1 >= WF_THEME_ID_MAX) {
			return false;
		}
		if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
		            '_' == ch || '-' == ch)) {
			return false;
		}
	}
	return true;
}

void wf_theme_palette_of(
        WfThemeId id, const WfCustomTheme *custom, WfThemePalette *out)
{
	const WfThemeDefinition *def = wf_theme_preset(id);

	if (def) {
		*out = def->palette;
		return;
	}
	if (WF_THEME_CUSTOM == id && custom) {
		wf_theme_build_custom(custom, out);
		return;
	}
	*out = presets[WF_THEME_DEFAULT].palette;
}

size_t wf_theme_check_contrast(
        const WfThemePalette *p, WfContrastIssue *issues, size_t cap)
{
	const struct
	{
		const char *what;
		uint32_t fg, bg;
		double minimum;
	} pairs[] = {
	        {"text on background", p->foreground, p->background, 4.5},
	        {"text on surface", p->foreground, p->surface, 4.5},
	        {"secondary text on background", p->muted_foreground,
	                p->background, 3.0},
	        {"accent on background", p->accent, p->background, 3.0},
	        {"selected row", p->selection_foreground,
	                p->selection_background, 4.5},
	        {"status bar", p->status_foreground, p->status_background, 4.5},
	        {"success on background", p->success_foreground, p->background,
	                3.0},
	        {"warning on background", p->warning_foreground, p->background,
	                3.0},
	        {"error on background", p->error_foreground, p->background,
	                3.0},
	};
	size_t i, n = 0;

	for (i = 0; i < WF_ARRAY_LEN(pairs); i++) {
		double ratio = wf_color_contrast(pairs[i].fg, pairs[i].bg);

		if (ratio >= pairs[i].minimum) {
			continue;
		}
		if (n < cap) {
			issues[n].what = pairs[i].what;
			issues[n].ratio = ratio;
			issues[n].minimum = pairs[i].minimum;
		}
		n++;
	}
	return n < cap ? n : cap;
}

static WfThemePalette active_palette;
static WfThemeStyles active_styles;
static uint32_t generation;

static WfStyle style(uint32_t fg, uint32_t bg, uint16_t attributes)
{
	WfStyle s;

	s.fg_rgb = fg;
	s.bg_rgb = bg;
	s.attributes = attributes;
	return s;
}

static uint32_t readable(uint32_t c, uint32_t bg, uint32_t fg);
static void ensure_active(void);

void wf_theme_activate(const WfThemePalette *p)
{
	WfThemeStyles *s = &active_styles;
	uint32_t type_fg, key_fg, markup_fg;

	active_palette = *p;
	s->base = style(p->foreground, p->background, 0);
	s->dim = style(p->muted_foreground, p->background, 0);
	s->accent = style(p->accent, p->background, WF_ATTR_BOLD);
	s->directory =
	        style(p->directory_foreground, p->background, WF_ATTR_BOLD);
	s->selection =
	        style(p->selection_foreground, p->selection_background, 0);
	s->bar = style(p->status_foreground, p->status_background, 0);
	s->bar_bold =
	        style(p->status_foreground, p->status_background, WF_ATTR_BOLD);
	s->error = style(p->background, p->error_foreground, WF_ATTR_BOLD);
	s->note = style(p->background, p->success_foreground, 0);
	s->warning = style(p->warning_foreground, p->background, 0);
	s->prompt = style(p->search_foreground, p->search_background, 0);
	s->prompt_label =
	        style(p->search_foreground, p->search_background, WF_ATTR_BOLD);
	s->panel = style(p->foreground, p->surface, 0);
	s->border = style(p->border, p->background, 0);
	s->gutter = style(p->muted_foreground, p->background, 0);
	s->gutter_current = style(p->foreground, p->background, WF_ATTR_BOLD);
	s->text_selection = s->selection;
	s->cursor_line = style(p->foreground, p->surface, 0);
	s->match = style(p->background, p->warning_foreground, 0);
	s->whitespace = style(p->border, p->background, 0);

	type_fg = wf_color_mix(p->accent, p->foreground, 50);
	key_fg = wf_color_mix(p->accent, p->success_foreground, 50);
	markup_fg = wf_color_mix(p->accent, p->error_foreground, 50);
	s->syntax[0] = p->foreground;         /* plain */
	s->syntax[1] = p->muted_foreground;   /* comment */
	s->syntax[2] = p->success_foreground; /* string */
	s->syntax[3] = p->warning_foreground; /* escape */
	s->syntax[4] = p->warning_foreground; /* number */
	s->syntax[5] = p->accent;             /* keyword */
	s->syntax[6] = type_fg;               /* type */
	s->syntax[7] = p->error_foreground;   /* preprocessor */
	s->syntax[8] = p->accent;             /* heading */
	s->syntax[9] = key_fg;                /* key */
	s->syntax[10] = markup_fg;            /* markup */
	/* the classes of the grammars (DR-DRAFT-033): derived from the palette,
	 * each pushed from the background until it reads at 4.5:1 */
	s->syntax[11] =
	        readable(wf_color_mix(p->accent, p->warning_foreground, 50),
	                p->background, p->foreground); /* boolean */
	s->syntax[12] = readable(wf_color_mix(p->foreground, p->accent, 35),
	        p->background, p->foreground); /* null */
	s->syntax[13] = readable(p->directory_foreground, p->background,
	        p->foreground); /* function */
	s->syntax[14] =
	        readable(wf_color_mix(p->foreground, p->success_foreground, 30),
	                p->background, p->foreground); /* variable */
	s->syntax[15] = readable(
	        wf_color_mix(p->warning_foreground, p->error_foreground, 40),
	        p->background, p->foreground); /* constant */
	s->syntax[16] = readable(wf_color_mix(p->foreground, p->accent, 45),
	        p->background, p->foreground); /* operator */
	s->syntax[17] =
	        readable(wf_color_mix(p->foreground, p->muted_foreground, 45),
	                p->background, p->foreground); /* punctuation */
	s->syntax[18] =
	        readable(wf_color_mix(p->accent, p->success_foreground, 30),
	                p->background, p->foreground); /* link */
	s->syntax[19] = readable(
	        p->error_foreground, p->background, p->foreground); /* error */
	generation++;
}

/* A colour that reads on `bg`: itself when it already has contrast 4.5, else
 * moved toward `fg` (which the palette tests hold to 4.5) in tenths. */
static uint32_t readable(uint32_t c, uint32_t bg, uint32_t fg)
{
	for (unsigned step = 0; step <= 10; step++) {
		uint32_t m = wf_color_mix(c, fg, step * 10u);

		if (wf_color_contrast(m, bg) >= 4.5) {
			return m;
		}
	}
	return fg;
}

WfStyle wf_theme_syntax_style(uint8_t cls)
{
	const WfThemeStyles *t;
	WfStyle st;

	ensure_active();
	t = &active_styles;
	st = t->base;
	if (cls >= WF_SYNTAX_CLASSES) {
		return st;
	}
	st.fg_rgb = t->syntax[cls];
	if (1 == cls) {
		st.attributes |= WF_ATTR_DIM; /* comment */
	}
	if (5 == cls || 8 == cls) {
		st.attributes |= WF_ATTR_BOLD; /* keyword, heading */
	}
	if (18 == cls || 19 == cls) {
		st.attributes |= WF_ATTR_UNDERLINE; /* link, error */
	}
	return st;
}

/* Until something chooses a theme, the default is in force: nothing is ever
 * drawn from an all-zero (black on black) style table. */
static void ensure_active(void)
{
	if (!generation) {
		wf_theme_activate(&presets[WF_THEME_DEFAULT].palette);
	}
}

const WfThemePalette *wf_theme_active(void)
{
	ensure_active();
	return &active_palette;
}

const WfThemeStyles *wf_theme_styles(void)
{
	ensure_active();
	return &active_styles;
}

uint32_t wf_theme_generation(void)
{
	return generation;
}

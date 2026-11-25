#ifndef WF_THEME_H
#define WF_THEME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wf_frame.h"

/*
 * Colours by meaning. Nothing that draws writes an RGB value of its own: it
 * asks for a style, and the style comes from the active palette. The eight
 * presets are constant data; the Custom theme overrides the WF_CUSTOM_COUNT
 * editable colours of a preset. Theme IDs are stable text ("spade"); display names may
 * change without touching anyone's configuration.
 *
 * The active theme is process-wide and is only read and changed from the UI
 * thread (the render loop); workers never see it.
 */

#define WF_THEME_PRESET_COUNT 8

typedef enum
{
	WF_THEME_SPADE,
	WF_THEME_HEART,
	WF_THEME_DIAMOND,
	WF_THEME_CLUB,
	WF_THEME_JOKER,
	WF_THEME_KING,
	WF_THEME_QUEEN,
	WF_THEME_ACE,
	WF_THEME_CUSTOM
} WfThemeId;

#define WF_THEME_DEFAULT WF_THEME_SPADE

typedef struct
{
	uint32_t background, surface, foreground, muted_foreground;
	uint32_t accent;
	uint32_t selection_background, selection_foreground, border;
	uint32_t directory_foreground, file_foreground;
	uint32_t success_foreground, warning_foreground, error_foreground;
	uint32_t search_background, search_foreground;
	uint32_t status_background, status_foreground;
} WfThemePalette;

typedef struct
{
	const char *id;           /* stable, lower case: "spade" */
	const char *display_name; /* "Spade" */
	WfThemePalette palette;
} WfThemeDefinition;

/* The eight presets, in selection order. NULL for Custom or out of range. */
const WfThemeDefinition *wf_theme_preset(WfThemeId id);

/* Stable ID of a preset or of "custom"; NULL when out of range. */
const char *wf_theme_id_text(WfThemeId id);
const char *wf_theme_display_name(WfThemeId id);
/* Exact lower-case IDs, plus the retired names of an earlier set (midnight,
 * sakura, snow, forest, sunset, amethyst, graphite). Anything else is false
 * and *out is untouched. */
bool wf_theme_id_parse(const char *text, WfThemeId *out);

/* "#RRGGBB" and nothing else (hex digits of either case). */
bool wf_color_parse_hex(const char *text, uint32_t *rgb);
void wf_color_format_hex(uint32_t rgb, char out[8]);
/* WCAG 2.x contrast ratio, 1.0 .. 21.0. */
double wf_color_contrast(uint32_t a, uint32_t b);
/* a*(100-percent)% + b*percent%. */
uint32_t wf_color_mix(uint32_t a, uint32_t b, unsigned percent);

typedef enum
{
	WF_CUSTOM_BACKGROUND,
	WF_CUSTOM_SURFACE,
	WF_CUSTOM_FOREGROUND,
	WF_CUSTOM_ACCENT,
	WF_CUSTOM_MUTED,
	WF_CUSTOM_SELECTION_BACKGROUND,
	WF_CUSTOM_SELECTION_FOREGROUND,
	WF_CUSTOM_BORDER,
	WF_CUSTOM_ERROR,
	WF_CUSTOM_WARNING,
	WF_CUSTOM_SUCCESS,
	WF_CUSTOM_COUNT
} WfCustomSlot;

const char *wf_custom_slot_key(WfCustomSlot slot);   /* "background" */
const char *wf_custom_slot_label(WfCustomSlot slot); /* "Background" */

/* Custom colours: a negative value means "keep the base theme's". */
typedef struct
{
	WfThemeId base; /* a preset */
	int64_t color[WF_CUSTOM_COUNT];
} WfCustomTheme;

void wf_custom_init(WfCustomTheme *c);

/* A theme the user made: a name that is shown, a stable ID that is written
 * in the configuration ([a-z0-9_-], up to 23), a preset it started from and
 * the colours it changes. Presets are never edited; a custom theme is a copy
 * of one with its own changes. */
#define WF_THEME_ID_MAX 24
#define WF_THEME_NAME_MAX 64 /* bytes: about 21 Japanese characters */
#define WF_CUSTOM_THEMES 8
typedef struct
{
	char id[WF_THEME_ID_MAX];
	char name[WF_THEME_NAME_MAX];
	WfCustomTheme theme;
} WfCustomEntry;

/* A valid custom theme ID: 1..23 of [a-z0-9_-]. */
bool wf_custom_id_valid(const char *id);
/* The palette of Custom: the base preset with the overrides applied and
 * every dependent token re-derived. */
void wf_theme_build_custom(const WfCustomTheme *c, WfThemePalette *out);
/* Palette of any theme (Custom needs `custom`; presets ignore it). */
void wf_theme_palette_of(
        WfThemeId id, const WfCustomTheme *custom, WfThemePalette *out);

typedef struct
{
	const char *what; /* "text on background" */
	double ratio, minimum;
} WfContrastIssue;

/* Pairs that fall under their minimum (4.5 for text, 3 for secondary text
 * and accents). Returns how many were stored, at most cap. */
size_t wf_theme_check_contrast(
        const WfThemePalette *p, WfContrastIssue *issues, size_t cap);

/* One colour for each class of text (the numbers of WfSynClass in
 * syntax/wf_syntax_abi.h: plain comment string escape number keyword type
 * preproc heading key markup boolean null function variable constant
 * operator punctuation link error). */
#define WF_SYNTAX_CLASSES 20

typedef struct
{
	WfStyle base, dim, accent, directory, selection, bar, bar_bold;
	WfStyle error, note, warning, prompt, prompt_label, panel, border;
	WfStyle gutter, gutter_current, text_selection, cursor_line, match;
	WfStyle whitespace;
	uint32_t syntax[WF_SYNTAX_CLASSES]; /* foreground per syntax class */
} WfThemeStyles;

/* The style of text of syntax class `cls` on the base background: the class
 * colour, with the few attributes a class carries (comments dim, keywords and
 * headings bold, links and errors underlined). An unknown class is plain. */
WfStyle wf_theme_syntax_style(uint8_t cls);

/* Makes `p` the active palette and recomputes the styles. */
void wf_theme_activate(const WfThemePalette *p);
const WfThemePalette *wf_theme_active(void);
const WfThemeStyles *wf_theme_styles(void);
/* Incremented by every activation, so a cache can tell it is stale. */
uint32_t wf_theme_generation(void);

#endif

#ifndef WF_CONFIG_H
#define WF_CONFIG_H

#include <stddef.h>

#include "base/wf_status.h"
#include "input/wf_keymap.h"
#include "render/wf_theme.h"
#include "wf_ini_edit.h"

#define WF_CONFIG_MAX_BYTES (256u * 1024u)
#define WF_CONFIG_MAX_LINE 4096u
#define WF_CONFIG_SCHEMA_VERSION 1
#define WF_PLACES 26u             /* the letters a to z */
#define WF_PLACE_BYTES 1024u      /* a place's folder as UTF-8, with the NUL */
#define WF_CONFIG_VALUE_MAX 1100u /* the longest value of a change */

typedef enum
{
	WF_COLOR_TRUECOLOR,
	WF_COLOR_256
} WfColorMode;
/* [appearance] color_mode: how many colours the terminal is given. */
typedef enum
{
	WF_COLOR_AUTO,
	WF_COLOR_POLICY_TRUECOLOR,
	WF_COLOR_POLICY_256,
	WF_COLOR_POLICY_16,
	WF_COLOR_POLICY_NONE
} WfColorPolicy;
typedef enum
{
	WF_IMAGE_DEFAULT,
	WF_IMAGE_HALFBLOCK,
	WF_IMAGE_ASCII,
	WF_IMAGE_OFF
} WfImageStyle;
typedef enum
{
	WF_CONFIG_SORT_NAME,
	WF_CONFIG_SORT_SIZE,
	WF_CONFIG_SORT_DATE,
	WF_CONFIG_SORT_EXT
} WfConfigSortBy;
typedef enum
{
	WF_VERIFY_NONE,
	WF_VERIFY_SHA256,
	WF_VERIFY_HMAC_SHA256
} WfCopyVerify;
typedef enum
{
	WF_LAUNCH_NEUTRAL,
	WF_LAUNCH_FILE_DIR
} WfLaunchDirectory;
typedef enum
{
	WF_LOG_ERROR,
	WF_LOG_WARNING,
	WF_LOG_INFO,
	WF_LOG_DEBUG
} WfLogLevel;
typedef enum
{
	WF_FALLBACK_CP932 = 932,
	WF_FALLBACK_UTF8 = 65001
} WfTextFallback;

/* Owned UTF-8 start_directory, NULL means "cwd"; "home" is preserved.
 * All other members are values. Initialize once; dispose before reuse. No
 * setting grants a production gate or allows overwrite/permanent deletion. */
typedef struct
{
	int language;     /* [ui] language: 0 = English, kept for old files */
	int lang_setting; /* [general] language: WfLangSetting */
	int parent_ratio, current_ratio, preview_ratio;
	int color_mode, show_hidden, ambiguous_width;
	int preview_enabled, images, image_style;
	int graphics,
	        image_quality; /* 0 auto/high, 1 sixel/balanced, 2 text/fast */
	double image_cell_aspect;
	int image_cache_mib, text_max_kib;
	int text_fallback_encoding; /* Windows codepage; boundary must check
	                               availability */
	int remote_auto_preview, cloud_auto_preview;
	int directories_first, sort_descending;
	int git_folder_color; /* [git] folder_color: tint folders that hold changes */
	WfConfigSortBy sort_by;
	WfCopyVerify copy_verify;
	WfLaunchDirectory launch_working_directory;
	char *start_directory;
	int confirm_unc_navigation, log_paths;
	int session_persist; /* [session] persist: save the session (off) */
	WfLogLevel log_level;
	/* [editor]. keymap: 0 = piero, 1 = nano (vim is reserved, not accepted).
	 * indent: 0 = tabs, 1 = spaces. */
	int editor_enabled, editor_keymap, editor_tab_width, editor_indent;
	int editor_auto_indent, editor_line_numbers, editor_syntax;
	int editor_soft_wrap, editor_show_whitespace, editor_backup;
	int editor_case_sensitive, editor_max_mib, editor_undo_mib;
	int editor_recovery_draft; /* keep unsaved text as a draft; on by default */
	int editor_markdown_split; /* 0 auto, 1 on, 2 off (DR-DRAFT-025) */
	/* [meta] and [appearance]. theme is a WfThemeId (a preset or custom);
	 * color_policy is a WfColorPolicy. Theme overrides are stored below in
	 * customs[].theme.color[], in WfCustomSlot order; -1 keeps the base colour. */
	int schema_version, onboarding_completed;
	int update_checks, update_consent;
	int updates_blocked; /* transient: malformed/future configuration, never saved */
	int theme, color_policy, dir_preview;
	/* [keyboard] (profile and the Custom overrides) and the themes the user
	 * made. "theme = custom" means the one named by custom_theme (the first
	 * if that names none). */
	WfKeyConfig keys;
	WfCustomEntry customs[WF_CUSTOM_THEMES];
	int custom_count;
	char custom_theme[WF_THEME_ID_MAX];
	/* [places] (DR-DRAFT-030): a folder kept under a letter, `a` to `z`, as UTF-8;
	 * "" is no place. Not a setting of the settings screen; written by `m`. */
	char places[WF_PLACES][WF_PLACE_BYTES];
} WfConfig;

typedef enum
{
	WF_CONFIG_UNKNOWN_KEY,
	WF_CONFIG_DUPLICATE_KEY,
	WF_CONFIG_INVALID_VALUE,
	WF_CONFIG_SYNTAX,
	WF_CONFIG_INVALID_ENCODING,
	WF_CONFIG_TOO_LARGE,
	WF_CONFIG_LINE_TOO_LONG,
	WF_CONFIG_RATIO_SUM
} WfConfigWarning;

/* Static ASCII names only: callbacks never receive untrusted INI text.
 * Line is one-based; zero denotes a whole-file error. */
typedef void (*WfConfigDiagnostic)(void *context,
        WfConfigWarning warning,
        size_t line,
        const char *section,
        const char *key);
typedef struct
{
	size_t warnings, lines;
	size_t text_fallback_encoding_line; /* for OS codepage-availability
	                                       warning */
} WfConfigReport;

void wf_config_init(WfConfig *config);
void wf_config_dispose(WfConfig *config);
/* Replaces an initialized output. Syntax/value warnings return WF_OK, except
 * an invalid explicit schema_version returns INVAL: it must not become an
 * unversioned writable file. Positive future versions are retained for read-
 * only use; writers must refuse versions above WF_CONFIG_SCHEMA_VERSION.
 * Encoding/size/line errors return INVAL/RANGE with the entire output reset
 * to defaults; OOM also leaves defaults. Input is borrowed, never changed.
 * UTF-8, optionally BOM-prefixed; UTF-16 and embedded NUL are rejected.
 * This core performs no I/O and never creates a configuration file. */
WfStatus wf_config_parse(WfConfig *config,
        const void *bytes,
        size_t size,
        WfConfigDiagnostic diagnostic,
        void *context,
        WfConfigReport *report);
const char *wf_config_warning_name(WfConfigWarning warning);

/* Edit configuration text with the same section aliases as the parser.
 * Duplicate keys across equivalent sections become one assignment, or are
 * all removed, so saving and restarting cannot restore an older override.
 * Encoding/size/line errors and invalid explicit schema versions are refused;
 * warning-level syntax is preserved. A future schema returns WF_E_ACCESS
 * without producing edited text, regardless of the requested changes. */
WfStatus wf_config_apply(const char *text,
        size_t n,
        const WfIniSet *sets,
        size_t count,
        char **out,
        size_t *out_n);

/* The palette the configuration asks for: a preset, or the Custom theme built
 * from [theme.custom]. */
void wf_config_palette(const WfConfig *config, WfThemePalette *out);

/* One setting that differs between two configurations, as it would be written
 * in the file. Only settings with a plain text form are compared (not
 * start_directory, the image aspect, the fallback code page or saved places).
 * Unsetting a custom colour emits a key removal. */
typedef struct
{
	const char *section, *key; /* a setting that is built (a custom theme, a
	                              key override) points into this record: the
	                              array is used where it was filled */
	char value[WF_CONFIG_VALUE_MAX];
	bool remove; /* take the key out of the file */
	char section_buf[40];
} WfConfigChange;

/* Whether any setting differs (the same test, nothing stored). */
bool wf_config_differs(const WfConfig *a, const WfConfig *b);

const WfCustomEntry *wf_config_custom_find(const WfConfig *c, const char *id);
WfCustomEntry *wf_config_custom_find_mut(WfConfig *c, const char *id);
/* The theme "custom" stands for, or NULL when there is none. */
const WfCustomEntry *wf_config_custom_selected(const WfConfig *c);
/* Adds a theme (a copy of `from`, or the default preset when NULL). False when
 * the ID is taken or invalid, or there is no room. */
bool wf_config_custom_add(WfConfig *c,
        const char *id,
        const char *name,
        const WfCustomTheme *from);
bool wf_config_custom_remove(WfConfig *c, const char *id);
/* An ID that is not taken, from a stem ("my-theme", "my-theme-2"...). */
void wf_config_custom_unique_id(
        const WfConfig *c, const char *stem, char out[WF_THEME_ID_MAX]);

/* Settings whose value in `to` differs from `from`: returns min(total, cap).
 * With non-NULL out, stores that many records; NULL counts without storing. */
size_t wf_config_diff(const WfConfig *from,
        const WfConfig *to,
        WfConfigChange *out,
        size_t cap);

#endif

#include "wf_config.h"

#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../i18n/wf_i18n.h"
#include "../render/wf_theme.h"
#include "../base/wf_macro.h"

typedef enum
{
	INTEGER,
	BOOLEAN,
	CHOICE,
	ASPECT,
	START,
	CODEPAGE,
	THEME,    /* a theme ID, or one of the retired names */
	PRESET,   /* the same, but only a preset (not "custom") */
	HEXCOLOR, /* "#RRGGBB" */
	SCHEMA /* meta schema_version: retain future markers without writing */
} ValueKind;
typedef struct
{
	const char *section, *key;
	ValueKind kind;
	size_t offset;
	int minimum, maximum;
	const char *choices;
	size_t member_size; /* int-valued kinds store sizeof(int) bytes via
	                       memcpy */
} Setting;
#define FIELD(section, key, member, kind, lo, hi, choices)                     \
	{section, key, kind, offsetof(WfConfig, member), lo, hi, choices,      \
	        sizeof(((WfConfig *)0)->member)}
static const Setting settings[] = {
        FIELD("updates", "enabled", update_checks, BOOLEAN, 0, 1, NULL),
        FIELD("updates",
                "consent_recorded",
                update_consent,
                BOOLEAN,
                0,
                1,
                NULL),
        FIELD("ui", "language", language, CHOICE, 0, 0, "en"),
        FIELD("general",
                "language",
                lang_setting,
                CHOICE,
                0,
                0,
                wf_langset_choices),
        FIELD("ui", "parent_ratio", parent_ratio, INTEGER, 0, 100, NULL),
        FIELD("ui", "current_ratio", current_ratio, INTEGER, 0, 100, NULL),
        FIELD("ui", "preview_ratio", preview_ratio, INTEGER, 0, 100, NULL),
        FIELD("ui", "color_mode", color_mode, CHOICE, 0, 0, "truecolor|256"),
        FIELD("ui", "show_hidden", show_hidden, BOOLEAN, 0, 1, NULL),
        FIELD("terminal",
                "ambiguous_width",
                ambiguous_width,
                INTEGER,
                1,
                2,
                NULL),
        FIELD("preview", "enabled", preview_enabled, BOOLEAN, 0, 1, NULL),
        FIELD("preview", "images", images, BOOLEAN, 0, 1, NULL),
        FIELD("preview", "graphics", graphics, CHOICE, 0, 0, "auto|sixel|text"),
        FIELD("preview",
                "image_quality",
                image_quality,
                CHOICE,
                0,
                0,
                "high|balanced|fast"),
        FIELD("preview",
                "image_style",
                image_style,
                CHOICE,
                0,
                0,
                "default|halfblock|ascii|off"),
        FIELD("preview",
                "image_cell_aspect",
                image_cell_aspect,
                ASPECT,
                0,
                0,
                NULL),
        FIELD("preview",
                "image_cache_mib",
                image_cache_mib,
                INTEGER,
                4,
                128,
                NULL),
        FIELD("preview", "text_max_kib", text_max_kib, INTEGER, 16, 1024, NULL),
        FIELD("preview",
                "text_fallback_encoding",
                text_fallback_encoding,
                CODEPAGE,
                0,
                0,
                NULL),
        FIELD("preview",
                "remote_auto_preview",
                remote_auto_preview,
                BOOLEAN,
                0,
                1,
                NULL),
        FIELD("preview",
                "cloud_auto_preview",
                cloud_auto_preview,
                BOOLEAN,
                0,
                1,
                NULL),
        FIELD("files",
                "directories_first",
                directories_first,
                BOOLEAN,
                0,
                1,
                NULL),
        FIELD("files", "sort_by", sort_by, CHOICE, 0, 0, "name|size|date|ext"),
        FIELD("files", "sort_descending", sort_descending, BOOLEAN, 0, 1, NULL),
        FIELD("git", "folder_color", git_folder_color, BOOLEAN, 0, 1, NULL),
        FIELD("copy",
                "verify",
                copy_verify,
                CHOICE,
                0,
                0,
                "none|sha256|hmac-sha256"),
        FIELD("launch",
                "working_directory",
                launch_working_directory,
                CHOICE,
                0,
                0,
                "neutral|file_dir"),
        FIELD("behavior",
                "start_directory",
                start_directory,
                START,
                0,
                0,
                NULL),
        FIELD("behavior",
                "confirm_unc_navigation",
                confirm_unc_navigation,
                BOOLEAN,
                0,
                1,
                NULL),
        FIELD("diagnostics",
                "log_level",
                log_level,
                CHOICE,
                0,
                0,
                "error|warning|info|debug"),
        FIELD("diagnostics", "log_paths", log_paths, BOOLEAN, 0, 1, NULL),
        FIELD("session", "persist", session_persist, BOOLEAN, 0, 1, NULL),
        FIELD("editor", "enabled", editor_enabled, BOOLEAN, 0, 1, NULL),
        FIELD("editor", "keymap", editor_keymap, CHOICE, 0, 0, "piero|nano"),
        FIELD("editor", "tab_width", editor_tab_width, INTEGER, 1, 16, NULL),
        FIELD("editor", "indent", editor_indent, CHOICE, 0, 0, "tabs|spaces"),
        FIELD("editor", "auto_indent", editor_auto_indent, BOOLEAN, 0, 1, NULL),
        FIELD("editor",
                "line_numbers",
                editor_line_numbers,
                BOOLEAN,
                0,
                1,
                NULL),
        FIELD("editor", "syntax_highlight", editor_syntax, BOOLEAN, 0, 1, NULL),
        FIELD("editor", "soft_wrap", editor_soft_wrap, BOOLEAN, 0, 1, NULL),
        FIELD("editor",
                "show_whitespace",
                editor_show_whitespace,
                BOOLEAN,
                0,
                1,
                NULL),
        FIELD("editor", "backup", editor_backup, BOOLEAN, 1, 1, NULL),
        FIELD("editor",
                "case_sensitive",
                editor_case_sensitive,
                BOOLEAN,
                0,
                1,
                NULL),
        FIELD("editor", "max_size_mib", editor_max_mib, INTEGER, 1, 256, NULL),
        FIELD("editor", "undo_mib", editor_undo_mib, INTEGER, 1, 256, NULL),
        FIELD("editor",
                "recovery_draft",
                editor_recovery_draft,
                BOOLEAN,
                0,
                1,
                NULL),
        FIELD("editor",
                "markdown_split",
                editor_markdown_split,
                CHOICE,
                0,
                0,
                "auto|on|off"),
        FIELD("meta",
                "schema_version",
                schema_version,
                SCHEMA,
                1,
                INT_MAX,
                NULL),
        FIELD("meta",
                "onboarding_completed",
                onboarding_completed,
                BOOLEAN,
                0,
                1,
                NULL),
        FIELD("appearance", "theme", theme, THEME, 0, 0, NULL),
        FIELD("appearance",
                "color_mode",
                color_policy,
                CHOICE,
                0,
                0,
                "auto|truecolor|256|16|none"),
        FIELD("preview", "directory_preview", dir_preview, BOOLEAN, 0, 1, NULL),
        FIELD("keyboard",
                "profile",
                keys.profile,
                CHOICE,
                0,
                0,
                "standard|nano|vim|custom"),
        FIELD("keyboard",
                "bindings_version",
                keys.version,
                INTEGER,
                1,
                2,
                NULL),
        FIELD("keyboard.custom", "base", keys.base, CHOICE, 0, 0, "nano|vim")};
#undef FIELD
#define SETTING_COUNT (WF_ARRAY_LEN(settings))
_Static_assert(sizeof(WfThemeId) == sizeof(int) &&
                sizeof(WfConfigSortBy) == sizeof(int) &&
                sizeof(WfCopyVerify) == sizeof(int) &&
                sizeof(WfLaunchDirectory) == sizeof(int) &&
                sizeof(WfLogLevel) == sizeof(int),
        "config enums are stored through int-sized writes");
void wf_config_init(WfConfig *c)
{
	/* the language table is built on first use, and wf_langset_choices (read by
	 * the table of settings) is filled by that: ask for it before any parse */
	(void)wf_i18n_lang_count();
	memset(c, 0, sizeof *c);
	c->parent_ratio = 25;
	c->current_ratio = 45;
	c->preview_ratio = 30;
	c->ambiguous_width = 1;
	c->preview_enabled = 1;
	c->images =
	        1; /* pictures and PDF are decoded in the isolated host only;
		    * see DR-DRAFT-039 for what was checked */
	c->image_cell_aspect = 1.0;
	c->image_cache_mib = 32;
	c->text_max_kib = 256;
	c->text_fallback_encoding = WF_FALLBACK_CP932;
	c->directories_first = 1;
	c->git_folder_color = 1;
	c->confirm_unc_navigation = 1;
	c->session_persist =
	        0; /* nothing about where you were is written until asked */
	/* SHA-256 read-back is the standard check for copies; `none` is an
	 * explicit opt-out (DR-DRAFT-007). */
	c->copy_verify = WF_VERIFY_SHA256;
	c->log_level = WF_LOG_WARNING;
	c->editor_enabled = 1;
	c->editor_tab_width = 8;
	c->editor_auto_indent = 1;
	c->editor_line_numbers = 1;
	c->editor_syntax = 1;
	c->editor_max_mib = 32;
	c->editor_undo_mib = 16;
	c->editor_backup = 1; /* the previous version must remain recoverable */
	c->editor_recovery_draft =
	        1; /* unsaved text is kept as a draft (see
		      wf_edraft_win.h for what is never kept) */
	c->theme = WF_THEME_DEFAULT;
	c->dir_preview = 1;
	wf_keyconfig_init(&c->keys);
}
void wf_config_dispose(WfConfig *c)
{
	if (!c) {
		return;
	}
	free(c->start_directory);
	wf_config_init(c);
}
const char *wf_config_warning_name(WfConfigWarning w)
{
	static const char *const names[] = {"unknown-key", "duplicate-key",
	        "invalid-value", "syntax", "invalid-encoding", "file-too-large",
	        "line-too-long", "ratio-sum"};
	return (unsigned)w < WF_ARRAY_LEN(names) ? names[w] : "unknown";
}
static void warn(WfConfigReport *r,
        WfConfigDiagnostic d,
        void *ctx,
        WfConfigWarning kind,
        size_t line,
        const Setting *setting)
{
	r->warnings++;
	if (d) {
		d(ctx, kind, line, setting ? setting->section : "",
		        setting ? setting->key : "");
	}
}
/* Reject an undecodable file before applying any security-sensitive value. */
static int utf8_valid(const unsigned char *s, size_t n)
{
	size_t i = 0;
	while (i < n) {
		uint32_t cp;
		size_t count;
		unsigned char first = s[i++];
		if (!first) {
			return 0;
		}
		if (first < 0x80) {
			continue;
		}
		if (first >= 0xC2 && first <= 0xDF) {
			cp = first & 31u;
			count = 1;
		} else if (first >= 0xE0 && first <= 0xEF) {
			cp = first & 15u;
			count = 2;
		} else if (first >= 0xF0 && first <= 0xF4) {
			cp = first & 7u;
			count = 3;
		} else {
			return 0;
		}
		if (count > n - i) {
			return 0;
		}
		for (size_t j = 0; j < count; j++) {
			unsigned char ch = s[i++];
			if (0x80u != (ch & 0xC0u)) {
				return 0;
			}
			cp = (cp << 6) | (ch & 63u);
		}
		if ((1 == count && cp < 0x80) || (2 == count && cp < 0x800) ||
		        (3 == count && cp < 0x10000) || cp > 0x10FFFF ||
		        (cp >= 0xD800 && cp <= 0xDFFF)) {
			return 0;
		}
	}
	return 1;
}
static char *trim(char *s)
{
	char *end;
	while (' ' == *s || '\t' == *s) {
		s++;
	}
	end = s + strlen(s);
	while (end > s && (' ' == end[-1] || '\t' == end[-1])) {
		end--;
	}
	*end = 0;
	return s;
}
static bool integer(const char *s, int minimum, int maximum, int *out)
{
	int value = 0;
	if (!*s) {
		return 0;
	}
	for (; *s; s++) {
		int digit = *s - '0';
		if (digit < 0 || digit > 9 || digit > maximum ||
		        value > (maximum - digit) / 10) {
			return 0;
		}
		value = value * 10 + digit;
	}
	if (value < minimum) {
		return 0;
	}
	*out = value;
	return 1;
}
static bool choice(const char *s, const char *choices, int *out)
{
	int index = 0;
	for (;;) {
		const char *end = strchr(choices, '|');
		size_t n = end ? (size_t)(end - choices) : strlen(choices);
		if (strlen(s) == n && !memcmp(s, choices, n)) {
			*out = index;
			return 1;
		}
		if (!end) {
			return 0;
		}
		choices = end + 1;
		index++;
	}
}
/* Locale-independent decimal, no NaN/infinity/overflow or ignored suffixes. */
static bool aspect(const char *s, double *out)
{
	double value = 0, scale = 0.1;
	int digits = 0;
	while (*s >= '0' && *s <= '9') {
		value = value * 10 + (*s++ - '0');
		digits++;
		if (value > 2) {
			return 0;
		}
	}
	if ('.' == *s) {
		s++;
		while (*s >= '0' && *s <= '9') {
			value += (*s++ - '0') * scale;
			scale *= 0.1;
			digits++;
		}
	}
	if (*s || !digits || value < 0.5 || value > 2.0) {
		return 0;
	}
	*out = value;
	return 1;
}
static bool start_valid(const char *s)
{
	int absolute;
	size_t n = strlen(s);
	if (!strcmp(s, "cwd") || !strcmp(s, "home")) {
		return 1;
	}
	absolute = n >= 3 &&
	        ((s[0] >= 'A' && s[0] <= 'Z') ||
	                (s[0] >= 'a' && s[0] <= 'z')) &&
	        ':' == s[1] && ('\\' == s[2] || '/' == s[2]);
	if (!absolute) {
		absolute = n >= 2 &&
		        (('\\' == s[0] && '\\' == s[1]) ||
		                ('/' == s[0] && '/' == s[1]));
	}
	if (!absolute) {
		return 0;
	}
	for (; *s; s++) {
		if ((unsigned char)*s < 32 || '"' == *s || '<' == *s ||
		        '>' == *s || '|' == *s) {
			return 0;
		}
	}
	return 1;
}
static WfStatus assign(WfConfig *c,
        const WfConfig *defaults,
        size_t id,
        const char *value,
        bool *valid)
{
	const Setting *s = &settings[id];
	char *target = (char *)c + s->offset;
	const char *initial = (const char *)defaults + s->offset;
	int number = 0;
	*valid = 0;
	if (START != s->kind && ASPECT != s->kind &&
	        s->member_size != sizeof number) {
		return WF_E_INVAL; /* table/struct mismatch: int-sized members
		                      only */
	}
	if (START == s->kind) {
		char *owned = NULL;
		if (start_valid(value)) {
			*valid = 1;
			if (strcmp(value, "cwd")) {
				owned = malloc(strlen(value) + 1);
				if (!owned) {
					return WF_E_NOMEM;
				}
				memcpy(owned, value, strlen(value) + 1);
			}
		}
		free(c->start_directory);
		c->start_directory = owned;
	} else if (ASPECT == s->kind) {
		double v;
		*valid = aspect(value, &v);
		c->image_cell_aspect = *valid ? v : defaults->image_cell_aspect;
	} else {
		if (INTEGER == s->kind) {
			*valid =
			        integer(value, s->minimum, s->maximum, &number);
		} else if (BOOLEAN == s->kind) {
			*valid = choice(value, "false|true", &number) &&
			        number >= s->minimum && number <= s->maximum;
		} else if (THEME == s->kind || PRESET == s->kind) {
			WfThemeId theme_id;

			*valid = wf_theme_id_parse(value, &theme_id) &&
			        (THEME == s->kind ||
			                WF_THEME_CUSTOM != theme_id);
			if (*valid) {
				number = (int)theme_id;
			}
		} else if (HEXCOLOR == s->kind) {
			uint32_t rgb;

			*valid = wf_color_parse_hex(value, &rgb);
			if (*valid) {
				number = (int)rgb;
			}
		} else if (SCHEMA == s->kind) {
			*valid =
			        integer(value, s->minimum, s->maximum, &number);
		} else if (CODEPAGE == s->kind) {
			if (!strcmp(value, "utf8") || !strcmp(value, "utf-8")) {
				number = 65001;
				*valid = 1;
			} else if (!strncmp(value, "cp", 2)) {
				*valid = integer(value + 2, 1, 65535, &number);
			}
		} else {
			if (s->offset == offsetof(WfConfig, lang_setting)) {
				WfLangSetting language;
				*valid = wf_i18n_config_setting(
				        value, &language);
				if (*valid) {
					number = language;
				}
			} else {
				*valid = choice(value, s->choices, &number);
			}
		}
		if (!*valid) {
			/* A removed language is recoverable display state, not a startup
			 * failure. Keep the original INI key unless the user changes it. */
			if (s->offset == offsetof(WfConfig, lang_setting)) {
				number = WF_LANGSET_EN;
			} else {
				memcpy(&number, initial, sizeof number);
			}
		}
		memcpy(target, &number,
		        sizeof number); /* int or int-sized enum member */
	}
	return WF_OK;
}
/* Cuts the next line out of `data` (without \n or a closing \r) into `line`.
 * Counts it. A line longer than the limit ends the parse. */
static WfStatus next_line(const unsigned char *data,
        size_t size,
        size_t *offset,
        char line[WF_CONFIG_MAX_LINE + 1],
        WfConfigReport *local,
        WfConfigDiagnostic diagnostic,
        void *ctx)
{
	size_t start = *offset, length;

	while (*offset < size && '\n' != data[*offset]) {
		++*offset;
	}
	length = *offset - start;
	if (length && '\r' == data[start + length - 1]) {
		length--;
	}
	local->lines++;
	if (length > WF_CONFIG_MAX_LINE) {
		warn(local, diagnostic, ctx, WF_CONFIG_LINE_TOO_LONG,
		        local->lines, NULL);
		return WF_E_RANGE;
	}
	memcpy(line, data + start, length);
	line[length] = 0;
	if (*offset < size) {
		++*offset;
	}
	return WF_OK;
}

/* "[name]": makes `name` the current section. A malformed header leaves no
 * section at all, so the keys after it are not read into the wrong one. */
static void read_section(char *text,
        char *section,
        WfConfigReport *local,
        WfConfigDiagnostic diagnostic,
        void *ctx)
{
	size_t n = strlen(text);

	section[0] = 0; /* never inherit a prior section after a malformed
	                   header */
	if (n < 3 || ']' != text[n - 1] || strchr(text + 1, '[') ||
	        strchr(text + 1, ']') != text + n - 1) {
		warn(local, diagnostic, ctx, WF_CONFIG_SYNTAX, local->lines,
		        NULL);
		return;
	}
	text[n - 1] = 0;
	text = trim(text + 1);
	if (!*text) {
		warn(local, diagnostic, ctx, WF_CONFIG_SYNTAX, local->lines,
		        NULL);
		return;
	}
	memcpy(section, text, strlen(text) + 1);
}

static size_t find_setting(const char *section, const char *key)
{
	size_t id;

	for (id = 0; id < SETTING_COUNT; id++) {
		if (!strcmp(section, settings[id].section) &&
		        !strcmp(key, settings[id].key)) {
			break;
		}
	}
	return id;
}

typedef enum
{
	DYN_NOT_MINE,
	DYN_OK,
	DYN_UNKNOWN,
	DYN_INVALID,
	DYN_DUPLICATE
} Dyn;

static bool has_prefix(const char *s, const char *prefix)
{
	return !strncmp(s, prefix, strlen(prefix));
}

static WfCustomEntry *entry_for(WfConfig *c, const char *id)
{
	WfCustomEntry *e = wf_config_custom_find_mut(c, id);

	if (e || c->custom_count >= WF_CUSTOM_THEMES) {
		return e;
	}
	e = &c->customs[c->custom_count++];
	memset(e, 0, sizeof *e);
	snprintf(e->id, sizeof e->id, "%s", id);
	snprintf(e->name, sizeof e->name, "%s",
	        !strcmp(id, "custom") ? "Custom" : id);
	wf_custom_init(&e->theme);
	return e;
}

static Dyn custom_theme_key(
        WfConfig *c, const char *section, const char *key, const char *value)
{
	const char *id = !strcmp(section, "theme.custom")
	        ? "custom"
	        : section + strlen("theme.custom.");
	WfCustomEntry *e;
	size_t slot;

	if (!wf_custom_id_valid(id)) {
		return DYN_UNKNOWN;
	}
	if (!(e = entry_for(c, id))) {
		return DYN_INVALID;
	}
	if (!strcmp(key, "name")) {
		size_t n = strlen(value);

		if (!n || n >= sizeof e->name) {
			return DYN_INVALID;
		}
		memcpy(e->name, value, n + 1);
		return DYN_OK;
	}
	if (!strcmp(key, "base")) {
		WfThemeId theme;

		if (!wf_theme_id_parse(value, &theme) ||
		        WF_THEME_CUSTOM == theme) {
			return DYN_INVALID;
		}
		e->theme.base = theme;
		return DYN_OK;
	}
	for (slot = 0; slot < WF_CUSTOM_COUNT; slot++) {
		if (!strcmp(key, wf_custom_slot_key((WfCustomSlot)slot))) {
			uint32_t rgb;

			if (!wf_color_parse_hex(value, &rgb)) {
				return DYN_INVALID;
			}
			e->theme.color[slot] = rgb;
			return DYN_OK;
		}
	}
	return DYN_UNKNOWN;
}

/* "[keyboard.custom.fm] fm.nav.up = k, Up" */
static Dyn key_override(
        WfConfig *c, const char *section, const char *key, const char *value)
{
	WfKeyContext ctx;
	const WfCommandInfo *ci = wf_cmd_by_name(key);
	WfKeyOverride o;

	if (!wf_context_from_name(section + strlen("keyboard.custom."), &ctx) ||
	        !ci || !(ci->ctx_mask & WF_CTX_BIT(ctx))) {
		return DYN_UNKNOWN;
	}
	if (!strcmp(value, "default")) { /* the base profile's keys */
		return DYN_OK;
	}
	memset(&o, 0, sizeof o);
	o.ctx = (uint8_t)ctx;
	o.cmd = ci->id;
	if (!wf_keys_parse_list(value, &o)) {
		return DYN_INVALID;
	}
	if (wf_keyconfig_find(&c->keys, ctx, ci->id)) {
		return DYN_DUPLICATE;
	}
	return wf_keyconfig_set(&c->keys, &o) ? DYN_OK : DYN_INVALID;
}

/* [places]: `a = C:\folder`. One letter, a folder that is text and fits. */
static Dyn place_key(WfConfig *c, const char *key, const char *value)
{
	size_t slot, n = strlen(value);
	Dyn dup;

	if (1 != strlen(key) || key[0] < 'a' || key[0] > 'z') {
		return DYN_UNKNOWN;
	}
	slot = (size_t)(key[0] - 'a');
	if (!n || n >= WF_PLACE_BYTES) {
		return DYN_INVALID;
	}
	for (size_t i = 0; i < n; i++) {
		if ((unsigned char)value[i] < 0x20 ||
		        0x7F == (unsigned char)value[i]) {
			return DYN_INVALID;
		}
	}
	dup = c->places[slot][0] ? DYN_DUPLICATE
	                         : DYN_OK; /* the first one stays */
	if (DYN_OK == dup) {
		memcpy(c->places[slot], value, n + 1);
	}
	return dup;
}

static Dyn dynamic(
        WfConfig *c, const char *section, const char *key, const char *value)
{
	if (!strcmp(section, "places")) {
		return place_key(c, key, value);
	}
	if (!strcmp(section, "appearance") && !strcmp(key, "custom_theme")) {
		if (!wf_custom_id_valid(value)) {
			return DYN_INVALID;
		}
		snprintf(c->custom_theme, sizeof c->custom_theme, "%s", value);
		return DYN_OK;
	}
	if (!strcmp(section, "theme.custom") ||
	        has_prefix(section, "theme.custom.")) {
		return custom_theme_key(c, section, key, value);
	}
	if (has_prefix(section, "keyboard.custom.")) {
		return key_override(c, section, key, value);
	}
	return DYN_NOT_MINE;
}

/* Configuration aliases have one identity in both the reader and writer. */
static const char *canonical_section(
        const char *section, const char *key, char out[96])
{
	const char *context = NULL;
	WfKeyContext ctx;

	if (!strcmp(section, "keymap")) {
		return !strcmp(key, "base") ? "keyboard.custom" : "keyboard";
	}
	if (!strncmp(section, "keymap.", 7)) {
		context = section + 7;
	} else if (!strncmp(section, "keyboard.custom.", 16)) {
		context = section + 16;
	}
	if (context && wf_context_from_name(context, &ctx)) {
		snprintf(out, 96, "keyboard.custom.%s", wf_context_name(ctx));
		return out;
	}
	return section;
}

static bool config_section_matches(
        const char *section, size_t n, const char *target, const char *key)
{
	char name[96], a[96], b[96];

	if (n == strlen(target) && !memcmp(section, target, n)) {
		return true;
	}
	if (n >= sizeof name) {
		return false;
	}
	memcpy(name, section, n);
	name[n] = '\0';
	return !strcmp(canonical_section(name, key, a),
	        canonical_section(target, key, b));
}

WfStatus wf_config_apply(const char *text,
        size_t n,
        const WfIniSet *sets,
        size_t count,
        char **out,
        size_t *out_n)
{
	WfConfig *config;
	WfStatus status;

	if (!out || !out_n) {
		return WF_E_INVAL;
	}
	*out = NULL;
	*out_n = 0;
	config = malloc(sizeof *config);
	if (!config) {
		return WF_E_NOMEM;
	}
	wf_config_init(config);
	status = wf_config_parse(config, text, n, NULL, NULL, NULL);
	if (WF_OK == status &&
	        config->schema_version > WF_CONFIG_SCHEMA_VERSION) {
		status = WF_E_ACCESS;
	}
	wf_config_dispose(config);
	free(config);
	if (WF_OK != status) {
		return status;
	}
	return wf_ini_apply_matching(
	        text, n, sets, count, config_section_matches, out, out_n);
}

/* Most invalid assignments warn and use defaults. Invalid explicit schema
 * versions and allocation failures abort parsing. */
static WfStatus read_assignment(WfConfig *c,
        const WfConfig *defaults,
        char *text,
        const char *section,
        size_t seen[SETTING_COUNT],
        size_t *ratio_line,
        WfConfigReport *local,
        WfConfigDiagnostic diagnostic,
        void *ctx)
{
	char *equal = strchr(text, '='), *value;
	WfStatus status;
	size_t id;
	bool valid;
	char canonical[96];

	if (!equal) {
		warn(local, diagnostic, ctx, WF_CONFIG_SYNTAX, local->lines,
		        NULL);
		return WF_OK;
	}
	*equal = 0;
	text = trim(text);
	value = trim(equal + 1);
	if (!*text || !*section) {
		warn(local, diagnostic, ctx, WF_CONFIG_SYNTAX, local->lines,
		        NULL);
		return WF_OK;
	}
	section = canonical_section(section, text, canonical);
	id = find_setting(section, text);
	if (SETTING_COUNT == id) {
		Dyn d = dynamic(c, section, text, value);

		if (DYN_OK == d) {
			return WF_OK;
		}
		warn(local, diagnostic, ctx,
		        DYN_INVALID == d             ? WF_CONFIG_INVALID_VALUE
		                : DYN_DUPLICATE == d ? WF_CONFIG_DUPLICATE_KEY
		                                     : WF_CONFIG_UNKNOWN_KEY,
		        local->lines, NULL);
		return WF_OK;
	}
	if (seen[id]) {
		warn(local, diagnostic, ctx, WF_CONFIG_DUPLICATE_KEY,
		        local->lines, &settings[id]);
	}
	seen[id] = local->lines;
	if (CODEPAGE == settings[id].kind) {
		local->text_fallback_encoding_line = local->lines;
	}
	if (settings[id].offset == offsetof(WfConfig, parent_ratio) ||
	        settings[id].offset == offsetof(WfConfig, current_ratio) ||
	        settings[id].offset == offsetof(WfConfig, preview_ratio)) {
		*ratio_line = local->lines;
	}
	status = assign(c, defaults, id, value, &valid);
	if (WF_OK == status && !valid) {
		warn(local, diagnostic, ctx, WF_CONFIG_INVALID_VALUE,
		        local->lines, &settings[id]);
		if (SCHEMA == settings[id].kind) {
			return WF_E_INVAL;
		}
	}
	return status;
}

/* The three pane ratios are only meaningful together. */
static void settle_ratios(WfConfig *c,
        size_t ratio_line,
        WfConfigReport *local,
        WfConfigDiagnostic diagnostic,
        void *ctx)
{
	if (c->parent_ratio + c->current_ratio + c->preview_ratio != 100) {
		c->parent_ratio = 25;
		c->current_ratio = 45;
		c->preview_ratio = 30;
		warn(local, diagnostic, ctx, WF_CONFIG_RATIO_SUM, ratio_line,
		        NULL);
	}
}

WfStatus wf_config_parse(WfConfig *c,
        const void *bytes,
        size_t size,
        WfConfigDiagnostic diagnostic,
        void *ctx,
        WfConfigReport *report)
{
	WfConfigReport local = {0};
	WfConfig defaults;
	const unsigned char *data = bytes;
	size_t offset = 0, seen[SETTING_COUNT] = {0}, ratio_line = 0;
	char section[WF_CONFIG_MAX_LINE + 1] = "";
	WfStatus status = WF_OK;
	if (report) {
		*report = local;
	}
	if (!c) {
		return WF_E_INVAL;
	}
	wf_config_dispose(c);
	/* Existing Custom Nano configurations keep their navigation base. */
	c->keys.version = WF_BINDINGS_COMPAT;
	wf_config_init(&defaults);
	if (!data && size) {
		return WF_E_INVAL;
	}
	if (size > WF_CONFIG_MAX_BYTES) {
		warn(&local, diagnostic, ctx, WF_CONFIG_TOO_LARGE, 0, NULL);
		status = WF_E_RANGE;
		goto done;
	}
	if (!utf8_valid(data, size)) {
		warn(&local, diagnostic, ctx, WF_CONFIG_INVALID_ENCODING, 0,
		        NULL);
		status = WF_E_INVAL;
		goto done;
	}
	if (size >= 3 && !memcmp(data, "\xEF\xBB\xBF", 3)) {
		offset = 3;
	}
	while (offset < size) {
		char line[WF_CONFIG_MAX_LINE + 1];
		char *text;

		status = next_line(
		        data, size, &offset, line, &local, diagnostic, ctx);
		if (WF_OK != status) {
			goto done;
		}
		text = trim(line);
		if (!*text || ';' == *text || '#' == *text) {
			continue;
		}
		if ('[' == *text) {
			read_section(text, section, &local, diagnostic, ctx);
		} else {
			status = read_assignment(c, &defaults, text, section,
			        seen, &ratio_line, &local, diagnostic, ctx);
		}
		if (WF_OK != status) {
			goto done;
		}
	}
	settle_ratios(c, ratio_line, &local, diagnostic, ctx);
done:
	if (WF_OK != status) {
		wf_config_dispose(c);
	}
	if (WF_OK != status || local.warnings ||
	        c->schema_version > WF_CONFIG_SCHEMA_VERSION) {
		c->update_checks = 0;
		c->updates_blocked = 1;
	}
	if (report) {
		*report = local;
	}
	return status;
}

/* The n-th alternative of "a|b|c". */
static const char *choice_at(const char *choices, int index, size_t *len)
{
	for (;;) {
		const char *end = strchr(choices, '|');

		if (index-- == 0) {
			*len = end ? (size_t)(end - choices) : strlen(choices);
			return choices;
		}
		if (!end) {
			return NULL;
		}
		choices = end + 1;
	}
}

/* The text form of a stored value; false when it has none to write. */
static bool format_value(const Setting *s, int number, char out[32])
{
	size_t n;
	const char *text;

	switch (s->kind) {
	case INTEGER:
	case SCHEMA:  snprintf(out, 32, "%d", number); return true;
	case BOOLEAN:
		snprintf(out, 32, "%s", number ? "true" : "false");
		return true;
	case CHOICE:
		text = choice_at(s->choices, number, &n);
		if (!text || n >= 32) {
			return false;
		}
		memcpy(out, text, n);
		out[n] = '\0';
		return true;
	case THEME:
	case PRESET:
		text = wf_theme_id_text((WfThemeId)number);
		if (!text) {
			return false;
		}
		snprintf(out, 32, "%s", text);
		return true;
	case HEXCOLOR:
		if (number < 0) {
			return false;
		}
		wf_color_format_hex((uint32_t)number, out);
		return true;
	case ASPECT:
	case START:
	case CODEPAGE: break;
	}
	return false;
}

typedef struct
{
	WfConfigChange *out;
	size_t cap, found;
	bool first_only; /* only "is there a difference": stop at the first */
} Sink;

/* True when nothing more is wanted of the comparison. The same functions make
 * the changes to write and answer "do they differ", so the two cannot disagree
 * about what a difference is. */
static bool enough(const Sink *k)
{
	return k->first_only && k->found > 0;
}

static void emit(Sink *k,
        const char *section,
        const char *key,
        const char *value) /* NULL: remove the key */
{
	if (k->out && k->found < k->cap) {
		WfConfigChange *ch = &k->out[k->found];

		memset(ch, 0, sizeof *ch);
		snprintf(
		        ch->section_buf, sizeof ch->section_buf, "%s", section);
		ch->section = ch->section_buf;
		ch->key = key;
		ch->remove = NULL == value;
		if (value) {
			snprintf(ch->value, sizeof ch->value, "%s", value);
		}
	}
	k->found++;
}

static void diff_fixed(const WfConfig *from, const WfConfig *to, Sink *k)
{
	size_t id;

	for (id = 0; id < SETTING_COUNT && !enough(k); id++) {
		const Setting *s = &settings[id];
		char text[32];
		int a, b;

		if (START == s->kind || ASPECT == s->kind ||
		        CODEPAGE == s->kind) {
			continue;
		}
		memcpy(&a, (const char *)from + s->offset, sizeof a);
		memcpy(&b, (const char *)to + s->offset, sizeof b);
		if (a == b || !format_value(s, b, text)) {
			continue;
		}
		if (k->out && k->found < k->cap) {
			WfConfigChange *ch = &k->out[k->found];

			memset(ch, 0, sizeof *ch);
			ch->section = s->section;
			ch->key = s->key;
			snprintf(ch->value, sizeof ch->value, "%s", text);
		}
		k->found++;
	}
}

/* The theme made before there were several keeps its old section. */
static void theme_section(const char *id, char out[40])
{
	if (!strcmp(id, "custom")) {
		snprintf(out, 40, "theme.custom");
	} else {
		snprintf(out, 40, "theme.custom.%s", id);
	}
}

static void hex_of(int64_t v, char out[8])
{
	wf_color_format_hex((uint32_t)v, out);
}

/* A custom theme that is new or changed is written whole where it differs;
 * a colour taken back to "the preset's own" is removed from the file. */
static void diff_theme(
        const WfCustomEntry *was, const WfCustomEntry *now, Sink *k)
{
	char section[40], hex[8];
	size_t slot;

	theme_section(now->id, section);
	if (!was || strcmp(was->name, now->name)) {
		emit(k, section, "name", now->name);
	}
	if (!was || was->theme.base != now->theme.base) {
		emit(k, section, "base", wf_theme_id_text(now->theme.base));
	}
	for (slot = 0; slot < WF_CUSTOM_COUNT && !enough(k); slot++) {
		int64_t a = was ? was->theme.color[slot] : -1,
		        b = now->theme.color[slot];

		if (a == b) {
			continue;
		}
		if (b < 0) {
			emit(k, section, wf_custom_slot_key((WfCustomSlot)slot),
			        NULL);
		} else {
			hex_of(b, hex);
			emit(k, section, wf_custom_slot_key((WfCustomSlot)slot),
			        hex);
		}
	}
}

static void diff_themes(const WfConfig *from, const WfConfig *to, Sink *k)
{
	int i;
	size_t slot;

	for (i = 0; i < to->custom_count && !enough(k); i++) {
		diff_theme(wf_config_custom_find(from, to->customs[i].id),
		        &to->customs[i], k);
	}
	for (i = 0; i < from->custom_count && !enough(k); i++) {
		const WfCustomEntry *gone = &from->customs[i];
		char section[40];

		if (wf_config_custom_find(to, gone->id)) {
			continue;
		}
		/* removed: its keys leave the file (the section header stays) */
		theme_section(gone->id, section);
		emit(k, section, "name", NULL);
		emit(k, section, "base", NULL);
		for (slot = 0; slot < WF_CUSTOM_COUNT; slot++) {
			if (gone->theme.color[slot] >= 0) {
				emit(k, section,
				        wf_custom_slot_key((WfCustomSlot)slot),
				        NULL);
			}
		}
	}
	if (!enough(k) && strcmp(from->custom_theme, to->custom_theme)) {
		emit(k, "appearance", "custom_theme",
		        to->custom_theme[0] ? to->custom_theme : "custom");
	}
}

static void diff_keys(const WfConfig *from, const WfConfig *to, Sink *k)
{
	size_t i;

	for (i = 0; i < to->keys.count && !enough(k); i++) {
		const WfKeyOverride *now = &to->keys.o[i],
		                    *was = wf_keyconfig_find(&from->keys,
		                            (WfKeyContext)now->ctx, now->cmd);
		const WfCommandInfo *ci = wf_cmd_info(now->cmd);
		char section[40], list[96];

		if (!ci) {
			continue;
		}
		if (was) {
			uint8_t j;
			bool same = was->n == now->n;

			for (j = 0; same && j < now->n; j++) {
				same = wf_seq_equal(&was->seq[j], &now->seq[j]);
			}
			if (same) {
				continue;
			}
		}
		snprintf(section, sizeof section, "keyboard.custom.%s",
		        wf_context_name((WfKeyContext)now->ctx));
		wf_keys_format_list(now, list, sizeof list);
		emit(k, section, ci->name, list);
	}
	for (i = 0; i < from->keys.count && !enough(k); i++) {
		const WfKeyOverride *was = &from->keys.o[i];
		const WfCommandInfo *ci = wf_cmd_info(was->cmd);
		char section[40];

		if (!ci ||
		        wf_keyconfig_find(
		                &to->keys, (WfKeyContext)was->ctx, was->cmd)) {
			continue;
		}
		snprintf(section, sizeof section, "keyboard.custom.%s",
		        wf_context_name((WfKeyContext)was->ctx));
		emit(k, section, ci->name, NULL);
	}
}

size_t wf_config_diff(const WfConfig *from,
        const WfConfig *to,
        WfConfigChange *out,
        size_t cap)
{
	Sink k = {out, cap, 0, false};

	diff_fixed(from, to, &k);
	diff_themes(from, to, &k);
	diff_keys(from, to, &k);
	return k.found < cap ? k.found : cap;
}

bool wf_config_differs(const WfConfig *a, const WfConfig *b)
{
	Sink k = {NULL, 0, 0,
	        true}; /* nothing is written down, and it ends at the first */

	diff_fixed(a, b, &k);
	diff_themes(a, b, &k);
	diff_keys(a, b, &k);
	return k.found > 0;
}

WfCustomEntry *wf_config_custom_find_mut(WfConfig *c, const char *id)
{
	int i;

	for (i = 0; i < c->custom_count; i++) {
		if (!strcmp(c->customs[i].id, id)) {
			return &c->customs[i];
		}
	}
	return NULL;
}

const WfCustomEntry *wf_config_custom_find(const WfConfig *c, const char *id)
{
	int i;

	for (i = 0; i < c->custom_count; i++) {
		if (!strcmp(c->customs[i].id, id)) {
			return &c->customs[i];
		}
	}
	return NULL;
}

const WfCustomEntry *wf_config_custom_selected(const WfConfig *c)
{
	const WfCustomEntry *e = c->custom_theme[0]
	        ? wf_config_custom_find(c, c->custom_theme)
	        : NULL;

	if (e) {
		return e;
	}
	return c->custom_count ? &c->customs[0] : NULL;
}

bool wf_config_custom_add(WfConfig *c,
        const char *id,
        const char *name,
        const WfCustomTheme *from)
{
	WfCustomEntry *e;

	if (!wf_custom_id_valid(id) || wf_config_custom_find(c, id) ||
	        c->custom_count >= WF_CUSTOM_THEMES || !name || !name[0] ||
	        strlen(name) >= WF_THEME_NAME_MAX) {
		return false;
	}
	e = &c->customs[c->custom_count++];
	memset(e, 0, sizeof *e);
	snprintf(e->id, sizeof e->id, "%s", id);
	snprintf(e->name, sizeof e->name, "%s", name);
	if (from) {
		e->theme = *from;
	} else {
		wf_custom_init(&e->theme);
	}
	return true;
}

bool wf_config_custom_remove(WfConfig *c, const char *id)
{
	int i;

	for (i = 0; i < c->custom_count; i++) {
		if (!strcmp(c->customs[i].id, id)) {
			memmove(&c->customs[i], &c->customs[i + 1],
			        (size_t)(c->custom_count - i - 1) *
			                sizeof c->customs[0]);
			c->custom_count--;
			if (!strcmp(c->custom_theme, id)) {
				c->custom_theme[0] = '\0';
			}
			return true;
		}
	}
	return false;
}

void wf_config_custom_unique_id(
        const WfConfig *c, const char *stem, char out[WF_THEME_ID_MAX])
{
	int n;

	snprintf(out, WF_THEME_ID_MAX, "%.18s", stem && *stem ? stem : "theme");
	for (n = 2; wf_config_custom_find(c, out) && n < 100; n++) {
		snprintf(out, WF_THEME_ID_MAX, "%.18s-%d",
		        stem && *stem ? stem : "theme", n);
	}
}

void wf_config_palette(const WfConfig *c, WfThemePalette *out)
{
	WfCustomTheme custom;
	const WfCustomEntry *e = wf_config_custom_selected(c);

	if (e) {
		custom = e->theme;
	} else {
		wf_custom_init(&custom);
	}
	wf_theme_palette_of((WfThemeId)c->theme, &custom, out);
}

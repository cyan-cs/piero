#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "config/wf_cli.h"
#include "edit/wf_ekeymap.h"
#include "config/wf_config.h"
#include "render/wf_theme.h"
#include "wf_test.h"
#include "base/wf_macro.h"

typedef struct
{
	size_t counts[8], last_line;
} Diagnostics;
static void warning(void *ctx,
        WfConfigWarning kind,
        size_t line,
        const char *section,
        const char *key)
{
	Diagnostics *d = ctx;
	(void)section;
	(void)key;
	d->counts[kind]++;
	d->last_line = line;
}
static WfStatus parse(WfConfig *c, const char *s, Diagnostics *d)
{
	return wf_config_parse(c, s, strlen(s), warning, d, NULL);
}

/* [editor]: defaults, every key, and what is refused. "vim" is reserved and
 * must not become a selectable preset. */
static void editor_settings(void)
{
	WfConfig c;
	Diagnostics d = {0};

	wf_config_init(&c);
	CHECK(1 == c.editor_enabled && 0 == c.editor_keymap &&
	        8 == c.editor_tab_width);
	CHECK(0 == c.editor_indent && 1 == c.editor_auto_indent &&
	        1 == c.editor_line_numbers);
	CHECK(1 == c.editor_syntax && 0 == c.editor_soft_wrap &&
	        0 == c.editor_show_whitespace);
	CHECK(1 == c.editor_backup && 32 == c.editor_max_mib &&
	        16 == c.editor_undo_mib);
	CHECK(1 == c.editor_recovery_draft); /* drafts are on unless turned off */

	CHECK(parse(&c,
	              "[editor]\nenabled = false\nkeymap = nano\ntab_width = "
	              "4\n"
	              "indent = spaces\nauto_indent = false\nline_numbers = "
	              "false\n"
	              "syntax_highlight = false\nsoft_wrap = true\n"
	              "show_whitespace = true\nbackup = true\ncase_sensitive = "
	              "true\n"
	              "max_size_mib = 64\nundo_mib = 4\n",
	              &d) == WF_OK);
	CHECK(0 == c.editor_enabled && 1 == c.editor_keymap &&
	        4 == c.editor_tab_width);
	CHECK(1 == c.editor_indent && 0 == c.editor_auto_indent &&
	        0 == c.editor_line_numbers);
	CHECK(0 == c.editor_syntax && 1 == c.editor_soft_wrap &&
	        1 == c.editor_show_whitespace);
	CHECK(1 == c.editor_backup && 1 == c.editor_case_sensitive);
	CHECK(64 == c.editor_max_mib && 4 == c.editor_undo_mib);
	CHECK(0 == d.counts[WF_CONFIG_INVALID_VALUE] &&
	        0 == d.counts[WF_CONFIG_UNKNOWN_KEY]);
	CHECK(WF_OK == parse(&c, "[editor]\nbackup = false\n", &d));
	CHECK(1 == c.editor_backup && 1 == d.counts[WF_CONFIG_INVALID_VALUE]);

	/* bad values keep the previous (default) value and are reported */
	wf_config_init(&c);
	memset(&d, 0, sizeof d);
	CHECK(parse(&c,
	              "[editor]\nkeymap = vim\ntab_width = 0\nmax_size_mib = "
	              "100000\n"
	              "indent = tabs-and-spaces\nundo_mib = -3\n",
	              &d) == WF_OK);
	CHECK(0 == c.editor_keymap && 8 == c.editor_tab_width &&
	        32 == c.editor_max_mib);
	CHECK(0 == c.editor_indent && 16 == c.editor_undo_mib);
	CHECK(5 == d.counts[WF_CONFIG_INVALID_VALUE]);

	/* Recovery drafts can be disabled and re-enabled independently. */
	wf_config_init(&c);
	CHECK(WF_OK == parse(&c, "[editor]\nrecovery_draft = false\n", &d));
	CHECK(0 == c.editor_recovery_draft);
	CHECK(WF_OK == parse(&c, "[editor]\nrecovery_draft = true\n", &d));
	CHECK(1 == c.editor_recovery_draft);
}

/* Catches ignored values, resetting the entire config on a local error,
 * permissive booleans, and duplicates preserving the previous valid value. */
static void values_and_diagnostics(void)
{
	WfConfig c;
	Diagnostics d = {0};
	WfConfigReport r;
	const char *s = "\xEF\xBB\xBF[ui]\r\nparent_ratio=0\r\ncurrent_ratio="
	                "100\r\npreview_ratio="
	                "0\r\nshow_hidden=true\r\nshow_hidden=TRUE\r\n[preview]"
	                "\r\ntext_max_kib="
	                "1024\r\nremote_auto_preview=yes\r\nunknown=true\r\n";
	wf_config_init(&c);
	CHECK(WF_OK == wf_config_parse(&c, s, strlen(s), warning, &d, &r));
	CHECK(0 == c.parent_ratio && 100 == c.current_ratio &&
	        0 == c.preview_ratio);
	CHECK(0 == c.show_hidden && 1024 == c.text_max_kib);
	CHECK(0 == c.remote_auto_preview);
	CHECK(1 == d.counts[WF_CONFIG_DUPLICATE_KEY]);
	CHECK(2 == d.counts[WF_CONFIG_INVALID_VALUE]);
	CHECK(1 == d.counts[WF_CONFIG_UNKNOWN_KEY] && 10 == d.last_line);
	CHECK(4 == r.warnings && 10 == r.lines);
	wf_config_dispose(&c);
}

/* Catches unsupported comments/quotes/escapes becoming an accepted value,
 * numeric overflow, an unsafe default, and invalid ratio combinations. */
static void rejects_unsafe_values(void)
{
	WfConfig c;
	Diagnostics d = {0};
	wf_config_init(&c);
	CHECK(parse(&c,
	              "[ui]\nparent_ratio=90\nshow_hidden=true # "
	              "hi\n[preview]\nimage_cell_aspect=nan\nimage_cache_mib="
	              "99999999999999999999999999\ntext_max_kib=15\ncloud_auto_"
	              "preview="
	              "1\n[behavior]\nconfirm_unc_navigation=garbage\nstart_"
	              "directory="
	              "relative\\path\n[copy]\nverify=\"none\"\n",
	              &d) == WF_OK);
	CHECK(25 == c.parent_ratio && 45 == c.current_ratio &&
	        30 == c.preview_ratio);
	CHECK(0 == c.show_hidden && 1.0 == c.image_cell_aspect);
	CHECK(32 == c.image_cache_mib && 256 == c.text_max_kib);
	CHECK(0 == c.cloud_auto_preview && 1 == c.confirm_unc_navigation);
	CHECK(NULL == c.start_directory && WF_VERIFY_SHA256 == c.copy_verify);
	CHECK(1 == d.counts[WF_CONFIG_RATIO_SUM] &&
	        8 == d.counts[WF_CONFIG_INVALID_VALUE]);
	wf_config_dispose(&c);
}

static void ratio_warning_names_the_last_ratio_line(void)
{
	static const char *const ratios[] = {
	        "parent_ratio", "current_ratio", "preview_ratio"};
	WfConfig c;

	wf_config_init(&c);
	for (size_t i = 0; i < WF_ARRAY_LEN(ratios); i++) {
		char text[128];
		Diagnostics d = {0};

		snprintf(text, sizeof text,
		        "[general]\nlanguage=en\n[ui]\n%s=1\n"
		        "[updates]\nconsent_recorded=true\n",
		        ratios[i]);
		CHECK(WF_OK == parse(&c, text, &d));
		CHECK(d.counts[WF_CONFIG_RATIO_SUM] == 1 && d.last_line == 4);
		CHECK(c.parent_ratio == 25 && c.current_ratio == 45 &&
		        c.preview_ratio == 30);
	}
	wf_config_dispose(&c);
}

static void encoding_and_boundaries(void)
{
	WfConfig c;
	Diagnostics d = {0};
	char *bytes = malloc(262146);
	const char bad[] = "[preview]\nremote_auto_preview=true\n#\xC0\xAF";
	const unsigned char utf16[] = {0xFF, 0xFE, '[', 0};
	const char nul[] = {'#', 0, 'x'};
	wf_config_init(&c);
	CHECK(NULL != bytes);
	if (!bytes) {
		return;
	}
	CHECK(WF_E_INVAL ==
	        wf_config_parse(&c, bad, sizeof bad - 1, warning, &d, NULL));
	CHECK(0 == c.remote_auto_preview);
	CHECK(WF_E_INVAL ==
	        wf_config_parse(&c, utf16, sizeof utf16, warning, &d, NULL));
	CHECK(WF_E_INVAL ==
	        wf_config_parse(&c, nul, sizeof nul, warning, &d, NULL));
	memset(bytes, '#', 4096);
	bytes[4096] = '\n';
	CHECK(WF_OK == wf_config_parse(&c, bytes, 4097, warning, &d, NULL));
	memset(bytes, '#', 4097);
	CHECK(WF_E_RANGE ==
	        wf_config_parse(&c, bytes, 4097, warning, &d, NULL));
	memset(bytes, '\n', 262144);
	CHECK(WF_OK == wf_config_parse(&c, bytes, 262144, warning, &d, NULL));
	CHECK(WF_E_RANGE ==
	        wf_config_parse(&c, bytes, 262145, warning, &d, NULL));
	CHECK(WF_OK ==
	        parse(&c, "[behavior]\nstart_directory=C:\\日本語\\😀\n", &d));
	CHECK(c.start_directory &&
	        0 == strcmp(c.start_directory, "C:\\日本語\\😀"));
	free(bytes);
	wf_config_dispose(&c);
}

static void all_documented_settings(void)
{
	WfConfig c;
	Diagnostics d = {0};
	wf_config_init(&c);
	CHECK(parse(&c,
	              "; comment\n# "
	              "comment\n[ui]\nlanguage=en\ncolor_mode=256\nshow_hidden="
	              "true\n["
	              "terminal]\nambiguous_width=2\n[preview]\nenabled="
	              "false\nimages="
	              "false\nimage_style=off\nimage_cell_aspect=0.5\nimage_"
	              "cache_mib="
	              "4\ntext_max_kib=16\ntext_fallback_encoding=utf8\nremote_"
	              "auto_"
	              "preview=true\ncloud_auto_preview=true\n[files]"
	              "\ndirectories_first="
	              "false\nsort_by=ext\nsort_descending=true\n[copy]"
	              "\nverify=sha256\n["
	              "launch]\nworking_directory=file_dir\n[behavior]\nstart_"
	              "directory="
	              "home\nconfirm_unc_navigation=false\n[diagnostics]\nlog_"
	              "level="
	              "debug\nlog_paths=true\n",
	              &d) == WF_OK);
	CHECK(0 == d.last_line);
	CHECK(WF_COLOR_256 == c.color_mode && 1 == c.show_hidden &&
	        2 == c.ambiguous_width);
	CHECK(!c.preview_enabled && !c.images && WF_IMAGE_OFF == c.image_style);
	CHECK(0.5 == c.image_cell_aspect && 4 == c.image_cache_mib &&
	        16 == c.text_max_kib);
	CHECK(WF_FALLBACK_UTF8 == c.text_fallback_encoding &&
	        c.remote_auto_preview && c.cloud_auto_preview);
	CHECK(!c.directories_first && WF_CONFIG_SORT_EXT == c.sort_by &&
	        c.sort_descending);
	CHECK(WF_VERIFY_SHA256 == c.copy_verify &&
	        WF_LAUNCH_FILE_DIR == c.launch_working_directory);
	CHECK(c.start_directory && 0 == strcmp(c.start_directory, "home"));
	CHECK(!c.confirm_unc_navigation && WF_LOG_DEBUG == c.log_level &&
	        c.log_paths);
	wf_config_dispose(&c);
}

static void cli_validation(void)
{
	WfCli c;
	WfCliReport r;
	const wchar_t *valid[] = {L"wfile", L"--config", L"設定.ini",
	        L"--print-last-dir", L"last.txt", L"C:\\日本語"};
	const wchar_t *unknown[] = {L"wfile", L"--force"};
	const wchar_t *missing[] = {L"wfile", L"--config", L"--help"};
	const wchar_t *extra[] = {L"wfile", L"one", L"two"};
	const wchar_t *conflict[] = {L"wfile", L"--help", L"--version"};
	const wchar_t *duplicate[] = {
	        L"wfile", L"--config", L"one", L"--config", L"two"};
	const wchar_t *dash[] = {L"wfile", L"--", L"-folder"};
	const wchar_t *abandon[] = {L"wfile", L"--abandon-recovery", L"id-1"};
	const wchar_t *no_mix[] = {L"wfile", L"--reset-terminal", L"folder"};
	wf_cli_init(&c);
	CHECK(WF_OK == wf_cli_parse(&c, 6, valid, &r));
	CHECK(c.path && 0 == wcscmp(c.path, L"C:\\日本語"));
	CHECK(c.config_path && 0 == wcscmp(c.config_path, L"設定.ini"));
	CHECK(c.print_last_dir && 0 == wcscmp(c.print_last_dir, L"last.txt"));
	CHECK(WF_E_INVAL == wf_cli_parse(&c, 2, unknown, &r) &&
	        WF_CLI_ERROR_UNKNOWN_OPTION == r.error);
	CHECK(!c.path && !c.config_path);
	CHECK(WF_E_INVAL == wf_cli_parse(&c, 3, missing, &r) &&
	        WF_CLI_ERROR_MISSING_VALUE == r.error);
	CHECK(WF_E_INVAL == wf_cli_parse(&c, 3, extra, &r));
	CHECK(WF_E_INVAL == wf_cli_parse(&c, 3, conflict, &r) &&
	        WF_CLI_ERROR_CONFLICT == r.error);
	CHECK(WF_E_INVAL == wf_cli_parse(&c, 5, duplicate, &r) &&
	        WF_CLI_ERROR_DUPLICATE == r.error);
	CHECK(WF_OK == wf_cli_parse(&c, 3, dash, &r) && c.path &&
	        0 == wcscmp(c.path, L"-folder"));
	CHECK(WF_OK == wf_cli_parse(&c, 3, abandon, &r) &&
	        WF_CLI_ABANDON_RECOVERY == c.action);
	CHECK(c.recovery_id && 0 == wcscmp(c.recovery_id, L"id-1"));
	CHECK(WF_E_INVAL == wf_cli_parse(&c, 3, no_mix, &r));
	wf_cli_dispose(&c);
}

/* Removing an upper/lower bound, accepting UTF-8 surrogates, keeping a
 * prior permissive duplicate, or imposing MAX_PATH makes these fail. */
/* A malformed section header must not leave the previous section active, and
 * a repeated key keeps its first value's default on an invalid repeat. */
static void sections_and_duplicate_keys(void)
{
	WfConfig c;
	Diagnostics d = {0};

	wf_config_init(&c);
	CHECK(WF_OK ==
	        parse(&c, "[preview]\n[broken\nremote_auto_preview=true\n",
	                &d));
	CHECK(0 == c.remote_auto_preview && 2 == d.counts[WF_CONFIG_SYNTAX]);
	CHECK(parse(&c,
	              "[preview]\nremote_auto_preview=true\nremote_auto_"
	              "preview="
	              "TRUE\ncloud_auto_preview=true\ncloud_auto_preview=no\n["
	              "behavior]"
	              "\nconfirm_unc_navigation=false\nconfirm_unc_navigation="
	              "0\n",
	              &d) == WF_OK);
	CHECK(!c.remote_auto_preview && !c.cloud_auto_preview &&
	        c.confirm_unc_navigation);
	wf_config_dispose(&c);
}

/* Bad UTF-8 is refused outright; empty input is a valid, empty file. */
static void encoding_and_empty_input(void)
{
	WfConfig c;
	WfConfigReport r;
	unsigned char surrogate[] = {'#', 0xED, 0xA0, 0x80};

	wf_config_init(&c);
	CHECK(wf_config_parse(&c, surrogate, sizeof surrogate, NULL, NULL,
	              &r) == WF_E_INVAL &&
	        1 == r.warnings);
	CHECK(WF_OK == wf_config_parse(&c, NULL, 0, NULL, NULL, &r) &&
	        0 == r.lines && 0 == r.warnings);
	wf_config_dispose(&c);
}

/* The preview limits accept their bounds and fall back to the defaults just
 * outside them, or when the number carries trailing junk. */
static void preview_value_ranges(void)
{
	WfConfig c;
	Diagnostics d = {0};

	wf_config_init(&c);
	CHECK(parse(&c,
	              "[preview]\nimage_cell_aspect=2.0\nimage_cache_mib="
	              "128\ntext_max_"
	              "kib=1024\n",
	              &d) == WF_OK);
	CHECK(2.0 == c.image_cell_aspect && 128 == c.image_cache_mib &&
	        1024 == c.text_max_kib);
	CHECK(parse(&c,
	              "[preview]\nimage_cell_aspect=2.01\nimage_cache_mib="
	              "129\ntext_max_"
	              "kib=1025\n",
	              &d) == WF_OK);
	CHECK(1.0 == c.image_cell_aspect && 32 == c.image_cache_mib &&
	        256 == c.text_max_kib);
	CHECK(parse(&c,
	              "[preview]\nimage_cell_aspect=0.49\nimage_cache_mib="
	              "3\ntext_max_"
	              "kib=16junk\n",
	              &d) == WF_OK);
	CHECK(1.0 == c.image_cell_aspect && 32 == c.image_cache_mib &&
	        256 == c.text_max_kib);
	wf_config_dispose(&c);
}

/* Fallback code pages: known names map to their numbers, an impossible one
 * falls back to CP932. */
static void fallback_encoding_values(void)
{
	WfConfig c;
	Diagnostics d = {0};

	wf_config_init(&c);
	CHECK(WF_OK ==
	        parse(&c, "[preview]\ntext_fallback_encoding=cp1252\n", &d));
	CHECK(1252 == c.text_fallback_encoding);
	CHECK(WF_OK ==
	        parse(&c, "[preview]\ntext_fallback_encoding=utf-8\n", &d));
	CHECK(65001 == c.text_fallback_encoding);
	CHECK(WF_OK ==
	        parse(&c, "[preview]\ntext_fallback_encoding=cp65536\n", &d));
	CHECK(932 == c.text_fallback_encoding);
	wf_config_dispose(&c);
}

/* Command-line text must be valid UTF-16, a very long path survives intact,
 * and the parsed path is a copy of the argument. */
static void cli_text_and_argument_copy(void)
{
	WfCli cli;
	WfCliReport cr;
	wchar_t lone[] = {(wchar_t)0xD800, 0};
	wchar_t *long_path = malloc(40001 * sizeof *long_path);
	const wchar_t *args[] = {L"wfile", long_path};
	const wchar_t *invalid[] = {L"wfile", lone};

	wf_cli_init(&cli);
	CHECK(WF_E_INVAL == wf_cli_parse(&cli, 2, invalid, &cr) &&
	        WF_CLI_ERROR_INVALID_TEXT == cr.error);
	CHECK(NULL != long_path);
	if (long_path) {
		for (size_t i = 0; i < 40000; i++) {
			long_path[i] = L'x';
		}
		long_path[40000] = 0;
		CHECK(WF_OK == wf_cli_parse(&cli, 2, args, &cr));
		CHECK(cli.path && 40000 == wcslen(cli.path));
		long_path[0] = L'y';
		CHECK(cli.path && L'x' == cli.path[0]);
	}
	free(long_path);
	wf_cli_dispose(&cli);
}

static void focused_regressions(void)
{
	sections_and_duplicate_keys();
	encoding_and_empty_input();
	preview_value_ranges();
	fallback_encoding_values();
	cli_text_and_argument_copy();
}

static void cli_actions(void)
{
	static const struct
	{
		const wchar_t *option;
		WfCliAction action;
	} cases[] = {{L"--help", WF_CLI_HELP}, {L"--version", WF_CLI_VERSION},
	        {L"--reset-terminal", WF_CLI_RESET_TERMINAL},
	        {L"--purge-state", WF_CLI_PURGE_STATE},
	        {L"--image-worker", WF_CLI_IMAGE_WORKER}};
	WfCli cli;
	WfCliReport report;
	wf_cli_init(&cli);
	for (size_t i = 0; i < WF_ARRAY_LEN(cases); i++) {
		const wchar_t *args[] = {L"wfile", cases[i].option};
		CHECK(WF_OK == wf_cli_parse(&cli, 2, args, &report) &&
		        cli.action == cases[i].action);
	}
	/* --setup is a flag, not an action; it combines with a path and --config */
	{
		const wchar_t *args[] = {L"wfile", L"--setup", L"dir"};

		CHECK(WF_OK == wf_cli_parse(&cli, 3, args, &report) &&
		        1 == cli.setup && WF_CLI_BROWSE == cli.action &&
		        NULL != cli.path);
	}
	{
		const wchar_t *args[] = {
		        L"wfile", L"--config", L"c.ini", L"--setup"};

		CHECK(WF_OK == wf_cli_parse(&cli, 4, args, &report) &&
		        1 == cli.setup && NULL != cli.config_path);
	}
	{
		const wchar_t *args[] = {L"wfile"};

		CHECK(WF_OK == wf_cli_parse(&cli, 1, args, &report) &&
		        0 == cli.setup);
	}
	wf_cli_dispose(&cli);
}

/* Deterministic byte-stream fuzz: bounded random content and corrupted UTF-8
 * appended after an explicitly permissive setting must remain fail-closed. */
static void fuzz_bytes(void)
{
	uint32_t seed = 0x9e3779b9u;
	unsigned char bytes[1024];
	WfConfig c;
	WfConfigReport r;
	wf_config_init(&c);
	for (size_t iteration = 0; iteration < 30000; iteration++) {
		seed ^= seed << 13;
		seed ^= seed >> 17;
		seed ^= seed << 5;
		size_t n = seed % sizeof bytes;
		for (size_t i = 0; i < n; i++) {
			seed ^= seed << 13;
			seed ^= seed >> 17;
			seed ^= seed << 5;
			bytes[i] = (unsigned char)seed;
		}
		WfStatus status = wf_config_parse(&c, bytes, n, NULL, NULL, &r);
		CHECK(WF_OK == status || WF_E_INVAL == status ||
		        WF_E_RANGE == status);
		CHECK(c.parent_ratio + c.current_ratio + c.preview_ratio ==
		        100);
		CHECK(c.text_max_kib >= 16 && c.text_max_kib <= 1024);
		CHECK(0 == c.remote_auto_preview && 0 == c.cloud_auto_preview &&
		        1 == c.confirm_unc_navigation);
		if (wf_test_failures) {
			break;
		}
	}
	for (size_t i = 0; i < 256; i++) {
		const char prefix[] = "[preview]\nremote_auto_preview=true\n#";
		memcpy(bytes, prefix, sizeof prefix - 1);
		bytes[sizeof prefix - 1] = (unsigned char)i;
		if (0 == i || i >= 0x80) {
			CHECK(wf_config_parse(&c, bytes, sizeof prefix, NULL,
			              NULL, &r) == WF_E_INVAL);
			CHECK(0 == c.remote_auto_preview);
		}
	}
	wf_config_dispose(&c);
}

/* Rejecting the explicit HMAC choice or changing the safe default fails. */
static void hmac_verify_choice(void)
{
	WfConfig c;
	Diagnostics d = {0};
	wf_config_init(&c);
	CHECK(WF_VERIFY_SHA256 == c.copy_verify); /* the standard check */
	CHECK(WF_OK == parse(&c, "[copy]\nverify=none\n", &d));
	CHECK(WF_VERIFY_NONE == c.copy_verify &&
	        0 == d.counts[WF_CONFIG_INVALID_VALUE]); /* explicit opt-out */
	CHECK(WF_OK == parse(&c, "[copy]\nverify=hmac-sha256\n", &d));
	CHECK(WF_VERIFY_HMAC_SHA256 == c.copy_verify &&
	        0 == d.counts[WF_CONFIG_INVALID_VALUE]);
	CHECK(WF_OK == parse(&c, "[copy]\nverify=HMAC-SHA256\n", &d));
	CHECK(WF_VERIFY_SHA256 == c.copy_verify &&
	        1 == d.counts[WF_CONFIG_INVALID_VALUE]); /* bad value: default */
	wf_config_dispose(&c);
}

/* [meta], [appearance], [theme.custom] and [preview] directory_preview. */
static void theme_settings(void)
{
	WfConfig c;
	Diagnostics d = {0};

	wf_config_init(&c);
	CHECK(WF_THEME_SPADE == c.theme && WF_COLOR_AUTO == c.color_policy);
	CHECK(0 == c.onboarding_completed && 0 == c.schema_version);
	CHECK(1 == c.dir_preview && 0 == c.custom_count &&
	        NULL == wf_config_custom_selected(&c));
	CHECK(WF_PROFILE_LEGACY == c.keys.profile && 0 == c.keys.count);

	CHECK(parse(&c,
	              "[meta]\nschema_version = 1\nonboarding_completed = "
	              "true\n[appearance]\ntheme = queen\ncolor_mode = 256\n"
	              "[preview]\ndirectory_preview = false\n",
	              &d) == WF_OK);
	CHECK(0 == d.counts[WF_CONFIG_INVALID_VALUE] &&
	        0 == d.counts[WF_CONFIG_UNKNOWN_KEY]);
	CHECK(1 == c.schema_version && 1 == c.onboarding_completed &&
	        WF_THEME_QUEEN == c.theme &&
	        WF_COLOR_POLICY_256 == c.color_policy && 0 == c.dir_preview);

	/* every ID, and the retired names, read as the preset they became */
	{
		static const struct
		{
			const char *text;
			int theme;
		} ids[] = {{"spade", WF_THEME_SPADE}, {"heart", WF_THEME_HEART},
		        {"diamond", WF_THEME_DIAMOND}, {"club", WF_THEME_CLUB},
		        {"joker", WF_THEME_JOKER}, {"king", WF_THEME_KING},
		        {"queen", WF_THEME_QUEEN}, {"ace", WF_THEME_ACE},
		        {"custom", WF_THEME_CUSTOM},
		        {"midnight", WF_THEME_SPADE},
		        {"sakura", WF_THEME_HEART}, {"snow", WF_THEME_DIAMOND},
		        {"forest", WF_THEME_CLUB}, {"sunset", WF_THEME_KING},
		        {"amethyst", WF_THEME_QUEEN},
		        {"graphite", WF_THEME_ACE}};

		for (size_t i = 0; i < WF_ARRAY_LEN(ids); i++) {
			char text[64];

			snprintf(text, sizeof text,
			        "[appearance]\ntheme = %s\n", ids[i].text);
			CHECK(WF_OK == parse(&c, text, &d) &&
			        c.theme == ids[i].theme);
		}
	}

	/* a theme that does not exist keeps the default and says so */
	memset(&d, 0, sizeof d);
	CHECK(WF_OK == parse(&c, "[appearance]\ntheme = paper\n", &d) &&
	        WF_THEME_SPADE == c.theme &&
	        1 == d.counts[WF_CONFIG_INVALID_VALUE]);
	memset(&d, 0, sizeof d);
	CHECK(parse(&c, "[appearance]\ntheme = Spade\ncolor_mode = rgb\n",
	              &d) == WF_OK &&
	        WF_THEME_SPADE == c.theme && WF_COLOR_AUTO == c.color_policy &&
	        2 == d.counts[WF_CONFIG_INVALID_VALUE]);

	/* the custom theme */
	memset(&d, 0, sizeof d);
	CHECK(parse(&c,
	              "[theme.custom]\nbase = king\nbackground = #112233\n"
	              "accent = #abcdef\nborder = #FFFFFF\n",
	              &d) == WF_OK &&
	        0 == d.counts[WF_CONFIG_INVALID_VALUE]);
	/* the one theme of the earlier format is the entry "custom" */
	{
		const WfCustomEntry *e = wf_config_custom_find(&c, "custom");

		CHECK(1 == c.custom_count && e && !strcmp(e->name, "Custom"));
		CHECK(WF_THEME_KING == e->theme.base &&
		        0x112233 == e->theme.color[WF_CUSTOM_BACKGROUND] &&
		        0xABCDEF == e->theme.color[WF_CUSTOM_ACCENT] &&
		        0xFFFFFF == e->theme.color[WF_CUSTOM_BORDER] &&
		        -1 == e->theme.color[WF_CUSTOM_SURFACE]);
	}
	memset(&d, 0, sizeof d);
	CHECK(parse(&c,
	              "[theme.custom]\nbase = custom\nbackground = 112233\n"
	              "surface = #12345\nforeground = #1234567\n"
	              "accent = #GGGGGG\nmuted = # 12345\n"
	              "selection_background = #-12345\nselection_foreground = "
	              "red\n",
	              &d) == WF_OK &&
	        8 == d.counts[WF_CONFIG_INVALID_VALUE]);
	{
		/* a base must be a preset; nothing bad was taken */
		const WfCustomEntry *e = wf_config_custom_find(&c, "custom");

		CHECK(!e ||
		        (WF_THEME_SPADE == e->theme.base &&
		                -1 == e->theme.color[WF_CUSTOM_BACKGROUND]));
	}

	/* a newer file than this program knows about is noticed, not trusted */
	memset(&d, 0, sizeof d);
	CHECK(WF_E_INVAL == parse(&c, "[meta]\nschema_version = 0\n", &d) &&
	        1 == d.counts[WF_CONFIG_INVALID_VALUE] &&
	        0 == c.schema_version);
	CHECK(WF_OK == parse(&c, "[meta]\nschema_version = 7\n", &d) &&
	        7 == c.schema_version);
	CHECK(WF_OK == parse(&c, "[meta]\nschema_version = 1001\n", &d) &&
	        1001 == c.schema_version);
	CHECK(WF_OK == parse(&c, "[meta]\nschema_version = 2147483647\n", &d) &&
	        2147483647 == c.schema_version);
	CHECK(WF_E_INVAL ==
	        parse(&c, "[meta]\nschema_version = 2147483648\n", &d));
	CHECK(WF_E_INVAL == parse(&c, "[meta]\nschema_version = future\n", &d));
	wf_config_dispose(&c);
}

/* What differs between two configurations, as file text. */
static void changes_between_configurations(void)
{
	WfConfig a, b;
	WfConfigChange ch[64];
	size_t n;

	wf_config_init(&a);
	wf_config_init(&b);
	CHECK(0 == wf_config_diff(&a, &b, ch, 64));

	b.theme = WF_THEME_JOKER;
	b.onboarding_completed = 1;
	b.schema_version = 1;
	b.color_policy = WF_COLOR_POLICY_16;
	b.show_hidden = 1;
	b.sort_by = WF_CONFIG_SORT_DATE;
	b.sort_descending = 1;
	b.editor_tab_width = 4;
	b.editor_keymap = 1;
	b.dir_preview = 0;
	{
		WfCustomTheme t;

		wf_custom_init(&t);
		t.base = WF_THEME_HEART;
		t.color[WF_CUSTOM_ACCENT] = 0x00FF7F;
		CHECK(wf_config_custom_add(&b, "custom", "Custom", &t));
	}
	n = wf_config_diff(&a, &b, ch, 64);
	CHECK(13 == n);
	CHECK(wf_config_differs(&a, &b) && !wf_config_differs(&a, &a));
	for (size_t i = 0; i < n; i++) {
		const WfConfigChange *c = &ch[i];

		if (!strcmp(c->key, "theme")) {
			CHECK(!strcmp(c->section, "appearance") &&
			        !strcmp(c->value, "joker"));
		} else if (!strcmp(c->key, "color_mode")) {
			CHECK(!strcmp(c->value, "16"));
		} else if (!strcmp(c->key, "onboarding_completed")) {
			CHECK(!strcmp(c->section, "meta") &&
			        !strcmp(c->value, "true"));
		} else if (!strcmp(c->key, "schema_version")) {
			CHECK(!strcmp(c->value, "1"));
		} else if (!strcmp(c->key, "sort_by")) {
			CHECK(!strcmp(c->value, "date"));
		} else if (!strcmp(c->key, "editor_tab_width") ||
		        !strcmp(c->key, "tab_width")) {
			CHECK(!strcmp(c->value, "4"));
		} else if (!strcmp(c->key, "keymap")) {
			CHECK(!strcmp(c->value, "nano"));
		} else if (!strcmp(c->key, "directory_preview")) {
			CHECK(!strcmp(c->value, "false"));
		} else if (!strcmp(c->key, "name")) {
			CHECK(!strcmp(c->section, "theme.custom") &&
			        !strcmp(c->value, "Custom"));
		} else if (!strcmp(c->key, "base")) {
			CHECK(!strcmp(c->section, "theme.custom") &&
			        !strcmp(c->value, "heart"));
		} else if (!strcmp(c->key, "accent")) {
			CHECK(!strcmp(c->value, "#00FF7F"));
		} else if (!strcmp(c->key, "show_hidden") ||
		        !strcmp(c->key, "sort_descending")) {
			CHECK(!strcmp(c->value, "true"));
		} else {
			CHECK_FAIL("an unexpected change");
		}
	}
	/* limited room: the count is what fits, nothing is written past it */
	CHECK(3 == wf_config_diff(&a, &b, ch, 3));
	CHECK(0 == wf_config_diff(&a, &b, ch, 0));
	/* an unset custom colour is not written */
	b.customs[0].theme.color[WF_CUSTOM_ACCENT] = -1;
	CHECK(wf_config_custom_add(&a, "custom", "Custom", NULL));
	a.customs[0].theme.color[WF_CUSTOM_ACCENT] = 0x123456;
	n = wf_config_diff(&a, &b, ch, 64);
	{
		bool removed = false;

		for (size_t i = 0; i < n; i++) {
			if (!strcmp(ch[i].key, "accent")) {
				removed =
				        ch[i].remove; /* taken out, not written */
				CHECK(removed);
			}
		}
		CHECK(removed);
	}
	/* a start directory is not part of the comparison */
	b.start_directory = _strdup("C:\\somewhere");
	CHECK(wf_config_diff(&a, &b, ch, 64) == n);
	wf_config_dispose(&a);
	wf_config_dispose(&b);
}

/* "Do they differ" and "what differs" are one comparison: whichever single thing
 * is made different, the answer is yes exactly when there is a change to write. */
static void differs_agrees_with_the_list_of_changes(void)
{
	WfConfig a, b;
	WfConfigChange *ch = calloc(512, sizeof *ch);
	Diagnostics d = {0};
	WfCustomTheme t;

	CHECK(NULL != ch);
	wf_config_init(&a);
	wf_config_init(&b);
	CHECK(!wf_config_differs(&a, &b) &&
	        0 == wf_config_diff(&a, &b, ch, 512));
	/* a plain setting, the last kind of setting compared first and the others later */
	b.show_hidden = !a.show_hidden;
	CHECK(wf_config_differs(&a, &b) &&
	        wf_config_diff(&a, &b, ch, 512) >= 1);
	wf_config_init(&b);
	/* a custom theme that is new, then one that is changed, then one that is gone */
	wf_custom_init(&t);
	t.base = WF_THEME_HEART;
	CHECK(wf_config_custom_add(&b, "mine", "Mine", &t));
	CHECK(wf_config_differs(&a, &b) &&
	        wf_config_diff(&a, &b, ch, 512) >= 1);
	CHECK(wf_config_differs(&b, &a) &&
	        wf_config_diff(&b, &a, ch, 512) >= 1);
	a = b;
	CHECK(!wf_config_differs(&a, &b) &&
	        0 == wf_config_diff(&a, &b, ch, 512));
	{
		WfCustomEntry *e = wf_config_custom_find_mut(&b, "mine");

		CHECK(e);
		e->theme.color[WF_CUSTOM_ACCENT] = 0x112233;
	}
	CHECK(wf_config_differs(&a, &b) &&
	        wf_config_diff(&a, &b, ch, 512) >= 1);
	/* a key override that is new, and one that is gone */
	wf_config_init(&a);
	wf_config_init(&b);
	CHECK(parse(&b,
	              "[keyboard]\nprofile = custom\n[keyboard.custom.fm]\n"
	              "fm.quit = none\n",
	              &d) == WF_OK);
	CHECK(wf_config_differs(&a, &b) &&
	        wf_config_diff(&a, &b, ch, 512) >= 1);
	CHECK(wf_config_differs(&b, &a) &&
	        wf_config_diff(&b, &a, ch, 512) >= 1);
	a = b;
	CHECK(!wf_config_differs(&a, &b) &&
	        0 == wf_config_diff(&a, &b, ch, 512));
	free(ch);
}

/* [keyboard] and the named custom themes: read, written, read again. */
static void keyboard_and_themes(void)
{
	WfConfig a, b;
	Diagnostics d = {0};
	WfConfigChange *ch = calloc(512, sizeof *ch);
	size_t n, i;
	const WfKeyOverride *o;

	CHECK(NULL != ch);
	wf_config_init(&a);
	CHECK(parse(&a,
	              "[keyboard]\nprofile = custom\n[keyboard.custom]\nbase = "
	              "vim\n"
	              "[keyboard.custom.fm]\nfm.nav.down = n, Down\n"
	              "fm.quit = none\n[keyboard.custom.edit]\ned.save = "
	              "Ctrl+W\n",
	              &d) == WF_OK);
	CHECK(WF_PROFILE_CUSTOM == a.keys.profile &&
	        WF_BASE_VIM == a.keys.base);
	o = wf_keyconfig_find(&a.keys, WF_CTX_FM, WF_FM_NAV_DOWN);
	CHECK(o && 2 == o->n);
	o = wf_keyconfig_find(&a.keys, WF_CTX_FM, WF_FM_QUIT);
	CHECK(o && 0 == o->n);
	CHECK(0 == d.counts[WF_CONFIG_INVALID_VALUE] &&
	        0 == d.counts[WF_CONFIG_UNKNOWN_KEY]);

	/* bad things are warnings, and are not taken */
	memset(&d, 0, sizeof d);
	CHECK(parse(&a,
	              "[keyboard]\nprofile = emacs\n"
	              "[keyboard.custom.fm]\nfm.nav.down = Hyper+Bogus\n"
	              "no.such.command = x\nfm.nav.up = j\nfm.nav.up = k\n"
	              "[keyboard.custom.nowhere]\nfm.nav.up = x\n",
	              &d) == WF_OK);
	CHECK(WF_PROFILE_LEGACY == a.keys.profile);
	CHECK(NULL == wf_keyconfig_find(&a.keys, WF_CTX_FM, WF_FM_NAV_DOWN));
	CHECK(d.counts[WF_CONFIG_INVALID_VALUE] >= 2 &&
	        d.counts[WF_CONFIG_UNKNOWN_KEY] >= 2 &&
	        d.counts[WF_CONFIG_DUPLICATE_KEY] >= 1);

	/* the same file written the way yazi lays it out: [keymap], layers named
	 * mgr / pager / editor..., keys as <C-s> */
	wf_config_init(&a);
	memset(&d, 0, sizeof d);
	CHECK(parse(&a,
	              "[keymap]\nprofile = custom\nbase = vim\n"
	              "[keymap.mgr]\nfm.nav.down = n, <Down>\nfm.quit = <C-q>\n"
	              "[keymap.editor-normal]\ned.save = <C-s>, <A-w>\n"
	              "[keymap.pager]\npv.close = <Esc>, q\n",
	              &d) == WF_OK);
	CHECK(0 == d.counts[WF_CONFIG_INVALID_VALUE] &&
	        0 == d.counts[WF_CONFIG_UNKNOWN_KEY]);
	CHECK(WF_PROFILE_CUSTOM == a.keys.profile &&
	        WF_BASE_VIM == a.keys.base);
	o = wf_keyconfig_find(&a.keys, WF_CTX_FM, WF_FM_NAV_DOWN);
	CHECK(o && 2 == o->n);
	o = wf_keyconfig_find(&a.keys, WF_CTX_FM, WF_FM_QUIT);
	CHECK(o && 1 == o->n && WF_MOD_CTRL == o->seq[0].s[0].mods &&
	        'q' == o->seq[0].s[0].code);
	CHECK(NULL !=
	        wf_keyconfig_find(&a.keys, WF_CTX_VIM_NORMAL, WF_CMD_SAVE));
	CHECK(NULL != wf_keyconfig_find(&a.keys, WF_CTX_PREVIEW, WF_PV_CLOSE));

	/* a theme ID that cannot be written back is not taken */
	memset(&d, 0, sizeof d);
	CHECK(parse(&a,
	              "[theme.custom.Bad Id]\nname = x\n[theme.custom.]\nname "
	              "= y\n"
	              "[appearance]\ncustom_theme = Not Valid\n",
	              &d) == WF_OK);
	CHECK(0 == a.custom_count && '\0' == a.custom_theme[0]);
	CHECK(d.counts[WF_CONFIG_UNKNOWN_KEY] >= 2 &&
	        d.counts[WF_CONFIG_INVALID_VALUE] >= 1);

	/* named themes */
	memset(&d, 0, sizeof d);
	CHECK(parse(&a,
	              "[appearance]\ntheme = custom\ncustom_theme = mine\n"
	              "[theme.custom.mine]\nname = My theme\nbase = club\n"
	              "accent = #123456\nerror = #FF0000\n"
	              "[theme.custom.other]\nname = Other\n",
	              &d) == WF_OK);
	CHECK(WF_THEME_CUSTOM == a.theme && 2 == a.custom_count);
	{
		const WfCustomEntry *e = wf_config_custom_selected(&a);

		CHECK(e && !strcmp(e->id, "mine") &&
		        !strcmp(e->name, "My theme"));
		CHECK(WF_THEME_CLUB == e->theme.base &&
		        0x123456 == e->theme.color[WF_CUSTOM_ACCENT] &&
		        0xFF0000 == e->theme.color[WF_CUSTOM_ERROR]);
	}
	CHECK(!wf_custom_id_valid("Bad Id") && !wf_custom_id_valid("") &&
	        wf_custom_id_valid("a_b-9"));

	/* what differs is written, and what is written reads back the same */
	wf_config_init(&b);
	n = wf_config_diff(&b, &a, ch, 512);
	CHECK(n > 0 && n < 512);
	{
		char text[4096] = "";
		char section[48] = "";

		for (i = 0; i < n; i++) {
			if (strcmp(section, ch[i].section)) {
				snprintf(section, sizeof section, "%s",
				        ch[i].section);
				snprintf(text + strlen(text),
				        sizeof text - strlen(text), "[%s]\n",
				        section);
			}
			if (!ch[i].remove) {
				snprintf(text + strlen(text),
				        sizeof text - strlen(text), "%s = %s\n",
				        ch[i].key, ch[i].value);
			}
		}
		wf_config_init(&b);
		memset(&d, 0, sizeof d);
		CHECK(WF_OK == parse(&b, text, &d));
		CHECK(!wf_config_differs(&a, &b));
		CHECK(0 == d.counts[WF_CONFIG_INVALID_VALUE] &&
		        0 == d.counts[WF_CONFIG_UNKNOWN_KEY]);
	}

	/* removing a theme removes its keys */
	CHECK(wf_config_custom_remove(&b, "other"));
	n = wf_config_diff(&a, &b, ch, 512);
	CHECK(n >= 1);
	for (i = 0; i < n; i++) {
		CHECK(ch[i].remove || !strcmp(ch[i].section, "appearance"));
	}
	{
		char id[WF_THEME_ID_MAX];

		wf_config_custom_unique_id(&a, "mine", id);
		CHECK(0 != strcmp(id, "mine") && wf_custom_id_valid(id));
	}
	free(ch);
	wf_config_dispose(&a);
	wf_config_dispose(&b);
}

static void existing_custom_navigation_is_preserved(void)
{
	WfConfig c;
	Diagnostics d = {0};
	WfKeymap *m = NULL;
	WfKeyIssue issue;
	char keys[64];
	wf_config_init(&c);
	CHECK(parse(&c,
	              "[keyboard]\nprofile=custom\n[keyboard.custom]\nbase="
	              "nano\n",
	              &d) == WF_OK);
	CHECK(1 == c.keys.version);
	CHECK(WF_OK == wf_keymap_build(&c.keys, 0, &m, &issue));
	wf_keymap_keys_text(m, WF_CTX_FM, WF_FM_NAV_DOWN, keys, sizeof keys);
	CHECK(!strcmp(keys, "j, Down"));
	wf_keymap_free(m);
	CHECK(parse(&c,
	              "[keyboard]\nprofile=custom\nbindings_version=2\n["
	              "keyboard.custom]\nbase=nano\n",
	              &d) == WF_OK);
	CHECK(2 == c.keys.version);
	CHECK(WF_OK == wf_keymap_build(&c.keys, 0, &m, &issue));
	wf_keymap_keys_text(m, WF_CTX_FM, WF_FM_NAV_DOWN, keys, sizeof keys);
	CHECK(!strcmp(keys, "Down, Ctrl+N"));
	wf_keymap_free(m);
	wf_config_dispose(&c);
}

/* [places] (DR-DRAFT-030): a letter and a folder. What is refused leaves the
 * place empty and says so; the rest of the file is read as before. */
static void places_are_read_one_letter_one_folder(void)
{
	WfConfig c;
	Diagnostics d = {0};

	wf_config_init(&c);
	CHECK(WF_OK ==
	        parse(&c,
	                "[places]\na = C:\\Work\nz = D:\\a folder\\with spaces"
	                "\nm = \\\\server\\share\\x\n",
	                &d));
	CHECK(!strcmp(c.places[0], "C:\\Work"));
	CHECK(!strcmp(c.places[25], "D:\\a folder\\with spaces"));
	CHECK(!strcmp(c.places['m' - 'a'], "\\\\server\\share\\x"));
	CHECK(!c.places[1][0] && !c.places[2][0]);
	CHECK(0 == d.counts[WF_CONFIG_UNKNOWN_KEY] &&
	        0 == d.counts[WF_CONFIG_INVALID_VALUE] &&
	        0 == d.counts[WF_CONFIG_DUPLICATE_KEY]);
	wf_config_dispose(&c);

	/* UTF-8 in a folder name is kept byte for byte */
	wf_config_init(&c);
	memset(&d, 0, sizeof d);
	CHECK(WF_OK ==
	        parse(&c, "[places]\nj = C:\\\xe6\x97\xa5\xe6\x9c\xac\\x\n",
	                &d));
	CHECK(!strcmp(c.places['j' - 'a'], "C:\\\xe6\x97\xa5\xe6\x9c\xac\\x"));
	wf_config_dispose(&c);

	/* a letter twice: the first stays and the repeat is said */
	wf_config_init(&c);
	memset(&d, 0, sizeof d);
	CHECK(WF_OK == parse(&c, "[places]\na = C:\\One\na = C:\\Two\n", &d));
	CHECK(!strcmp(c.places[0], "C:\\One"));
	CHECK(1 == d.counts[WF_CONFIG_DUPLICATE_KEY]);
	wf_config_dispose(&c);

	/* not a letter a-z, an empty folder, a control character, a folder too long */
	{
		char big[WF_PLACE_BYTES + 64];

		memset(big, 'x', sizeof big);
		memcpy(big, "[places]\nq = C:\\", 15);
		memcpy(big + sizeof big - 3, "\n", 2);
		wf_config_init(&c);
		memset(&d, 0, sizeof d);
		CHECK(WF_OK ==
		        wf_config_parse(
		                &c, big, strlen(big), warning, &d, NULL));
		CHECK(!c.places['q' - 'a'][0] &&
		        1 == d.counts[WF_CONFIG_INVALID_VALUE]);
		wf_config_dispose(&c);
	}
	wf_config_init(&c);
	memset(&d, 0, sizeof d);
	CHECK(WF_OK ==
	        parse(&c,
	                "[places]\nA = C:\\Up\nab = C:\\Two\n1 = C:\\Digit\n"
	                "c =\nd = C:\\\x01x\ne = C:\\Ok\n",
	                &d));
	CHECK(3 == d.counts[WF_CONFIG_UNKNOWN_KEY]);
	CHECK(2 == d.counts[WF_CONFIG_INVALID_VALUE]);
	CHECK(!c.places[2][0] && !c.places[3][0]);
	CHECK(!strcmp(c.places[4], "C:\\Ok")); /* the rest is read as before */
	wf_config_dispose(&c);

	/* the same key in another section is not a place */
	wf_config_init(&c);
	memset(&d, 0, sizeof d);
	CHECK(WF_OK == parse(&c, "[ui]\na = C:\\Nope\n", &d));
	CHECK(!c.places[0][0] && 1 == d.counts[WF_CONFIG_UNKNOWN_KEY]);
	wf_config_dispose(&c);
}

/* A place is written as one key of [places] and nothing else in the file moves:
 * existing keys, comments and other sections stay as they were. */
static void a_place_is_written_as_one_key_and_nothing_else_moves(void)
{
	static const char original[] =
	        "# my settings\n[ui]\nparent_ratio = 25\n";
	WfIniSet set = {"places", "a", "C:\\Work"};
	char *out = NULL;
	size_t n = 0;
	WfConfig c;
	Diagnostics d = {0};

	CHECK(WF_OK ==
	        wf_config_apply(original, strlen(original), &set, 1, &out, &n));
	CHECK(out && strstr(out, "# my settings\n[ui]\nparent_ratio = 25\n"));
	CHECK(strstr(out, "[places]"));
	wf_config_init(&c);
	CHECK(WF_OK == wf_config_parse(&c, out, n, warning, &d, NULL));
	CHECK(!strcmp(c.places[0], "C:\\Work") && 25 == c.parent_ratio);
	CHECK(0 == d.counts[WF_CONFIG_UNKNOWN_KEY]);
	wf_config_dispose(&c);

	/* a second place joins the section; the first stays; a repeat of the
	 * letter replaces its value */
	{
		WfIniSet more = {"places", "b", "D:\\Data"};
		WfIniSet again = {"places", "a", "E:\\Else"};
		char *out2 = NULL, *out3 = NULL;
		size_t n2 = 0, n3 = 0;

		CHECK(WF_OK == wf_config_apply(out, n, &more, 1, &out2, &n2));
		CHECK(WF_OK ==
		        wf_config_apply(out2, n2, &again, 1, &out3, &n3));
		wf_config_init(&c);
		CHECK(WF_OK ==
		        wf_config_parse(&c, out3, n3, warning, &d, NULL));
		CHECK(!strcmp(c.places[0], "E:\\Else"));
		CHECK(!strcmp(c.places[1], "D:\\Data"));
		CHECK(0 == d.counts[WF_CONFIG_DUPLICATE_KEY]);
		wf_config_dispose(&c);
		free(out2);
		free(out3);
	}
	free(out);
}

/* The change record is wide enough for a long folder (it used to hold 95 bytes). */
static void a_change_can_hold_a_long_folder(void)
{
	WfConfigChange ch;
	char path[WF_PLACE_BYTES];

	memset(path, 'p', sizeof path - 1);
	path[sizeof path - 1] = '\0';
	snprintf(ch.value, sizeof ch.value, "%s", path);
	CHECK(strlen(ch.value) == WF_PLACE_BYTES - 1);
}

int main(void)
{
	keyboard_and_themes();
	existing_custom_navigation_is_preserved();
	places_are_read_one_letter_one_folder();
	a_place_is_written_as_one_key_and_nothing_else_moves();
	a_change_can_hold_a_long_folder();
	editor_settings();
	theme_settings();
	changes_between_configurations();
	differs_agrees_with_the_list_of_changes();
	hmac_verify_choice();
	values_and_diagnostics();
	rejects_unsafe_values();
	ratio_warning_names_the_last_ratio_line();
	encoding_and_boundaries();
	all_documented_settings();
	cli_validation();
	focused_regressions();
	cli_actions();
	fuzz_bytes();
	return TEST_RESULT();
}

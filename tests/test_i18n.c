/*
 * The message catalog: every id has both languages, the placeholders agree
 * between them and with the declared arity, formatting is bounded and safe,
 * and the language rules (Windows' list, the setting, the plural) behave.
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "i18n/wf_i18n.h"
#include "wf_test.h"

/* Which of {0}..{3} a text uses; false if a brace is not well formed. */
static bool placeholders(const char *text, unsigned *mask)
{
	*mask = 0;
	for (const char *p = text; *p; p++) {
		if ('{' == p[0] && '{' == p[1]) {
			p++;
		} else if ('{' == p[0]) {
			if (p[1] < '0' || p[1] >= '0' + WF_MSG_MAX_ARGS ||
			        '}' != p[2]) {
				return false;
			}
			*mask |= 1u << (p[1] - '0');
			p += 2;
		} else if ('}' == p[0]) {
			return false; /* a lone closing brace */
		}
	}
	return true;
}

static unsigned arity_mask(size_t arity)
{
	return arity >= 32 ? ~0u : (1u << arity) - 1u;
}

static bool valid_utf8(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;

	while (*p) {
		size_t n = *p < 0x80      ? 1
		        : 6 == (*p >> 5)  ? 2
		        : 14 == (*p >> 4) ? 3
		        : 30 == (*p >> 3) ? 4
		                          : 0;

		if (!n) {
			return false;
		}
		for (size_t i = 1; i < n; i++) {
			if (0x80 != (p[i] & 0xC0)) {
				return false;
			}
		}
		p += n;
	}
	return true;
}

/* Every registered language: a text of its own for every id, the same
 * placeholders as the English one, valid UTF-8, nothing that acts on a
 * terminal. A new language is held to exactly this. */
static void every_message_is_complete_and_consistent(void)
{
	for (int lang = 0; lang < WF_UILANG_COUNT; lang++) {
		CHECK(wf_i18n_lang_id((WfUiLang)lang)[0]);
		CHECK(valid_utf8(wf_i18n_lang_name((WfUiLang)lang)));
		for (int id = 0; id < WF_MSG_COUNT; id++) {
			const char *name = wf_msg_name((WfMsg)id);
			const char *en = wf_msg_in(WF_UILANG_EN, (WfMsg)id);
			const char *tx = wf_msg_in((WfUiLang)lang, (WfMsg)id);
			unsigned men = 0, mtx = 0;

			CHECK(!strncmp(name, "MSG_", 4));
			CHECK(en && en[0] &&
			        0 != strcmp(en, name)); /* a real English text */
			if (!wf_msg_has((WfUiLang)lang, (WfMsg)id)) {
				printf("missing in %s: %s\n",
				        wf_i18n_lang_id((WfUiLang)lang), name);
				CHECK(false);
			}
			CHECK(tx && tx[0] && 0 != strcmp(tx, name));
			CHECK(valid_utf8(en) && valid_utf8(tx));
			CHECK(placeholders(en, &men) && placeholders(tx, &mtx));
			if (men != mtx ||
			        men != arity_mask(wf_msg_arity((WfMsg)id))) {
				printf("placeholders differ or do not match "
				       "the "
				       "arity: %s (%s)\n",
				        name, wf_i18n_lang_id((WfUiLang)lang));
				CHECK(false);
			}
			CHECK(wf_msg_arity((WfMsg)id) <= WF_MSG_MAX_ARGS);
			/* nothing in a catalog text may act on the terminal */
			for (const char *p = tx; *p; p++) {
				CHECK((unsigned char)*p >= 0x20 && 0x7f != *p);
			}
		}
	}
}

/* The language table drives the rest: ids are unique, round-trip through the
 * setting, and the tags find their language. */
static void the_languages_table_is_consistent(void)
{
	for (int a = 0; a < WF_UILANG_COUNT; a++) {
		WfLangSetting s;
		WfUiLang found;
		wchar_t tag[16];
		const char *id = wf_i18n_lang_id((WfUiLang)a);

		for (int b = a + 1; b < WF_UILANG_COUNT; b++) {
			CHECK(0 != strcmp(id, wf_i18n_lang_id((WfUiLang)b)));
		}
		CHECK(wf_i18n_parse_setting(id, &s) &&
		        s == wf_i18n_setting_of((WfUiLang)a) &&
		        wf_i18n_resolve(s, WF_UILANG_EN) == (WfUiLang)a);
		for (size_t i = 0; id[i]; i++) {
			tag[i] = (wchar_t)id[i], tag[i + 1] = 0;
		}
		CHECK(wf_i18n_classify_tag(tag, &found) &&
		        found == (WfUiLang)a);
		CHECK(wf_msg_has((WfUiLang)a, MSG_SETTINGS_TITLE));
	}
	CHECK(!wf_msg_has(WF_UILANG_COUNT, MSG_SETTINGS_TITLE));
	CHECK(!wf_msg_has(WF_UILANG_EN, WF_MSG_COUNT));
}

static void ids_are_unique_and_every_text_is_distinct_in_meaning(void)
{
	for (int a = 0; a < WF_MSG_COUNT; a++) {
		for (int b = a + 1; b < WF_MSG_COUNT; b++) {
			CHECK(strcmp(wf_msg_name((WfMsg)a),
			              wf_msg_name((WfMsg)b)) != 0);
		}
	}
	/* the same ids give different texts in the two languages (the table is
	 * not just English twice), apart from a few that are the same words */
	{
		int same = 0;

		for (int id = 0; id < WF_MSG_COUNT; id++) {
			if (!strcmp(wf_msg_in(WF_UILANG_EN, (WfMsg)id),
			            wf_msg_in(WF_UILANG_JA, (WfMsg)id))) {
				same++;
			}
		}
		CHECK(same < WF_MSG_COUNT / 20);
	}
}

static void bad_ids_and_languages_are_safe(void)
{
	CHECK(!strcmp(wf_msg_in(WF_UILANG_EN, (WfMsg)-1), "MSG_INVALID"));
	CHECK(!strcmp(wf_msg_in(WF_UILANG_EN, WF_MSG_COUNT), "MSG_INVALID"));
	CHECK(!strcmp(wf_msg_name(WF_MSG_COUNT), "MSG_INVALID"));
	CHECK(0 == wf_msg_arity(WF_MSG_COUNT));
	/* an unknown language shows English */
	CHECK(!strcmp(wf_msg_in((WfUiLang)99, MSG_SETTINGS_TITLE), "Settings"));
	CHECK(!strcmp(wf_msg_in((WfUiLang)-1, MSG_SETTINGS_TITLE), "Settings"));
	wf_i18n_set_language((WfUiLang)99); /* refused */
	CHECK(WF_UILANG_EN == wf_i18n_language());
}

static void the_language_switches_the_text(void)
{
	wf_i18n_set_language(WF_UILANG_EN);
	CHECK(!strcmp(wf_msg(MSG_SETTINGS_TITLE), "Settings"));
	wf_i18n_set_language(WF_UILANG_JA);
	CHECK(WF_UILANG_JA == wf_i18n_language());
	CHECK(!strcmp(wf_msg(MSG_SETTINGS_TITLE), "設定"));
	CHECK(!strcmp(wf_msg(MSG_SMART_GO_NO_MATCHES), "一致する候補なし"));
	CHECK(!strcmp(
	        wf_msg(MSG_ERROR_PATH_NOT_FOUND), "パスが見つかりません"));
	wf_i18n_set_language(WF_UILANG_EN);
}

static void formatting_fills_places_and_stays_inside_the_buffer(void)
{
	char out[64];
	size_t n;

	wf_i18n_set_language(WF_UILANG_EN);
	n = wf_msgf(out, sizeof out, MSG_OPS_EXISTS, &WF_ARG_S("main.c"), 1);
	CHECK(n == strlen(out) && n < sizeof out); /* cut, not overrun */
	n = wf_msgf(out, sizeof out, MSG_OPEN_OPENED, &WF_ARG_S("a.txt"), 1);
	CHECK(!strcmp(out, "Opened: a.txt") && n == strlen(out));
	wf_i18n_set_language(WF_UILANG_JA);
	wf_msgf(out, sizeof out, MSG_OPEN_OPENED, &WF_ARG_S("a.txt"), 1);
	CHECK(!strcmp(out, "開きました: a.txt"));
	wf_i18n_set_language(WF_UILANG_EN);

	/* a number, a negative one, the largest one */
	wf_msgf(out, sizeof out, MSG_PREVIEW_COLUMN, &WF_ARG_N(-42), 1);
	CHECK(!strcmp(out, "[col -42]"));
	wf_msgf(out, sizeof out, MSG_PREVIEW_COLUMN, &WF_ARG_N(LLONG_MIN), 1);
	CHECK(!strcmp(out, "[col -9223372036854775808]"));
	wf_msgf(out, sizeof out, MSG_PREVIEW_COLUMN, &WF_ARG_N(LLONG_MAX), 1);
	CHECK(!strcmp(out, "[col 9223372036854775807]"));

	/* two arguments, in the order the text asks for */
	wf_msgf(out, sizeof out, MSG_NAV_ERROR,
	        (WfMsgArg[]){WF_ARG_S("ACCESS"), WF_ARG_S("denied")}, 2);
	CHECK(!strcmp(out, "ERROR ACCESS: denied"));

	/* a missing argument prints nothing; extra ones are ignored */
	wf_msgf(out, sizeof out, MSG_NAV_ERROR, &WF_ARG_S("X"), 1);
	CHECK(!strcmp(out, "ERROR X: "));
	wf_msgf(out, sizeof out, MSG_NAV_ERROR, NULL, 0);
	CHECK(!strcmp(out, "ERROR : "));
	wf_msgf(out, sizeof out, MSG_SETTINGS_TITLE,
	        (WfMsgArg[]){WF_ARG_S("x"), WF_ARG_S("y")}, 2);
	CHECK(!strcmp(out, "Settings"));

	/* control characters in a file name never reach the screen */
	wf_msgf(out, sizeof out, MSG_OPEN_OPENED,
	        &WF_ARG_S("a\x1b[31m\r\nb\x7f"), 1);
	CHECK(!strcmp(out, "Opened: a?[31m??b?"));
}

static void a_cut_never_splits_a_character(void)
{
	char out[24];
	size_t n;

	wf_i18n_set_language(WF_UILANG_JA);
	for (size_t cap = 1; cap < sizeof out; cap++) {
		memset(out, 'x', sizeof out);
		n = wf_msgf(out, cap, MSG_SETTINGS_HELP_SYNTAX, NULL, 0);
		CHECK(n < cap && '\0' == out[n] && n == strlen(out));
		CHECK(valid_utf8(out));
	}
	/* a cap of zero writes nothing */
	out[0] = 'z';
	CHECK(0 == wf_msgf(out, 0, MSG_SETTINGS_TITLE, NULL, 0) &&
	        'z' == out[0]);
	CHECK(0 == wf_msgf(NULL, 10, MSG_SETTINGS_TITLE, NULL, 0));
	/* an argument cut mid-character too */
	for (size_t cap = 1; cap < 20; cap++) {
		wf_msgf(out, cap, MSG_OPEN_OPENED,
		        &WF_ARG_S("日本語のファイル"), 1);
		CHECK(valid_utf8(out));
	}
	wf_i18n_set_language(WF_UILANG_EN);
}

static void braces_can_be_escaped(void)
{
	/* no catalog text needs it, but the rule is part of the format */
	char out[32];

	wf_i18n_set_language(WF_UILANG_EN);
	CHECK(placeholders("a {{ b {0}", &(unsigned){0}));
	CHECK(!placeholders("a {x}", &(unsigned){0}));
	CHECK(!placeholders("a {4}", &(unsigned){0}));
	CHECK(!placeholders("a }", &(unsigned){0}));
	CHECK(!placeholders("a {0", &(unsigned){0}));
	(void)out;
}

static void counts_follow_each_languages_rule(void)
{
	wf_i18n_set_language(WF_UILANG_EN);
	CHECK(MSG_INFO_FILES_ONE ==
	        wf_msg_count(MSG_INFO_FILES_ONE, MSG_INFO_FILES_OTHER, 1));
	CHECK(MSG_INFO_FILES_OTHER ==
	        wf_msg_count(MSG_INFO_FILES_ONE, MSG_INFO_FILES_OTHER, 0));
	CHECK(MSG_INFO_FILES_OTHER ==
	        wf_msg_count(MSG_INFO_FILES_ONE, MSG_INFO_FILES_OTHER, 2));
	wf_i18n_set_language(WF_UILANG_JA);
	CHECK(MSG_INFO_FILES_OTHER ==
	        wf_msg_count(MSG_INFO_FILES_ONE, MSG_INFO_FILES_OTHER, 1));
	{
		char out[64];

		wf_msgf(out, sizeof out, MSG_INFO_FILES_OTHER, &WF_ARG_N(1), 1);
		CHECK(!strcmp(out, "ファイル1件"));
	}
	wf_i18n_set_language(WF_UILANG_EN);
	{
		char out[64];

		wf_msgf(out, sizeof out, MSG_INFO_FILES_ONE, &WF_ARG_N(1), 1);
		CHECK(!strcmp(out, "1 file"));
		wf_msgf(out, sizeof out, MSG_INFO_FILES_OTHER, &WF_ARG_N(2), 1);
		CHECK(!strcmp(out, "2 files"));
	}
}

static void reasons_are_translated_by_their_english_text(void)
{
	wf_i18n_set_language(WF_UILANG_EN);
	CHECK(!strcmp(wf_msg_reason("reading failed"), "reading failed"));
	wf_i18n_set_language(WF_UILANG_JA);
	CHECK(!strcmp(
	        wf_msg_reason("reading failed"), "読み取りに失敗しました"));
	CHECK(!strcmp(wf_msg_reason("PNG image"), "PNG 画像"));
	/* what the catalog does not know stays as it is; NULL stays NULL */
	CHECK(!strcmp(wf_msg_reason("something new"), "something new"));
	CHECK(NULL == wf_msg_reason(NULL));
	/* only ids named MSG_REASON_* answer, not any message with that text */
	CHECK(!strcmp(wf_msg_reason("Saved."), "Saved."));
	wf_i18n_set_language(WF_UILANG_EN);
}

static void tags_and_lists_pick_a_supported_language(void)
{
	WfUiLang l = WF_UILANG_EN;

	CHECK(wf_i18n_classify_tag(L"ja", &l) && WF_UILANG_JA == l);
	CHECK(wf_i18n_classify_tag(L"ja-JP", &l) && WF_UILANG_JA == l);
	CHECK(wf_i18n_classify_tag(L"JA_jp", &l) && WF_UILANG_JA == l);
	CHECK(wf_i18n_classify_tag(L"en-US", &l) && WF_UILANG_EN == l);
	CHECK(wf_i18n_classify_tag(L"en-GB", &l) && WF_UILANG_EN == l);
	CHECK(wf_i18n_classify_tag(L"en", &l) && WF_UILANG_EN == l);
	CHECK(!wf_i18n_classify_tag(L"fr-FR", &l));
	CHECK(!wf_i18n_classify_tag(L"jav", &l)); /* Javanese is not Japanese */
	CHECK(!wf_i18n_classify_tag(L"jaxx-JP", &l));
	CHECK(!wf_i18n_classify_tag(L"", &l));
	CHECK(!wf_i18n_classify_tag(NULL, &l));
	CHECK(!wf_i18n_classify_tag(L"zh-CN", &l));

	/* GetUserPreferredUILanguages: double-NUL-terminated, best first */
	CHECK(WF_UILANG_JA == wf_i18n_classify_list(L"ja-JP\0en-US\0"));
	CHECK(WF_UILANG_EN == wf_i18n_classify_list(L"en-US\0ja-JP\0"));
	CHECK(WF_UILANG_JA == wf_i18n_classify_list(L"fr-FR\0ja-JP\0"));
	CHECK(WF_UILANG_EN == wf_i18n_classify_list(L"fr-FR\0de-DE\0"));
	CHECK(WF_UILANG_EN == wf_i18n_classify_list(L""));
	CHECK(WF_UILANG_EN == wf_i18n_classify_list(NULL));
	CHECK(WF_UILANG_EN == wf_i18n_classify_list(L"en-GB\0"));
	/* the machine's own answer is always a language we have */
	l = wf_i18n_detect();
	CHECK(WF_UILANG_EN == l || WF_UILANG_JA == l);
}

static void the_setting_beats_what_windows_says(void)
{
	CHECK(WF_UILANG_JA == wf_i18n_resolve(WF_LANGSET_AUTO, WF_UILANG_JA));
	CHECK(WF_UILANG_EN == wf_i18n_resolve(WF_LANGSET_AUTO, WF_UILANG_EN));
	CHECK(WF_UILANG_EN == wf_i18n_resolve(WF_LANGSET_EN, WF_UILANG_JA));
	CHECK(WF_UILANG_JA == wf_i18n_resolve(WF_LANGSET_JA, WF_UILANG_EN));
	CHECK(WF_UILANG_EN == wf_i18n_resolve(WF_LANGSET_AUTO, (WfUiLang)7));
	CHECK(WF_UILANG_JA == wf_i18n_resolve((WfLangSetting)9, WF_UILANG_JA));
}

static void setting_ids_round_trip_and_bad_ones_are_refused(void)
{
	WfLangSetting s = WF_LANGSET_EN;

	for (int i = 0; i < WF_LANGSET_COUNT; i++) {
		CHECK(wf_i18n_parse_setting(
		        wf_i18n_setting_id((WfLangSetting)i), &s));
		CHECK((int)s == i);
	}
	CHECK(wf_i18n_parse_setting("auto", &s) && WF_LANGSET_AUTO == s);
	CHECK(wf_i18n_parse_setting("en", &s) && WF_LANGSET_EN == s);
	CHECK(wf_i18n_parse_setting("ja", &s) && WF_LANGSET_JA == s);
	s = WF_LANGSET_JA;
	CHECK(!wf_i18n_parse_setting("fr", &s) && WF_LANGSET_JA == s);
	CHECK(!wf_i18n_parse_setting("JA", &s));
	CHECK(!wf_i18n_parse_setting("", &s));
	CHECK(!wf_i18n_parse_setting("ja ", &s));
	CHECK(!wf_i18n_parse_setting(NULL, &s));
	CHECK(!strcmp(wf_i18n_setting_id((WfLangSetting)99), "auto"));
}

int main(void)
{
	every_message_is_complete_and_consistent();
	the_languages_table_is_consistent();
	ids_are_unique_and_every_text_is_distinct_in_meaning();
	bad_ids_and_languages_are_safe();
	the_language_switches_the_text();
	formatting_fills_places_and_stays_inside_the_buffer();
	a_cut_never_splits_a_character();
	braces_can_be_escaped();
	counts_follow_each_languages_rule();
	reasons_are_translated_by_their_english_text();
	tags_and_lists_pick_a_supported_language();
	the_setting_beats_what_windows_says();
	setting_ids_round_trip_and_bad_ones_are_refused();
	return TEST_RESULT();
}

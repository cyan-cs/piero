#ifndef WF_I18N_H
#define WF_I18N_H

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

/* Stable message IDs and English emergency data are generated from INI.
 * External catalogs never own returned pointers. Interned strings stay valid
 * across switches within a bounded process-lifetime storage budget. */
typedef int WfUiLang;
typedef int WfLangSetting;
enum
{
	WF_UILANG_EN = 0,
	WF_UILANG_JA = 1
};
enum
{
	WF_LANGSET_AUTO = 0,
	WF_LANGSET_EN = 1,
	WF_LANGSET_JA = 2
};
#define WF_I18N_LANG_LIMIT 32
#define WF_UILANG_COUNT (wf_i18n_lang_count())
#define WF_LANGSET_COUNT (wf_i18n_lang_count() + 1)
#include "wf_msg_ids.h"

/* Optional explicit directories, before any lookup (embedders/tests).
 * Otherwise use executable/languages and LOCALAPPDATA/piero/languages.
 * Discovery and selection run on the UI thread; lookups are thread safe. */
void wf_i18n_initialize(const wchar_t *bundle, const wchar_t *user);
void wf_i18n_refresh(void);
int wf_i18n_lang_count(void);
size_t wf_i18n_storage_used(void);
const char *wf_i18n_lang_id(WfUiLang lang);
const char *wf_i18n_lang_name(WfUiLang lang);
bool wf_i18n_lang_available(WfUiLang lang);
/* Preserve a valid configured code even when its file is absent. */
bool wf_i18n_config_setting(const char *text, WfLangSetting *out);
WfLangSetting wf_i18n_setting_of(WfUiLang lang);
extern char wf_langset_choices[WF_I18N_LANG_LIMIT * 33 + 5];

/* One argument of a message: a string (file name, path...) or a number. */
typedef struct
{
	const char *s; /* UTF-8; NULL for a number */
	long long n;
} WfMsgArg;
#define WF_ARG_S(text) ((WfMsgArg){(text), 0})
#define WF_ARG_N(number) ((WfMsgArg){NULL, (long long)(number)})

#define WF_MSG_MAX_ARGS 4

/* The language the screen is drawn in. English until set. */
WfUiLang wf_i18n_language(void);
void wf_i18n_set_language(WfUiLang lang);

/* Language ids as written in the configuration. */
const char *wf_i18n_setting_id(WfLangSetting s);
bool wf_i18n_parse_setting(const char *text, WfLangSetting *out);

/* "ja", "ja-JP", "en-GB", ... -> a supported language; false for others. */
bool wf_i18n_classify_tag(const wchar_t *tag, WfUiLang *out);
/* A double-NUL-terminated list of tags in preference order (the way
 * GetUserPreferredUILanguages returns it): the first supported one wins,
 * English when none is. */
WfUiLang wf_i18n_classify_list(const wchar_t *tags);
/* Windows' preferred UI language; English when it cannot be read. */
WfUiLang wf_i18n_detect(void);
/* The language a setting means, given what was detected. */
WfUiLang wf_i18n_resolve(WfLangSetting setting, WfUiLang detected);

/* The text of `id`: never NULL, never empty. */
const char *wf_msg(WfMsg id);
const char *wf_msg_in(WfUiLang lang, WfMsg id);
/* Whether the language has a text of its own for `id` (English always does);
 * without one wf_msg_in shows the English text. */
bool wf_msg_has(WfUiLang lang, WfMsg id);
/* A reason some lower layer reported in English (static text such as
 * "reading failed"): the same reason in the current language when the catalog
 * has it under MSG_REASON_*, else the text itself. Never NULL for non-NULL. */
const char *wf_msg_reason(const char *english);
/* The stable name of the id ("MSG_SETTINGS_TITLE"), for tests and logs. */
const char *wf_msg_name(WfMsg id);
/* How many {n} arguments the message takes. */
size_t wf_msg_arity(WfMsg id);

/*
 * Writes the message with {0}..{3} replaced by the arguments. With non-NULL
 * out and cap > 0, NUL-terminates and trims an incomplete trailing UTF-8
 * character when the buffer fills; otherwise returns 0 without writing. Control
 * characters in string arguments become '?'. Returns the length written.
 * A placeholder without an argument prints nothing. "{{" is a brace.
 */
size_t wf_msgf(
        char *out, size_t cap, WfMsg id, const WfMsgArg *args, size_t nargs);
size_t wf_msgf_in(WfUiLang lang,
        char *out,
        size_t cap,
        WfMsg id,
        const WfMsgArg *args,
        size_t nargs);

/* The singular or the plural message for a count, by the language's rule
 * (English: 1 is singular; Japanese has no plural). */
WfMsg wf_msg_count(WfMsg one, WfMsg other, long long n);

#endif

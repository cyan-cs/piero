#include <windows.h>

#include "wf_i18n.h"
#include "../base/wf_macro.h"

#include <string.h>
#include <wchar.h>

static bool primary_is(const wchar_t *tag, const char *lang)
{
	size_t n = strlen(lang);

	for (size_t i = 0; i < n; i++) {
		wchar_t c = tag[i];

		if (c >= L'A' && c <= L'Z') {
			c = (wchar_t)(c + (L'a' - L'A'));
		}
		if (c == L'_') {
			c = L'-';
		}
		if (c != (wchar_t)(unsigned char)lang[i]) {
			return false;
		}
	}
	return L'\0' == tag[n] || L'-' == tag[n] || L'_' == tag[n];
}

bool wf_i18n_classify_tag(const wchar_t *tag, WfUiLang *out)
{
	size_t longest = 0;
	if (!tag) {
		return false;
	}
	for (int i = 0; i < WF_UILANG_COUNT; i++) {
		const char *code = wf_i18n_lang_id((WfUiLang)i);
		if (wf_i18n_lang_available(i) && strlen(code) > longest &&
		        primary_is(tag, code)) {
			*out = (WfUiLang)i;
			longest = strlen(code);
		}
	}
	return longest != 0;
}

WfUiLang wf_i18n_classify_list(const wchar_t *tags)
{
	WfUiLang lang;

	for (; tags && *tags; tags += wcslen(tags) + 1) {
		if (wf_i18n_classify_tag(tags, &lang)) {
			return lang;
		}
	}
	return WF_UILANG_EN;
}

WfUiLang wf_i18n_detect(void)
{
	wchar_t buf[256];
	ULONG count = 0, len = (ULONG)(WF_ARRAY_LEN(buf));

	if (!GetUserPreferredUILanguages(
	            MUI_LANGUAGE_NAME, &count, buf, &len) ||
	        0 == count || len < 2) {
		return WF_UILANG_EN;
	}
	buf[(WF_ARRAY_LEN(buf)) - 1] = L'\0';
	return wf_i18n_classify_list(buf);
}

WfUiLang wf_i18n_resolve(WfLangSetting setting, WfUiLang detected)
{
	if ((int)setting > 0 && setting < WF_LANGSET_COUNT) {
		return (WfUiLang)(setting - 1);
	}
	return (int)detected >= 0 && detected < WF_UILANG_COUNT ? detected
	                                                        : WF_UILANG_EN;
}

typedef struct
{
	char *out;
	size_t cap, len;
} Out;

static void put(Out *o, char c)
{
	if (o->len + 1 < o->cap) {
		o->out[o->len++] = c;
	}
}

static void put_text(Out *o, const char *s)
{
	for (; *s && o->len + 1 < o->cap; s++) {
		put(o, (unsigned char)*s < 0x20 || 0x7f == *s ? '?' : *s);
	}
}

static void put_number(Out *o, long long n)
{
	char digits[24];
	size_t k = 0;
	unsigned long long u =
	        n < 0 ? 0ull - (unsigned long long)n : (unsigned long long)n;

	if (n < 0) {
		put(o, '-');
	}
	do {
		digits[k++] = (char)('0' + u % 10);
		u /= 10;
	} while (u);
	while (k) {
		put(o, digits[--k]);
	}
}

/* Drops a multi-byte character the buffer ended in the middle of. */
static void trim_partial(Out *o)
{
	size_t k = o->len;

	while (k && 0x80 == ((unsigned char)o->out[k - 1] & 0xC0)) {
		k--;
	}
	if (k) {
		unsigned char lead = (unsigned char)o->out[k - 1];
		size_t need = lead >= 0xF0 ? 4
		        : lead >= 0xE0     ? 3
		        : lead >= 0xC0     ? 2
		                           : 1;

		if (o->len - (k - 1) < need) {
			o->len = k - 1;
		}
	}
}

size_t wf_msgf_in(WfUiLang lang,
        char *out,
        size_t cap,
        WfMsg id,
        const WfMsgArg *args,
        size_t nargs)
{
	const char *p = wf_msg_in(lang, id);
	Out o = {out, cap, 0};
	bool cut = false;

	if (!out || !cap) {
		return 0;
	}
	for (; *p; p++) {
		if ('{' == p[0] && '{' == p[1]) {
			put(&o, '{');
			p++;
		} else if ('{' == p[0] && p[1] >= '0' &&
		        p[1] < '0' + WF_MSG_MAX_ARGS && '}' == p[2]) {
			size_t i = (size_t)(p[1] - '0');

			if (args && i < nargs) {
				if (args[i].s) {
					put_text(&o, args[i].s);
				} else {
					put_number(&o, args[i].n);
				}
			}
			p += 2;
		} else {
			put(&o, *p);
		}
		if (o.len + 1 >= cap) {
			cut = true;
			break;
		}
	}
	if (cut || o.len + 1 >= cap) {
		trim_partial(&o);
	}
	o.out[o.len] = '\0';
	return o.len;
}

size_t wf_msgf(
        char *out, size_t cap, WfMsg id, const WfMsgArg *args, size_t nargs)
{
	return wf_msgf_in(wf_i18n_language(), out, cap, id, args, nargs);
}

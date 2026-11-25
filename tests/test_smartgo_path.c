/* What Smart Go makes of the text typed after `g`: every notation, the
 * refusals, the completion split, and the editing helpers. Pure: the home
 * folder, the known folders and the environment come from a fake. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "path/wf_smartgo_path.h"
#include "wf_test.h"
#include "base/wf_macro.h"

#define CWD L"C:\\Users\\user\\dev\\filer"

static wchar_t *dup(const wchar_t *s)
{
	return _wcsdup(s);
}

static wchar_t *fake_home(void *ctx)
{
	(void)ctx;
	return dup(L"C:\\Users\\user");
}

static wchar_t *fake_known(void *ctx, const wchar_t *alias)
{
	(void)ctx;
	if (!wcscmp(alias, L"desktop")) {
		return dup(L"C:\\Users\\user\\OneDrive\\Desktop");
	}
	if (!wcscmp(alias, L"downloads")) {
		return dup(L"D:\\Downloads");
	}
	if (!wcscmp(alias, L"documents")) {
		return dup(L"C:\\Users\\user\\Documents");
	}
	if (!wcscmp(alias, L"pictures")) {
		return dup(L"C:\\Users\\user\\Pictures");
	}
	if (!wcscmp(alias, L"music")) {
		return dup(L"C:\\Users\\user\\Music");
	}
	if (!wcscmp(alias, L"videos")) {
		return dup(L"C:\\Users\\user\\Videos");
	}
	if (!wcscmp(alias, L"appdata")) {
		return dup(L"C:\\Users\\user\\AppData\\Roaming");
	}
	return NULL;
}

static wchar_t *fake_env(void *ctx, const wchar_t *name)
{
	(void)ctx;
	if (!_wcsicmp(name, L"TEMP")) {
		return dup(L"C:\\Users\\user\\AppData\\Local\\Temp");
	}
	if (!_wcsicmp(name, L"APPDATA")) {
		return dup(L"C:\\Users\\user\\AppData\\Roaming");
	}
	if (!_wcsicmp(name, L"PROJ")) {
		return dup(L"D:\\Projects");
	}
	if (!_wcsicmp(name, L"EVIL")) {
		return dup(L"C:\\a\nb");
	}
	return NULL;
}

static const WfSmartEnv env = {NULL, fake_home, fake_known, fake_env};

static WfSmartResult go(
        const wchar_t *cwd, const wchar_t *previous, const wchar_t *text)
{
	WfSmartResult r;

	CHECK(WF_OK ==
	        wf_smart_resolve(&env, cwd, previous, text, wcslen(text), &r));
	return r;
}

static bool target_is(const wchar_t *text, const wchar_t *expected)
{
	WfSmartResult r = go(CWD, NULL, text);
	bool ok =
	        WF_SG_OK == r.error && r.target && !wcscmp(r.target, expected);

	if (!ok) {
		printf("  %ls -> error %d target %ls (wanted %ls)\n", text,
		        r.error, r.target ? r.target : L"-", expected);
	}
	wf_smart_result_free(&r);
	return ok;
}

static bool refused(const wchar_t *text, WfSmartError why)
{
	WfSmartResult r = go(CWD, NULL, text);
	bool ok = r.error == why && !r.target;

	if (!ok) {
		printf("  %ls -> error %d (wanted %d)\n", text, r.error, why);
	}
	wf_smart_result_free(&r);
	return ok;
}

static void relative_paths_follow_the_shown_folder(void)
{
	CHECK(target_is(L"src", CWD L"\\src"));
	CHECK(target_is(L"src/core", CWD L"\\src\\core"));
	CHECK(target_is(L"src\\core", CWD L"\\src\\core"));
	CHECK(target_is(L"./src", CWD L"\\src"));
	CHECK(target_is(L".\\src", CWD L"\\src"));
	CHECK(target_is(L".", CWD));
	CHECK(target_is(L"..", L"C:\\Users\\user\\dev"));
	CHECK(target_is(L"../..", L"C:\\Users\\user"));
	CHECK(target_is(L"../../Documents", L"C:\\Users\\user\\Documents"));
	CHECK(target_is(L"../other", L"C:\\Users\\user\\dev\\other"));
	CHECK(target_is(L"src/../lib/./x", CWD L"\\lib\\x"));
	CHECK(target_is(L"src//core", CWD L"\\src\\core"));
	CHECK(target_is(L"src/", CWD L"\\src")); /* no trailing separator */
	/* never above a root */
	CHECK(target_is(L"../../../../../..", L"C:\\"));
	{
		WfSmartResult r = go(L"C:\\", NULL, L"..");

		CHECK(WF_SG_OK == r.error && !wcscmp(r.target, L"C:\\"));
		wf_smart_result_free(&r);
	}
	/* names with spaces and Unicode */
	CHECK(target_is(L"My Documents", CWD L"\\My Documents"));
	CHECK(target_is(L"\x65E5\x672C\x8A9E", CWD L"\\\x65E5\x672C\x8A9E"));
	CHECK(target_is(L"x\xD83D\xDE00y", CWD L"\\x\xD83D\xDE00y"));
}

static void the_home_folder_is_only_tilde_alone_or_before_a_separator(void)
{
	CHECK(target_is(L"~", L"C:\\Users\\user"));
	CHECK(target_is(L"~/", L"C:\\Users\\user"));
	CHECK(target_is(L"~\\", L"C:\\Users\\user"));
	CHECK(target_is(L"~/Downloads", L"C:\\Users\\user\\Downloads"));
	CHECK(target_is(L"~\\Documents", L"C:\\Users\\user\\Documents"));
	CHECK(target_is(L"~/a/../b", L"C:\\Users\\user\\b"));
	/* another user's home is NOT guessed: it is a plain name here */
	CHECK(target_is(L"~otheruser", CWD L"\\~otheruser"));
	CHECK(target_is(L"~x/y", CWD L"\\~x\\y"));
	{
		WfSmartResult r;
		WfSmartEnv none = {NULL, NULL, NULL, NULL};

		CHECK(WF_OK ==
		                wf_smart_resolve(
		                        &none, CWD, NULL, L"~", 1, &r) &&
		        WF_SG_NO_HOME == r.error);
		wf_smart_result_free(&r);
	}
}

static void absolute_paths_and_roots(void)
{
	CHECK(target_is(L"C:\\", L"C:\\"));
	CHECK(target_is(L"C:/", L"C:\\"));
	CHECK(target_is(L"c:\\users", L"c:\\users"));
	CHECK(target_is(L"D:\\Projects", L"D:\\Projects"));
	CHECK(target_is(L"D:\\Projects\\", L"D:\\Projects"));
	CHECK(target_is(L"C:/Users/user", L"C:\\Users\\user"));
	CHECK(target_is(L"C:\\a\\..\\b", L"C:\\b"));
	CHECK(target_is(L"C:\\..\\..", L"C:\\"));
	/* the root of the shown volume */
	CHECK(target_is(L"\\", L"C:\\"));
	CHECK(target_is(L"/", L"C:\\"));
	CHECK(target_is(L"\\Windows", L"C:\\Windows"));
	CHECK(target_is(L"/Windows/System32", L"C:\\Windows\\System32"));
	{
		WfSmartResult r = go(L"\\\\srv\\share\\a\\b", NULL, L"/x");

		CHECK(WF_SG_OK == r.error &&
		        !wcscmp(r.target, L"\\\\srv\\share\\x"));
		wf_smart_result_free(&r);
		r = go(L"\\\\srv\\share\\a\\b", NULL, L"../../../..");
		CHECK(WF_SG_OK == r.error &&
		        !wcscmp(r.target, L"\\\\srv\\share"));
		wf_smart_result_free(&r);
		r = go(L"\\\\?\\D:\\very\\deep", NULL, L"\\");
		CHECK(WF_SG_OK == r.error && !wcscmp(r.target, L"\\\\?\\D:\\"));
		wf_smart_result_free(&r);
	}
}

static void unc_and_extended_paths(void)
{
	CHECK(target_is(L"\\\\server\\share", L"\\\\server\\share"));
	CHECK(target_is(L"\\\\server\\share\\", L"\\\\server\\share"));
	CHECK(target_is(L"\\\\server\\share\\dir\\sub",
	        L"\\\\server\\share\\dir\\sub"));
	CHECK(target_is(L"//server/share/dir", L"\\\\server\\share\\dir"));
	CHECK(target_is(
	        L"\\\\server\\share\\a\\..\\..\\..", L"\\\\server\\share"));
	CHECK(target_is(L"\\\\?\\C:\\Users", L"\\\\?\\C:\\Users"));
	CHECK(target_is(L"\\\\?\\C:\\", L"\\\\?\\C:\\"));
	CHECK(target_is(L"//?/C:/Users", L"\\\\?\\C:\\Users"));
	CHECK(target_is(L"\\\\?\\UNC\\srv\\sh\\x", L"\\\\?\\UNC\\srv\\sh\\x"));
	/* an incomplete share, and device namespaces, are not folders */
	CHECK(refused(L"\\\\server", WF_SG_INVALID));
	CHECK(refused(L"\\\\server\\", WF_SG_INVALID));
	CHECK(refused(L"\\\\", WF_SG_INVALID));
	CHECK(refused(L"\\\\.\\COM1", WF_SG_INVALID));
	CHECK(refused(L"\\\\?\\", WF_SG_INVALID));
	CHECK(refused(L"\\\\?\\UNC\\srv", WF_SG_INVALID));
}

static void environment_variables_expand_or_are_refused(void)
{
	CHECK(target_is(L"%TEMP%", L"C:\\Users\\user\\AppData\\Local\\Temp"));
	CHECK(target_is(
	        L"%temp%\\x", L"C:\\Users\\user\\AppData\\Local\\Temp\\x"));
	CHECK(target_is(L"%APPDATA%", L"C:\\Users\\user\\AppData\\Roaming"));
	CHECK(target_is(L"%PROJ%/piero", L"D:\\Projects\\piero"));
}

static void bad_text_is_named_not_guessed(void)
{
	WfSmartResult r = go(CWD, NULL, L"%NOPE%\\x");

	CHECK(WF_SG_ENV_UNDEFINED == r.error && !wcscmp(r.name, L"NOPE"));
	wf_smart_result_free(&r);
	CHECK(refused(
	        L"%EVIL%", WF_SG_INVALID)); /* a value with a line break */
	CHECK(refused(L"C:", WF_SG_DRIVE_RELATIVE));
	CHECK(refused(L"C:folder", WF_SG_DRIVE_RELATIVE));
	CHECK(refused(L"c:..", WF_SG_DRIVE_RELATIVE));
	CHECK(refused(L"a\x01"
	              L"b",
	        WF_SG_INVALID));
	CHECK(refused(L"a|b", WF_SG_INVALID));
	CHECK(refused(L"a*b", WF_SG_INVALID));
	CHECK(refused(L"a?b", WF_SG_INVALID));
	CHECK(refused(L"a<b", WF_SG_INVALID));
	CHECK(refused(L"a\"b", WF_SG_INVALID));
	CHECK(refused(L"C:\\a:b", WF_SG_INVALID));
	CHECK(refused(L"dir.", WF_SG_INVALID)); /* trailing dot */
	CHECK(refused(L"dir ", WF_SG_INVALID)); /* trailing space */
	CHECK(refused(L"con", WF_SG_INVALID));
	CHECK(refused(L"x\\NUL.txt", WF_SG_INVALID));
	CHECK(refused(L"C:\\COM1", WF_SG_INVALID));
	CHECK(refused(L"a\x200b"
	              L"b",
	        WF_SG_INVALID));                  /* invisible character */
	CHECK(refused(L"\xD800", WF_SG_INVALID)); /* lone surrogate */
	{
		wchar_t *huge = malloc(70000 * sizeof *huge);

		for (int i = 0; i < 69999; i++) {
			huge[i] = i % 100 == 99 ? L'\\' : L'a';
		}
		huge[69999] = 0;
		r = go(CWD, NULL, huge);
		CHECK(WF_SG_OK != r.error && !r.target);
		wf_smart_result_free(&r);
		free(huge);
	}
	CHECK(refused(
	        L"x\\aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	        L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	        L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	        L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	        L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	        L"aaaaaaaaaaaaaaaaaaaaaaaaa",
	        WF_SG_INVALID)); /* a component over 255 */
}

static void aliases_previous_folder_and_quotes(void)
{
	CHECK(target_is(L"@home", L"C:\\Users\\user"));
	CHECK(target_is(L"@HOME", L"C:\\Users\\user"));
	CHECK(target_is(L"@desktop", L"C:\\Users\\user\\OneDrive\\Desktop"));
	CHECK(target_is(L"@downloads", L"D:\\Downloads"));
	CHECK(target_is(L"@documents", L"C:\\Users\\user\\Documents"));
	CHECK(target_is(L"@pictures", L"C:\\Users\\user\\Pictures"));
	CHECK(target_is(L"@music", L"C:\\Users\\user\\Music"));
	CHECK(target_is(L"@videos", L"C:\\Users\\user\\Videos"));
	CHECK(target_is(L"@appdata", L"C:\\Users\\user\\AppData\\Roaming"));
	CHECK(target_is(L"@downloads\\piero", L"D:\\Downloads\\piero"));
	CHECK(target_is(L"@documents/..", L"C:\\Users\\user"));
	CHECK(refused(L"@nonsense\\x", WF_SG_UNKNOWN_ALIAS));
	{
		WfSmartResult r = go(CWD, NULL, L"@do");

		CHECK(WF_SG_ALIAS_PARTIAL == r.error &&
		        !wcscmp(r.prefix, L"do") && 1 == r.prefix_start);
		wf_smart_result_free(&r);
		r = go(CWD, NULL, L"@");
		CHECK(WF_SG_ALIAS_PARTIAL == r.error && !wcscmp(r.prefix, L""));
		wf_smart_result_free(&r);
		r = go(CWD, L"C:\\Prev", L"-");
		CHECK(WF_SG_OK == r.error && !wcscmp(r.target, L"C:\\Prev"));
		wf_smart_result_free(&r);
		r = go(CWD, NULL, L"-");
		CHECK(WF_SG_NO_PREVIOUS == r.error);
		wf_smart_result_free(&r);
		r = go(CWD, NULL, L"");
		CHECK(WF_SG_EMPTY == r.error && !r.target);
		wf_smart_result_free(&r);
	}
	/* Explorer's "copy as path" */
	CHECK(target_is(L"\"C:\\Program Files\\Some Application\"",
	        L"C:\\Program Files\\Some Application"));
	CHECK(refused(L"\"C:\\Program Files", WF_SG_INVALID)); /* unbalanced */
	CHECK(refused(L"\"C:\\a\"b\"", WF_SG_INVALID));
}

/* What Tab works on. */
static void the_last_element_is_split_off_for_completion(void)
{
	WfSmartResult r = go(CWD, NULL, L"~/Do");

	CHECK(WF_SG_OK == r.error && !wcscmp(r.list_dir, L"C:\\Users\\user") &&
	        !wcscmp(r.prefix, L"Do") && 2 == r.prefix_start &&
	        !r.insert_separator);
	wf_smart_result_free(&r);

	r = go(CWD, NULL, L"~");
	CHECK(!wcscmp(r.list_dir, L"C:\\Users\\user") &&
	        !wcscmp(r.prefix, L"") && r.insert_separator &&
	        1 == r.prefix_start);
	wf_smart_result_free(&r);

	r = go(CWD, NULL, L"~/");
	CHECK(!wcscmp(r.list_dir, L"C:\\Users\\user") &&
	        !wcscmp(r.prefix, L"") && !r.insert_separator &&
	        2 == r.prefix_start);
	wf_smart_result_free(&r);

	r = go(CWD, NULL, L"src");
	CHECK(!wcscmp(r.list_dir, CWD) && !wcscmp(r.prefix, L"src") &&
	        0 == r.prefix_start);
	wf_smart_result_free(&r);

	r = go(CWD, NULL, L"src\\co");
	CHECK(!wcscmp(r.list_dir, CWD L"\\src") && !wcscmp(r.prefix, L"co") &&
	        4 == r.prefix_start);
	wf_smart_result_free(&r);

	r = go(CWD, NULL, L"..");
	CHECK(!wcscmp(r.list_dir, L"C:\\Users\\user\\dev") &&
	        !wcscmp(r.prefix, L"") && r.insert_separator);
	wf_smart_result_free(&r);

	r = go(CWD, NULL, L"../..\\Doc");
	CHECK(!wcscmp(r.list_dir, L"C:\\Users\\user") &&
	        !wcscmp(r.prefix, L"Doc"));
	wf_smart_result_free(&r);

	r = go(CWD, NULL, L"C:\\");
	CHECK(!wcscmp(r.list_dir, L"C:\\") && !wcscmp(r.prefix, L"") &&
	        !r.insert_separator);
	wf_smart_result_free(&r);

	r = go(CWD, NULL, L"C:\\Us");
	CHECK(!wcscmp(r.list_dir, L"C:\\") && !wcscmp(r.prefix, L"Us"));
	wf_smart_result_free(&r);

	r = go(CWD, NULL, L"D:\\Projects\\p");
	CHECK(!wcscmp(r.list_dir, L"D:\\Projects") && !wcscmp(r.prefix, L"p"));
	wf_smart_result_free(&r);

	r = go(CWD, NULL, L"\\\\srv\\share");
	CHECK(!wcscmp(r.list_dir, L"\\\\srv\\share") && r.insert_separator);
	wf_smart_result_free(&r);

	r = go(CWD, NULL, L"\\\\srv\\share\\ab");
	CHECK(!wcscmp(r.list_dir, L"\\\\srv\\share") &&
	        !wcscmp(r.prefix, L"ab"));
	wf_smart_result_free(&r);

	r = go(CWD, NULL, L"%TEMP%");
	CHECK(!wcscmp(r.list_dir, L"C:\\Users\\user\\AppData\\Local\\Temp") &&
	        !wcscmp(r.prefix, L"") && r.insert_separator);
	wf_smart_result_free(&r);

	r = go(CWD, NULL, L"@home");
	CHECK(!wcscmp(r.list_dir, L"C:\\Users\\user") && r.insert_separator);
	wf_smart_result_free(&r);

	r = go(CWD, NULL, L"@downloads/pi");
	CHECK(!wcscmp(r.list_dir, L"D:\\Downloads") &&
	        !wcscmp(r.prefix, L"pi"));
	wf_smart_result_free(&r);

	/* inside quotes the offsets shift by the opening quote */
	r = go(CWD, NULL, L"\"C:\\Program Files\\So\"");
	CHECK(!wcscmp(r.list_dir, L"C:\\Program Files") &&
	        !wcscmp(r.prefix, L"So") && r.prefix_start == 1 + 17);
	wf_smart_result_free(&r);
}

static void completion_text_replaces_only_the_element_before_the_caret(void)
{
	WfSmartResult r = go(CWD, NULL, L"~/Do");
	wchar_t *out;
	size_t n, cur;

	CHECK(wf_smart_complete(L"~/Do", 4, 4, &r, L"Documents", true, L'\\',
	              &out, &n, &cur) == WF_OK);
	CHECK(!wcscmp(out, L"~/Documents\\") && 12 == n && 12 == cur);
	free(out);
	CHECK(wf_smart_complete(L"~/Do", 4, 4, &r, L"Do.txt", false, L'/', &out,
	              &n, &cur) == WF_OK);
	CHECK(!wcscmp(out, L"~/Do.txt") && cur == n);
	free(out);
	/* the caret in the middle: the part after it stays */
	CHECK(wf_smart_complete(L"~/Do\\tail", 9, 4, &r, L"Docs", true, L'\\',
	              &out, &n, &cur) == WF_OK);
	CHECK(!wcscmp(out, L"~/Docs\\\\tail") && 7 == cur);
	free(out);
	wf_smart_result_free(&r);

	/* a whole-folder input gets a separator first */
	r = go(CWD, NULL, L"~");
	CHECK(wf_smart_complete(L"~", 1, 1, &r, L"Music", true, L'\\', &out, &n,
	              &cur) == WF_OK);
	CHECK(!wcscmp(out, L"~\\Music\\"));
	free(out);
	wf_smart_result_free(&r);
	r = go(CWD, NULL, L"@home");
	CHECK(wf_smart_complete(L"@home", 5, 5, &r, L"a", false, L'/', &out, &n,
	              &cur) == WF_OK);
	CHECK(!wcscmp(out, L"@home/a"));
	free(out);
	wf_smart_result_free(&r);
	/* a bad caret is refused */
	r = go(CWD, NULL, L"src");
	CHECK(wf_smart_complete(L"src", 3, 9, &r, L"x", false, L'\\', &out, &n,
	              &cur) == WF_E_INVAL);
	wf_smart_result_free(&r);
}

static void ctrl_backspace_deletes_one_path_element(void)
{
	static const struct
	{
		const wchar_t *text;
		size_t cursor, start;
	} cases[] = {{L"~/Projects/piero/src/", 21, 17},
	        {L"~/Projects/piero/", 17, 11}, {L"~/Projects/", 11, 2},
	        {L"~/", 2, 0}, {L"~", 1, 0}, {L"C:\\Users\\user", 13, 9},
	        {L"C:\\Users\\", 9, 3}, {L"C:\\Users", 8, 3}, {L"C:\\", 3, 0},
	        {L"C:\\a", 4, 3}, {L"\\\\srv\\share\\dir", 15, 12},
	        {L"\\\\srv\\share\\", 12, 0}, {L"\\\\srv\\share", 11, 0},
	        {L"a\\b", 3, 2}, {L"abc", 3, 0}, {L"", 0, 0},
	        {L"a\\b\\c", 2, 0}, {L"a\\\\\\b", 5, 4},
	        {L"x\xD83D\xDE00", 3, 0},
	        {L"p\\\xD83D\xDE00\xD83D\xDE01", 6, 2}};

	for (size_t i = 0; i < WF_ARRAY_LEN(cases); i++) {
		size_t got =
		        wf_smart_element_start(cases[i].text, cases[i].cursor);

		if (got != cases[i].start) {
			printf("  [%ls] at %zu -> %zu (wanted %zu)\n",
			        cases[i].text, cases[i].cursor, got,
			        cases[i].start);
		}
		CHECK(got == cases[i].start);
	}
}

static void pasted_text_is_made_safe(void)
{
	wchar_t *out;
	size_t n;

	CHECK(wf_smart_clean_paste(
	              L"\"C:\\Program Files\\App\"", 22, &out, &n) == WF_OK);
	CHECK(!wcscmp(out, L"C:\\Program Files\\App") && 20 == n);
	free(out);
	CHECK(WF_OK == wf_smart_clean_paste(L"  C:\\x \r\n", 9, &out, &n));
	CHECK(!wcscmp(out, L"C:\\x"));
	free(out);
	CHECK(WF_E_INVAL ==
	                wf_smart_clean_paste(L"C:\\a\nC:\\b", 9, &out, &n) &&
	        NULL == out);
	CHECK(wf_smart_clean_paste(L"a\x01"
	                           L"b",
	              3, &out, &n) == WF_E_INVAL);
	CHECK(WF_OK == wf_smart_clean_paste(L"\"unbalanced", 11, &out, &n));
	CHECK(!wcscmp(out, L"\"unbalanced")); /* left for resolve to refuse */
	free(out);
	CHECK(WF_OK == wf_smart_clean_paste(L"", 0, &out, &n) && 0 == n);
	free(out);
	CHECK(WF_OK == wf_smart_clean_paste(NULL, 0, &out, &n));
	CHECK(out && !out[0] && !n);
	free(out);
	/* spaces inside a path are kept */
	CHECK(WF_OK == wf_smart_clean_paste(L"\"a b  c\"", 8, &out, &n));
	CHECK(!wcscmp(out, L"a b  c"));
	free(out);
}

int main(void)
{
	relative_paths_follow_the_shown_folder();
	the_home_folder_is_only_tilde_alone_or_before_a_separator();
	absolute_paths_and_roots();
	unc_and_extended_paths();
	environment_variables_expand_or_are_refused();
	bad_text_is_named_not_guessed();
	aliases_previous_folder_and_quotes();
	the_last_element_is_split_off_for_completion();
	completion_text_replaces_only_the_element_before_the_caret();
	ctrl_backspace_deletes_one_path_element();
	pasted_text_is_made_safe();
	return TEST_RESULT();
}

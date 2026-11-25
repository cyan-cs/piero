/* Editing a few keys of an INI text without disturbing the rest of it. */
#include <stdlib.h>
#include <string.h>

#include "config/wf_ini_edit.h"
#include "config/wf_config.h"
#include "wf_test.h"

static char *edit(const char *text, const WfIniSet *sets, size_t count)
{
	char *out = NULL;
	size_t n = 0;

	if (WF_OK != wf_ini_apply(text, strlen(text), sets, count, &out, &n)) {
		return NULL;
	}
	CHECK(n == strlen(out)); /* NUL-terminated, length exact */
	return out;
}

static void a_present_key_is_replaced_in_place(void)
{
	static const WfIniSet set[] = {{"appearance", "theme", "heart"}};
	char *out = edit("; mine\n[appearance]\ntheme = spade\ncolor = x\n"
	                 "[files]\nsort_by = date\n",
	        set, 1);

	CHECK(out &&
	        !strcmp(out,
	                "; mine\n[appearance]\ntheme = heart\ncolor = x\n"
	                "[files]\nsort_by = date\n"));
	free(out);
}

static void a_missing_key_joins_its_section(void)
{
	static const WfIniSet set[] = {{"files", "sort_descending", "true"}};
	char *out = edit(
	        "[files]\nsort_by = date\n\n# note\n[ui]\nx = 1\n", set, 1);

	CHECK(out &&
	        !strcmp(out,
	                "[files]\nsort_by = date\nsort_descending = true\n"
	                "\n# note\n[ui]\nx = 1\n"));
	free(out);
}

static void a_missing_section_goes_at_the_end(void)
{
	static const WfIniSet set[] = {{"meta", "schema_version", "1"},
	        {"meta", "onboarding_completed", "true"}};
	char *out = edit("[ui]\nx = 1\n", set, 2);

	CHECK(out &&
	        !strcmp(out,
	                "[ui]\nx = 1\n\n[meta]\nschema_version = 1\n"
	                "onboarding_completed = true\n"));
	free(out);
	/* an empty text gets only the new section */
	out = edit("", set, 2);
	CHECK(out &&
	        !strcmp(out,
	                "[meta]\r\nschema_version = 1\r\n"
	                "onboarding_completed = true\r\n"));
	free(out);
}

static void the_line_ending_style_is_kept(void)
{
	static const WfIniSet set[] = {{"ui", "show_hidden", "true"}};
	char *out = edit("[ui]\r\nx = 1\r\n", set, 1);

	CHECK(out && !strcmp(out, "[ui]\r\nx = 1\r\nshow_hidden = true\r\n"));
	free(out);
	out = edit("[ui]\nx = 1", set, 1); /* LF, and no final newline */
	CHECK(out && !strcmp(out, "[ui]\nx = 1\nshow_hidden = true\n"));
	free(out);
}

static void duplicate_keys_are_canonicalized(void)
{
	static const WfIniSet set[] = {{"ui", "k", "9"}};
	char *out = edit("[ui]\nk = 1\nk = 2\n", set, 1);

	CHECK(out && !strcmp(out, "[ui]\nk = 9\n"));
	free(out);
}

static void duplicate_keyboard_overrides_roundtrip(void)
{
	const char *text = "[keymap.mgr]\nfm.nav.up = Up\n"
	                   "; keep\n[files]\nsort_by = date\n"
	                   "[keyboard.custom.fm]\nfm.nav.up = k\n"
	                   "[keyboard.custom.mgr]\nfm.nav.up = Left\n"
	                   "[keymap.fm]\nfm.nav.up = Right\n"
	                   "[keyboard.custom.fm]\nfm.nav.up = Up\n"
	                   "[other]\nfm.nav.up = keep\n";
	static const WfIniSet set[] = {
	        {"keyboard.custom.fm", "fm.nav.up", "Down"}};
	static const WfIniSet gone[] = {
	        {"keyboard.custom.fm", "fm.nav.up", NULL}};
	WfConfig c;
	char *out = NULL;
	size_t out_n;
	char keys[96];
	const WfKeyOverride *o;
	const WfCommandInfo *ci = wf_cmd_by_name("fm.nav.up");

	wf_config_init(&c);
	CHECK(WF_OK ==
	        wf_config_apply(text, strlen(text), set, 1, &out, &out_n));
	CHECK(out && ci);
	if (out && ci) {
		CHECK(WF_OK ==
		        wf_config_parse(
		                &c, out, strlen(out), NULL, NULL, NULL));
		o = wf_keyconfig_find(&c.keys, WF_CTX_FM, ci->id);
		CHECK(o && 1 == c.keys.count);
		if (o) {
			wf_keys_format_list(o, keys, sizeof keys);
			CHECK(!strcmp(keys, "Down"));
		}
		CHECK(strstr(out, "; keep\n[files]\nsort_by = date\n"));
		CHECK(strstr(out, "[other]\nfm.nav.up = keep\n"));
	}
	free(out);
	out = NULL;
	CHECK(WF_OK ==
	        wf_config_apply(text, strlen(text), gone, 1, &out, &out_n));
	CHECK(out && !strstr(out, "fm.nav.up = Up"));
	CHECK(out && strstr(out, "[other]\nfm.nav.up = keep\n"));
	if (out) {
		CHECK(WF_OK ==
		        wf_config_parse(
		                &c, out, strlen(out), NULL, NULL, NULL));
		CHECK(0 == c.keys.count);
	}
	free(out);
	wf_config_dispose(&c);
}

static void keymap_section_alias_depends_on_the_key(void)
{
	const char *text = "[keymap]\nprofile = nano\nbase = nano\n"
	                   "; keep\n[files]\nsort_by = date\n"
	                   "[keyboard]\nprofile = vim\n"
	                   "[keyboard.custom]\nbase = nano\n";
	static const WfIniSet sets[] = {{"keyboard", "profile", "standard"},
	        {"keyboard.custom", "base", "vim"}};
	WfConfig c;
	char *out = NULL;
	size_t n;

	wf_config_init(&c);
	CHECK(WF_OK == wf_config_apply(text, strlen(text), sets, 2, &out, &n));
	if (out) {
		CHECK(WF_OK == wf_config_parse(&c, out, n, NULL, NULL, NULL));
		CHECK(WF_PROFILE_LEGACY == c.keys.profile &&
		        WF_BASE_VIM == c.keys.base);
		CHECK(strstr(out, "; keep\n[files]\nsort_by = date\n"));
	}
	free(out);
	wf_config_dispose(&c);
}

static void newer_config_text_cannot_be_edited(void)
{
	const char *future = "[meta]\nschema_version = 1001\n"
	                     "[future]\nunknown = keep\n";
	const char *invalid = "[meta]\nschema_version = 2147483648\n";
	static const WfIniSet sets[] = {{"meta", "schema_version", "1"}};
	char *out = (char *)1;
	size_t n = 7;

	CHECK(WF_E_ACCESS ==
	        wf_config_apply(future, strlen(future), sets, 1, &out, &n));
	CHECK(NULL == out && 0 == n);
	CHECK(WF_E_INVAL ==
	        wf_config_apply(invalid, strlen(invalid), sets, 1, &out, &n));
	CHECK(NULL == out && 0 == n);
}

static void comments_unknown_keys_and_other_sections_stay(void)
{
	static const WfIniSet set[] = {{"b", "k", "new"}};
	const char *text = "# top\n; second\n[a]\nk = keep\nweird line\n"
	                   "unknown_key = 1\n\n[b]\n; inner\nk = old\n";
	char *out = edit(text, set, 1);

	CHECK(out &&
	        !strcmp(out,
	                "# top\n; second\n[a]\nk = keep\nweird line\n"
	                "unknown_key = 1\n\n[b]\n; inner\nk = new\n"));
	free(out);
	/* the same key name in another section is another key */
	{
		static const WfIniSet other[] = {{"c", "k", "1"}};

		out = edit("[a]\nk = keep\n", other, 1);
		CHECK(out && !strcmp(out, "[a]\nk = keep\n\n[c]\nk = 1\n"));
		free(out);
	}
	/* a commented-out key is not the key */
	{
		static const WfIniSet one[] = {{"a", "k", "2"}};

		out = edit("[a]\n; k = 1\n", one, 1);
		CHECK(out && !strcmp(out, "[a]\nk = 2\n; k = 1\n"));
		free(out);
	}
}

static void unsafe_input_is_refused_and_nothing_is_made(void)
{
	char *out = (char *)1;
	size_t n = 7;
	static const WfIniSet bad_value[] = {{"a", "k", "x\ny"}};
	static const WfIniSet bad_ctrl[] = {{"a", "k", "x\x01"}};
	static const WfIniSet bad_unit_sep[] = {{"a", "k", "x\x1f"}};
	static const WfIniSet bad_delete[] = {{"a", "k", "x\x7f"}};
	static const WfIniSet bad_key[] = {{"a", "k=v", "1"}};
	static const WfIniSet bad_section[] = {{"a]b", "k", "1"}};
	static const WfIniSet empty_key[] = {{"a", "", "1"}};
	static const WfIniSet empty_value[] = {{"a", "k", ""}};

	CHECK(WF_E_INVAL == wf_ini_apply("", 0, bad_value, 1, &out, &n) &&
	        NULL == out && 0 == n);
	out = (char *)1;
	CHECK(WF_E_INVAL == wf_ini_apply("", 0, bad_ctrl, 1, &out, &n));
	CHECK(WF_E_INVAL == wf_ini_apply("", 0, bad_unit_sep, 1, &out, &n));
	CHECK(WF_E_INVAL == wf_ini_apply("", 0, bad_delete, 1, &out, &n));
	CHECK(WF_E_INVAL == wf_ini_apply("", 0, bad_key, 1, &out, &n));
	CHECK(WF_E_INVAL == wf_ini_apply("", 0, bad_section, 1, &out, &n));
	CHECK(WF_E_INVAL == wf_ini_apply("", 0, empty_key, 1, &out, &n));
	CHECK(WF_E_INVAL == wf_ini_apply("", 0, empty_value, 1, &out, &n));
	CHECK(WF_E_INVAL == wf_ini_apply(NULL, 3, bad_value, 1, &out, &n));
	CHECK(WF_E_INVAL == wf_ini_apply("", 0, NULL, 1, &out, &n));
	CHECK(WF_E_INVAL == wf_ini_apply("", 0, NULL, 1, NULL, &n));
}

static void no_changes_give_the_same_text(void)
{
	const char *text = "[a]\nk = 1\n";
	char *out = edit(text, NULL, 0);

	CHECK(out && !strcmp(out, text));
	free(out);
}

static void a_key_is_removed_when_its_value_is_null(void)
{
	static const WfIniSet gone[] = {{"a", "k", NULL}};
	static const WfIniSet absent[] = {
	        {"a", "nope", NULL}, {"zzz", "k", NULL}};
	static const WfIniSet twice[] = {{"a", "k", NULL}};
	char *out = edit("; head\n[a]\nk = 1\nm = 2\n[b]\nk = 3\n", gone, 1);

	/* only that key of that section; its neighbours and the other section
	 * stay, and so do the comments */
	CHECK(out && !strcmp(out, "; head\n[a]\nm = 2\n[b]\nk = 3\n"));
	free(out);
	/* a key that is not there is not an error, and nothing is added */
	out = edit("[a]\nk = 1\n", absent, 2);
	CHECK(out && !strcmp(out, "[a]\nk = 1\n"));
	free(out);
	/* Removal cannot reveal an earlier duplicate on restart. */
	out = edit("[a]\nk = 1\nk = 2\n", twice, 1);
	CHECK(out && !strcmp(out, "[a]\n"));
	free(out);
	/* the line-ending style survives a removal */
	out = edit("[a]\r\nk = 1\r\nm = 2\r\n", gone, 1);
	CHECK(out && !strcmp(out, "[a]\r\nm = 2\r\n"));
	free(out);
	/* removing and setting in one call */
	{
		static const WfIniSet both[] = {
		        {"a", "k", NULL}, {"a", "m", "9"}};

		out = edit("[a]\nk = 1\nm = 2\n", both, 2);
		CHECK(out && !strcmp(out, "[a]\nm = 9\n"));
		free(out);
	}
}

/* Very many duplicates of a key, over repeated sections and among other lines:
 * the result is what removing them one at a time would give (the last one stays
 * and takes the value, everything else keeps its place), and it is made in one pass. */
static void very_many_duplicates_are_dropped_in_order(void)
{
	enum
	{
		ROUNDS = 3000
	};
	size_t cap = (size_t)ROUNDS * 64u + 256u, at = 0;
	char *in = malloc(cap), *want = malloc(cap), *out;
	size_t w = 0;
	static const WfIniSet set[] = {{"files", "sort_by", "date"}};
	static const WfIniSet del[] = {{"files", "sort_by", NULL}};

	CHECK(in && want);
	if (!in || !want) {
		free(in);
		free(want);
		return;
	}
	for (int i = 0; i < ROUNDS; i++) {
		at += (size_t)snprintf(in + at, cap - at,
		        "[files]\nsort_by = n%d\nother%d = x\n[ui]\nsort_by = "
		        "ui%d\n",
		        i, i, i);
		w += (size_t)snprintf(want + w, cap - w,
		        "[files]\nother%d = x\n[ui]\nsort_by = ui%d\n", i, i);
	}
	/* the last one of [files] stays in place, with the new value */
	at += (size_t)snprintf(
	        in + at, cap - at, "[files]\nsort_by = last\n# tail\n");
	w += (size_t)snprintf(
	        want + w, cap - w, "[files]\nsort_by = date\n# tail\n");
	out = edit(in, set, 1);
	CHECK(out && !strcmp(out, want));
	free(out);
	/* deleting the key takes the last one too */
	w = 0;
	for (int i = 0; i < ROUNDS; i++) {
		w += (size_t)snprintf(want + w, cap - w,
		        "[files]\nother%d = x\n[ui]\nsort_by = ui%d\n", i, i);
	}
	w += (size_t)snprintf(want + w, cap - w, "[files]\n# tail\n");
	out = edit(in, del, 1);
	CHECK(out && !strcmp(out, want));
	free(out);
	free(in);
	free(want);
}

int main(void)
{
	a_present_key_is_replaced_in_place();
	a_missing_key_joins_its_section();
	a_missing_section_goes_at_the_end();
	the_line_ending_style_is_kept();
	duplicate_keys_are_canonicalized();
	duplicate_keyboard_overrides_roundtrip();
	keymap_section_alias_depends_on_the_key();
	newer_config_text_cannot_be_edited();
	comments_unknown_keys_and_other_sections_stay();
	unsafe_input_is_refused_and_nothing_is_made();
	no_changes_give_the_same_text();
	a_key_is_removed_when_its_value_is_null();
	very_many_duplicates_are_dropped_in_order();
	return TEST_RESULT();
}

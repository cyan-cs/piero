/*
 * Saving settings to the real configuration file: created when missing,
 * edited in place otherwise (comments and unknown keys stay), refused when the
 * file changed behind our back, and never half-written.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "config/wf_config.h"
#include "config/wf_config_save_win.h"
#include "render/wf_theme.h"
#include "wf_test.h"
#include "base/wf_macro.h"
#include "xfer/wf_copyjournal_win.h"

static P0Lab lab;
static wchar_t root[1024];

static bool write_raw(const wchar_t *path, const char *data, size_t n)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD w = 0;
	bool ok;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = WriteFile(h, data, (DWORD)n, &w, NULL) && w == n;
	CloseHandle(h);
	return ok;
}

static char *read_all(const wchar_t *path, size_t *n)
{
	HANDLE h = CreateFileW(path, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	char *buf = calloc(1, 65536);
	DWORD got = 0;

	*n = 0;
	if (INVALID_HANDLE_VALUE == h) {
		free(buf);
		return NULL;
	}
	ReadFile(h, buf, 65535, &got, NULL);
	CloseHandle(h);
	*n = got;
	return buf;
}

static bool file_is(const wchar_t *path, const char *expected)
{
	size_t n;
	char *got = read_all(path, &n);
	bool same = got && n == strlen(expected) && !memcmp(got, expected, n);

	free(got);
	return same;
}

static void set_path(wchar_t *out, const wchar_t *leaf)
{
	swprintf(out, 1100, L"%ls\\%ls", root, leaf);
}

static void a_missing_file_is_created_with_its_folder(void)
{
	wchar_t p[1100];
	WfConfigFile f;
	WfConfigSaveResult r;
	WfConfig c;
	static const WfIniSet sets[] = {{"meta", "schema_version", "1"},
	        {"meta", "onboarding_completed", "true"},
	        {"appearance", "theme", "heart"}};
	size_t n;
	char *got;

	wf_config_init(&c);
	set_path(p, L"fresh\\config.ini"); /* the folder does not exist yet */
	CHECK(WF_OK == wf_config_file_open(&f, p) && !f.stamp.valid);
	CHECK(WF_OK == wf_config_file_save(&f, sets, 3, &r) &&
	        WF_CS_OK == r.verdict);
	got = read_all(p, &n);
	CHECK(got && strstr(got, "[meta]\r\nschema_version = 1\r\n"));
	CHECK(got && n >= 3 &&
	        0 != memcmp(got, "\xEF\xBB\xBF", 3)); /* no BOM */
	CHECK(f.stamp.valid);
	/* what was written is what the reader understands */
	CHECK(WF_OK == wf_config_parse(&c, got, n, NULL, NULL, NULL) &&
	        WF_THEME_HEART == c.theme && 1 == c.onboarding_completed &&
	        1 == c.schema_version);
	free(got);
	/* a second save edits the file just made */
	{
		static const WfIniSet again[] = {
		        {"appearance", "theme", "ace"}};

		CHECK(WF_OK == wf_config_file_save(&f, again, 1, &r) &&
		        WF_CS_OK == r.verdict);
		got = read_all(p, &n);
		CHECK(got && strstr(got, "theme = ace") &&
		        !strstr(got, "heart"));
		free(got);
		/* Replacing settings also preserves the original. Another save
		 * never overwrites that recovery copy: it keeps its own original
		 * under the next free name and still succeeds. */
		CHECK(WF_OK == wf_config_file_save(&f, again, 1, &r) &&
		        WF_CS_OK == r.verdict);
	}
	wf_config_file_close(&f);
	wf_config_dispose(&c);
}

static void update_opt_out_survives_reopening(void)
{
	P0Lab updates_lab;
	wchar_t p[1100], journal[1100], backup[1100];
	static const char original[] =
	        "[preview]\ntext_fallback_encoding=utf8\nimages=true\n"
	        "image_style=halfblock\n[ui]\nparent_ratio=25\n"
	        "current_ratio=45\npreview_ratio=30\n";
	WfConfig before, after, loaded;
	WfConfigChange changes[8];
	WfIniSet sets[8];
	WfConfigFile f;
	WfConfigSaveResult result;
	size_t count, bytes;
	char *text;

	/* A mapped owned root reproduces a real drive-root configuration path
	 * without ever writing to the system drive's root. */
	if (!p0_lab_open(&updates_lab, "updates-root", 1) ||
	        !updates_lab.area) {
		CHECK(false);
		return;
	}
	CHECK(p0_lab_path(&updates_lab, L"updates.ini", p, WF_ARRAY_LEN(p)) &&
	        p0_lab_path(&updates_lab, L"updates.ini~", backup,
	                WF_ARRAY_LEN(backup)) &&
	        p0_lab_path(&updates_lab, L"updates-journal.txt", journal,
	                WF_ARRAY_LEN(journal)));
	CHECK(write_raw(p, original, sizeof original - 1));
	WfCopyJournal *writer = wf_cj_open(journal);
	CHECK(writer != NULL);
	wf_efile_set_journal(journal);
	wf_config_init(&before);
	wf_config_init(&loaded);
	CHECK(WF_OK ==
	        wf_config_parse(&before, original, sizeof original - 1, NULL,
	                NULL, NULL));
	after = before;
	after.update_consent = 1;
	after.update_checks = 0;
	count = wf_config_diff(&before, &after, changes, WF_ARRAY_LEN(changes));
	CHECK(count > 0 && count <= WF_ARRAY_LEN(sets));
	for (size_t i = 0; i < count && i < WF_ARRAY_LEN(sets); i++) {
		sets[i] = (WfIniSet){changes[i].section, changes[i].key,
		        changes[i].remove ? NULL : changes[i].value};
	}
	CHECK(WF_OK == wf_config_file_open(&f, p) && f.stamp.valid);
	CHECK(WF_OK == wf_config_file_save(&f, sets, count, &result) &&
	        result.verdict == WF_CS_OK);
	text = read_all(p, &bytes);
	CHECK(text &&
	        WF_OK ==
	                wf_config_parse(
	                        &loaded, text, bytes, NULL, NULL, NULL) &&
	        loaded.update_consent && !loaded.update_checks);
	CHECK(file_is(backup, original));
	free(text);
	wf_config_file_close(&f);
	wf_config_dispose(&before);
	wf_config_dispose(&loaded);
	wf_cj_close(writer);
	wf_efile_set_journal(NULL);
	DeleteFileW(p);
	DeleteFileW(backup);
	DeleteFileW(journal);
	CHECK(0 == p0_lab_close(&updates_lab, 0 != wf_test_failures));
}

static void an_existing_file_keeps_everything_else(void)
{
	wchar_t p[1100];
	WfConfigFile f;
	WfConfigSaveResult r;
	static const char original[] =
	        "; my settings\r\n[ui]\r\nshow_hidden = true\r\n"
	        "[appearance]\r\ntheme = spade\r\n[x]\r\nunknown = 1\r\n";
	static const WfIniSet sets[] = {
	        {"appearance", "theme", "club"}, {"files", "sort_by", "size"}};

	set_path(p, L"keep.ini");
	CHECK(write_raw(p, original, strlen(original)));
	CHECK(WF_OK == wf_config_file_open(&f, p) && f.stamp.valid);
	CHECK(WF_OK == wf_config_file_save(&f, sets, 2, &r) &&
	        WF_CS_OK == r.verdict);
	CHECK(file_is(p,
	        "; my settings\r\n[ui]\r\nshow_hidden = true\r\n"
	        "[appearance]\r\ntheme = club\r\n[x]\r\nunknown = 1\r\n\r\n"
	        "[files]\r\nsort_by = size\r\n"));
	wf_config_file_close(&f);

	/* a file with a byte order mark keeps it */
	set_path(p, L"bom.ini");
	CHECK(write_raw(p, "\xEF\xBB\xBF[ui]\nshow_hidden = false\n", 28));
	CHECK(WF_OK == wf_config_file_open(&f, p) && f.stamp.valid);
	CHECK(WF_OK == wf_config_file_save(&f, sets, 1, &r) &&
	        WF_CS_OK == r.verdict);
	CHECK(file_is(p,
	        "\xEF\xBB\xBF[ui]\nshow_hidden = false\n\n"
	        "[appearance]\ntheme = club\n"));
	wf_config_file_close(&f);
}

static void a_file_changed_behind_our_back_is_not_overwritten(void)
{
	wchar_t p[1100];
	WfConfigFile f;
	WfConfigSaveResult r;
	static const WfIniSet sets[] = {{"appearance", "theme", "king"}};
	static const char theirs[] = "[appearance]\ntheme = queen\n# theirs\n";

	set_path(p, L"race.ini");
	CHECK(write_raw(p, "[appearance]\ntheme = spade\n", 27));
	CHECK(WF_OK == wf_config_file_open(&f, p) && f.stamp.valid);
	CHECK(write_raw(p, theirs, strlen(theirs))); /* someone edits it */
	CHECK(WF_OK == wf_config_file_save(&f, sets, 1, &r) &&
	        WF_CS_CONFLICT == r.verdict && r.reason);
	CHECK(file_is(p, theirs)); /* untouched */

	/* deleted behind our back */
	CHECK(DeleteFileW(p));
	CHECK(WF_OK == wf_config_file_save(&f, sets, 1, &r) &&
	        WF_CS_CONFLICT == r.verdict);
	CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(p));
	wf_config_file_close(&f);

	/* it was missing when read, and somebody created it since */
	CHECK(WF_OK == wf_config_file_open(&f, p) && !f.stamp.valid);
	CHECK(write_raw(p, theirs, strlen(theirs)));
	CHECK(WF_OK == wf_config_file_save(&f, sets, 1, &r) &&
	        WF_CS_CONFLICT == r.verdict);
	CHECK(file_is(p, theirs));
	wf_config_file_close(&f);
}

static void a_file_that_cannot_be_replaced_is_left_alone(void)
{
	wchar_t p[1100];
	WfConfigFile f;
	WfConfigSaveResult r;
	static const WfIniSet sets[] = {{"appearance", "theme", "joker"}};
	static const char text[] = "[appearance]\ntheme = spade\n";

	set_path(p, L"readonly.ini");
	CHECK(write_raw(p, text, strlen(text)));
	CHECK(WF_OK == wf_config_file_open(&f, p) && f.stamp.valid);
	CHECK(SetFileAttributesW(p, FILE_ATTRIBUTE_READONLY));
	CHECK(WF_OK == wf_config_file_save(&f, sets, 1, &r) &&
	        WF_CS_REFUSED == r.verdict && r.reason);
	CHECK(file_is(p, text));
	SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
	wf_config_file_close(&f);
}

static void bad_requests_are_refused_before_anything_is_written(void)
{
	wchar_t p[1100];
	WfConfigFile f;
	WfConfigSaveResult r;
	static const WfIniSet bad[] = {{"appearance", "theme", "a\nb"}};
	static const char text[] = "[appearance]\ntheme = spade\n";

	set_path(p, L"bad.ini");
	CHECK(write_raw(p, text, strlen(text)));
	CHECK(WF_OK == wf_config_file_open(&f, p));
	CHECK(WF_E_INVAL == wf_config_file_save(&f, bad, 1, &r));
	CHECK(file_is(p, text));
	CHECK(WF_E_INVAL == wf_config_file_save(NULL, bad, 1, &r));
	CHECK(WF_E_INVAL == wf_config_file_open(&f, L""));
	wf_config_file_close(&f);
}

static void an_unverified_commit_requires_reopening(void)
{
	WfConfigFile f = {0};
	WfConfigSaveResult r;
	f.path = _wcsdup(L"Z:\\missing-review-config.ini");
	CHECK(f.path != NULL);
	if (!f.path) {
		return;
	}
	f.unverified_error = ERROR_ACCESS_DENIED;
	CHECK(WF_OK == wf_config_file_save(&f, NULL, 0, &r));
	CHECK(r.verdict == WF_CS_COMMITTED_UNVERIFIED &&
	        r.error == ERROR_ACCESS_DENIED);
	wf_config_file_close(&f);
}

int main(void)
{
	an_unverified_commit_requires_reopening();
	size_t i;
	static const wchar_t *const leftovers[] = {L"fresh\\config.ini",
	        L"fresh\\config.ini~", L"fresh\\config.ini~2", L"keep.ini~", L"bom.ini~", L"keep.ini",
	        L"bom.ini", L"race.ini", L"readonly.ini", L"bad.ini"};

	if (!p0_lab_open(&lab, "configsave", 0) || !lab.area) {
		return TEST_RESULT();
	}
	wcscpy_s(root, 1024, lab.root);
	while (wcslen(root) > 3 && L'\\' == root[wcslen(root) - 1]) {
		root[wcslen(root) - 1] = 0;
	}
	a_missing_file_is_created_with_its_folder();
	an_existing_file_keeps_everything_else();
	a_file_changed_behind_our_back_is_not_overwritten();
	a_file_that_cannot_be_replaced_is_left_alone();
	bad_requests_are_refused_before_anything_is_written();
	update_opt_out_survives_reopening();
	for (i = 0; i < WF_ARRAY_LEN(leftovers); i++) {
		wchar_t p[1100];

		set_path(p, leftovers[i]);
		SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
		DeleteFileW(p);
	}
	{
		wchar_t p[1100];

		set_path(p, L"fresh");
		RemoveDirectoryW(p);
	}
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}

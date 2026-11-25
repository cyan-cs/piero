/* Entry details: formatting is pure, so every case here is deterministic. */
#include <string.h>

#include "ui/wf_app_priv.h"
#include "ui/wf_info.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

/* FILETIME for `days` whole days after 1601-01-01 plus `seconds`. The day
 * counts below were computed independently (Python datetime). */
static WfFileTime filetime(int days_since_1601, int seconds)
{
	return ((WfFileTime)days_since_1601 * 86400u + (WfFileTime)seconds) *
	        10000000u;
}

static void sizes(void)
{
	char s[64];

	wf_info_format_size(0, s, sizeof s);
	CHECK(!strcmp(s, "0 bytes"));
	wf_info_format_size(1023, s, sizeof s);
	CHECK(!strcmp(s, "1,023 bytes"));
	wf_info_format_size(1024, s, sizeof s);
	CHECK(!strcmp(s, "1,024 bytes (1.00 KiB)"));
	wf_info_format_size(1536, s, sizeof s);
	CHECK(!strcmp(s, "1,536 bytes (1.50 KiB)"));
	wf_info_format_size(12345, s, sizeof s);
	CHECK(!strcmp(s, "12,345 bytes (12.05 KiB)"));
	wf_info_format_size(5ull << 30, s, sizeof s);
	CHECK(!strcmp(s, "5,368,709,120 bytes (5.00 GiB)"));
	wf_info_format_size(UINT64_MAX, s, sizeof s);
	CHECK(strstr(s, "18,446,744,073,709,551,615 bytes (15.99 EiB)"));
	/* a tiny buffer is cut, never overrun */
	wf_info_format_size(UINT64_MAX, s, 8);
	CHECK(7 == strlen(s));
}

static void times(void)
{
	char s[64];

	wf_info_format_time(0, s, sizeof s);
	CHECK(!strcmp(s, "unknown"));
	/* 1601-01-01 00:00:01 */
	wf_info_format_time(10000000u, s, sizeof s);
	CHECK(!strcmp(s, "1601-01-01 00:00:01"));
	/* Unix epoch: 134774 days after 1601-01-01 */
	wf_info_format_time(filetime(134774, 0), s, sizeof s);
	CHECK(!strcmp(s, "1970-01-01 00:00:00"));
	/* 2000 is a leap year (divisible by 400) */
	wf_info_format_time(filetime(145790, 86399), s, sizeof s);
	CHECK(!strcmp(s, "2000-02-29 23:59:59"));
	/* 1900 is not (divisible by 100 but not 400): Feb 28 is followed by Mar 1 */
	wf_info_format_time(filetime(109265, 0), s, sizeof s);
	CHECK(!strcmp(s, "1900-02-28 00:00:00"));
	wf_info_format_time(filetime(109266, 0), s, sizeof s);
	CHECK(!strcmp(s, "1900-03-01 00:00:00"));
	wf_info_format_time(filetime(155522, 45296), s, sizeof s);
	CHECK(!strcmp(s, "2026-10-22 12:34:56"));
	wf_info_format_time(filetime(3067670, 86399), s, sizeof s);
	CHECK(!strcmp(s, "9999-12-31 23:59:59"));
	wf_info_format_time(filetime(3067670 + 1, 0), s, sizeof s);
	CHECK(!strcmp(s, "out of range"));
	wf_info_format_time(UINT64_MAX, s, sizeof s);
	CHECK(!strcmp(s, "out of range"));
}

static void attributes(void)
{
	char s[96];

	wf_info_format_attributes(0, s, sizeof s);
	CHECK(!strcmp(s, "none"));
	wf_info_format_attributes(0x1 | 0x2 | 0x20, s, sizeof s);
	CHECK(!strcmp(s, "ReadOnly Hidden Archive"));
	wf_info_format_attributes(
	        0x10, s, sizeof s); /* directory: not an attribute word */
	CHECK(!strcmp(s, "none"));
	wf_info_format_attributes(
	        0xFFFFFFFFu, s, 20); /* truncated, terminated */
	CHECK(strlen(s) < 20);
	wf_info_format_reparse(0xA000000Cu, s, sizeof s);
	CHECK(!strcmp(s, "symbolic link"));
	wf_info_format_reparse(0xA0000003u, s, sizeof s);
	CHECK(!strcmp(s, "junction"));
	wf_info_format_reparse(0x9000701Au, s, sizeof s);
	CHECK(!strcmp(s, "cloud placeholder"));
	wf_info_format_reparse(0x12345678u, s, sizeof s);
	CHECK(!strcmp(s, "reparse point 0x12345678"));
}

static bool has_line(const WfInfo *info, const char *needle)
{
	for (int i = 0; i < info->n; i++) {
		if (strstr(info->text[i], needle)) {
			return true;
		}
	}
	return false;
}

static WfFileTime plus_nine_hours(WfFileTime utc)
{
	return utc + 9ull * 3600 * 10000000u;
}

static WfFileTime failing_local(WfFileTime utc)
{
	(void)utc;
	return 0;
}

static void entry_details(void)
{
	WfDir *d = wf_dir_new();
	WfInfo info;
	WfEntry *e;

	CHECK(NULL != d);
	CHECK(WF_OK ==
	        wf_dir_add(d, L"a.txt", 5, 0, 2048, filetime(134774, 3600)));
	CHECK(WF_OK == wf_dir_add(d, L"Docs", 4, WF_ENT_DIR, 0, 0));
	CHECK(WF_OK ==
	        wf_dir_add_ex(d, L"lnk", 3, WF_ENT_LINK, 7, 0, 0xA000000Cu));
	CHECK(WF_OK == wf_dir_add(d, L"cloud.bin", 9, WF_ENT_CLOUD, 5, 0));

	/* A listing without OS metadata must say "unknown", never invent. */
	e = &d->e[0];
	wf_info_build(&info, e, d, NULL, NULL);
	CHECK(has_line(&info, "Name:      a.txt"));
	CHECK(has_line(&info, "file (.txt)"));
	CHECK(has_line(&info, "2,048 bytes (2.00 KiB)"));
	CHECK(has_line(&info, "Created:   unknown"));
	CHECK(has_line(&info, "Attributes: unknown"));
	CHECK(has_line(&info, "Read-only:  unknown"));
	CHECK(has_line(&info, "Modified:  1970-01-01 01:00:00 UTC"));
	CHECK(has_line(&info, "Selected:  none"));

	wf_dir_set_meta(d, 0, filetime(134774, 0), 0x1 | 0x20);
	wf_info_build(&info, &d->e[0], d, NULL, NULL);
	CHECK(has_line(&info, "Created:   1970-01-01 00:00:00 UTC"));
	CHECK(has_line(&info, "Attributes: ReadOnly Archive"));
	CHECK(has_line(&info, "Read-only:  yes"));

	/* local time: converted values carry no UTC suffix */
	wf_info_build(&info, &d->e[0], d, NULL, plus_nine_hours);
	CHECK(has_line(&info, "Created:   1970-01-01 09:00:00"));
	CHECK(!has_line(&info, "09:00:00 UTC"));
	/* a converter that fails falls back to labelled UTC */
	wf_info_build(&info, &d->e[0], d, NULL, failing_local);
	CHECK(has_line(&info, "Created:   1970-01-01 00:00:00 UTC"));

	wf_info_build(&info, &d->e[1], d, NULL, NULL);
	CHECK(has_line(&info, "Type:      folder") &&
	        has_line(&info, "n/a (folder)"));
	wf_info_build(&info, &d->e[2], d, NULL, NULL);
	CHECK(has_line(&info, "file link") &&
	        has_line(&info, "Link:      symbolic link"));
	wf_info_build(&info, &d->e[3], d, NULL, NULL);
	CHECK(has_line(&info, "content not local"));

	wf_info_build(&info, NULL, d, NULL, NULL);
	CHECK(has_line(&info, "no entry"));
	wf_dir_free(d);
}

static void selection_summary(void)
{
	WfSelStore sel;
	WfDir *d = wf_dir_new();
	WfInfo info;

	CHECK(NULL != d);
	wf_sel_init(&sel);
	CHECK(WF_OK == wf_dir_add(d, L"a", 1, 0, 100, 0));
	CHECK(WF_OK == wf_dir_add(d, L"b", 1, 0, 2000, 0));
	CHECK(WF_OK == wf_dir_add(d, L"dir", 3, WF_ENT_DIR, 0, 0));
	CHECK(WF_OK == wf_sel_toggle(&sel, L"C:\\x", L"a"));
	CHECK(WF_OK == wf_sel_toggle(&sel, L"C:\\x", L"dir"));
	CHECK(WF_OK == wf_sel_toggle(&sel, L"C:\\x", L"gone"));
	wf_info_build(&info, &d->e[0], d, wf_sel_folder(&sel, L"C:\\x"), NULL);
	CHECK(has_line(&info, "3 items (1 file, 1 folder), 1 not in listing"));
	CHECK(has_line(&info, "Total:     100 bytes (files only)"));
	wf_sel_toggle(&sel, L"C:\\x", L"b");
	wf_info_build(&info, &d->e[0], d, wf_sel_folder(&sel, L"C:\\x"), NULL);
	CHECK(has_line(&info, "4 items (2 files, 1 folder)"));
	CHECK(has_line(&info, "Total:     2,100 bytes"));

	/* sizes that overflow are reported as a lower bound, not wrapped */
	d->e[0].size = UINT64_MAX;
	d->e[1].size = 5;
	wf_info_build(&info, &d->e[0], d, wf_sel_folder(&sel, L"C:\\x"), NULL);
	CHECK(has_line(&info, "at least"));
	wf_sel_free(&sel);
	wf_dir_free(d);
}

/* Arrows scroll the window and the other keys close it, so the text must not
 * say that any key closes it; the key line under it says what each key does. */
static void no_false_promise_about_closing(void)
{
	WfDir *d = wf_dir_new();
	WfInfo info;

	CHECK(NULL != d);
	CHECK(WF_OK == wf_dir_add(d, L"a.txt", 5, 0, 1, 0));
	wf_info_build(&info, &d->e[0], d, NULL, NULL);
	CHECK(!has_line(&info, "Any key"));
	wf_dir_free(d);
}

static void mode_roundtrip(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfEvent i = {WF_EV_CHAR, 'i', WF_KEY_NONE, 0};
	WfEvent any = {WF_EV_CHAR, 'x', WF_KEY_NONE, 0};
	WfFrame *f;

	CHECK(WF_OK == wf_app_init(&a, 100, 30, fake_port(&fp), L"C:\\Users"));
	CHECK(WF_ACT_REDRAW == a.mode->on_event(&a, &i));
	CHECK(&WF_MODE_INFO == a.mode && a.info.n > 4);
	f = wf_frame_new(100, 30, &WF_STYLE_BASE);
	CHECK(NULL != f);
	if (f) {
		wf_app_draw(&a, f);
		wf_frame_free(f);
	}
	CHECK(WF_ACT_REDRAW == a.mode->on_event(&a, &any));
	CHECK(&WF_MODE_NORMAL == a.mode);
	wf_app_destroy(&a);
}

int main(void)
{
	sizes();
	times();
	attributes();
	entry_details();
	selection_summary();
	no_false_promise_about_closing();
	mode_roundtrip();
	return TEST_RESULT();
}

/*
 * Reading archives: what every supported format lists and returns, what
 * damaged and hostile archives do (the index never contains a name that could
 * leave the archive, limits hold, cancel stops), and that a changed file is
 * indexed again. The archives in tests/archive are made by
 * tools/make_archive_fixtures.py; the hostile ones are written here, entry by
 * entry, so every byte of them is in this file.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "archive/wf_archive.h"
#include "wf_test.h"
#include "wf_zipwriter.h"
#include "base/wf_macro.h"

#ifndef WF_TEST_ARCHIVE_DIR
#	error WF_TEST_ARCHIVE_DIR must name tests/archive
#endif

static P0Lab lab;
static wchar_t fixtures[1024], scratch[1024];

static void fixture_path(const wchar_t *name, wchar_t *out)
{
	swprintf(out, 1100, L"%ls\\%ls", fixtures, name);
}

static void vpath(const wchar_t *file, const wchar_t *inner, wchar_t *out)
{
	swprintf(out, 2200, L"%ls|%ls%ls", file, *inner ? L"\\" : L"", inner);
}

static int by_name(const void *a, const void *b)
{
	return wcscmp(*(wchar_t *const *)a, *(wchar_t *const *)b);
}

/* The names in a folder of an archive as "a b/ c" (folders end in '/'),
 * sorted. */
static WfStatus names_of(
        const wchar_t *file, const wchar_t *inner, wchar_t *out, size_t cap)
{
	wchar_t v[2200];
	WfDir *d = NULL;
	wchar_t *names[256];
	size_t i;
	WfStatus st;

	vpath(file, inner, v);
	st = wf_vpath_list(v, 0, NULL, &d);
	out[0] = L'\0';
	if (WF_OK != st) {
		return st;
	}
	CHECK(d->n <= 256);
	for (i = 0; i < d->n && i < 256; i++) {
		size_t n = wcslen(d->e[i].name);

		names[i] = malloc((n + 2) * sizeof(wchar_t));
		memcpy(names[i], d->e[i].name, n * sizeof(wchar_t));
		names[i][n] = (d->e[i].flags & WF_ENT_DIR) ? L'/' : L'\0';
		names[i][n + 1] = L'\0';
	}
	qsort(names, i, sizeof *names, by_name);
	for (size_t k = 0; k < i; k++) {
		if (k) {
			wcscat_s(out, cap, L" ");
		}
		wcscat_s(out, cap, names[k]);
		free(names[k]);
	}
	wf_dir_free(d);
	return WF_OK;
}

static bool names_are(
        const wchar_t *file, const wchar_t *inner, const wchar_t *expect)
{
	wchar_t got[2048];

	if (WF_OK != names_of(file, inner, got, 2048)) {
		return false;
	}
	if (wcscmp(got, expect)) {
		const wchar_t *c;

		printf("  got:    ");
		for (c = got; *c; c++) {
			printf(*c < 128 ? "%c" : "<%04X>", *c);
		}
		printf("\n  wanted: ");
		for (c = expect; *c; c++) {
			printf(*c < 128 ? "%c" : "<%04X>", *c);
		}
		printf("\n");
		return false;
	}
	return true;
}

static bool read_is(
        const wchar_t *file, const wchar_t *inner, const char *expect)
{
	wchar_t v[2200];
	WfArchiveData d;
	WfStatus st;
	bool ok;

	vpath(file, inner, v);
	st = wf_vpath_read(v, WF_ARCHIVE_READ_MAX, NULL, &d);
	if (WF_OK != st) {
		return false;
	}
	ok = d.n == strlen(expect) && !memcmp(d.bytes, expect, d.n) &&
	        !d.truncated && d.verified;
	wf_archive_data_free(&d);
	return ok;
}

static const wchar_t TOP[] = L"README.md docs/ src/ \u65e5\u672c\u8a9e/";

static void one_tree(
        const wchar_t *name, WfArchiveFormat format, const char *filter)
{
	wchar_t f[1100];
	WfArchive *a;
	WfArchiveInfo info;

	fixture_path(name, f);
	CHECK(names_are(f, L"", TOP));
	CHECK(names_are(f, L"src", L"main.c utils.c"));
	CHECK(names_are(f, L"docs", L"deep/"));
	CHECK(names_are(f, L"docs\\deep\\a\\b\\c", L"d.txt"));
	CHECK(names_are(
	        f, L"\u65e5\u672c\u8a9e", L"\u30d5\u30a1\u30a4\u30eb.txt"));
	CHECK(read_is(
	        f, L"src\\main.c", "int main(void)\n{\n\treturn 0;\n}\n"));
	CHECK(read_is(f, L"README.md",
	        "# piero fixture\n\nhello from the archive\n"));
	CHECK(read_is(f, L"\u65e5\u672c\u8a9e\\\u30d5\u30a1\u30a4\u30eb.txt",
	        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x86"
	        "\xe3\x82"
	        "\xad\xe3\x82\xb9\xe3\x83\x88\n"));
	CHECK(WF_OK == wf_archive_open(f, NULL, &a));
	if (a) {
		info = wf_archive_info(a);
		CHECK(info.format == format);
		CHECK(NULL != strstr(info.filters, filter));
		wf_archive_release(a);
	}
	/* a folder is not a file, a file is not a folder, nothing is nothing */
	{
		wchar_t v[2200];
		WfArchiveData d;
		WfDir *dir;

		vpath(f, L"src", v);
		CHECK(WF_E_INVAL == wf_vpath_read(v, 100, NULL, &d));
		vpath(f, L"src\\main.c", v);
		CHECK(WF_E_NOENT == wf_vpath_list(v, 0, NULL, &dir));
		vpath(f, L"src\\nothing.c", v);
		CHECK(WF_E_NOENT == wf_vpath_read(v, 100, NULL, &d));
		vpath(f, L"..", v);
		CHECK(WF_E_NOENT == wf_vpath_list(v, 0, NULL, &dir));
	}
}

static void every_format_lists_and_reads(void)
{
	one_tree(L"basic.zip", WF_AF_ZIP, "");
	one_tree(L"basic.7z", WF_AF_7Z, "");
	one_tree(L"basic.tar", WF_AF_TAR, "");
	one_tree(L"basic.tar.gz", WF_AF_TAR, "gzip");
	one_tree(L"basic.tgz", WF_AF_TAR, "gzip");
	one_tree(L"basic.tar.xz", WF_AF_TAR, "xz");
	one_tree(L"basic.txz", WF_AF_TAR, "xz");
	one_tree(L"basic.tar.zst", WF_AF_TAR, "zstd");
	one_tree(L"basic.tzst", WF_AF_TAR, "zstd");
}

static void single_files_are_one_file(void)
{
	static const wchar_t *const names[] = {
	        L"single.txt.gz", L"single.txt.xz", L"single.txt.zst"};
	static const char *const filters[] = {"gzip", "xz", "zstd"};
	size_t i;

	for (i = 0; i < 3; i++) {
		wchar_t f[1100];
		WfArchive *a;

		fixture_path(names[i], f);
		CHECK(names_are(f, L"", L"single.txt"));
		CHECK(read_is(f, L"single.txt",
		        "single compressed file\nline two\n"));
		CHECK(WF_OK == wf_archive_open(f, NULL, &a));
		if (a) {
			WfArchiveInfo info = wf_archive_info(a);

			CHECK(WF_AF_SINGLE == info.format);
			CHECK(NULL != strstr(info.filters, filters[i]));
			wf_archive_release(a);
		}
	}
}

static void zip64_solid_and_empty(void)
{
	wchar_t f[1100];

	fixture_path(L"zip64.zip", f);
	CHECK(names_are(f, L"", L"big/"));
	CHECK(names_are(f, L"big", L"forced64.txt"));
	fixture_path(L"empty.zip", f);
	CHECK(names_are(f, L"", L""));
	fixture_path(L"empty.tar", f);
	CHECK(names_are(f, L"", L""));
	fixture_path(L"solid.7z", f);
	{
		wchar_t want[1024] = L"";
		wchar_t one[32];
		int i;

		for (i = 0; i < 40; i++) {
			swprintf(one, 32, L"%sf%02d.txt", i ? L" " : L"", i);
			wcscat_s(want, 1024, one);
		}
		CHECK(names_are(f, L"", want));
	}
	/* a file in the middle of a solid block */
	{
		char expect[16 * 50 + 1] = "";
		int k;

		for (k = 0; k < 50; k++) {
			strcat_s(expect, sizeof expect, "file 17\n");
		}
		CHECK(read_is(f, L"f17.txt", expect));
	}
}

static void reading_is_bounded(void)
{
	wchar_t f[1100], v[2200];
	WfArchiveData d;

	fixture_path(L"basic.zip", f);
	vpath(f, L"README.md", v);
	CHECK(WF_OK == wf_vpath_read(v, 5, NULL, &d));
	CHECK(5 == d.n && d.truncated && !d.verified &&
	        !memcmp(d.bytes, "# pie", 5));
	CHECK(d.size_known && 40 == d.size);
	wf_archive_data_free(&d);
	CHECK(WF_E_INVAL == wf_vpath_read(v, 0, NULL, &d));
	CHECK(WF_E_INVAL ==
	        wf_vpath_read(v, WF_ARCHIVE_ENTRY_MAX + 1, NULL, &d));
}

static bool write_file(const wchar_t *path, const void *data, size_t n)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD w = 0;
	bool ok;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = !n || (WriteFile(h, data, (DWORD)n, &w, NULL) && w == n);
	CloseHandle(h);
	return ok;
}

/* Writes a file in the scratch folder (tracked for removal the first time). */
static bool scratch_write(
        const wchar_t *name, const void *data, size_t n, wchar_t *path)
{
	static wchar_t seen[32][64];
	static size_t nseen;
	size_t i;
	bool known = false;
	bool ok;

	swprintf(path, 1100, L"%ls\\%ls", scratch, name);
	ok = write_file(path, data, n);
	for (i = 0; i < nseen; i++) {
		known = known || !wcscmp(seen[i], name);
	}
	if (ok && !known && nseen < 32) {
		wchar_t rel[128];

		wcscpy_s(seen[nseen++], 64, name);
		swprintf(rel, 128, L"w\\%ls", name);
		CHECK(p0_area_track(lab.area, rel, 0));
	}
	return ok;
}

static bool slurp(const wchar_t *path, uint8_t **data, size_t *n)
{
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	LARGE_INTEGER sz;
	DWORD got = 0;

	*data = NULL;
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	GetFileSizeEx(h, &sz);
	*n = (size_t)sz.QuadPart;
	*data = malloc(*n + 1);
	if (!*data || !ReadFile(h, *data, (DWORD)*n, &got, NULL) || got != *n) {
		free(*data);
		*data = NULL;
		CloseHandle(h);
		return false;
	}
	CloseHandle(h);
	return true;
}

static void not_an_archive_is_not_pretended_to_be_one(void)
{
	wchar_t f[1100];
	WfArchive *a = NULL;
	WfDir *d;
	wchar_t v[2200];
	char text[4096];
	size_t i;

	/* plain text named .zip, .gz, .7z: unsupported, never "a file called data" */
	for (i = 0; i < sizeof text; i++) {
		text[i] = (char)('a' + i % 23);
	}
	CHECK(scratch_write(L"text.zip", text, sizeof text, f));
	CHECK(WF_E_FORMAT == wf_archive_open(f, NULL, &a) && !a);
	CHECK(scratch_write(L"text.gz", text, sizeof text, f));
	CHECK(WF_E_FORMAT == wf_archive_open(f, NULL, &a) && !a);
	CHECK(scratch_write(L"text.7z", text, sizeof text, f));
	CHECK(WF_E_FORMAT == wf_archive_open(f, NULL, &a) && !a);
	/* an empty file */
	CHECK(scratch_write(L"nothing.zip", "", 0, f));
	{
		WfStatus es = wf_archive_open(f, NULL, &a);

		if (WF_E_FORMAT != es) {
			printf("empty file: %d\n", (int)es);
		}
		CHECK(WF_E_FORMAT == es && !a);
	}
	/* a file that does not exist; a folder */
	swprintf(f, 1100, L"%ls\\missing.zip", scratch);
	CHECK(WF_E_NOENT == wf_archive_open(f, NULL, &a));
	CHECK(WF_E_NOENT == wf_archive_open(scratch, NULL, &a));
	/* not a virtual path */
	CHECK(WF_E_INVAL == wf_vpath_list(L"C:\\x", 0, NULL, &d));
	vpath(f, L"", v);
	CHECK(WF_E_NOENT == wf_vpath_list(v, 0, NULL, &d));
}

static void damage_is_reported_not_trusted(void)
{
	uint8_t *data;
	size_t n, cut;
	wchar_t src[1100], f[1100];
	static const wchar_t *const names[] = {L"basic.zip", L"basic.7z",
	        L"basic.tar.gz", L"basic.tar.xz", L"basic.tar.zst",
	        L"single.txt.gz", L"single.txt.xz"};
	size_t k;

	for (k = 0; k < WF_ARRAY_LEN(names); k++) {
		fixture_path(names[k], src);
		CHECK(slurp(src, &data, &n));
		if (!data) {
			continue;
		}
		/* cut short at several places: never a crash, never success
		 * with the wrong content, always one of the named errors */
		for (cut = 1; cut < n; cut += (n / 23) + 1) {
			WfArchive *a = NULL;
			WfStatus st;

			CHECK(scratch_write(L"cut.bin", data, cut, f));
			wf_archive_cache_clear();
			st = wf_archive_open(f, NULL, &a);
			if (WF_OK != st && WF_E_FORMAT != st &&
			        WF_E_CORRUPT != st) {
				printf("cut %ls at %zu: %d\n", names[k], cut,
				        (int)st);
			}
			CHECK(WF_OK == st || WF_E_FORMAT == st ||
			        WF_E_CORRUPT == st || WF_E_ENCRYPTED == st);
			wf_archive_release(a);
		}
		/* flipped bytes everywhere */
		for (cut = 0; cut < n; cut += (n / 31) + 1) {
			WfArchive *a = NULL;
			WfStatus st;
			uint8_t keep = data[cut];

			data[cut] ^= 0x5A;
			CHECK(scratch_write(L"flip.bin", data, n, f));
			data[cut] = keep;
			wf_archive_cache_clear();
			st = wf_archive_open(f, NULL, &a);
			CHECK(WF_OK == st || WF_E_FORMAT == st ||
			        WF_E_CORRUPT == st || WF_E_ENCRYPTED == st ||
			        WF_E_RANGE == st);
			wf_archive_release(a);
		}
		free(data);
	}
}

static bool zip_save(ZipOut *z, const wchar_t *name, wchar_t *path)
{
	uint8_t *data;
	size_t n;
	bool ok;

	zip_finish(z, &data, &n);
	ok = scratch_write(name, data, n, path);
	free(data);
	return ok;
}

/* Every name in every folder, recursively, must be a plain name. */
static bool all_names_plain(
        const wchar_t *file, const wchar_t *inner, unsigned depth)
{
	wchar_t v[2200];
	WfDir *d = NULL;
	size_t i;
	bool ok = true;

	vpath(file, inner, v);
	if (WF_OK != wf_vpath_list(v, 0, NULL, &d) || depth > 80) {
		return false;
	}
	for (i = 0; i < d->n && ok; i++) {
		const wchar_t *nm = d->e[i].name;
		size_t k, n = wcslen(nm);
		wchar_t sub[2200];

		ok = n > 0 && n <= 255 && wcscmp(nm, L".") && wcscmp(nm, L"..");
		for (k = 0; k < n && ok; k++) {
			ok = nm[k] >= 0x20 && 0x7F != nm[k] &&
			        !wcschr(L"\\/|:*?\"<>", nm[k]);
		}
		if (ok && (d->e[i].flags & WF_ENT_DIR)) {
			swprintf(sub, 2200, L"%ls%ls%ls", inner,
			        *inner ? L"\\" : L"", nm);
			ok = all_names_plain(file, sub, depth + 1);
		}
	}
	wf_dir_free(d);
	return ok;
}

static void hostile_names_cannot_leave_or_confuse_the_index(void)
{
	ZipOut z;
	wchar_t f[1100];
	char deep[600] = "";
	char longname[400];
	int i;
	WfArchive *a;

	memset(&z, 0, sizeof z);
	zip_add_s(&z, "../evil.txt", "x");
	zip_add_s(&z, "/abs/x.txt", "abs");
	zip_add_s(&z, "C:\\win\\y.txt", "win");
	zip_add_s(&z, "a/../../z", "z");
	zip_add_s(&z, "ok/./f.txt", "ok");
	zip_add_s(&z, "dup.txt", "first");
	zip_add_s(&z, "dup.txt", "second");
	zip_add_s(&z, "dir/", "");
	zip_add_s(&z, "dir", "a file where a folder is");
	zip_add_s(&z, "file", "a file");
	zip_add_s(&z, "file/child", "under a file");
	zip_add_s(&z, "ctl\x1b[2Jname", "esc");
	zip_add_s(&z, "pipe|name", "pipe");
	zip_add_s(&z, "w\\x.txt", "backslash");
	memset(longname, 'n', sizeof longname - 1);
	longname[sizeof longname - 1] = 0;
	zip_add_s(&z, longname, "too long");
	for (i = 0; i < 70; i++) {
		strcat_s(deep, sizeof deep, "d/");
	}
	strcat_s(deep, sizeof deep, "f");
	zip_add_s(&z, deep, "too deep");
	zip_add_s(&z, "", "empty name");
	CHECK(zip_save(&z, L"hostile.zip", f));
	CHECK(names_are(f, L"",
	        L"C\uFFFD/ abs/ ctl\uFFFD[2Jname dir/ dup.txt file ok/ "
	        L"pipe\uFFFDname w/"));
	CHECK(all_names_plain(f, L"", 0));
	CHECK(read_is(f, L"dup.txt", "second"));
	CHECK(read_is(f, L"abs\\x.txt", "abs"));
	CHECK(read_is(f, L"w\\x.txt", "backslash"));
	CHECK(read_is(f, L"ok\\f.txt", "ok"));
	CHECK(read_is(f, L"file", "a file"));
	CHECK(names_are(f, L"dir", L""));
	CHECK(WF_OK == wf_archive_open(f, NULL, &a));
	if (a) {
		WfArchiveInfo info = wf_archive_info(a);

		/* ../evil, a/../../z, dir (file), file/child, too long, too deep,
		 * empty name */
		CHECK(7 == info.skipped);
		wf_archive_release(a);
	}
	{
		wchar_t v[2200];
		WfDir *d;

		/* none of the skipped names is reachable by any spelling */
		vpath(f, L"..\\evil.txt", v);
		CHECK(WF_E_NOENT == wf_vpath_list(v, 0, NULL, &d));
		vpath(f, L"evil.txt", v);
		CHECK(WF_E_NOENT == wf_vpath_list(v, 0, NULL, &d));
		vpath(f, L"z", v);
		CHECK(WF_E_NOENT == wf_vpath_list(v, 0, NULL, &d));
	}
}

static void names_in_the_system_code_page_are_read_as_such(void)
{
	/* "日本語.txt" in code page 932, no UTF-8 flag: what older Windows tools
	 * write; and the same name flagged as UTF-8 */
	static const char sjis[] = "\x93\xfa\x96\x7b\x8c\xea.txt";
	static const char utf8[] = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.txt";
	ZipOut z;
	wchar_t f[1100];

	memset(&z, 0, sizeof z);
	zip_add(&z, sjis, sizeof sjis - 1, "a", 1, 1, 0);
	CHECK(zip_save(&z, L"sjis.zip", f));
	CHECK(names_are(f, L"", L"\u65e5\u672c\u8a9e.txt"));
	CHECK(read_is(f, L"\u65e5\u672c\u8a9e.txt", "a"));
	zip_add(&z, utf8, sizeof utf8 - 1, "b", 1, 1, 0x800);
	CHECK(zip_save(&z, L"utf8.zip", f));
	CHECK(names_are(f, L"", L"\u65e5\u672c\u8a9e.txt"));
	/* both kinds in one archive */
	zip_add(&z, utf8, sizeof utf8 - 1, "b", 1, 1, 0x800);
	zip_add(&z, "\x83\x74.txt", 6, "c", 1, 1, 0);
	CHECK(zip_save(&z, L"mixed.zip", f));
	CHECK(names_are(f, L"", L"\u30d5.txt \u65e5\u672c\u8a9e.txt"));
}

static void sizes_that_lie_are_not_believed(void)
{
	ZipOut z;
	wchar_t f[1100], v[2200];
	WfArchiveData d;
	WfStatus st;

	memset(&z, 0, sizeof z);
	/* the headers claim 4 GiB - 1 for 5 bytes of data */
	zip_add(&z, "liar.bin", 8, "hello", 5, 0xFFFFFFFFu, 0x800);
	CHECK(zip_save(&z, L"liar.zip", f));
	vpath(f, L"liar.bin", v);
	st = wf_vpath_read(v, 1000, NULL, &d);
	CHECK(WF_E_CORRUPT == st || WF_E_CHANGED == st || WF_E_FORMAT == st);
	CHECK(!d.bytes);
	/* the same claim with a small preview limit: still not "success" with
	 * made-up content */
	st = wf_vpath_read(v, 3, NULL, &d);
	CHECK(WF_OK == st ? (3 == d.n && d.truncated && !d.verified &&
	                            !memcmp(d.bytes, "hel", 3))
	                  : true);
	wf_archive_data_free(&d);
}

static void limits_hold(void)
{
	wchar_t f[1100];
	WfArchive *a;
	ZipOut z;
	int i;
	char name[32];

	/* the bomb: 4 MiB of zeros in 4 KiB of gzip, with a scan limit below it */
	fixture_path(L"bomb.tar.gz", f);
	wf_archive_set_limits(0, 1u << 20);
	wf_archive_cache_clear();
	CHECK(WF_E_RANGE == wf_archive_open(f, NULL, &a) && !a);
	/* the failure is remembered for this file: no second scan */
	CHECK(WF_E_RANGE == wf_archive_open(f, NULL, &a) && !a);
	wf_archive_set_limits(0, 0);
	wf_archive_cache_clear();
	CHECK(WF_OK == wf_archive_open(f, NULL, &a));
	wf_archive_release(a);
	/* too many entries */
	memset(&z, 0, sizeof z);
	for (i = 0; i < 300; i++) {
		snprintf(name, sizeof name, "d%d/f%d.txt", i % 7, i);
		zip_add_s(&z, name, "x");
	}
	CHECK(zip_save(&z, L"many.zip", f));
	wf_archive_set_limits(100, 0);
	wf_archive_cache_clear();
	CHECK(WF_E_RANGE == wf_archive_open(f, NULL, &a) && !a);
	wf_archive_set_limits(0, 0);
	wf_archive_cache_clear();
	CHECK(WF_OK == wf_archive_open(f, NULL, &a));
	if (a) {
		CHECK(wf_archive_info(a).entries == 300 + 7);
		wf_archive_release(a);
	}
}

static bool always_cancelled(void *ctx)
{
	(void)ctx;
	return true;
}

static void empty_headers_count_toward_scan_limit(void)
{
	uint8_t tar[18 * 512] = {0};
	wchar_t file[1100];
	WfArchive *a = NULL;

	/* Repeated empty directories consume stream bytes without growing the index. */
	for (size_t i = 0; i < 16; i++) {
		char *header = (char *)tar + i * 512;
		unsigned checksum = 0;

		memcpy(header, "dir/", 4);
		memcpy(header + 100, "0000755", 7);
		memcpy(header + 108, "0000000", 7);
		memcpy(header + 116, "0000000", 7);
		memcpy(header + 124, "00000000000", 11);
		memcpy(header + 136, "00000000000", 11);
		memset(header + 148, ' ', 8);
		header[156] = '5';
		memcpy(header + 257, "ustar", 5);
		memcpy(header + 263, "00", 2);
		for (size_t j = 0; j < 512; j++) {
			checksum += (unsigned char)header[j];
		}
		(void)snprintf(header + 148, 8, "%06o", checksum);
		header[155] = ' ';
	}
	CHECK(scratch_write(L"empty-headers.tar", tar, sizeof tar, file));
	wf_archive_cache_clear();
	wf_archive_set_limits(0, 2048);
	CHECK(WF_E_RANGE == wf_archive_open(file, NULL, &a) && !a);
	wf_archive_release(a);
	wf_archive_set_limits(3, 0);
	wf_archive_cache_clear();
	a = NULL;
	CHECK(WF_E_RANGE == wf_archive_open(file, NULL, &a) && !a);
	wf_archive_release(a);
	wf_archive_set_limits(0, 0);
	wf_archive_cache_clear();
	a = NULL;
	CHECK(WF_OK == wf_archive_open(file, NULL, &a));
	CHECK(a && wf_archive_info(a).entries == 1);
	wf_archive_release(a);
	wf_archive_cache_clear();
}

static void cancel_stops_and_leaves_nothing(void)
{
	wchar_t f[1100];
	WfArchive *a = NULL;
	WfArchiveCtl ctl = {always_cancelled, NULL};
	WfDir *d;
	wchar_t v[2200];
	WfArchiveData data;

	fixture_path(L"basic.tar.gz", f);
	wf_archive_cache_clear();
	CHECK(WF_E_CANCELLED == wf_archive_open(f, &ctl, &a) && !a);
	/* nothing was cached by the cancelled attempt */
	CHECK(WF_OK == wf_archive_open(f, NULL, &a));
	wf_archive_release(a);
	wf_archive_cache_clear();
	vpath(f, L"", v);
	CHECK(WF_E_CANCELLED == wf_vpath_list(v, 0, &ctl, &d));
	vpath(f, L"README.md", v);
	CHECK(WF_E_CANCELLED == wf_vpath_read(v, 100, &ctl, &data));
	CHECK(!data.bytes);
}

static void links_and_special_entries_are_listed_never_followed(void)
{
	wchar_t f[1100], v[2200];
	WfDir *d;
	WfArchiveData data;
	size_t i;

	fixture_path(L"links.tar", f);
	vpath(f, L"", v);
	CHECK(WF_OK == wf_vpath_list(v, 0, NULL, &d));
	if (d) {
		bool sym = false, hard = false, fifo = false, real = false;

		for (i = 0; i < d->n; i++) {
			bool link = 0 != (d->e[i].flags & WF_ENT_LINK);

			if (!wcscmp(d->e[i].name, L"sym")) {
				sym = link;
			}
			if (!wcscmp(d->e[i].name, L"hard")) {
				hard = link;
			}
			if (!wcscmp(d->e[i].name, L"fifo")) {
				fifo = link;
			}
			if (!wcscmp(d->e[i].name, L"real.txt")) {
				real = !link;
			}
			CHECK(wcscmp(d->e[i].name, L"escape.txt"));
		}
		CHECK(sym && hard && fifo && real);
		wf_dir_free(d);
	}
	vpath(f, L"sym", v);
	CHECK(WF_E_ACCESS == wf_vpath_read(v, 100, NULL, &data));
	vpath(f, L"hard", v);
	CHECK(WF_E_ACCESS == wf_vpath_read(v, 100, NULL, &data));
	vpath(f, L"fifo", v);
	CHECK(WF_E_ACCESS == wf_vpath_read(v, 100, NULL, &data));
	CHECK(read_is(f, L"real.txt", "the real file\n"));
}

static void an_encrypted_entry_is_listed_and_not_read(void)
{
	wchar_t f[1100], v[2200];
	WfArchiveData data;

	fixture_path(L"encrypted.zip", f);
	CHECK(names_are(f, L"", L"secret.txt"));
	vpath(f, L"secret.txt", v);
	CHECK(WF_E_ENCRYPTED == wf_vpath_read(v, 100, NULL, &data));
	CHECK(!data.bytes);
}

static bool copy_over(const wchar_t *from, const wchar_t *to)
{
	uint8_t *d;
	size_t n;
	bool ok = slurp(from, &d, &n) && write_file(to, d, n);

	free(d);
	return ok;
}

static void a_replaced_archive_is_not_answered_from_the_old_index(void)
{
	wchar_t src[1100], f[1100];
	WfArchive *a;
	wchar_t v[2200];
	WfArchiveData data;

	fixture_path(L"basic.zip", src);
	CHECK(scratch_write(L"live.zip", "", 0, f));
	CHECK(copy_over(src, f));
	CHECK(names_are(f, L"src", L"main.c utils.c"));
	CHECK(WF_OK == wf_archive_open(f, NULL, &a));
	CHECK(a && WF_AF_ZIP == wf_archive_info(a).format);
	/* the same name now holds a different archive */
	fixture_path(L"links.tar", src);
	CHECK(copy_over(src, f));
	Sleep(20);
	CHECK(names_are(f, L"", L"fifo hard real.txt sym"));
	{
		WfArchive *b;

		CHECK(WF_OK == wf_archive_open(f, NULL, &b));
		CHECK(b && WF_AF_TAR == wf_archive_info(b).format);
		wf_archive_release(b);
	}
	/* the old reference is still a complete, valid index of what it indexed */
	CHECK(a && WF_AF_ZIP == wf_archive_info(a).format);
	{
		WfDir *d;

		CHECK(WF_OK == wf_archive_list(a, L"src", 0, &d));
		wf_dir_free(d);
	}
	wf_archive_release(a);
	/* a plain file that stopped being an archive */
	CHECK(write_file(f, "plain text now, not an archive at all......", 42));
	vpath(f, L"README.md", v);
	CHECK(WF_E_FORMAT == wf_vpath_read(v, 100, NULL, &data));
}

typedef struct
{
	wchar_t file[1100];
	volatile LONG bad;
} Race;

static DWORD WINAPI racer(LPVOID p)
{
	Race *r = p;
	int i;

	for (i = 0; i < 300; i++) {
		wchar_t v[2200];
		WfDir *d = NULL;
		WfArchiveData data;

		if (i % 50 == 25) {
			wf_archive_cache_clear();
		}
		vpath(r->file, L"src", v);
		if (WF_OK != wf_vpath_list(v, 0, NULL, &d) || 2 != d->n) {
			InterlockedIncrement(&r->bad);
		}
		wf_dir_free(d);
		vpath(r->file, L"README.md", v);
		if (WF_OK != wf_vpath_read(v, 100, NULL, &data) ||
		        40 != data.n) {
			InterlockedIncrement(&r->bad);
		}
		wf_archive_data_free(&data);
	}
	return 0;
}

static void threads_share_the_cache(void)
{
	Race r;
	HANDLE t[4];
	int i;
	static const wchar_t *const names[] = {
	        L"basic.zip", L"basic.7z", L"basic.tar.gz", L"basic.tar.zst"};

	for (i = 0; i < 4; i++) {
		memset(&r, 0, sizeof r);
		fixture_path(names[i], r.file);
		{
			int k;

			for (k = 0; k < 4; k++) {
				t[k] = CreateThread(
				        NULL, 0, racer, &r, 0, NULL);
			}
			WaitForMultipleObjects(4, t, TRUE, 60000);
			for (k = 0; k < 4; k++) {
				CloseHandle(t[k]);
			}
		}
		CHECK(0 == r.bad);
	}
}

typedef struct
{
	wchar_t rel[300];
	bool dir, complete;
	size_t size, chunks, biggest;
	char head[16];
} XItem;

typedef struct
{
	XItem item[64];
	size_t n;
	struct
	{
		wchar_t rel[300];
		WfExtractIssue kind;
	} issue[64];
	size_t n_issue;
	bool open;
	size_t cancel_after; /* files after which cancelled() says yes; 0: never */
	size_t files_ended;
	int skip_file; /* file_begin refuses the nth file (1-based); 0: none */
	size_t begun;
} XLog;

static WfExtractAct x_dir(void *ctx, const wchar_t *rel, uint64_t mtime)
{
	XLog *l = ctx;

	(void)mtime;
	if (l->n < 64) {
		wcscpy_s(l->item[l->n].rel, 300, rel);
		l->item[l->n].dir = true;
		l->item[l->n].complete = true;
		l->n++;
	}
	return WF_EX_CONTINUE;
}

static WfExtractAct x_begin(
        void *ctx, const wchar_t *rel, uint64_t size, bool known, uint64_t m)
{
	XLog *l = ctx;

	(void)size;
	(void)known;
	(void)m;
	l->begun++;
	if (l->skip_file && (int)l->begun == l->skip_file) {
		return WF_EX_SKIP;
	}
	if (l->n < 64) {
		memset(&l->item[l->n], 0, sizeof l->item[l->n]);
		wcscpy_s(l->item[l->n].rel, 300, rel);
		l->open = true;
	}
	return WF_EX_CONTINUE;
}

static WfExtractAct x_data(void *ctx, const void *p, size_t n)
{
	XLog *l = ctx;
	XItem *it = &l->item[l->n];

	if (!it->size) {
		memcpy(it->head, p, n < 15 ? n : 15);
	}
	it->size += n;
	it->chunks++;
	if (n > it->biggest) {
		it->biggest = n;
	}
	return WF_EX_CONTINUE;
}

static void x_end(void *ctx, bool complete)
{
	XLog *l = ctx;

	l->item[l->n].complete = complete;
	if (l->open && l->n < 63) {
		l->n++;
	}
	l->open = false;
	l->files_ended++;
}

static void x_issue(void *ctx, const wchar_t *rel, WfExtractIssue kind)
{
	XLog *l = ctx;

	if (l->n_issue < 64) {
		wcscpy_s(l->issue[l->n_issue].rel, 300, rel);
		l->issue[l->n_issue].kind = kind;
		l->n_issue++;
	}
}

static bool x_cancelled(void *ctx)
{
	XLog *l = ctx;

	return l->cancel_after && l->files_ended >= l->cancel_after;
}

static WfStatus extract(const wchar_t *file,
        const wchar_t *const *inner,
        size_t n,
        const WfExtractLimits *lim,
        XLog *l,
        WfExtractStats *st)
{
	WfExtractSink sink = {
	        l, x_dir, x_begin, x_data, x_end, x_issue, x_cancelled};

	memset(l->item, 0, sizeof l->item);
	l->n = l->n_issue = l->files_ended = l->begun = 0;
	l->open = false;
	return wf_archive_extract(file, inner, n, lim, &sink, NULL, st);
}

static const XItem *x_find(const XLog *l, const wchar_t *rel)
{
	for (size_t i = 0; i < l->n; i++) {
		if (!wcscmp(l->item[i].rel, rel)) {
			return &l->item[i];
		}
	}
	return NULL;
}

static bool x_issue_is(const XLog *l, const wchar_t *rel, WfExtractIssue k)
{
	for (size_t i = 0; i < l->n_issue; i++) {
		if (!wcscmp(l->issue[i].rel, rel)) {
			return l->issue[i].kind == k;
		}
	}
	return false;
}

static XLog xlog;

static void a_selection_extracts_under_its_own_names(void)
{
	ZipOut z;
	wchar_t f[1100];
	WfExtractStats st;
	const wchar_t *one[] = {L"a\\b"};
	const wchar_t *nested[] = {L"a", L"a\\b", L"a"};
	const wchar_t *two[] = {L"a\\e.txt", L"top.txt"};
	const wchar_t *whole[] = {L""};

	memset(&z, 0, sizeof z);
	zip_add_s(&z, "top.txt", "top");
	zip_add_s(&z, "a/b/c.txt", "ccc");
	zip_add_s(&z, "a/b/d.txt", "ddd");
	zip_add_s(&z, "a/e.txt", "eee");
	zip_add_s(&z, "empty/", "");
	CHECK(zip_save(&z, L"x_sel.zip", f));

	/* a folder keeps its name and what is below it */
	CHECK(WF_OK == extract(f, one, 1, NULL, &xlog, &st));
	CHECK(2 == st.files && 0 == st.issues && 6 == st.bytes);
	CHECK(x_find(&xlog, L"b\\c.txt") && x_find(&xlog, L"b\\d.txt") &&
	        !x_find(&xlog, L"a\\e.txt") && !x_find(&xlog, L"top.txt"));
	CHECK(0 == memcmp(x_find(&xlog, L"b\\c.txt")->head, "ccc", 3));

	/* an item inside another, and the same item twice, are taken once */
	CHECK(WF_OK == extract(f, nested, 3, NULL, &xlog, &st));
	CHECK(3 == st.files && x_find(&xlog, L"a\\b\\c.txt") &&
	        x_find(&xlog, L"a\\e.txt") && !x_find(&xlog, L"b\\c.txt"));

	/* two items from different places: each by its own name */
	CHECK(WF_OK == extract(f, two, 2, NULL, &xlog, &st));
	CHECK(2 == st.files && x_find(&xlog, L"e.txt") &&
	        x_find(&xlog, L"top.txt"));

	/* the whole archive: its top-level items, an empty folder included */
	CHECK(WF_OK == extract(f, whole, 1, NULL, &xlog, &st));
	CHECK(4 == st.files && 1 == st.dirs && x_find(&xlog, L"empty") &&
	        x_find(&xlog, L"empty")->dir);

	/* an item that is not there */
	{
		const wchar_t *gone[] = {L"nope"};

		CHECK(WF_E_NOENT == extract(f, gone, 1, NULL, &xlog, &st));
		CHECK(0 == xlog.n && 0 == xlog.begun);
	}
	/* what a selection holds, without reading it */
	CHECK(WF_OK == wf_archive_extract_measure(f, one, 1, NULL, &st));
	CHECK(2 == st.declared_files && 6 == st.declared_bytes);
}

static void unsafe_names_are_refused_where_the_data_would_be_written(void)
{
	ZipOut z;
	wchar_t f[1100];
	WfExtractStats st;
	const wchar_t *whole[] = {L""};

	memset(&z, 0, sizeof z);
	zip_add_s(&z, "ok.txt", "ok");
	zip_add_s(&z, "../evil.txt", "x");
	zip_add_s(&z, "a/../../z", "z");
	zip_add_s(&z, "/abs/x.txt", "abs");
	zip_add_s(&z, "\\\\srv\\share\\u.txt", "unc");
	zip_add_s(&z, "C:\\win\\y.txt", "win");
	zip_add_s(&z, "nul.txt", "dev");
	zip_add_s(&z, "Con", "dev");
	zip_add_s(&z, "lpt1.log", "dev");
	zip_add_s(&z, "sub/aux.c", "dev");
	zip_add_s(&z, "trail.", "dot");
	zip_add_s(&z, "space ", "sp");
	zip_add_s(&z, "ads.txt:stream", "ads");
	zip_add_s(&z, "pipe|name", "pipe");
	zip_add_s(&z, "ctl\x1b[2Jname", "esc");
	zip_add_s(&z, "Doc.txt", "first");
	zip_add_s(&z, "doc.TXT", "second");
	zip_add_s(&z, "Dir/x", "x");
	zip_add_s(&z, "dir/y", "y");
	zip_add(&z, "bad\xff\xfe.txt", 10, "b", 1, 1,
	        0); /* not text, no UTF-8 flag */
	CHECK(zip_save(&z, L"x_hostile.zip", f));

	CHECK(WF_OK == extract(f, whole, 1, NULL, &xlog, &st));
	CHECK(x_find(&xlog, L"ok.txt") && x_find(&xlog, L"Doc.txt") &&
	        x_find(&xlog, L"Dir\\x"));
	CHECK(x_issue_is(&xlog, L"doc.TXT", WF_XI_COLLISION));
	CHECK(x_issue_is(&xlog, L"dir\\y", WF_XI_COLLISION));
	CHECK(x_issue_is(&xlog, L"abs\\x.txt", WF_XI_NAME));
	CHECK(x_issue_is(&xlog, L"nul.txt", WF_XI_NAME));
	CHECK(x_issue_is(&xlog, L"Con", WF_XI_NAME));
	CHECK(x_issue_is(&xlog, L"lpt1.log", WF_XI_NAME));
	CHECK(x_issue_is(&xlog, L"sub\\aux.c", WF_XI_NAME));
	CHECK(x_issue_is(&xlog, L"trail.", WF_XI_NAME));
	CHECK(x_issue_is(&xlog, L"space ", WF_XI_NAME));
	CHECK(x_issue_is(&xlog, L"ads.txt\uFFFDstream", WF_XI_NAME));
	CHECK(x_issue_is(&xlog, L"pipe\uFFFDname", WF_XI_NAME));
	/* nothing that left the archive's namespace, or got in as another name */
	for (size_t i = 0; i < xlog.n; i++) {
		const wchar_t *r = xlog.item[i].rel;

		CHECK(!wcsstr(r, L"..") && !wcschr(r, L':') && L'\\' != r[0] &&
		        !wcsstr(r, L"evil") && !wcsstr(r, L"win\\y"));
	}
	CHECK(st.unplaced >= 2); /* ../evil and a/../../z were never placed */
	CHECK(3 == st.files);
}

static void a_bad_checksum_throws_the_file_away(void)
{
	ZipOut z;
	wchar_t f[1100];
	WfExtractStats st;
	const wchar_t *whole[] = {L""};
	const XItem *it;

	memset(&z, 0, sizeof z);
	zip_add_s(&z, "good.txt", "good");
	zip_crc_xor = 1;
	zip_add_s(&z, "bad.txt", "bad!");
	zip_crc_xor = 0;
	zip_add_s(&z, "after.txt", "after");
	CHECK(zip_save(&z, L"x_crc.zip", f));
	CHECK(WF_OK == extract(f, whole, 1, NULL, &xlog, &st));
	it = x_find(&xlog, L"bad.txt");
	CHECK(it && !it->complete); /* handed over, then disowned */
	CHECK(x_issue_is(&xlog, L"bad.txt", WF_XI_CHECKSUM));
	CHECK(x_find(&xlog, L"good.txt") &&
	        x_find(&xlog, L"good.txt")->complete);
	CHECK(x_find(&xlog, L"after.txt") &&
	        x_find(&xlog, L"after.txt")->complete);
	CHECK(2 == st.files && 1 == st.issues && 9 == st.bytes);

	/* a size that is not what the data holds is no better */
	memset(&z, 0, sizeof z);
	zip_add(&z, "short.txt", 9, "abc", 3, 5, 0x800);
	CHECK(zip_save(&z, L"x_size.zip", f));
	{
		WfStatus s = extract(f, whole, 1, NULL, &xlog, &st);

		CHECK(WF_OK != s || 0 == st.files);
	}
}

static void limits_stop_a_bomb_before_and_while_it_is_written(void)
{
	ZipOut z;
	wchar_t f[1100];
	WfExtractStats st;
	WfExtractLimits lim = wf_extract_default_limits();
	const wchar_t *whole[] = {L""};
	char kb[1000];

	memset(kb, 'k', sizeof kb);
	memset(&z, 0, sizeof z);
	zip_add(&z, "one.bin", 7, kb, sizeof kb, sizeof kb, 0x800);
	zip_add(&z, "two.bin", 7, kb, sizeof kb, sizeof kb, 0x800);
	zip_add(&z, "three.bin", 9, kb, sizeof kb, sizeof kb, 0x800);
	zip_add_s(&z, "dir/", "");
	CHECK(zip_save(&z, L"x_lim.zip", f));

	/* too much in all: refused from the index, nothing is written */
	lim.max_total_bytes = 2500;
	CHECK(WF_E_RANGE == extract(f, whole, 1, &lim, &xlog, &st));
	CHECK(0 == xlog.begun && 0 == xlog.n);
	/* too many entries */
	lim = wf_extract_default_limits();
	lim.max_files = 3;
	CHECK(WF_E_RANGE == extract(f, whole, 1, &lim, &xlog, &st));
	CHECK(0 == xlog.begun);
	/* one file over its limit is left out and said so; the rest is taken */
	lim = wf_extract_default_limits();
	lim.max_file_bytes = 999;
	CHECK(WF_OK == extract(f, whole, 1, &lim, &xlog, &st));
	CHECK(0 == st.files && 3 == st.issues &&
	        x_issue_is(&xlog, L"one.bin", WF_XI_TOO_BIG));
	CHECK(0 ==
	        xlog.begun); /* refused from the entry's size: nothing opened */
	/* the names it has to remember are bounded too */
	lim = wf_extract_default_limits();
	lim.max_name_bytes = 8;
	CHECK(WF_E_RANGE == extract(f, whole, 1, &lim, &xlog, &st));
}

/* A real bomb: a few hundred kilobytes that inflate to 8 MiB. One entry says
 * so; another says it holds 100 bytes. The honest one is refused from its
 * size; for the liar libarchive itself stops at the size it declared and
 * reports a read error (observed: no byte is handed on), and the counters on
 * the bytes that really come out stand behind that. Neither gets past the
 * limit, and the file next to them is not harmed. */
static void a_bomb_is_stopped_by_the_bytes_that_come_out(void)
{
	ZipOut z;
	wchar_t f[1100];
	WfExtractStats st;
	WfExtractLimits lim = wf_extract_default_limits();
	const wchar_t *whole[] = {L""};
	uint8_t *comp;
	size_t cn, matches = 32 * 1024;
	uint64_t real = 1 + 258ull * matches;

	deflate_run_of_a(matches, &comp, &cn);
	CHECK(cn < real / 100); /* it really is a bomb */
	memset(&z, 0, sizeof z);
	zip_add_deflated(&z, "honest.bin", comp, cn, real, 0);
	zip_add_deflated(&z, "liar.bin", comp, cn, 100, 0);
	zip_add_s(&z, "small.txt", "small");
	free(comp);
	CHECK(zip_save(&z, L"x_bomb.zip", f));

	/* over the limit for a file: the honest one is refused from its size, the
	 * liar's is read and stopped; nothing past the limit is handed on */
	lim.max_file_bytes = 1u << 20;
	CHECK(WF_OK == extract(f, whole, 1, &lim, &xlog, &st));
	for (size_t i = 0; i < xlog.n; i++) {
		CHECK(xlog.item[i].size <= lim.max_file_bytes + 256u * 1024u);
	}
	CHECK(x_find(&xlog, L"small.txt") &&
	        x_find(&xlog, L"small.txt")->complete);
	CHECK(!x_find(&xlog, L"honest.bin"));
	CHECK(x_issue_is(&xlog, L"honest.bin", WF_XI_TOO_BIG));
	CHECK(!x_find(&xlog, L"liar.bin") ||
	        !x_find(&xlog, L"liar.bin")->complete);
	CHECK(x_issue_is(&xlog, L"liar.bin", WF_XI_TOO_BIG) ||
	        x_issue_is(&xlog, L"liar.bin", WF_XI_SIZE) ||
	        x_issue_is(&xlog, L"liar.bin", WF_XI_CHECKSUM) ||
	        x_issue_is(&xlog, L"liar.bin", WF_XI_READ));
	CHECK(st.bytes <= 16u);

	/* the whole job's limit, on the bytes that come out of an entry that
	 * understates itself */
	lim = wf_extract_default_limits();
	lim.max_total_bytes = 1u << 20;
	memset(&z, 0, sizeof z);
	deflate_run_of_a(matches, &comp, &cn);
	zip_add_deflated(&z, "liar.bin", comp, cn, 100, 0);
	free(comp);
	CHECK(zip_save(&z, L"x_bomb2.zip", f));
	{
		WfStatus s2 = extract(f, whole, 1, &lim, &xlog, &st);

		CHECK(WF_E_RANGE == s2 || (WF_OK == s2 && 0 == st.files));
		for (size_t i = 0; i < xlog.n; i++) {
			CHECK(xlog.item[i].size <=
			        lim.max_total_bytes + 256u * 1024u);
		}
	}
}

/* The reading side alone, measured: thousands of entries through the checks
 * and the names, into a sink that keeps nothing. */
static void many_entries_are_read_and_checked_quickly(void)
{
	ZipOut z;
	wchar_t f[1100];
	WfExtractStats st;
	const wchar_t *whole[] = {L""};
	WfExtractSink sink = {
	        &xlog, x_dir, x_begin, x_data, x_end, x_issue, x_cancelled};
	ULONGLONG t0, ms;
	enum
	{
		N = 20000
	};

	memset(&z, 0, sizeof z);
	for (int i = 0; i < N; i++) {
		char name[64];

		snprintf(name, sizeof name, "d%03d/f%05d.txt", i % 200, i);
		zip_add_s(&z, name, "x");
	}
	CHECK(zip_save(&z, L"x_many.zip", f));
	memset(&xlog, 0, sizeof xlog);
	t0 = GetTickCount64();
	CHECK(WF_OK == wf_archive_extract(f, whole, 1, NULL, &sink, NULL, &st));
	ms = GetTickCount64() - t0;
	printf("info: %d entries read, checked and handed on in %llu ms "
	       "(the first call also builds the index)\n",
	        N, (unsigned long long)ms);
	CHECK((size_t)N == st.files && 0 == st.issues);
	CHECK(ms < 60000);
	{
		WfArchive *a;

		/* what releasing an idle cache gives back (DR-DRAFT-037) */
		if (WF_OK == wf_archive_open(f, NULL, &a)) {
			WfArchiveInfo info = wf_archive_info(a);

			printf("info: the index of %zu entries holds %zu bytes "
			       "(%zu per entry)\n",
			        info.entries, info.bytes,
			        info.entries ? info.bytes / info.entries : 0);
			wf_archive_release(a);
		}
	}
}

static void a_file_streams_in_blocks_and_a_cancel_stops_it(void)
{
	ZipOut z;
	wchar_t f[1100];
	WfExtractStats st;
	const wchar_t *whole[] = {L""};
	size_t big = (size_t)20 << 20;
	char *data = malloc(big);

	CHECK(NULL != data);
	if (!data) {
		return;
	}
	memset(data, 'z', big);
	memset(&z, 0, sizeof z);
	zip_add(&z, "big.bin", 7, data, big, big, 0x800);
	zip_add_s(&z, "small.txt", "small");
	zip_add_s(&z, "small2.txt", "small");
	free(data);
	CHECK(zip_save(&z, L"x_big.zip", f));
	CHECK(WF_OK == extract(f, whole, 1, NULL, &xlog, &st));
	CHECK(x_find(&xlog, L"big.bin") &&
	        big == x_find(&xlog, L"big.bin")->size);
	CHECK(x_find(&xlog, L"big.bin")->chunks > 1 &&
	        x_find(&xlog, L"big.bin")->biggest <= 256u * 1024u);

	/* cancelled after the first file: stops, says so, takes no more */
	xlog.cancel_after = 1;
	CHECK(WF_E_CANCELLED == extract(f, whole, 1, NULL, &xlog, &st));
	CHECK(1 == xlog.begun);
	xlog.cancel_after = 0;

	/* the sink gives one file up: it is reported, the others go on */
	xlog.skip_file = 2;
	CHECK(WF_OK == extract(f, whole, 1, NULL, &xlog, &st));
	CHECK(2 == st.files && 1 == st.issues);
	xlog.skip_file = 0;
}

static void only_zip_is_extracted(void)
{
	wchar_t f[1100];
	WfExtractStats st;
	const wchar_t *whole[] = {L""};

	fixture_path(L"basic.tar.gz", f);
	CHECK(WF_E_FORMAT == extract(f, whole, 1, NULL, &xlog, &st));
	CHECK(0 == xlog.begun);
	fixture_path(L"zip64.zip", f);
	CHECK(WF_OK == extract(f, whole, 1, NULL, &xlog, &st));
	CHECK(st.files > 0 && 0 == st.issues);
	fixture_path(L"basic.zip", f);
	CHECK(WF_OK == extract(f, whole, 1, NULL, &xlog, &st));
	CHECK(st.files > 0);
}

/* Asks the cache, once, while the index is being built. */
static bool ask_while_building(void *ctx)
{
	uint64_t *seen = ctx;

	if (UINT64_MAX == *seen) {
		*seen = wf_archive_cache_idle_wait(6000, 1000);
	}
	return false;
}

/* DR-DRAFT-037: the cache says when it has been unused for long enough, a touch
 * counts as use, and a clear never takes an index from a caller that holds it. */
static void the_cache_is_released_only_when_unused(void)
{
	wchar_t f[1100];
	WfArchive *a, *again;
	const uint64_t period = 1000;
	uint64_t seen = UINT64_MAX;
	WfArchiveCtl ctl = {ask_while_building, &seen};

	fixture_path(L"basic.tar.gz", f);
	/* a build in progress is use, however long ago the last touch was */
	wf_archive_cache_clear();
	wf_archive_cache_touch(1000);
	CHECK(WF_OK == wf_archive_open(f, &ctl, &a));
	wf_archive_release(a);
	CHECK(1000 == seen);
	wf_archive_cache_clear();
	CHECK(UINT64_MAX ==
	        wf_archive_cache_idle_wait(5000, period)); /* nothing kept */
	CHECK(WF_OK == wf_archive_open(f, NULL, &a));
	wf_archive_cache_touch(10000);
	CHECK(period == wf_archive_cache_idle_wait(10000, period));
	CHECK(400 == wf_archive_cache_idle_wait(10600, period));
	CHECK(0 == wf_archive_cache_idle_wait(11000, period));
	CHECK(0 == wf_archive_cache_idle_wait(99999, period));
	/* a touch later is use: the time counts from it */
	wf_archive_cache_touch(11500);
	CHECK(period == wf_archive_cache_idle_wait(11500, period));
	CHECK(0 != wf_archive_cache_idle_wait(12000, period));
	/* a clock that reads before the stamp is no time passed */
	CHECK(period == wf_archive_cache_idle_wait(11000, period));
	/* an open, a hit or a build, is use: the real clock is stamped */
	wf_archive_cache_touch(10000);
	CHECK(WF_OK == wf_archive_open(f, NULL, &again)); /* a hit */
	wf_archive_release(again);
	CHECK(wf_archive_cache_idle_wait(GetTickCount64(), 60000) >= 59000);
	wf_archive_cache_clear();
	wf_archive_cache_touch(10000);
	CHECK(WF_OK == wf_archive_open(f, NULL, &again)); /* a build */
	wf_archive_release(again);
	CHECK(wf_archive_cache_idle_wait(GetTickCount64(), 60000) >= 59000);
	/* cleared: the reference held is still good, and the next open indexes again */
	wf_archive_cache_clear();
	CHECK(UINT64_MAX == wf_archive_cache_idle_wait(99999, period));
	CHECK(wf_archive_info(a).entries > 0);
	CHECK(WF_OK == wf_archive_open(f, NULL, &again));
	CHECK(again != a);
	wf_archive_release(a);
	wf_archive_release(again);
	wf_archive_cache_clear();
}

int main(void)
{
	wf_archive_set_codepage(932);
	CHECK(p0_lab_open(&lab, "archive", 0));
	if (!lab.area) {
		return TEST_RESULT();
	}
	{
		int n = MultiByteToWideChar(
		        CP_UTF8, 0, WF_TEST_ARCHIVE_DIR, -1, fixtures, 1024);

		CHECK(n > 0);
		for (wchar_t *c = fixtures; *c; c++) {
			if (L'/' == *c) {
				*c = L'\\';
			}
		}
	}
	CHECK(p0_area_mkdir(lab.area, L"w"));
	CHECK(p0_lab_path(&lab, L"w", scratch, 1024));
	every_format_lists_and_reads();
	single_files_are_one_file();
	zip64_solid_and_empty();
	reading_is_bounded();
	not_an_archive_is_not_pretended_to_be_one();
	damage_is_reported_not_trusted();
	hostile_names_cannot_leave_or_confuse_the_index();
	names_in_the_system_code_page_are_read_as_such();
	sizes_that_lie_are_not_believed();
	limits_hold();
	empty_headers_count_toward_scan_limit();
	cancel_stops_and_leaves_nothing();
	links_and_special_entries_are_listed_never_followed();
	an_encrypted_entry_is_listed_and_not_read();
	a_replaced_archive_is_not_answered_from_the_old_index();
	threads_share_the_cache();
	the_cache_is_released_only_when_unused();
	a_selection_extracts_under_its_own_names();
	unsafe_names_are_refused_where_the_data_would_be_written();
	a_bad_checksum_throws_the_file_away();
	limits_stop_a_bomb_before_and_while_it_is_written();
	a_bomb_is_stopped_by_the_bytes_that_come_out();
	many_entries_are_read_and_checked_quickly();
	a_file_streams_in_blocks_and_a_cancel_stops_it();
	only_zip_is_extracted();
	wf_archive_cache_clear();
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}

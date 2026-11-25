/*
 * The pattern of a batch rename (DR-DRAFT-022): template, numbers, find and
 * replace, case, the extension, and every wrong template or number that does not
 * fit.
 */
#include <stdlib.h>
#include <string.h>

#include "xfer/wf_bulkname.h"
#include "wf_test.h"

static bool gives(const WfBulkPattern *p,
        const wchar_t *old,
        bool dir,
        size_t index,
        const wchar_t *want)
{
	wchar_t *got = NULL;
	bool ok = WF_OK == wf_bulk_apply(p, old, dir, index, &got) && got &&
	        !wcscmp(got, want);

	free(got);
	return ok;
}

static WfBulkPattern pat(const wchar_t *tmpl)
{
	WfBulkPattern p;

	memset(&p, 0, sizeof p);
	p.tmpl = tmpl;
	p.start = 1;
	p.step = 1;
	return p;
}

static void numbering_pads_and_steps(void)
{
	WfBulkPattern p = pat(L"photo_{n:3}");

	CHECK(gives(&p, L"IMG_9.jpg", false, 0, L"photo_001.jpg"));
	CHECK(gives(&p, L"IMG_9.jpg", false, 9, L"photo_010.jpg"));
	CHECK(gives(&p, L"x.jpg", false, 998, L"photo_999.jpg"));
	CHECK(gives(&p, L"x.jpg", false, 999,
	        L"photo_1000.jpg")); /* wider is kept */
	p.tmpl = L"{n}-{name}";
	p.start = 10;
	p.step = -3;
	CHECK(gives(&p, L"a.txt", false, 0, L"10-a.txt"));
	CHECK(gives(&p, L"a.txt", false, 4, L"-2-a.txt"));
}

static void prefix_suffix_and_the_extension(void)
{
	WfBulkPattern p = pat(L"2026_{name}_final");

	CHECK(gives(
	        &p, L"report.tar.gz", false, 0, L"2026_report.tar_final.gz"));
	CHECK(gives(&p, L".gitignore", false, 0, L"2026_.gitignore_final"));
	CHECK(gives(&p, L"archive.v2", true, 0,
	        L"2026_archive.v2_final")); /* a folder */
	CHECK(gives(&p, L"noext", false, 0, L"2026_noext_final"));
	CHECK(gives(&p, L"\x5199\x771f.JPG", false, 0,
	        L"2026_\x5199\x771f_final.JPG"));
}

static void find_and_replace_are_literal(void)
{
	WfBulkPattern p = pat(NULL);

	p.find = L"IMG";
	p.replace = L"photo";
	CHECK(gives(&p, L"IMG_IMG.jpg", false, 0, L"photo_photo.jpg"));
	CHECK(gives(&p, L"img.jpg", false, 0, L"img.jpg")); /* case sensitive */
	p.find = L".*";
	p.replace = L"";
	CHECK(gives(&p, L"a.*b.txt", false, 0, L"ab.txt")); /* not a pattern */
	p.find = L"jpg";
	CHECK(gives(&p, L"a.jpg", false, 0,
	        L"a.jpg")); /* the extension is not touched */
	p.find = L"";
	CHECK(gives(&p, L"a.jpg", false, 0, L"a.jpg"));
}

static void case_of_stem_and_extension(void)
{
	WfBulkPattern p = pat(NULL);

	p.stem_case = WF_BULK_CASE_UPPER;
	p.ext_case = WF_BULK_CASE_LOWER;
	CHECK(gives(&p, L"readme.TXT", false, 0, L"README.txt"));
	p.stem_case = WF_BULK_CASE_LOWER;
	p.ext_case = WF_BULK_CASE_UPPER;
	CHECK(gives(&p, L"ReadMe.txt", false, 0, L"readme.TXT"));
	CHECK(gives(&p, L"Dir.Name", true, 0, L"dir.name"));
}

static void braces_are_checked(void)
{
	WfBulkPattern p = pat(L"{{n}} {n}");
	size_t pos = 99;

	CHECK(WF_OK == wf_bulk_check(&p, &pos));
	CHECK(gives(&p, L"a.txt", false, 4, L"{n} 5.txt"));
	p.tmpl = L"a{x}";
	CHECK(WF_E_INVAL == wf_bulk_check(&p, &pos) && 1 == pos);
	p.tmpl = L"ab}";
	CHECK(WF_E_INVAL == wf_bulk_check(&p, &pos) && 2 == pos);
	p.tmpl = L"{n:0}";
	CHECK(WF_E_INVAL == wf_bulk_check(&p, &pos));
	p.tmpl = L"{n:12}";
	CHECK(WF_E_INVAL == wf_bulk_check(&p, &pos));
	p.tmpl = L"{name";
	CHECK(WF_E_INVAL == wf_bulk_check(&p, &pos));
	{
		wchar_t *got = NULL;

		CHECK(WF_E_INVAL == wf_bulk_apply(&p, L"a", false, 0, &got) &&
		        !got);
	}
}

static void what_does_not_fit_is_an_error_not_a_wrong_name(void)
{
	WfBulkPattern p = pat(L"{n}");
	wchar_t *got = NULL;
	static wchar_t big[5000];

	p.start = INT64_MAX;
	p.step = 1;
	CHECK(WF_OK == wf_bulk_apply(&p, L"a", false, 0, &got));
	free(got);
	CHECK(WF_E_RANGE == wf_bulk_apply(&p, L"a", false, 1, &got) && !got);
	p.start = 0;
	p.step = INT64_MAX;
	CHECK(WF_E_RANGE == wf_bulk_apply(&p, L"a", false, 2, &got) && !got);
	p.step = INT64_MIN;
	CHECK(WF_E_RANGE == wf_bulk_apply(&p, L"a", false, 1, &got) && !got);
	for (size_t i = 0; i + 1 < sizeof big / sizeof *big; i++) {
		big[i] = L'x';
	}
	p.tmpl = L"{name}{name}";
	p.step = 1;
	CHECK(WF_E_RANGE == wf_bulk_apply(&p, big, false, 0, &got) && !got);
}

int main(void)
{
	numbering_pads_and_steps();
	prefix_suffix_and_the_extension();
	find_and_replace_are_literal();
	case_of_stem_and_extension();
	braces_are_checked();
	what_does_not_fit_is_an_error_not_a_wrong_name();
	return TEST_RESULT();
}

#include <stdio.h>
#include <wchar.h>

#include "ui/wf_sel.h"
#include "wf_test.h"

static void test_toggle(void)
{
	WfSelStore s;
	const WfNameSet *set;

	wf_sel_init(&s);
	CHECK(NULL == wf_sel_folder(&s, L"C:\\a"));
	CHECK(!wf_nameset_has(NULL, L"x"));
	CHECK(0 == wf_sel_count(&s, L"C:\\a"));

	CHECK(WF_OK == wf_sel_toggle(&s, L"C:\\a", L"b"));
	CHECK(WF_OK == wf_sel_toggle(&s, L"C:\\a", L"a"));
	CHECK(WF_OK == wf_sel_toggle(&s, L"C:\\a", L"c"));
	set = wf_sel_folder(&s, L"C:\\a");
	CHECK(set && 3 == set->n);
	CHECK(0 == wcscmp(set->names[0], L"a") &&
	        0 == wcscmp(set->names[2], L"c"));
	CHECK(wf_nameset_has(set, L"b") && !wf_nameset_has(set, L"z"));

	/* folders are independent */
	CHECK(WF_OK == wf_sel_toggle(&s, L"C:\\other", L"b"));
	CHECK(3 == wf_sel_count(&s, L"C:\\a") &&
	        1 == wf_sel_count(&s, L"C:\\other"));

	/* toggling again removes; the last removal drops the folder entirely */
	CHECK(WF_OK == wf_sel_toggle(&s, L"C:\\a", L"b"));
	CHECK(2 == wf_sel_count(&s, L"C:\\a"));
	CHECK(WF_OK == wf_sel_toggle(&s, L"C:\\other", L"b"));
	CHECK(NULL == wf_sel_folder(&s, L"C:\\other"));

	wf_sel_clear(&s, L"C:\\a");
	CHECK(0 == wf_sel_count(&s, L"C:\\a"));
	wf_sel_clear(&s, L"C:\\never"); /* no-op */
	wf_sel_free(&s);
}

static void test_many(void)
{
	WfSelStore s;
	wchar_t name[16];
	int i;

	wf_sel_init(&s);
	for (i = 0; i < 1000; i++) {
		swprintf(name, 16, L"n%04d",
		        (i * 37) % 1000); /* scrambled order */
		CHECK(WF_OK == wf_sel_toggle(&s, L"C:\\d", name));
	}
	{
		const WfNameSet *set = wf_sel_folder(&s, L"C:\\d");
		size_t k;

		CHECK(set && 1000 == set->n);
		for (k = 1; set && k < set->n; k++) {
			if (wcscmp(set->names[k - 1], set->names[k]) >= 0) {
				CHECK_FAIL("sorted and unique");
				break;
			}
		}
	}
	wf_sel_free(&s);
}

static void test_prune(void)
{
	WfSelStore s;
	WfDir *d = wf_dir_new();

	wf_dir_add(d, L"keep", 4, 0, 0, 0);
	wf_dir_add(d, L"also", 4, 0, 0, 0);
	wf_sel_init(&s);
	wf_sel_toggle(&s, L"C:\\d", L"keep");
	wf_sel_toggle(&s, L"C:\\d", L"gone");
	wf_sel_toggle(&s, L"C:\\d", L"zzz");

	wf_sel_prune(&s, L"C:\\d", d);
	CHECK(1 == wf_sel_count(&s, L"C:\\d"));
	CHECK(wf_nameset_has(wf_sel_folder(&s, L"C:\\d"), L"keep"));

	/* nothing left selected exists: the folder entry disappears */
	wf_sel_toggle(&s, L"C:\\e", L"x");
	wf_sel_prune(&s, L"C:\\e", d);
	CHECK(NULL == wf_sel_folder(&s, L"C:\\e"));

	/* an empty listing prunes everything; unknown folders are ignored */
	{
		WfDir *empty = wf_dir_new();

		wf_sel_prune(&s, L"C:\\d", empty);
		CHECK(NULL == wf_sel_folder(&s, L"C:\\d"));
		wf_sel_prune(&s, L"C:\\nope", empty);
		wf_dir_free(empty);
	}
	wf_dir_free(d);
	wf_sel_free(&s);
}

/* The listing is in any order and may hold a name twice; a long listing and a
 * short selection cost one pass; the names that stay are still sorted. */
static void test_prune_unsorted_duplicates_and_a_long_listing(void)
{
	WfSelStore s;
	WfDir *d = wf_dir_new();
	const WfNameSet *set;
	wchar_t name[32];

	for (int i = 20000;
	        i-- > 0;) { /* descending, so not in the set's order */
		swprintf(name, 32, L"file%05d", i);
		CHECK(WF_OK == wf_dir_add(d, name, wcslen(name), 0, 0, 0));
	}
	CHECK(WF_OK == wf_dir_add(d, L"file00007", 9, 0, 0, 0)); /* twice */
	wf_sel_init(&s);
	CHECK(WF_OK == wf_sel_toggle(&s, L"C:\\d", L"file00007"));
	CHECK(WF_OK == wf_sel_toggle(&s, L"C:\\d", L"file19999"));
	CHECK(WF_OK == wf_sel_toggle(&s, L"C:\\d", L"file00000"));
	CHECK(WF_OK == wf_sel_toggle(&s, L"C:\\d", L"file20000")); /* gone */
	CHECK(WF_OK == wf_sel_toggle(&s, L"C:\\d", L"a-name-before-all"));
	wf_sel_prune(&s, L"C:\\d", d);
	set = wf_sel_folder(&s, L"C:\\d");
	CHECK(set && 3 == set->n);
	CHECK(wf_nameset_has(set, L"file00007") &&
	        wf_nameset_has(set, L"file19999") &&
	        wf_nameset_has(set, L"file00000"));
	CHECK(!wf_nameset_has(set, L"file20000") &&
	        !wf_nameset_has(set, L"a-name-before-all"));
	for (size_t k = 1; set && k < set->n; k++) {
		CHECK(wcscmp(set->names[k - 1], set->names[k]) < 0);
	}
	/* every selected name is in the listing: all stay */
	wf_sel_prune(&s, L"C:\\d", d);
	CHECK(3 == wf_sel_count(&s, L"C:\\d"));
	/* an empty selection is not a folder: nothing to prune */
	wf_sel_prune(&s, L"C:\\other", d);
	CHECK(NULL == wf_sel_folder(&s, L"C:\\other"));
	wf_dir_free(d);
	wf_sel_free(&s);
}

int main(void)
{
	test_toggle();
	test_many();
	test_prune();
	test_prune_unsorted_duplicates_and_a_long_listing();
	return TEST_RESULT();
}

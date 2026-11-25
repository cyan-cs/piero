#include "fs/wf_debounce.h"
#include "wf_test.h"

static void test_basic(void)
{
	WfDebounce d;

	wf_debounce_init(&d);
	CHECK(WF_WAIT_FOREVER == wf_debounce_timeout(&d, 1000));
	CHECK(!wf_debounce_take(&d, 1000));

	wf_debounce_note(&d, 1000);
	CHECK(100 == wf_debounce_timeout(&d, 1000));
	CHECK(60 == wf_debounce_timeout(&d, 1040));
	CHECK(!wf_debounce_take(&d, 1099));
	CHECK(wf_debounce_take(&d, 1100));
	CHECK(!wf_debounce_take(&d, 1100)); /* consumed exactly once */
	CHECK(WF_WAIT_FOREVER == wf_debounce_timeout(&d, 1100));
}

static void test_burst_does_not_extend(void)
{
	WfDebounce d;

	wf_debounce_init(&d);
	wf_debounce_note(&d, 1000);
	wf_debounce_note(&d, 1050);
	wf_debounce_note(
	        &d, 1090); /* same window: must not push the deadline out */
	CHECK(wf_debounce_take(&d, 1100));
}

static void test_overdue(void)
{
	WfDebounce d;

	wf_debounce_init(&d);
	wf_debounce_note(&d, 1000);
	CHECK(0 == wf_debounce_timeout(&d, 5000));
	CHECK(wf_debounce_take(&d, 5000));
}

static void test_backoff_and_reset(void)
{
	WfDebounce d;

	wf_debounce_init(&d);
	wf_debounce_note(&d, 1000); /* 100 */
	CHECK(wf_debounce_take(&d, 1100));
	wf_debounce_note(&d, 1150); /* busy: 250 */
	CHECK(250 == wf_debounce_timeout(&d, 1150));
	CHECK(wf_debounce_take(&d, 1400));
	wf_debounce_note(&d, 1500); /* 500 */
	CHECK(500 == wf_debounce_timeout(&d, 1500));
	CHECK(wf_debounce_take(&d, 2000));
	wf_debounce_note(&d, 2100); /* 1000 */
	CHECK(1000 == wf_debounce_timeout(&d, 2100));
	CHECK(wf_debounce_take(&d, 3100));
	wf_debounce_note(&d, 3200); /* capped at 1000 */
	CHECK(1000 == wf_debounce_timeout(&d, 3200));
	CHECK(wf_debounce_take(&d, 4200));

	wf_debounce_note(&d, 20000); /* quiet for a long time: back to 100 */
	CHECK(100 == wf_debounce_timeout(&d, 20000));
}

int main(void)
{
	test_basic();
	test_burst_does_not_extend();
	test_overdue();
	test_backoff_and_reset();
	return TEST_RESULT();
}

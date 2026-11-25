/*
 * Batch journal (DR-DRAFT-022): CRC and name round trips, torn-tail recovery,
 * interior damage, incomplete plans, unsupported versions and append after a
 * cut. Uses real files in a temporary directory.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xfer/wf_batchlog_win.h"
#include "wf_test.h"

static wchar_t dir[MAX_PATH + 32];
static const uint8_t BATCH[16] = {
        0xaa, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 0xbb};

static void make_dir(void)
{
	wchar_t tmp[MAX_PATH];

	GetTempPathW(MAX_PATH, tmp);
	swprintf(dir, MAX_PATH + 32, L"%lspiero-batchlog-%lu", tmp,
	        GetCurrentProcessId());
	CreateDirectoryW(dir, NULL);
}

static void wipe(void)
{
	wchar_t pattern[MAX_PATH + 40];
	WIN32_FIND_DATAW d;
	HANDLE h;

	swprintf(pattern, MAX_PATH + 40, L"%ls\\*", dir);
	h = FindFirstFileW(pattern, &d);
	if (INVALID_HANDLE_VALUE != h) {
		do {
			wchar_t f[MAX_PATH + 300];

			if (d.cFileName[0] != L'.') {
				swprintf(f, MAX_PATH + 300, L"%ls\\%ls", dir,
				        d.cFileName);
				DeleteFileW(f);
			}
		} while (FindNextFileW(h, &d));
		FindClose(h);
	}
}

static void journal_path(wchar_t out[MAX_PATH + 80])
{
	swprintf(out, MAX_PATH + 80,
	        L"%ls\\aa0102030405060708090a0b0c0d0ebb.pbj", dir);
}

static const wchar_t LONE[] = {
        L'C', L':', L'\\', L'x', 0xd800, L'y', L'.', L't', 0};

static WfBjItem items[2];
static WfBjStep steps[3];

static void sample(void)
{
	memset(items, 0, sizeof items);
	items[0].volume = 0x1122334455667788ull;
	for (int i = 0; i < 16; i++) {
		items[0].id[i] = (uint8_t)i;
		items[1].id[i] = (uint8_t)(255 - i);
	}
	items[0].path = (wchar_t
	                *)L"C:\\Work\\\x5199\x771f one.jpg"; /* a space, Japanese */
	items[1].path = (wchar_t *)LONE; /* a lone surrogate */
	items[1].dir = true;
	steps[0].item = 0;
	steps[0].to = (wchar_t *)L"C:\\Work\\.piero-batch-x-0";
	steps[0].temp = true;
	steps[1].item = 1;
	steps[1].to = (wchar_t *)L"C:\\Work\\a*b%c.txt";
	steps[2].item = 0;
	steps[2].to =
	        (wchar_t *)L"C:\\Work\\\xd83d\xde00.jpg"; /* a surrogate pair */
}

static void write_whole(bool with_marks)
{
	WfBatchLog *log = NULL;

	sample();
	CHECK(WF_OK == wf_batchlog_create(dir, BATCH, &log));
	CHECK(WF_OK == wf_batchlog_plan(log, items, 2, steps, 3));
	if (with_marks) {
		CHECK(WF_OK == wf_batchlog_mark(log, 'B', 0));
		CHECK(WF_OK == wf_batchlog_mark(log, 'D', 0));
		CHECK(WF_OK == wf_batchlog_mark(log, 'B', 1));
	}
	wf_batchlog_close(log);
}

static void the_plan_and_the_marks_come_back_exactly(void)
{
	wchar_t f[MAX_PATH + 80];
	WfBatchJournal j;

	wipe();
	write_whole(true);
	journal_path(f);
	CHECK(WF_BJ_READ_OK == wf_batchlog_read(f, &j));
	CHECK(2 == j.n_items && 3 == j.n_steps && !j.ended && !j.torn);
	CHECK(0x1122334455667788ull == j.item[0].volume &&
	        7 == j.item[0].id[7] && 248 == j.item[1].id[7]);
	CHECK(!wcscmp(j.item[0].path, L"C:\\Work\\\x5199\x771f one.jpg"));
	CHECK(!wcscmp(j.item[1].path, LONE) && j.item[1].dir && !j.item[0].dir);
	CHECK(!wcscmp(j.step[1].to, L"C:\\Work\\a*b%c.txt"));
	CHECK(!wcscmp(j.step[2].to, L"C:\\Work\\\xd83d\xde00.jpg") &&
	        j.step[0].temp && !j.step[1].temp && 0 == j.step[2].item &&
	        1 == j.step[1].item);
	CHECK(WF_BJ_STEP_DONE == j.state[0] && WF_BJ_STEP_BEGUN == j.state[1] &&
	        WF_BJ_STEP_PLANNED == j.state[2]);
	wf_batchjournal_free(&j);
}

/* A line written by hand, with its CRC, for the cases the writer never makes. */
static void raw_line(HANDLE h, const char *text)
{
	char line[512];
	DWORD n;
	int len = snprintf(line, sizeof line, "%s*%08x\n", text,
	        (unsigned)wf_batchlog_crc32(text, strlen(text)));

	WriteFile(h, line, (DWORD)len, &n, NULL);
}

static HANDLE raw_file(const wchar_t *f)
{
	HANDLE h = CreateFileW(f, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);

	CHECK(INVALID_HANDLE_VALUE != h);
	return h;
}

static void an_end_is_read_and_a_line_after_it_is_damage(void)
{
	wchar_t f[MAX_PATH + 80];
	WfBatchJournal j;

	wipe();
	journal_path(f);
	{
		HANDLE h = raw_file(f);

		raw_line(h, "PBJ1 0 0");
		raw_line(h, "G");
		raw_line(h, "E SUCCESS");
		CloseHandle(h);
		CHECK(WF_BJ_READ_OK == wf_batchlog_read(f, &j));
		CHECK(j.ended && WF_BJ_SUCCESS == j.end);
		wf_batchjournal_free(&j);
	}
	wipe();
	{
		HANDLE h = raw_file(f);

		raw_line(h, "PBJ1 0 0");
		raw_line(h, "G");
		raw_line(h, "E SUCCESS");
		raw_line(h, "B 0");
		CloseHandle(h);
		/* a good CRC and a line that cannot follow an end: not a torn write */
		CHECK(WF_BJ_READ_DAMAGED == wf_batchlog_read(f, &j));
	}
}

static void cut_the_file(const wchar_t *f, long long bytes_off)
{
	HANDLE h = CreateFileW(f, GENERIC_READ | GENERIC_WRITE, 0, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	LARGE_INTEGER size, at;

	CHECK(INVALID_HANDLE_VALUE != h);
	GetFileSizeEx(h, &size);
	at.QuadPart = size.QuadPart + bytes_off;
	SetFilePointerEx(h, at, NULL, FILE_BEGIN);
	SetEndOfFile(h);
	CloseHandle(h);
}

static void flip_a_byte(const wchar_t *f, long long from_start)
{
	HANDLE h = CreateFileW(f, GENERIC_READ | GENERIC_WRITE, 0, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	LARGE_INTEGER at;
	char c;
	DWORD n;

	CHECK(INVALID_HANDLE_VALUE != h);
	at.QuadPart = from_start;
	SetFilePointerEx(h, at, NULL, FILE_BEGIN);
	ReadFile(h, &c, 1, &n, NULL);
	c ^= 1;
	SetFilePointerEx(h, at, NULL, FILE_BEGIN);
	WriteFile(h, &c, 1, &n, NULL);
	CloseHandle(h);
}

static void a_torn_last_line_is_ignored_and_the_rest_is_kept(void)
{
	wchar_t f[MAX_PATH + 80];
	WfBatchJournal j;

	wipe();
	write_whole(true);
	journal_path(f);
	cut_the_file(f, -5); /* the last line, B 1, without its end */
	CHECK(WF_BJ_READ_OK == wf_batchlog_read(f, &j));
	CHECK(j.torn && WF_BJ_STEP_DONE == j.state[0] &&
	        WF_BJ_STEP_PLANNED == j.state[1]);
	wf_batchjournal_free(&j);
	/* cut inside the last line but after its newline-less body: still torn */
	wipe();
	write_whole(true);
	cut_the_file(f, -1);
	CHECK(WF_BJ_READ_OK == wf_batchlog_read(f, &j) && j.torn);
	wf_batchjournal_free(&j);
}

static void damage_in_the_middle_is_not_a_torn_tail(void)
{
	wchar_t f[MAX_PATH + 80];
	WfBatchJournal j;

	wipe();
	write_whole(true);
	journal_path(f);
	flip_a_byte(f, 60); /* inside an item line, with lines after it */
	CHECK(WF_BJ_READ_DAMAGED == wf_batchlog_read(f, &j));
	CHECK(0 == j.n_items && !j.item);
}

static void a_plan_that_was_not_completed_is_no_plan(void)
{
	wchar_t f[MAX_PATH + 80];
	WfBatchJournal j;
	HANDLE h;

	wipe();
	journal_path(f);
	h = raw_file(f);
	raw_line(h, "PBJ1 1 1");
	raw_line(h,
	        "I 0 0000000000000001 000102030405060708090a0b0c0d0e0f f "
	        "C:\\a");
	raw_line(h, "S 0 0 C:\\b 0"); /* and then the process ended: no G */
	CloseHandle(h);
	CHECK(WF_BJ_READ_NO_PLAN == wf_batchlog_read(f, &j));
	CHECK(0 == j.n_items && !j.item);
}

static void another_version_and_nonsense_are_not_read(void)
{
	wchar_t f[MAX_PATH + 80];
	WfBatchJournal j;
	HANDLE h;
	DWORD n;

	wipe();
	journal_path(f);
	h = CreateFileW(f, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	WriteFile(h, "PBJ2 1 1*00000000\n", 18, &n, NULL);
	CloseHandle(h);
	CHECK(WF_BJ_READ_UNSUPPORTED == wf_batchlog_read(f, &j));
	wipe();
	h = CreateFileW(f, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	WriteFile(h, "not a journal\nat all\n", 21, &n, NULL);
	CloseHandle(h);
	CHECK(WF_BJ_READ_DAMAGED == wf_batchlog_read(f, &j));
	wipe();
	CHECK(WF_BJ_READ_DAMAGED == wf_batchlog_read(f, &j)); /* missing */
}

static void writing_goes_on_after_a_cut_and_the_state_follows(void)
{
	wchar_t f[MAX_PATH + 80];
	WfBatchJournal j;
	WfBatchLog *log;

	wipe();
	write_whole(true);
	journal_path(f);
	cut_the_file(f, -3); /* B 1 is torn */
	CHECK(WF_BJ_READ_OK == wf_batchlog_read(f, &j) && j.torn);
	CHECK(WF_OK == wf_batchlog_resume(f, j.valid_bytes, &log));
	wf_batchjournal_free(&j);
	CHECK(WF_OK == wf_batchlog_mark(log, 'R', 0));
	CHECK(WF_OK == wf_batchlog_end(log, WF_BJ_ROLLED_BACK));
	wf_batchlog_close(log);
	CHECK(WF_BJ_READ_OK == wf_batchlog_read(f, &j));
	CHECK(!j.torn && j.ended && WF_BJ_ROLLED_BACK == j.end);
	CHECK(WF_BJ_STEP_RETURNED == j.state[0] &&
	        WF_BJ_STEP_PLANNED == j.state[1]);
	wf_batchjournal_free(&j);
}

static void a_journal_is_never_replaced_and_marks_are_checked(void)
{
	WfBatchLog *log = NULL, *again = NULL;

	wipe();
	write_whole(false);
	CHECK(WF_E_INVAL == wf_batchlog_create(dir, BATCH, &again) && !again);
	CHECK(WF_OK == wf_batchlog_create(dir, (const uint8_t[16]){1}, &log));
	CHECK(WF_E_INVAL == wf_batchlog_mark(log, 'X', 0));
	CHECK(WF_E_INVAL == wf_batchlog_end(log, (WfBatchEnd)9));
	wf_batchlog_close(log);
}

static void every_state_change_is_flushed(void)
{
	WfBatchLog *log = NULL;
	WfBatchLogStats s;

	wipe();
	sample();
	CHECK(WF_OK == wf_batchlog_create(dir, BATCH, &log));
	CHECK(WF_OK == wf_batchlog_plan(log, items, 2, steps, 3));
	s = wf_batchlog_stats(log);
	CHECK(1 == s.flushes &&
	        1 == s.writes); /* the whole plan, one write, one flush */
	CHECK(WF_OK == wf_batchlog_mark(log, 'B', 0));
	CHECK(WF_OK == wf_batchlog_mark(log, 'D', 0));
	s = wf_batchlog_stats(log);
	CHECK(3 == s.flushes);
	wf_batchlog_close(log);
}

static void the_crc_is_the_usual_one(void)
{
	CHECK(0xcbf43926u == wf_batchlog_crc32("123456789", 9));
	CHECK(0u == wf_batchlog_crc32("", 0));
}

int main(void)
{
	make_dir();
	the_crc_is_the_usual_one();
	the_plan_and_the_marks_come_back_exactly();
	an_end_is_read_and_a_line_after_it_is_damage();
	a_torn_last_line_is_ignored_and_the_rest_is_kept();
	damage_in_the_middle_is_not_a_torn_tail();
	a_plan_that_was_not_completed_is_no_plan();
	another_version_and_nonsense_are_not_read();
	writing_goes_on_after_a_cut_and_the_state_follows();
	a_journal_is_never_replaced_and_marks_are_checked();
	every_state_change_is_flushed();
	wipe();
	RemoveDirectoryW(dir);
	return TEST_RESULT();
}

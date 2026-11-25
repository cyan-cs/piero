/*
 * The two-slot record file (drafts, the saved session): the newest whole
 * record wins, a write goes to the older slot so the other stays whole, a slot
 * cut short or of another kind is ignored, and a record over the limit is
 * refused. Real files in the temporary folder, removed afterwards.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "base/wf_slotfile.h"
#include "wf_test.h"

#define MAGIC "piero-test 1"
#define LIMIT 1024

static wchar_t file_a[MAX_PATH + 16], file_b[MAX_PATH + 16];

static void make_names(void)
{
	wchar_t dir[MAX_PATH];

	GetTempPathW(MAX_PATH, dir);
	swprintf(file_a, MAX_PATH + 16, L"%lsslotfile-%lu.a", dir,
	        GetCurrentProcessId());
	swprintf(file_b, MAX_PATH + 16, L"%lsslotfile-%lu.b", dir,
	        GetCurrentProcessId());
}

static void put_raw(const wchar_t *file, const char *bytes)
{
	HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD n;

	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		WriteFile(h, bytes, (DWORD)strlen(bytes), &n, NULL);
		CloseHandle(h);
	}
}

static const wchar_t *const files[2] = {file_a, file_b};

/* The text a reader would trust now, or NULL. */
static char *newest(void)
{
	WfSlotFile s[2];
	char *text = NULL;
	int pick;

	wf_slotfile_read(file_a, MAGIC, LIMIT, &s[0]);
	wf_slotfile_read(file_b, MAGIC, LIMIT, &s[1]);
	pick = wf_slotfile_pick(&s[0], &s[1]);
	if (pick >= 0) {
		text = _strdup(s[pick].body);
	}
	wf_slotfile_free(&s[0]);
	wf_slotfile_free(&s[1]);
	return text;
}

static void a_record_round_trips_and_the_newest_wins(void)
{
	char *got;

	CHECK(WF_OK ==
	        wf_slotfile_write(
	                files, MAGIC, "x one\n", "y two\n", "first", 5, LIMIT));
	got = newest();
	CHECK(got && !strcmp(got, "first"));
	free(got);
	CHECK(WF_OK ==
	        wf_slotfile_write(
	                files, MAGIC, NULL, NULL, "second", 6, LIMIT));
	got = newest();
	CHECK(got && !strcmp(got, "second"));
	free(got);
	CHECK(WF_OK ==
	        wf_slotfile_write(files, MAGIC, NULL, NULL, "", 0, LIMIT));
	got = newest();
	CHECK(got && !*got); /* an empty body is a record too */
	free(got);
}

static void the_header_lines_are_kept_where_they_were_put(void)
{
	WfSlotFile s[2];
	int pick;

	CHECK(WF_OK ==
	        wf_slotfile_write(files, MAGIC, "before 1\n", "after 2\n", "b",
	                1, LIMIT));
	wf_slotfile_read(file_a, MAGIC, LIMIT, &s[0]);
	wf_slotfile_read(file_b, MAGIC, LIMIT, &s[1]);
	pick = wf_slotfile_pick(&s[0], &s[1]);
	CHECK(pick >= 0);
	if (pick >= 0) {
		const char *h = s[pick].header;
		const char *b4 = strstr(h, "\nbefore 1"),
		           *sq = strstr(h, "\nseq "),
		           *af = strstr(h, "\nafter 2"),
		           *sz = strstr(h, "\nsize ");

		CHECK(b4 && sq && af && sz && b4 < sq && sq < af && af < sz);
	}
	wf_slotfile_free(&s[0]);
	wf_slotfile_free(&s[1]);
}

/* A write that stops half way leaves the other slot whole. */
static void a_cut_short_slot_is_ignored_and_the_other_is_kept(void)
{
	char *got;
	WfSlotFile s;

	CHECK(WF_OK ==
	        wf_slotfile_write(files, MAGIC, NULL, NULL, "keep", 4, LIMIT));
	CHECK(WF_OK ==
	        wf_slotfile_write(files, MAGIC, NULL, NULL, "newer", 5, LIMIT));
	/* the newer one is cut short: its size says 5, only 2 bytes are there */
	put_raw(file_a, MAGIC "\nseq 99\nsize 5\n--\nne");
	put_raw(file_b, MAGIC "\nseq 1\nsize 4\n--\nkeep");
	got = newest();
	CHECK(got && !strcmp(got, "keep"));
	free(got);
	wf_slotfile_read(file_a, MAGIC, LIMIT, &s);
	CHECK(!s.whole && !s.header && !s.body);
	/* the next write goes over the cut-short slot, not the whole one */
	CHECK(WF_OK ==
	        wf_slotfile_write(files, MAGIC, NULL, NULL, "after", 5, LIMIT));
	got = newest();
	CHECK(got && !strcmp(got, "after"));
	free(got);
}

/* A write decides which slot to go over from the head of each file and its size:
 * the bodies are not read. A draft is megabytes and is written every few seconds. */
static void a_big_record_is_written_without_reading_the_other_slots(void)
{
	enum
	{
		BODY = 8 << 20,
		BIG = 16 << 20,
		WRITES = 8
	};
	char *body = malloc(BODY), *got;
	clock_t t0;
	double ms;
	WfSlotFile s[2];
	int pick;

	CHECK(body != NULL);
	for (size_t i = 0; i < BODY; i++) {
		body[i] = (char)('a' + i % 23);
	}
	t0 = clock();
	for (int i = 0; i < WRITES; i++) {
		body[0] = (char)('A' + i); /* each record is different */
		CHECK(WF_OK ==
		        wf_slotfile_write(files, MAGIC, "path x\n", "enc 1\n",
		                body, BODY, BIG));
	}
	ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / (double)WRITES;
	printf("info: an 8 MiB record written, both slots whole: %.1f ms a "
	       "write\n",
	        ms);
	wf_slotfile_read(file_a, MAGIC, BIG, &s[0]);
	wf_slotfile_read(file_b, MAGIC, BIG, &s[1]);
	pick = wf_slotfile_pick(&s[0], &s[1]);
	CHECK(pick >= 0 && s[pick].body_n == BODY &&
	        s[pick].body[0] == (char)('A' + WRITES - 1));
	/* the older one is the other record of the two, also whole */
	CHECK(s[0].whole && s[1].whole && s[0].seq != s[1].seq);
	wf_slotfile_free(&s[0]);
	wf_slotfile_free(&s[1]);
	got = NULL;
	free(got);
	free(body);
}

static void other_kinds_and_damage_are_not_records(void)
{
	WfSlotFile s;

	put_raw(file_a, "piero-other 1\nseq 1\nsize 1\n--\nx");
	wf_slotfile_read(file_a, MAGIC, LIMIT, &s);
	CHECK(!s.whole);
	put_raw(file_a, MAGIC "\nsize 1\n--\nx"); /* no seq */
	wf_slotfile_read(file_a, MAGIC, LIMIT, &s);
	CHECK(!s.whole);
	put_raw(file_a, MAGIC "\nseq 1\n--\nx"); /* no size */
	wf_slotfile_read(file_a, MAGIC, LIMIT, &s);
	CHECK(!s.whole);
	put_raw(file_a, MAGIC "\nseq 1\nsize 9\n--\nx"); /* size lies */
	wf_slotfile_read(file_a, MAGIC, LIMIT, &s);
	CHECK(!s.whole);
	/* numbers that are not numbers: a sign, a number too large for 64 bits, the
	 * largest value (it has no next one), a tail, a space, and nothing */
	{
		static const char *const bad[] = {
		        MAGIC "\nseq -1\nsize 1\n--\nx",
		        MAGIC "\nseq 99999999999999999999\nsize 1\n--\nx",
		        MAGIC "\nseq 18446744073709551615\nsize 1\n--\nx",
		        MAGIC "\nseq 5x\nsize 1\n--\nx",
		        MAGIC "\nseq  5\nsize 1\n--\nx",
		        MAGIC "\nseq \nsize 1\n--\nx",
		        MAGIC "\nseq 5\nsize +1\n--\nx",
		};

		for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
			put_raw(file_a, bad[i]);
			wf_slotfile_read(file_a, MAGIC, LIMIT, &s);
			CHECK(!s.whole);
			wf_slotfile_free(&s);
		}
		put_raw(file_a,
		        MAGIC "\nseq 18446744073709551614\nsize 1\n--\nx");
		wf_slotfile_read(file_a, MAGIC, LIMIT, &s);
		CHECK(s.whole && 18446744073709551614ull == s.seq);
		wf_slotfile_free(&s);
	}
	put_raw(file_a, "short");
	wf_slotfile_read(file_a, MAGIC, LIMIT, &s);
	CHECK(!s.whole);
	wf_slotfile_read(L"C:\\no\\such\\folder\\x.a", MAGIC, LIMIT, &s);
	CHECK(!s.whole);
}

static void a_record_over_the_limit_is_refused_and_not_read(void)
{
	char big[LIMIT + 8000];
	WfSlotFile s;

	memset(big, 'x', sizeof big);
	CHECK(WF_E_RANGE ==
	        wf_slotfile_write(
	                files, MAGIC, NULL, NULL, big, LIMIT + 1, LIMIT));
	/* Header allowance must not become extra body capacity, or let an
	 * oversized slot's sequence prevent the next valid write. */
	{
		char text[LIMIT + 128];

		snprintf(text, sizeof text,
		        MAGIC "\nseq 18446744073709551614\nsize %d\n--\n%.*s",
		        LIMIT + 1, LIMIT + 1, big);
		put_raw(file_a, text);
		DeleteFileW(file_b);
		wf_slotfile_read(file_a, MAGIC, LIMIT, &s);
		CHECK(!s.whole && !s.header && !s.body);
		wf_slotfile_free(&s);
		CHECK(WF_OK ==
		        wf_slotfile_write(
		                files, MAGIC, NULL, NULL, big, LIMIT, LIMIT));
		wf_slotfile_read(file_a, MAGIC, LIMIT, &s);
		CHECK(s.whole && s.body_n == LIMIT && s.seq == 1);
		wf_slotfile_free(&s);
	}
	/* a file larger than the reader's limit (plus a header's slack) is not
	 * read at all, whatever it claims */
	{
		char text[LIMIT + 8000 + 64];

		snprintf(text, sizeof text, MAGIC "\nseq 1\nsize %zu\n--\n%.*s",
		        sizeof big, (int)sizeof big, big);
		put_raw(file_a, text);
		wf_slotfile_read(file_a, MAGIC, LIMIT, &s);
		CHECK(!s.whole);
	}
}

/* A slot with the highest number a reader takes: a write has no number after it
 * and refuses, and leaves the slots as they were (never a 0 that the old slot
 * would outrank). A slot with -1 is not a record, so the new data wins. */
static void the_generation_number_never_wraps(void)
{
	char *text;

	put_raw(file_a, MAGIC "\nseq 18446744073709551614\nsize 3\n--\nOLD");
	DeleteFileW(file_b);
	CHECK(WF_E_RANGE ==
	        wf_slotfile_write(files, MAGIC, NULL, NULL, "NEW", 3, LIMIT));
	text = newest();
	CHECK(text && !strcmp(text, "OLD"));
	free(text);
	put_raw(file_a, MAGIC "\nseq -1\nsize 3\n--\nOLD");
	CHECK(WF_OK ==
	        wf_slotfile_write(files, MAGIC, NULL, NULL, "NEW", 3, LIMIT));
	text = newest();
	CHECK(text && !strcmp(text, "NEW"));
	free(text);
}

/* The whole content of a file, or NULL. */
static char *read_all(const wchar_t *file)
{
	HANDLE h = CreateFileW(file, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
	        NULL);
	char *text;
	DWORD got = 0, size;

	if (INVALID_HANDLE_VALUE == h) {
		return NULL;
	}
	size = GetFileSize(h, NULL);
	text = calloc(size + 1, 1);
	if (text) {
		ReadFile(h, text, size, &got, NULL);
	}
	CloseHandle(h);
	return text;
}

/* A slot that is a second name for another file, or a link to one, is never
 * written through: the other file keeps what it had, and the write is refused. */
static void a_slot_that_leads_to_another_file_is_not_written(void)
{
	wchar_t dir[MAX_PATH], target[MAX_PATH + 32], l_a[MAX_PATH + 32],
	        l_b[MAX_PATH + 32];
	const wchar_t *const linked[2] = {l_a, l_b};
	WfStatus st;
	char *got;

	GetTempPathW(MAX_PATH, dir);
	swprintf(target, MAX_PATH + 32, L"%lsslotfile-%lu.target", dir,
	        GetCurrentProcessId());
	swprintf(l_a, MAX_PATH + 32, L"%lsslotfile-%lu.link-a", dir,
	        GetCurrentProcessId());
	swprintf(l_b, MAX_PATH + 32, L"%lsslotfile-%lu.link-b", dir,
	        GetCurrentProcessId());
	DeleteFileW(l_a);
	DeleteFileW(l_b);
	put_raw(target, "PRECIOUS");

	/* a hard link: the slot is a second name of the file */
	if (CreateHardLinkW(l_a, target, NULL)) {
		st = wf_slotfile_write(
		        linked, MAGIC, NULL, NULL, "NEW", 3, LIMIT);
		CHECK(WF_OK != st);
		got = read_all(target);
		CHECK(got && !strcmp(got, "PRECIOUS"));
		free(got);
		DeleteFileW(l_a);
	} else {
		CHECK(0 && "a hard link could not be made");
	}
	/* a symbolic link, where this account may make one */
	if (CreateSymbolicLinkW(l_a, target,
	            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
		st = wf_slotfile_write(
		        linked, MAGIC, NULL, NULL, "NEW", 3, LIMIT);
		CHECK(WF_OK != st);
		got = read_all(target);
		CHECK(got && !strcmp(got, "PRECIOUS"));
		free(got);
		DeleteFileW(l_a);
	} else {
		printf("skipped: no symbolic link could be made here\n");
	}
	/* an ordinary slot is still written, and cut short first */
	put_raw(l_a, "OLD OLD OLD OLD OLD OLD OLD OLD OLD OLD OLD OLD OLD OLD");
	st = wf_slotfile_write(linked, MAGIC, NULL, NULL, "NEW", 3, LIMIT);
	CHECK(WF_OK == st);
	got = read_all(l_a);
	CHECK(got && NULL != strstr(got, "NEW") && NULL == strstr(got, "OLD"));
	free(got);
	DeleteFileW(l_a);
	DeleteFileW(l_b);
	DeleteFileW(target);
}

/* A header that gives seq or size twice is not a record, whichever number comes
 * first: neither the read nor the look a write takes at the slot trusts it. */
static void a_header_that_repeats_a_field_is_not_a_record(void)
{
	static const char *const twice[] = {MAGIC
	        "\n"
	        "seq 99\nseq 1\nsize 3\n--\nOLD",
	        MAGIC "\n"
	              "seq 1\nseq 99\nsize 3\n--\nOLD",
	        MAGIC "\n"
	              "seq 7\nsize 3\nsize 3\n--\nOLD"};
	char *text;

	for (size_t i = 0; i < sizeof twice / sizeof *twice; i++) {
		DeleteFileW(file_b);
		put_raw(file_a, twice[i]);
		text = newest();
		CHECK(NULL == text); /* read: nothing is whole */
		free(text);
		/* write: the slot is taken as not whole, so it is the one written, and
		 * the number goes on from nothing, not from the 99 it claimed */
		CHECK(WF_OK ==
		        wf_slotfile_write(
		                files, MAGIC, NULL, NULL, "NEW", 3, LIMIT));
		text = newest();
		CHECK(text && !strcmp(text, "NEW"));
		free(text);
		{
			WfSlotFile s;

			wf_slotfile_read(file_a, MAGIC, LIMIT, &s);
			CHECK(s.whole && 1 == s.seq);
			wf_slotfile_free(&s);
		}
	}
	/* once each, as written, it is read as before */
	put_raw(file_a,
	        MAGIC "\n"
	              "seq 5\nsize 3\n--\nOLD");
	DeleteFileW(file_b);
	text = newest();
	CHECK(text && !strcmp(text, "OLD"));
	free(text);
}

static void a_write_to_an_unusable_place_fails_and_changes_nothing(void)
{
	const wchar_t *bad[2] = {
	        L"C:\\no\\such\\folder\\x.a", L"C:\\no\\such\\folder\\x.b"};

	CHECK(WF_E_OS ==
	        wf_slotfile_write(bad, MAGIC, NULL, NULL, "x", 1, LIMIT));
}

int main(void)
{
	make_names();
	a_record_round_trips_and_the_newest_wins();
	the_header_lines_are_kept_where_they_were_put();
	a_cut_short_slot_is_ignored_and_the_other_is_kept();
	a_big_record_is_written_without_reading_the_other_slots();
	other_kinds_and_damage_are_not_records();
	a_record_over_the_limit_is_refused_and_not_read();
	the_generation_number_never_wraps();
	a_write_to_an_unusable_place_fails_and_changes_nothing();
	a_header_that_repeats_a_field_is_not_a_record();
	a_slot_that_leads_to_another_file_is_not_written();
	DeleteFileW(file_a);
	DeleteFileW(file_b);
	return TEST_RESULT();
}

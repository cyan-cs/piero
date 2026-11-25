/*
 * The query of the search below a folder (DR-DRAFT-031): words (folded, all of
 * them, quotes), ext:, size> and size< with units, newer: and older: by day (UTC),
 * what is refused and where, and what a folder can match. Pure: names in, a
 * verdict out.
 */
#include <string.h>
#include <wchar.h>

#include "search/wf_query.h"
#include "wf_test.h"

#define DAY_TICKS (86400ull * 10000000ull)
#define UNIX_EPOCH_TICKS 116444736000000000ull /* 1970-01-01 as a FILETIME */

static bool parses(const wchar_t *text, WfQuery *q)
{
	size_t pos = 99999;

	return WF_OK == wf_query_parse(text, q, &pos);
}

static size_t bad_at(const wchar_t *text)
{
	WfQuery q;
	size_t pos = 99999;

	return WF_E_INVAL == wf_query_parse(text, &q, &pos) ? pos : 99999;
}

static bool file(
        const WfQuery *q, const wchar_t *name, uint64_t size, WfFileTime t)
{
	return wf_query_match(q, name, false, size, t);
}

static void words(void)
{
	WfQuery q;

	CHECK(parses(L"", &q) && wf_query_empty(&q));
	CHECK(parses(L"   \t ", &q) && wf_query_empty(&q));
	CHECK(parses(L"report", &q) && !wf_query_empty(&q) && 1 == q.n_word);
	CHECK(file(&q, L"annual_Report.pdf", 1, 1));
	CHECK(file(&q, L"REPORT", 1, 1));
	CHECK(!file(&q, L"rapport.txt", 1, 1));
	/* all of them, in any order */
	CHECK(parses(L"annual  report", &q) && 2 == q.n_word);
	CHECK(file(&q, L"report of the Annual meeting.txt", 1, 1));
	CHECK(!file(&q, L"annual.txt", 1, 1));
	/* quotes keep a space */
	CHECK(parses(L"\"annual report\"", &q) && 1 == q.n_word);
	CHECK(file(&q, L"annual report 2026.txt", 1, 1));
	CHECK(!file(&q, L"annual_report.txt", 1, 1));
	CHECK(parses(L"x \"a b\" y", &q) && 3 == q.n_word);
	/* Japanese, and a word that is only part of a name */
	CHECK(parses(L"日本", &q));
	CHECK(file(&q, L"日本語.txt", 1, 1));
	CHECK(!file(&q, L"語日.txt", 1, 1));
	/* the case of letters beyond ASCII is folded too */
	CHECK(parses(L"Été", &q));
	CHECK(file(&q, L"éTÉ 2026.txt", 1, 1));
	/* a term is a term only at the start of a word */
	CHECK(parses(L"myext:txt", &q) && 1 == q.n_word && !q.n_ext);
	CHECK(parses(L"EXT:Txt", &q) && 1 == q.n_ext);
	/* limits: 8 words, a word of 127 units */
	CHECK(parses(L"a b c d e f g h", &q) && 8 == q.n_word);
	CHECK(16 == bad_at(L"a b c d e f g h i"));
	{
		wchar_t big[200];

		for (int i = 0; i < 128; i++) {
			big[i] = L'x';
		}
		big[128] = 0;
		CHECK(0 == bad_at(big));
		big[127] = 0;
		CHECK(parses(big, &q));
	}
}

static void extensions(void)
{
	WfQuery q;

	CHECK(parses(L"ext:txt", &q) && 1 == q.n_ext);
	CHECK(file(&q, L"a.txt", 1, 1) && file(&q, L"a.TXT", 1, 1));
	CHECK(!file(&q, L"a.txt.bak", 1, 1) && !file(&q, L"txt", 1, 1));
	CHECK(!file(&q, L".txt", 1,
	        1)); /* a dotfile has a name, not an extension */
	CHECK(!file(&q, L"a", 1, 1));
	CHECK(parses(L"ext:.txt", &q) && file(&q, L"a.txt", 1, 1));
	/* any of up to 4, and with a word */
	CHECK(parses(L"ext:jpg ext:PNG", &q) && 2 == q.n_ext);
	CHECK(file(&q, L"x.png", 1, 1) && file(&q, L"y.jpg", 1, 1));
	CHECK(!file(&q, L"z.gif", 1, 1));
	CHECK(parses(L"cat ext:jpg", &q));
	CHECK(file(&q, L"my cat.jpg", 1, 1) && !file(&q, L"my dog.jpg", 1, 1) &&
	        !file(&q, L"my cat.png", 1, 1));
	CHECK(parses(L"ext:a ext:b ext:c ext:d", &q) && 4 == q.n_ext);
	CHECK(24 == bad_at(L"ext:a ext:b ext:c ext:d ext:e"));
	/* nothing after the colon, or too long */
	CHECK(0 == bad_at(L"ext:"));
	CHECK(0 == bad_at(L"ext:."));
	CHECK(0 == bad_at(L"ext:abcdefghijklmnop"));
	CHECK(parses(L"ext:abcdefghijklmno", &q));
}

static void sizes(void)
{
	WfQuery q;

	CHECK(parses(L"size>10k", &q) && q.has_gt && 10240 == q.gt);
	CHECK(!file(&q, L"a", 10240, 1) && file(&q, L"a", 10241, 1) &&
	        !file(&q, L"a", 0, 1));
	CHECK(parses(L"size<2m", &q) && q.has_lt && 2097152 == q.lt);
	CHECK(file(&q, L"a", 2097151, 1) && !file(&q, L"a", 2097152, 1));
	CHECK(parses(L"size>1KiB", &q) && 1024 == q.gt);
	CHECK(parses(L"size>1kb", &q) && 1024 == q.gt);
	CHECK(parses(L"size>5", &q) && 5 == q.gt);
	CHECK(parses(L"size>5b", &q) && 5 == q.gt);
	CHECK(parses(L"size>3G", &q) && 3ull << 30 == q.gt);
	CHECK(parses(L"size>2t", &q) && 2ull << 40 == q.gt);
	CHECK(parses(L"size>1k size<1m", &q) && q.has_gt && q.has_lt);
	CHECK(file(&q, L"a", 5000, 1) && !file(&q, L"a", 500, 1) &&
	        !file(&q, L"a", 5000000, 1));
	/* not a size */
	CHECK(0 == bad_at(L"size>"));
	CHECK(0 == bad_at(L"size>abc"));
	CHECK(0 == bad_at(L"size>1.5k"));
	CHECK(0 == bad_at(L"size>-1"));
	CHECK(0 == bad_at(L"size>10x"));
	CHECK(0 == bad_at(L"size>10kk"));
	CHECK(0 == bad_at(L"size>10kib2"));
	CHECK(0 == bad_at(L"size>5bb"));
	CHECK(0 == bad_at(L"size>99999999999999999999"));
	CHECK(0 == bad_at(L"size>20000000t")); /* does not fit in 64 bits */
	/* twice: the second is the one that is wrong */
	CHECK(8 == bad_at(L"size>1k size>2k"));
	CHECK(8 == bad_at(L"size<1k size<2k"));
	/* a folder has no size to test */
	CHECK(parses(L"size>1k", &q));
	CHECK(!wf_query_match(&q, L"dir", true, 5000, 1));
}

static void days(void)
{
	WfQuery q;

	CHECK(parses(L"newer:1970-01-01", &q) && q.has_newer &&
	        UNIX_EPOCH_TICKS == q.newer);
	CHECK(file(&q, L"a", 1, UNIX_EPOCH_TICKS) &&
	        !file(&q, L"a", 1, UNIX_EPOCH_TICKS - 1));
	CHECK(parses(L"older:1970-01-02", &q) && q.has_older &&
	        UNIX_EPOCH_TICKS + DAY_TICKS == q.older);
	CHECK(file(&q, L"a", 1, UNIX_EPOCH_TICKS + DAY_TICKS - 1) &&
	        !file(&q, L"a", 1, UNIX_EPOCH_TICKS + DAY_TICKS));
	CHECK(parses(L"newer:1970-01-01 older:1970-01-03", &q));
	CHECK(file(&q, L"a", 1, UNIX_EPOCH_TICKS + DAY_TICKS) &&
	        !file(&q, L"a", 1, UNIX_EPOCH_TICKS - 1) &&
	        !file(&q, L"a", 1, UNIX_EPOCH_TICKS + 2 * DAY_TICKS));
	CHECK(parses(L"newer:2024-02-29", &q)); /* a leap day */
	CHECK(0 == bad_at(L"newer:2023-02-29"));
	CHECK(0 == bad_at(L"newer:2026-02-31"));
	CHECK(0 == bad_at(L"newer:2026-13-01"));
	CHECK(0 == bad_at(L"newer:2026-00-10"));
	CHECK(0 == bad_at(L"newer:2026-1-1"));
	CHECK(0 == bad_at(L"newer:20261001"));
	CHECK(0 == bad_at(L"newer:"));
	CHECK(0 == bad_at(L"older:abcd-ef-gh"));
	CHECK(0 == bad_at(L"newer:1600-12-31"));
	CHECK(0 == bad_at(L"newer:10000-01-01"));
	CHECK(17 == bad_at(L"newer:2026-10-01 newer:2026-10-02"));
	/* a folder is dated like a file, and with a word */
	CHECK(parses(L"photos newer:1970-01-01", &q));
	CHECK(wf_query_match(&q, L"Photos", true, 0, UNIX_EPOCH_TICKS));
	CHECK(!wf_query_match(&q, L"Photos", true, 0, 5));
}

static void errors_and_folders(void)
{
	WfQuery q;

	CHECK(2 == bad_at(L"a ext:"));
	CHECK(4 == bad_at(L"a b size>x"));
	CHECK(0 == bad_at(L"\"abc"));
	CHECK(2 == bad_at(L"a \"abc"));
	CHECK(0 == bad_at(L"\"\"")); /* an empty quoted word */
	CHECK(WF_E_INVAL == wf_query_parse(NULL, &q, NULL));
	CHECK(WF_E_INVAL == wf_query_parse(L"a", NULL, NULL));
	/* a folder: words and dates yes, extension and size never */
	CHECK(parses(L"docs", &q) &&
	        wf_query_match(&q, L"My Docs", true, 0, 1));
	CHECK(parses(L"ext:txt", &q) &&
	        !wf_query_match(&q, L"a.txt", true, 0, 1));
	/* only terms, no word: every name that passes them */
	CHECK(parses(L"ext:md", &q) && file(&q, L"README.md", 1, 1));
	/* a very long name is not matched against words, and does not crash */
	{
		wchar_t name[700];

		for (int i = 0; i < 699; i++) {
			name[i] = L'a';
		}
		name[699] = 0;
		CHECK(parses(L"a", &q));
		CHECK(!file(&q, name, 1, 1));
	}
}

int main(void)
{
	words();
	extensions();
	sizes();
	days();
	errors_and_folders();
	return TEST_RESULT();
}

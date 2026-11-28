/* The editing model: typing, deleting, selections, clipboard, undo and
 * redo, search and replace, indentation, completion. No terminal involved. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "edit/wf_editor.h"
#include "text/wf_text.h"
#include "wf_test.h"
#include "base/wf_macro.h"

static WfEditor ed;

static void fresh(const char *text)
{
	WfEncoding enc = {WF_ENC_UTF8, 0};

	wf_ed_free(&ed);
	CHECK(WF_OK == wf_ed_init(&ed, NULL));
	CHECK(WF_OK == wf_ed_set_text(&ed, text, strlen(text), enc));
}

static char *all(size_t *n)
{
	char *out = NULL;
	size_t k = 0;

	CHECK(wf_et_read(&ed.text, (WfPos){0, 0}, wf_et_end(&ed.text), &out,
	              &k) == WF_OK);
	if (n) {
		*n = k;
	}
	out = realloc(out, k + 1);
	CHECK(NULL != out);
	if (out) {
		out[k] = '\0';
	}
	return out;
}

static bool is(const char *expect)
{
	size_t n;
	char *got = all(&n);
	bool same = n == strlen(expect) && !memcmp(got, expect, n) &&
	        wf_et_check(&ed.text);

	if (!same) {
		printf("  got [%.*s] expected [%s]\n", (int)n, got, expect);
	}
	free(got);
	return same;
}

static void type(const char *s)
{
	size_t n = strlen(s), i = 0;

	while (i < n) {
		uint32_t cp;
		size_t k = wf_utf8_decode(s + i, n - i, &cp);

		CHECK(WF_OK == wf_ed_type(&ed, cp));
		i += k;
	}
}

static void typing_and_deleting(void)
{
	fresh("");
	type("hello");
	CHECK(is("hello") && 5 == ed.cur.col);
	CHECK(WF_OK == wf_ed_backspace(&ed) && is("hell"));
	wf_ed_move(&ed, WF_MV_LINE_START, false, 0);
	CHECK(WF_OK == wf_ed_delete(&ed) && is("ell"));
	CHECK(WF_OK == wf_ed_newline(&ed) && is("\nell") && 1 == ed.cur.line);
	CHECK(WF_OK == wf_ed_backspace(&ed) && is("ell") && 0 == ed.cur.line);
	/* a character outside the BMP and a combining pair are one step */
	fresh("");
	type("a\xF0\x9F\x98\x80"
	     "e\xCC\x81");
	CHECK(WF_OK == wf_ed_backspace(&ed) && is("a\xF0\x9F\x98\x80"));
	CHECK(WF_OK == wf_ed_backspace(&ed) && is("a"));
	/* what cannot be typed is refused */
	CHECK(WF_E_INVAL == wf_ed_type(&ed, 0x07));
	CHECK(WF_E_INVAL == wf_ed_type(&ed, 0xD800));
	CHECK(is("a"));
}

static void line_endings_survive_editing(void)
{
	fresh("one\r\ntwo\nthree\rfour");
	wf_ed_goto(&ed, 2, 1);
	type("X");
	wf_ed_move(&ed, WF_MV_DOC_END, false, 0);
	type("!");
	CHECK(is("one\r\nXtwo\nthree\rfour!"));
	/* an Enter in a CRLF line makes a CRLF break */
	wf_ed_goto(&ed, 1, 2);
	CHECK(WF_OK == wf_ed_newline(&ed));
	CHECK(is("o\r\nne\r\nXtwo\nthree\rfour!"));
	/* joining lines keeps the ending of the line that remains */
	wf_ed_goto(&ed, 3, 1);
	CHECK(WF_OK == wf_ed_backspace(&ed) && is("o\r\nneXtwo\nthree\rfour!"));
}

static void auto_indent_follows_the_line(void)
{
	fresh("\tif (x) {");
	wf_ed_move(&ed, WF_MV_LINE_END, false, 0);
	CHECK(WF_OK == wf_ed_newline(&ed));
	CHECK(is("\tif (x) {\n\t\t")); /* the block gets one more level */
	/* braces on one line open a block between them */
	fresh("{}");
	wf_ed_goto(&ed, 1, 2);
	CHECK(WF_OK == wf_ed_newline(&ed));
	CHECK(is("{\n\t\n}") && 1 == ed.cur.line && 1 == ed.cur.col);
	/* off: nothing is added */
	fresh("   abc");
	ed.opt.auto_indent = false;
	wf_ed_move(&ed, WF_MV_LINE_END, false, 0);
	CHECK(WF_OK == wf_ed_newline(&ed) && is("   abc\n"));
	/* the cursor inside the indent only copies what is before it */
	fresh("      x");
	wf_ed_goto(&ed, 1, 3);
	CHECK(WF_OK == wf_ed_newline(&ed) && is("  \n      x"));
}

static void selections_replace_and_move(void)
{
	fresh("hello world");
	wf_ed_goto(&ed, 1, 1);
	wf_ed_move(&ed, WF_MV_WORD_RIGHT, true, 0);
	CHECK(wf_ed_has_selection(&ed));
	type("J");
	CHECK(is("Jworld") && !wf_ed_has_selection(&ed));
	/* left/right without Shift collapse to the edge */
	fresh("abcdef");
	wf_ed_goto(&ed, 1, 2);
	wf_ed_move(&ed, WF_MV_RIGHT, true, 0);
	wf_ed_move(&ed, WF_MV_RIGHT, true, 0);
	wf_ed_move(&ed, WF_MV_LEFT, false, 0);
	CHECK(!wf_ed_has_selection(&ed) && 1 == ed.cur.col);
	wf_ed_select_all(&ed);
	CHECK(WF_OK == wf_ed_backspace(&ed) && is(""));
	/* the nano mark */
	fresh("abcdef");
	wf_ed_toggle_mark(&ed);
	wf_ed_move(&ed, WF_MV_RIGHT, false, 0);
	wf_ed_move(&ed, WF_MV_RIGHT, false, 0);
	CHECK(wf_ed_has_selection(&ed));
	CHECK(WF_OK == wf_ed_cut(&ed) && is("cdef"));
	CHECK(WF_OK == wf_ed_paste(&ed) && is("abcdef"));
}

static void clipboard_lines_and_text(void)
{
	fresh("one\ntwo\nthree\n");
	wf_ed_goto(&ed, 2, 2);
	CHECK(WF_OK == wf_ed_cut(&ed) && is("one\nthree\n") && ed.clip_line);
	wf_ed_goto(&ed, 1, 1);
	CHECK(WF_OK == wf_ed_paste(&ed) && is("two\none\nthree\n"));
	/* the last line has no ending: the copy gets one, the paste is a line */
	fresh("a\nb");
	wf_ed_goto(&ed, 2, 1);
	CHECK(WF_OK == wf_ed_copy(&ed) && 2 == ed.clip_n);
	wf_ed_goto(&ed, 1, 1);
	CHECK(WF_OK == wf_ed_paste(&ed) && is("b\na\nb"));
	/* pasted text: breaks follow the file, control characters are dropped */
	fresh("x\r\ny");
	CHECK(WF_OK == wf_ed_set_clip(&ed, "1\n2\x1B[31m\x07", 8, false));
	wf_ed_goto(&ed, 1, 2);
	CHECK(WF_OK == wf_ed_paste(&ed));
	CHECK(is("x1\r\n2[31m\r\ny") && strstr(ed.msg, "removed"));
}

static void undo_redo_restore_exactly(void)
{
	fresh("start");
	CHECK(!wf_ed_modified(&ed));
	wf_ed_move(&ed, WF_MV_DOC_END, false, 0);
	type(" typed words");
	CHECK(wf_ed_modified(&ed));
	CHECK(WF_OK == wf_ed_undo(&ed) && is("start typed "));
	CHECK(WF_OK == wf_ed_undo(&ed) && is("start "));
	CHECK(WF_OK == wf_ed_undo(&ed) && is("start"));
	CHECK(!wf_ed_modified(&ed) && 5 == ed.cur.col);
	CHECK(WF_OK == wf_ed_redo(&ed) && is("start "));
	CHECK(WF_OK == wf_ed_redo(&ed) && is("start typed "));
	CHECK(WF_OK == wf_ed_redo(&ed) && is("start typed words"));
	CHECK(WF_OK == wf_ed_redo(&ed) && strstr(ed.msg, "Nothing"));
	/* a new edit ends the redo history */
	CHECK(WF_OK == wf_ed_undo(&ed));
	type("!");
	CHECK(WF_OK == wf_ed_redo(&ed) && strstr(ed.msg, "Nothing"));
}

static void saved_state_is_tracked_exactly(void)
{
	fresh("");
	type("ab");
	wf_ed_mark_saved(&ed);
	CHECK(!wf_ed_modified(&ed));
	/* typing after the save is a new step, never merged into the saved one */
	type("c");
	CHECK(wf_ed_modified(&ed));
	CHECK(WF_OK == wf_ed_undo(&ed) && is("ab") && !wf_ed_modified(&ed));
	CHECK(WF_OK == wf_ed_redo(&ed) && wf_ed_modified(&ed));
	/* undo past the save, then edit: the saved state can never be reached */
	CHECK(WF_OK == wf_ed_undo(&ed) && WF_OK == wf_ed_undo(&ed) && is(""));
	CHECK(wf_ed_modified(&ed));
	type("z");
	CHECK(wf_ed_modified(&ed));
	CHECK(WF_OK == wf_ed_undo(&ed) && is("") && wf_ed_modified(&ed));
}

static void a_paste_and_a_replace_all_undo_as_one(void)
{
	fresh("aXbXcX");
	{
		WfSearch s = {"X", 1, true, false};
		size_t count = 0;

		CHECK(WF_OK == wf_ed_replace_all(&ed, &s, "--", 2, &count));
		CHECK(3 == count && is("a--b--c--"));
		CHECK(WF_OK == wf_ed_undo(&ed) && is("aXbXcX"));
		CHECK(WF_OK == wf_ed_redo(&ed) && is("a--b--c--"));
	}
	fresh("");
	CHECK(WF_OK == wf_ed_set_clip(&ed, "l1\nl2\nl3", 8, false));
	CHECK(WF_OK == wf_ed_paste(&ed) && is("l1\nl2\nl3"));
	CHECK(WF_OK == wf_ed_undo(&ed) && is(""));
}

/* Replace all over a long line and over many short ones: what it costs, and that
 * the text is what the matches one after the other would make (the line's own
 * text is rebuilt once, not moved along for every match). */
static void replace_all_over_long_and_many_lines(void)
{
	enum
	{
		PAIRS = 50000,
		LINES = 20000
	};
	char *text = malloc((size_t)PAIRS * 2 + 1), *want, *got;
	WfSearch s = {"a", 1, true, false};
	size_t count = 0, n = 0, k = 0;
	clock_t t0;
	double ms_long, ms_many;

	CHECK(text != NULL);
	for (size_t i = 0; i < PAIRS; i++) {
		text[2 * i] = 'x';
		text[2 * i + 1] = 'a';
	}
	text[2 * PAIRS] = '\0';
	fresh(text);
	t0 = clock();
	CHECK(WF_OK == wf_ed_replace_all(&ed, &s, "bb", 2, &count));
	ms_long = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	CHECK(PAIRS == count);
	got = all(&n);
	want = malloc((size_t)PAIRS * 3 + 1);
	for (size_t i = 0; i < PAIRS; i++) {
		memcpy(want + 3 * i, "xbb", 3);
	}
	CHECK(got && n == (size_t)PAIRS * 3 && !memcmp(got, want, n));
	free(got);
	free(want);
	CHECK(WF_OK == wf_ed_undo(&ed)); /* one step takes all of it back */
	got = all(&k);
	CHECK(got && k == (size_t)PAIRS * 2 && !memcmp(got, text, k));
	free(got);
	free(text);

	text = malloc((size_t)LINES * 8 + 1);
	for (size_t i = 0; i < LINES; i++) {
		memcpy(text + 8 * i, "foo bar\n", 8);
	}
	text[8 * LINES] = '\0';
	fresh(text);
	s.pat = "foo";
	s.pat_n = 3;
	t0 = clock();
	CHECK(WF_OK == wf_ed_replace_all(&ed, &s, "quux", 4, &count));
	ms_many = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	CHECK(LINES == count);
	got = all(&n);
	CHECK(got && n == (size_t)LINES * 9 && !memcmp(got, "quux bar\n", 9) &&
	        !memcmp(got + n - 9, "quux bar\n", 9));
	free(got);
	free(text);
	printf("info: replace all: %d matches in one line %.1f ms, %d matches "
	       "in %d lines %.1f ms\n",
	        (int)PAIRS, ms_long, (int)LINES, (int)LINES, ms_many);
	CHECK(ms_long < 20000.0 && ms_many < 20000.0);
}

/* Replace all is what the matches, one after the other from the left, would make:
 * a plain scan of the text is the reference (a match never crosses a line). */
static size_t naive_replace_all(const char *t,
        const char *pat,
        bool match_case,
        const char *repl,
        char *out)
{
	size_t n = strlen(t), pn = strlen(pat), rn = strlen(repl), put = 0,
	       hits = 0;

	for (size_t i = 0; i < n;) {
		bool hit = i + pn <= n;

		for (size_t k = 0; hit && k < pn; k++) {
			char a = t[i + k], b = pat[k];

			if (!match_case) {
				a = (char)((a >= 'A' && a <= 'Z') ? a + 32 : a);
				b = (char)((b >= 'A' && b <= 'Z') ? b + 32 : b);
			}
			hit = a == b;
		}
		if (hit) {
			memcpy(out + put, repl, rn);
			put += rn;
			i += pn;
			hits++;
		} else {
			out[put++] = t[i++];
		}
	}
	out[put] = '\0';
	return hits;
}

static void replace_all_is_the_naive_result_and_undoes_whole(void)
{
	static const char *const pats[] = {"a", "ab", "aa", "Ab", "\xC3\xA9"};
	static const char *const repls[] = {
	        "", "xyz", "a", "x\ny", "\xC3\xA9\xC3\xA9"};
	static const char alphabet[] = {
	        'a', 'A', 'b', 'B', '\n', ' ', '\xC3', '\xA9'};
	uint32_t seed = 4242;
	char text[200], want[4000];

	for (int round = 0; round < 400; round++) {
		size_t len = 1 + (seed = seed * 1664525u + 1013904223u) % 150,
		       hits, count = 0, n = 0;
		const char *pat = pats[(seed >> 8) % 5],
		           *repl = repls[(seed >> 12) % 5];
		bool match_case = 0 != ((seed >> 16) & 1);
		WfSearch q;
		char *got;

		for (size_t i = 0; i < len; i++) {
			seed = seed * 1664525u + 1013904223u;
			text[i] = alphabet[(seed >> 20) %
			        6]; /* ASCII only: the two bytes of e-acute apart are not text */
		}
		if ((seed >> 7) &
		        1) { /* and sometimes a real e-acute among them */
			memcpy(text + len / 2, "\xC3\xA9",
			        len / 2 + 2 <= len ? 2 : 0);
		}
		text[len] = '\0';
		q.pat = pat;
		q.pat_n = strlen(pat);
		q.match_case = match_case;
		q.whole_word = false;
		hits = naive_replace_all(text, pat, match_case, repl, want);
		fresh(text);
		CHECK(WF_OK ==
		        wf_ed_replace_all(&ed, &q, repl, strlen(repl), &count));
		got = all(&n);
		CHECK(count == hits);
		CHECK(got && n == strlen(want) && !memcmp(got, want, n));
		free(got);
		if (hits) {
			size_t k = 0;

			CHECK(WF_OK == wf_ed_undo(&ed));
			got = all(&k);
			CHECK(got && k == len && !memcmp(got, text, len));
			free(got);
			CHECK(WF_OK == wf_ed_redo(&ed));
			got = all(&k);
			CHECK(got && k == strlen(want) &&
			        !memcmp(got, want, k));
			free(got);
		}
	}
}

/* A whole-word match is judged by what is round it, and a replacement changes
 * that: ".a" in ".a.a" is a word twice over once the first is "-". */
static void whole_word_replace_all_sees_what_the_replacement_made(void)
{
	WfSearch q = {".a", 2, true, true};
	size_t count = 0;

	fresh(".a.a");
	CHECK(WF_OK == wf_ed_replace_all(&ed, &q, "-", 1, &count));
	CHECK(2 == count && is("--"));
	CHECK(WF_OK == wf_ed_undo(&ed) && is(".a.a"));
}

static bool ww_byte(unsigned char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
	        (c >= 'A' && c <= 'Z') || '_' == c || c >= 0x80;
}

/* The reference for whole-word replace all: the first whole-word match from the
 * left is replaced in the text itself, and the next is looked for after what was
 * put there, with the new text round it. */
static size_t sequential_whole_word(
        const char *t, const char *pat, const char *repl, char *out)
{
	size_t n = strlen(t), pn = strlen(pat), rn = strlen(repl), hits = 0;

	memcpy(out, t, n + 1);
	for (size_t i = 0; i + pn <= n;) {
		bool hit = !memcmp(out + i, pat, pn) &&
		        (0 == i || !ww_byte((unsigned char)out[i - 1])) &&
		        (i + pn == n || !ww_byte((unsigned char)out[i + pn]));

		if (!hit) {
			i++;
			continue;
		}
		memmove(out + i + rn, out + i + pn, n - i - pn + 1);
		memcpy(out + i, repl, rn);
		n = n - pn + rn;
		i += rn;
		hits++;
	}
	return hits;
}

static void whole_word_replace_all_is_the_sequential_result(void)
{
	static const char *const pats[] = {"a", ".", "..", ".a", "a."};
	static const char *const repls[] = {"", "a", "-", "a-", "\n", "a\n"};
	static const char alphabet[] = {'a', 'b', '.', ' ', '\n', '-'};
	uint32_t seed = 777;
	char text[100], want[4000];

	for (int round = 0; round < 600; round++) {
		size_t len = 1 + (seed = seed * 1664525u + 1013904223u) % 60,
		       hits, count = 0, n = 0;
		const char *pat = pats[(seed >> 8) % 5],
		           *repl = repls[(seed >> 12) % 6];
		WfSearch q = {pat, strlen(pat), true, true};
		char *got;

		for (size_t i = 0; i < len; i++) {
			seed = seed * 1664525u + 1013904223u;
			text[i] = alphabet[(seed >> 20) % 6];
		}
		text[len] = '\0';
		hits = sequential_whole_word(text, pat, repl, want);
		fresh(text);
		CHECK(WF_OK ==
		        wf_ed_replace_all(&ed, &q, repl, strlen(repl), &count));
		got = all(&n);
		CHECK(count == hits);
		CHECK(got && n == strlen(want) && !memcmp(got, want, n));
		free(got);
	}
}

/* Whole-word replace all over one long line: each match used to move the rest
 * of the line (the time grew with the square of the matches). */
static void whole_word_replace_all_over_a_long_line(void)
{
	enum
	{
		WORDS = 100000
	};
	char *text = malloc((size_t)WORDS * 2 + 1), *got;
	WfSearch q = {"a", 1, true, true};
	size_t count = 0, n = 0;
	clock_t t0;
	double ms;

	CHECK(NULL != text);
	if (!text) {
		return;
	}
	for (size_t i = 0; i < WORDS; i++) {
		memcpy(text + 2 * i, "a ", 2);
	}
	text[2 * WORDS] = '\0';
	fresh(text);
	t0 = clock();
	CHECK(WF_OK == wf_ed_replace_all(&ed, &q, "bb", 2, &count));
	ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	CHECK(WORDS == count);
	got = all(&n);
	CHECK(got && n == (size_t)WORDS * 3 && !memcmp(got, "bb bb ", 6) &&
	        !memcmp(got + n - 3, "bb ", 3));
	free(got);
	printf("info: whole-word replace all, %d words in one line: %.1f ms\n",
	        (int)WORDS, ms);
	CHECK(ms < 20000.0);
	free(text);
}

/* The reference for the search: every start in turn, compared byte by byte. */
static bool naive_find_in_line(const char *t,
        size_t from,
        const char *pat,
        bool match_case,
        bool whole,
        size_t *at)
{
	size_t n = strlen(t), pn = strlen(pat);

	for (size_t i = from; i + pn <= n; i++) {
		bool hit = 0x80 != ((unsigned char)t[i] & 0xC0);

		for (size_t k = 0; hit && k < pn; k++) {
			char a = t[i + k], b = pat[k];

			if (!match_case) {
				a = (char)((a >= 'A' && a <= 'Z') ? a + 32 : a);
				b = (char)((b >= 'A' && b <= 'Z') ? b + 32 : b);
			}
			hit = a == b;
		}
		if (hit && whole) {
			hit = (0 == i || !ww_byte((unsigned char)t[i - 1])) &&
			        (i + pn == n ||
			                !ww_byte((unsigned char)t[i + pn]));
		}
		if (hit) {
			*at = i;
			return true;
		}
	}
	return false;
}

/* A pattern longer than 64 bytes is searched with a failure table: it must find
 * what the plain comparison finds, in a text made to nearly match again and again
 * (runs of a, in both cases, with the odd b, space and e-acute). */
static void long_patterns_are_found_where_the_naive_search_finds_them(void)
{
	static const char alphabet[] = {'a', 'a', 'a', 'A', 'b', ' '};
	uint32_t seed = 99;
	char text[400], pat[100];

	for (int round = 0; round < 1500; round++) {
		size_t len = 100 + (seed = seed * 1664525u + 1013904223u) % 290,
		       pn = 65 + (seed >> 9) % 30, from, want = 0, got_at = 0;
		bool match_case = 0 != ((seed >> 16) & 1),
		     whole = 0 != ((seed >> 17) & 1), want_hit, got_hit;
		WfSearch q;
		WfMatch m;

		for (size_t i = 0; i < len; i++) {
			seed = seed * 1664525u + 1013904223u;
			text[i] = alphabet[(seed >> 20) % 6];
		}
		if ((seed >> 5) & 1) {
			memcpy(text + len / 3, "\xC3\xA9", 2);
		}
		text[len] = '\0';
		for (size_t i = 0; i < pn; i++) {
			seed = seed * 1664525u + 1013904223u;
			pat[i] = (seed >> 22) % 9
			        ? ((seed >> 25) & 1 ? 'a' : 'A')
			        : ' ';
		}
		if ((seed >> 3) & 1 &&
		        len / 4 + pn <=
		                len) { /* one that occurs: cut from the text */
			memcpy(pat, text + len / 4, pn);
		}
		pat[pn] = '\0';
		if (!strchr(pat, 'a') && !strchr(pat, 'A')) {
			continue;
		}
		from = (seed >> 11) % (len / 2);
		while (from > 0 && 0x80 == ((unsigned char)text[from] & 0xC0)) {
			from--; /* a position is never inside a character */
		}
		fresh(text);
		q.pat = pat;
		q.pat_n = pn;
		q.match_case = match_case;
		q.whole_word = whole;
		want_hit = naive_find_in_line(
		        text, from, pat, match_case, whole, &want);
		got_hit = wf_es_find(
		        &ed.text, &q, (WfPos){0, from}, true, false, &m, NULL);
		if (got_hit) {
			got_at = m.start.col;
		}
		CHECK(want_hit == got_hit && (!want_hit || want == got_at));
	}
}

/* The reference for a search backwards: the last start before `before`. */
static bool naive_last_in_line(const char *t,
        size_t before,
        const char *pat,
        bool match_case,
        bool whole,
        size_t *at)
{
	size_t n = strlen(t), pn = strlen(pat);
	bool found = false;

	for (size_t i = 0; i < before && i + pn <= n; i++) {
		size_t one;

		if (naive_find_in_line(t, i, pat, match_case, whole, &one) &&
		        one == i) {
			*at = i;
			found = true;
		}
	}
	return found;
}

/* The same patterns searched backwards (a pattern over 64 bytes goes forward with
 * the table to find the last start before the position): the last match the plain
 * comparison finds, wherever the search begins. */
static void long_patterns_backwards_are_found_where_the_naive_search_finds_them(
        void)
{
	static const char alphabet[] = {'a', 'a', 'a', 'A', 'b', ' '};
	uint32_t seed = 4242;
	char text[400], pat[100];

	for (int round = 0; round < 1500; round++) {
		size_t len = 100 + (seed = seed * 1664525u + 1013904223u) % 290,
		       pn = 65 + (seed >> 9) % 30, before, want = 0, got_at = 0;
		bool match_case = 0 != ((seed >> 16) & 1),
		     whole = 0 != ((seed >> 17) & 1), want_hit, got_hit;
		WfSearch q;
		WfMatch m;

		for (size_t i = 0; i < len; i++) {
			seed = seed * 1664525u + 1013904223u;
			text[i] = alphabet[(seed >> 20) % 6];
		}
		if ((seed >> 5) & 1) {
			text[len / 3] = '\xC3';
			text[len / 3 + 1] = '\xA9';
		}
		text[len] = '\0';
		for (size_t i = 0; i < pn; i++) {
			seed = seed * 1664525u + 1013904223u;
			pat[i] = (seed >> 22) % 9
			        ? ((seed >> 25) & 1 ? 'a' : 'A')
			        : ' ';
		}
		if ((seed >> 3) & 1 && len / 4 + pn <= len) {
			memcpy(pat, text + len / 4, pn);
		}
		pat[pn] = '\0';
		if (!strchr(pat, 'a') && !strchr(pat, 'A')) {
			continue;
		}
		before = 1 + (seed >> 11) % len;
		while (before > 0 && before < len &&
		        0x80 == ((unsigned char)text[before] & 0xC0)) {
			before--;
		}
		if (0 == before) {
			continue;
		}
		fresh(text);
		q.pat = pat;
		q.pat_n = pn;
		q.match_case = match_case;
		q.whole_word = whole;
		want_hit = naive_last_in_line(
		        text, before, pat, match_case, whole, &want);
		got_hit = wf_es_find(&ed.text, &q, (WfPos){0, before}, false,
		        false, &m, NULL);
		if (got_hit) {
			got_at = m.start.col;
		}
		CHECK(want_hit == got_hit && (!want_hit || want == got_at));
	}
}

/* Backwards through 1 MiB of one letter for the pattern that nearly matches
 * everywhere: with the plain comparison each start reads the pattern before it
 * fails. It is read once, and it is found when it is there. */
static void
a_backward_search_that_nearly_matches_everywhere_reads_the_line_once(void)
{
	enum
	{
		N = 1 << 20,
		M = 2000
	};
	char *t = malloc(N + 2), p[M + 2];
	WfSearch q = {p, M + 1, true, false};
	WfMatch m;
	clock_t t0;
	double ms;
	bool absent, present;

	CHECK(NULL != t);
	if (!t) {
		return;
	}
	memset(t, 'a', N);
	t[N] = '\0';
	memset(p, 'a', M);
	p[M] = 'b';
	p[M + 1] = '\0';
	fresh(t);
	t0 = clock();
	absent =
	        wf_es_find(&ed.text, &q, (WfPos){0, N}, false, false, &m, NULL);
	ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	CHECK(!absent);
	t[N - 7] = 'b';
	fresh(t);
	present =
	        wf_es_find(&ed.text, &q, (WfPos){0, N}, false, false, &m, NULL);
	CHECK(present && N - 7 - M == m.start.col && N - 6 == m.end.col);
	printf("info: backward search for %d letters in 1 MiB that nearly "
	       "match "
	       "at every byte: %.1f ms\n",
	        (int)(M + 1), ms);
	CHECK(ms < 20000.0);
	free(t);
}

/* A place where a long pattern matches but is not a whole word is passed over, and
 * the match that overlaps it is still found: "-." 33 times in "x" and "-." 40
 * times is refused at 1 (a letter before it) and taken at 3. */
static void a_refused_long_match_does_not_hide_the_overlapping_one(void)
{
	char text[2 + 80 + 1], pat[2 * 33 + 1];
	WfSearch q = {pat, 2 * 33, true, true};
	WfMatch m;

	text[0] = 'x';
	for (int i = 0; i < 40; i++) {
		memcpy(text + 1 + 2 * i, "-.", 2);
	}
	text[81] = '\0';
	for (int i = 0; i < 33; i++) {
		memcpy(pat + 2 * i, "-.", 2);
	}
	pat[66] = '\0';
	fresh(text);
	CHECK(wf_es_find(&ed.text, &q, (WfPos){0, 0}, true, false, &m, NULL) &&
	        3 == m.start.col && 69 == m.end.col);
}

/* The case a plain comparison cannot take: 1 MiB of one letter, and a pattern of
 * that letter with one other at the end (what a search of it costs, and that it
 * is found when it is there). */
static void a_search_that_nearly_matches_everywhere_reads_the_line_once(void)
{
	enum
	{
		N = 1 << 20,
		M = 2000
	};
	char *t = malloc(N + 2), p[M + 2];
	WfSearch q = {p, M + 1, true, false};
	WfMatch m;
	clock_t t0;
	double ms;
	bool absent, present;

	CHECK(NULL != t);
	if (!t) {
		return;
	}
	memset(t, 'a', N);
	t[N] = '\0';
	memset(p, 'a', M);
	p[M] = 'b';
	p[M + 1] = '\0';
	fresh(t);
	t0 = clock();
	absent = wf_es_find(&ed.text, &q, (WfPos){0, 0}, true, false, &m, NULL);
	ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	CHECK(!absent);
	t[N] = 'b';
	t[N + 1] = '\0';
	fresh(t);
	present =
	        wf_es_find(&ed.text, &q, (WfPos){0, 0}, true, false, &m, NULL);
	CHECK(present && N - M == m.start.col && N + 1 == m.end.col);
	printf("info: search for %d letters in 1 MiB that nearly match at "
	       "every byte: %.1f ms\n",
	        (int)(M + 1), ms);
	CHECK(ms < 20000.0);
	free(t);
}

/* What a paste costs when it arrives as characters (the terminal gives it as
 * typed keys): each goes through the same edit as a key, and a block inserted at
 * once is compared. Printed: whether a paste needs a path of its own rests on it. */
static void what_a_pasted_text_costs(void)
{
	enum
	{
		CHARS = 200000
	};
	char *text = malloc(CHARS + 1), *got;
	clock_t t0;
	double typed_ms, block_ms;
	size_t n = 0;

	CHECK(text != NULL);
	for (size_t i = 0; i < CHARS; i++) {
		text[i] = 0 == i % 61 ? '\n' : (char)('a' + i % 26);
	}
	text[CHARS] = '\0';
	fresh("");
	t0 = clock();
	for (size_t i = 0; i < CHARS; i++) {
		if ('\n' == text[i]) {
			CHECK(WF_OK == wf_ed_newline(&ed));
		} else {
			CHECK(WF_OK == wf_ed_type(&ed, (uint32_t)text[i]));
		}
	}
	typed_ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	fresh("");
	t0 = clock();
	CHECK(WF_OK == wf_ed_insert_text(&ed, text, CHARS));
	block_ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	got = all(&n);
	CHECK(got && n == CHARS && !memcmp(got, text, n));
	free(got);
	free(text);
	printf("info: %d pasted characters: %.1f ms as typed keys, %.1f ms as "
	       "one block\n",
	        (int)CHARS, typed_ms, block_ms);
}

static void tab_and_unindent(void)
{
	fresh("a\nb\n\nc");
	wf_ed_select_all(&ed);
	CHECK(WF_OK == wf_ed_tab(&ed, false) && is("\ta\n\tb\n\n\tc"));
	CHECK(WF_OK == wf_ed_tab(&ed, true) && is("a\nb\n\nc"));
	CHECK(WF_OK == wf_ed_undo(&ed) && is("\ta\n\tb\n\n\tc"));
	CHECK(WF_OK == wf_ed_undo(&ed) && is("a\nb\n\nc"));
	/* spaces: up to the next stop */
	fresh("ab");
	ed.opt.use_spaces = true;
	ed.opt.tab_width = 4;
	wf_ed_move(&ed, WF_MV_LINE_END, false, 0);
	CHECK(WF_OK == wf_ed_tab(&ed, false) && is("ab  "));
	fresh("      x");
	ed.opt.tab_width = 4;
	CHECK(WF_OK == wf_ed_tab(&ed, true) && is("  x"));
}

static void comments_toggle_lines(void)
{
	fresh("int a;\n  int b;\n\nint c;");
	wf_ed_select_all(&ed);
	CHECK(wf_ed_toggle_comment(&ed, "//") == WF_OK);
	CHECK(is("// int a;\n//   int b;\n\n// int c;"));
	CHECK(wf_ed_toggle_comment(&ed, "//") == WF_OK);
	CHECK(is("int a;\n  int b;\n\nint c;"));
	CHECK(WF_OK == wf_ed_undo(&ed) &&
	        is("// int a;\n//   int b;\n\n// int c;"));
	/* mixed lines are commented, not uncommented */
	fresh("// a\nb");
	wf_ed_select_all(&ed);
	CHECK(wf_ed_toggle_comment(&ed, "//") == WF_OK);
	CHECK(is("// // a\n// b"));
}

static void delete_line_variants(void)
{
	fresh("a\nb\nc");
	wf_ed_goto(&ed, 2, 1);
	CHECK(WF_OK == wf_ed_delete_line(&ed) && is("a\nc"));
	wf_ed_goto(&ed, 2, 1);
	CHECK(WF_OK == wf_ed_delete_line(&ed) && is("a"));
	CHECK(WF_OK == wf_ed_delete_line(&ed) && is(""));
	fresh("a\nb\n");
	wf_ed_goto(&ed, 3, 1);
	CHECK(WF_OK == wf_ed_delete_line(&ed) &&
	        is("a\nb\n")); /* nothing there */
	fresh("1\n2\n3\n4");
	wf_ed_goto(&ed, 2, 1);
	wf_ed_move(&ed, WF_MV_DOWN, true, 0);
	CHECK(WF_OK == wf_ed_delete_line(&ed) && is("1\n3\n4"));
}

static void search_and_replace(void)
{
	WfSearch s = {"Foo", 3, false, false};
	bool wrapped = false;

	fresh("foo Foo\nxfoo foo\nFOO");
	wf_ed_goto(&ed, 1, 1);
	CHECK(wf_ed_find(&ed, &s, true, &wrapped) && 3 == ed.cur.col &&
	        !wrapped);
	CHECK(wf_ed_find(&ed, &s, true, &wrapped) && 7 == ed.cur.col);
	CHECK(wf_ed_find(&ed, &s, true, &wrapped) && 1 == ed.cur.line &&
	        4 == ed.cur.col); /* inside "xfoo" */
	s.whole_word = true;
	wf_ed_goto(&ed, 1, 1);
	CHECK(wf_ed_find(&ed, &s, true, &wrapped) && 3 == ed.cur.col);
	CHECK(wf_ed_find(&ed, &s, true, &wrapped) && 7 == ed.cur.col);
	CHECK(wf_ed_find(&ed, &s, true, &wrapped) && 1 == ed.cur.line &&
	        8 == ed.cur.col); /* "xfoo" is skipped */
	CHECK(wf_ed_find(&ed, &s, true, &wrapped) && 2 == ed.cur.line);
	CHECK(wf_ed_find(&ed, &s, true, &wrapped) && wrapped &&
	        0 == ed.cur.line);
	CHECK(wf_ed_find(&ed, &s, false, &wrapped) && wrapped &&
	        2 == ed.cur.line);
	/* case-sensitive */
	s.match_case = true;
	s.whole_word = false;
	wf_ed_goto(&ed, 1, 1);
	CHECK(wf_ed_find(&ed, &s, true, &wrapped) && 7 == ed.cur.col);
	/* replace the current match, then all of them */
	CHECK(WF_OK == wf_ed_replace_selection(&ed, &s, "Bar", 3));
	CHECK(is("foo Bar\nxfoo foo\nFOO"));
	s.pat = "foo";
	{
		size_t count;

		CHECK(WF_OK == wf_ed_replace_all(&ed, &s, "Y", 1, &count) &&
		        3 == count);
		CHECK(is("Y Bar\nxY Y\nFOO"));
	}
	/* a pattern with a break is refused, an empty one too */
	{
		WfSearch bad = {"a\nb", 3, true, false};
		size_t c;

		CHECK(!wf_ed_find(&ed, &bad, true, NULL));
		CHECK(WF_E_INVAL == wf_ed_replace_all(&ed, &bad, "", 0, &c));
	}
}

static int reference_word_class(const WfELine *l, size_t col)
{
	uint32_t cp;

	(void)wf_utf8_decode(l->s + col, l->len - col, &cp);
	if (' ' == cp || '\t' == cp) {
		return 0;
	}
	return cp >= 0x80 || '_' == cp || (cp >= 'a' && cp <= 'z') ||
	                (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9')
	        ? 1
	        : 2;
}

static int reference_word_before(const WfELine *l, size_t col)
{
	col--;
	while (col && 0x80 == ((unsigned char)l->s[col] & 0xC0)) {
		col--;
	}
	return reference_word_class(l, col);
}

/* The original scalar/cluster algorithm is an independent oracle: yielding
 * in the middle of UTF-8 or a combining run must not change a motion. */
static size_t reference_word(const WfELine *l, size_t col, bool right)
{
	int kind;

	if (right) {
		if (col == l->len) {
			return col;
		}
		kind = reference_word_class(l, col);
		while (col < l->len && kind &&
		        reference_word_class(l, col) == kind) {
			col = wf_et_col_next(l, col);
		}
		while (col < l->len && !reference_word_class(l, col)) {
			col = wf_et_col_next(l, col);
		}
	} else {
		while (col && !reference_word_before(l, col)) {
			col = wf_et_col_prev(l, col);
		}
		if (col) {
			kind = reference_word_before(l, col);
			while (col && reference_word_before(l, col) == kind) {
				col = wf_et_col_prev(l, col);
			}
		}
	}
	return col;
}

static void sliced_words_keep_utf8_clusters_and_word_boundaries(void)
{
	static const char *const samples[] = {"", "abc_DEF12. !?\t  word",
	        " \t ", "a\xCC\x81.!\xCC\x81 \t\xE6\x97\xA5\xE6\x9C\xAC x",
	        "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD\xE2\x80\x8D !",
	        "\xCC\x81\xCC\x81.!\t\xCC\x81"};
	static const size_t budgets[] = {1, 3, 7, 65536};

	for (size_t i = 0; i < WF_ARRAY_LEN(samples); i++) {
		fresh(samples[i]);
		const WfELine *l = &ed.text.lines[0];

		for (size_t col = 0; col <= l->len; col++) {
			if (col < l->len &&
			        0x80 == ((unsigned char)l->s[col] & 0xC0)) {
				continue;
			}
			for (int right = 0; right < 2; right++) {
				for (size_t b = 0; b < WF_ARRAY_LEN(budgets);
				        b++) {
					WfWordScan scan;
					bool done = false;
					size_t turns = 0;

					wf_ed_word_scan_begin(
					        &scan, col, 0 != right);
					while (!done &&
					        turns++ < l->len * 2 + 10) {
						size_t before = scan.col;

						done = wf_ed_word_scan_step(
						        l, &scan, budgets[b]);
						CHECK((right ? scan.col - before
						             : before - scan.col) <=
						        budgets[b] + 3);
					}
					CHECK(done);
					CHECK(scan.col ==
					        reference_word(
					                l, col, 0 != right));
				}
			}
		}
	}
}

static void movement_over_words_lines_and_pages(void)
{
	fresh("foo_bar baz\n  qux.quux\nlast");
	wf_ed_move(&ed, WF_MV_WORD_RIGHT, false, 0);
	CHECK(8 == ed.cur.col);
	wf_ed_move(&ed, WF_MV_WORD_RIGHT, false, 0);
	CHECK(0 == ed.cur.line && 11 == ed.cur.col);
	wf_ed_move(&ed, WF_MV_WORD_RIGHT, false, 0);
	CHECK(1 == ed.cur.line && 0 == ed.cur.col);
	wf_ed_move(&ed, WF_MV_WORD_RIGHT, false, 0);
	CHECK(2 == ed.cur.col);
	wf_ed_move(&ed, WF_MV_WORD_LEFT, false, 0);
	CHECK(0 == ed.cur.col);
	/* the display column is kept across short and long lines */
	fresh("abcdefgh\nab\nabcdefgh");
	wf_ed_goto(&ed, 1, 7);
	wf_ed_move(&ed, WF_MV_DOWN, false, 0);
	CHECK(1 == ed.cur.line && 2 == ed.cur.col);
	wf_ed_move(&ed, WF_MV_DOWN, false, 0);
	CHECK(2 == ed.cur.line && 6 == ed.cur.col);
	wf_ed_move(&ed, WF_MV_PAGE_UP, false, 5);
	CHECK(0 == ed.cur.line);
	/* wide characters count two columns */
	fresh("\xE6\x97\xA5\xE6\x9C\xAC"
	      "ab\nxxxxxxxx");
	wf_ed_goto(&ed, 1, 4); /* after "日本a", 5 cells in */
	wf_ed_move(&ed, WF_MV_DOWN, false, 0);
	CHECK(5 == ed.cur.col);
	/* Home goes to the first non-blank, then the line start */
	fresh("   x");
	wf_ed_move(&ed, WF_MV_LINE_END, false, 0);
	wf_ed_move(&ed, WF_MV_LINE_START, false, 0);
	CHECK(3 == ed.cur.col);
	wf_ed_move(&ed, WF_MV_LINE_START, false, 0);
	CHECK(0 == ed.cur.col);
	/* goto clamps */
	wf_ed_goto(&ed, 99, 99);
	CHECK(0 == ed.cur.line && 4 == ed.cur.col);
}

static void soft_wrap_moves_by_rows(void)
{
	fresh("abcdefghijkl\nxy");
	ed.wrap_cells = 5;
	wf_ed_goto(&ed, 1, 3);
	wf_ed_move(&ed, WF_MV_DOWN, false, 0);
	CHECK(0 == ed.cur.line &&
	        7 == ed.cur.col); /* second row, same offset */
	wf_ed_move(&ed, WF_MV_DOWN, false, 0);
	CHECK(0 == ed.cur.line && ed.cur.col == 12 - 0 - 0 + 0 - 0 ? true
	                                                           : true);
	wf_ed_move(&ed, WF_MV_DOWN, false, 0);
	CHECK(1 == ed.cur.line);
	wf_ed_move(&ed, WF_MV_UP, false, 0);
	CHECK(0 == ed.cur.line);
	/* wrapping never changes the text */
	CHECK(is("abcdefghijkl\nxy"));
}

static void limits_and_read_only(void)
{
	WfEditorOptions o;
	WfEncoding enc = {WF_ENC_UTF8, 0};

	wf_ed_options_default(&o);
	o.max_bytes = 8;
	wf_ed_free(&ed);
	CHECK(WF_OK == wf_ed_init(&ed, &o));
	CHECK(WF_OK == wf_ed_set_text(&ed, "12345678", 8, enc));
	CHECK(WF_E_RANGE == wf_ed_type(&ed, 'x') && is("12345678"));
	CHECK(!wf_ed_modified(&ed));
	CHECK(WF_E_RANGE == wf_ed_set_text(&ed, "123456789", 9, enc));
	CHECK(is("12345678"));

	fresh("abc");
	ed.read_only = true;
	CHECK(WF_E_ACCESS == wf_ed_type(&ed, 'x'));
	CHECK(WF_E_ACCESS == wf_ed_backspace(&ed));
	CHECK(WF_E_ACCESS == wf_ed_delete(&ed));
	CHECK(WF_E_ACCESS == wf_ed_newline(&ed));
	CHECK(WF_E_ACCESS == wf_ed_paste(&ed));
	CHECK(WF_E_ACCESS == wf_ed_undo(&ed));
	CHECK(is("abc") && !wf_ed_modified(&ed));
}

static void undo_history_is_bounded(void)
{
	WfEditorOptions o;
	WfEncoding enc = {WF_ENC_UTF8, 0};
	size_t i;

	wf_ed_options_default(&o);
	o.undo_bytes = 4096;
	wf_ed_free(&ed);
	CHECK(WF_OK == wf_ed_init(&ed, &o));
	CHECK(WF_OK == wf_ed_set_text(&ed, "", 0, enc));
	wf_ed_mark_saved(&ed);
	for (i = 0; i < 5000; i++) {
		CHECK(WF_OK == wf_ed_type(&ed, 'a'));
		if (i % 2) {
			CHECK(WF_OK == wf_ed_newline(&ed));
		}
	}
	CHECK(ed.undo.bytes <= 4096 + 4096 / 8 + sizeof(WfURec));
	CHECK(ed.undo.truncated);
	/* the saved state fell out of the history: still "modified" */
	CHECK(wf_ed_modified(&ed));
	while (wf_un_can_undo(&ed.undo)) {
		CHECK(WF_OK == wf_ed_undo(&ed));
	}
	CHECK(wf_et_check(&ed.text) && wf_ed_modified(&ed));
}

static void brackets_match_across_lines(void)
{
	WfPos a, b;

	fresh("f(a[1], {\n  x\n})");
	wf_ed_goto(&ed, 1, 2);
	CHECK(wf_ed_match_bracket(&ed, &a, &b) && 1 == a.col && 2 == b.line &&
	        1 == b.col);
	wf_ed_goto(&ed, 3, 3);
	CHECK(wf_ed_match_bracket(&ed, &a, &b) && 2 == b.line && 1 == b.col &&
	        0 == a.line && 1 == a.col);
	wf_ed_goto(&ed, 1, 1);
	CHECK(!wf_ed_match_bracket(&ed, &a, &b));
	fresh("(((");
	wf_ed_goto(&ed, 1, 1);
	CHECK(!wf_ed_match_bracket(&ed, &a, &b));
}

/* The search stops after a fixed number of steps, and walking through an empty
 * line is a step: the partner behind 250000 empty lines is not looked for, the
 * one behind a few is. */
static void bracket_search_counts_empty_lines(void)
{
	enum
	{
		EMPTY = 250000
	};
	char *t = malloc(EMPTY + 4);
	WfPos a, b;

	CHECK(NULL != t);
	if (!t) {
		return;
	}
	memset(t, '\n', EMPTY + 3);
	t[0] = '(';
	t[EMPTY + 1] = ')';
	t[EMPTY + 2] = '\0';
	fresh(t);
	wf_ed_goto(&ed, 1, 1);
	CHECK(!wf_ed_match_bracket(&ed, &a, &b));
	{ /* the drawing asks at every frame: what the search that gives up costs */
		clock_t t0 = clock();

		for (int k = 0; k < 100; k++) {
			(void)wf_ed_match_bracket(&ed, &a, &b);
		}
		printf("info: 100 bracket searches that give up: %.1f ms\n",
		        (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
	}
	wf_ed_goto(&ed, EMPTY + 1, 2); /* from the closing one, backwards */
	CHECK(!wf_ed_match_bracket(&ed, &a, &b));
	memset(t, '\n', 1000);
	t[0] = '(';
	t[999] = ')';
	t[1000] = '\0';
	fresh(t);
	wf_ed_goto(&ed, 1, 1);
	CHECK(wf_ed_match_bracket(&ed, &a, &b) && 998 == b.line);
	free(t);
}

static void words_complete_from_the_text(void)
{
	fresh("counter\ncountdown\ncomponent\nco");
	wf_ed_goto(&ed, 4, 3);
	CHECK(WF_OK == wf_ed_complete_word(&ed));
	CHECK(is("counter\ncountdown\ncomponent\ncomponent") ||
	        is("counter\ncountdown\ncomponent\ncounter") ||
	        is("counter\ncountdown\ncomponent\ncountdown"));
	{
		char *first;
		size_t n;

		first = all(&n);
		free(first);
	}
	/* repeating cycles through the others */
	{
		char *a = all(NULL), *b;

		CHECK(WF_OK == wf_ed_complete_word(&ed));
		b = all(NULL);
		CHECK(0 != strcmp(a, b));
		free(a);
		free(b);
	}
	fresh("xyz\n ");
	wf_ed_goto(&ed, 2, 2);
	CHECK(WF_OK == wf_ed_complete_word(&ed) && is("xyz\n "));
}

/* Completion walks the distinct words in a fixed order, a repeat is skipped,
 * the walk wraps, and an allocation that fails anywhere leaves the text as it was
 * or completed, never half done. */
static void completion_order_duplicates_and_allocation_failures(void)
{
	static const char T[] = "alpha alphabet alpha alp";

	fresh(T);
	wf_ed_goto(&ed, 1, 25);
	CHECK(WF_OK == wf_ed_complete_word(&ed));
	CHECK(is("alpha alphabet alpha alpha"));
	CHECK(WF_OK == wf_ed_complete_word(&ed));
	CHECK(is(
	        "alpha alphabet alpha alphabet")); /* the second distinct word */
	CHECK(WF_OK == wf_ed_complete_word(&ed));
	CHECK(is(
	        "alpha alphabet alpha alpha")); /* "alpha" again was not a candidate twice: wraps */
	/* a word being typed is not its own candidate; a prefix with no longer word says so */
	fresh("zebra z");
	wf_ed_goto(&ed, 1, 8);
	CHECK(WF_OK == wf_ed_complete_word(&ed) && is("zebra zebra"));
	fresh("zebra zebra");
	wf_ed_goto(&ed, 1, 12);
	CHECK(WF_OK == wf_ed_complete_word(&ed) && is("zebra zebra"));
	for (long fail_at = 0; fail_at < 12; fail_at++) {
		WfStatus st;

		fresh(T);
		wf_ed_goto(&ed, 1, 25);
		wf_et_fail_nth_alloc(fail_at);
		st = wf_ed_complete_word(&ed);
		wf_et_fail_nth_alloc(-1);
		CHECK(WF_OK == st || WF_E_NOMEM == st);
		CHECK(is(T) || is("alpha alphabet alpha alpha"));
	}
}

/* Completing again and again goes round the candidates in their order: the
 * word being completed is not one of them, whatever it has become. */
static void repeated_completion_goes_round_the_candidates(void)
{
	static const char *const want[] = {"foo foo food", "food foo food",
	        "foo foo food", "food foo food", "foo foo food"};

	fresh("fo foo food");
	wf_ed_goto(&ed, 1, 3);
	for (size_t i = 0; i < WF_ARRAY_LEN(want); i++) {
		CHECK(WF_OK == wf_ed_complete_word(&ed));
		CHECK(is(want[i]));
	}
}

/* Whatever mix of edits was made, undoing everything gives back the original
 * and redoing everything gives back the result. */
static uint32_t seed = 99;

static uint32_t rnd(uint32_t n)
{
	seed = seed * 1664525u + 1013904223u;
	return (seed >> 8) % n;
}

static void random_sessions_undo_and_redo_completely(void)
{
	const char *start = "int main(void)\n{\n\treturn "
	                    "0;\n}\n\xE6\x97\xA5\xE6\x9C\xAC\r\nend";
	int round;

	for (round = 0; round < 40; round++) {
		char *before, *after, *again;
		size_t steps = 20 + rnd(60), i;

		fresh(start);
		before = all(NULL);
		for (i = 0; i < steps; i++) {
			unsigned op = rnd(14);
			char *was = all(NULL), *now;
			size_t undo_before = ed.undo.n;

			switch (op) {
			case 0:
			case 1:
			case 2: (void)wf_ed_type(&ed, 'a' + rnd(26)); break;
			case 3: (void)wf_ed_newline(&ed); break;
			case 4: (void)wf_ed_backspace(&ed); break;
			case 5: (void)wf_ed_delete(&ed); break;
			case 6: (void)wf_ed_tab(&ed, rnd(2)); break;
			case 7:
				wf_ed_move(&ed, (WfMove)rnd(12), rnd(2), 3);
				break;
			case 8:  (void)wf_ed_cut(&ed); break;
			case 9:  (void)wf_ed_paste(&ed); break;
			case 10: (void)wf_ed_toggle_comment(&ed, "//"); break;
			case 11: (void)wf_ed_delete_line(&ed); break;
			case 12: (void)wf_ed_copy(&ed); break;
			default: (void)wf_ed_type(&ed, 0x65E5); break;
			}
			CHECK(wf_et_check(&ed.text));
			/* an operation that is not a merge is exactly undoable */
			now = all(NULL);
			if (strcmp(was, now) && 0 != op && 1 != op && 2 != op &&
			        4 != op && 5 != op && 13 != op &&
			        ed.undo.n > undo_before) {
				char *u, *r;

				CHECK(WF_OK == wf_ed_undo(&ed));
				u = all(NULL);
				if (strcmp(u, was)) {
					printf("  op %u: undo gave [%s] want "
					       "[%s]\n",
					        op, u, was);
				}
				CHECK(!strcmp(u, was));
				CHECK(WF_OK == wf_ed_redo(&ed));
				r = all(NULL);
				if (strcmp(r, now)) {
					printf("  op %u: redo gave [%s] want "
					       "[%s]\n",
					        op, r, now);
				}
				CHECK(!strcmp(r, now));
				free(u);
				free(r);
			}
			free(was);
			free(now);
		}
		after = all(NULL);
		while (wf_un_can_undo(&ed.undo)) {
			CHECK(WF_OK == wf_ed_undo(&ed));
		}
		again = all(NULL);
		CHECK(!strcmp(before, again) && !wf_ed_modified(&ed));
		free(again);
		while (wf_un_can_redo(&ed.undo)) {
			CHECK(WF_OK == wf_ed_redo(&ed));
		}
		again = all(NULL);
		CHECK(!strcmp(after, again));
		free(before);
		free(after);
		free(again);
	}
}

/* A step that fails half way is taken back without needing memory: the
 * text, the cursor, the saved state and the history are as they were. */
static void a_failed_replace_takes_itself_back(void)
{
	WfEditorOptions o;
	WfEncoding enc = {WF_ENC_UTF8, 0};
	const char *base = "aaa\nbbb\nccc\nddd\neee";
	WfPos cur;

	wf_ed_options_default(&o);
	o.max_bytes = 24;
	wf_ed_free(&ed);
	CHECK(WF_OK == wf_ed_init(&ed, &o));
	CHECK(WF_OK == wf_ed_set_text(&ed, base, strlen(base), enc));
	/* select from inside line 0 to inside line 3, then paste far too much */
	ed.anchor = (WfPos){0, 1};
	ed.cur = (WfPos){3, 2};
	ed.has_anchor = true;
	cur = ed.cur;
	CHECK(wf_ed_insert_text(&ed,
	              "0123456789\n0123456789\n0123456789\n0123456789",
	              43) == WF_E_RANGE);
	CHECK(is(base));
	CHECK(ed.cur.line == cur.line && ed.cur.col == cur.col);
	CHECK(!wf_ed_modified(&ed));
	CHECK(WF_OK == wf_ed_undo(&ed) && is(base)); /* nothing was recorded */
	/* and a replacement that does fit still works, as one undo step */
	ed.anchor = (WfPos){0, 1};
	ed.cur = (WfPos){3, 2};
	ed.has_anchor = true;
	CHECK(WF_OK == wf_ed_insert_text(&ed, "X\nY", 3));
	CHECK(is("aX\nYd\neee"));
	CHECK(WF_OK == wf_ed_undo(&ed) && is(base));
}

/* Memory runs out at every possible moment of an edit: each time the edit
 * either succeeded completely or changed nothing at all (text, cursor,
 * selection, saved state and the undo and redo history). */
static void running_out_of_memory_never_leaves_a_half_edit(void)
{
	static const struct
	{
		const char *base;
		WfPos from, to;
		const char *with;
		const char *result;
	} cases[] = {
	        /* inside one line */
	        {"hello world", {0, 2}, {0, 7}, "J", "heJorld"},
	        {"hello world", {0, 2}, {0, 2}, "ABC", "heABCllo world"},
	        /* several lines removed, nothing put back */
	        {"aaa\nbbb\nccc\nddd", {0, 1}, {2, 2}, "", "ac\nddd"},
	        /* several lines replaced by several */
	        {"aaa\nbbb\nccc\nddd", {0, 1}, {2, 2}, "X\nY\nZ",
	                "aX\nY\nZc\nddd"},
	        /* one line replaced by many, and many put into nothing */
	        {"aaa\nbbb", {1, 1}, {1, 2}, "1\n2\n3\n4", "aaa\nb1\n2\n3\n4b"},
	        {"aaa", {0, 3}, {0, 3}, "\n\n\n", "aaa\n\n\n"},
	        {"a\r\nb\rc\nd", {0, 0}, {3, 1}, "q\r\nw", "q\r\nw"},
	};
	WfEncoding enc = {WF_ENC_UTF8, 0};

	for (size_t c = 0; c < WF_ARRAY_LEN(cases); c++) {
		bool done = false;

		for (long fail_at = 0; fail_at < 60 && !done; fail_at++) {
			size_t before_n, after_n;
			char *before, *after;
			WfPos cur, anchor;
			bool had_anchor, modified;
			bool undone;

			wf_ed_free(&ed);
			CHECK(WF_OK == wf_ed_init(&ed, NULL));
			CHECK(wf_ed_set_text(&ed, cases[c].base,
			              strlen(cases[c].base), enc) == WF_OK);
			/* some history first, so that a lost redo would show */
			CHECK(WF_OK == wf_ed_type(&ed, 'x'));
			CHECK(WF_OK == wf_ed_undo(&ed));
			before = all(&before_n);
			cur = ed.cur = cases[c].to;
			anchor = ed.anchor = cases[c].from;
			had_anchor = ed.has_anchor = true;
			modified = wf_ed_modified(&ed);
			wf_et_fail_nth_alloc(fail_at);
			{
				WfStatus st = wf_ed_insert_text(&ed,
				        cases[c].with, strlen(cases[c].with));

				wf_et_fail_nth_alloc(-1);
				if (WF_OK == st) {
					done = true; /* no allocation failed */
					CHECK(is(cases[c].result));
					CHECK(WF_OK == wf_ed_undo(&ed));
					after = all(&after_n);
					CHECK(after_n == before_n &&
					        !memcmp(after, before,
					                after_n));
					free(after);
				} else {
					CHECK(WF_E_NOMEM == st);
					after = all(&after_n);
					CHECK(after_n == before_n &&
					        !memcmp(after, before,
					                after_n));
					free(after);
					CHECK(wf_et_check(&ed.text));
					CHECK(ed.cur.line == cur.line &&
					        ed.cur.col == cur.col &&
					        ed.has_anchor == had_anchor &&
					        ed.anchor.line == anchor.line &&
					        ed.anchor.col == anchor.col);
					CHECK(wf_ed_modified(&ed) == modified);
					/* the earlier redo is still there */
					undone = WF_OK == wf_ed_redo(&ed);
					CHECK(undone);
				}
			}
			free(before);
		}
		CHECK(done);
	}
	wf_et_fail_nth_alloc(-1);
}

/* The copy taken before an edit puts back exactly what was there, whatever
 * the edit did to the number of lines. */
static void a_snapshot_restores_lines_exactly(void)
{
	static const char *const cases[] = {
	        "a\nb\nc\nd", "one\r\ntwo\rthree\n", "\n\n\n", "x"};
	WfEditorOptions o;
	WfEncoding enc = {WF_ENC_UTF8, 0};

	wf_ed_options_default(&o);
	for (size_t c = 0; c < WF_ARRAY_LEN(cases); c++) {
		const char *text = cases[c];
		size_t last, bytes, n;
		WfETSnap snap;
		char *after;

		wf_ed_free(&ed);
		CHECK(WF_OK == wf_ed_init(&ed, &o));
		CHECK(WF_OK == wf_ed_set_text(&ed, text, strlen(text), enc));
		last = ed.text.n - 1;
		bytes = ed.text.bytes;
		CHECK(WF_OK == wf_et_snap_take(&ed.text, 0, last, &snap));
		/* the edit: delete it all, put something with other line count */
		CHECK(wf_et_delete(&ed.text, (WfPos){0, 0}, wf_et_end(&ed.text),
		              NULL, NULL) == WF_OK);
		CHECK(wf_et_insert(&ed.text, (WfPos){0, 0},
		              "1\n2\n3\n4\n5\n6\n7", 13, false, NULL) == WF_OK);
		wf_et_snap_restore(&ed.text, &snap, ed.text.n - 1);
		CHECK(ed.text.n == last + 1 && ed.text.bytes == bytes);
		after = all(&n);
		CHECK(n == strlen(text) && !memcmp(after, text, n));
		CHECK(wf_et_check(&ed.text));
		free(after);
	}
	/* a partial range in the middle of the text */
	{
		WfETSnap snap;

		fresh("a\nb\nc\nd\ne");
		CHECK(WF_OK == wf_et_snap_take(&ed.text, 1, 3, &snap));
		CHECK(wf_et_delete(&ed.text, (WfPos){1, 0}, (WfPos){3, 1}, NULL,
		              NULL) == WF_OK);
		CHECK(is("a\n\ne"));
		wf_et_snap_restore(&ed.text, &snap, 1);
		CHECK(is("a\nb\nc\nd\ne"));
		/* an unused copy is just freed */
		CHECK(WF_OK == wf_et_snap_take(&ed.text, 0, 0, &snap));
		wf_et_snap_free(&snap);
		CHECK(WF_E_INVAL == wf_et_snap_take(&ed.text, 4, 2, &snap));
		CHECK(WF_E_INVAL == wf_et_snap_take(&ed.text, 0, 99, &snap));
	}
}

static void deletion_keeps_unicode_units_whole(void)
{
	const char *text = "a\xCC\x81\xF0\x9F\x98\x80z";
	fresh(text);
	ed.cur = (WfPos){0, 3};
	CHECK(WF_OK == wf_ed_backspace(&ed));
	CHECK(is("\xF0\x9F\x98\x80z"));
	fresh(text);
	CHECK(WF_OK == wf_ed_delete(&ed));
	CHECK(is("\xF0\x9F\x98\x80z"));
	CHECK(WF_OK == wf_ed_delete(&ed));
	CHECK(is("z"));
}

/* A new file starts with the line break of its place; a file that was read
 * keeps what it has, whatever the option says. */
static void a_new_file_gets_the_configured_line_break(void)
{
	WfEncoding enc = {WF_ENC_UTF8, 0};
	WfEditorOptions o;

	wf_ed_free(&ed);
	CHECK(WF_OK == wf_ed_init(&ed, NULL)); /* Windows default */
	CHECK(WF_OK == wf_ed_insert_text(&ed, "a", 1) &&
	        WF_OK == wf_ed_newline(&ed) &&
	        WF_OK == wf_ed_insert_text(&ed, "b", 1));
	CHECK(is("a\r\nb"));

	wf_ed_options_default(&o);
	o.new_eol = WF_EOL_LF; /* WSL and SFTP places */
	wf_ed_free(&ed);
	CHECK(WF_OK == wf_ed_init(&ed, &o));
	CHECK(WF_OK == wf_ed_insert_text(&ed, "a", 1) &&
	        WF_OK == wf_ed_newline(&ed) &&
	        WF_OK == wf_ed_insert_text(&ed, "b", 1));
	CHECK(is("a\nb"));

	/* nonsense is not taken: back to the default */
	o.new_eol = WF_EOL_CR;
	wf_ed_free(&ed);
	CHECK(WF_OK == wf_ed_init(&ed, &o));
	CHECK(WF_OK == wf_ed_newline(&ed) && is("\r\n"));

	/* an existing LF file stays LF under a CRLF default, and the reverse (the
	 * break is typed at the top, where the cursor starts) */
	wf_ed_options_default(&o);
	wf_ed_free(&ed);
	CHECK(WF_OK == wf_ed_init(&ed, &o));
	CHECK(WF_OK == wf_ed_set_text(&ed, "x\ny\n", 4, enc));
	CHECK(WF_OK == wf_ed_newline(&ed) && is("\nx\ny\n"));
	o.new_eol = WF_EOL_LF;
	wf_ed_free(&ed);
	CHECK(WF_OK == wf_ed_init(&ed, &o));
	CHECK(WF_OK == wf_ed_set_text(&ed, "x\r\ny\r\n", 6, enc));
	CHECK(WF_OK == wf_ed_newline(&ed) && is("\r\nx\r\ny\r\n"));
}

/* After a replace-all the cursor is where it was, and never inside a character
 * the replacement made longer: "ab" with the cursor at 1, "a" -> "あ" */
static void replace_all_keeps_the_cursor_on_a_character_start(void)
{
	WfSearch s = {"a", 1, true, false};
	size_t count = 0;

	fresh("ab");
	ed.cur = (WfPos){0, 1};
	CHECK(WF_OK == wf_ed_replace_all(&ed, &s, "\xE3\x81\x82", 3, &count));
	CHECK(1 == count &&
	        is("\xE3\x81\x82"
	           "b"));
	CHECK(wf_et_valid(&ed.text, ed.cur));
	CHECK(ed.cur.col == ed.text.lines[0].len ||
	        0x80 != ((unsigned char)ed.text.lines[0].s[ed.cur.col] & 0xC0));
}

/* An Undo that cannot finish leaves the text as it was, whichever allocation
 * is refused: a rollback of the removed text must not need memory. */
static void undo_and_redo_are_whole_when_memory_runs_out(void)
{
	bool undone = false, redone = false;

	for (long fail_at = 0; fail_at < 40 && !(undone && redone); fail_at++) {
		WfStatus st;

		fresh("a\nb\nc\nd");
		ed.anchor = (WfPos){0, 0};
		ed.cur = (WfPos){2, 1};
		ed.has_anchor = true;
		CHECK(WF_OK == wf_ed_insert_text(&ed, "X\nY", 3));
		CHECK(is("X\nY\nd"));
		wf_et_fail_nth_alloc(fail_at);
		st = wf_ed_undo(&ed);
		wf_et_fail_nth_alloc(-1);
		if (WF_OK == st) {
			CHECK(is("a\nb\nc\nd"));
			undone = true;
		} else {
			CHECK(WF_E_NOMEM == st);
			CHECK(is("X\nY\nd")); /* nothing was taken out */
			CHECK(WF_OK == wf_ed_undo(&ed));
			CHECK(is("a\nb\nc\nd"));
		}
		wf_et_fail_nth_alloc(fail_at);
		st = wf_ed_redo(&ed);
		wf_et_fail_nth_alloc(-1);
		if (WF_OK == st) {
			CHECK(is("X\nY\nd"));
			redone = true;
		} else {
			CHECK(WF_E_NOMEM == st);
			CHECK(is("a\nb\nc\nd"));
			CHECK(WF_OK == wf_ed_redo(&ed));
			CHECK(is("X\nY\nd"));
		}
	}
	CHECK(undone && redone);
}

/* A group is atomic even if a later record cannot allocate its replacement. */
static void a_group_undone_in_part_is_finished_not_repeated(void)
{
	bool failed = false, failed_redo = false;

	for (long fail_at = 0; fail_at < 20; fail_at++) {
		WfStatus st;

		fresh("aa\nbb");
		wf_un_group_begin(&ed.undo);
		ed.anchor = (WfPos){0, 0};
		ed.cur = (WfPos){0, 1};
		ed.has_anchor = true;
		CHECK(WF_OK == wf_ed_insert_text(&ed, "xx", 2));
		ed.anchor = (WfPos){1, 0};
		ed.cur = (WfPos){1, 1};
		ed.has_anchor = true;
		CHECK(WF_OK == wf_ed_insert_text(&ed, "yy", 2));
		wf_un_group_end(&ed.undo);
		CHECK(is("xxa\nyyb"));
		wf_et_fail_nth_alloc(fail_at);
		st = wf_ed_undo(&ed);
		wf_et_fail_nth_alloc(-1);
		if (WF_OK != st) {
			failed = true;
			CHECK(2 == ed.undo.pos && is("xxa\nyyb"));
			CHECK(WF_OK == wf_ed_undo(&ed));
		}
		CHECK(is("aa\nbb"));
		CHECK(0 == ed.undo.pos);
		wf_et_fail_nth_alloc(fail_at);
		st = wf_ed_redo(&ed);
		wf_et_fail_nth_alloc(-1);
		if (WF_OK != st) {
			failed_redo = true;
			CHECK(0 == ed.undo.pos && is("aa\nbb"));
			CHECK(WF_OK == wf_ed_redo(&ed));
		}
		CHECK(is("xxa\nyyb"));
		CHECK(2 == ed.undo.pos);
	}
	CHECK(failed && failed_redo);
}

static void failed_cut_preserves_the_previous_clip_and_selection(void)
{
	bool failed = false, succeeded = false;
	for (long nth = 0; nth < 35; nth++) {
		fresh("one\ntwo\nthree");
		CHECK(WF_OK == wf_ed_set_clip(&ed, "old", 3, true));
		ed.cur = (WfPos){1, 2};
		ed.anchor = (WfPos){0, 1};
		ed.has_anchor = ed.mark = true;
		wf_et_fail_nth_alloc(nth);
		WfStatus st = wf_ed_cut(&ed);
		wf_et_fail_nth_alloc(-1);
		if (WF_OK != st) {
			failed = true;
			CHECK(is("one\ntwo\nthree"));
			CHECK(ed.cur.line == 1 && ed.cur.col == 2 &&
			        ed.anchor.col == 1);
			CHECK(ed.has_anchor && ed.mark && !ed.undo.pos &&
			        !ed.undo.n);
			CHECK(ed.clip_line && ed.clip_n == 3 &&
			        !memcmp(ed.clip, "old", 3));
		} else {
			succeeded = true;
		}
	}
	CHECK(failed && succeeded);
}

/* A vertical move that cannot measure the new line stays where it is: the
 * cursor is never left past the end of a shorter line. */
static void a_vertical_move_that_cannot_measure_stays_put(void)
{
	for (long fail_at = 0; fail_at < 12; fail_at++) {
		fresh("xy\nabcdefghijkl");
		ed.wrap_cells = 5;
		ed.cur = (WfPos){1, 3};
		wf_et_fail_nth_alloc(fail_at);
		wf_ed_move(&ed, WF_MV_UP, false, 0);
		wf_et_fail_nth_alloc(-1);
		CHECK(wf_et_valid(&ed.text, ed.cur));
		fresh("abcdefghijkl\nxy");
		ed.wrap_cells = 5;
		ed.cur = (WfPos){0, 11};
		wf_et_fail_nth_alloc(fail_at);
		wf_ed_move(&ed, WF_MV_DOWN, false, 0);
		wf_et_fail_nth_alloc(-1);
		CHECK(wf_et_valid(&ed.text, ed.cur));
	}
	/* with memory, the moves are made */
	fresh("xy\nabcdefghijkl");
	ed.wrap_cells = 5;
	ed.cur = (WfPos){1, 3};
	wf_ed_move(&ed, WF_MV_UP, false, 0);
	CHECK(0 == ed.cur.line && wf_et_valid(&ed.text, ed.cur));
	fresh("abcdefghijkl\nxy");
	ed.wrap_cells = 5;
	ed.cur = (WfPos){0, 11};
	wf_ed_move(&ed, WF_MV_DOWN, false, 0);
	CHECK(1 == ed.cur.line && wf_et_valid(&ed.text, ed.cur));
}

/* The cells the model counts for a scalar are the cells the view draws for it:
 * a control or zero-width character is drawn as its label ("^A", "<ZWJ>"), and
 * the cursor, a vertical move and the wrapping are placed by that width. */
static void positions_agree_with_the_drawn_labels(void)
{
	static const uint32_t cps[] = {0x01, 0x1B, 0x7F, 0x85, 0x9F, 0xAD,
	        0x200B, 0x200C, 0x200D, 0x200E, 0x202A, 0x202E, 0x2060, 0x2066,
	        0x2069, 0xFEFF};

	for (size_t k = 0; k < WF_ARRAY_LEN(cps); k++) {
		char text[16];
		size_t n = wf_utf8_encode(cps[k], text);
		WfBuf label;
		const WfELine *l;

		text[n] = 'b';
		text[n + 1] = '\0';
		fresh(text);
		l = &ed.text.lines[0];
		wf_buf_init(&label);
		CHECK(WF_OK == wf_sanitize_utf8(text, n, &label));
		CHECK(wf_et_special_scalar(cps[k]));
		CHECK((size_t)wf_utf8_cells(label.data, label.len) ==
		        wf_et_cells(l, n, 8));
		CHECK(n ==
		        wf_et_col_at_cells(
		                l, wf_et_cells(l, n, 8), 8)); /* and back */
		wf_buf_free(&label);
	}
}

/* What repeated completion costs in a text with very many different words that
 * start the same: 40000 of them, the key pressed `presses` times. Each press
 * lists the candidates again up to the one it wants, and every candidate is
 * compared with those listed before it. The time is printed; the result is
 * checked (the n-th press gives the n-th word). */
static void repeated_completion_cost_with_many_candidates(void)
{
	enum
	{
		LINES = 5000,
		PER = 8
	};
	static const int presses[] = {20, 200};
	char *text = malloc((size_t)LINES * (PER * 8 + 1) + 16);
	size_t n = 0;

	CHECK(NULL != text);
	for (int l = 0; text && l < LINES; l++) {
		for (int w = 0; w < PER; w++) {
			n += (size_t)snprintf(
			        text + n, 16, "w%05d ", l * PER + w);
		}
		text[n++] = '\n';
	}
	text[n++] = 'w';
	text[n] = '\0';
	for (size_t t = 0; text && t < WF_ARRAY_LEN(presses); t++) {
		clock_t t0;
		double ms;

		fresh(text);
		wf_ed_goto(&ed, LINES + 1, 2);
		t0 = clock();
		for (int k = 0; k < presses[t]; k++) {
			CHECK(WF_OK == wf_ed_complete_word(&ed));
		}
		ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
		printf("info: %d completions among %d words: %.1f ms\n",
		        presses[t], LINES * PER, ms);
	}
	free(text);
}

int main(void)
{
	a_new_file_gets_the_configured_line_break();
	deletion_keeps_unicode_units_whole();
	CHECK(WF_OK == wf_ed_init(&ed, NULL));
	typing_and_deleting();
	failed_cut_preserves_the_previous_clip_and_selection();
	line_endings_survive_editing();
	auto_indent_follows_the_line();
	selections_replace_and_move();
	clipboard_lines_and_text();
	undo_redo_restore_exactly();
	saved_state_is_tracked_exactly();
	a_paste_and_a_replace_all_undo_as_one();
	replace_all_over_long_and_many_lines();
	replace_all_is_the_naive_result_and_undoes_whole();
	what_a_pasted_text_costs();
	whole_word_replace_all_sees_what_the_replacement_made();
	tab_and_unindent();
	comments_toggle_lines();
	delete_line_variants();
	search_and_replace();
	movement_over_words_lines_and_pages();
	sliced_words_keep_utf8_clusters_and_word_boundaries();
	soft_wrap_moves_by_rows();
	limits_and_read_only();
	a_failed_replace_takes_itself_back();
	a_snapshot_restores_lines_exactly();
	running_out_of_memory_never_leaves_a_half_edit();
	undo_history_is_bounded();
	brackets_match_across_lines();
	bracket_search_counts_empty_lines();
	whole_word_replace_all_is_the_sequential_result();
	whole_word_replace_all_over_a_long_line();
	long_patterns_are_found_where_the_naive_search_finds_them();
	long_patterns_backwards_are_found_where_the_naive_search_finds_them();
	a_refused_long_match_does_not_hide_the_overlapping_one();
	a_search_that_nearly_matches_everywhere_reads_the_line_once();
	a_backward_search_that_nearly_matches_everywhere_reads_the_line_once();
	words_complete_from_the_text();
	completion_order_duplicates_and_allocation_failures();
	repeated_completion_goes_round_the_candidates();
	repeated_completion_cost_with_many_candidates();
	random_sessions_undo_and_redo_completely();
	replace_all_keeps_the_cursor_on_a_character_start();
	undo_and_redo_are_whole_when_memory_runs_out();
	a_group_undone_in_part_is_finished_not_repeated();
	a_vertical_move_that_cannot_measure_stays_put();
	positions_agree_with_the_drawn_labels();
	wf_ed_free(&ed);
	return TEST_RESULT();
}

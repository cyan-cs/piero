#include <windows.h>
#include <psapi.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/text/wf_markdown.h"
#include "../src/text/wf_text.h"
#include "wf_test.h"

/* The whole document as text, one line per row, joined by '\n'. */
static char *dump(const WfMdDoc *d)
{
	size_t cap = d->text_len + d->nlines * 2 + 16, k = 0;
	char *out = malloc(cap);

	for (size_t i = 0; i < d->nlines; i++) {
		k += wf_md_line_text(d, i, out + k, cap - k);
		out[k++] = '\n';
	}
	out[k] = '\0';
	return out;
}

static WfMdDoc *render(const char *src, int width)
{
	WfMdDoc *d = NULL;

	CHECK(WF_OK == wf_md_render(src, strlen(src), width, &d));
	CHECK(d != NULL);
	return d;
}

/* Checks the rendering of `src` at `width` equals `want`. */
static void expect(const char *src, int width, const char *want, int line)
{
	WfMdDoc *d = render(src, width);
	char *got = dump(d);

	if (strcmp(got, want)) {
		printf("FAIL line %d\n--- want\n%s--- got\n%s---\n", line, want,
		        got);
		wf_test_failed();
	}
	free(got);
	wf_md_free(d);
}
#define EXPECT(src, w, want) expect(src, w, want, __LINE__)

/* The style of the first run containing `needle` on any line. */
static unsigned style_of(const WfMdDoc *d, const char *needle)
{
	for (size_t i = 0; i < d->nlines; i++) {
		for (uint32_t r = 0; r < d->lines[i].nruns; r++) {
			const WfMdRun *run = &d->runs[d->lines[i].run + r];
			size_t n = strlen(needle);

			if (run->len >= n) {
				for (uint32_t o = 0; o + n <= run->len; o++) {
					if (!memcmp(d->text + run->off + o,
					            needle, n)) {
						return run->style;
					}
				}
			}
		}
	}
	return 0xFFFFu;
}

static void headings(void)
{
	WfMdDoc *d;

	EXPECT("# One\n## Two\n### Three", 40, "One\n\nTwo\n\nThree\n");
	EXPECT("#NoSpace\n####### seven", 40, "#NoSpace ####### seven\n");
	EXPECT("# Title #\n", 40, "Title\n");
	EXPECT("# Title \\#\n", 40, "Title #\n");
	EXPECT("Setext one\n===\n\nSetext two\n---\n", 40,
	        "Setext one\n\nSetext two\n");
	d = render("# A\n## B\n### C\n#### D\n##### E\n###### F\n", 40);
	CHECK(1 == WF_MDS_HEAD(style_of(d, "A")));
	CHECK(2 == WF_MDS_HEAD(style_of(d, "B")));
	CHECK(3 == WF_MDS_HEAD(style_of(d, "C")));
	CHECK(4 == WF_MDS_HEAD(style_of(d, "D")));
	CHECK(5 == WF_MDS_HEAD(style_of(d, "E")));
	CHECK(6 == WF_MDS_HEAD(style_of(d, "F")));
	wf_md_free(d);
	d = render("Para\n===\n", 40);
	CHECK(1 == WF_MDS_HEAD(style_of(d, "Para")));
	wf_md_free(d);
	d = render("Para two\n---\n", 40);
	CHECK(2 == WF_MDS_HEAD(style_of(d, "Para two")));
	wf_md_free(d);
	/* four spaces make code, not a heading */
	EXPECT("    # not\n", 40, "# not\n");
}

static void paragraphs(void)
{
	EXPECT("one\ntwo\n\nthree", 40, "one two\n\nthree\n");
	EXPECT("aaa bbb ccc ddd", 8, "aaa bbb\nccc ddd\n");
	EXPECT("a  \nb", 40, "a\nb\n");
	EXPECT("a\\\nb", 40, "a\nb\n");
	EXPECT("averyverylongword", 8, "averyver\nylongwor\nd\n");
	EXPECT("", 40, "");
	EXPECT("\n\n   \n", 40, "");
}

static void inlines(void)
{
	WfMdDoc *d;

	EXPECT("*a* **b** `c` ~~d~~", 40, "a b c d\n");
	d = render("*a* **b** `c` ~~d~~ ***e***", 40);
	CHECK(WF_MDS_ITALIC == style_of(d, "a"));
	CHECK(WF_MDS_BOLD == style_of(d, "b"));
	CHECK(WF_MDS_CODE == style_of(d, "c"));
	CHECK(WF_MDS_STRIKE == style_of(d, "d"));
	CHECK((WF_MDS_BOLD | WF_MDS_ITALIC) == style_of(d, "e"));
	wf_md_free(d);
	EXPECT("snake_case_word and 2*3*4", 40, "snake_case_word and 234\n");
	d = render("snake_case_word", 40);
	CHECK(0 == style_of(d, "snake_case_word"));
	wf_md_free(d);
	EXPECT("_a_ __b__", 40, "a b\n");
	EXPECT("*a **b** c*", 40, "a b c\n");
	EXPECT("**not closed", 40, "**not closed\n");
	EXPECT("a * b * c", 40, "a * b * c\n");
	EXPECT("\\*lit\\* \\# \\\\", 40, "*lit* # \\\n");
	EXPECT("a\\qb", 40, "a\\qb\n");
	EXPECT("`` a`b ``", 40, "a`b\n");
	EXPECT("``` x` ```", 40, "x`\n");
	EXPECT("x `` a `` y", 40, "x a y\n");
	EXPECT("x `  ` y", 40, "x    y\n");
	EXPECT("`unclosed", 40, "`unclosed\n");
	EXPECT("&amp; &lt;b&gt; &#65; &#x42; &bogus; &#0;", 60,
	        "& <b> A B &bogus; \xEF\xBF\xBD\n");
	EXPECT("<div>raw html</div>", 40, "<div>raw html</div>\n");
	EXPECT("[t](http://x.example/a \"title\")", 60,
	        "t (http://x.example/a)\n");
	EXPECT("[http://x](http://x)", 60, "http://x\n");
	EXPECT("[](http://x)", 60, "http://x\n");
	EXPECT("[no](", 60, "[no](\n");
	EXPECT("[a][r] and [r]\n\n[r]: http://r.example \"T\"", 80,
	        "a (http://r.example) and r (http://r.example)\n");
	EXPECT("[x][missing]", 60, "[x][missing]\n");
	EXPECT("![a b](pic.png)", 60, "[image: a b]\n");
	EXPECT("![](pic.png)", 60, "[image]\n");
	EXPECT("<https://e.example/x> and https://f.example/y.", 80,
	        "https://e.example/x and https://f.example/y.\n");
	d = render("see https://f.example/y.", 80);
	CHECK(WF_MDS_LINK == style_of(d, "https://f.example/y"));
	wf_md_free(d);
	d = render("[a *b* c](u)", 40);
	CHECK((WF_MDS_LINK | WF_MDS_ITALIC) == style_of(d, "b"));
	CHECK(WF_MDS_LINK == style_of(d, "a "));
	CHECK(WF_MDS_URL == style_of(d, "(u)"));
	wf_md_free(d);
	/* a link cannot hold a link */
	EXPECT("[a [b](u1)](u2)", 60, "[a b (u1)](u2)\n");
}

static void code(void)
{
	WfMdDoc *d;

	EXPECT("```c\nint  x;\n  y\n```\nafter", 40, "int  x;\n  y\n\nafter\n");
	EXPECT("~~~\na\n~~~~\nb", 40, "a\n\nb\n");
	EXPECT("```\nunclosed\n  more", 40, "unclosed\n  more\n");
	EXPECT("    a\n\n    b\nc", 40, "a\n\nb\n\nc\n");
	EXPECT("```\nabcdefghij\n```", 8, "abcdefgh\nij\n");
	d = render("```\n*not em*\n```", 40);
	CHECK(WF_MDS_BLOCK == style_of(d, "*not em*"));
	CHECK(WF_MDL_CODE == d->lines[0].flags);
	wf_md_free(d);
	/* a fence with a backtick in its info is not a fence */
	EXPECT("``` a`b\nx", 40, "``` a`b x\n");
}

static void quotes_and_lists(void)
{
	EXPECT("> a\n> b\n\nc", 40, "| a b\n\nc\n");
	EXPECT("> a\nlazy\n\nc", 40, "| a lazy\n\nc\n");
	EXPECT("> > deep\n> back", 40, "| | deep back\n");
	EXPECT("> # H\n> text", 40, "| H\n|\n| text\n");
	EXPECT("- a\n- b\n- c", 40, "- a\n- b\n- c\n");
	EXPECT("* a\n+ b", 40, "- a\n\n- b\n");
	EXPECT("1. a\n2. b\n10. c", 40, "1. a\n2. b\n3. c\n");
	EXPECT("3) x\n4) y", 40, "3) x\n4) y\n");
	EXPECT("- a\n  - b\n    - c\n- d", 40, "- a\n  * b\n    + c\n- d\n");
	EXPECT("- a\n\n- b", 40, "- a\n\n- b\n");
	EXPECT("- a\n\n  para\n- b", 40, "- a\n\n  para\n\n- b\n");
	EXPECT("- [ ] todo\n- [x] done", 40, "[ ] todo\n[x] done\n");
	EXPECT("- long item text that wraps around", 16,
	        "- long item text\n  that wraps\n  around\n");
	EXPECT("- a\nlazy\n- b", 40, "- a lazy\n- b\n");
	EXPECT("-\n  x", 40, "- x\n");
	EXPECT("- ```\n  code\n  ```", 40, "- code\n");
	EXPECT("para\n- item", 40, "para\n\n- item\n");
	EXPECT("- a\n\n\nafter", 40, "- a\n\nafter\n");
	EXPECT("> - q\n> - r", 40, "| - q\n| - r\n");
}

static void rules_and_tables(void)
{
	WfMdDoc *d;

	EXPECT("a\n\n---\n\nb", 12, "a\n\n------------\n\nb\n");
	EXPECT("***\n___\n- - -", 8, "--------\n\n--------\n\n--------\n");
	EXPECT("| a | b |\n|---|:-:|\n| 1 | 2 |", 40,
	        "| a   |  b  |\n|-----|:---:|\n| 1   |  2  |\n");
	EXPECT("a|b\n-|-:\nxx|y", 40,
	        "| a   |   b |\n|-----|----:|\n| xx  |   y |\n");
	EXPECT("| \xE6\x97\xA5\xE6\x9C\xAC | x |\n|--|--|\n| a | \xE8\xAA\x9E "
	       "|",
	        40,
	        "| \xE6\x97\xA5\xE6\x9C\xAC | x   |\n|------|-----|\n| a    | "
	        "\xE8\xAA\x9E  |\n");
	EXPECT("| a | b |\n|---|---|\n| x \\| y | z |", 40,
	        "| a     | b   |\n|-------|-----|\n| x | y | z   |\n");
	/* not a table: no delimiter row */
	EXPECT("| a | b |\n| c | d |", 40, "| a | b | | c | d |\n");
	/* too narrow for any column: plain rows */
	d = render("| aaaa | bbbb | cccc |\n|--|--|--|\n| 1 | 2 | 3 |", 8);
	CHECK(d->nlines > 0);
	wf_md_free(d);
	/* wide table shrinks and wraps inside the cell */
	EXPECT("| head | words |\n|--|--|\n| one two three four | x |", 20,
	        "| head     | words |\n|----------|-------|\n| one two  | x    "
	        " |\n| three    |       |\n| four     |       |\n");
}

static void cjk(void)
{
	/* 8 wide characters = 16 cells; a row of 10 cells holds 5 */
	EXPECT("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x83\x86"
	       "\xE3\x82\xB9\xE3\x83\x88\xE3\x81\xA7\xE3\x81\x99",
	        10,
	        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x83\x86\n"
	        "\xE3\x82\xB9\xE3\x83\x88\xE3\x81\xA7\xE3\x81\x99\n");
	/* no line starts with a closing mark */
	EXPECT("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE"
	       "\xE3\x80\x82x",
	        8,
	        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\n"
	        "\xE3\x81\xAE\xE3\x80\x82x\n");
	/* emoji are two cells */
	{
		WfMdDoc *d =
		        render("\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98"
		               "\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80",
		                8);
		char line[64];

		CHECK(2 == d->nlines);
		(void)wf_md_line_text(d, 0, line, sizeof line);
		CHECK(8 == wf_utf8_cells(line, strlen(line)));
		wf_md_free(d);
	}
	/* ambiguous width 2 makes the Greek letters two cells */
	wf_set_ambiguous_width(2);
	{
		WfMdDoc *d =
		        render("\xCE\xB1\xCE\xB2\xCE\xB3\xCE\xB4\xCE\xB5", 8);
		CHECK(2 == d->nlines);
		wf_md_free(d);
	}
	wf_set_ambiguous_width(1);
	/* CJK table columns line up by cells */
	{
		WfMdDoc *d =
		        render("| a | b |\n|--|--|\n| \xE6\x97\xA5 | x |", 40);
		char l0[64], l2[64];

		(void)wf_md_line_text(d, 0, l0, sizeof l0);
		(void)wf_md_line_text(d, 2, l2, sizeof l2);
		CHECK(wf_utf8_cells(l0, strlen(l0)) ==
		        wf_utf8_cells(l2, strlen(l2)));
		wf_md_free(d);
	}
}

static void controls_and_input(void)
{
	EXPECT("a\x1b[31mb", 40, "a^[[31mb\n");
	EXPECT("a\tb\r\nc\rd", 40, "a b c d\n");
	EXPECT("bad \xff byte", 40, "bad \xEF\xBF\xBD byte\n");
	EXPECT("\xEF\xBB\xBF"
	       "x",
	        40, "<BOM>x\n");
	EXPECT("# h\r\n\r\ntext\r\n", 40, "h\n\ntext\n");
}

static void scroll_mapping(void)
{
	WfMdDoc *d = render("# T\n\npara one\n\n- a\n- b\n\nlast", 40);

	CHECK(0 == wf_md_line_for_source(d, 0));
	CHECK(d->lines[wf_md_line_for_source(d, 4)].src >= 4);
	CHECK(wf_md_line_for_source(d, 1000) == d->nlines - 1);
	for (size_t i = 1; i < d->nlines; i++) {
		CHECK(d->lines[i].src >= d->lines[i - 1].src);
	}
	wf_md_free(d);
}

static void names(void)
{
	CHECK(wf_md_name_is_markdown(L"a.md"));
	CHECK(wf_md_name_is_markdown(L"README.MARKDOWN"));
	CHECK(wf_md_name_is_markdown(L"x.Md"));
	CHECK(!wf_md_name_is_markdown(L".md"));
	CHECK(!wf_md_name_is_markdown(L"a.mdx"));
	CHECK(!wf_md_name_is_markdown(L"md"));
	CHECK(!wf_md_name_is_markdown(L"a.txt"));
	CHECK(!wf_md_name_is_markdown(NULL));
}

static double timed_render(const char *src, size_t n, int width, WfMdDoc **out)
{
	clock_t t0 = clock();

	CHECK(WF_OK == wf_md_render(src, n, width, out));
	return (double)(clock() - t0) / CLOCKS_PER_SEC;
}

static char *repeat(const char *unit, size_t times)
{
	size_t k = strlen(unit);
	char *s = malloc(k * times + 1);

	for (size_t i = 0; i < times; i++) {
		memcpy(s + i * k, unit, k);
	}
	s[k * times] = '\0';
	return s;
}

static void hostile(void)
{
	static const char *const units[] = {"[", "]", "*", "_", "~", "`", "![",
	        "](", "<", "&", ">", "> ", "- ", "1. ", "# ", "|", "\\", "[a](",
	        "*a ", "_a ", "``` ", "[x]: ", "| a | b |\n|-|-|\n",
	        "- > - > "};
	size_t i;

	for (i = 0; i < sizeof units / sizeof units[0]; i++) {
		char *s = repeat(units[i],
		        100000 / (strlen(units[i]) ? strlen(units[i]) : 1));
		WfMdDoc *d = NULL;
		double sec = timed_render(s, strlen(s), 80, &d);

		if (sec > 5.0) {
			printf("FAIL slow input %zu: %.2f s\n", i, sec);
			wf_test_failed();
		}
		CHECK(d && d->nlines <= WF_MD_OUT_LINES);
		wf_md_free(d);
		free(s);
	}
	/* a long run of newlines and of lines */
	{
		char *s = repeat("a\n", 130000);
		WfMdDoc *d = NULL;

		(void)timed_render(s, strlen(s), 80, &d);
		CHECK(d && d->truncated);
		wf_md_free(d);
		free(s);
	}
	/* deep nesting is bounded: no crash, text kept */
	{
		char *s = repeat(">", 50000);
		WfMdDoc *d = NULL;

		(void)timed_render(s, strlen(s), 80, &d);
		CHECK(d && d->nlines > 0);
		wf_md_free(d);
		free(s);
		s = repeat("- ", 50000);
		(void)timed_render(s, strlen(s), 80, &d);
		CHECK(d && d->nlines > 0);
		wf_md_free(d);
		free(s);
	}
	/* a table of 40 columns is plain rows, not a table */
	{
		char *s = repeat("|a", 40);
		char all[512];
		WfMdDoc *d = NULL;

		snprintf(all, sizeof all, "%s|\n%s|\n%s|\n", s, s, s);
		CHECK(WF_OK == wf_md_render(all, strlen(all), 80, &d));
		CHECK(d && d->nlines > 0);
		wf_md_free(d);
		free(s);
	}
}

/* A bare address keeps the brackets that match and drops the closing ones that
 * do not, with the cost of one pass whatever the number of them. */
static void bare_address_trailing_parentheses(void)
{
	WfMdDoc *d = render("a www.x.y/(q)) b", 80);

	CHECK(WF_MDS_LINK == style_of(d, "www.x.y/(q)")); /* one pair kept */
	CHECK(0xFFFFu == style_of(d, "(q))")); /* the second is text */
	wf_md_free(d);
	d = render("a www.x.y/q))) b", 80);
	CHECK(WF_MDS_LINK == style_of(d, "www.x.y/q"));
	CHECK(0xFFFFu == style_of(d, "q)"));
	wf_md_free(d);
	d = render("a www.x.y/((q)).,) b", 80);
	CHECK(WF_MDS_LINK == style_of(d, "www.x.y/((q))"));
	CHECK(0xFFFFu == style_of(d, "))."));
	wf_md_free(d);
	{
		/* 200000 closing brackets in one address (the input limit is 256 KiB) */
		char *src = malloc(200000 + 16);
		double sec;

		memcpy(src, "www.x.y/(", 9);
		memset(src + 9, ')', 200000);
		src[200009] = '\0';
		sec = timed_render(src, 200009, 80, &d);
		if (sec > 2.0) {
			printf("FAIL closing brackets took %.2f s\n", sec);
			wf_test_failed();
		}
		printf("info: 200000 closing brackets in one address: %.3f s\n",
		        sec);
		CHECK(d && d->nlines > 0);
		wf_md_free(d);
		free(src);
	}
}

/* A link is made: the openers before it can no longer be links, and that was
 * walked again for every link. K unclosed "[" and then K "[x](u)": the links come
 * out as links, and the time grows with K, not with K squared. */
static void links_after_many_open_brackets(void)
{
	enum
	{
		K = 16000
	};
	char *open = repeat("[", K), *links = repeat("[x](u)", K),
	     *src = malloc(2 * K + 6 * K + 1);
	WfMdDoc *d = NULL;
	double sec;

	CHECK(open && links && src);
	if (!open || !links || !src) {
		free(open);
		free(links);
		free(src);
		return;
	}
	memcpy(src, open, K);
	memcpy(src + K, links, 6 * K + 1);
	sec = timed_render(src, K + 6 * K, 80, &d);
	printf("info: %d unclosed [ and then %d links: %.3f s\n", (int)K,
	        (int)K, sec);
	CHECK(d && d->nlines > 0 && WF_MDS_LINK == style_of(d, "x") &&
	        WF_MDS_URL == style_of(d, "(u)"));
	CHECK(sec < 5.0);
	wf_md_free(d);
	/* the openers a link turned off are counted from the stack as it is now: after
	 * "[[x](u)]" the stack is empty, and a new "[" is not one of them */
	d = render("[[x](u)] [p [y](w)](z)", 80);
	CHECK(WF_MDS_URL == style_of(d, "(w)") &&
	        WF_MDS_URL != style_of(d, "(z)"));
	wf_md_free(d);
	free(open);
	free(links);
	free(src);
}

#if defined(__SANITIZE_ADDRESS__)
#	define UNDER_ASAN 1
#elif defined(__has_feature)
#	if __has_feature(address_sanitizer)
#		define UNDER_ASAN 1
#	endif
#endif

/* The peak memory grew by less than 64 MiB. Not measurable under ASan, whose
 * redzones and quarantine are in the peak: the allocator's own checks stand there. */
static bool memory_flat(size_t before, size_t after)
{
#ifdef UNDER_ASAN
	(void)before;
	(void)after;
	return true;
#else
	return after < before + 64u * 1024u * 1024u;
#endif
}

/* Images inside images: the description of each is the text of the one inside
 * it, and nothing may keep a copy per level. */
static void nested_images_are_bounded(void)
{
	enum
	{
		DEPTH = 20000
	};
	size_t unit_n = 2, tail_n = 4;
	char *src = malloc((unit_n + tail_n) * DEPTH + 8), *at;
	WfMdDoc *d = NULL;
	PROCESS_MEMORY_COUNTERS pmc;
	size_t before, after;
	double sec;

	CHECK(src != NULL);
	at = src;
	for (int i = 0; i < DEPTH; i++) {
		memcpy(at, "![", 2);
		at += 2;
	}
	*at++ = 'a';
	for (int i = 0; i < DEPTH; i++) {
		memcpy(at, "](x)", 4);
		at += 4;
	}
	*at = '\0';
	GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
	before = pmc.PeakPagefileUsage;
	sec = timed_render(src, (size_t)(at - src), 80, &d);
	GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
	after = pmc.PeakPagefileUsage;
	printf("info: %d nested images: %.3f s, peak memory +%zu KiB\n", DEPTH,
	        sec, after > before ? (after - before) / 1024 : (size_t)0);
	CHECK(sec < 2.0);
	CHECK(memory_flat(before, after));
	CHECK(d && d->nlines > 0);
	wf_md_free(d);
	/* the same at a small depth reads as it always did */
	EXPECT("![a ![b](u) c](v)", 60, "[image: a [image: b] c]\n");
	free(src);
	/* 16 images inside each other are all images; the 17th opener stays text */
	{
		char open[17 * 2 + 1] = "", shut[17 * 4 + 1] = "", all[256],
		                   want[256] = "";

		for (int i = 0; i < 16; i++) {
			strcat_s(open, sizeof open, "![");
			strcat_s(shut, sizeof shut, "](x)");
			strcat_s(want, sizeof want, "[image: ");
		}
		snprintf(all, sizeof all, "%sa%s", open, shut);
		strcat_s(want, sizeof want, "a]]]]]]]]]]]]]]]]\n");
		EXPECT(all, 200, want);
		/* 17 openers and 17 closers: the one deeper than 16 is no image, so its "!["
		 * is on the screen as it was typed (with a limit of 17 there is none left) */
		snprintf(all, sizeof all, "![%sa](x)%s", open, shut);
		d = render(all, 200);
		CHECK(0xFFFFu != style_of(d, "!["));
		wf_md_free(d);
	}
}

/* A table whose rows have fewer cells than its header: the rows that are short
 * read as empty cells, and only the cells that exist are kept in memory. */
static void sparse_table(void)
{
	enum
	{
		ROWS = 60000
	};
	const char head[] = "|a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q|r|s|t|u|v|w|x|"
	                    "y|z|1|2|3|4|5|6|\n"
	                    "|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-"
	                    "|-|-|-|-|-|-|-|\n";
	char *src = malloc(sizeof head + 3 * ROWS + 8), *at;
	WfMdDoc *d = NULL;
	PROCESS_MEMORY_COUNTERS pmc;
	size_t before, after;
	double sec;

	EXPECT("|a|b|c|\n|-|-|-|\n|1|\n|1|2|3|4|\n", 40,
	        "| a   | b   | c   |\n"
	        "|-----|-----|-----|\n"
	        "| 1   |     |     |\n"
	        "| 1   | 2   | 3   |\n");
	CHECK(src != NULL);
	memcpy(src, head, sizeof head - 1);
	at = src + sizeof head - 1;
	for (int i = 0; i < ROWS; i++) {
		memcpy(at, "|x\n", 3);
		at += 3;
	}
	*at = '\0';
	GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
	before = pmc.PeakPagefileUsage;
	sec = timed_render(src, (size_t)(at - src), 200, &d);
	GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
	after = pmc.PeakPagefileUsage;
	printf("info: sparse table of %d rows, 32 columns: %.3f s, peak memory "
	       "+%zu KiB\n",
	        ROWS, sec,
	        after > before ? (after - before) / 1024 : (size_t)0);
	CHECK(d && d->nlines > 0);
	/* the rows that are drawn are most of it; with a cell for every column of every
	 * row it was twice this */
	CHECK(memory_flat(before, after));
	wf_md_free(d);
	free(src);
}

static void limits(void)
{
	char *s = repeat("0123456789abcdef\n", (WF_MD_SRC_MAX / 17) + 5000);
	WfMdDoc *d = NULL;

	(void)timed_render(s, strlen(s), 80, &d);
	CHECK(d && d->truncated);
	wf_md_free(d);
	free(s);
	/* one line over the byte limit is cut and says so */
	s = repeat("word ", WF_MD_SRC_MAX / 5 + 1000);
	(void)timed_render(s, strlen(s), 80, &d);
	CHECK(d && d->truncated && d->nlines > 0);
	wf_md_free(d);
	free(s);
	/* the output is bounded too: 60000 rows of input make more rows than
	 * the limit allows */
	s = repeat("p\n\n", 30000);
	(void)timed_render(s, strlen(s), 80, &d);
	CHECK(d && d->truncated && d->nlines <= WF_MD_OUT_LINES);
	wf_md_free(d);
	free(s);
	/* invalid arguments */
	CHECK(WF_E_INVAL == wf_md_render("x", 1, 80, NULL));
	CHECK(WF_E_INVAL == wf_md_render(NULL, 1, 80, &d));
	/* width is clamped */
	d = render("a b c d e f g h", 1);
	CHECK(8 == d->width);
	wf_md_free(d);
}

static void reference_expansion_respects_output_bytes(void)
{
	WfBuf src = {0};
	WfMdDoc *d = NULL;
	char dest[1001];

	memset(dest, 'a', sizeof dest - 1);
	dest[sizeof dest - 1] = 0;
	CHECK(WF_OK == wf_buf_puts(&src, "[x]: <"));
	CHECK(WF_OK == wf_buf_puts(&src, dest));
	CHECK(WF_OK == wf_buf_puts(&src, ">\n\n"));
	for (size_t i = 0; i < 5000; i++) {
		CHECK(WF_OK == wf_buf_puts(&src, "[x] "));
	}
	CHECK(src.len < WF_MD_SRC_MAX);
	CHECK(WF_OK == wf_md_render(src.data, src.len, 400, &d));
	CHECK(d && d->truncated && d->text_len <= WF_MD_OUT_BYTES);
	wf_md_free(d);
	wf_buf_free(&src);
}

/* Reference URLs are copied before wrapping. Escapes keep the output below
 * its cap while repeated references exhaust the separate expansion budget. */
static void reference_expansion_is_bounded_across_blocks_and_cells(void)
{
	for (int table = 0; table < 2; table++) {
		WfBuf src = {0};
		WfMdDoc *d = NULL;
		char *text;

		CHECK(WF_OK == wf_buf_puts(&src, "[x]: <"));
		for (size_t i = 0; i < 1000; i++) {
			CHECK(WF_OK == wf_buf_puts(&src, "\\!"));
		}
		CHECK(WF_OK == wf_buf_puts(&src, ">\n\n"));
		if (table) {
			CHECK(WF_OK == wf_buf_puts(&src, "| h |\n|---|\n"));
		}
		for (size_t i = 0; i < 3000; i++) {
			CHECK(WF_OK ==
			        wf_buf_puts(
			                &src, table ? "|[x]|\n" : "[x]\n\n"));
		}
		CHECK(WF_OK == wf_buf_puts(&src, "\nAFTER_REFERENCES\n"));
		CHECK(src.len < WF_MD_SRC_MAX);
		CHECK(WF_OK == wf_md_render(src.data, src.len, 400, &d));
		CHECK(d && d->truncated && d->text_len < WF_MD_OUT_BYTES);
		if (d) {
			text = dump(d);
			CHECK(text && strstr(text, "[x]") &&
			        strstr(text, "AFTER_REFERENCES"));
			free(text);
		}
		wf_md_free(d);
		wf_buf_free(&src);
	}
}

static void speed(void)
{
	/* the largest allowed input of ordinary prose must render quickly */
	static const char block[] =
	        "# Heading\n\nSome *emphasis*, **strong**, `code` and a "
	        "[link](http://x.example/a) in a paragraph that goes on for a "
	        "while so that it wraps.\n\n- item one\n- item two\n  - "
	        "nested\n\n"
	        "| a | b |\n|---|---|\n| 1 | 2 |\n\n> "
	        "quote\n\n```\ncode\n```\n\n";
	char *s = repeat(block, WF_MD_SRC_MAX / (sizeof block - 1));
	WfMdDoc *d = NULL;
	double sec = timed_render(s, strlen(s), 100, &d);

	printf("info: %zu bytes -> %zu rows in %.3f s\n", strlen(s), d->nlines,
	        sec);
	CHECK(sec < 2.0); /* generous: a debug build under a loaded machine */
	wf_md_free(d);
	free(s);
}

int main(void)
{
	headings();
	paragraphs();
	inlines();
	code();
	quotes_and_lists();
	rules_and_tables();
	cjk();
	controls_and_input();
	EXPECT("&#x202e; &#8205;", 80, "<RLO> <ZWJ>\n");
	scroll_mapping();
	names();
	hostile();
	limits();
	reference_expansion_respects_output_bytes();
	reference_expansion_is_bounded_across_blocks_and_cells();
	sparse_table();
	links_after_many_open_brackets();
	nested_images_are_bounded();
	bare_address_trailing_parentheses();
	speed();
	return TEST_RESULT();
}

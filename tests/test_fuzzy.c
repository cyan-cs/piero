/* The fuzzy search engine (DR-DRAFT-023): the ranking a person expects, the
 * prefilter, UTF-16 (surrogate pairs, lone surrogates, Japanese), the fold,
 * the programme against its references (rolling = full = the plain score of the
 * returned path; never above a brute force), Top-K against a full sort, a
 * search over a listing, the limits. Pure: no disk, no threads. */
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "search/wf_fuzzy.h"
#include "wf_test.h"

typedef struct
{
	WfDir *dir;
	size_t *vis;
	size_t n;
} Corpus;

static Corpus corpus_make(const wchar_t *const *names, size_t n)
{
	Corpus c = {wf_dir_new(), malloc(n * sizeof(size_t)), n};

	CHECK(c.dir && c.vis);
	for (size_t i = 0; i < n; i++) {
		CHECK(WF_OK ==
		        wf_dir_add(c.dir, names[i], wcslen(names[i]), 0, 0, 0));
		c.vis[i] = i;
	}
	return c;
}

static void corpus_free(Corpus *c)
{
	wf_dir_free(c->dir);
	free(c->vis);
}

/* The names that match `query`, best first, as a "|"-joined string. */
static const wchar_t *ranked(
        const wchar_t *query, const wchar_t *const *names, size_t n, size_t k)
{
	static wchar_t out[2048];
	WfFzQuery q;
	WfFzScratch *x = wf_fz_scratch_new();
	WfFzTopK top;
	Corpus c = corpus_make(names, n);
	WfFzStats st;

	out[0] = L'\0';
	CHECK(x && wf_fz_query_set(&q, query, wcslen(query)));
	CHECK(WF_OK == wf_fz_topk_init(&top, k));
	CHECK(WF_OK ==
	        wf_fz_search(
	                &q, c.dir->e, c.vis, c.n, x, &top, NULL, NULL, &st));
	for (size_t i = 0; i < top.n; i++) {
		if (i) {
			wcscat_s(out, 2048, L"|");
		}
		wcscat_s(out, 2048, names[top.hit[i].index]);
	}
	wf_fz_topk_free(&top);
	wf_fz_scratch_free(x);
	corpus_free(&c);
	return out;
}

#define RANK(q, k, ...)                                                        \
	ranked(q, (const wchar_t *const[]){__VA_ARGS__},                       \
	        sizeof((const wchar_t *const[]){__VA_ARGS__}) /                \
	                sizeof(wchar_t *),                                     \
	        k)

static int32_t score_of(const wchar_t *query, const wchar_t *name)
{
	WfFzQuery q;
	WfFzScratch *x = wf_fz_scratch_new();
	int32_t s;

	CHECK(x && wf_fz_query_set(&q, query, wcslen(query)));
	s = wf_fz_score(&q, name, wcslen(name), x);
	wf_fz_scratch_free(x);
	return s;
}

static void the_example_of_the_request(void)
{
	/* cfg: the compact one first, the long one last */
	CHECK(!wcscmp(RANK(L"cfg", 64, L"cache_file_generator.c",
	                      L"config_file.go", L"config.go"),
	        L"config.go|config_file.go|cache_file_generator.c"));
	/* the order of the list does not decide it */
	CHECK(!wcscmp(RANK(L"cfg", 64, L"config.go", L"config_file.go",
	                      L"cache_file_generator.c"),
	        L"config.go|config_file.go|cache_file_generator.c"));
}

#define CORPUS                                                                 \
	L"config.json", L"config_file.go", L"cache_file_generator.c",          \
	        L"main.c", L"main_test.c", L"README.md", L"read_me.txt",       \
	        L"HTTPServer.c", L"http_server.c"

static void exact_and_prefix(void)
{
	CHECK(!wcscmp(RANK(L"main.c", 64, CORPUS), L"main.c|main_test.c"));
	CHECK(!wcscmp(RANK(L"main", 64, CORPUS), L"main.c|main_test.c"));
	/* a prefix beats the same letters inside a word */
	CHECK(!wcscmp(
	        RANK(L"log", 64, L"changelog.md", L"log.txt", L"catalog.c"),
	        L"log.txt|catalog.c|changelog.md"));
	CHECK(!wcscmp(RANK(L"readme", 64, CORPUS), L"README.md|read_me.txt"));
}

static void word_boundaries(void)
{
	/* snake_case and kebab-case word starts */
	CHECK(!wcscmp(RANK(L"fb", 64, L"foobar.txt", L"foo-bar.txt"),
	        L"foo-bar.txt|foobar.txt"));
	CHECK(!wcscmp(RANK(L"ft", 64, L"filetools.c", L"file_tools.c"),
	        L"file_tools.c|filetools.c"));
	/* mt: the word starts of main_test.c, not the middle of read_me.txt */
	CHECK(!wcscmp(RANK(L"mt", 64, CORPUS), L"main_test.c|read_me.txt"));
	/* hs: the word start after '_' beats an upper case letter that follows
	 * upper case letters */
	CHECK(!wcscmp(RANK(L"hs", 64, L"HTTPServer.c", L"http_server.c"),
	        L"http_server.c|HTTPServer.c"));
}

static void camel_case(void)
{
	CHECK(!wcscmp(RANK(L"ps", 64, L"passwords.txt", L"parts.txt",
	                      L"ParseString.c"),
	        L"ParseString.c|parts.txt|passwords.txt"));
	CHECK(!wcscmp(RANK(L"gs", 64, L"gist.txt", L"getString.c"),
	        L"getString.c|gist.txt"));
}

static void extension(void)
{
	CHECK(!wcscmp(RANK(L"py", 64, L"copy.txt", L"a.py", L"happy.c"),
	        L"a.py|copy.txt|happy.c"));
	CHECK(!wcscmp(RANK(L"go", 64, L"config.go", L"gopher.txt", L"xgox"),
	        L"gopher.txt|config.go|xgox"));
}

static void ties_keep_the_order_of_the_list(void)
{
	/* same score: the earlier row first, both ways round */
	CHECK(!wcscmp(RANK(L"a", 64, L"x_a", L"y_a"), L"x_a|y_a"));
	CHECK(!wcscmp(RANK(L"a", 64, L"y_a", L"x_a"), L"y_a|x_a"));
	/* case does not change the score of an otherwise equal name */
	CHECK(!wcscmp(
	        RANK(L"http", 64, CORPUS), L"HTTPServer.c|http_server.c"));
	CHECK(score_of(L"cfg", L"CONFIG.GO") != WF_FZ_NO_MATCH);
}

static void no_match_and_empty(void)
{
	WfFzQuery q;

	CHECK(!wcscmp(RANK(L"zzz", 64, CORPUS), L""));
	CHECK(!wcscmp(RANK(L"cfg", 64, L"dog.txt"), L""));
	CHECK(WF_FZ_NO_MATCH == score_of(L"cfg", L"dog.txt"));
	/* an empty query is not a query */
	CHECK(!wf_fz_query_set(&q, L"", 0) && 0 == q.n);
}

/* Each line is a sum that can be followed on paper: match 16, the bonus of the
 * place (the first query character's doubled), a gap -3 then -1, one point for each
 * position before the first match (8 at most) and for each character of the name
 * beyond the query (64 at most). */
static void the_score_numbers_are_pinned(void)
{
	/* a b on "ab": (16 + 10*2) + (16 + 10, the run keeps the start's bonus) */
	CHECK(62 == score_of(L"ab", L"ab"));
	/* x a b: a at 1 (16, one position in) then b in the run (16 + 4), minus 1 */
	CHECK(34 == score_of(L"ab", L"xab"));
	/* x a x b: a (16 - 1), a gap of one (-3), b (16), minus 2 */
	CHECK(26 == score_of(L"ab", L"xaxb"));
	/* a gap of three costs -3 -1 -1 */
	CHECK(44 == score_of(L"ae", L"abbbe"));
	/* word starts: after '-' 8, after a space 10, after a path separator 9 */
	CHECK(46 == score_of(L"fb", L"foo-bar.txt"));
	CHECK(52 == score_of(L"fb", L"foo bar"));
	CHECK(30 == score_of(L"b", L"a\\b"));
	/* camelCase and letter-to-digit: 7 */
	CHECK(42 == score_of(L"ps", L"ParseString.c"));
	CHECK(37 == score_of(L"b2", L"ab2"));
	/* the same letters without a word start */
	CHECK(38 == score_of(L"ps", L"passwords.txt"));
	/* the first match counts only the first 8 positions, the length 64 */
	CHECK(-2 == score_of(L"z", L"aaaaaaaaaaz"));
	{
		wchar_t longer[120];

		longer[0] = L'z';
		for (int i = 1; i < 100; i++) {
			longer[i] = L'a';
		}
		longer[100] = L'\0';
		CHECK(-28 == score_of(L"z", longer));
	}
	CHECK(61 == score_of(L"cfg", L"config.go"));
}

static void japanese(void)
{
	CHECK(!wcscmp(RANK(L"設定", 64, L"ファイル設定.txt",
	                      L"設定ファイル.txt", L"readme.txt"),
	        L"設定ファイル.txt|ファイル設定.txt"));
	/* Hiragana finds Katakana and the other way round */
	CHECK(score_of(L"ふぁいる", L"ファイル.txt") != WF_FZ_NO_MATCH);
	CHECK(score_of(L"ファイル", L"ふぁいる.txt") != WF_FZ_NO_MATCH);
	/* Hiragana and Katakana are not the same as unrelated kana */
	CHECK(WF_FZ_NO_MATCH == score_of(L"ふぁいる", L"ファイロ.txt"));
	/* full-width Latin letters and digits */
	CHECK(score_of(L"abc1", L"ＡＢＣ１.txt") != WF_FZ_NO_MATCH);
	CHECK(score_of(L"ＡＢＣ", L"abc.txt") != WF_FZ_NO_MATCH);
}

static void other_scripts(void)
{
	CHECK(score_of(L"café", L"CAFÉ.txt") != WF_FZ_NO_MATCH);
	CHECK(score_of(L"CAFÉ", L"café.txt") != WF_FZ_NO_MATCH);
	CHECK(score_of(L"привет", L"ПРИВЕТ.txt") != WF_FZ_NO_MATCH);
	CHECK(score_of(L"αβγ", L"ΑΒΓ.txt") != WF_FZ_NO_MATCH);
	CHECK(score_of(L"łódź", L"ŁÓDŹ.txt") != WF_FZ_NO_MATCH);
	/* what is not claimed: no accent removal, no normalization */
	CHECK(WF_FZ_NO_MATCH == score_of(L"cafe", L"Café.txt"));
	CHECK(WF_FZ_NO_MATCH == score_of(L"cafe\x0301", L"caf\x00e9.txt"));
	/* a character that is not folded matches only itself */
	CHECK(WF_FZ_NO_MATCH == score_of(L"日", L"本.txt"));
	CHECK(wf_fz_fold(L'A') == L'a' && wf_fz_fold(L'a') == L'a');
	CHECK(wf_fz_fold(0xD7) == 0xD7 && wf_fz_fold(0xF7) == 0xF7);
	CHECK(wf_fz_fold(0x30FC) == 0x30FC); /* the long vowel mark stays */
	CHECK(wf_fz_fold(0x1F600) == 0x1F600);
}

static void surrogate_pairs(void)
{
	WfFzQuery q;
	WfFzScratch *x = wf_fz_scratch_new();
	uint32_t pos[WF_FZ_QUERY_MAX];
	int32_t score = 0;
	const wchar_t *name = L"\xD842\xDFB7野家.txt"; /* U+20BB7 野 家 */

	CHECK(x);
	/* the characters 野家 begin at units 2 and 3 */
	CHECK(wf_fz_query_set(&q, L"野家", 2));
	CHECK(wf_fz_positions(&q, name, wcslen(name), x, pos, &score));
	CHECK(2 == pos[0] && 3 == pos[1]);
	CHECK(score == wf_fz_score(&q, name, wcslen(name), x));
	/* the pair itself: one character, two units, starting at 0 */
	CHECK(wf_fz_query_set(&q, L"\xD842\xDFB7", 2) && 1 == q.n);
	CHECK(wf_fz_positions(&q, name, wcslen(name), x, pos, &score));
	CHECK(0 == pos[0]);
	CHECK(2 == wf_fz_cp_units(name, wcslen(name), pos[0]));
	/* half a pair is a character of its own and does not match inside one */
	CHECK(wf_fz_query_set(&q, L"\xD842", 1) && 1 == q.n);
	CHECK(WF_FZ_NO_MATCH == wf_fz_score(&q, name, wcslen(name), x));
	CHECK(wf_fz_query_set(&q, L"\xDFB7", 1) && 1 == q.n);
	CHECK(WF_FZ_NO_MATCH == wf_fz_score(&q, name, wcslen(name), x));
	/* ... but it matches the same lone unit, and nothing is split */
	{
		const wchar_t *lone = L"a\xD842z.txt";

		CHECK(wf_fz_query_set(&q, L"\xD842z", 2) && 2 == q.n);
		CHECK(wf_fz_positions(&q, lone, wcslen(lone), x, pos, NULL));
		CHECK(1 == pos[0] && 2 == pos[1]);
		CHECK(1 == wf_fz_cp_units(lone, wcslen(lone), 1));
	}
	/* an emoji (D83D DE00) in the middle of the letters */
	{
		const wchar_t *e = L"a\xD83D\xDE00"
		                   L"b";

		CHECK(wf_fz_query_set(&q, L"ab", 2));
		CHECK(wf_fz_positions(&q, e, wcslen(e), x, pos, NULL));
		CHECK(0 == pos[0] && 3 == pos[1]);
	}
	/* a query of one pair is one character, not two */
	CHECK(wf_fz_query_set(&q, L"\xD83D\xDE00", 2) && 1 == q.n);
	wf_fz_scratch_free(x);
}

static void limits(void)
{
	WfFzQuery q;
	wchar_t big[WF_FZ_QUERY_MAX + 8], name[WF_FZ_NAME_MAX * 2 + 16];
	WfFzScratch *x = wf_fz_scratch_new();
	size_t i;

	CHECK(x);
	for (i = 0; i < WF_FZ_QUERY_MAX + 8; i++) {
		big[i] = L'a';
	}
	CHECK(wf_fz_query_set(&q, big, WF_FZ_QUERY_MAX) &&
	        WF_FZ_QUERY_MAX == q.n);
	CHECK(!wf_fz_query_set(&q, big, WF_FZ_QUERY_MAX + 1) && 0 == q.n);
	/* the longest query finds a name that holds it */
	for (i = 0; i < WF_FZ_QUERY_MAX; i++) {
		name[i] = L'a';
	}
	name[WF_FZ_QUERY_MAX] = L'\0';
	CHECK(wf_fz_query_set(&q, big, WF_FZ_QUERY_MAX));
	CHECK(WF_FZ_NO_MATCH != wf_fz_score(&q, name, WF_FZ_QUERY_MAX, x));
	/* shorter than the query: no match, no read beyond the name */
	CHECK(WF_FZ_NO_MATCH == wf_fz_score(&q, name, WF_FZ_QUERY_MAX - 1, x));
	/* a name longer than the limit: the letters inside it are found, the ones
	 * only beyond it are not (the prefilter passes, the scoring does not) */
	for (i = 0; i < WF_FZ_NAME_MAX * 2; i++) {
		name[i] = L'x';
	}
	name[10] = L'q';
	name[WF_FZ_NAME_MAX * 2] = L'\0';
	CHECK(wf_fz_query_set(&q, L"q", 1));
	CHECK(WF_FZ_NO_MATCH != wf_fz_score(&q, name, WF_FZ_NAME_MAX * 2, x));
	name[10] = L'x';
	name[WF_FZ_NAME_MAX + 5] = L'q';
	CHECK(wf_fz_prefilter(&q, name, WF_FZ_NAME_MAX * 2));
	CHECK(WF_FZ_NO_MATCH == wf_fz_score(&q, name, WF_FZ_NAME_MAX * 2, x));
	/* the last character that is scored is the 256th */
	name[WF_FZ_NAME_MAX + 5] = L'x';
	name[WF_FZ_NAME_MAX - 1] = L'q';
	CHECK(WF_FZ_NO_MATCH != wf_fz_score(&q, name, WF_FZ_NAME_MAX * 2, x));
	/* an empty name never matches */
	CHECK(WF_FZ_NO_MATCH == wf_fz_score(&q, L"", 0, x));
	wf_fz_scratch_free(x);
}

static uint32_t rng_state = 12345;

static uint32_t rnd(uint32_t n)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return (rng_state >> 8) % n;
}

/* A random name over an alphabet that has every kind of character: Latin
 * in both cases, digits, symbols, kana, a Han character, pairs, lone units. */
static size_t random_name(wchar_t *out, size_t max_cp)
{
	static const wchar_t *const pool[] = {L"a", L"b", L"c", L"A", L"B",
	        L"_", L"-", L".", L" ", L"1", L"é", L"É", L"日", L"カ", L"か",
	        L"\xD842\xDFB7", L"\xD83D\xDE00", L"\xD800", L"\xDC00", L"/",
	        L"b", L"a"};
	size_t n = 1 + rnd((uint32_t)max_cp), len = 0;

	for (size_t i = 0; i < n; i++) {
		const wchar_t *p = pool[rnd(sizeof pool / sizeof *pool)];

		out[len++] = p[0];
		if (p[1]) {
			out[len++] = p[1];
		}
	}
	out[len] = L'\0';
	return len;
}

/* The start of each character of a name and its folded value. */
static size_t cps_of(const wchar_t *s, size_t len, uint32_t *at, uint32_t *fold)
{
	size_t i = 0, n = 0;

	while (i < len) {
		size_t u = wf_fz_cp_units(s, len, i);
		uint32_t c = (uint16_t)s[i];

		if (2 == u) {
			c = 0x10000u + ((c - 0xD800u) << 10) +
			        ((uint16_t)s[i + 1] - 0xDC00u);
		}
		at[n] = (uint32_t)i;
		fold[n] = wf_fz_fold(c);
		n++;
		i += u;
	}
	return n;
}

/* The best score of any path, by trying all of them (short names only). */
static int32_t brute(const WfFzQuery *q,
        const wchar_t *s,
        size_t len,
        WfFzScratch *x,
        const uint32_t *at,
        const uint32_t *fold,
        size_t ncp,
        size_t qi,
        size_t from,
        uint32_t *units)
{
	int32_t best = WF_FZ_NO_MATCH;

	if (qi == q->n) {
		return wf_fz_path_score(q, s, len, x, units);
	}
	for (size_t j = from; j < ncp; j++) {
		if (fold[j] == q->cp[qi]) {
			int32_t v;

			units[qi] = at[j];
			v = brute(q, s, len, x, at, fold, ncp, qi + 1, j + 1,
			        units);
			if (v > best) {
				best = v;
			}
		}
	}
	return best;
}

static void the_programme_agrees_with_its_references(void)
{
	WfFzScratch *x = wf_fz_scratch_new(), *y = wf_fz_scratch_new();
	unsigned cases = 0, matches = 0, optimal = 0;

	CHECK(x && y);
	for (unsigned iter = 0; iter < 20000; iter++) {
		wchar_t name[64], qs[16];
		uint32_t at[64], fold[64], units[WF_FZ_QUERY_MAX];
		uint32_t bu[WF_FZ_QUERY_MAX];
		size_t len = random_name(name, 9), ncp, qn = 0;
		WfFzQuery q;
		int32_t rolling, full = 0, path, bf;
		bool prefilter, pos;

		ncp = cps_of(name, len, at, fold);
		/* a query made of some of the name's own characters, or noise */
		if (rnd(4)) {
			size_t want = 1 + rnd(4);

			for (size_t j = 0; j < ncp && qn < want; j++) {
				if (rnd(2)) {
					size_t u = wf_fz_cp_units(
					        name, len, at[j]);

					qs[qn++] = name[at[j]];
					if (2 == u) {
						qs[qn++] = name[at[j] + 1];
					}
				}
			}
			if (0 == qn) {
				qs[qn++] = name[0];
				if (2 == wf_fz_cp_units(name, len, 0)) {
					qs[qn++] = name[1];
				}
			}
		} else {
			qn = random_name(qs, 3);
		}
		if (!wf_fz_query_set(&q, qs, qn)) {
			continue;
		}
		cases++;
		prefilter = wf_fz_prefilter(&q, name, len);
		rolling = wf_fz_score(&q, name, len, x);
		pos = wf_fz_positions(&q, name, len, y, units, &full);
		/* nothing passes the prefilter without being a subsequence, and a
		 * score is only given after it */
		if (WF_FZ_NO_MATCH != rolling) {
			CHECK(prefilter);
		}
		CHECK((WF_FZ_NO_MATCH != rolling) == pos);
		if (WF_FZ_NO_MATCH == rolling) {
			/* with these short names the window always holds the
			 * letters, so the prefilter is the whole answer */
			CHECK(!prefilter);
			continue;
		}
		matches++;
		CHECK(rolling == full); /* rolling rows = the full matrix */
		/* the path read back is a real path of the characters, rising,
		 * starting at the start of a character */
		for (size_t i = 0; i < q.n; i++) {
			bool starts = false;

			for (size_t j = 0; j < ncp; j++) {
				starts |=
				        at[j] == units[i] && fold[j] == q.cp[i];
			}
			CHECK(starts);
			if (i) {
				CHECK(units[i] > units[i - 1]);
			}
		}
		path = wf_fz_path_score(&q, name, len, x, units);
		CHECK(path ==
		        rolling); /* the plain rules give the same score */
		bf = brute(&q, name, len, x, at, fold, ncp, 0, 0, bu);
		CHECK(bf >= rolling); /* never above the best possible */
		optimal += bf == rolling;
	}
	printf("fuzzy: %u cases, %u matches, programme optimal in %u\n", cases,
	        matches, optimal);
	CHECK(matches > 3000);
	/* it is the fzf V2 programme: nearly always the best path (the run bonus
	 * depends on the path, so it is not exact in every case) */
	CHECK(optimal * 100u >= matches * 97u);
	wf_fz_scratch_free(x);
	wf_fz_scratch_free(y);
}

static void random_input_never_breaks_the_buffers(void)
{
	WfFzScratch *x = wf_fz_scratch_new();
	uint32_t units[WF_FZ_QUERY_MAX];

	CHECK(x);
	for (unsigned iter = 0; iter < 3000; iter++) {
		wchar_t name[700], qs[WF_FZ_QUERY_MAX + 2];
		size_t len = 0, qn = 0;
		WfFzQuery q;

		/* any units at all, whole names up to well past the limit */
		for (size_t n = rnd(700); len < n; len++) {
			name[len] = (wchar_t)(rnd(4) ? 0x20 + rnd(0x60)
			                             : rnd(0x10000));
		}
		for (size_t n = rnd(WF_FZ_QUERY_MAX + 2); qn < n; qn++) {
			qs[qn] = (wchar_t)(rnd(3) ? 0x20 + rnd(0x60)
			                          : rnd(0x10000));
		}
		if (qn && rnd(2) && len) {
			/* make a match likely: copy a few units of the name */
			qn = 1 + rnd(8);
			for (size_t i = 0; i < qn && i < WF_FZ_QUERY_MAX; i++) {
				qs[i] = name[rnd((uint32_t)len)];
			}
		}
		if (!wf_fz_query_set(&q, qs, qn)) {
			continue;
		}
		{
			int32_t a = wf_fz_score(&q, name, len, x), b = 0;
			bool ok = wf_fz_positions(&q, name, len, x, units, &b);

			CHECK((WF_FZ_NO_MATCH != a) == ok);
			if (ok) {
				CHECK(a == b);
				for (size_t i = 0; i < q.n; i++) {
					CHECK(units[i] < len);
					CHECK(wf_fz_cp_units(
					              name, len, units[i]) > 0);
				}
			}
		}
	}
	wf_fz_scratch_free(x);
}

static int by_rank(const void *a, const void *b)
{
	WfFzHit x = *(const WfFzHit *)a, y = *(const WfFzHit *)b;

	return wf_fz_better(x, y) ? -1 : wf_fz_better(y, x) ? 1 : 0;
}

static void topk_equals_the_head_of_a_full_sort(void)
{
	enum
	{
		N = 3000
	};
	static WfFzHit all[N], sorted[N];
	static const size_t ks[] = {0, 1, 2, 5, 64, 1000, N, N + 100};

	for (size_t i = 0; i < N; i++) {
		/* few distinct scores, so that there are many ties */
		all[i] = (WfFzHit){(int32_t)rnd(40) - 10, (uint32_t)i};
	}
	memcpy(sorted, all, sizeof all);
	qsort(sorted, N, sizeof sorted[0], by_rank);
	for (size_t t = 0; t < sizeof ks / sizeof *ks; t++) {
		WfFzTopK top;
		size_t want = ks[t] < N ? ks[t] : N;

		CHECK(WF_OK == wf_fz_topk_init(&top, ks[t]));
		for (size_t i = 0; i < N; i++) {
			wf_fz_topk_offer(&top, all[i]);
		}
		wf_fz_topk_finish(&top);
		CHECK(top.n == want);
		for (size_t i = 0; i < top.n; i++) {
			CHECK(top.hit[i].score == sorted[i].score &&
			        top.hit[i].index == sorted[i].index);
		}
		wf_fz_topk_free(&top);
	}
	/* the order is a strict order: equal score, lower index first */
	CHECK(wf_fz_better((WfFzHit){5, 1}, (WfFzHit){5, 2}));
	CHECK(!wf_fz_better((WfFzHit){5, 2}, (WfFzHit){5, 1}));
	CHECK(wf_fz_better((WfFzHit){6, 9}, (WfFzHit){5, 0}));
	CHECK(!wf_fz_better((WfFzHit){5, 3}, (WfFzHit){5, 3}));
	/* a K that cannot be allocated is refused and leaves nothing */
	{
		WfFzTopK top;

		CHECK(WF_OK != wf_fz_topk_init(&top, SIZE_MAX / 4));
		CHECK(0 == top.k && NULL == top.hit);
	}
}

static void a_search_counts_what_it_saw(void)
{
	static const wchar_t *const names[] = {CORPUS, L"dog.txt"};
	size_t vis[] = {
	        9, 8, 7, 6, 5, 4, 3, 2, 1, 0}; /* the list's own order */
	Corpus c = corpus_make(names, 10);
	WfFzQuery q;
	WfFzScratch *x = wf_fz_scratch_new();
	WfFzTopK top;
	WfFzStats st;

	CHECK(x && wf_fz_query_set(&q, L"cfg", 3));
	CHECK(WF_OK == wf_fz_topk_init(&top, 2));
	CHECK(WF_OK ==
	        wf_fz_search(&q, c.dir->e, vis, 10, x, &top, NULL, NULL, &st));
	/* ten names asked about, the two that hold c, f, g in order are config_file
	 * and cache_file_generator, and config.json */
	CHECK(10 == st.scanned && !st.cancelled);
	/* the prefilter lets through exactly the three names that hold c, f, g in
	 * order; dog.txt and the rest never reach the scoring */
	CHECK(3 == st.passed && 3 == st.matched);
	CHECK(2 == top.n);
	/* hits carry the row of the list asked about (vis order), not the entry */
	for (size_t i = 0; i < top.n; i++) {
		CHECK(top.hit[i].index < 10);
		CHECK(WF_FZ_NO_MATCH !=
		        score_of(L"cfg", names[vis[top.hit[i].index]]));
	}
	wf_fz_topk_free(&top);
	/* a bad call is refused */
	CHECK(WF_OK == wf_fz_topk_init(&top, 2));
	CHECK(WF_E_INVAL ==
	        wf_fz_search(&q, c.dir->e, NULL, 3, x, &top, NULL, NULL, &st));
	wf_fz_topk_free(&top);
	wf_fz_scratch_free(x);
	corpus_free(&c);
}

static unsigned cancel_calls, cancel_after;

static bool cancel_cb(void *ctx)
{
	(void)ctx;
	return ++cancel_calls > cancel_after;
}

static void a_search_stops_when_asked(void)
{
	wchar_t(*names)[16] = malloc(200 * sizeof *names);
	const wchar_t *p[200];
	Corpus c;
	WfFzQuery q;
	WfFzScratch *x = wf_fz_scratch_new();
	WfFzTopK top;
	WfFzStats st;

	CHECK(names && x);
	for (int i = 0; i < 200; i++) {
		swprintf_s(names[i], 16, L"file%03d.txt", i);
		p[i] = names[i];
	}
	c = corpus_make(p, 200);
	CHECK(wf_fz_query_set(&q, L"file", 4));
	/* asked every 32 names: the third look says stop, after 64 names */
	cancel_calls = 0;
	cancel_after = 2;
	CHECK(WF_OK == wf_fz_topk_init(&top, 5));
	CHECK(WF_OK ==
	        wf_fz_search(&q, c.dir->e, c.vis, 200, x, &top, cancel_cb, NULL,
	                &st));
	CHECK(st.cancelled && 64 == st.scanned && 3 == cancel_calls);
	wf_fz_topk_free(&top);
	/* never asked to stop: all of them */
	cancel_calls = 0;
	cancel_after = 1000;
	CHECK(WF_OK == wf_fz_topk_init(&top, 5));
	CHECK(WF_OK ==
	        wf_fz_search(&q, c.dir->e, c.vis, 200, x, &top, cancel_cb, NULL,
	                &st));
	CHECK(!st.cancelled && 200 == st.scanned && 200 == st.matched);
	CHECK(7 == cancel_calls); /* 0, 32, ... 192 */
	/* the five kept are the first five rows (equal scores, list order) */
	for (size_t i = 0; i < top.n; i++) {
		CHECK(top.hit[i].index == i);
	}
	wf_fz_topk_free(&top);
	wf_fz_scratch_free(x);
	corpus_free(&c);
	free(names);
}

/* What one search over a large listing costs, and what asking for more results
 * of the same letters costs (the screen asks again with four times as many when
 * the list is scrolled to its end). Printed: the decision about keeping the
 * scored names between the requests rests on these numbers. */
static void what_a_search_costs(void)
{
	enum
	{
		N = 200000
	};
	WfDir *d = wf_dir_new();
	size_t *vis = malloc(N * sizeof *vis);
	WfFzScratch *x = wf_fz_scratch_new();
	WfFzQuery q;
	uint32_t seed = 99;
	static const size_t ks[] = {64, 256, 1024};

	CHECK(d && vis && x && wf_fz_query_set(&q, L"rep", 3));
	d->limit = N + 1;
	d->no_display = true;
	for (size_t i = 0; i < N; i++) {
		wchar_t name[40];

		seed = seed * 1664525u + 1013904223u;
		swprintf(name, 40, L"%c%c%c-report-%06u-%c%c.txt",
		        L'a' + (seed >> 24) % 26, L'a' + (seed >> 19) % 26,
		        L'a' + (seed >> 14) % 26, seed >> 8,
		        L'a' + (seed >> 5) % 26, L'a' + seed % 26);
		CHECK(WF_OK == wf_dir_add(d, name, wcslen(name), 0, 0, 0));
		vis[i] = i;
	}
	for (size_t i = 0; i < sizeof ks / sizeof *ks; i++) {
		WfFzTopK top;
		WfFzStats st;
		clock_t t0 = clock();

		CHECK(WF_OK == wf_fz_topk_init(&top, ks[i]));
		CHECK(WF_OK ==
		        wf_fz_search(
		                &q, d->e, vis, N, x, &top, NULL, NULL, &st));
		printf("info: search of %d names for \"rep\", the best %zu: "
		       "%.1f ms (%zu matched)\n",
		        (int)N, ks[i],
		        (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC,
		        (size_t)st.matched);
		wf_fz_topk_free(&top);
	}
	{ /* the matched letters of the rows on screen, as a frame works them out */
		uint32_t units[WF_FZ_QUERY_MAX];
		enum
		{
			ROWS = 40,
			FRAMES = 2000
		};
		clock_t t0 = clock();

		for (int f = 0; f < FRAMES; f++) {
			for (int r = 0; r < ROWS; r++) {
				const wchar_t *name =
				        d->e[(size_t)r * 997].name;

				CHECK(wf_fz_positions(&q, name, wcslen(name), x,
				        units, NULL));
			}
		}
		printf("info: the matched letters of %d rows: %.3f ms a "
		       "frame\n",
		        (int)ROWS,
		        (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC /
		                (double)FRAMES);
	}
	wf_fz_scratch_free(x);
	wf_dir_free(d);
	free(vis);
}

int main(void)
{
	the_example_of_the_request();
	what_a_search_costs();
	exact_and_prefix();
	word_boundaries();
	camel_case();
	extension();
	ties_keep_the_order_of_the_list();
	no_match_and_empty();
	the_score_numbers_are_pinned();
	japanese();
	other_scripts();
	surrogate_pairs();
	limits();
	the_programme_agrees_with_its_references();
	random_input_never_breaks_the_buffers();
	topk_equals_the_head_of_a_full_sort();
	a_search_counts_what_it_saw();
	a_search_stops_when_asked();
	return TEST_RESULT();
}

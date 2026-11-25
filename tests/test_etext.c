/* The editor's text buffer against a flat byte string: whatever is inserted
 * and deleted, the buffer must hold exactly the bytes the flat string holds,
 * with its invariants intact. */
#include <stdlib.h>
#include <string.h>

#include "edit/wf_etext.h"
#include "wf_test.h"
#include "base/wf_macro.h"

static uint32_t rng_state = 12345;

static uint32_t rnd(uint32_t n)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return (rng_state >> 8) % n;
}

/* Everything the buffer holds, as raw bytes. */
static char *dump(const WfEText *t, size_t *n)
{
	char *out = NULL;

	CHECK(WF_OK == wf_et_read(t, (WfPos){0, 0}, wf_et_end(t), &out, n));
	return out;
}

static bool holds(const WfEText *t, const char *flat, size_t flat_n)
{
	size_t n = 0;
	char *got = dump(t, &n);
	bool same = got && n == flat_n && !memcmp(got, flat, n) &&
	        n == t->bytes && wf_et_check(t);

	free(got);
	return same;
}

/* A random position on a scalar boundary, and its offset in the flat bytes. */
static WfPos random_pos(const WfEText *t, size_t *offset)
{
	WfPos p;
	const WfELine *l;
	size_t i, off = 0, col = 0;

	p.line = rnd((uint32_t)t->n);
	l = &t->lines[p.line];
	while (col < l->len && rnd(3)) { /* random walk over scalars */
		col = wf_et_col_next(l, col);
	}
	p.col = col;
	for (i = 0; i < p.line; i++) {
		off += t->lines[i].len + wf_eol_len(t->lines[i].eol);
	}
	*offset = off + p.col;
	return p;
}

static const char *const PIECES[] = {"a", "b", "xyz", "\xC3\xA9",
        "\xE6\x97\xA5", "\xF0\x9F\x98\x80", "e\xCC\x81", "\t", " ", "\n",
        "\r\n", "\r", "a\nb", "one\r\ntwo\rthree\nfour"};

static char *random_text(size_t *n)
{
	char *s = malloc(256);
	size_t k = rnd(4), len = 0, i;

	CHECK(NULL != s);
	for (i = 0; i < k; i++) {
		const char *p = PIECES[rnd(WF_ARRAY_LEN(PIECES))];

		memcpy(s + len, p, strlen(p));
		len += strlen(p);
	}
	*n = len;
	return s;
}

static void random_edits_match_the_flat_model(void)
{
	WfEText t;
	char *flat = malloc(1 << 16);
	size_t flat_n = 0, step;

	CHECK(NULL != flat);
	CHECK(WF_OK == wf_et_init(&t, WF_EOL_CRLF, 0));
	for (step = 0; step < 4000; step++) {
		size_t off, off2, n;

		if (0 != rnd(3) || 0 == flat_n) {
			WfPos at = random_pos(&t, &off);
			char *s = random_text(&n);
			WfPos end;

			if (flat_n + n > (1 << 16) - 8) {
				free(s);
				continue;
			}
			CHECK(WF_OK == wf_et_insert(&t, at, s, n, false, &end));
			memmove(flat + off + n, flat + off, flat_n - off);
			memcpy(flat + off, s, n);
			flat_n += n;
			free(s);
			CHECK(wf_et_valid(&t, end));
		} else {
			WfPos a = random_pos(&t, &off),
			      b = random_pos(&t, &off2);
			char *gone = NULL;
			size_t gone_n = 0;

			if (wf_pos_cmp(a, b) > 0) {
				WfPos tp = a;
				size_t to = off;

				a = b;
				b = tp;
				off = off2;
				off2 = to;
			}
			CHECK(WF_OK == wf_et_delete(&t, a, b, &gone, &gone_n));
			CHECK(gone_n == off2 - off &&
			        !memcmp(gone ? gone : "", flat + off, gone_n));
			free(gone);
			memmove(flat + off, flat + off2, flat_n - off2);
			flat_n -= off2 - off;
		}
		if (!holds(&t, flat, flat_n)) {
			CHECK_FAIL("buffer differs from the flat model");
			break;
		}
	}
	wf_et_free(&t);
	free(flat);
}

static void load_and_dump_are_inverse(void)
{
	static const char *const SAMPLES[] = {"", "a", "a\n", "\n", "\r\n",
	        "a\r\nb\nc\rd", "a\r\n\r\nb\r\n", "\n\n\n", "x\r",
	        "\xE6\x97\xA5\r\n"};
	size_t i;

	for (i = 0; i < WF_ARRAY_LEN(SAMPLES); i++) {
		WfEText t;
		size_t n = strlen(SAMPLES[i]);

		CHECK(WF_OK == wf_et_init(&t, WF_EOL_LF, 0));
		CHECK(WF_OK == wf_et_load(&t, SAMPLES[i], n));
		CHECK(holds(&t, SAMPLES[i], n));
		wf_et_free(&t);
	}
	/* the shape the model promises: a final terminator leaves an empty last
	 * line, a missing one does not */
	{
		WfEText t;

		CHECK(WF_OK == wf_et_init(&t, WF_EOL_LF, 0));
		CHECK(WF_OK == wf_et_load(&t, "a\nb\n", 4) && 3 == t.n &&
		        0 == t.lines[2].len && WF_EOL_NONE == t.lines[2].eol);
		CHECK(WF_OK == wf_et_load(&t, "a\nb", 3) && 2 == t.n &&
		        1 == t.lines[1].len);
		CHECK(WF_OK == wf_et_load(&t, "a\r\nb\nc", 6) &&
		        WF_EOL_CRLF == t.lines[0].eol &&
		        WF_EOL_LF == t.lines[1].eol);
		wf_et_free(&t);
	}
}

/* A typed or pasted break takes the terminator of the line it splits. */
static void normalized_breaks_follow_the_line(void)
{
	WfEText t;
	WfPos end;
	size_t n;
	char *got;

	CHECK(WF_OK == wf_et_init(&t, WF_EOL_LF, 0));
	CHECK(WF_OK == wf_et_load(&t, "ab\r\ncd", 6));
	CHECK(WF_OK ==
	        wf_et_insert(&t, (WfPos){0, 1}, "X\nY\rZ\r\nW", 8, true, &end));
	got = dump(&t, &n);
	CHECK(16 == n && !memcmp(got, "aX\r\nY\r\nZ\r\nWb\r\ncd", 16));
	free(got);
	CHECK(3 == end.line && 1 == end.col);
	/* the last line has none: the default terminator is used */
	CHECK(WF_OK == wf_et_insert(&t, wf_et_end(&t), "\n", 1, true, NULL));
	CHECK(WF_EOL_LF == t.lines[t.n - 2].eol);
	CHECK(wf_et_check(&t));
	wf_et_free(&t);
}

static void the_size_limit_changes_nothing(void)
{
	WfEText t;
	size_t n;
	char *before, *after;

	CHECK(WF_OK == wf_et_init(&t, WF_EOL_LF, 10));
	CHECK(WF_OK ==
	        wf_et_insert(&t, (WfPos){0, 0}, "12345678", 8, false, NULL));
	before = dump(&t, &n);
	CHECK(WF_E_RANGE ==
	        wf_et_insert(&t, (WfPos){0, 3}, "abc", 3, false, NULL));
	CHECK(WF_OK == wf_et_insert(&t, (WfPos){0, 3}, "\n\n", 2, false, NULL));
	CHECK(WF_E_RANGE ==
	        wf_et_insert(&t, (WfPos){0, 3}, "\n", 1, false, NULL));
	after = dump(&t, &n);
	CHECK(10 == n && wf_et_check(&t));
	free(before);
	free(after);
	CHECK(WF_E_RANGE == wf_et_load(&t, "0123456789ab", 12));
	CHECK(wf_et_check(&t) && 10 == t.bytes);
	wf_et_free(&t);
}

/* An emoji with a skin tone is one step. A zero width joiner is not folded
 * into its neighbours: it is shown as a visible <ZWJ> label on purpose (it
 * can disguise a name), so it is a step of its own, as on screen. */
static void emoji_with_a_skin_tone_is_one_step(void)
{
	WfEText t;
	/* a, thumbs up + medium skin tone, z */
	const char text[] = "a\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBDz";
	const WfELine *l;
	size_t c1, c2;

	CHECK(WF_OK == wf_et_init(&t, WF_EOL_LF, 0));
	CHECK(WF_OK == wf_et_load(&t, text, sizeof text - 1));
	l = &t.lines[0];
	c1 = wf_et_col_next(l, 0);
	CHECK(1 == c1); /* a */
	c2 = wf_et_col_next(l, c1);
	CHECK(c2 == 1 + 4 + 4); /* the thumb and its skin tone at once */
	CHECK(wf_et_col_prev(l, c2) == c1);
	CHECK(wf_et_col_prev(l, c1) == 0);
	wf_et_free(&t);
}

static void columns_step_over_whole_clusters(void)
{
	WfEText t;
	/* e + combining acute, a wide character, then an emoji */
	const char text[] = "e\xCC\x81\xE6\x97\xA5\xF0\x9F\x98\x80z";
	const WfELine *l;
	size_t c;

	CHECK(WF_OK == wf_et_init(&t, WF_EOL_LF, 0));
	CHECK(WF_OK == wf_et_load(&t, text, sizeof text - 1));
	l = &t.lines[0];
	c = wf_et_col_next(l, 0);
	CHECK(3 == c); /* e + the accent are one step */
	CHECK(0 == wf_et_col_prev(l, c));
	CHECK(6 == wf_et_col_next(l, c));
	CHECK(wf_et_col_prev(l, l->len) == l->len - 1);
	CHECK(wf_et_col_next(l, l->len) == l->len);
	CHECK(1 == wf_et_cells(l, 3, 8) && 3 == wf_et_cells(l, 6, 8));
	CHECK(3 ==
	        wf_et_col_at_cells(l, 2, 8)); /* the wide one needs 2 cells */
	CHECK(3 == wf_et_col_at_cells(l, 1, 8));
	wf_et_free(&t);

	CHECK(WF_OK == wf_et_init(&t, WF_EOL_LF, 0));
	CHECK(WF_OK == wf_et_load(&t, "a\tb", 3));
	CHECK(8 == wf_et_cells(&t.lines[0], 2, 8) &&
	        9 == wf_et_cells(&t.lines[0], 3, 8));
	wf_et_free(&t);
}

static void one_huge_line_stays_cheap_to_edit(void)
{
	WfEText t;
	size_t i;
	char *big = malloc(8u << 20);

	CHECK(NULL != big);
	memset(big, 'x', 8u << 20);
	CHECK(WF_OK == wf_et_init(&t, WF_EOL_LF, 0));
	CHECK(WF_OK == wf_et_load(&t, big, 8u << 20) && 1 == t.n);
	for (i = 0; i < 200; i++) {
		CHECK(WF_OK ==
		        wf_et_insert(&t, (WfPos){0, 100}, "y", 1, false, NULL));
	}
	CHECK(t.bytes == (8u << 20) + 200 && wf_et_check(&t));
	free(big);
	wf_et_free(&t);
}

int main(void)
{
	load_and_dump_are_inverse();
	random_edits_match_the_flat_model();
	normalized_breaks_follow_the_line();
	the_size_limit_changes_nothing();
	emoji_with_a_skin_tone_is_one_step();
	columns_step_over_whole_clusters();
	one_huge_line_stays_cheap_to_edit();
	return TEST_RESULT();
}

#include <string.h>

#include "text/wf_text.h"
#include "wf_test.h"
#include "base/wf_macro.h"

static int sanitized_eq(const char *in, size_t n, const char *want)
{
	WfBuf b;
	int ok;

	wf_buf_init(&b);
	ok = WF_OK == wf_sanitize_utf8(in, n, &b) &&
	        0 == strcmp(wf_buf_cstr(&b), want);
	if (!ok) {
		printf("  got \"%s\" want \"%s\"\n", wf_buf_cstr(&b), want);
	}
	wf_buf_free(&b);
	return ok;
}

static void test_decode(void)
{
	uint32_t cp;

	CHECK(1 == wf_utf8_decode("A", 1, &cp) && 'A' == cp);
	CHECK(3 == wf_utf8_decode("\xE3\x81\x82", 3, &cp) && 0x3042 == cp);
	CHECK(4 == wf_utf8_decode("\xF0\x9F\x98\x80", 4, &cp) && 0x1F600 == cp);
	/* overlong "/" */
	CHECK(1 == wf_utf8_decode("\xC0\xAF", 2, &cp) &&
	        WF_CP_REPLACEMENT == cp);
	/* UTF-8 encoded surrogate */
	CHECK(1 == wf_utf8_decode("\xED\xA0\x80", 3, &cp) &&
	        WF_CP_REPLACEMENT == cp);
	/* truncated sequence */
	CHECK(1 == wf_utf8_decode("\xE3\x81", 2, &cp) &&
	        WF_CP_REPLACEMENT == cp);
	/* beyond U+10FFFF */
	CHECK(1 == wf_utf8_decode("\xF4\x90\x80\x80", 4, &cp) &&
	        WF_CP_REPLACEMENT == cp);
}

static void malformed_bytes_are_separate_cursor_steps(void)
{
	static const char *const cases[] = {"a\x80", "\x80\x80", "\xE3\x81",
	        "\xF0\x80\x80\x80", "\xE3\x81\x82\x80"};

	for (size_t i = 0; i < WF_ARRAY_LEN(cases); i++) {
		size_t n = strlen(cases[i]);
		size_t at = n, previous = n;

		/* The final invalid byte must not consume the preceding scalar. */
		CHECK(n - 1 == wf_utf8_cluster_prev(cases[i], n, n));
		while (at) {
			previous = wf_utf8_cluster_prev(cases[i], n, at);
			CHECK(previous < at);
			CHECK(at ==
			        wf_utf8_cluster_next(cases[i], n, previous));
			at = previous;
		}
	}
}

static void test_encode_roundtrip(void)
{
	static const uint32_t samples[] = {
	        0x24, 0xA2, 0x20AC, 0x10348, 0x10FFFF};
	size_t i;

	for (i = 0; i < WF_ARRAY_LEN(samples); i++) {
		char b[4];
		uint32_t cp;
		size_t n = wf_utf8_encode(samples[i], b);

		CHECK(wf_utf8_decode(b, n, &cp) == n && cp == samples[i]);
	}
	{
		char b[4];

		CHECK(3 == wf_utf8_encode(0xD800, b)); /* U+FFFD */
		CHECK(3 == wf_utf8_encode(0x110000, b));
	}
}

static void test_utf16(void)
{
	static const wchar_t pair[] = {0xD83D, 0xDE00};
	static const wchar_t lone[] = {0xD83D, L'x', 0xDE00};
	size_t i = 0;
	wchar_t out[8];
	size_t used;
	uint32_t used32;
	WfBuf b;

	CHECK(0x1F600 == wf_utf16_next(pair, 2, &i) && 2 == i);
	i = 0;
	CHECK(WF_CP_REPLACEMENT == wf_utf16_next(lone, 3, &i) && 1 == i);
	CHECK('x' == wf_utf16_next(lone, 3, &i));
	CHECK(WF_CP_REPLACEMENT == wf_utf16_next(lone, 3, &i));

	CHECK(WF_OK ==
	                wf_utf8_to_utf16(
	                        "a\xF0\x9F\x98\x80", 5, out, 8, &used) &&
	        3 == used);
	CHECK(L'a' == out[0] && 0xD83D == out[1] && 0xDE00 == out[2]);
	CHECK(WF_E_RANGE ==
	                wf_utf8_to_utf16(
	                        "a\xF0\x9F\x98\x80", 5, out, 2, &used) &&
	        0 == used);
	CHECK(0 == wf_utf8_decode("", 0, &used32));
	CHECK(1 == wf_utf16_chunk_end(pair, 2, 0));

	wf_buf_init(&b);
	CHECK(WF_OK == wf_sanitize_utf16(lone, 3, &b));
	CHECK(0 == strcmp(wf_buf_cstr(&b), "\xEF\xBF\xBDx\xEF\xBF\xBD"));
	wf_buf_free(&b);
}

static void test_sanitize(void)
{
	CHECK(sanitized_eq("abc", 3, "abc"));
	CHECK(sanitized_eq("a\x1b[2Jb", 6, "a^[[2Jb"));
	CHECK(sanitized_eq("\x7f", 1, "^?"));
	CHECK(sanitized_eq("a\tb", 3, "a    b"));
	CHECK(sanitized_eq("\xC2\x85", 2, "<U+0085>"));
	CHECK(sanitized_eq("a\xE2\x80\xAE"
	                   "b",
	        5, "a<RLO>b"));
	CHECK(sanitized_eq("a\xE2\x80\x8D"
	                   "b",
	        5, "a<ZWJ>b"));
	CHECK(sanitized_eq("\xFF", 1, "\xEF\xBF\xBD"));
	CHECK(sanitized_eq("", 0, ""));
	CHECK(sanitized_eq(
	        "\0", 1, "^@")); /* embedded NUL is data, not a terminator */
}

static void test_width(void)
{
	CHECK(1 == wf_cp_width('a'));
	CHECK(2 == wf_cp_width(0x3042));  /* hiragana */
	CHECK(2 == wf_cp_width(0xFF21));  /* fullwidth A */
	CHECK(0 == wf_cp_width(0x0301));  /* combining acute */
	CHECK(1 == wf_cp_width(0x00A1));  /* ambiguous -> 1 */
	CHECK(2 == wf_cp_width(0x1F600)); /* emoji presentation: two cells */
	CHECK(2 == wf_cp_width(0x1F680)); /* rocket */
	CHECK(2 == wf_cp_width(0x1F9D1)); /* person */
	CHECK(2 == wf_cp_width(0x2705));  /* check mark button */
	CHECK(2 == wf_cp_width(0x2B50));  /* star */
	CHECK(1 == wf_cp_width(0x2764));  /* heart: text presentation */
	CHECK(1 == wf_cp_width(0x00A9));  /* copyright sign */
	CHECK(1 == wf_cp_width(0x1F1E6)); /* regional indicator: unknowable */
	CHECK(0 == wf_cp_width(0xFE0F));  /* variation selector-16 */
	CHECK(4 == wf_utf8_cells("a\xF0\x9F\x98\x80z", 6));
}

/* `[terminal] ambiguous_width`: East_Asian_Width=A is drawn wide by some
 * terminals, and only then does piero count it as two cells. */
static void test_ambiguous_width(void)
{
	CHECK(1 == wf_cp_width(0x00E9)); /* the default: one cell */
	CHECK(1 == wf_cp_width(0x25CB));
	wf_set_ambiguous_width(2);
	CHECK(2 == wf_cp_width(0x00A1)); /* the first ambiguous scalar */
	CHECK(2 == wf_cp_width(0x00E9)); /* e with acute */
	CHECK(2 == wf_cp_width(0x00B0)); /* degree sign */
	CHECK(2 == wf_cp_width(0x03B1)); /* Greek alpha */
	CHECK(2 == wf_cp_width(0x0416)); /* Cyrillic zhe */
	CHECK(2 == wf_cp_width(0x2192)); /* arrow */
	CHECK(2 == wf_cp_width(0x25CB)); /* white circle */
	CHECK(2 == wf_cp_width(0x2500)); /* box drawing */
	CHECK(2 == wf_cp_width(0xE000)); /* private use */
	CHECK(2 == wf_cp_width(0xFFFD)); /* replacement character */
	/* everything else is as it was */
	CHECK(1 == wf_cp_width('a'));
	CHECK(1 == wf_cp_width(0x00A0));  /* no-break space: neutral */
	CHECK(1 == wf_cp_width(0x02FF));  /* just below the combining marks */
	CHECK(1 == wf_cp_width(0x2764));  /* heart: neutral */
	CHECK(2 == wf_cp_width(0x3042));  /* wide stays wide */
	CHECK(0 == wf_cp_width(0x0301));  /* combining stays zero, though "A" */
	CHECK(0 == wf_cp_width(0xFE0F));  /* so does a variation selector */
	CHECK(2 == wf_cp_width(0x1F600)); /* emoji: already two */
	CHECK(4 ==
	        wf_utf8_cells("\xC3\xA9\xE2\x97\x8B", 5)); /* e-acute, circle */
	/* only 2 turns it on; anything else is the default */
	wf_set_ambiguous_width(0);
	CHECK(1 == wf_cp_width(0x00E9));
	wf_set_ambiguous_width(2);
	wf_set_ambiguous_width(3);
	CHECK(1 == wf_cp_width(0x00E9));
	wf_set_ambiguous_width(2);
	wf_set_ambiguous_width(1);
	CHECK(1 == wf_cp_width(0x00E9));
	CHECK(1 == wf_cp_width(0x25CB));
}

static void test_buf(void)
{
	WfBuf b;

	wf_buf_init(&b);
	CHECK(WF_E_INVAL == wf_buf_append(&b, NULL, 1));
	CHECK(!b.data && !b.len && !b.cap);
	CHECK(WF_E_RANGE == wf_buf_append(&b, NULL, SIZE_MAX));
	CHECK(!b.data && !b.len && !b.cap);
	CHECK(0 == strcmp(wf_buf_cstr(&b), ""));
	CHECK(WF_OK == wf_buf_put_uint(&b, 0));
	CHECK(WF_OK == wf_buf_put_uint(&b, 4294967295u));
	CHECK(0 == strcmp(wf_buf_cstr(&b), "04294967295"));
	{
		char *data = b.data;
		size_t capacity = b.cap;

		CHECK(WF_E_INVAL == wf_buf_append(&b, NULL, capacity + 1));
		CHECK(b.data == data && b.cap == capacity && b.len == 11);
		CHECK(0 == strcmp(wf_buf_cstr(&b), "04294967295"));
	}
	/* growth across many appends keeps the NUL invariant */
	{
		int i;

		for (i = 0; i < 1000; i++) {
			CHECK(WF_OK == wf_buf_putc(&b, 'x'));
		}
		CHECK(1011 == b.len && '\0' == b.data[b.len]);
	}
	CHECK(WF_E_RANGE == wf_buf_append(&b, NULL, (size_t)-1));
	wf_buf_free(&b);
}

/* A chunk ends "inside" an escape sequence if its last ESC is not followed by
 * the sequence's final byte before the chunk ends. */
static int ends_mid_sequence(const wchar_t *w, size_t n)
{
	size_t i = n, k;

	while (i > 0 && 0x1B != w[i - 1]) {
		i--;
	}
	if (0 == i) {
		return 0;
	}
	k = i; /* the character after that ESC */
	if (k >= n) {
		return 1;
	}
	if (L'[' != w[k]) {
		return 0; /* two-character escapes are complete once the second
		             arrives */
	}
	for (k++; k < n; k++) {
		if (w[k] >= 0x40 && w[k] <= 0x7E) {
			return 0; /* final byte seen */
		}
	}
	return 1;
}

static void test_chunking(void)
{
	enum
	{
		WANT = 64
	};
	static wchar_t big[8192], out[8192];
	unsigned seed = 12345;
	size_t round;

	/* small input: one piece */
	CHECK(3 == wf_utf16_chunk_end(L"abc", 3, 64));

	/* plain text, a pair straddling the limit: the pair stays together */
	{
		wchar_t t[10] = {L'a', L'b', L'c', 0xD83D, 0xDE00, L'd', L'e',
		        L'f', L'g', L'h'};

		CHECK(3 ==
		        wf_utf16_chunk_end(
		                t, 10, 4)); /* cut before the high surrogate */
		CHECK(5 ==
		        wf_utf16_chunk_end(t, 10, 5)); /* the pair fits whole */
	}

	/* an ESC inside the window: cut right before it */
	{
		wchar_t t[40];
		size_t i;

		for (i = 0; i < 40; i++) {
			t[i] = L'x';
		}
		t[7] = 0x1B;
		CHECK(7 == wf_utf16_chunk_end(t, 40, 20));
		t[0] = 0x1B; /* an ESC at index 0 must not produce an empty
		                chunk */
		t[7] = L'x';
		CHECK(20 == wf_utf16_chunk_end(t, 40, 20));
	}

	/* Property: over many random full-screen-like streams the chunks tile
	 * the input exactly, never split a surrogate pair, and never end
	 * mid-sequence. */
	for (round = 0; round < 200; round++) {
		size_t n = 0, pos = 0, produced = 0;

		while (n + 48 < WF_ARRAY_LEN(big)) {
			unsigned r;

			seed = seed * 1103515245u + 12345u;
			r = (seed >> 16) & 0xFF;
			if (r < 90) { /* cursor position with varying digit
			                 counts */
				n += (size_t)swprintf(big + n, 40,
				        L"\x1b[%u;%uH", r % 60 + 1,
				        (r * 7) % 200 + 1);
			} else if (r < 120) { /* truecolor SGR */
				n += (size_t)swprintf(big + n, 48,
				        L"\x1b[0;38;2;%u;%u;%um", r, r / 2,
				        r / 3);
			} else if (r < 140) { /* an astral character */
				big[n++] = 0xD83D;
				big[n++] = (wchar_t)(0xDE00 + r % 40);
			} else {
				big[n++] = (wchar_t)(L'a' + r % 26);
			}
		}
		while (pos < n) {
			size_t len =
			        wf_utf16_chunk_end(big + pos, n - pos, WANT);

			CHECK(len >= 1);
			if (n - pos > WANT && len > WANT) {
				CHECK_FAIL("chunk longer than the limit");
				break;
			}
			CHECK(!((uint16_t)big[pos + len - 1] >= 0xD800 &&
			        (uint16_t)big[pos + len - 1] <= 0xDBFF &&
			        pos + len < n));
			CHECK(!ends_mid_sequence(big + pos, len) ||
			        pos + len == n);
			memcpy(out + produced, big + pos, len * sizeof *big);
			produced += len;
			pos += len;
		}
		CHECK(produced == n && 0 == memcmp(out, big, n * sizeof *big));
	}
}

static void utf8_budgets_preserve_scalars(void)
{
	char input[16390], shown[12];
	memset(input, 'x', 16383);
	memcpy(input + 16383, "\xe6\x97\xa5", 4);
	CHECK(wf_utf8_prefix(input, 16386, 16384) == 16383);
	CHECK(wf_utf8_prefix(input + 16383, 3, 3) == 3);
	CHECK(wf_utf8_prefix(input + 16383, 3, 2) == 0);
	wf_utf8_elide(shown, sizeof shown,
	        "a\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5", 6);
	CHECK(!strcmp(shown, "a\xe6\x97\xa5..."));
	wf_utf8_elide(shown, 5, "\xe6\x97\xa5\xe6\x97\xa5", 20);
	CHECK(!strcmp(shown, "..."));
	wf_utf8_elide(shown, sizeof shown, "ok", 2);
	CHECK(!strcmp(shown, "ok"));
}

static void utf16_carets_preserve_scalars(void)
{
	const wchar_t text[] = {
	        L'a', 0xD83D, 0xDE00, L'e', 0x0301, 0xD800, L'z', 0xDC00};
	const size_t boundaries[] = {0, 1, 3, 4, 5, 6, 7, 8};
	for (size_t i = 0; i + 1 < sizeof boundaries / sizeof boundaries[0];
	        i++) {
		CHECK(wf_utf16_next_boundary(text, 8, boundaries[i]) ==
		        boundaries[i + 1]);
		CHECK(wf_utf16_prev_boundary(text, boundaries[i + 1]) ==
		        boundaries[i]);
	}
	CHECK(wf_utf16_prev_boundary(NULL, 0) == 0);
	CHECK(wf_utf16_next_boundary(NULL, 0, 0) == 0);
	CHECK(wf_utf16_next_boundary(text, 8, SIZE_MAX) == 8);
}

/* Steps over what is seen as one character: a letter with its accent, an
 * emoji with its skin tone; invalid bytes are steps of their own. */
static void cluster_steps(void)
{
	/* e + combining acute, a, thumbs up + skin tone, a bad byte, z */
	const char t[] = "e\xCC\x81" "a" "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD" "\xFF" "z";
	size_t n = sizeof t - 1;

	CHECK(3 == wf_utf8_cluster_next(t, n, 0));
	CHECK(0 == wf_utf8_cluster_prev(t, n, 3));
	CHECK(4 == wf_utf8_cluster_next(t, n, 3));
	CHECK(12 == wf_utf8_cluster_next(t, n, 4));
	CHECK(4 == wf_utf8_cluster_prev(t, n, 12));
	CHECK(13 == wf_utf8_cluster_next(t, n, 12)); /* the bad byte moves on */
	CHECK(12 == wf_utf8_cluster_prev(t, n, 13));
	CHECK(n == wf_utf8_cluster_next(t, n, n));
	CHECK(n == wf_utf8_cluster_next(t, n, n - 1));
	CHECK(0 == wf_utf8_cluster_prev(t, n, 0));
}

int main(void)
{
	utf16_carets_preserve_scalars();
	utf8_budgets_preserve_scalars();
	for (unsigned i = 0; i < 128; i++) {
		char byte = (char)i;
		CHECK(wf_utf8_cells(&byte, 1) == wf_cp_width(i));
	}
	test_decode();
	malformed_bytes_are_separate_cursor_steps();
	test_encode_roundtrip();
	test_utf16();
	test_sanitize();
	test_width();
	test_ambiguous_width();
	test_buf();
	test_chunking();
	cluster_steps();
	return TEST_RESULT();
}

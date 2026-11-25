/* The syntax lexers: what gets which class, and what carries across lines. */
#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "edit/wf_esyntax.h"
#include "wf_test.h"
#include "base/wf_macro.h"

static WfEText text;
static WfSyntax syn;

static void setup(WfLang lang, const char *src)
{
	wf_et_free(&text);
	wf_syn_free(&syn);
	CHECK(WF_OK == wf_et_init(&text, WF_EOL_LF, 0));
	CHECK(WF_OK == wf_et_load(&text, src, strlen(src)));
	wf_syn_init(&syn, lang);
}

/* The class of the byte at `col` on `line`. */
static int tok(size_t line, size_t col)
{
	uint8_t out[512] = {0};

	CHECK(wf_syn_line(&syn, &text, line, out, sizeof out));
	return out[col];
}

static bool all_of(size_t line, size_t from, size_t to, WfTok t)
{
	size_t i;

	for (i = from; i < to; i++) {
		if (tok(line, i) != (int)t) {
			return false;
		}
	}
	return true;
}

static void c_code(void)
{
	setup(WF_LANG_C,
	        "#include <stdio.h>\n"
	        "static int main(void) { /* c */ return 0x1F + \"a\\n\"[0]; } "
	        "// end\n"
	        "uint32_t x = 3.5e-2f; char c = 'q';");
	CHECK(all_of(0, 0, 8, WF_TOK_PREPROC));
	CHECK(all_of(0, 9, 18, WF_TOK_STRING)); /* <stdio.h> */
	CHECK(all_of(1, 0, 6, WF_TOK_KEYWORD)); /* static */
	CHECK(all_of(1, 7, 10, WF_TOK_TYPE));   /* int */
	CHECK(WF_TOK_PLAIN == tok(1, 11));      /* main */
	CHECK(all_of(1, 24, 31, WF_TOK_COMMENT));
	CHECK(all_of(1, 32, 38, WF_TOK_KEYWORD)); /* return */
	CHECK(all_of(1, 39, 43, WF_TOK_NUMBER));  /* 0x1F */
	CHECK(WF_TOK_STRING == tok(1, 46) && WF_TOK_STRING == tok(1, 47));
	CHECK(all_of(1, 48, 50, WF_TOK_ESCAPE));  /* \n */
	CHECK(all_of(1, 59, 63, WF_TOK_COMMENT)); /* // end */
	CHECK(all_of(2, 0, 8, WF_TOK_TYPE));      /* uint32_t */
	CHECK(all_of(2, 13, 20, WF_TOK_NUMBER));  /* 3.5e-2f */
	CHECK(all_of(2, 31, 34, WF_TOK_STRING));  /* 'q' */
}

static void block_comments_and_invalidation(void)
{
	WfPos at = {1, 0};

	setup(WF_LANG_C,
	        "int a; /* open\nstill comment\nend */ int b;\nint c;");
	CHECK(all_of(0, 7, 14, WF_TOK_COMMENT));
	CHECK(all_of(1, 0, 13, WF_TOK_COMMENT));
	CHECK(all_of(2, 0, 6, WF_TOK_COMMENT));
	CHECK(all_of(2, 7, 10, WF_TOK_TYPE));
	CHECK(all_of(3, 0, 3, WF_TOK_TYPE));
	/* closing the comment early changes what follows, once invalidated */
	CHECK(WF_OK == wf_et_insert(&text, at, "*/ int z;", 9, false, NULL));
	wf_syn_invalidate(&syn, 1);
	CHECK(all_of(1, 0, 2, WF_TOK_COMMENT));
	CHECK(all_of(1, 3, 6, WF_TOK_TYPE)); /* int, now outside the comment */
	CHECK(WF_TOK_COMMENT != tok(2, 0));  /* the old comment end is code */
}

static void json_keys_and_values(void)
{
	setup(WF_LANG_JSON,
	        "{\"name\": \"v\\u0041\", \"n\": -12.5, \"ok\": true, \"x\": "
	        "null}");
	CHECK(all_of(0, 1, 7, WF_TOK_KEY));       /* "name" */
	CHECK(WF_TOK_STRING == tok(0, 10));       /* "v... */
	CHECK(all_of(0, 11, 17, WF_TOK_ESCAPE));  /* A */
	CHECK(all_of(0, 25, 30, WF_TOK_NUMBER));  /* -12.5 */
	CHECK(all_of(0, 38, 42, WF_TOK_KEYWORD)); /* true */
	CHECK(all_of(0, 49, 53, WF_TOK_KEYWORD)); /* null */
}

static void ini_and_yaml(void)
{
	setup(WF_LANG_INI,
	        "; note\n[core]\nname = piero\nsize=42\nflag = true\n");
	CHECK(all_of(0, 0, 6, WF_TOK_COMMENT));
	CHECK(all_of(1, 0, 6, WF_TOK_HEADING));
	CHECK(all_of(2, 0, 4, WF_TOK_KEY));
	CHECK(WF_TOK_PLAIN == tok(2, 5));
	CHECK(all_of(3, 5, 7, WF_TOK_NUMBER));
	CHECK(all_of(4, 7, 11, WF_TOK_KEYWORD));

	setup(WF_LANG_YAML, "---\n# c\nname: \"x\" # tail\nitems:\n  - a: 1\n");
	CHECK(all_of(0, 0, 3, WF_TOK_MARKUP));
	CHECK(all_of(1, 0, 3, WF_TOK_COMMENT));
	CHECK(all_of(2, 0, 4, WF_TOK_KEY));
	CHECK(all_of(2, 6, 9, WF_TOK_STRING));
	CHECK(all_of(2, 10, 16, WF_TOK_COMMENT));
	CHECK(all_of(3, 0, 5, WF_TOK_KEY));
	CHECK(WF_TOK_MARKUP == tok(4, 2) && WF_TOK_KEY == tok(4, 4));
	CHECK(WF_TOK_NUMBER == tok(4, 7));
}

static void markdown_fences_carry_across_lines(void)
{
	setup(WF_LANG_MARKDOWN,
	        "# Title\ntext `code` and **bold**\n```c\n# not a "
	        "heading\n```\n- "
	        "item\n> quote\n[a](http://x)");
	CHECK(all_of(0, 0, 7, WF_TOK_HEADING));
	CHECK(all_of(1, 5, 11, WF_TOK_STRING));
	CHECK(all_of(1, 16, 24, WF_TOK_MARKUP));
	CHECK(all_of(2, 0, 4, WF_TOK_MARKUP));
	CHECK(all_of(3, 0, 14, WF_TOK_STRING)); /* inside the fence */
	CHECK(all_of(4, 0, 3, WF_TOK_MARKUP));
	CHECK(WF_TOK_MARKUP == tok(5, 0));
	CHECK(all_of(6, 0, 7, WF_TOK_COMMENT));
	CHECK(all_of(7, 0, 3, WF_TOK_KEY) && all_of(7, 3, 13, WF_TOK_STRING));
}

/* One line of nothing but openers must cost time proportional to its length,
 * and a line too long to colour must not stop the lines after it from being
 * lexed, nor hide a fence. */
static void markdown_is_linear_and_long_lines_keep_the_state(void)
{
	enum
	{
		N = 200000,
		LONG = 20000
	};
	static const char *const openers[] = {
	        "[", "x`", "*", "_", "[a](", "**a"};
	size_t k;
	ULONGLONG t0 = GetTickCount64();

	for (k = 0; k < WF_ARRAY_LEN(openers); k++) {
		size_t len = strlen(openers[k]), at = 0, reps = N / len;
		char *src = malloc(reps * len + 16);

		CHECK(NULL != src);
		if (!src) {
			return;
		}
		while (reps--) {
			memcpy(src + at, openers[k], len);
			at += len;
		}
		memcpy(src + at, "\n# Head\n", 9);
		setup(WF_LANG_MARKDOWN, src);
		/* the next line is lexed from state worked out past the huge one */
		CHECK(WF_TOK_HEADING == tok(1, 0));
		free(src);
	}
	CHECK(GetTickCount64() - t0 < 3000);

	/* the lines that are coloured (up to the limit) are the ones where a
	 * rescan per opener would show: 24 of them, 16000 openers each */
	{
		enum
		{
			W = 16000,
			ROWS = 24
		};
		size_t row, per = W + 1;
		char *src = malloc(ROWS * per + 1);
		uint8_t *out = malloc(WF_SYN_LINE_MAX);

		CHECK(NULL != src && NULL != out);
		if (!src || !out) {
			free(src);
			free(out);
			return;
		}
		for (row = 0; row < ROWS; row++) {
			memset(src + row * per, '[', W);
			src[row * per + W] = row + 1 < ROWS ? '\n' : '\0';
		}
		setup(WF_LANG_MARKDOWN, src);
		t0 = GetTickCount64();
		for (row = 0; row < ROWS; row++) {
			CHECK(wf_syn_line(
			        &syn, &text, row, out, WF_SYN_LINE_MAX));
		}
		CHECK(GetTickCount64() - t0 < 1000);
		free(src);
		free(out);
	}

	/* a line over the limit is not lexed for the state it leaves behind:
	 * an unclosed comment in it does not colour the rest of the file */
	{
		char *src = malloc(WF_SYN_LINE_MAX + 64);

		CHECK(NULL != src);
		if (!src) {
			return;
		}
		memcpy(src, "/*", 2);
		memset(src + 2, 'c', WF_SYN_LINE_MAX);
		memcpy(src + 2 + WF_SYN_LINE_MAX, "\nint x;", 8);
		setup(WF_LANG_C, src);
		CHECK(all_of(1, 0, 3, WF_TOK_TYPE));
		free(src);
	}

	{
		char *src = malloc(2 * LONG + 64);
		size_t at = 0;

		CHECK(NULL != src);
		if (!src) {
			return;
		}
		memcpy(src, "```\n", 4);
		at = 4;
		memset(src + at, 'x', LONG);
		at += LONG;
		memcpy(src + at, "\n# inside\n```\n# outside\n", 25);
		setup(WF_LANG_MARKDOWN, src);
		CHECK(all_of(2, 0, 8, WF_TOK_STRING)); /* still in the fence */
		CHECK(all_of(4, 0, 9, WF_TOK_HEADING));
		free(src);

		/* a fence marker that is itself a huge line still toggles */
		src = malloc(LONG + 64);
		CHECK(NULL != src);
		if (!src) {
			return;
		}
		memcpy(src, "```", 3);
		memset(src + 3, 'y', LONG);
		memcpy(src + 3 + LONG, "\n# in\n", 7);
		setup(WF_LANG_MARKDOWN, src);
		CHECK(all_of(1, 0, 4, WF_TOK_STRING));
		free(src);
	}
}

static void powershell(void)
{
	setup(WF_LANG_POWERSHELL,
	        "# c\nGet-ChildItem $env:TEMP -Recurse | Where-Object { $_ -eq "
	        "\"a`n$x\" }\n<# block\nmore #> if ($a) { 1 }");
	CHECK(all_of(0, 0, 3, WF_TOK_COMMENT));
	CHECK(all_of(1, 0, 13, WF_TOK_TYPE));
	CHECK(all_of(1, 14, 23, WF_TOK_KEY));
	CHECK(all_of(1, 24, 32, WF_TOK_KEYWORD));
	CHECK(all_of(2, 0, 7, WF_TOK_COMMENT));
	CHECK(all_of(3, 0, 7, WF_TOK_COMMENT));
	CHECK(all_of(3, 8, 10, WF_TOK_KEYWORD));
}

static void names_and_fallbacks(void)
{
	uint8_t out[4];

	CHECK(WF_LANG_C == wf_syn_lang_for_name(L"a.C"));
	CHECK(WF_LANG_CPP == wf_syn_lang_for_name(L"x.hpp"));
	CHECK(WF_LANG_JSON == wf_syn_lang_for_name(L"c.json"));
	CHECK(WF_LANG_YAML == wf_syn_lang_for_name(L"c.YML"));
	CHECK(WF_LANG_INI == wf_syn_lang_for_name(L"config.ini"));
	CHECK(WF_LANG_MARKDOWN == wf_syn_lang_for_name(L"README.md"));
	CHECK(WF_LANG_POWERSHELL == wf_syn_lang_for_name(L"s.ps1"));
	CHECK(WF_LANG_TEXT == wf_syn_lang_for_name(L"notes.txt"));
	CHECK(WF_LANG_TEXT == wf_syn_lang_for_name(L"Makefile"));
	CHECK(!strcmp(wf_syn_comment_prefix(WF_LANG_C), "//"));
	CHECK(NULL == wf_syn_comment_prefix(WF_LANG_JSON));
	/* plain text is never coloured, and a line longer than the buffer is
	 * refused rather than overrun */
	setup(WF_LANG_TEXT, "abc");
	CHECK(!wf_syn_line(&syn, &text, 0, out, sizeof out));
	setup(WF_LANG_C, "abcdefgh");
	CHECK(!wf_syn_line(&syn, &text, 0, out, sizeof out));
	CHECK(!wf_syn_line(&syn, &text, 5, out, sizeof out));
}

/* Showing lines in order works out each line's end state as it goes: the next
 * line starts from it, and what is shown is what a fresh walk from the top gives. */
static void lines_in_order_keep_their_end_states(void)
{
	static const char SRC[] =
	        "/* open\n still open\n close */ int x;\nint y; // c\n";
	WfSyntax fresh;
	uint8_t a[64], b[64];
	size_t lines;

	setup(WF_LANG_C, SRC);
	lines = text.n;
	CHECK(lines >= 4);
	for (size_t line = 0; line < lines; line++) {
		CHECK(wf_syn_line(&syn, &text, line, a, sizeof a));
		/* after line k, the states up to k + 1 are known: nothing is lexed twice */
		CHECK(syn.valid == line + 2);
	}
	/* the same classes asked for in the other order, from a fresh state */
	wf_syn_init(&fresh, WF_LANG_C);
	for (size_t line = lines; line-- > 0;) {
		const WfELine *l = &text.lines[line];

		CHECK(wf_syn_line(&fresh, &text, line, a, sizeof a));
		CHECK(wf_syn_line(&syn, &text, line, b, sizeof b));
		CHECK(!memcmp(a, b, l->len));
	}
	wf_syn_free(&fresh);
	/* an edit takes back what came after it, and only that */
	wf_syn_invalidate(&syn, 2);
	CHECK(syn.valid <= 3);
	CHECK(wf_syn_line(&syn, &text, 3, a, sizeof a));
	CHECK(syn.valid == 5);
}

int main(void)
{
	CHECK(WF_OK == wf_et_init(&text, WF_EOL_LF, 0));
	wf_syn_init(&syn, WF_LANG_TEXT);
	c_code();
	lines_in_order_keep_their_end_states();
	block_comments_and_invalidation();
	json_keys_and_values();
	ini_and_yaml();
	markdown_fences_carry_across_lines();
	markdown_is_linear_and_long_lines_keep_the_state();
	powershell();
	names_and_fallbacks();
	wf_et_free(&text);
	wf_syn_free(&syn);
	return TEST_RESULT();
}

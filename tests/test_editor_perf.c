/*
 * How the editing model behaves on big text: the figures are printed so they
 * can be read in the log, and each is checked against a loose ceiling that a
 * normal machine meets by a wide margin. The ceilings catch a design that has
 * become proportional to the file size where it should not be.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/wf_editor.h"
#include "edit/wf_esyntax.h"
#include "wf_test.h"

static double now_ms(void)
{
	LARGE_INTEGER f, c;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* `lines` lines of source-like text; returns the bytes and their length. */
static char *make_text(size_t lines, size_t *n)
{
	size_t cap = lines * 72 + 16, at = 0, i;
	char *t = malloc(cap);

	CHECK(NULL != t);
	for (i = 0; i < lines; i++) {
		at += (size_t)snprintf(t + at, cap - at,
		        "int value_%zu = compute(%zu, \"text\"); // line\r\n",
		        i, i * 7);
	}
	*n = at;
	return t;
}

static WfEditor *fresh_editor(const char *text, size_t n, size_t max_bytes)
{
	WfEditor *e = calloc(1, sizeof *e);
	WfEditorOptions o;
	WfEncoding enc = {WF_ENC_UTF8, 0};

	wf_ed_options_default(&o);
	o.max_bytes = max_bytes;
	CHECK(e && WF_OK == wf_ed_init(e, &o));
	CHECK(WF_OK == wf_ed_set_text(e, text, n, enc));
	return e;
}

static void many_lines(void)
{
	size_t n;
	char *text = make_text(300000, &n); /* about 15 MB, 300k lines */
	double t0 = now_ms(), t_load, t_type, t_undo, t_find;
	WfEditor *e = fresh_editor(text, n, 64u << 20);
	size_t i;
	WfSearch q = {"value_299999 ", 13, true, false};
	WfMatch m;

	t_load = now_ms() - t0;
	printf("  load %zu bytes / %zu lines: %.1f ms\n", n, e->text.n, t_load);
	CHECK(300001 == e->text.n);
	CHECK(t_load < 3000);

	/* typing at the very top: cost must not follow the line count */
	t0 = now_ms();
	for (i = 0; i < 20000; i++) {
		CHECK(WF_OK == wf_ed_type(e, 'a' + (uint32_t)(i % 26)));
	}
	t_type = now_ms() - t0;
	printf("  20000 typed characters at the top: %.1f ms (%.3f ms each)\n",
	        t_type, t_type / 20000);
	CHECK(t_type / 20000 < 1.0);

	/* Enter at the top moves the line pointers: one memmove, not a rescan */
	t0 = now_ms();
	for (i = 0; i < 2000; i++) {
		CHECK(WF_OK == wf_ed_newline(e));
	}
	printf("  2000 line breaks at the top: %.1f ms (%.3f ms each)\n",
	        now_ms() - t0, (now_ms() - t0) / 2000);
	CHECK((now_ms() - t0) / 2000 < 5.0);

	/* a search that has to look through everything */
	t0 = now_ms();
	CHECK(wf_es_find(&e->text, &q, (WfPos){0, 0}, true, false, &m, NULL));
	t_find = now_ms() - t0;
	printf("  search through 15 MB: %.1f ms\n", t_find);
	CHECK(t_find < 2000);

	/* undo of all of it */
	t0 = now_ms();
	while (wf_un_can_undo(&e->undo)) {
		CHECK(WF_OK == wf_ed_undo(e));
	}
	t_undo = now_ms() - t0;
	printf("  undo of 22000 edits: %.1f ms\n", t_undo);
	CHECK(t_undo < 5000);
	CHECK(300001 == e->text.n && wf_et_check(&e->text));
	CHECK(!wf_ed_modified(e));

	/* the colouring of one screen deep into the file, from a cold start */
	{
		WfSyntax syn;
		uint8_t out[4096];
		size_t line;

		wf_syn_init(&syn, WF_LANG_C);
		t0 = now_ms();
		for (line = 290000; line < 290050; line++) {
			CHECK(wf_syn_line(
			        &syn, &e->text, line, out, sizeof out));
		}
		printf("  colouring a screen at line 290000 (states from the "
		       "top): %.1f ms\n",
		        now_ms() - t0);
		CHECK(now_ms() - t0 < 2000);
		/* after an edit near the top, a redraw only redoes from there */
		wf_syn_invalidate(&syn, 10);
		t0 = now_ms();
		CHECK(wf_syn_line(&syn, &e->text, 60, out, sizeof out));
		printf("  recolouring after an edit near the top: %.2f ms\n",
		        now_ms() - t0);
		wf_syn_free(&syn);
	}
	wf_ed_free(e);
	free(e);
	free(text);
}

static void one_huge_line(void)
{
	size_t n = 8u << 20, i;
	char *text = malloc(n);
	double t0;
	WfEditor *e;

	CHECK(NULL != text);
	memset(text, 'x', n);
	e = fresh_editor(text, n, 64u << 20);
	CHECK(1 == e->text.n);
	e->cur.col = n / 2;
	t0 = now_ms();
	for (i = 0; i < 2000; i++) {
		CHECK(WF_OK == wf_ed_type(e, 'y'));
	}
	printf("  2000 characters typed in the middle of an 8 MB line: %.1f ms "
	       "(%.3f ms each)\n",
	        now_ms() - t0, (now_ms() - t0) / 2000);
	CHECK((now_ms() - t0) / 2000 < 3.0);
	/* moving by columns and cells on that line stays cheap */
	t0 = now_ms();
	wf_ed_move(e, WF_MV_LINE_END, false, 0);
	CHECK(n + 2000 == e->cur.col);
	wf_ed_move(e, WF_MV_LEFT, false, 0);
	CHECK(n + 1999 == e->cur.col);
	wf_ed_move(e, WF_MV_WORD_LEFT, false, 0);
	CHECK(0 == e->cur.col);
	printf("  line end, left, word left on it: %.1f ms\n", now_ms() - t0);
	CHECK(now_ms() - t0 < 500);
	wf_ed_free(e);
	free(e);
	free(text);
}

static void big_paste_and_size_limit(void)
{
	size_t n = 6u << 20, i;
	char *clip = malloc(n);
	WfEditor *e = fresh_editor("", 0, 8u << 20);
	double t0;

	for (i = 0; i < n; i++) {
		clip[i] = i % 61 == 60 ? '\n' : (char)('a' + i % 26);
	}
	CHECK(WF_OK == wf_ed_set_clip(e, clip, n, false));
	t0 = now_ms();
	CHECK(WF_OK == wf_ed_paste(e));
	printf("  paste of 6 MB (about %zu lines): %.1f ms\n", e->text.n,
	        now_ms() - t0);
	CHECK(now_ms() - t0 < 3000);
	CHECK(wf_et_check(&e->text));
	/* a second one would go past the 8 MB limit: refused, nothing changed */
	{
		size_t before = e->text.bytes;

		CHECK(WF_E_RANGE == wf_ed_paste(e));
		CHECK(e->text.bytes == before && wf_et_check(&e->text));
	}
	t0 = now_ms();
	CHECK(WF_OK == wf_ed_undo(e));
	printf("  undo of the paste: %.1f ms\n", now_ms() - t0);
	CHECK(0 == e->text.bytes);
	wf_ed_free(e);
	free(e);
	free(clip);
}

static void a_long_session_stays_bounded(void)
{
	WfEditorOptions o;
	WfEditor e;
	WfEncoding enc = {WF_ENC_UTF8, 0};
	size_t i;
	double t0 = now_ms();

	wf_ed_options_default(&o);
	o.undo_bytes = 1u << 20;
	CHECK(WF_OK == wf_ed_init(&e, &o));
	CHECK(WF_OK == wf_ed_set_text(&e, "", 0, enc));
	for (i = 0; i < 300000; i++) {
		CHECK(WF_OK == wf_ed_type(&e, 'a' + (uint32_t)(i % 26)));
		if (i % 40 == 39) {
			CHECK(WF_OK == wf_ed_newline(&e));
		}
		if (i % 97 == 96) {
			CHECK(WF_OK == wf_ed_backspace(&e));
		}
	}
	printf("  300000 mixed edits with a 1 MiB history: %.1f ms, history "
	       "%zu bytes\n",
	        now_ms() - t0, e.undo.bytes);
	CHECK(e.undo.bytes <= (1u << 20) + (1u << 17) + 4096);
	CHECK(wf_et_check(&e.text));
	wf_ed_free(&e);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	many_lines();
	one_huge_line();
	big_paste_and_size_limit();
	a_long_session_stays_bounded();
	return TEST_RESULT();
}

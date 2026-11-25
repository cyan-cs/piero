/*
 * Syntax colours in the preview pane (DR-DRAFT-033), with a fake syntax worker:
 * which files are handed over and how, what an answer does and what a stale one
 * does not, the colours under horizontal scrolling and wide text, a theme change
 * (colours only: nothing is asked again), and what a new file leaves of the old
 * one's colours (nothing).
 */
#include <string.h>

#include "syntax/wf_synport.h"
#include "ui/wf_app.h"
#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

static struct
{
	int requests, closes;
	uint32_t slot, gen;
	char lang[24];
	char text[8192];
	size_t len;
	bool installed;
} FS;

static WfStatus fs_request(void *ctx,
        uint32_t slot,
        uint32_t gen,
        const char *lang,
        const char *text,
        size_t len)
{
	(void)ctx;
	FS.requests++;
	FS.slot = slot;
	FS.gen = gen;
	strcpy_s(FS.lang, sizeof FS.lang, lang);
	FS.len = len < sizeof FS.text ? len : sizeof FS.text;
	memcpy(FS.text, text, FS.len);
	return WF_OK;
}

static void fs_close(void *ctx, uint32_t slot)
{
	(void)ctx;
	(void)slot;
	FS.closes++;
}

static bool fs_installed(void *ctx, const char *lang)
{
	(void)ctx;
	(void)lang;
	return FS.installed;
}

static WfStatus pv_submit(
        void *ctx, uint32_t gen, const wchar_t *path, bool explicit_load)
{
	(void)path;
	(void)explicit_load;
	*(uint32_t *)ctx = gen;
	return WF_OK;
}

static void pv_cancel(void *ctx)
{
	(void)ctx;
}

typedef struct
{
	WfApp a;
	FakePort fp;
	uint32_t gen;
	WfPreviewPort port;
} Rig;

static void rig_open(Rig *r, bool with_syntax)
{
	WfSynPort sp = {NULL, fs_request, fs_close, fs_installed};

	memset(r, 0, sizeof *r);
	memset(&FS, 0, sizeof FS);
	FS.installed = true;
	r->fp.app = &r->a;
	r->port = (WfPreviewPort){&r->gen, pv_submit, pv_cancel};
	CHECK(WF_OK ==
	        wf_app_init(
	                &r->a, 100, 30, fake_port(&r->fp), L"C:\\Users\\dev"));
	wf_app_set_preview(&r->a, r->port);
	if (with_syntax) {
		wf_app_set_syntax(&r->a, sp);
	}
}

/* Shows `text` as the preview of the file `path`. */
static void show(Rig *r, const wchar_t *path, const char *text)
{
	WfPreview *p = NULL;

	CHECK(WF_OK == wf_preview_decode(text, strlen(text), 0, &p));
	free(r->a.preview_path);
	r->a.preview_path = _wcsdup(path);
	r->a.preview_busy = true;
	wf_app_on_preview(&r->a, r->a.preview_gen, p);
}

static void answer(
        Rig *r, uint32_t gen, uint32_t status, const WfSynRun *runs, uint32_t n)
{
	WfSynAnswer a;
	bool taken;

	memset(&a, 0, sizeof a);
	a.slot = WF_SYN_SLOT_PREVIEW;
	a.gen = gen;
	a.status = status;
	a.text_len = (uint32_t)FS.len;
	if (n) {
		a.runs = malloc(n * sizeof *runs);
		memcpy(a.runs, runs, n * sizeof *runs);
		a.nruns = n;
	}
	taken = wf_app_on_syntax(&r->a, &a);
	CHECK(taken);
	CHECK(NULL ==
	        a.runs); /* freed or kept: nothing is left to the caller */
}

/* The foreground of the first cell of `needle` in the pane (rows 2 up). */
static bool fg_of(Rig *r, const char *needle, uint32_t *fg, int w)
{
	WfFrame *f = wf_frame_new(100, 30, &(WfStyle){0, 0, 0});
	bool found = false;

	wf_app_draw_preview(&r->a, f, 0, 1, w, 20);
	for (int y = 1; y < 22 && !found; y++) {
		char row[2048] = "";
		int cell_of[2048];
		size_t n = 0;
		const char *hit;

		for (int x = 0; x < w; x++) {
			const char *u = f->cells[y * f->cols + x].utf8;

			for (size_t k = 0; u[k] && n + 1 < sizeof row; k++) {
				cell_of[n] = x;
				row[n++] = u[k];
			}
		}
		row[n] = '\0';
		hit = strstr(row, needle);
		if (hit) {
			*fg = f->cells[y * f->cols + cell_of[hit - row]].fg_rgb;
			found = true;
		}
	}
	wf_frame_free(f);
	return found;
}

static uint32_t colour(unsigned cls)
{
	return wf_theme_styles()->syntax[cls];
}

static void only_what_a_grammar_can_colour_is_handed_over(void)
{
	Rig r;

	rig_open(&r, true);
	show(&r, L"C:\\w\\a.txt", "{\"a\": 1}");
	CHECK(0 == FS.requests); /* no grammar names .txt */
	show(&r, L"C:\\w\\a.md", "# Title\n");
	CHECK(0 == FS.requests); /* Markdown is drawn rendered in the pane */
	FS.installed = false;
	show(&r, L"C:\\w\\a.json", "{\"a\": 1}\n");
	CHECK(0 == FS.requests); /* nothing installed */
	FS.installed = true;
	show(&r, L"C:\\w\\A.JSON", "{\"k\": 1,\n \"b\": [2]}\n");
	CHECK(1 == FS.requests && !strcmp(FS.lang, "json") &&
	        WF_SYN_SLOT_PREVIEW == FS.slot);
	/* the lines of the pane, joined as the parser reads them: each line and a line
	 * feed (the pane also has the empty line after the last line feed) */
	CHECK(FS.len == strlen("{\"k\": 1,\n \"b\": [2]}\n\n") &&
	        !memcmp(FS.text, "{\"k\": 1,\n \"b\": [2]}\n\n", FS.len));
	CHECK(r.a.prev_syn_gen == FS.gen);
	wf_app_destroy(&r.a);
	/* .env goes through the same door (the worker has its own lexer for it) */
	rig_open(&r, true);
	show(&r, L"C:\\w\\.env", "A=1\n");
	CHECK(1 == FS.requests && !strcmp(FS.lang, "dotenv"));
	wf_app_destroy(&r.a);
	/* no worker: nothing to ask, nothing breaks */
	rig_open(&r, false);
	show(&r, L"C:\\w\\a.json", "{\"a\": 1}\n");
	{
		uint32_t fg;

		CHECK(fg_of(&r, "\"a\"", &fg, 80) &&
		        fg == wf_theme_styles()->base.fg_rgb);
	}
	wf_app_destroy(&r.a);
}

static void the_answer_colours_the_text_and_a_stale_one_does_not(void)
{
	Rig r;
	uint32_t fg;
	WfSynRun runs[] = {{1, 3, WF_SC_KEY}, {6, 1, WF_SC_NUMBER}};
	uint32_t first;

	wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
	rig_open(&r, true);
	show(&r, L"C:\\w\\a.json", "{\"a\": 1}\n");
	first = FS.gen;
	/* plain until the answer comes */
	CHECK(fg_of(&r, "\"a\"", &fg, 80) &&
	        fg == wf_theme_styles()->base.fg_rgb);
	/* an answer for something else is dropped */
	answer(&r, first + 100, WF_SYN_OK, runs, 2);
	CHECK(fg_of(&r, "\"a\"", &fg, 80) &&
	        fg == wf_theme_styles()->base.fg_rgb);
	/* the answer for this text colours it */
	answer(&r, first, WF_SYN_OK, runs, 2);
	CHECK(fg_of(&r, "\"a\"", &fg, 80) && fg == colour(WF_SC_KEY));
	CHECK(fg_of(&r, "1}", &fg, 80) && fg == colour(WF_SC_NUMBER));
	CHECK(fg_of(&r, "{", &fg, 80) && fg == wf_theme_styles()->base.fg_rgb);
	/* a theme change: new colours, nothing asked */
	wf_theme_activate(&wf_theme_preset(WF_THEME_KING)->palette);
	CHECK(fg_of(&r, "\"a\"", &fg, 80) && fg == colour(WF_SC_KEY));
	CHECK(1 == FS.requests);
	wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
	/* an answer with an error leaves the text plain and says why */
	show(&r, L"C:\\w\\b.json", "{\"b\": 2}\n");
	answer(&r, FS.gen, WF_SYN_E_LIMIT, NULL, 0);
	CHECK(fg_of(&r, "\"b\"", &fg, 80) &&
	        fg == wf_theme_styles()->base.fg_rgb);
	CHECK(WF_SYN_E_LIMIT == r.a.prev_syn_status);
	wf_app_destroy(&r.a);
}

static void a_new_file_leaves_nothing_of_the_old_colours(void)
{
	Rig r;
	uint32_t fg, gen1;
	WfSynRun runs[] = {{1, 3, WF_SC_KEY}};
	int closes;

	wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
	rig_open(&r, true);
	show(&r, L"C:\\w\\a.json", "{\"a\": 1}\n");
	gen1 = FS.gen;
	answer(&r, gen1, WF_SYN_OK, runs, 1);
	CHECK(fg_of(&r, "\"a\"", &fg, 80) && fg == colour(WF_SC_KEY));
	/* another file with the same bytes: asked for again, and plain until answered */
	show(&r, L"C:\\w\\c.json", "{\"a\": 2}\n");
	CHECK(2 == FS.requests && FS.gen != gen1);
	CHECK(fg_of(&r, "\"a\"", &fg, 80) &&
	        fg == wf_theme_styles()->base.fg_rgb);
	/* the answer for the old file arrives late: dropped */
	answer(&r, gen1, WF_SYN_OK, runs, 1);
	CHECK(fg_of(&r, "\"a\"", &fg, 80) &&
	        fg == wf_theme_styles()->base.fg_rgb);
	/* a file no grammar names: the session of the slot is closed, nothing coloured */
	closes = FS.closes;
	show(&r, L"C:\\w\\n.txt", "{\"a\": 3}\n");
	CHECK(FS.closes > closes);
	CHECK(fg_of(&r, "\"a\"", &fg, 80) &&
	        fg == wf_theme_styles()->base.fg_rgb);
	/* the preview cleared (the cursor moved to a folder): closed, plain */
	show(&r, L"C:\\w\\d.json", "{\"a\": 4}\n");
	answer(&r, FS.gen, WF_SYN_OK, runs, 1);
	CHECK(fg_of(&r, "\"a\"", &fg, 80) && fg == colour(WF_SC_KEY));
	closes = FS.closes;
	wf_app_preview_clear(&r.a);
	CHECK(FS.closes == closes + 1);
	CHECK(!r.a.prev_ov.active && 0 == r.a.prev_syn_gen);
	/* the same text delivered again (a refresh that found nothing new): the colours
	 * stay and nothing is asked */
	show(&r, L"C:\\w\\d.json", "{\"a\": 4}\n");
	answer(&r, FS.gen, WF_SYN_OK, runs, 1);
	{
		int n = FS.requests;

		show(&r, L"C:\\w\\d.json", "{\"a\": 4}\n");
		CHECK(n == FS.requests);
		CHECK(fg_of(&r, "\"a\"", &fg, 80) && fg == colour(WF_SC_KEY));
	}
	wf_app_destroy(&r.a);
}

static void colours_follow_scrolling_wide_text_and_narrow_panes(void)
{
	Rig r;
	uint32_t fg;
	/* {"名前": 1, "k": 22} : the key is 8 bytes (two wide characters and quotes) */
	static const char T[] =
	        "{\"\xe5\x90\x8d\xe5\x89\x8d\": 1, \"k\": 22}\n";
	WfSynRun runs[] = {{1, 8, WF_SC_KEY}, {11, 1, WF_SC_NUMBER},
	        {14, 3, WF_SC_KEY}, {19, 2, WF_SC_NUMBER}};

	wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
	rig_open(&r, true);
	show(&r, L"C:\\w\\a.json", T);
	answer(&r, FS.gen, WF_SYN_OK, runs, 4);
	CHECK(fg_of(&r, "\"\xe5\x90\x8d\xe5\x89\x8d\"", &fg, 80) &&
	        fg == colour(WF_SC_KEY));
	CHECK(fg_of(&r, "22", &fg, 80) && fg == colour(WF_SC_NUMBER));
	/* scrolled to the right by 4 cells: what is left keeps its colours */
	r.a.preview_left = 4;
	CHECK(fg_of(&r, "\"k\"", &fg, 80) && fg == colour(WF_SC_KEY));
	CHECK(fg_of(&r, "22", &fg, 80) && fg == colour(WF_SC_NUMBER));
	/* scrolled into the middle of a wide character: the half is dropped, the rest holds */
	r.a.preview_left = 3;
	CHECK(fg_of(&r, "22", &fg, 80) && fg == colour(WF_SC_NUMBER));
	/* narrow panes draw without trouble, whatever the width */
	for (int w = 1; w < 30; w++) {
		WfFrame *f = wf_frame_new(100, 30, &(WfStyle){0, 0, 0});

		r.a.preview_left = (size_t)(w % 7);
		wf_app_draw_preview(&r.a, f, 0, 1, w, 20);
		wf_frame_free(f);
	}
	wf_app_destroy(&r.a);
}

static void long_lines_and_bad_runs_cannot_hurt(void)
{
	Rig r;
	uint32_t fg;
	static char big[6000];
	/* runs that run past the end of the text, overlap or are not sorted by a
	 * misbehaving worker: they are clipped where the text ends, never read past it */
	WfSynRun wild[] = {{2, 4000, WF_SC_KEY}, {3, 2, WF_SC_NUMBER},
	        {4294967290u, 100, WF_SC_STRING}};

	wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
	rig_open(&r, true);
	/* a line longer than the pane keeps: drawn plain, still drawn */
	memset(big, 'a', sizeof big - 1);
	big[0] = '[';
	show(&r, L"C:\\w\\a.json", big);
	{
		WfSynRun one = {0, 1, WF_SC_PUNCTUATION};

		answer(&r, FS.gen, WF_SYN_OK, &one, 1);
	}
	CHECK(fg_of(&r, "[", &fg, 80));
	show(&r, L"C:\\w\\b.json", "[1]\n");
	answer(&r, FS.gen, WF_SYN_OK, wild, 3);
	{
		WfFrame *f = wf_frame_new(100, 30, &(WfStyle){0, 0, 0});

		wf_app_draw_preview(&r.a, f, 0, 1, 80,
		        20); /* must not read past the line */
		wf_frame_free(f);
	}
	/* such runs are not kept at all: the text is plain, not half coloured */
	CHECK(!r.a.prev_ov.active);
	CHECK(fg_of(&r, "[1]", &fg, 80) &&
	        fg == wf_theme_styles()->base.fg_rgb);
	wf_app_destroy(&r.a);
}

int main(void)
{
	only_what_a_grammar_can_colour_is_handed_over();
	the_answer_colours_the_text_and_a_stale_one_does_not();
	a_new_file_leaves_nothing_of_the_old_colours();
	colours_follow_scrolling_wide_text_and_narrow_panes();
	long_lines_and_bad_runs_cannot_hurt();
	return TEST_RESULT();
}

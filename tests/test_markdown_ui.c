/*
 * The Markdown preview as the person meets it (DR-DRAFT-025): the file
 * manager's preview of a .md file, and the editor's two panes. A fake file
 * world stands in for the disk; keys go in, frames come out.
 */
#include <stdlib.h>
#include <string.h>

#include "edit/wf_emdpane.h"
#include "edit/wf_esession.h"
#include "input/wf_exline.h"
#include "input/wf_keymap.h"
#include "render/wf_theme.h"
#include "text/wf_text.h"
#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

static struct
{
	wchar_t path[64];
	char data[2048];
	size_t n;
	char saved[2048];
	size_t saved_n;
	int saves;
} W;

static WfStatus fake_load(void *ctx,
        const wchar_t *path,
        size_t max,
        unsigned cp,
        const WfEncoding *force,
        WfLoaded *out)
{
	(void)ctx;
	(void)max;
	(void)cp;
	(void)force;
	memset(out, 0, sizeof *out);
	if (wcscmp(path, W.path)) {
		out->verdict = WF_FL_MISSING;
		return WF_OK;
	}
	out->verdict = WF_FL_OK;
	out->enc.kind = WF_ENC_UTF8;
	out->utf8 = malloc(W.n + 1);
	memcpy(out->utf8, W.data, W.n);
	out->utf8[W.n] = '\0';
	out->utf8_n = W.n;
	out->stamp.valid = true;
	out->stamp.size = W.n;
	out->stamp.links = 1;
	return WF_OK;
}

static WfStatus fake_over(void *ctx,
        const wchar_t *path,
        const WfFileStamp *was,
        const char *utf8,
        size_t n,
        WfEncoding enc,
        const WfSaveOptions *opt,
        WfSaveResult *out)
{
	(void)ctx;
	(void)path;
	(void)was;
	(void)enc;
	(void)opt;
	memset(out, 0, sizeof *out);
	memcpy(W.saved, utf8, n);
	W.saved_n = n;
	W.saves++;
	out->verdict = WF_SV_OK;
	out->stamp.valid = true;
	out->stamp.size = n;
	out->stamp.links = 1;
	return WF_OK;
}

static WfStatus fake_new(void *ctx,
        const wchar_t *path,
        const char *utf8,
        size_t n,
        WfEncoding enc,
        WfSaveResult *out)
{
	(void)ctx;
	(void)path;
	(void)utf8;
	(void)n;
	(void)enc;
	memset(out, 0, sizeof *out);
	out->verdict = WF_SV_EXISTS;
	return WF_OK;
}

static WfStatus fake_clip_get(void *ctx, char **utf8, size_t *n)
{
	(void)ctx;
	*utf8 = NULL;
	*n = 0;
	return WF_E_NOENT;
}

static WfStatus fake_clip_set(void *ctx, const char *utf8, size_t n)
{
	(void)ctx;
	(void)utf8;
	(void)n;
	return WF_OK;
}

static WfEditSession *S;
static int ROWS = 30, COLS = 120;
static uint64_t NOW = 1000;

static void setup(const wchar_t *path, const char *text, int md_split)
{
	WfEditConfig c;
	WfEditIo io = {NULL, fake_load, fake_over, fake_new, fake_clip_get,
	        fake_clip_set, NULL, NULL, NULL, NULL, NULL};
	char why[160];

	memset(&W, 0, sizeof W);
	wcscpy_s(W.path, 64, path);
	W.n = strlen(text);
	memcpy(W.data, text, W.n);
	wf_es_free(S);
	wf_es_config_default(&c);
	c.md_split = md_split;
	c.drafts = false;
	S = wf_es_new(&c, io);
	CHECK(S != NULL);
	NOW = 1000;
	wf_es_set_clock(S, NOW);
	CHECK(WF_OPEN_OK == wf_es_open(S, path, why, sizeof why));
}

static WfEsResult press(const WfEvent *e)
{
	wf_es_set_clock(S, NOW);
	return wf_es_event(S, e, ROWS, COLS);
}

static void typing(const char *s)
{
	for (; *s; s++) {
		WfEvent e = {WF_EV_CHAR, (uint32_t)(unsigned char)*s,
		        WF_KEY_NONE, 0};

		(void)press(&e);
	}
}

static WfEsResult alt(char c)
{
	WfEvent e = {WF_EV_ALT, (uint32_t)c, WF_KEY_NONE, 0};

	return press(&e);
}

static WfEsResult ctrl(char c)
{
	WfEvent e = {WF_EV_CTRL, (uint32_t)c, WF_KEY_NONE, 0};

	return press(&e);
}

static WfFrame *draw(void)
{
	WfFrame *f = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});

	wf_es_draw(S, f);
	return f;
}

/* The text of a row between columns [x0, x1). */
static void row_text(
        const WfFrame *f, int y, int x0, int x1, char *out, size_t cap)
{
	out[0] = '\0';
	for (int x = x0; x < x1 && x < f->cols; x++) {
		strcat_s(out, cap, f->cells[y * f->cols + x].utf8);
	}
}

/* The first cell at or after column x0 of row y whose text starts a word. */
static const WfCell *find_in_rows(
        const WfFrame *f, int x0, int x1, const char *word, int *at_y)
{
	for (int y = 1; y < f->rows - 2; y++) {
		char row[1024];
		const char *hit;

		row_text(f, y, x0, x1, row, sizeof row);
		hit = strstr(row, word);
		if (hit) {
			/* ASCII only: one cell per byte */
			if (at_y) {
				*at_y = y;
			}
			return &f->cells[y * f->cols + x0 + (int)(hit - row)];
		}
	}
	return NULL;
}

static void two_panes_show_text_and_preview(void)
{
	WfFrame *f;
	const WfCell *c;
	char sep[8];
	int y = 0;

	COLS = 120;
	setup(L"C:\\n\\a.md", "# Title\n\nsome *words* here\n\n- one\n- two\n",
	        0);
	f = draw();
	/* left: the source as typed; right: the heading without its mark */
	CHECK(NULL != find_in_rows(f, 0, 59, "# Title", NULL));
	c = find_in_rows(f, 60, 120, "Title", &y);
	CHECK(c != NULL);
	if (c) {
		CHECK((c->attributes & WF_ATTR_BOLD) &&
		        (c->attributes & WF_ATTR_UNDERLINE));
	}
	CHECK(NULL == find_in_rows(f, 60, 120, "# Title", NULL));
	CHECK(NULL != find_in_rows(f, 60, 120, "- one", NULL));
	c = find_in_rows(f, 60, 120, "words", NULL);
	CHECK(c && (c->attributes & WF_ATTR_ITALIC));
	row_text(f, 2, 60, 61, sep, sizeof sep);
	CHECK(!strcmp(sep, "|"));
	wf_frame_free(f);
}

static void follows_edits_after_the_deadline(void)
{
	WfFrame *f;

	COLS = 120;
	setup(L"C:\\n\\a.md", "# Title\n", 0);
	f = draw();
	wf_frame_free(f);
	typing("X");
	f = draw(); /* the layout waits: the right pane still shows the old text */
	CHECK(NULL == find_in_rows(f, 60, 120, "XTitle", NULL));
	CHECK(NULL != find_in_rows(f, 0, 59, "X# Title", NULL));
	wf_frame_free(f);
	CHECK(wf_es_timeout(S, NOW) <= WF_MDP_DEBOUNCE_MS);
	CHECK(!wf_es_tick(S, NOW + 10)); /* not yet */
	f = draw();
	CHECK(NULL == find_in_rows(f, 60, 120, "XTitle", NULL));
	wf_frame_free(f);
	/* typing again moves the deadline */
	NOW += 50;
	typing("Y");
	f = draw();
	wf_frame_free(f);
	CHECK(!wf_es_tick(S, NOW + 60)); /* 60 ms after the last key */
	CHECK(wf_es_tick(S, NOW + 90));
	f = draw();
	CHECK(NULL != find_in_rows(f, 60, 120, "XY# Title", NULL));
	wf_frame_free(f);
	/* nothing waits any more */
	CHECK(UINT32_MAX == wf_es_timeout(S, NOW + 1000));
}

static void narrow_falls_back_and_the_key_toggles(void)
{
	WfFrame *f;
	char msg[64];

	COLS = 80;
	setup(L"C:\\n\\a.md", "# Title\n", 0);
	f = draw();
	CHECK(NULL != find_in_rows(f, 0, 40, "# Title", NULL));
	CHECK(NULL == find_in_rows(f, 41, 80, "Title", NULL)); /* one pane */
	wf_frame_free(f);
	CHECK(WF_ES_REDRAW == alt('m')); /* 80 columns are enough for `on` */
	f = draw();
	CHECK(NULL != find_in_rows(f, 41, 80, "Title", NULL));
	wf_frame_free(f);
	(void)alt('m'); /* and off again */
	f = draw();
	CHECK(NULL == find_in_rows(f, 41, 80, "Title", NULL));
	wf_frame_free(f);
	COLS = 50; /* too narrow even to ask for it */
	(void)alt('m');
	snprintf(msg, sizeof msg, "%s", wf_es_status(S));
	CHECK(strstr(msg, "narrow") || strstr(msg, "60"));
	f = draw();
	CHECK(NULL == find_in_rows(f, 26, 50, "Title", NULL));
	wf_frame_free(f);
	COLS = 120;
}

static void configuration_off_and_on(void)
{
	WfFrame *f;

	COLS = 120;
	setup(L"C:\\n\\a.md", "# Title\n", 2); /* off */
	f = draw();
	CHECK(NULL == find_in_rows(f, 60, 120, "Title", NULL));
	wf_frame_free(f);
	(void)alt('m'); /* the key still turns it on for the session */
	f = draw();
	CHECK(NULL != find_in_rows(f, 60, 120, "Title", NULL));
	wf_frame_free(f);
	COLS = 70;
	setup(L"C:\\n\\a.md", "# Title\n", 1); /* on: from 60 columns */
	f = draw();
	CHECK(NULL != find_in_rows(f, 35, 70, "Title", NULL));
	wf_frame_free(f);
	COLS = 120;
}

static void other_files_have_no_preview(void)
{
	WfFrame *f;

	COLS = 120;
	setup(L"C:\\n\\a.txt", "# Title\n", 0);
	f = draw();
	CHECK(NULL == find_in_rows(f, 60, 120, "Title", NULL));
	wf_frame_free(f);
	(void)alt('m');
	CHECK(NULL != strstr(wf_es_status(S), "Markdown"));
	f = draw();
	CHECK(NULL == find_in_rows(f, 60, 120, "Title", NULL));
	wf_frame_free(f);
}

static void saving_is_the_same_with_or_without_the_pane(void)
{
	WfFrame *f;
	char with[2048], without[2048];
	size_t nw, nwo;

	COLS = 120;
	setup(L"C:\\n\\a.md", "# T\r\n\r\ntext\r\n", 0);
	typing("ab");
	f = draw();
	wf_frame_free(f);
	(void)wf_es_tick(S, NOW + 500);
	CHECK(WF_ES_REDRAW == ctrl('s'));
	CHECK(1 == W.saves);
	memcpy(with, W.saved, W.saved_n);
	nw = W.saved_n;
	setup(L"C:\\n\\a.md", "# T\r\n\r\ntext\r\n", 2);
	typing("ab");
	f = draw();
	wf_frame_free(f);
	CHECK(WF_ES_REDRAW == ctrl('s'));
	memcpy(without, W.saved, W.saved_n);
	nwo = W.saved_n;
	CHECK(nw == nwo && !memcmp(with, without, nw));
	CHECK(nw == strlen("ab# T\r\n\r\ntext\r\n") &&
	        !memcmp(with, "ab# T\r\n\r\ntext\r\n", nw));
}

static void the_left_pane_keeps_its_width_for_wrapping(void)
{
	WfFrame *f;
	WfEditConfig c;
	WfEditIo io = {NULL, fake_load, fake_over, fake_new, fake_clip_get,
	        fake_clip_set, NULL, NULL, NULL, NULL, NULL};
	char why[160];
	char longline[400];

	memset(longline, 'a', sizeof longline - 1);
	longline[sizeof longline - 1] = '\0';
	memset(&W, 0, sizeof W);
	wcscpy_s(W.path, 64, L"C:\\n\\a.md");
	snprintf(W.data, sizeof W.data, "%.100s\n", longline);
	W.n = strlen(W.data);
	wf_es_free(S);
	wf_es_config_default(&c);
	c.soft_wrap = true;
	c.drafts = false;
	S = wf_es_new(&c, io);
	COLS = 120;
	CHECK(WF_OPEN_OK == wf_es_open(S, L"C:\\n\\a.md", why, sizeof why));
	f = draw();
	{
		char row[256];

		/* a long line wraps at the left pane's width: its second row
		 * (after the gutter) is on screen, left of the separator */
		row_text(f, 2, 3, 60, row, sizeof row);
		CHECK(NULL != strchr(row, 'a'));
		/* the separator is on every row and the left text never
		 * reaches it */
		for (int y = 1; y < 6; y++) {
			row_text(f, y, 60, 61, row, sizeof row);
			CHECK(!strcmp(row, "|"));
		}
	}
	wf_frame_free(f);
}

static void keys_exist_in_every_profile(void)
{
	WfKeyConfig cfg;
	WfKeymap *m = NULL;
	WfEvent altm = {WF_EV_ALT, (uint32_t)'m', WF_KEY_NONE, 0};

	CHECK(WF_CMD_MD_PREVIEW == wf_ekm_lookup(WF_KM_PIERO, &altm).cmd);
	CHECK(WF_CMD_MD_PREVIEW == wf_ekm_lookup(WF_KM_NANO, &altm).cmd);
	wf_keyconfig_init(&cfg);
	for (int profile = 0; profile < 2; profile++) {
		char out[64] = "", out2[64] = "";

		cfg.profile = 0 == profile ? WF_PROFILE_NANO : WF_PROFILE_VIM;
		CHECK(WF_OK == wf_keymap_build_defaults(&cfg, WF_KM_PIERO, &m));
		if (0 == profile) {
			(void)wf_keymap_first_key(m, WF_CTX_EDIT,
			        WF_CMD_MD_PREVIEW, out, sizeof out);
			CHECK(!strcmp(out, "Alt+M"));
		} else { /* both modes of the Vim keys */
			(void)wf_keymap_first_key(m, WF_CTX_VIM_NORMAL,
			        WF_CMD_MD_PREVIEW, out, sizeof out);
			(void)wf_keymap_first_key(m, WF_CTX_VIM_INSERT,
			        WF_CMD_MD_PREVIEW, out2, sizeof out2);
			CHECK(!strcmp(out, "Alt+M") && !strcmp(out2, "Alt+M"));
		}
		wf_keymap_free(m);
		m = NULL;
	}
	{
		WfEx e;

		CHECK(wf_ex_parse("preview", 7, WF_EXS_EDITOR, &e) &&
		        WF_EX_PREVIEW == e.id);
		CHECK(!wf_ex_parse("preview", 7, WF_EXS_FM, &e));
	}
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

static void preview_draws_markdown_by_its_grammar(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	uint32_t gen = 0;
	WfPreviewPort port = {&gen, pv_submit, pv_cancel};
	WfPreview *p = NULL;
	WfFrame *f;
	char big[8000] = "# Big title\n\n";
	const char *src = "# Heading\n\nSome **bold** words.\n\n- one\n- two\n";
	const WfCell *c;
	int y = 0;

	CHECK(WF_OK ==
	        wf_app_init(&a, 100, 30, fake_port(&fp), L"C:\\Users\\dev"));
	wf_app_set_preview(&a, port);
	WfEvent j = {WF_EV_CHAR, 'j', WF_KEY_NONE, 0};

	a.mode->on_event(&a, &j);
	CHECK(WF_OK == wf_preview_decode(src, strlen(src), 0, &p));
	wf_app_on_preview(&a, gen, p);
	/* the entry is file2.txt: a text file keeps the line numbers */
	f = wf_frame_new(100, 30, &WF_STYLE_BASE);
	wf_app_draw_preview(&a, f, 0, 1, 100, 20);
	CHECK(NULL != find_in_rows(f, 0, 100, "1 | # Heading", NULL));
	wf_frame_free(f);
	/* the same text as a Markdown file */
	free(a.preview_path);
	a.preview_path = _wcsdup(L"C:\\Users\\dev\\notes.md");
	wf_app_md_invalidate(&a);
	f = wf_frame_new(100, 30, &WF_STYLE_BASE);
	wf_app_draw_preview(&a, f, 0, 1, 100, 20);
	CHECK(NULL == find_in_rows(f, 0, 100, "# Heading", NULL));
	c = find_in_rows(f, 0, 100, "Heading", &y);
	CHECK(c && (c->attributes & WF_ATTR_BOLD) &&
	        (c->attributes & WF_ATTR_UNDERLINE));
	c = find_in_rows(f, 0, 100, "bold", NULL);
	CHECK(c && (c->attributes & WF_ATTR_BOLD));
	CHECK(NULL != find_in_rows(f, 0, 100, "- one", NULL));
	wf_frame_free(f);
	/* the raw text of the file did not change */
	CHECK(a.preview && 6 <= a.preview->nlines);
	/* a long document scrolls by rendered rows */
	for (int i = 0; i < 150; i++) {
		strcat_s(big, sizeof big, "para\n\n");
	}
	strcat_s(big, sizeof big, "LAST_SOURCE_LINE\n");
	wf_app_preview_clear(&a);
	a.preview_path = _wcsdup(L"C:\\Users\\dev\\big.md");
	a.preview_busy = true;
	CHECK(WF_OK == wf_preview_decode(big, strlen(big), 0, &p));
	wf_app_on_preview(&a, a.preview_gen, p);
	a.mode = &WF_MODE_PREVIEW;
	WfEvent bottom = {WF_EV_KEY, 0, WF_KEY_CTRL_DOWN, 0};

	a.mode->on_event(&a, &bottom);
	CHECK(a.preview_top > 100);
	f = wf_frame_new(100, 30, &WF_STYLE_BASE);
	wf_app_draw(&a, f);
	CHECK(NULL != find_in_rows(f, 0, 100, "para", NULL));
	CHECK(NULL == find_in_rows(f, 0, 100, "Big title", NULL));
	wf_frame_free(f);
	/* A layout limit is independent of whether reading the source was cut.
	 * Its notice reserves one row, which must remain reachable by scrolling. */
	CHECK(a.md && a.md->doc && a.preview && !a.preview->truncated);
	if (a.md && a.md->doc) {
		size_t previous_top = a.preview_top;
		char notice[1024];

		a.md->doc->truncated = true;
		a.mode->on_event(&a, &bottom);
		CHECK(a.preview_top == previous_top + 1);
		f = wf_frame_new(100, 30, &WF_STYLE_BASE);
		/* Covered panes must not rebuild this cache at their narrower width
		 * before the fullscreen view draws at the same width as last time. */
		wf_app_draw(&a, f);
		CHECK(a.md->doc && a.md->doc->truncated);
		row_text(f, 28, 0, 100, notice, sizeof notice);
		CHECK(NULL != strstr(notice, "bounded preview; truncated"));
		CHECK(NULL !=
		        find_in_rows(f, 0, 100, "LAST_SOURCE_LINE", NULL));
		wf_frame_free(f);
	}
	wf_app_destroy(&a);
}

static void oversized_lines_keep_a_bounded_preview(void)
{
	for (int leading_line = 0; leading_line < 2; leading_line++) {
		WfEText text;
		WfMdPane pane = {0};
		size_t n = WF_MD_SRC_MAX + 8;
		char *source = malloc(n);

		CHECK(source);
		if (!source) {
			return;
		}
		memset(source, 'a', n);
		memcpy(source, leading_line ? "intro\nVISIBLE" : "VISIBLE",
		        leading_line ? 13 : 7);
		memcpy(source + WF_MD_SRC_MAX - 2, "\xe4\xb8\x80", 3);
		CHECK(WF_OK == wf_et_init(&text, WF_EOL_LF, n));
		CHECK(WF_OK == wf_et_load(&text, source, n));
		uint64_t generation = text.gen;
		CHECK(WF_OK == wf_mdp_refresh(&pane, &text, 400));
		CHECK(pane.doc && pane.doc->truncated &&
		        pane.doc->text_len > 200000);
		if (pane.doc && pane.doc->text) {
			CHECK(strstr(pane.doc->text, "VISIBLE"));
			CHECK(!strstr(pane.doc->text, "\xef\xbf\xbd"));
		}
		CHECK(text.gen == generation && text.bytes == n);
		CHECK(text.lines[text.n - 1].len ==
		        n - (leading_line ? 6u : 0u));
		CHECK(!memcmp(text.lines[text.n - 1].s,
		        source + (leading_line ? 6 : 0),
		        text.lines[text.n - 1].len));
		wf_mdp_free(&pane);
		wf_et_free(&text);
		free(source);
	}
}

int main(void)
{
	wf_set_ambiguous_width(1);
	preview_draws_markdown_by_its_grammar();
	two_panes_show_text_and_preview();
	follows_edits_after_the_deadline();
	narrow_falls_back_and_the_key_toggles();
	configuration_off_and_on();
	other_files_have_no_preview();
	saving_is_the_same_with_or_without_the_pane();
	the_left_pane_keeps_its_width_for_wrapping();
	keys_exist_in_every_profile();
	oversized_lines_keep_a_bounded_preview();
	wf_es_free(S);
	return TEST_RESULT();
}

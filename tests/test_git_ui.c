/*
 * Git in the file list (DR-DRAFT-038), with a port that records what is asked:
 * nothing is asked outside a work tree, the letters come with the answer and only
 * with the answer of the newest request, a reload does not ask at every event,
 * and what belongs to a repository is gone when the repository is.
 */
#include <string.h>

#include "config/wf_config.h"
#include "git/wf_gitport.h"
#include "input/wf_keymap.h"
#include "render/wf_theme.h"
#include "ui/wf_app.h"
#include "ui/wf_app_priv.h"
#include "ui/wf_layout.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

typedef struct
{
	WfApp a;
	FakePort fp;
	int roots, repos;
	uint32_t last_gen;
	wchar_t last_root[300];
	WfStatus repo_result;
	const wchar_t *
	        root_for; /* the folder that holds .git for C:\Users\dev (NULL: none) */
	bool later; /* root_of cannot tell without waiting: the worker resolves */
	bool resolve; /* the last request asked to find the .git as well */
	int mode;     /* what the last request asked for (WF_GIT_REPO_*) */
} Rig;

static WfGitRootKind root_of(
        void *ctx, const wchar_t *dir, WfGitEnv *env, wchar_t *root, size_t cap)
{
	Rig *r = ctx;
	size_t n = wcslen(r->root_for ? r->root_for : L"");

	r->roots++;
	if (!r->root_for || wcsncmp(dir, r->root_for, n)) {
		return WF_GIT_ROOT_NONE;
	}
	memset(env, 0, sizeof *env);
	env->kind = WF_GENV_WINDOWS;
	if (r->later) {
		wcscpy_s(root, cap,
		        dir); /* the folder shown: the .git is for the worker */
		return WF_GIT_ROOT_LATER;
	}
	wcscpy_s(root, cap, r->root_for);
	return WF_GIT_ROOT_FOUND;
}

static wchar_t g_answer_root[300]; /* the repository of the newest request */

static WfStatus repo(void *ctx, uint32_t gen, const wchar_t *root, int mode)
{
	Rig *r = ctx;

	r->mode = mode;
	r->resolve = WF_GIT_REPO_RESOLVE == mode;
	wcscpy_s(g_answer_root, 300, root);
	r->repos++;
	r->last_gen = gen;
	wcscpy_s(r->last_root, 300, root);
	return r->repo_result;
}

static void rig_open(Rig *r, const wchar_t *start, const wchar_t *root_for)
{
	WfGitPort p;

	memset(r, 0, sizeof *r);
	r->fp.app = &r->a;
	r->root_for = root_for;
	CHECK(WF_OK == wf_app_init(&r->a, 100, 30, fake_port(&r->fp), start));
	memset(&p, 0, sizeof p);
	p.ctx = r;
	p.root_of = root_of;
	p.repo = repo;
	wf_app_set_git(&r->a, p);
	r->a.now_ms = 1000;
}

/* The answer a worker would give for this porcelain text. */
static WfGitAnswer answer(
        uint32_t gen, WfGitState st, const char *text, size_t n)
{
	WfGitAnswer a;

	memset(&a, 0, sizeof a);
	a.kind = WF_GIT_A_REPO;
	a.gen = gen;
	a.state = st;
	a.asked = _wcsdup(
	        g_answer_root); /* every answer says what it was asked about */
	a.root = _wcsdup(g_answer_root);
	if (WF_GITR_OK == st) {
		CHECK(WF_OK ==
		        wf_gitstatus_parse((const uint8_t *)text, n, &a.table));
	}
	return a;
}

#define ANSWER(gen, st, lit) answer(gen, st, lit, sizeof lit - 1)

static const WfEntry *entry(const WfApp *a, const wchar_t *name)
{
	for (size_t i = 0; i < a->cur.dir->n; i++) {
		if (!wcscmp(a->cur.dir->e[i].name, name)) {
			return &a->cur.dir->e[i];
		}
	}
	return NULL;
}

/* The text of the row of `name` in the current pane. */
static bool row_of(const WfApp *a, const wchar_t *name, char *out, size_t cap)
{
	WfFrame *f = wf_frame_new(100, 30, &WF_STYLE_BASE);
	WfLayout lay;
	char needle[64];
	bool found = false;

	CHECK(f);
	wf_app_draw(a, f);
	wf_layout_compute(a->cols, a->rows, &a->ratios, &lay);
	WideCharToMultiByte(
	        CP_UTF8, 0, name, -1, needle, sizeof needle, NULL, NULL);
	for (int y = 0; y < f->rows && !found; y++) {
		char line[512] = "";

		for (int x = lay.pane[WF_PANE_CURRENT].x;
		        x < lay.pane[WF_PANE_CURRENT].x +
		                lay.pane[WF_PANE_CURRENT].w;
		        x++) {
			strcat_s(line, sizeof line,
			        f->cells[y * f->cols + x].utf8);
		}
		if (strstr(line, needle)) {
			strcpy_s(out, cap, line);
			found = true;
		}
	}
	wf_frame_free(f);
	return found;
}

static char last_char(const char *row)
{
	size_t n = strlen(row);

	return n ? row[n - 1] : 0;
}

static void outside_a_work_tree_nothing_is_asked_and_nothing_shown(void)
{
	static Rig r;
	char row[512];

	rig_open(&r, L"C:\\Users\\dev", NULL); /* no .git above */
	wf_app_reload(&r.a);
	CHECK(r.roots >= 1 &&
	        0 == r.repos); /* looked at the folder, asked nothing */
	CHECK(0 == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	CHECK(row_of(&r.a, L"file10.txt", row, sizeof row) &&
	        ' ' == last_char(row));
	wf_app_destroy(&r.a);
	/* without a port at all: nothing happens */
	{
		static Rig q;

		memset(&q, 0, sizeof q);
		q.fp.app = &q.a;
		CHECK(WF_OK ==
		        wf_app_init(&q.a, 100, 30, fake_port(&q.fp),
		                L"C:\\Users\\dev"));
		wf_app_reload(&q.a);
		CHECK(0 == q.roots && 0 == q.repos);
		wf_app_destroy(&q.a);
	}
}

/* The colour of the first cell of `name` in the current pane. */
static bool name_colour(const WfApp *a, const wchar_t *name, uint32_t *fg)
{
	WfFrame *f = wf_frame_new(100, 30, &WF_STYLE_BASE);
	WfLayout lay;
	char needle[64];
	bool found = false;

	CHECK(f);
	wf_app_draw(a, f);
	wf_layout_compute(a->cols, a->rows, &a->ratios, &lay);
	WideCharToMultiByte(
	        CP_UTF8, 0, name, -1, needle, sizeof needle, NULL, NULL);
	for (int y = 0; y < f->rows && !found; y++) {
		char line[512] = "";
		int x0 = lay.pane[WF_PANE_CURRENT].x;
		const char *at;

		for (int x = x0; x < x0 + lay.pane[WF_PANE_CURRENT].w; x++) {
			strcat_s(line, sizeof line,
			        f->cells[y * f->cols + x].utf8);
		}
		at = strstr(line, needle);
		if (at) { /* the cells of the row are one byte each here */
			*fg = f->cells[y * f->cols + x0 + (at - line)].fg_rgb;
			found = true;
		}
	}
	wf_frame_free(f);
	return found;
}

/* The name takes the colour of its letter; a folder that holds a change is
 * tinted (a setting, on unless turned off); what did not change is left as it
 * was. The cursor row keeps the cursor's colours. */
static void changed_names_and_their_folders_take_a_colour(void)
{
	static Rig r;
	static WfConfig cfg;
	WfGitAnswer a;
	uint32_t file10, file2, docs;
	const WfThemeStyles *t;

	wf_config_init(&cfg);
	rig_open(&r, L"C:\\Users\\dev", L"C:\\Users\\dev");
	r.a.config = &cfg;
	wf_app_reload(&r.a);
	a = ANSWER(r.last_gen, WF_GITR_OK,
	        "1 .M N... 100644 100644 100644 a b file10.txt\0"
	        "1 .M N... 100644 100644 100644 a b Docs/deep/er/x.txt\0");
	CHECK(wf_app_on_git(&r.a, &a));
	wf_gitanswer_free(&a);
	t = wf_theme_styles();
	for (size_t i = 0; i < r.a.cur.nvis; i++) { /* off the rows looked at */
		if (!wcscmp(r.a.cur.dir->e[r.a.cur.vis[i]].name,
		            L"file2.txt")) {
			r.a.cur.cursor = i;
		}
	}
	CHECK('M' == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	CHECK(0 == wf_app_git_letter(&r.a, entry(&r.a, L"Docs")));
	CHECK(wf_app_git_dir_changed(&r.a, entry(&r.a, L"Docs")));
	CHECK(!wf_app_git_dir_changed(&r.a, entry(&r.a, L"file10.txt")));
	CHECK(name_colour(&r.a, L"file10.txt", &file10));
	CHECK(name_colour(&r.a, L"file2.txt", &file2));
	CHECK(name_colour(&r.a, L"Docs", &docs));
	CHECK(file10 == t->warning.fg_rgb); /* the colour of its letter M */
	CHECK(docs == t->warning.fg_rgb);
	CHECK(file2 != t->warning.fg_rgb);
	cfg.git_folder_color = 0;
	CHECK(name_colour(&r.a, L"Docs", &docs));
	CHECK(docs != t->warning.fg_rgb);
	CHECK(name_colour(&r.a, L"file10.txt", &file10));
	CHECK(file10 == t->warning.fg_rgb); /* the file is not a setting */
	wf_app_destroy(&r.a);
}

/* Git names a submodule without a trailing '/': its change is shown on the
 * folder like any other. */
static void a_changed_submodule_is_marked_on_its_folder(void)
{
	static Rig r;
	WfGitAnswer a;
	uint32_t docs;

	rig_open(&r, L"C:\\Users\\dev", L"C:\\Users\\dev");
	wf_app_reload(&r.a);
	a = ANSWER(r.last_gen, WF_GITR_OK,
	        "1 .M S.M. 160000 160000 160000 a b Docs\0");
	CHECK(wf_app_on_git(&r.a, &a));
	wf_gitanswer_free(&a);
	CHECK('M' == wf_app_git_letter(&r.a, entry(&r.a, L"Docs")));
	for (size_t i = 0; i < r.a.cur.nvis; i++) { /* off the row looked at */
		if (!wcscmp(r.a.cur.dir->e[r.a.cur.vis[i]].name,
		            L"file2.txt")) {
			r.a.cur.cursor = i;
		}
	}
	CHECK(name_colour(&r.a, L"Docs", &docs));
	CHECK(docs == wf_theme_styles()->warning.fg_rgb);
	wf_app_destroy(&r.a);
}

static void the_letters_appear_with_the_answer_of_the_newest_request(void)
{
	static Rig r;
	char row[512];
	WfGitAnswer a;

	rig_open(&r, L"C:\\Users\\dev", L"C:\\Users\\dev");
	wf_app_reload(&r.a);
	CHECK(1 == r.repos && !wcscmp(r.last_root, L"C:\\Users\\dev"));
	/* the list is drawn at once, without letters */
	CHECK(row_of(&r.a, L"file10.txt", row, sizeof row));
	CHECK(0 == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	/* an answer for another generation is not shown */
	a = ANSWER(r.last_gen + 7, WF_GITR_OK,
	        "1 .M N... 100644 100644 100644 a b file10.txt\0");
	CHECK(!wf_app_on_git(&r.a, &a));
	wf_gitanswer_free(&a);
	CHECK(0 == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	/* the right one is */
	a = ANSWER(r.last_gen, WF_GITR_OK,
	        "1 .M N... 100644 100644 100644 a b file10.txt\0"
	        "? Docs/\0"
	        "1 A. N... 000000 100644 100644 000 b file2.txt\0");
	CHECK(wf_app_on_git(&r.a, &a));
	wf_gitanswer_free(&a);
	CHECK('M' == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	CHECK('A' == wf_app_git_letter(&r.a, entry(&r.a, L"file2.txt")));
	CHECK('U' ==
	        wf_app_git_letter(&r.a,
	                entry(&r.a, L"Docs"))); /* a folder: its own record */
	/* they are drawn at the right edge of the row, a space before them */
	CHECK(row_of(&r.a, L"file10.txt", row, sizeof row));
	CHECK('M' == last_char(row) && ' ' == row[strlen(row) - 2]);
	CHECK(row_of(&r.a, L"file2.txt", row, sizeof row) &&
	        'A' == last_char(row));
	/* the name keeps its room: still readable */
	CHECK(strstr(row, "file2.txt"));
	wf_app_destroy(&r.a);
}

static void a_folder_in_the_repository_is_looked_up_with_its_prefix(void)
{
	static Rig r;
	WfGitAnswer a;
	int asked;

	rig_open(&r, L"C:\\Users\\dev",
	        L"C:\\Users"); /* .git is one folder above */
	wf_app_reload(&r.a);
	CHECK(1 == r.repos && !wcscmp(r.last_root, L"C:\\Users"));
	a = ANSWER(r.last_gen, WF_GITR_OK,
	        "1 .M N... 100644 100644 100644 a b dev/file10.txt\0"
	        "1 .M N... 100644 100644 100644 a b file2.txt\0"); /* not in this folder */
	CHECK(wf_app_on_git(&r.a, &a));
	wf_gitanswer_free(&a);
	CHECK('M' == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	CHECK(0 ==
	        wf_app_git_letter(&r.a,
	                entry(&r.a,
	                        L"file2.txt"))); /* "dev/file2.txt" is not it */
	/* moving inside the same repository asks nothing again */
	asked = r.repos;
	r.a.now_ms += 500;
	wf_app_git_note_listing(&r.a, false);
	CHECK(asked == r.repos);
	wf_app_destroy(&r.a);
}

static void
a_reload_asks_again_only_after_a_second_and_keeps_the_letters_meanwhile(void)
{
	static Rig r;
	WfGitAnswer a;
	uint32_t first;

	rig_open(&r, L"C:\\Users\\dev", L"C:\\Users\\dev");
	wf_app_reload(&r.a);
	first = r.last_gen;
	a = ANSWER(first, WF_GITR_OK,
	        "1 .M N... 100644 100644 100644 a b file10.txt\0");
	CHECK(wf_app_on_git(&r.a, &a));
	wf_gitanswer_free(&a);
	/* a burst of reloads (a build writing files) is one request */
	for (int i = 0; i < 20; i++) {
		r.a.now_ms += 20;
		wf_app_git_note_listing(&r.a, true);
	}
	CHECK(1 == r.repos);
	/* the last of the burst is not lost: it is made when the gap has passed */
	{
		uint32_t wait = wf_app_git_timeout(&r.a);

		CHECK(wait > 0 && wait <= 1000 && wait != UINT32_MAX);
		r.a.now_ms += wait - 1;
		CHECK(!wf_app_git_tick(&r.a) && 1 == r.repos); /* not yet */
		r.a.now_ms += 1;
		CHECK(0 == wf_app_git_timeout(&r.a));
		CHECK(!wf_app_git_tick(&r.a));
		CHECK(2 == r.repos && r.last_gen != first);
		CHECK(UINT32_MAX ==
		        wf_app_git_timeout(&r.a)); /* made: nothing waits */
	}
	/* the letters stay until the new answer, and are replaced by it */
	CHECK('M' == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	a = ANSWER(r.last_gen, WF_GITR_OK,
	        "1 .M N... 100644 100644 100644 a b file2.txt\0");
	CHECK(wf_app_on_git(&r.a, &a));
	wf_gitanswer_free(&a);
	CHECK(0 == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	CHECK('M' == wf_app_git_letter(&r.a, entry(&r.a, L"file2.txt")));
	/* an old answer that comes late is dropped */
	a = ANSWER(first, WF_GITR_OK,
	        "1 .M N... 100644 100644 100644 a b file10.txt\0");
	CHECK(!wf_app_on_git(&r.a, &a));
	wf_gitanswer_free(&a);
	CHECK(0 == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	/* moving inside the repository after a long while asks again */
	r.a.now_ms += 10000;
	wf_app_git_note_listing(&r.a, false);
	CHECK(3 == r.repos);
	wf_app_destroy(&r.a);
}

static void no_git_or_a_failure_is_not_asked_again_at_every_listing(void)
{
	static Rig r;
	WfGitAnswer a;

	rig_open(&r, L"C:\\Users\\dev", L"C:\\Users\\dev");
	wf_app_reload(&r.a);
	a = answer(r.last_gen, WF_GITR_UNAVAILABLE, "", 0);
	CHECK(!wf_app_on_git(
	        &r.a, &a)); /* nothing was shown, nothing changes */
	wf_gitanswer_free(&a);
	CHECK(0 == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	for (int i = 0; i < 10; i++) {
		r.a.now_ms += 2000;
		wf_app_git_note_listing(&r.a, true);
	}
	CHECK(1 == r.repos); /* 20 s: not yet */
	r.a.now_ms += 20000;
	wf_app_git_note_listing(&r.a, true);
	CHECK(2 == r.repos);
	a = answer(r.last_gen, WF_GITR_FAILED, "", 0);
	wf_app_on_git(&r.a, &a);
	wf_gitanswer_free(&a);
	CHECK(0 == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	wf_app_destroy(&r.a);
}

static void leaving_the_work_tree_or_changing_repository_drops_what_was_known(
        void)
{
	static Rig r;
	WfGitAnswer a;

	rig_open(&r, L"C:\\Users\\dev", L"C:\\Users\\dev");
	wf_app_reload(&r.a);
	a = ANSWER(r.last_gen, WF_GITR_OK,
	        "1 .M N... 100644 100644 100644 a b file10.txt\0");
	CHECK(wf_app_on_git(&r.a, &a));
	wf_gitanswer_free(&a);
	CHECK(r.a.git.table.n > 0);
	/* another repository: the letters of the first are gone at once */
	r.root_for = L"C:\\Users\\dev\\other";
	wf_app_reload(&r.a);
	/* (the fake says C:\Users\dev is not under that root) */
	CHECK(0 == r.a.git.table.n && !r.a.git.root);
	CHECK(0 == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	/* back in a work tree: asked afresh */
	{
		int before = r.repos;

		r.root_for = L"C:\\Users\\dev";
		wf_app_reload(&r.a);
		CHECK(before + 1 == r.repos);
	}
	/* a root that cannot be asked about drops it too */
	r.repo_result = WF_E_NOMEM;
	r.root_for = L"C:\\Users";
	wf_app_reload(&r.a);
	CHECK(!r.a.git.root && 0 == r.a.git.table.n);
	wf_app_destroy(&r.a);
}

static void a_file_of_the_repository_is_found_for_the_editor(void)
{
	static Rig r;
	WfGitAnswer a;
	char letter, orig[64];

	rig_open(&r, L"C:\\Users\\dev", L"C:\\Users\\dev");
	wf_app_reload(&r.a);
	CHECK(!wf_app_git_file(&r.a, L"C:\\Users\\dev", "file10.txt", &letter,
	        orig, sizeof orig)); /* no answer yet */
	a = ANSWER(r.last_gen, WF_GITR_OK,
	        "2 R. N... 100644 100644 100644 a b R100 new.txt\0old.txt\0"
	        "? fresh.txt\0");
	CHECK(wf_app_on_git(&r.a, &a));
	wf_gitanswer_free(&a);
	CHECK(wf_app_git_file(&r.a, L"C:\\Users\\dev", "new.txt", &letter, orig,
	        sizeof orig));
	CHECK('R' == letter && !strcmp(orig, "old.txt"));
	CHECK(wf_app_git_file(&r.a, L"c:\\users\\DEV", "fresh.txt", &letter,
	        orig, sizeof orig));
	CHECK('U' == letter && !orig[0]);
	CHECK(wf_app_git_file(&r.a, L"C:\\Users\\dev", "unchanged.txt", &letter,
	        orig, sizeof orig));
	CHECK(0 == letter);
	/* another repository: nothing is known about it here */
	CHECK(!wf_app_git_file(
	        &r.a, L"C:\\Elsewhere", "new.txt", &letter, orig, sizeof orig));
	wf_app_destroy(&r.a);
}

/* A request that was dropped somewhere (the answer never comes) must not leave the
 * list without letters for good: it is asked again, late enough not to cut a slow
 * status short. */
static void a_request_nothing_came_back_for_is_asked_again_late(void)
{
	static Rig r;

	rig_open(&r, L"C:\\Users\\dev", L"C:\\Users\\dev");
	wf_app_reload(&r.a); /* asked at 1000 ms, never answered */
	for (int i = 0; i < 10; i++) {
		r.a.now_ms += 5000;
		wf_app_git_note_listing(&r.a, true);
	}
	CHECK(1 == r.repos); /* 51 s: a status may still be running */
	r.a.now_ms = 1000 + 74999;
	wf_app_git_note_listing(&r.a, true);
	CHECK(1 == r.repos);
	r.a.now_ms = 1000 + 75000;
	wf_app_git_note_listing(&r.a, true);
	CHECK(2 == r.repos);
	wf_app_destroy(&r.a);
}

/* Two repositories, one after the other: the first request of the second is not the
 * first request of the first, and an answer for one never lands on the other. */
static void an_answer_of_another_repository_is_never_taken(void)
{
	static Rig r;
	WfGitAnswer a, b;
	uint32_t gen_a, gen_b;

	rig_open(
	        &r, L"C:\\Users\\dev", L"C:\\Users"); /* the repository above */
	wf_app_reload(&r.a);
	gen_a = r.last_gen;
	wcscpy_s(g_answer_root, 300, r.last_root);
	a = ANSWER(gen_a, WF_GITR_OK,
	        "1 .M N... 100644 100644 100644 a b dev/file10.txt\0");
	/* before it arrives, the screen is in the repository that is below */
	r.root_for = L"C:\\Users\\dev";
	wf_app_reload(&r.a);
	gen_b = r.last_gen;
	CHECK(gen_b != gen_a); /* the generation goes on across repositories */
	CHECK(!wf_app_on_git(
	        &r.a, &a)); /* the late answer of the first is dropped */
	CHECK(0 == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	wf_gitanswer_free(&a);
	/* even with the very generation, an answer for the other repository is not taken */
	b = ANSWER(gen_b, WF_GITR_OK,
	        "1 .M N... 100644 100644 100644 a b file10.txt\0");
	free(b.asked);
	b.asked = _wcsdup(L"C:\\Users");
	CHECK(!wf_app_on_git(&r.a, &b));
	wf_gitanswer_free(&b);
	CHECK(0 == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	/* the right one is */
	wcscpy_s(g_answer_root, 300, r.last_root);
	b = ANSWER(gen_b, WF_GITR_OK,
	        "1 .M N... 100644 100644 100644 a b file10.txt\0");
	CHECK(wf_app_on_git(&r.a, &b));
	wf_gitanswer_free(&b);
	CHECK('M' == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	wf_app_destroy(&r.a);
}

/* Git says an untracked folder in one record: what is inside has none of its own. */
static void what_is_inside_an_untracked_folder_is_new_too(void)
{
	static Rig r;
	WfGitAnswer a;
	char letter, orig[32];

	rig_open(&r, L"C:\\Users\\dev",
	        L"C:\\Users"); /* this folder is "dev/" of the repository */
	wf_app_reload(&r.a);
	a = ANSWER(r.last_gen, WF_GITR_OK, "? dev/\0");
	CHECK(wf_app_on_git(&r.a, &a));
	wf_gitanswer_free(&a);
	CHECK('U' == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	CHECK('U' == wf_app_git_letter(&r.a, entry(&r.a, L"Docs")));
	CHECK(wf_app_git_file(&r.a, L"C:\\Users", "dev/file10.txt", &letter,
	        orig, sizeof orig));
	CHECK('U' == letter);
	CHECK(wf_app_git_file(&r.a, L"C:\\Users", "dev/deeper/still/file.c",
	        &letter, orig, sizeof orig));
	CHECK('U' == letter);
	CHECK(wf_app_git_file(
	        &r.a, L"C:\\Users", "devx/file.c", &letter, orig, sizeof orig));
	CHECK(0 ==
	        letter); /* a folder that only starts with the same letters */
	wf_app_destroy(&r.a);
}

/* On a WSL or a removable drive the screen cannot look for the .git without waiting:
 * it asks the worker to resolve, and takes the repository the answer names. */
static void a_folder_whose_repository_is_found_later_is_resolved_by_the_worker(
        void)
{
	static Rig r;
	WfGitAnswer a;
	char row[512];

	rig_open(&r, L"C:\\Users\\dev", L"C:\\Users\\dev");
	r.later = true;
	wf_app_reload(&r.a);
	CHECK(1 == r.repos && r.resolve); /* asked to find the .git as well */
	CHECK(!wcscmp(r.last_root, L"C:\\Users\\dev"));
	CHECK(row_of(
	        &r.a, L"file10.txt", row, sizeof row)); /* drawn, no wait */
	/* the worker found the .git one folder up */
	a = ANSWER(r.last_gen, WF_GITR_OK,
	        "1 .M N... 100644 100644 100644 a b dev/file10.txt\0");
	free(a.root);
	a.root = _wcsdup(L"C:\\Users");
	CHECK(wf_app_on_git(&r.a, &a));
	wf_gitanswer_free(&a);
	CHECK('M' == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	/* the repository is known now: the same folder again asks without resolving */
	r.a.now_ms += 20000;
	wf_app_git_note_listing(&r.a, false);
	CHECK(2 == r.repos && !r.resolve && !wcscmp(r.last_root, L"C:\\Users"));
	wf_app_destroy(&r.a);
}

/* No .git above: the worker says so and no more is asked at once. */
static void a_folder_in_no_work_tree_found_later_is_not_asked_again_at_once(
        void)
{
	static Rig r;
	WfGitAnswer a;

	rig_open(&r, L"C:\\Users\\dev", L"C:\\Users\\dev");
	r.later = true;
	wf_app_reload(&r.a);
	a = ANSWER(r.last_gen, WF_GITR_NOT_REPO, "");
	free(a.root);
	a.root = NULL;
	CHECK(!wf_app_on_git(&r.a, &a));
	wf_gitanswer_free(&a);
	CHECK(0 == wf_app_git_letter(&r.a, entry(&r.a, L"file10.txt")));
	r.a.now_ms += 2000;
	wf_app_git_note_listing(&r.a, true);
	CHECK(1 == r.repos);
	wf_app_destroy(&r.a);
}

/* Where the worker finds the .git, a folder below the root is not known to be in
 * that repository: a nested one may be there. The folder is checked, the letters
 * are hidden until it is, and a nested repository is asked about on its own. */
static void a_nested_repository_below_a_found_root_is_noticed(void)
{
	static Rig r;
	WfGitAnswer a;
	char letter, orig[8];

	rig_open(&r, L"C:\\Users\\dev", L"C:\\Users\\dev");
	r.later = true;
	wf_app_reload(&r.a);
	a = ANSWER(r.last_gen, WF_GITR_OK,
	        "1 .M N... 100644 100644 100644 a b dev/Docs/x.txt\0");
	free(a.root);
	a.root = _wcsdup(L"C:\\Users"); /* found one folder up */
	CHECK(wf_app_on_git(&r.a, &a));
	wf_gitanswer_free(&a);
	CHECK(wf_app_git_file(&r.a, L"C:\\Users", "dev/Docs/x.txt", &letter,
	        orig, sizeof orig));
	CHECK('M' == letter);
	/* into a folder below it: asked for the nearest .git, nothing hidden is shown */
	free(r.a.cwd);
	r.a.cwd = _wcsdup(L"C:\\Users\\dev\\Docs");
	r.a.now_ms += 100;
	wf_app_git_note_listing(&r.a, false);
	CHECK(WF_GIT_REPO_LOCATE == r.mode &&
	        !wcscmp(r.last_root, L"C:\\Users\\dev\\Docs"));
	CHECK(!wf_app_git_file(&r.a, L"C:\\Users", "dev/Docs/x.txt", &letter,
	        orig, sizeof orig));
	{
		WfEntry stand_in;

		memset(&stand_in, 0, sizeof stand_in);
		stand_in.name = (wchar_t
		                *)L"x.txt"; /* a file of the folder now shown */
		CHECK(0 ==
		        wf_app_git_letter(
		                &r.a, &stand_in)); /* hidden, not wrong */
	}
	/* the folder is in the same repository: the letters are back, nothing else asked */
	{
		int asked = r.repos;
		WfEntry stand_in;

		a = ANSWER(r.last_gen, WF_GITR_OK, "");
		free(a.asked);
		a.asked = _wcsdup(L"C:\\Users\\dev\\Docs");
		free(a.root);
		a.root = _wcsdup(L"C:\\Users");
		wf_gitstatus_free(&a.table);
		CHECK(wf_app_on_git(&r.a, &a));
		wf_gitanswer_free(&a);
		CHECK(asked == r.repos);
		CHECK(wf_app_git_file(&r.a, L"C:\\Users", "dev/Docs/x.txt",
		        &letter, orig, sizeof orig));
		memset(&stand_in, 0, sizeof stand_in);
		stand_in.name = (wchar_t *)L"x.txt";
		CHECK('M' == wf_app_git_letter(&r.a, &stand_in));
	}
	/* a deeper folder holds its own repository: that one is asked about */
	free(r.a.cwd);
	r.a.cwd = _wcsdup(L"C:\\Users\\dev\\Docs\\sub");
	r.a.now_ms += 100;
	wf_app_git_note_listing(&r.a, false);
	CHECK(WF_GIT_REPO_LOCATE == r.mode);
	a = ANSWER(r.last_gen, WF_GITR_OK, "");
	free(a.asked);
	a.asked = _wcsdup(L"C:\\Users\\dev\\Docs\\sub");
	free(a.root);
	a.root = _wcsdup(L"C:\\Users\\dev\\Docs\\sub");
	wf_gitstatus_free(&a.table);
	{
		int asked = r.repos;

		wf_app_on_git(&r.a, &a);
		wf_gitanswer_free(&a);
		CHECK(asked + 1 == r.repos && WF_GIT_REPO_PLAIN == r.mode &&
		        !wcscmp(r.last_root, L"C:\\Users\\dev\\Docs\\sub"));
	}
	/* the parent's table is not used for the nested repository's files */
	CHECK(!wf_app_git_file(&r.a, L"C:\\Users", "dev/Docs/x.txt", &letter,
	        orig, sizeof orig));
	wf_app_destroy(&r.a);
}

/* A status that is due when the folder changes must not overtake the check for a
 * nearer .git: that answer would be dropped as old and the folder never verified
 * (the letters hidden for good). The status waits for the check. */
static void a_status_due_with_a_folder_check_waits_for_the_check(void)
{
	static Rig r;
	WfGitAnswer a;
	char letter, orig[8];
	int asked;

	rig_open(&r, L"C:\\Users\\dev", L"C:\\Users\\dev");
	r.later = true;
	wf_app_reload(&r.a);
	a = ANSWER(r.last_gen, WF_GITR_OK,
	        "1 .M N... 100644 100644 100644 a b dev/Docs/x.txt\0");
	free(a.root);
	a.root = _wcsdup(L"C:\\Users");
	CHECK(wf_app_on_git(&r.a, &a));
	wf_gitanswer_free(&a);
	/* the status is old (more than 10 s) and the folder changes */
	r.a.now_ms += 11000;
	asked = r.repos;
	free(r.a.cwd);
	r.a.cwd = _wcsdup(L"C:\\Users\\dev\\Docs");
	wf_app_git_note_listing(&r.a, false);
	CHECK(asked + 1 == r.repos &&
	        WF_GIT_REPO_LOCATE == r.mode); /* only the check */
	/* a timer that fires while the check is out asks nothing either */
	r.a.git.due_ms = r.a.now_ms;
	CHECK(!wf_app_git_tick(&r.a) && asked + 1 == r.repos);
	CHECK(0 == r.a.git.due_ms); /* and does not fire again and again */
	/* the check comes back: same repository, and the status that was due is asked */
	a = ANSWER(r.last_gen, WF_GITR_OK, "");
	free(a.asked);
	a.asked = _wcsdup(L"C:\\Users\\dev\\Docs");
	free(a.root);
	a.root = _wcsdup(L"C:\\Users");
	wf_app_on_git(&r.a, &a);
	wf_gitanswer_free(&a);
	CHECK(asked + 2 == r.repos && WF_GIT_REPO_PLAIN == r.mode);
	/* the folder is verified: the letters of the table that was kept are shown */
	CHECK(wf_app_git_file(&r.a, L"C:\\Users", "dev/Docs/x.txt", &letter,
	        orig, sizeof orig));
	CHECK('M' == letter);
	wf_app_destroy(&r.a);
}

/* The editor asks for a file's base with what the screen knows, and only what it
 * knows for sure: before the folder is checked for a nearer .git, the parent's
 * root is not used (a nested repository's file would be "all new" against the
 * parent's HEAD, and it would stay so). The worker finds the repository then. */
static void the_editor_does_not_use_a_parent_root_before_the_folder_is_checked(
        void)
{
	static Rig r;
	WfGitAnswer a;
	WfGitEnv env;
	const wchar_t *root = NULL;
	char rel[64], orig[64];
	bool expect_new = true;
	const wchar_t *file = L"C:\\Users\\dev\\Docs\\sub\\f.c";

	rig_open(&r, L"C:\\Users\\dev", L"C:\\Users\\dev");
	r.later = true;
	wf_app_reload(&r.a);
	a = ANSWER(r.last_gen, WF_GITR_OK,
	        "1 .M N... 100644 100644 100644 a b dev/Docs/sub/f.c\0");
	free(a.root);
	a.root = _wcsdup(L"C:\\Users");
	CHECK(wf_app_on_git(&r.a, &a));
	wf_gitanswer_free(&a);
	CHECK(WF_GENV_WINDOWS == wf_gitenv_classify_text(file, &env));
	/* the folder of the file is the one on screen and has been checked */
	free(r.a.cwd);
	r.a.cwd = _wcsdup(L"C:\\Users\\dev\\Docs\\sub");
	r.a.git.vcwd = _wcsdup(L"C:\\Users\\dev\\Docs\\sub");
	CHECK(wf_app_git_base_hint(&r.a, &env, file, &root, rel, sizeof rel,
	        &expect_new, orig, sizeof orig));
	CHECK(!strcmp(rel, "dev/Docs/sub/f.c") && !wcscmp(root, L"C:\\Users"));
	/* the screen moved into it and the check is still out: nothing is used */
	free(r.a.git.vcwd);
	r.a.git.vcwd = _wcsdup(L"C:\\Users\\dev");
	CHECK(!wf_app_git_base_hint(&r.a, &env, file, &root, rel, sizeof rel,
	        &expect_new, orig, sizeof orig));
	/* a file of another environment or outside the root is never matched */
	CHECK(WF_GENV_WINDOWS ==
	        wf_gitenv_classify_text(L"D:\\other\\f.c", &env));
	r.a.git.vcwd = (free(r.a.git.vcwd), _wcsdup(r.a.cwd));
	CHECK(!wf_app_git_base_hint(&r.a, &env, L"D:\\other\\f.c", &root, rel,
	        sizeof rel, &expect_new, orig, sizeof orig));
	wf_app_destroy(&r.a);
}

/* A reload while the first answer is awaited is not lost: the request under way
 * is left alone, and Git is asked again when its answer is in. */
static void a_reload_while_the_answer_is_awaited_is_asked_again_after_it(void)
{
	static Rig r;
	WfGitAnswer a;

	rig_open(&r, L"C:\\Users\\dev", L"C:\\Users\\dev");
	wf_app_reload(&r.a);
	CHECK(1 == r.repos);
	r.a.now_ms += 400;
	wf_app_git_note_listing(
	        &r.a, true); /* the files changed; the answer is out */
	r.a.now_ms += 600;
	wf_app_git_note_listing(&r.a, true);
	CHECK(1 ==
	        r.repos); /* not cancelled, not asked a second time at once */
	CHECK(UINT32_MAX == wf_app_git_timeout(&r.a));
	r.a.now_ms += 2000;
	a = ANSWER(r.last_gen, WF_GITR_OK,
	        "1 .M N... 100644 100644 100644 a b file10.txt\0");
	CHECK(wf_app_on_git(&r.a, &a)); /* what it saw may be old */
	wf_gitanswer_free(&a);
	CHECK(0 ==
	        wf_app_git_timeout(
	                &r.a)); /* asked again at once: the gap has passed */
	CHECK(!wf_app_git_tick(&r.a) && 2 == r.repos);
	/* with no reload in between, nothing is asked again */
	{
		static Rig q;

		rig_open(&q, L"C:\\Users\\dev", L"C:\\Users\\dev");
		wf_app_reload(&q.a);
		q.a.now_ms += 3000;
		a = ANSWER(q.last_gen, WF_GITR_OK,
		        "1 .M N... 100644 100644 100644 a b file10.txt\0");
		wf_app_on_git(&q.a, &a);
		wf_gitanswer_free(&a);
		CHECK(UINT32_MAX == wf_app_git_timeout(&q.a));
		wf_app_destroy(&q.a);
	}
	wf_app_destroy(&r.a);
}

int main(void)
{
	outside_a_work_tree_nothing_is_asked_and_nothing_shown();
	the_letters_appear_with_the_answer_of_the_newest_request();
	changed_names_and_their_folders_take_a_colour();
	a_changed_submodule_is_marked_on_its_folder();
	a_folder_in_the_repository_is_looked_up_with_its_prefix();
	a_reload_asks_again_only_after_a_second_and_keeps_the_letters_meanwhile();
	no_git_or_a_failure_is_not_asked_again_at_every_listing();
	leaving_the_work_tree_or_changing_repository_drops_what_was_known();
	a_file_of_the_repository_is_found_for_the_editor();
	a_request_nothing_came_back_for_is_asked_again_late();
	an_answer_of_another_repository_is_never_taken();
	what_is_inside_an_untracked_folder_is_new_too();
	a_folder_whose_repository_is_found_later_is_resolved_by_the_worker();
	a_folder_in_no_work_tree_found_later_is_not_asked_again_at_once();
	a_nested_repository_below_a_found_root_is_noticed();
	a_reload_while_the_answer_is_awaited_is_asked_again_after_it();
	a_status_due_with_a_folder_check_waits_for_the_check();
	the_editor_does_not_use_a_parent_root_before_the_folder_is_checked();
	return TEST_RESULT();
}

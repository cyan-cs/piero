/*
 * What Git costs (DR-DRAFT-038), measured on the Git of this machine, by hand:
 * finding git.exe, asking whether a folder is a work tree, the status of
 * repositories of 10 to 50,000 files (the process, the parse and sort, the whole
 * trip through the worker), the content of a file at HEAD, the line diff, what a
 * row of the file list costs, what the editor pays for a key with the diff pane
 * on and off, and how many diffs each debounce time would make for a typing
 * trace. WSL is not measured here: it is run only where a distribution answers.
 * Medians over RUNS (spread is min..max); no thresholds.
 */
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/wf_egitpane.h"
#include "edit/wf_esession.h"
#include "edit/wf_esession_internal.h"
#include "git/wf_gitbackend_win.h"
#include "git/wf_gitenv.h"
#include "git/wf_gitstatus.h"
#include "git/wf_gitworker.h"
#include "text/wf_linediff.h"

#define RUNS 31

static double qpc_ms(LARGE_INTEGER a, LARGE_INTEGER b)
{
	LARGE_INTEGER f;

	QueryPerformanceFrequency(&f);
	return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

typedef struct
{
	double med, lo, hi, p99;
} Stat;

static Stat stat_of(double *v, size_t n)
{
	Stat s;

	qsort(v, n, sizeof *v, cmp_d);
	s.med = v[n / 2];
	s.lo = v[0];
	s.hi = v[n - 1];
	s.p99 = v[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1];
	return s;
}

static void line(const char *what, Stat s, const char *unit)
{
	printf("%-52s %9.3f %s   (%.3f .. %.3f, p99 %.3f)\n", what, s.med, unit,
	        s.lo, s.hi, s.p99);
}

static wchar_t root[MAX_PATH];

static void join(wchar_t *out, const wchar_t *a, const wchar_t *b)
{
	swprintf(out, MAX_PATH * 2, L"%ls\\%ls", a, b);
}

static void remove_tree(const wchar_t *dir)
{
	wchar_t pat[MAX_PATH * 2];
	WIN32_FIND_DATAW fd;
	HANDLE h;

	join(pat, dir, L"*");
	h = FindFirstFileW(pat, &fd);
	if (INVALID_HANDLE_VALUE != h) {
		do {
			wchar_t p[MAX_PATH * 2];

			if (!wcscmp(fd.cFileName, L".") ||
			        !wcscmp(fd.cFileName, L"..")) {
				continue;
			}
			join(p, dir, fd.cFileName);
			SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				remove_tree(p);
			} else {
				DeleteFileW(p);
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir);
}

static bool git_ok(
        const WfGitEnv *env, const wchar_t *dir, const wchar_t *const *args)
{
	WfGitRunResult r;
	WfStatus st =
	        wf_git_exec(env, dir, args, 600000, 64u << 20, NULL, NULL, &r);
	bool ok = WF_OK == st && 0 == r.exit_code;

	wf_gitproc_result_free(&r);
	return ok;
}

#define GIT(env, dir, ...)                                                     \
	git_ok(env, dir, (const wchar_t *const[]){__VA_ARGS__, NULL})

static void put(
        const wchar_t *dir, const wchar_t *name, const void *data, size_t n)
{
	wchar_t p[MAX_PATH * 2];
	HANDLE h;
	DWORD done;

	join(p, dir, name);
	h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE != h) {
		WriteFile(h, data, (DWORD)n, &done, NULL);
		CloseHandle(h);
	}
}

/* A repository of `files` small files in folders of 100, 1% modified and 1% new. */
static bool make_repo(const WfGitEnv *env, wchar_t *dir, int files)
{
	wchar_t sub[MAX_PATH * 2];
	int made_dirs = 0;

	swprintf(dir, MAX_PATH * 2, L"%ls\\repo%d", root, files);
	CreateDirectoryW(dir, NULL);
	if (!GIT(env, dir, L"init", L"-q", L"-b", L"main") ||
	        !GIT(env, dir, L"config", L"user.name", L"t") ||
	        !GIT(env, dir, L"config", L"user.email", L"t@example.com") ||
	        !GIT(env, dir, L"config", L"core.autocrlf", L"false")) {
		return false;
	}
	for (int i = 0; i < files; i++) {
		wchar_t name[64];

		if (0 == i % 100) {
			swprintf(name, 64, L"d%04d", i / 100);
			join(sub, dir, name);
			CreateDirectoryW(sub, NULL);
			made_dirs++;
		}
		swprintf(name, 64, L"d%04d\\f%06d.txt", i / 100, i);
		put(dir, name, "line one\nline two\n", 18);
	}
	if (!GIT(env, dir, L"add", L"-A") ||
	        !GIT(env, dir, L"commit", L"-q", L"-m", L"c")) {
		return false;
	}
	for (int i = 0; i < files; i += 100) { /* 1% modified */
		wchar_t name[64];

		swprintf(name, 64, L"d%04d\\f%06d.txt", i / 100, i);
		put(dir, name, "line one\nCHANGED\n", 17);
	}
	for (int i = 0; i < files; i += 100) { /* 1% new */
		wchar_t name[64];

		swprintf(name, 64, L"d%04d\\new%06d.txt", i / 100, i);
		put(dir, name, "new\n", 4);
	}
	(void)made_dirs;
	return true;
}

static void bench_detection(const WfGitEnv *env)
{
	double v[RUNS];
	LARGE_INTEGER a, b;
	wchar_t found[MAX_PATH * 2], deep[MAX_PATH * 2];

	for (int i = 0; i < RUNS; i++) {
		wf_git_cache_reset();
		QueryPerformanceCounter(&a);
		(void)wf_git_capability(env, NULL, NULL);
		QueryPerformanceCounter(&b);
		v[i] = qpc_ms(a, b);
	}
	line("Windows Git: first look (PATH scan, no process)",
	        stat_of(v, RUNS), "ms");
	for (int i = 0; i < RUNS; i++) {
		QueryPerformanceCounter(&a);
		for (int k = 0; k < 10000; k++) {
			(void)wf_git_capability(env, NULL, NULL);
		}
		QueryPerformanceCounter(&b);
		v[i] = qpc_ms(a, b) / 10000.0 * 1000.0;
	}
	line("Windows Git: the answer kept (per call)", stat_of(v, RUNS), "us");
	swprintf(deep, MAX_PATH * 2, L"%ls\\repo10\\d0000\\a\\b\\c\\d\\e\\f\\g",
	        root);
	for (int i = 0; i < RUNS; i++) {
		QueryPerformanceCounter(&a);
		(void)wf_git_nearest_dotgit(deep, found, MAX_PATH * 2);
		QueryPerformanceCounter(&b);
		v[i] = qpc_ms(a, b) * 1000.0;
	}
	line("nearest .git, 8 folders below the root (attributes)",
	        stat_of(v, RUNS), "us");
	{
		wchar_t repo[MAX_PATH * 2];

		swprintf(repo, MAX_PATH * 2, L"%ls\\repo10", root);
		for (int i = 0; i < RUNS; i++) {
			WfGitRunResult r;

			QueryPerformanceCounter(&a);
			(void)wf_git_exec(env, repo,
			        (const wchar_t *const[]){L"rev-parse",
			                L"--is-inside-work-tree", NULL},
			        60000, 256, NULL, NULL, &r);
			QueryPerformanceCounter(&b);
			wf_gitproc_result_free(&r);
			v[i] = qpc_ms(a, b);
		}
		line("repository detection (git rev-parse)", stat_of(v, RUNS),
		        "ms");
	}
}

static void bench_status(const WfGitEnv *env, int files)
{
	wchar_t dir[MAX_PATH * 2];
	double run[9], parse[9];
	LARGE_INTEGER a, b;
	size_t entries = 0, bytes = 0;
	char label[96];
	WfGitWorker *w = NULL;
	double trip[9];

	if (!make_repo(env, dir, files)) {
		printf("could not make the repository of %d files\n", files);
		return;
	}
	for (int i = 0; i < 9; i++) {
		WfGitRunResult r;
		WfGitStatus s;

		QueryPerformanceCounter(&a);
		(void)wf_git_exec(env, dir,
		        (const wchar_t *const[]){L"status", L"--porcelain=v2",
		                L"-z", L"--untracked-files=normal", NULL},
		        600000, 64u << 20, NULL, NULL, &r);
		QueryPerformanceCounter(&b);
		run[i] = qpc_ms(a, b);
		QueryPerformanceCounter(&a);
		(void)wf_gitstatus_parse(r.out, r.out_n, &s);
		QueryPerformanceCounter(&b);
		parse[i] = qpc_ms(a, b);
		entries = s.n;
		bytes = r.out_n;
		wf_gitstatus_free(&s);
		wf_gitproc_result_free(&r);
	}
	snprintf(label, sizeof label,
	        "status of %d files: git process (%zu records, %zu B)", files,
	        entries, bytes);
	line(label, stat_of(run, 9), "ms");
	snprintf(label, sizeof label, "status of %d files: parse and sort",
	        files);
	line(label, stat_of(parse, 9), "ms");
	if (WF_OK == wf_gitworker_start(&w)) {
		for (int i = 0; i < 9; i++) {
			WfGitAnswer ans;
			uint32_t gen = (uint32_t)i + 1;

			QueryPerformanceCounter(&a);
			(void)wf_gitworker_repo(w, gen, env, dir, false);
			for (;;) {
				WaitForSingleObject(wf_gitworker_event(w), 50);
				if (wf_gitworker_poll(w, &ans)) {
					wf_gitanswer_free(&ans);
					break;
				}
			}
			QueryPerformanceCounter(&b);
			trip[i] = qpc_ms(a, b);
		}
		snprintf(label, sizeof label,
		        "status of %d files: request to answer (worker)",
		        files);
		line(label, stat_of(trip, 9), "ms");
		(void)wf_gitworker_stop(w);
	}
	/* the lookups a list makes: 40 rows, as often as a frame is drawn */
	{
		WfGitRunResult r;
		WfGitStatus s;
		double per[RUNS];

		(void)wf_git_exec(env, dir,
		        (const wchar_t *const[]){L"status", L"--porcelain=v2",
		                L"-z", L"--untracked-files=normal", NULL},
		        600000, 64u << 20, NULL, NULL, &r);
		(void)wf_gitstatus_parse(r.out, r.out_n, &s);
		for (int i = 0; i < RUNS; i++) {
			QueryPerformanceCounter(&a);
			for (int k = 0; k < 1000; k++) {
				for (int row = 0; row < 40; row++) {
					char key[64];
					int n = sprintf_s(key, sizeof key,
					        "d%04d/f%06d.txt",
					        (k + row) %
					                (files / 100 ? files / 100
					                             : 1),
					        ((k + row) % files));

					(void)wf_gitstatus_find(
					        &s, key, (size_t)n);
				}
			}
			QueryPerformanceCounter(&b);
			per[i] = qpc_ms(a, b) / 1000.0 * 1000.0;
		}
		snprintf(label, sizeof label,
		        "a frame's 40 row lookups in %d records", files);
		line(label, stat_of(per, RUNS), "us");
		wf_gitstatus_free(&s);
		wf_gitproc_result_free(&r);
	}
}

static void bench_head(const WfGitEnv *env)
{
	wchar_t dir[MAX_PATH * 2];
	double v[RUNS];
	LARGE_INTEGER a, b;
	char *big = malloc(1u << 20);

	swprintf(dir, MAX_PATH * 2, L"%ls\\repo10", root);
	memset(big, 'x', 1u << 20);
	put(dir, L"big.txt", big, 1u << 20);
	put(dir, L"small.txt", "x\n", 2);
	GIT(env, dir, L"add", L"big.txt", L"small.txt");
	GIT(env, dir, L"commit", L"-q", L"-m", L"b");
	for (int pass = 0; pass < 2; pass++) {
		const wchar_t *spec =
		        pass ? L"HEAD:big.txt" : L"HEAD:small.txt";

		for (int i = 0; i < RUNS; i++) {
			WfGitRunResult r;

			QueryPerformanceCounter(&a);
			(void)wf_git_exec(env, dir,
			        (const wchar_t *const[]){
			                L"cat-file", L"blob", spec, NULL},
			        60000, 4u << 20, NULL, NULL, &r);
			QueryPerformanceCounter(&b);
			wf_gitproc_result_free(&r);
			v[i] = qpc_ms(a, b);
		}
		line(pass ? "HEAD content of a 1 MiB file (git cat-file)"
		          : "HEAD content of a 2 byte file (git cat-file)",
		        stat_of(v, RUNS), "ms");
	}
	free(big);
}

static char *numbered(int lines, int every, size_t *n)
{
	char *t = malloc((size_t)lines * 48);
	size_t k = 0;

	for (int i = 0; i < lines; i++) {
		if (every && 0 == i % every) {
			k += (size_t)sprintf_s(t + k, (size_t)lines * 48 - k,
			        "CHANGED line %d\n", i);
		} else {
			k += (size_t)sprintf_s(t + k, (size_t)lines * 48 - k,
			        "line number %d of the file\n", i);
		}
	}
	*n = k;
	return t;
}

static void bench_diff(void)
{
	static const int sizes[] = {2000, 20000, 100000};
	static const int every[] = {0, 1000, 100, 20};

	for (size_t s = 0; s < 3; s++) {
		for (size_t e = 0; e < 4; e++) {
			size_t an, bn;
			char *a = numbered(sizes[s], 0, &an),
			     *b = numbered(sizes[s], every[e], &bn);
			double v[RUNS];
			char label[96];
			WfStatus st = WF_OK;

			for (int i = 0; i < RUNS; i++) {
				LARGE_INTEGER t0, t1;
				WfLineDiff d;

				QueryPerformanceCounter(&t0);
				st = wf_linediff(a, an, b, bn, &d);
				QueryPerformanceCounter(&t1);
				wf_linediff_free(&d);
				v[i] = qpc_ms(t0, t1);
			}
			snprintf(label, sizeof label,
			        "line diff, %d lines, %d changed%s", sizes[s],
			        every[e] ? sizes[s] / every[e] : 0,
			        WF_OK == st ? "" : " (too many)");
			line(label, stat_of(v, RUNS), "ms");
			free(a);
			free(b);
		}
	}
}

static char *g_file;
static size_t g_file_n;

static WfStatus fake_load(void *ctx,
        const wchar_t *path,
        size_t max,
        unsigned cp,
        const WfEncoding *force,
        WfLoaded *out)
{
	(void)ctx;
	(void)path;
	(void)max;
	(void)cp;
	(void)force;
	memset(out, 0, sizeof *out);
	out->verdict = WF_FL_OK;
	out->enc.kind = WF_ENC_UTF8;
	out->utf8 = malloc(g_file_n + 1);
	memcpy(out->utf8, g_file, g_file_n + 1);
	out->utf8_n = g_file_n;
	out->stamp.valid = true;
	out->stamp.size = g_file_n;
	out->stamp.links = 1;
	return WF_OK;
}

static WfStatus fake_over(void *ctx,
        const wchar_t *p,
        const WfFileStamp *was,
        const char *u,
        size_t n,
        WfEncoding enc,
        const WfSaveOptions *opt,
        WfSaveResult *out)
{
	(void)ctx;
	(void)p;
	(void)was;
	(void)u;
	(void)n;
	(void)enc;
	(void)opt;
	memset(out, 0, sizeof *out);
	return WF_E_ACCESS;
}

static WfStatus fake_new(void *ctx,
        const wchar_t *p,
        const char *u,
        size_t n,
        WfEncoding enc,
        WfSaveResult *out)
{
	(void)ctx;
	(void)p;
	(void)u;
	(void)n;
	(void)enc;
	memset(out, 0, sizeof *out);
	return WF_E_ACCESS;
}

static WfStatus fake_clip_get(void *ctx, char **u, size_t *n)
{
	(void)ctx;
	*u = NULL;
	*n = 0;
	return WF_E_NOENT;
}

static WfStatus fake_clip_set(void *ctx, const char *u, size_t n)
{
	(void)ctx;
	(void)u;
	(void)n;
	return WF_OK;
}

static uint32_t g_base_gen;
static uint64_t g_base_id;

static WfStatus p_base(void *c, uint32_t gen, uint64_t id, const wchar_t *p)
{
	(void)c;
	(void)p;
	g_base_gen = gen;
	g_base_id = id;
	return WF_OK;
}

static uint32_t g_diff_gen;
static uint64_t g_diff_id;
static WfStatus p_diff(void *c,
        uint32_t gen,
        uint64_t id,
        WfGitBlob *b,
        const char *t,
        size_t n)
{
	(void)c;
	(void)b;
	(void)t;
	(void)n;
	g_diff_gen = gen;
	g_diff_id = id;
	return WF_OK;
}

static void p_cancel(void *c)
{
	(void)c;
}

/* The cost of a key (its handling and the drawing of the screen) with the diff
 * pane showing a diff of `changes` changed lines, and without Git. */
static void bench_editor(void)
{
	size_t an;
	char *base = numbered(5000, 0, &an);
	enum
	{
		KEYS = 2000
	};

	g_file = numbered(5000, 100, &g_file_n);
	for (int with_git = 0; with_git < 2; with_git++) {
		WfEditConfig c;
		WfEditIo io = {NULL, fake_load, fake_over, fake_new,
		        fake_clip_get, fake_clip_set, NULL, NULL, NULL, NULL,
		        NULL};
		WfEditSession *s;
		WfFrame *f = wf_frame_new(160, 40, &(WfStyle){0, 0, 0});
		static double v[KEYS];
		char why[64];
		LARGE_INTEGER a, b;
		uint64_t now = 1000;
		char label[96];

		wf_es_config_default(&c);
		s = wf_es_new(&c, io);
		if (with_git) {
			WfGitPort p = {
			        NULL, NULL, NULL, p_base, p_diff, p_cancel};

			wf_es_set_git(s, p);
		}
		wf_es_set_clock(s, now);
		(void)wf_es_open(s, L"C:\\repo\\f.c", why, sizeof why);
		if (with_git) {
			WfGitAnswer ans;

			memset(&ans, 0, sizeof ans);
			ans.kind = WF_GIT_A_BASE;
			ans.gen = g_base_gen;
			ans.id = g_base_id;
			ans.state = WF_GITR_OK;
			ans.base = wf_gitblob_new(base, an);
			(void)wf_es_on_git(s, &ans);
			wf_gitanswer_free(&ans);
			wf_es_draw(s, f);
			(void)wf_es_tick(s, now + 1);
			memset(&ans, 0, sizeof ans);
			ans.kind = WF_GIT_A_DIFF;
			ans.gen = g_diff_gen;
			ans.id = g_diff_id;
			ans.state = WF_GITR_OK;
			(void)wf_linediff(
			        base, an, g_file, g_file_n, &ans.diff);
			ans.state = WF_GITR_OK;
			(void)wf_es_on_git(s, &ans);
			wf_gitanswer_free(&ans);
		}
		for (int i = 0; i < KEYS; i++) {
			WfEvent ev = {WF_EV_CHAR, (uint32_t)('a' + i % 26),
			        WF_KEY_NONE, 0};

			now += 30;
			wf_es_set_clock(s, now);
			QueryPerformanceCounter(&a);
			(void)wf_es_event(s, &ev, 40, 160);
			wf_es_draw(s, f);
			QueryPerformanceCounter(&b);
			v[i] = qpc_ms(a, b);
			(void)wf_es_tick(s, now);
		}
		snprintf(label, sizeof label,
		        "editor: key to painted screen, 5000 lines, diff pane "
		        "%s",
		        with_git ? "ON (100 hunks)" : "off (no Git)");
		line(label, stat_of(v, KEYS), "ms");
		wf_frame_free(f);
		wf_es_free(s);
	}
	free(g_file);
	free(base);
}

/* Which debounce time: the diffs a typing trace would cost, and how stale the
 * pane is at worst. The trace: bursts of 12 keys 80 ms apart, a pause of 700 ms,
 * for 60 s; the cost of a diff is the measured median above for 20,000 lines
 * with 200 changes. */
static void bench_debounce(void)
{
	static const unsigned delays[] = {50, 100, 150, 200, 300, 500};
	size_t an, bn;
	char *a = numbered(20000, 0, &an), *b = numbered(20000, 100, &bn);
	double v[RUNS], cost;
	LARGE_INTEGER t0, t1;

	for (int i = 0; i < RUNS; i++) {
		WfLineDiff d;

		QueryPerformanceCounter(&t0);
		(void)wf_linediff(a, an, b, bn, &d);
		QueryPerformanceCounter(&t1);
		wf_linediff_free(&d);
		v[i] = qpc_ms(t0, t1);
	}
	cost = stat_of(v, RUNS).med;
	printf("\ndebounce time against a typing trace (bursts of 12 keys 80 "
	       "ms apart, 700 ms pauses, 60 s),\n"
	       "diff cost %.3f ms (20000 lines, 200 changes):\n",
	        cost);
	printf("  %-10s %-14s %-16s %-22s\n", "delay ms", "diffs in 60 s",
	        "CPU ms in 60 s", "worst staleness ms");
	for (size_t i = 0; i < sizeof delays / sizeof *delays; i++) {
		unsigned d = delays[i], t = 0, diffs = 0;
		unsigned last_key = 0;
		bool pending = false;

		while (t < 60000) { /* one burst */
			for (int k = 0; k < 12; k++) {
				if (pending &&
				        t - last_key >=
				                d) { /* a gap long enough inside the burst */
					diffs++;
					pending = false;
				}
				last_key = t;
				pending = true;
				t += 80;
			}
			if (pending && 700 >= d) {
				diffs++;
				pending = false;
			}
			if (pending) { /* the pause is shorter than the delay: it waits on */
			}
			t += 700;
		}
		printf("  %-10u %-14u %-16.2f %-22u\n", d, diffs, diffs * cost,
		        d);
	}
	free(a);
	free(b);
}

int main(void)
{
	WfGitEnv env;
	wchar_t tmp[MAX_PATH];

	if (WF_GENV_WINDOWS != wf_gitenv_classify_text(L"C:\\x", &env) ||
	        WF_GIT_AVAILABLE != wf_git_capability(&env, NULL, NULL)) {
		printf("Windows Git is not available: nothing to measure\n");
		return 0;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	GetTempPathW(MAX_PATH, tmp);
	swprintf(root, MAX_PATH, L"%lspiero-bench-git-%lu", tmp,
	        GetCurrentProcessId());
	remove_tree(root);
	CreateDirectoryW(root, NULL);
	printf("Windows Git, medians of %d runs unless said\n\n", RUNS);
	if (make_repo(&env, (wchar_t[MAX_PATH * 2]){0}, 10)) {
		/* repo10 is used by the detection and the HEAD measurements */
	}
	bench_detection(&env);
	bench_head(&env);
	printf("\n");
	bench_status(&env, 10);
	bench_status(&env, 1000);
	bench_status(&env, 10000);
	if (!GetEnvironmentVariableA("PIERO_BENCH_NO_50K", NULL, 0)) {
		bench_status(&env, 50000);
	}
	printf("\n");
	bench_diff();
	printf("\n");
	bench_editor();
	bench_debounce();
	remove_tree(root);
	return 0;
}

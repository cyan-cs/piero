/*
 * Git through the Git of the environment that owns the repository
 * (DR-DRAFT-038): the classification of paths, the status table, the line diff,
 * the contract that no environment ever uses the other's Git, and the process
 * runner (against this very program, run as a helper with --echo, --sleep ...).
 */
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fs/wf_loc.h"
#include "git/wf_gitbackend_win.h"
#include "git/wf_gitenv.h"
#include "git/wf_gitproc_win.h"
#include "git/wf_gitstatus.h"
#include "text/wf_linediff.h"
#include "wf_test.h"

static WfGitEnvKind kind_of(const wchar_t *p, char *distro, size_t cap)
{
	WfGitEnv e;
	WfGitEnvKind k = wf_gitenv_classify_text(p, &e);

	if (distro && cap) {
		strcpy_s(distro, cap, e.distro);
	}
	return k;
}

static void paths_are_classified_by_the_environment_that_owns_them(void)
{
	char d[WF_WSL_NAME_MAX];

	CHECK(WF_GENV_WINDOWS == kind_of(L"C:\\x\\y", d, sizeof d));
	CHECK(WF_GENV_WINDOWS == kind_of(L"c:/x", d, sizeof d));
	CHECK(WF_GENV_WINDOWS == kind_of(L"\\\\?\\D:\\x", d, sizeof d));
	CHECK(WF_GENV_WINDOWS == kind_of(L"C:", d, sizeof d));
	CHECK(WF_GENV_WSL ==
	                kind_of(L"\\\\wsl.localhost\\Ubuntu\\home\\u", d,
	                        sizeof d) &&
	        !strcmp(d, "Ubuntu"));
	CHECK(WF_GENV_WSL == kind_of(L"\\\\wsl$\\Debian\\x", d, sizeof d) &&
	        !strcmp(d, "Debian"));
	CHECK(WF_GENV_WSL ==
	                kind_of(L"\\\\WSL.LOCALHOST\\kali-linux\\", d,
	                        sizeof d) &&
	        !strcmp(d, "kali-linux"));
	CHECK(WF_GENV_WSL ==
	                kind_of(L"\\\\?\\UNC\\wsl.localhost\\Arch\\x", d,
	                        sizeof d) &&
	        !strcmp(d, "Arch"));
	CHECK(WF_GENV_WSL ==
	                kind_of(L"\\\\wsl.localhost\\CustomDistro", d,
	                        sizeof d) &&
	        !strcmp(d, "CustomDistro"));
	/* a name that cannot be a distribution never becomes an argument */
	CHECK(WF_GENV_OTHER ==
	        kind_of(L"\\\\wsl.localhost\\-evil\\x", d, sizeof d));
	CHECK(WF_GENV_OTHER ==
	        kind_of(L"\\\\wsl.localhost\\a b\\x", d, sizeof d));
	CHECK(WF_GENV_OTHER ==
	        kind_of(L"\\\\wsl.localhost\\a;b\\x", d, sizeof d));
	CHECK(WF_GENV_OTHER ==
	        kind_of(L"\\\\wsl.localhost\\\xE6\x97\xA5\\x", d, sizeof d));
	CHECK(WF_GENV_OTHER == kind_of(L"\\\\wsl.localhost\\", d, sizeof d));
	CHECK(WF_GENV_OTHER == kind_of(L"\\\\server\\share\\x", d, sizeof d));
	CHECK(WF_GENV_OTHER == kind_of(L"/home/x", d, sizeof d));
	CHECK(WF_GENV_OTHER == kind_of(L"C:\\a.zip|docs", d, sizeof d));
	CHECK(WF_GENV_OTHER == kind_of(L"", d, sizeof d));
	CHECK(WF_GENV_OTHER == wf_gitenv_classify_text(NULL, NULL));
	CHECK(WF_GENV_OTHER == kind_of(L"relative\\x", d, sizeof d));
	/* the system drive is local */
	{
		wchar_t sys[MAX_PATH];

		if (GetSystemDirectoryW(sys, MAX_PATH)) {
			CHECK(WF_GENV_WINDOWS == wf_gitenv_classify(sys, NULL));
		}
	}
}

static void a_wsl_path_becomes_a_linux_path_without_a_shell(void)
{
	char out[512];

	CHECK(WF_OK ==
	        wf_gitenv_wsl_linux_path(
	                L"\\\\wsl.localhost\\Ubuntu\\home\\u\\my proj", out,
	                sizeof out));
	CHECK(!strcmp(out, "/home/u/my proj"));
	CHECK(WF_OK ==
	        wf_gitenv_wsl_linux_path(L"\\\\wsl$\\Ubuntu", out, sizeof out));
	CHECK(!strcmp(out, "/"));
	CHECK(WF_OK ==
	        wf_gitenv_wsl_linux_path(
	                L"\\\\wsl$\\Ubuntu\\", out, sizeof out));
	CHECK(!strcmp(out, "/"));
	CHECK(WF_OK ==
	        wf_gitenv_wsl_linux_path(
	                L"\\\\wsl.localhost\\Ubuntu\\home\\u\\", out,
	                sizeof out));
	CHECK(!strcmp(out, "/home/u"));
	CHECK(WF_OK ==
	        wf_gitenv_wsl_linux_path(
	                L"\\\\wsl.localhost\\Ubuntu\\home\\\\u", out,
	                sizeof out));
	CHECK(!strcmp(out, "/home/u"));
	/* Japanese and shell characters are bytes of the path, nothing more */
	CHECK(WF_OK ==
	        wf_gitenv_wsl_linux_path(
	                L"\\\\wsl.localhost\\Ubuntu\\home\\\x65E5\x672C\x8A9E",
	                out, sizeof out));
	CHECK(!strcmp(out, "/home/\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"));
	CHECK(WF_OK ==
	        wf_gitenv_wsl_linux_path(
	                L"\\\\wsl.localhost\\Ubuntu\\tmp\\a\"b&c^d;e$(x)'f",
	                out, sizeof out));
	CHECK(!strcmp(out, "/tmp/a\"b&c^d;e$(x)'f"));
	/* what is not a path of that kind */
	CHECK(WF_E_INVAL ==
	        wf_gitenv_wsl_linux_path(L"\\\\wsl.localhost\\Ubuntu\\a\\..\\b",
	                out, sizeof out));
	CHECK(WF_E_INVAL ==
	        wf_gitenv_wsl_linux_path(L"\\\\wsl.localhost\\Ubuntu\\a\\.\\b",
	                out, sizeof out));
	CHECK(WF_E_INVAL ==
	        wf_gitenv_wsl_linux_path(L"\\\\wsl.localhost\\Ubuntu\\a\xD800",
	                out, sizeof out));
	CHECK(WF_E_INVAL ==
	        wf_gitenv_wsl_linux_path(L"C:\\x", out, sizeof out));
	CHECK(WF_E_INVAL == wf_gitenv_wsl_linux_path(NULL, out, sizeof out));
	CHECK(WF_E_INVAL ==
	        wf_gitenv_wsl_linux_path(
	                L"\\\\wsl.localhost\\Ubuntu\\home\\u", out, 4));
}

static const char *rec(char *buf, size_t cap, const char *a)
{
	strcpy_s(buf, cap, a);
	return buf;
}

static void the_status_of_porcelain_v2_is_read_whole(void)
{
	/* NUL-separated; names with spaces, a tab, Japanese; a rename; a conflict */
	static const char text[] =
	        "# branch.oid abc\0"
	        "1 .M N... 100644 100644 100644 aaa bbb src/main.c\0"
	        "1 M. N... 100644 100644 100644 aaa bbb with space.c\0"
	        "1 A. N... 000000 100644 100644 000 bbb new\ttab.c\0"
	        "1 .D N... 100644 100644 000000 aaa bbb gone.c\0"
	        "1 AM N... 000000 100644 100644 000 bbb added then edited.c\0"
	        "2 R. N... 100644 100644 100644 aaa bbb R100 new name.c\0old "
	        "name.c\0"
	        "u UU N... 100644 100644 100644 100644 a b c conflict.c\0"
	        "? untracked \xE6\x97\xA5\xE6\x9C\xAC.txt\0"
	        "? newdir/\0"
	        "! ignored.o\0";
	WfGitStatus s;
	const WfGitEntry *e;

	CHECK(WF_OK ==
	        wf_gitstatus_parse((const uint8_t *)text, sizeof text - 1, &s));
	CHECK(9 == s.n); /* the header and the ignored file are not entries */
	for (size_t i = 1; i < s.n; i++) { /* sorted by path bytes */
		CHECK(strcmp(s.e[i - 1].path, s.e[i].path) < 0);
	}
	e = wf_gitstatus_find(&s, "src/main.c", 10);
	CHECK(e && '.' == e->index && 'M' == e->work &&
	        'M' == wf_gitentry_letter(e));
	e = wf_gitstatus_find(&s, "with space.c", 12);
	CHECK(e && 'M' == e->index && '.' == e->work &&
	        'M' == wf_gitentry_letter(e));
	e = wf_gitstatus_find(&s, "new\ttab.c", 9);
	CHECK(e && 'A' == wf_gitentry_letter(e));
	e = wf_gitstatus_find(&s, "gone.c", 6);
	CHECK(e && 'D' == wf_gitentry_letter(e));
	e = wf_gitstatus_find(&s, "added then edited.c", 19);
	CHECK(e && 'A' == e->index && 'M' == e->work &&
	        'A' == wf_gitentry_letter(e));
	e = wf_gitstatus_find(&s, "new name.c", 10);
	CHECK(e && 'R' == wf_gitentry_letter(e) && e->orig &&
	        !strcmp(e->orig, "old name.c"));
	CHECK(!wf_gitstatus_find(&s, "old name.c", 10));
	e = wf_gitstatus_find(&s, "conflict.c", 10);
	CHECK(e && e->conflict && '!' == wf_gitentry_letter(e));
	e = wf_gitstatus_find(&s, "untracked \xE6\x97\xA5\xE6\x9C\xAC.txt", 20);
	CHECK(e && 'U' == wf_gitentry_letter(e));
	e = wf_gitstatus_find(&s, "newdir/", 7);
	CHECK(e && 'U' == wf_gitentry_letter(e));
	CHECK(!wf_gitstatus_find(
	        &s, "newdir", 6)); /* a folder is looked up with its '/' */
	CHECK(!wf_gitstatus_find(
	        &s, "src/main", 8)); /* a prefix is not a path */
	CHECK(!wf_gitstatus_find(&s, "src/main.cc", 11));
	CHECK(!wf_gitstatus_find(&s, "", 0));
	wf_gitstatus_free(&s);
	wf_gitstatus_free(&s); /* safe to repeat */
	CHECK(0 == s.n && !s.e && !s.blob);
}

/* Git says an untracked folder once; what is inside is found by the folder. */
static void a_path_inside_an_untracked_folder_is_covered_by_it(void)
{
	static const char text[] =
	        "? newdir/\0"
	        "? a/b/\0"
	        "1 .M N... 100644 100644 100644 a b a/tracked.c\0"
	        "? plain.txt\0";
	WfGitStatus s;
	const WfGitEntry *e;

	CHECK(WF_OK ==
	        wf_gitstatus_parse((const uint8_t *)text, sizeof text - 1, &s));
	e = wf_gitstatus_find_covering(&s, "newdir/file.c", 13);
	CHECK(e && 'U' == wf_gitentry_letter(e) && !strcmp(e->path, "newdir/"));
	e = wf_gitstatus_find_covering(&s, "newdir/deeper/more/file.c", 25);
	CHECK(e && !strcmp(e->path, "newdir/"));
	e = wf_gitstatus_find_covering(&s, "a/b/c/d.txt", 11);
	CHECK(e && !strcmp(e->path, "a/b/"));
	e = wf_gitstatus_find_covering(
	        &s, "newdir/", 7); /* the folder itself */
	CHECK(e && !strcmp(e->path, "newdir/"));
	e = wf_gitstatus_find_covering(
	        &s, "a/tracked.c", 11); /* its own record wins */
	CHECK(e && 'M' == wf_gitentry_letter(e));
	CHECK(!wf_gitstatus_find_covering(
	        &s, "newdirx/file.c", 14)); /* not the same folder */
	CHECK(!wf_gitstatus_find_covering(
	        &s, "a/other.c", 9)); /* "a/" is not untracked */
	CHECK(!wf_gitstatus_find_covering(
	        &s, "plain.txt/x", 11)); /* a file is not a folder */
	CHECK(!wf_gitstatus_find_covering(&s, "other/file.c", 12));
	wf_gitstatus_free(&s);
	CHECK(!wf_gitstatus_find_covering(&s, "x", 1));
}

static void a_folder_holds_a_change_when_a_record_lies_below_it(void)
{
	static const char text[] =
	        "1 .M N... 100644 100644 100644 a b src/deep/er/f.c\0"
	        "1 .M N... 100644 100644 100644 a b srcx/g.c\0"
	        "? newdir/\0"
	        "1 .M N... 100644 100644 100644 a b zed.txt\0";
	WfGitStatus s;

	CHECK(WF_OK ==
	        wf_gitstatus_parse((const uint8_t *)text, sizeof text - 1, &s));
	CHECK(wf_gitstatus_has_under(&s, "src/", 4));
	CHECK(wf_gitstatus_has_under(&s, "src/deep/", 9));
	CHECK(wf_gitstatus_has_under(&s, "newdir/", 7)); /* its own record */
	CHECK(wf_gitstatus_has_under(&s, "srcx/", 5));
	CHECK(!wf_gitstatus_has_under(&s, "sr/", 3));
	CHECK(!wf_gitstatus_has_under(&s, "src/dee/", 8));
	CHECK(!wf_gitstatus_has_under(&s, "zed.txt/", 8)); /* a file */
	CHECK(!wf_gitstatus_has_under(&s, "zzz/", 4));     /* past the end */
	CHECK(!wf_gitstatus_has_under(&s, "", 0));
	wf_gitstatus_free(&s);
	CHECK(!wf_gitstatus_has_under(&s, "src/", 4)); /* an empty table */
	CHECK(!wf_gitstatus_has_under(NULL, "src/", 4));
}

static void letters_follow_the_rule_of_the_list(void)
{
	static const struct
	{
		char x, y;
		bool conflict;
		char want;
	} t[] = {{'.', '.', false, 0}, {'.', 'M', false, 'M'},
	        {'M', '.', false, 'M'}, {'T', '.', false, 'M'},
	        {'.', 'T', false, 'M'}, {'A', '.', false, 'A'},
	        {'A', 'M', false, 'A'}, {'A', 'D', false, 'D'},
	        {'D', '.', false, 'D'}, {'.', 'D', false, 'D'},
	        {'R', '.', false, 'R'}, {'R', 'M', false, 'R'},
	        {'C', '.', false, 'A'}, {'.', '?', false, 'U'},
	        {'U', 'U', true, '!'}, {'M', 'M', false, 'M'}};

	for (size_t i = 0; i < sizeof t / sizeof *t; i++) {
		WfGitEntry e = {"p", 1, NULL, t[i].x, t[i].y, t[i].conflict};

		if (wf_gitentry_letter(&e) != t[i].want) {
			printf("letter for %c%c: %c\n", t[i].x, t[i].y,
			        wf_gitentry_letter(&e));
		}
		CHECK(wf_gitentry_letter(&e) == t[i].want);
	}
	CHECK(0 == wf_gitentry_letter(NULL));
}

static void a_status_that_does_not_fit_is_refused_whole(void)
{
	static const char *const bad[] = {
	        "1 .M N... 100644 100644 100644 aaa bbb", /* no path */
	        "1 .M N... 100644 100644 100644 aaa bbb ",
	        "1 ZZ N... 100644 100644 100644 aaa bbb x", "1 .M N... 100644",
	        "2 R. N... 100644 100644 100644 a b R100 new", "? ", "?x",
	        "X something", "u UU N... a b c d e f g"};
	char buf[256];
	WfGitStatus s;

	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
		size_t n = strlen(rec(buf, sizeof buf, bad[i]));

		CHECK(WF_E_CORRUPT ==
		        wf_gitstatus_parse((const uint8_t *)buf, n + 1, &s));
		CHECK(!s.e && !s.blob && 0 == s.n);
	}
	CHECK(WF_OK == wf_gitstatus_parse(NULL, 0, &s) && 0 == s.n);
	CHECK(!wf_gitstatus_find(&s, "x", 1));
	wf_gitstatus_free(&s);
}

/* The table is sized by the records of the input, not by its bytes: bytes that
 * are only separators ask for nothing, and more records than the bound are
 * refused with nothing kept. */
static void the_table_follows_the_records_and_is_bounded(void)
{
	size_t n = (size_t)8 << 20, more = WF_GITSTATUS_MAX_ENTRIES + 1;
	uint8_t *nuls = calloc(n, 1);
	char *many = malloc(more * 4 + 1);
	WfGitStatus s;

	CHECK(nuls && many);
	if (!nuls || !many) {
		free(nuls);
		free(many);
		return;
	}
	CHECK(WF_OK == wf_gitstatus_parse(nuls, n, &s) && 0 == s.n);
	wf_gitstatus_free(&s);
	for (size_t i = 0; i < more; i++) {
		memcpy(many + i * 4, "? a", 4); /* "? a" and its NUL */
	}
	CHECK(WF_E_RANGE ==
	        wf_gitstatus_parse((const uint8_t *)many, more * 4, &s));
	CHECK(!s.e && !s.blob && 0 == s.n);
	CHECK(WF_OK ==
	                wf_gitstatus_parse((const uint8_t *)many,
	                        (WF_GITSTATUS_MAX_ENTRIES) * 4, &s) &&
	        WF_GITSTATUS_MAX_ENTRIES == s.n);
	wf_gitstatus_free(&s);
	free(nuls);
	free(many);
}

static void a_big_status_is_found_by_search(void)
{
	enum
	{
		N = 50000
	};
	size_t cap = (size_t)N * 64, n = 0;
	char *buf = malloc(cap);
	WfGitStatus s;
	ULONGLONG t0;
	size_t hits = 0;

	CHECK(buf);
	if (!buf) {
		return;
	}
	for (int i = N - 1; i >= 0; i--) { /* not in order */
		n += (size_t)sprintf_s(buf + n, cap - n,
		        "1 .M N... 100644 100644 100644 a b dir%03d/file%05d.c",
		        i % 500, i);
		buf[n++] = '\0';
	}
	t0 = GetTickCount64();
	CHECK(WF_OK == wf_gitstatus_parse((const uint8_t *)buf, n, &s) &&
	        N == s.n);
	printf("info: %d records parsed and sorted in %llu ms\n", N,
	        (unsigned long long)(GetTickCount64() - t0));
	t0 = GetTickCount64();
	for (int i = 0; i < N; i++) {
		char key[64];
		int k = sprintf_s(
		        key, sizeof key, "dir%03d/file%05d.c", i % 500, i);

		hits += NULL != wf_gitstatus_find(&s, key, (size_t)k);
	}
	printf("info: %d lookups in %llu ms\n", N,
	        (unsigned long long)(GetTickCount64() - t0));
	CHECK(N == hits);
	CHECK(!wf_gitstatus_find(&s, "dir000/file00001.c", 18));
	wf_gitstatus_free(&s);
	free(buf);
}

static size_t lines_of(const char *t)
{
	size_t n = 0, len = strlen(t);

	for (size_t i = 0; i < len; i++) {
		n += '\n' == t[i];
	}
	return n + (len && '\n' != t[len - 1]);
}

static bool diff_text(const char *a, const char *b, WfLineDiff *d)
{
	return WF_OK == wf_linediff(a, strlen(a), b, strlen(b), d);
}

/* Applies the hunks to the old lines: the new lines must come out. */
static bool script_rebuilds(const WfLineDiff *d)
{
	size_t i = 0, j = 0;

	for (size_t h = 0; h <= d->n; h++) {
		size_t a_stop = h < d->n ? d->h[h].a : d->a_n;
		size_t b_stop = h < d->n ? d->h[h].b : d->b_n;

		if (a_stop - i != b_stop - j) {
			return false; /* the equal stretch must be as long on both sides */
		}
		for (; i < a_stop; i++, j++) {
			const WfDiffLine *x = &d->a_line[i], *y = &d->b_line[j];
			size_t xl =
			        x->len && '\r' == d->a_text[x->off + x->len - 1]
			        ? x->len - 1
			        : x->len;
			size_t yl =
			        y->len && '\r' == d->b_text[y->off + y->len - 1]
			        ? y->len - 1
			        : y->len;

			if (xl != yl ||
			        memcmp(d->a_text + x->off, d->b_text + y->off,
			                xl)) {
				return false;
			}
		}
		if (h < d->n) {
			i += d->h[h].an;
			j += d->h[h].bn;
		}
	}
	return true;
}

static size_t lcs_len(
        const char *const *x, size_t xn, const char *const *y, size_t yn)
{
	size_t *row = calloc(yn + 1, sizeof *row), best;

	for (size_t i = 1; i <= xn; i++) {
		size_t diag = 0;

		for (size_t j = 1; j <= yn; j++) {
			size_t up = row[j];

			row[j] = !strcmp(x[i - 1], y[j - 1])
			        ? diag + 1
			        : (up > row[j - 1] ? up : row[j - 1]);
			diag = up;
		}
	}
	best = row[yn];
	free(row);
	return best;
}

static void the_line_diff_finds_the_changes(void)
{
	WfLineDiff d;

	CHECK(diff_text("a\nb\nc\n", "a\nb\nc\n", &d) && 0 == d.n);
	wf_linediff_free(&d);
	CHECK(diff_text("", "", &d) && 0 == d.n && 0 == d.a_n && 0 == d.b_n);
	wf_linediff_free(&d);
	/* one line changed */
	CHECK(diff_text("a\nb\nc\n", "a\nX\nc\n", &d) && 1 == d.n);
	CHECK(d.h[0].a == 1 && d.h[0].an == 1 && d.h[0].b == 1 &&
	        d.h[0].bn == 1);
	wf_linediff_free(&d);
	/* an insertion and a deletion */
	CHECK(diff_text("a\nc\n", "a\nb\nc\n", &d) && 1 == d.n);
	CHECK(d.h[0].a == 1 && d.h[0].an == 0 && d.h[0].b == 1 &&
	        d.h[0].bn == 1);
	wf_linediff_free(&d);
	CHECK(diff_text("a\nb\nc\n", "a\nc\n", &d) && 1 == d.n);
	CHECK(d.h[0].a == 1 && d.h[0].an == 1 && d.h[0].b == 1 &&
	        d.h[0].bn == 0);
	wf_linediff_free(&d);
	/* all new, all gone */
	CHECK(diff_text("", "x\ny\n", &d) && 1 == d.n && 0 == d.h[0].an &&
	        2 == d.h[0].bn);
	wf_linediff_free(&d);
	CHECK(diff_text("x\ny\n", "", &d) && 1 == d.n && 2 == d.h[0].an &&
	        0 == d.h[0].bn);
	wf_linediff_free(&d);
	/* two separate changes */
	CHECK(diff_text("1\n2\n3\n4\n5\n6\n", "1\nB\n3\n4\nE\n6\n", &d) &&
	        2 == d.n);
	wf_linediff_free(&d);
	/* CRLF against LF is no change; no final line break is no change */
	CHECK(diff_text("a\r\nb\r\n", "a\nb\n", &d) && 0 == d.n);
	wf_linediff_free(&d);
	CHECK(diff_text("a\nb", "a\nb\n", &d) && 0 == d.n);
	wf_linediff_free(&d);
	CHECK(diff_text("a\r\nb\r\n", "a\nX\n", &d) && 1 == d.n &&
	        1 == d.h[0].a);
	wf_linediff_free(&d);
	/* a line that is only a CR is a line */
	CHECK(diff_text("a\n\r\nb\n", "a\n\nb\n", &d) && 0 == d.n);
	wf_linediff_free(&d);
	/* Japanese and long lines */
	CHECK(diff_text("\xE6\x97\xA5\xE6\x9C\xAC\n\xE8\xAA\x9E\n",
	              "\xE6\x97\xA5\xE6\x9C\xAC\n\xE8\xAA\x9E!\n", &d) &&
	        1 == d.n);
	wf_linediff_free(&d);
	{
		char *big = malloc(300000);
		char *big2 = malloc(300000);

		memset(big, 'x', 299990);
		big[299990] = '\n';
		big[299991] = '\0';
		memcpy(big2, big, 300000);
		big2[1000] = 'y';
		CHECK(diff_text(big, big2, &d) && 1 == d.n);
		wf_linediff_free(&d);
		free(big);
		free(big2);
	}
}

static void the_line_diff_is_minimal_and_rebuilds_the_text(void)
{
	static const char alphabet[] = "abcd";
	uint32_t seed = 12345;

	for (int round = 0; round < 400; round++) {
		char a[80], b[80];
		const char *ax[40], *bx[40];
		char acell[40][2], bcell[40][2];
		size_t na, nb, k = 0;
		WfLineDiff d;
		size_t edits = 0;

		seed = seed * 1664525u + 1013904223u;
		na = (seed >> 8) % 20;
		seed = seed * 1664525u + 1013904223u;
		nb = (seed >> 8) % 20;
		for (size_t i = 0; i < na; i++) {
			seed = seed * 1664525u + 1013904223u;
			acell[i][0] = alphabet[(seed >> 16) % 4];
			acell[i][1] = '\0';
			ax[i] = acell[i];
			a[k++] = acell[i][0];
			a[k++] = '\n';
		}
		a[k] = '\0';
		k = 0;
		for (size_t i = 0; i < nb; i++) {
			seed = seed * 1664525u + 1013904223u;
			bcell[i][0] = alphabet[(seed >> 16) % 4];
			bcell[i][1] = '\0';
			bx[i] = bcell[i];
			b[k++] = bcell[i][0];
			b[k++] = '\n';
		}
		b[k] = '\0';
		CHECK(diff_text(a, b, &d));
		CHECK(na == d.a_n && nb == d.b_n && na == lines_of(a) &&
		        nb == lines_of(b));
		CHECK(script_rebuilds(&d));
		for (size_t h = 0; h < d.n; h++) {
			edits += d.h[h].an + d.h[h].bn;
		}
		/* the shortest script: all lines but the longest common subsequence */
		CHECK(edits == na + nb - 2 * lcs_len(ax, na, bx, nb));
		wf_linediff_free(&d);
	}
}

/* `n` lines "<tag><i>\n". */
static char *many_lines(const char *tag, int n, size_t *len)
{
	char *t = malloc((size_t)n * 16 + 1);
	size_t k = 0;

	for (int i = 0; i < n; i++) {
		k += (size_t)sprintf_s(
		        t + k, (size_t)n * 16 + 1 - k, "%s%d\n", tag, i);
	}
	*len = k;
	return t;
}

/* The bound on changed lines holds the same whichever way the changes are found:
 * all new, all gone, or somewhere in the middle. */
static void the_bound_on_changed_lines_holds_for_every_kind_of_change(void)
{
	static const int edge[] = {WF_LD_EDITS_MAX, WF_LD_EDITS_MAX + 1};
	WfLineDiff d;

	for (size_t i = 0; i < 2; i++) {
		size_t n;
		char *t = many_lines("x", edge[i], &n);
		WfStatus want = 0 == i ? WF_OK : WF_E_RANGE;

		/* all new, and all gone, from and to nothing */
		CHECK(want == wf_linediff("", 0, t, n, &d));
		CHECK(WF_OK != want ? !d.h && 0 == d.n
		                    : 1 == d.n && (size_t)edge[i] == d.h[0].bn);
		wf_linediff_free(&d);
		CHECK(want == wf_linediff(t, n, "", 0, &d));
		wf_linediff_free(&d);
		/* all new between lines both texts have */
		{
			char *a = malloc(n + 16), *b = malloc(n + 16);
			size_t ka =
			        (size_t)sprintf_s(a, n + 16, "top\nbottom\n");
			size_t kb = (size_t)sprintf_s(b, n + 16, "top\n");

			memcpy(b + kb, t, n);
			memcpy(b + kb + n, "bottom\n", 8);
			CHECK(want == wf_linediff(a, ka, b, kb + n + 7, &d));
			wf_linediff_free(&d);
			free(a);
			free(b);
		}
		free(t);
	}
	/* some gone and some new: the two add up */
	{
		size_t na, nb;
		char *a = many_lines("a", 500, &na),
		     *b = many_lines("b", 500, &nb);

		CHECK(WF_OK == wf_linediff(a, na, b, nb, &d)); /* 500 + 500 */
		wf_linediff_free(&d);
		free(a);
		free(b);
		a = many_lines("a", 500, &na);
		b = many_lines("b", 501, &nb);
		CHECK(WF_E_RANGE ==
		        wf_linediff(a, na, b, nb, &d)); /* 500 + 501 */
		CHECK(!d.h && 0 == d.n);
		free(a);
		free(b);
	}
}

static void a_diff_with_too_much_to_do_says_so(void)
{
	size_t n = 3000;
	char *a = malloc(n * 8), *b = malloc(n * 8);
	size_t ka = 0, kb = 0;
	WfLineDiff d;

	for (size_t i = 0; i < n; i++) {
		ka += (size_t)sprintf_s(a + ka, n * 8 - ka, "a%zu\n", i);
		kb += (size_t)sprintf_s(b + kb, n * 8 - kb, "b%zu\n", i);
	}
	CHECK(WF_E_RANGE == wf_linediff(a, ka, b, kb, &d));
	CHECK(!d.h && 0 == d.n && !d.a_text);
	free(a);
	free(b);
	/* too many lines to compare at all */
	{
		size_t big = (size_t)WF_LD_LINES_MAX + 5;
		char *t = malloc(big * 2 + 1);

		memset(t, '\n', big * 2);
		t[big * 2] = '\0';
		CHECK(WF_E_RANGE == wf_linediff(t, big * 2, "x\n", 2, &d));
		free(t);
	}
	/* a long file with a few changes is quick */
	{
		size_t lines = 100000;
		char *x = malloc(lines * 8), *y;
		size_t k = 0;
		ULONGLONG t0;

		for (size_t i = 0; i < lines; i++) {
			k += (size_t)sprintf_s(
			        x + k, lines * 8 - k, "l%zu\n", i);
		}
		y = malloc(k + 16);
		memcpy(y, x, k + 1);
		y[10] = 'Z';
		y[k - 3] = 'Z';
		t0 = GetTickCount64();
		CHECK(WF_OK == wf_linediff(x, k, y, k, &d) && 2 == d.n);
		printf("info: 100000 lines, 2 changes: %llu ms\n",
		        (unsigned long long)(GetTickCount64() - t0));
		wf_linediff_free(&d);
		free(x);
		free(y);
	}
}

typedef struct
{
	int calls;
	bool saw_windows_exe, saw_wsl_exe;
	wchar_t last_exe[MAX_PATH];
	wchar_t arg[40][300];
	size_t nargs;
	wchar_t cwd[300];
	bool has_cwd;
	bool env_prompt_off;
	bool wsl_git_exists; /* what `wsl.exe -d D --exec git --version` says */
	const char *distro_with_git;
} Fake;

static Fake fake;
static const wchar_t win_git[] = L"C:\\fake\\git.exe";
static uint64_t fake_clock;

static WfStatus fake_run(void *ctx, const WfGitRun *run, WfGitRunResult *res)
{
	size_t n = 0;
	bool is_wsl;

	(void)ctx;
	memset(res, 0, sizeof *res);
	fake.calls++;
	wcscpy_s(fake.last_exe, MAX_PATH, run->exe);
	is_wsl = wcslen(run->exe) > 8 &&
	        !_wcsicmp(run->exe + wcslen(run->exe) - 7, L"wsl.exe");
	if (!wcscmp(run->exe, win_git)) {
		fake.saw_windows_exe = true;
	}
	if (is_wsl) {
		fake.saw_wsl_exe = true;
	}
	fake.has_cwd = NULL != run->cwd;
	if (run->cwd) {
		wcscpy_s(fake.cwd, 300, run->cwd);
	}
	fake.env_prompt_off = false;
	for (size_t i = 0; run->extra_env && run->extra_env[i]; i++) {
		fake.env_prompt_off |=
		        !wcscmp(run->extra_env[i], L"GIT_TERMINAL_PROMPT=0");
	}
	for (; run->args && run->args[n] && n < 40; n++) {
		wcscpy_s(fake.arg[n], 300, run->args[n]);
	}
	fake.nargs = n;
	if (is_wsl && n >= 5 && !wcscmp(run->args[2], L"--exec") &&
	        !wcscmp(run->args[4], L"--version")) {
		char want[64];

		WideCharToMultiByte(CP_UTF8, 0, run->args[1], -1, want,
		        sizeof want, NULL, NULL);
		if (fake.distro_with_git &&
		        !strcmp(want, fake.distro_with_git)) {
			res->out = (uint8_t *)_strdup("git version 2.43.0\n");
			res->out_n = strlen((char *)res->out);
			return WF_OK;
		}
		res->exit_code = 1;
		return WF_OK;
	}
	return WF_OK;
}

static bool fake_finder_yes(wchar_t *out, size_t cap)
{
	wcscpy_s(out, cap, win_git);
	return true;
}

static bool fake_finder_no(wchar_t *out, size_t cap)
{
	(void)out;
	(void)cap;
	return false;
}

static uint64_t fake_now(void)
{
	return fake_clock;
}

static void hooks(bool windows_git, const char *wsl_distro_with_git)
{
	WfGitHooks h = {fake_run, NULL,
	        windows_git ? fake_finder_yes : fake_finder_no, fake_now};

	memset(&fake, 0, sizeof fake);
	fake.distro_with_git = wsl_distro_with_git;
	fake_clock = 1000;
	wf_git_hooks_set(&h);
}

static void env_of(WfGitEnv *e, const wchar_t *path)
{
	CHECK(WF_GENV_OTHER != wf_gitenv_classify_text(path, e));
}

static void a_windows_repository_never_uses_wsl_git(void)
{
	WfGitEnv e;
	WfGitRunResult r;
	const wchar_t *args[] = {L"status", NULL};

	hooks(false, "Ubuntu"); /* no Windows Git; Ubuntu has one */
	env_of(&e, L"C:\\repo");
	CHECK(WF_GIT_UNAVAILABLE == wf_git_capability(&e, NULL, NULL));
	CHECK(WF_E_NOENT ==
	        wf_git_exec(&e, L"C:\\repo", args, 1000, 100, NULL, NULL, &r));
	CHECK(0 == fake.calls &&
	        !fake.saw_wsl_exe); /* wsl.exe was not even asked */
	wf_gitproc_result_free(&r);
	/* with Windows Git it is used, and only it */
	hooks(true, NULL);
	CHECK(WF_GIT_AVAILABLE == wf_git_capability(&e, NULL, NULL));
	CHECK(WF_OK ==
	        wf_git_exec(&e, L"C:\\repo", args, 1000, 100, NULL, NULL, &r));
	CHECK(1 == fake.calls && fake.saw_windows_exe && !fake.saw_wsl_exe);
	wf_gitproc_result_free(&r);
	wf_git_hooks_set(NULL);
}

static void a_wsl_repository_never_uses_windows_git(void)
{
	WfGitEnv e;
	WfGitRunResult r;
	const wchar_t *args[] = {L"status", NULL};

	hooks(true, NULL); /* Windows Git exists; Ubuntu has none */
	env_of(&e, L"\\\\wsl.localhost\\Ubuntu\\home\\u\\p");
	CHECK(WF_GIT_UNAVAILABLE == wf_git_capability(&e, NULL, NULL));
	CHECK(1 == fake.calls && fake.saw_wsl_exe && !fake.saw_windows_exe);
	CHECK(!wcscmp(fake.arg[0], L"-d") && !wcscmp(fake.arg[1], L"Ubuntu"));
	/* the answer is kept for a while: no second process */
	CHECK(WF_GIT_UNAVAILABLE == wf_git_capability(&e, NULL, NULL));
	CHECK(1 == fake.calls);
	/* and nothing else of Windows Git is used for this repository */
	fake.calls = 0;
	CHECK(WF_OK ==
	        wf_git_exec(&e, L"\\\\wsl.localhost\\Ubuntu\\home\\u\\p", args,
	                1000, 100, NULL, NULL, &r));
	CHECK(1 == fake.calls && fake.saw_wsl_exe && !fake.saw_windows_exe);
	wf_gitproc_result_free(&r);
	wf_git_hooks_set(NULL);
}

static void the_capability_of_each_environment_is_kept_apart(void)
{
	WfGitEnv ubuntu, debian, win;
	char name[16];

	hooks(true, "Ubuntu");
	env_of(&ubuntu, L"\\\\wsl.localhost\\Ubuntu\\x");
	env_of(&debian, L"\\\\wsl.localhost\\Debian\\x");
	env_of(&win, L"C:\\x");
	CHECK(WF_GIT_AVAILABLE == wf_git_capability(&ubuntu, NULL, NULL));
	CHECK(1 == fake.calls);
	CHECK(WF_GIT_AVAILABLE == wf_git_capability(&ubuntu, NULL, NULL));
	CHECK(1 == fake.calls); /* available is kept */
	CHECK(WF_GIT_UNAVAILABLE == wf_git_capability(&debian, NULL, NULL));
	CHECK(2 == fake.calls); /* another distribution is another answer */
	CHECK(WF_GIT_AVAILABLE == wf_git_capability(&win, NULL, NULL));
	CHECK(2 == fake.calls); /* Windows needs no process */
	/* unavailable is asked again after a while, not before */
	fake_clock += WF_GIT_RETRY_MS - 1;
	CHECK(WF_GIT_UNAVAILABLE == wf_git_capability(&debian, NULL, NULL));
	CHECK(2 == fake.calls);
	fake_clock += 2;
	CHECK(WF_GIT_UNAVAILABLE == wf_git_capability(&debian, NULL, NULL));
	CHECK(3 == fake.calls);
	/* Git installed in the distribution meanwhile */
	fake.distro_with_git = "Debian";
	fake_clock += WF_GIT_RETRY_MS + 1;
	CHECK(WF_GIT_AVAILABLE == wf_git_capability(&debian, NULL, NULL));
	/* the cache has room for WF_GIT_DISTROS distributions; one more still works */
	for (int i = 0; i < WF_GIT_DISTROS + 2; i++) {
		wchar_t p[80];
		WfGitEnv e;

		sprintf_s(name, sizeof name, "D%d", i);
		swprintf(p, 80, L"\\\\wsl.localhost\\D%d\\x", i);
		env_of(&e, p);
		fake.distro_with_git = name;
		CHECK(WF_GIT_AVAILABLE == wf_git_capability(&e, NULL, NULL));
	}
	/* no Windows Git: unavailable for a while, then looked for again */
	hooks(false, NULL);
	CHECK(WF_GIT_UNAVAILABLE == wf_git_capability(&win, NULL, NULL));
	{
		WfGitHooks h = {fake_run, NULL, fake_finder_yes, fake_now};

		wf_git_hooks_set(&h);
		CHECK(WF_GIT_AVAILABLE == wf_git_capability(&win, NULL, NULL));
	}
	wf_git_hooks_set(NULL);
}

static void the_commands_are_arrays_for_each_environment(void)
{
	WfGitEnv e;
	WfGitRunResult r;
	const wchar_t *args[] = {
	        L"cat-file", L"blob", L"HEAD:a b/\"q\".c", NULL};

	/* Windows: git.exe, the folder as the working folder, the prompt off */
	hooks(true, NULL);
	env_of(&e, L"C:\\my repo\\sub");
	CHECK(WF_OK ==
	        wf_git_exec(&e, L"C:\\my repo\\sub", args, 1000, 100, NULL,
	                NULL, &r));
	CHECK(!wcscmp(fake.last_exe, win_git) && fake.has_cwd &&
	        !wcscmp(fake.cwd, L"C:\\my repo\\sub"));
	CHECK(fake.env_prompt_off);
	CHECK(!wcscmp(fake.arg[0], L"--no-pager") &&
	        !wcscmp(fake.arg[1], L"--no-optional-locks"));
	CHECK(!wcscmp(fake.arg[2], L"-c") &&
	        !wcscmp(fake.arg[3], L"core.fsmonitor=false"));
	CHECK(!wcscmp(fake.arg[fake.nargs - 1], L"HEAD:a b/\"q\".c"));
	wf_gitproc_result_free(&r);
	/* WSL: wsl.exe, the distribution, the Linux folder, no shell (--exec) */
	hooks(false, "kali-linux");
	env_of(&e, L"\\\\wsl.localhost\\kali-linux\\home\\u\\a b&c;d");
	CHECK(WF_OK ==
	        wf_git_exec(&e,
	                L"\\\\wsl.localhost\\kali-linux\\home\\u\\a b&c;d",
	                args, 1000, 100, NULL, NULL, &r));
	CHECK(!fake.has_cwd); /* piero's own folder: the repository is --cd */
	CHECK(!wcscmp(fake.arg[0], L"-d") &&
	        !wcscmp(fake.arg[1], L"kali-linux"));
	CHECK(!wcscmp(fake.arg[2], L"--cd") &&
	        !wcscmp(fake.arg[3], L"/home/u/a b&c;d"));
	CHECK(!wcscmp(fake.arg[4], L"--exec") && !wcscmp(fake.arg[5], L"git"));
	CHECK(!wcscmp(fake.arg[6], L"--no-pager"));
	CHECK(!wcscmp(fake.arg[fake.nargs - 1], L"HEAD:a b/\"q\".c"));
	wf_gitproc_result_free(&r);
	/* a path of another kind, or a WSL path that is not valid, runs nothing */
	fake.calls = 0;
	env_of(&e, L"\\\\wsl.localhost\\kali-linux\\home\\u");
	CHECK(WF_E_INVAL ==
	        wf_git_exec(&e, L"\\\\wsl.localhost\\kali-linux\\a\\..\\b",
	                args, 1000, 100, NULL, NULL, &r));
	{
		WfGitEnv other;

		memset(&other, 0, sizeof other);
		CHECK(WF_E_INVAL ==
		        wf_git_exec(&other, L"X:\\y", args, 1000, 100, NULL,
		                NULL, &r));
		/* a forged distribution name is not an argument either */
		memset(&other, 0, sizeof other);
		other.kind = WF_GENV_WSL;
		strcpy_s(other.distro, sizeof other.distro, "-e");
		CHECK(WF_E_INVAL ==
		        wf_git_exec(&other, L"\\\\wsl.localhost\\x\\y", args,
		                1000, 100, NULL, NULL, &r));
		strcpy_s(other.distro, sizeof other.distro, "a b");
		CHECK(WF_E_INVAL ==
		        wf_git_exec(&other, L"\\\\wsl.localhost\\x\\y", args,
		                1000, 100, NULL, NULL, &r));
		CHECK(WF_GIT_UNAVAILABLE ==
		        wf_git_capability(&other, NULL, NULL));
	}
	CHECK(0 == fake.calls);
	wf_git_hooks_set(NULL);
}

static wchar_t self_exe[MAX_PATH];

static int helper_main(int argc, wchar_t **argv)
{
	const wchar_t *mode = argv[1];

	if (!wcscmp(mode, L"--echo")) {
		for (int i = 2; i < argc; i++) {
			char u[2048];
			int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, u,
			        sizeof u, NULL, NULL);

			printf("[%s]\n", n > 0 ? u : "?");
		}
		return 0;
	}
	if (!wcscmp(mode, L"--exit")) {
		return _wtoi(argv[2]);
	}
	if (!wcscmp(mode,
	            L"--sleep")) { /* writes its pid to a file, then sleeps */
		FILE *f = NULL;

		_wfopen_s(&f, argv[2], L"w");
		if (f) {
			fprintf(f, "%lu", GetCurrentProcessId());
			fclose(f);
		}
		Sleep(60000);
		return 0;
	}
	if (!wcscmp(mode,
	            L"--spew")) { /* N bytes on stdout, M on stderr, at once */
		size_t out = (size_t)_wtoi(argv[2]),
		       err = (size_t)_wtoi(argv[3]);
		char block[4096];

		memset(block, 'x', sizeof block);
		while (out || err) {
			size_t o = out < sizeof block ? out : sizeof block;
			size_t e = err < sizeof block ? err : sizeof block;

			fwrite(block, 1, o, stdout);
			fwrite(block, 1, e, stderr);
			out -= o;
			err -= e;
		}
		fflush(stdout);
		fflush(stderr);
		return 0;
	}
	if (!wcscmp(mode, L"--forever")) { /* writes as fast as it can, to out or to err, until ended */
		bool to_err = argc > 2 && !wcscmp(argv[2], L"err");
		char block[4096];

		memset(block, 'y', sizeof block);
		for (;;) {
			fwrite(block, 1, sizeof block, to_err ? stderr : stdout);
			fflush(to_err ? stderr : stdout);
		}
	}
	if (!wcscmp(mode, L"--stdin")) { /* how many bytes can be read: none */
		char c;
		size_t n = fread(&c, 1, 1, stdin);

		printf("%zu", n);
		return 0;
	}
	if (!wcscmp(mode, L"--env")) {
		wchar_t v[512];
		DWORD n = GetEnvironmentVariableW(argv[2], v, 512);

		if (n && n < 512) {
			char u[1024];

			WideCharToMultiByte(
			        CP_UTF8, 0, v, -1, u, sizeof u, NULL, NULL);
			printf("set:%s", u);
		} else {
			printf("unset");
		}
		return 0;
	}
	if (!wcscmp(mode, L"--cwd")) {
		wchar_t d[MAX_PATH];

		GetCurrentDirectoryW(MAX_PATH, d);
		printf("%ls", d);
		return 0;
	}
	if (!wcscmp(mode,
	            L"--rawline")) { /* the command line as it was given */
		char u[2048];

		WideCharToMultiByte(CP_UTF8, 0, GetCommandLineW(), -1, u,
		        sizeof u, NULL, NULL);
		printf("%s", u);
		return 0;
	}
	return 2;
}

static WfStatus run_self(const wchar_t *const *args,
        const wchar_t *cwd,
        uint32_t timeout_ms,
        size_t max_out,
        bool (*cancelled)(void *),
        void *cctx,
        const wchar_t *const *env,
        WfGitRunResult *r)
{
	WfGitRun run;

	memset(&run, 0, sizeof run);
	run.exe = self_exe;
	run.args = args;
	run.cwd = cwd;
	run.extra_env = env;
	run.timeout_ms = timeout_ms;
	run.max_out = max_out;
	run.cancelled = cancelled;
	run.cancel_ctx = cctx;
	return wf_gitproc_run(&run, r);
}

static void arguments_arrive_exactly_as_given(void)
{
	static const wchar_t *const tricky[] = {L"a b", L"\"quoted\"", L"&",
	        L"^", L";", L"%PATH%", L"$(rm -rf x)", L"C:\\dir with space\\",
	        L"C:\\dir\\", L"\x65E5\x672C\x8A9E", L"a\\\"b", L"--option=x y",
	        L"-c", L"tab\there", L"two  spaces",
	        L"ends with backslash\\\\"};
	const wchar_t *args[2 + sizeof tricky / sizeof *tricky + 1];
	WfGitRunResult r;
	char expect[8192] = "";
	size_t k = 0;

	args[k++] = L"--echo";
	for (size_t i = 0; i < sizeof tricky / sizeof *tricky; i++) {
		args[k++] = tricky[i];
		{
			char u[512];

			WideCharToMultiByte(CP_UTF8, 0, tricky[i], -1, u,
			        sizeof u, NULL, NULL);
			strcat_s(expect, sizeof expect, "[");
			strcat_s(expect, sizeof expect, u);
			strcat_s(expect, sizeof expect, "]\r\n");
		}
	}
	args[k] = NULL;
	CHECK(WF_OK == run_self(args, NULL, 20000, 8192, NULL, NULL, NULL, &r));
	CHECK(0 == r.exit_code && r.out);
	/* stdout of the helper is text-mode: LF written as CRLF */
	CHECK(r.out && !strcmp((const char *)r.out, expect));
	if (r.out && strcmp((const char *)r.out, expect)) {
		printf("got:\n%s\nwant:\n%s\n", (const char *)r.out, expect);
	}
	wf_gitproc_result_free(&r);
}

/*
 * wsl.exe reads its own options from the raw command line and does not know
 * "-d" or "--exec" in quotes (it takes them for the command to run, and a
 * quoted distribution name for a name with quotes in it). An argument that
 * needs no quoting is therefore not quoted; one that does is, as before.
 */
static void a_plain_argument_is_not_quoted(void)
{
	const wchar_t *args[] = {L"--rawline", L"-d", L"Ubuntu", L"--cd",
	        L"/home/u/a b", L"--exec", L"git", L"--format=%s", L"", NULL};
	WfGitRunResult r;
	const char *tail = " --rawline -d Ubuntu --cd \"/home/u/a b\" --exec "
	                   "git --format=%s \"\"";
	size_t n = strlen(tail);

	CHECK(WF_OK == run_self(args, NULL, 20000, 8192, NULL, NULL, NULL, &r));
	CHECK(0 == r.exit_code && r.out && r.out_n >= n);
	CHECK(r.out && r.out_n >= n &&
	        !strcmp((const char *)r.out + r.out_n - n, tail));
	if (r.out) {
		printf("raw: %s\n", (const char *)r.out);
	}
	wf_gitproc_result_free(&r);
}

static void exit_codes_input_environment_and_folder_are_the_childs_own(void)
{
	WfGitRunResult r;
	wchar_t tmp[MAX_PATH];
	const wchar_t *seven[] = {L"--exit", L"7", NULL};
	const wchar_t *stdin_args[] = {L"--stdin", NULL};
	const wchar_t *env_gitdir[] = {L"--env", L"GIT_DIR", NULL};
	const wchar_t *env_extra[] = {L"--env", L"PIERO_EXTRA", NULL};
	const wchar_t *env_prompt[] = {L"--env", L"GIT_TERMINAL_PROMPT", NULL};
	const wchar_t *cwd_args[] = {L"--cwd", NULL};
	static const wchar_t *const extra[] = {
	        L"PIERO_EXTRA=1", L"GIT_TERMINAL_PROMPT=0", NULL};

	CHECK(WF_OK ==
	                run_self(seven, NULL, 20000, 100, NULL, NULL, NULL,
	                        &r) &&
	        7 == r.exit_code);
	wf_gitproc_result_free(&r);
	/* the child's input is the null device: reading it ends at once */
	CHECK(WF_OK ==
	                run_self(stdin_args, NULL, 20000, 100, NULL, NULL, NULL,
	                        &r) &&
	        r.out && !strcmp((char *)r.out, "0"));
	wf_gitproc_result_free(&r);
	/* variables that point Git elsewhere are not passed on */
	SetEnvironmentVariableW(L"GIT_DIR", L"C:\\elsewhere");
	CHECK(WF_OK ==
	                run_self(env_gitdir, NULL, 20000, 100, NULL, NULL, NULL,
	                        &r) &&
	        r.out && !strcmp((char *)r.out, "unset"));
	wf_gitproc_result_free(&r);
	SetEnvironmentVariableW(L"GIT_DIR", NULL);
	CHECK(WF_OK ==
	                run_self(env_extra, NULL, 20000, 100, NULL, NULL, extra,
	                        &r) &&
	        r.out && !strcmp((char *)r.out, "set:1"));
	wf_gitproc_result_free(&r);
	SetEnvironmentVariableW(L"GIT_TERMINAL_PROMPT", L"1");
	/* what the caller sets wins over what piero's own environment has */
	CHECK(WF_OK ==
	                run_self(env_prompt, NULL, 20000, 100, NULL, NULL,
	                        extra, &r) &&
	        r.out && !strcmp((char *)r.out, "set:0"));
	wf_gitproc_result_free(&r);
	SetEnvironmentVariableW(L"GIT_TERMINAL_PROMPT", NULL);
	/* the working folder is the one asked for */
	GetTempPathW(MAX_PATH, tmp);
	{
		size_t n = wcslen(tmp);

		if (n > 3 && L'\\' == tmp[n - 1]) {
			tmp[n - 1] = L'\0';
		}
	}
	CHECK(WF_OK ==
	                run_self(cwd_args, tmp, 20000, 1000, NULL, NULL, NULL,
	                        &r) &&
	        r.out);
	if (r.out) {
		wchar_t got[MAX_PATH];

		MultiByteToWideChar(
		        CP_ACP, 0, (char *)r.out, -1, got, MAX_PATH);
		CHECK(!_wcsicmp(got, tmp));
	}
	wf_gitproc_result_free(&r);
}

static void heavy_output_on_both_pipes_does_not_stop_the_child(void)
{
	WfGitRunResult r;
	const wchar_t *args[] = {L"--spew", L"5000000", L"3000000", NULL};
	ULONGLONG t0 = GetTickCount64();

	CHECK(WF_OK ==
	        run_self(args, NULL, 30000, 8u << 20, NULL, NULL, NULL, &r));
	CHECK(0 == r.exit_code && 5000000 == r.out_n && r.out &&
	        'x' == r.out[4999999]);
	CHECK(strlen(r.err) ==
	        sizeof r.err - 1); /* only the start of the errors is kept */
	printf("info: 8 MB through two pipes in %llu ms\n",
	        (unsigned long long)(GetTickCount64() - t0));
	wf_gitproc_result_free(&r);
	/* past the limit the child is ended and the answer says so */
	CHECK(WF_E_RANGE ==
	        run_self(args, NULL, 30000, 1000000, NULL, NULL, NULL, &r));
	CHECK(!r.out && 0 == r.out_n);
	wf_gitproc_result_free(&r);
}

static bool process_gone(DWORD pid)
{
	HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
	bool gone;

	if (!h) {
		return true;
	}
	gone = WAIT_OBJECT_0 == WaitForSingleObject(h, 3000);
	CloseHandle(h);
	return gone;
}

static DWORD pid_from(const wchar_t *file)
{
	FILE *f = NULL;
	unsigned long pid = 0;

	for (int i = 0; i < 100 && !pid; i++) {
		if (0 == _wfopen_s(&f, file, L"r") && f) {
			if (1 != fscanf_s(f, "%lu", &pid)) {
				pid = 0;
			}
			fclose(f);
		}
		if (!pid) {
			Sleep(20);
		}
	}
	return (DWORD)pid;
}

static bool cancel_after(void *ctx)
{
	ULONGLONG *at = ctx;

	return GetTickCount64() >= *at;
}

/* A child that never stops writing does not keep the runner from its own clock:
 * the time limit, the cancel and the limit of the output are looked at while it
 * writes, and the answer is back in moments. */
static void a_child_that_never_stops_writing_is_still_ended_on_time(void)
{
	const wchar_t *to_err[] = {L"--forever", L"err", NULL};
	const wchar_t *to_out[] = {L"--forever", L"out", NULL};
	WfGitRunResult r;
	ULONGLONG t0, at;

	/* a time limit, with the errors flooding (they are kept only at the start) */
	t0 = GetTickCount64();
	CHECK(WF_E_TIMEOUT == run_self(to_err, NULL, 400, 1000, NULL, NULL, NULL, &r));
	CHECK(GetTickCount64() - t0 < 3000);
	wf_gitproc_result_free(&r);
	/* a cancel, the same */
	at = GetTickCount64() + 300;
	t0 = GetTickCount64();
	CHECK(WF_E_CANCELLED == run_self(to_err, NULL, 30000, 1000, cancel_after, &at, NULL, &r));
	CHECK(GetTickCount64() - t0 < 3000);
	wf_gitproc_result_free(&r);
	/* the output limit, with the output flooding: over it at once, nothing kept */
	t0 = GetTickCount64();
	CHECK(WF_E_RANGE == run_self(to_out, NULL, 30000, 1u << 20, NULL, NULL, NULL, &r));
	CHECK(GetTickCount64() - t0 < 3000);
	CHECK(!r.out && 0 == r.out_n);
	wf_gitproc_result_free(&r);
	/* a time limit with the output flooding below its limit */
	t0 = GetTickCount64();
	CHECK(WF_E_TIMEOUT == run_self(to_out, NULL, 150, (size_t)1 << 30, NULL, NULL, NULL, &r));
	CHECK(GetTickCount64() - t0 < 3000);
	wf_gitproc_result_free(&r);
}

static void a_slow_child_is_ended_by_the_time_limit_and_by_cancel(void)
{
	wchar_t pidfile[MAX_PATH];
	const wchar_t *args[] = {L"--sleep", pidfile, NULL};
	WfGitRunResult r;
	ULONGLONG t0, at;
	DWORD pid;

	GetTempPathW(MAX_PATH, pidfile);
	wcscat_s(pidfile, MAX_PATH, L"piero-git-test-pid.txt");
	DeleteFileW(pidfile);
	t0 = GetTickCount64();
	CHECK(WF_E_TIMEOUT ==
	        run_self(args, NULL, 400, 100, NULL, NULL, NULL, &r));
	CHECK(GetTickCount64() - t0 < 5000);
	pid = pid_from(pidfile);
	CHECK(pid && process_gone(pid)); /* nothing stays behind */
	wf_gitproc_result_free(&r);
	DeleteFileW(pidfile);
	at = GetTickCount64() + 300;
	t0 = GetTickCount64();
	CHECK(WF_E_CANCELLED ==
	        run_self(args, NULL, 30000, 100, cancel_after, &at, NULL, &r));
	CHECK(GetTickCount64() - t0 < 5000);
	pid = pid_from(pidfile);
	CHECK(pid && process_gone(pid));
	wf_gitproc_result_free(&r);
	DeleteFileW(pidfile);
}

static void a_program_that_is_not_there_is_not_found(void)
{
	WfGitRunResult r;
	WfGitRun run;

	memset(&run, 0, sizeof run);
	run.exe = L"C:\\no\\such\\dir\\git.exe";
	run.timeout_ms = 1000;
	CHECK(WF_E_NOENT == wf_gitproc_run(&run, &r));
	CHECK(!r.out && 0 == r.out_n);
	CHECK(WF_E_INVAL == wf_gitproc_run(NULL, &r));
}

/* A hundred runs leave neither handles nor processes. */
static void many_runs_leave_no_handle_behind(void)
{
	const wchar_t *args[] = {L"--exit", L"0", NULL};
	DWORD before = 0, after = 0;
	WfGitRunResult r;

	GetProcessHandleCount(GetCurrentProcess(), &before);
	for (int i = 0; i < 100; i++) {
		CHECK(WF_OK ==
		        run_self(args, NULL, 20000, 100, NULL, NULL, NULL, &r));
		wf_gitproc_result_free(&r);
	}
	GetProcessHandleCount(GetCurrentProcess(), &after);
	printf("info: handles %lu before, %lu after 100 runs\n", before, after);
	CHECK(after <= before + 2);
}

int wmain(int argc, wchar_t **argv)
{
	GetModuleFileNameW(NULL, self_exe, MAX_PATH);
	if (argc > 1 && L'-' == argv[1][0] && L'-' == argv[1][1]) {
		return helper_main(argc, argv);
	}
	paths_are_classified_by_the_environment_that_owns_them();
	a_wsl_path_becomes_a_linux_path_without_a_shell();
	the_status_of_porcelain_v2_is_read_whole();
	a_path_inside_an_untracked_folder_is_covered_by_it();
	a_folder_holds_a_change_when_a_record_lies_below_it();
	letters_follow_the_rule_of_the_list();
	a_status_that_does_not_fit_is_refused_whole();
	the_table_follows_the_records_and_is_bounded();
	a_big_status_is_found_by_search();
	the_line_diff_finds_the_changes();
	the_line_diff_is_minimal_and_rebuilds_the_text();
	the_bound_on_changed_lines_holds_for_every_kind_of_change();
	a_diff_with_too_much_to_do_says_so();
	a_windows_repository_never_uses_wsl_git();
	a_wsl_repository_never_uses_windows_git();
	the_capability_of_each_environment_is_kept_apart();
	the_commands_are_arrays_for_each_environment();
	arguments_arrive_exactly_as_given();
	a_plain_argument_is_not_quoted();
	exit_codes_input_environment_and_folder_are_the_childs_own();
	heavy_output_on_both_pipes_does_not_stop_the_child();
	a_child_that_never_stops_writing_is_still_ended_on_time();
	a_slow_child_is_ended_by_the_time_limit_and_by_cancel();
	a_program_that_is_not_there_is_not_found();
	many_runs_leave_no_handle_behind();
	return TEST_RESULT();
}

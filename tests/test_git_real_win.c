/*
 * Git as it really is (DR-DRAFT-038): the Windows Git of this machine against
 * repositories made here, in a folder of this test's own under %TEMP%, and the
 * Git of a WSL distribution if one answers. Nothing is installed: with no Git
 * (or no WSL distribution) the part says NOT RUN and, if nothing could run at
 * all, the test exits 77 (CTest: Skipped, never Passed).
 * PIERO_NO_REAL_WSL=1 skips the WSL part on purpose.
 */
#include <windows.h>

#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fs/wf_wsl_win.h"
#include "git/wf_gitbackend_win.h"
#include "git/wf_gitenv.h"
#include "git/wf_gitproc_win.h"
#include "git/wf_gitstatus.h"
#include "git/wf_gitworker.h"
#include "wf_test.h"

#define SKIP_EXIT 77

static wchar_t root[MAX_PATH]; /* the scratch folder */
static WfGitEnv win_env;

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
				if (fd.dwFileAttributes &
				        FILE_ATTRIBUTE_REPARSE_POINT) {
					RemoveDirectoryW(p);
				} else {
					remove_tree(p);
				}
			} else {
				DeleteFileW(p);
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir);
}

/* mkdir -p */
static bool make_path(const wchar_t *path)
{
	wchar_t p[MAX_PATH * 2];

	wcscpy_s(p, MAX_PATH * 2, path);
	for (wchar_t *c = p + 3; *c; c++) {
		if (L'\\' == *c) {
			*c = L'\0';
			CreateDirectoryW(p, NULL);
			*c = L'\\';
		}
	}
	return CreateDirectoryW(p, NULL) ||
	        ERROR_ALREADY_EXISTS == GetLastError();
}

static bool put(
        const wchar_t *dir, const wchar_t *name, const void *data, size_t n)
{
	wchar_t p[MAX_PATH * 2];
	HANDLE h;
	DWORD done = 0;
	bool ok;

	join(p, dir, name);
	h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = !n || (WriteFile(h, data, (DWORD)n, &done, NULL) && done == n);
	CloseHandle(h);
	return ok;
}

static bool put_text(const wchar_t *dir, const wchar_t *name, const char *text)
{
	return put(dir, name, text, strlen(text));
}

static WfStatus git(const WfGitEnv *env,
        const wchar_t *dir,
        const wchar_t *const *args,
        WfGitRunResult *r)
{
	return wf_git_exec(env, dir, args, 60000, 16u << 20, NULL, NULL, r);
}

/* Runs a command that must succeed; its output is dropped. */
static bool git_ok(const wchar_t *dir, const wchar_t *const *args)
{
	WfGitRunResult r;
	WfStatus st = git(&win_env, dir, args, &r);
	bool ok = WF_OK == st && 0 == r.exit_code;

	if (!ok) {
		printf("git failed: status %d exit %d: %s\n", (int)st,
		        r.exit_code, r.err);
	}
	wf_gitproc_result_free(&r);
	return ok;
}

#define GIT(dir, ...) git_ok(dir, (const wchar_t *const[]){__VA_ARGS__, NULL})

static bool new_repo(const wchar_t *dir)
{
	return CreateDirectoryW(dir, NULL) &&
	        GIT(dir, L"init", L"-q", L"-b", L"main") &&
	        GIT(dir, L"config", L"user.name", L"t") &&
	        GIT(dir, L"config", L"user.email", L"t@example.com") &&
	        GIT(dir, L"config", L"core.autocrlf", L"false");
}

static bool commit_all(const wchar_t *dir, const wchar_t *msg)
{
	return GIT(dir, L"add", L"-A") &&
	        GIT(dir, L"commit", L"-q", L"-m", msg);
}

/* The status of a repository, parsed. */
static bool status_of(const wchar_t *dir, WfGitStatus *s)
{
	WfGitRunResult r;
	WfStatus st;
	bool ok;

	st = git(&win_env, dir,
	        (const wchar_t *const[]){L"status", L"--porcelain=v2", L"-z",
	                L"--untracked-files=normal", NULL},
	        &r);
	ok = WF_OK == st && 0 == r.exit_code &&
	        WF_OK == wf_gitstatus_parse(r.out, r.out_n, s);
	wf_gitproc_result_free(&r);
	return ok;
}

static char letter(const WfGitStatus *s, const char *path)
{
	return wf_gitentry_letter(wf_gitstatus_find(s, path, strlen(path)));
}

static void status_reports_every_kind_of_change_and_odd_names(void)
{
	wchar_t a[MAX_PATH * 2], sub[MAX_PATH * 2], p[MAX_PATH * 2];
	WfGitStatus s;

	join(a, root, L"repo a");
	CHECK(new_repo(a));
	join(sub, a, L"sub dir");
	CreateDirectoryW(sub, NULL);
	put_text(a, L"plain.txt", "one\n");
	put_text(a, L"with space.txt", "x\n");
	put_text(a, L"\x65E5\x672C\x8A9E.txt", "jp\n");
	put_text(a, L"amp&semi;caret^'q.txt", "q\n");
	put_text(sub, L"inner.txt", "i\n");
	put_text(a, L"to_delete.txt", "d\n");
	put_text(a, L"to_rename.txt", "r\n");
	put_text(a, L"staged.txt", "s\n");
	put_text(a, L"both.txt", "b\n");
	CHECK(commit_all(a, L"first"));
	/* changes of every kind */
	put_text(a, L"plain.txt", "one\ntwo\n");         /* modified */
	put_text(a, L"with space.txt", "y\n");           /* modified */
	put_text(a, L"\x65E5\x672C\x8A9E.txt", "jp2\n"); /* modified */
	put_text(a, L"amp&semi;caret^'q.txt", "q2\n");   /* modified */
	put_text(sub, L"inner.txt", "i2\n"); /* modified in a folder */
	join(p, a, L"to_delete.txt");
	DeleteFileW(p); /* deleted */
	CHECK(GIT(
	        a, L"mv", L"to_rename.txt", L"renamed here.txt")); /* renamed */
	put_text(a, L"staged.txt", "s2\n");
	CHECK(GIT(a, L"add", L"staged.txt")); /* staged */
	put_text(a, L"both.txt", "b2\n");
	CHECK(GIT(a, L"add", L"both.txt"));
	put_text(a, L"both.txt", "b3\n"); /* staged and edited */
	put_text(a, L"added.txt", "new\n");
	CHECK(GIT(a, L"add", L"added.txt"));       /* added */
	put_text(a, L"untracked file.txt", "u\n"); /* untracked */
	{
		wchar_t nd[MAX_PATH * 2];

		join(nd, a, L"new folder");
		CreateDirectoryW(nd, NULL);
		put_text(nd, L"f.txt", "f\n"); /* an untracked folder */
	}
	CHECK(status_of(a, &s));
	CHECK('M' == letter(&s, "plain.txt"));
	CHECK('M' == letter(&s, "with space.txt"));
	CHECK('M' == letter(&s, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.txt"));
	CHECK('M' == letter(&s, "amp&semi;caret^'q.txt"));
	CHECK('M' == letter(&s, "sub dir/inner.txt"));
	CHECK('D' == letter(&s, "to_delete.txt"));
	CHECK('R' == letter(&s, "renamed here.txt"));
	{
		const WfGitEntry *e =
		        wf_gitstatus_find(&s, "renamed here.txt", 16);

		CHECK(e && e->orig && !strcmp(e->orig, "to_rename.txt"));
	}
	CHECK('M' == letter(&s, "staged.txt"));
	{
		const WfGitEntry *e = wf_gitstatus_find(&s, "staged.txt", 10);

		CHECK(e && 'M' == e->index &&
		        '.' == e->work); /* staged, told from not staged */
		e = wf_gitstatus_find(&s, "plain.txt", 9);
		CHECK(e && '.' == e->index && 'M' == e->work);
		e = wf_gitstatus_find(&s, "both.txt", 8);
		CHECK(e && 'M' == e->index && 'M' == e->work);
	}
	CHECK('A' == letter(&s, "added.txt"));
	CHECK('U' == letter(&s, "untracked file.txt"));
	CHECK('U' == letter(&s, "new folder/") &&
	        !letter(&s, "new folder/f.txt"));
	CHECK(0 == letter(&s, "to_rename.txt")); /* the old name is not a row */
	wf_gitstatus_free(&s);
	/* a clean tree has nothing */
	CHECK(GIT(a, L"add", L"-A") && GIT(a, L"commit", L"-q", L"-m", L"all"));
	CHECK(status_of(a, &s) && 0 == s.n);
	wf_gitstatus_free(&s);
}

static void a_merge_conflict_is_a_conflict(void)
{
	wchar_t c[MAX_PATH * 2];
	WfGitStatus s;
	WfGitRunResult r;

	join(c, root, L"repo conflict");
	CHECK(new_repo(c));
	put_text(c, L"f.txt", "base\n");
	CHECK(commit_all(c, L"base"));
	CHECK(GIT(c, L"checkout", L"-q", L"-b", L"other"));
	put_text(c, L"f.txt", "other\n");
	CHECK(commit_all(c, L"other"));
	CHECK(GIT(c, L"checkout", L"-q", L"main"));
	put_text(c, L"f.txt", "main\n");
	CHECK(commit_all(c, L"main"));
	/* the merge fails with a conflict: that is the state under test */
	CHECK(WF_OK ==
	        git(&win_env, c,
	                (const wchar_t *const[]){L"merge", L"other", NULL},
	                &r));
	CHECK(0 != r.exit_code);
	wf_gitproc_result_free(&r);
	CHECK(status_of(c, &s));
	CHECK('!' == letter(&s, "f.txt"));
	wf_gitstatus_free(&s);
}

static void the_nearest_dotgit_finds_roots_worktrees_nested_and_submodules(void)
{
	wchar_t a[MAX_PATH * 2], deep[MAX_PATH * 2], found[MAX_PATH * 2],
	        w[MAX_PATH * 2], nest[MAX_PATH * 2], nest_deep[MAX_PATH * 2],
	        plain[MAX_PATH * 2], sm[MAX_PATH * 2];

	join(a, root, L"repo a");
	join(deep, a, L"sub dir\\deeper");
	make_path(deep);
	CHECK(wf_git_nearest_dotgit(a, found, ARRAYSIZE(found)) &&
	        !_wcsicmp(found, a));
	CHECK(wf_git_nearest_dotgit(deep, found, ARRAYSIZE(found)) &&
	        !_wcsicmp(found, a));
	{
		wchar_t slash[MAX_PATH * 2];

		swprintf(slash, MAX_PATH * 2, L"%ls\\", deep);
		CHECK(wf_git_nearest_dotgit(slash, found, ARRAYSIZE(found)) &&
		        !_wcsicmp(found, a));
	}
	/* a folder with no .git above it: no process is needed to know */
	join(plain, root, L"not a repo\\x\\y");
	make_path(plain);
	CHECK(!wf_git_nearest_dotgit(plain, found, ARRAYSIZE(found)));
	/* a linked worktree has a .git that is a file */
	join(w, root, L"linked");
	CHECK(GIT(a, L"worktree", L"add", L"-q", L"-b", L"wt", w));
	{
		wchar_t dg[MAX_PATH * 2];
		DWORD at;

		join(dg, w, L".git");
		at = GetFileAttributesW(dg);
		CHECK(INVALID_FILE_ATTRIBUTES != at &&
		        !(at & FILE_ATTRIBUTE_DIRECTORY));
	}
	CHECK(wf_git_nearest_dotgit(w, found, ARRAYSIZE(found)) &&
	        !_wcsicmp(found, w));
	{
		WfGitRunResult r;

		CHECK(WF_OK ==
		        git(&win_env, w,
		                (const wchar_t *const[]){L"rev-parse",
		                        L"--is-inside-work-tree", NULL},
		                &r));
		CHECK(0 == r.exit_code && r.out &&
		        !strncmp((char *)r.out, "true", 4));
		wf_gitproc_result_free(&r);
	}
	/* a nested repository is its own root */
	join(nest, a, L"nested");
	CHECK(new_repo(nest));
	join(nest_deep, nest, L"x");
	CreateDirectoryW(nest_deep, NULL);
	CHECK(wf_git_nearest_dotgit(nest_deep, found, ARRAYSIZE(found)) &&
	        !_wcsicmp(found, nest));
	/* a submodule (its .git is a file too) */
	{
		wchar_t other[MAX_PATH * 2];

		join(other, root, L"sm source");
		CHECK(new_repo(other));
		put_text(other, L"s.txt", "s\n");
		CHECK(commit_all(other, L"s"));
		CHECK(GIT(a, L"-c", L"protocol.file.allow=always", L"submodule",
		        L"add", L"-q", other, L"mod"));
		join(sm, a, L"mod");
		CHECK(wf_git_nearest_dotgit(sm, found, ARRAYSIZE(found)) &&
		        !_wcsicmp(found, sm));
	}
}

static bool head_blob(const wchar_t *dir,
        const wchar_t *path_in_head,
        WfGitRunResult *r,
        size_t max_out)
{
	wchar_t spec[MAX_PATH * 2];
	const wchar_t *args[] = {L"cat-file", L"blob", spec, NULL};
	WfStatus st;

	swprintf(spec, MAX_PATH * 2, L"HEAD:%ls", path_in_head);
	st = wf_git_exec(&win_env, dir, args, 60000, max_out, NULL, NULL, r);
	return WF_OK == st && 0 == r->exit_code;
}

static void the_head_content_is_raw_and_found_by_the_right_path(void)
{
	wchar_t b[MAX_PATH * 2];
	WfGitRunResult r;
	static const unsigned char bin[] = {0, 1, 2, 0, 0xFF, '\r', '\n', 0};

	join(b, root, L"repo blobs");
	CHECK(new_repo(b));
	put_text(b, L"lf.txt", "a\nb\n");
	put_text(b, L"crlf.txt", "a\r\nb\r\n");
	put_text(b, L"\x65E5\x672C\x8A9E and space.txt", "jp\n");
	put(b, L"bin.dat", bin, sizeof bin);
	put_text(b, L"orig.txt", "original\n");
	put_text(b, L"empty.txt", "");
	{
		char *big = malloc(2u << 20);

		memset(big, 'z', 2u << 20);
		put(b, L"big.txt", big, 2u << 20);
		free(big);
	}
	CHECK(commit_all(b, L"c"));
	CHECK(head_blob(b, L"lf.txt", &r, 1000) && 4 == r.out_n &&
	        !memcmp(r.out, "a\nb\n", 4));
	wf_gitproc_result_free(&r);
	CHECK(head_blob(b, L"crlf.txt", &r, 1000) && 6 == r.out_n &&
	        !memcmp(r.out, "a\r\nb\r\n", 6));
	wf_gitproc_result_free(&r);
	CHECK(head_blob(b, L"\x65E5\x672C\x8A9E and space.txt", &r, 1000) &&
	        3 == r.out_n);
	wf_gitproc_result_free(&r);
	CHECK(head_blob(b, L"bin.dat", &r, 1000) && sizeof bin == r.out_n &&
	        !memcmp(r.out, bin, sizeof bin));
	wf_gitproc_result_free(&r);
	CHECK(head_blob(b, L"empty.txt", &r, 1000) && 0 == r.out_n);
	wf_gitproc_result_free(&r);
	/* not in HEAD: Git says so, and nothing is made up */
	CHECK(!head_blob(b, L"nothing.txt", &r, 1000));
	CHECK(r.exit_code != 0 && 0 == r.out_n);
	wf_gitproc_result_free(&r);
	/* a blob over the limit is not read */
	CHECK(WF_E_RANGE ==
	        wf_git_exec(&win_env, b,
	                (const wchar_t *const[]){
	                        L"cat-file", L"blob", L"HEAD:big.txt", NULL},
	                60000, 1u << 20, NULL, NULL, &r));
	wf_gitproc_result_free(&r);
	/* a rename: the base is the old path's blob, the new path has none */
	CHECK(GIT(b, L"mv", L"orig.txt", L"moved.txt"));
	CHECK(!head_blob(b, L"moved.txt", &r, 1000));
	wf_gitproc_result_free(&r);
	CHECK(head_blob(b, L"orig.txt", &r, 1000) && 9 == r.out_n);
	wf_gitproc_result_free(&r);
	/* an untracked file and a deleted repository: a failure to report, not a crash */
	{
		wchar_t gone[MAX_PATH * 2], head[MAX_PATH * 2];

		join(gone, root, L"repo broken");
		CHECK(new_repo(gone));
		put_text(gone, L"f.txt", "f\n");
		CHECK(commit_all(gone, L"c"));
		join(head, gone, L".git\\HEAD");
		SetFileAttributesW(head, FILE_ATTRIBUTE_NORMAL);
		DeleteFileW(head);
		CHECK(WF_OK ==
		        git(&win_env, gone,
		                (const wchar_t *const[]){L"status",
		                        L"--porcelain=v2", L"-z", NULL},
		                &r));
		CHECK(0 != r.exit_code);
		wf_gitproc_result_free(&r);
	}
	/* a folder that is no repository at all */
	{
		wchar_t plain[MAX_PATH * 2];

		join(plain, root, L"not a repo");
		CHECK(WF_OK ==
		        git(&win_env, plain,
		                (const wchar_t *const[]){L"rev-parse",
		                        L"--is-inside-work-tree", NULL},
		                &r));
		CHECK(0 != r.exit_code);
		wf_gitproc_result_free(&r);
	}
}

static bool base_from_worker(WfGitWorker *w,
        uint32_t gen,
        const wchar_t *dir,
        const char *rel,
        WfGitAnswer *a)
{
	ULONGLONG end = GetTickCount64() + 30000;

	if (WF_OK !=
	        wf_gitworker_base(w, gen, 1, &win_env, dir, rel, NULL, false)) {
		return false;
	}
	while (GetTickCount64() < end) {
		WaitForSingleObject(wf_gitworker_event(w), 50);
		if (wf_gitworker_poll(w, a)) {
			return true;
		}
	}
	return false;
}

static void the_base_of_a_new_file_is_decided_by_git_not_guessed(void)
{
	wchar_t r[MAX_PATH * 2], nd[MAX_PATH * 2], fresh[MAX_PATH * 2],
	        broken[MAX_PATH * 2], head[MAX_PATH * 2];
	WfGitWorker *w = NULL;
	WfGitAnswer a;
	WfGitRunResult run;

	join(r, root, L"repo newness");
	CHECK(new_repo(r));
	put_text(r, L"tracked.txt", "kept\n");
	CHECK(commit_all(r, L"c"));
	join(nd, r, L"newdir");
	CreateDirectoryW(nd, NULL);
	put_text(nd, L"file.c",
	        "new\n"); /* untracked, inside an untracked folder */
	CHECK(WF_OK == wf_gitworker_start(&w));
	/* what Git has: its content */
	CHECK(base_from_worker(w, 1, r, "tracked.txt", &a));
	CHECK(WF_GITR_OK == a.state && a.base && 5 == a.base->n);
	wf_gitanswer_free(&a);
	/* what it has not, in a folder that the status shows as one record: empty */
	CHECK(base_from_worker(w, 2, r, "newdir/file.c", &a));
	CHECK(WF_GITR_OK == a.state && a.base && 0 == a.base->n);
	wf_gitanswer_free(&a);
	/* the question the worker asks, as Git answers it: 1, and nothing said, for
	 * "not there" */
	CHECK(WF_OK ==
	        wf_git_exec(&win_env, r,
	                (const wchar_t *const[]){L"rev-parse", L"--verify",
	                        L"--quiet", L"HEAD:newdir/file.c", NULL},
	                60000, 256, NULL, NULL, &run));
	CHECK(1 == run.exit_code && 0 == run.err[0] && 0 == run.out_n);
	wf_gitproc_result_free(&run);
	/* a repository with no commit yet: every file is new */
	join(fresh, root, L"repo nocommit");
	CHECK(new_repo(fresh));
	put_text(fresh, L"a.txt", "a\n");
	CHECK(base_from_worker(w, 3, fresh, "a.txt", &a));
	CHECK(WF_GITR_OK == a.state && a.base && 0 == a.base->n);
	wf_gitanswer_free(&a);
	/* a repository Git cannot read is an error, not a new file */
	join(broken, root, L"repo damaged");
	CHECK(new_repo(broken));
	put_text(broken, L"f.txt", "f\n");
	CHECK(commit_all(broken, L"c"));
	join(head, broken, L".git\\HEAD");
	SetFileAttributesW(head, FILE_ATTRIBUTE_NORMAL);
	DeleteFileW(head);
	CHECK(base_from_worker(w, 4, broken, "f.txt", &a));
	CHECK(WF_GITR_FAILED == a.state && !a.base);
	wf_gitanswer_free(&a);
	CHECK(0 == wf_gitworker_stop(w).status);
}

static void git_is_looked_for_in_absolute_path_entries_only(void)
{
	wchar_t bin[MAX_PATH * 2], fake[MAX_PATH * 2], saved[32768],
	        path[MAX_PATH * 4], cmd[MAX_PATH];
	WfGitEnv w;

	GetEnvironmentVariableW(L"PATH", saved, ARRAYSIZE(saved));
	join(bin, root, L"bin");
	CreateDirectoryW(bin, NULL);
	join(fake, bin, L"git.exe");
	GetSystemDirectoryW(cmd, MAX_PATH);
	wcscat_s(cmd, MAX_PATH, L"\\ping.exe");
	CopyFileW(cmd, fake,
	        FALSE); /* any program: the search only looks for the name */
	wf_gitenv_classify_text(L"C:\\x", &w);
	wf_git_hooks_set(NULL);
	/* no PATH, no Git */
	SetEnvironmentVariableW(L"PATH", L"C:\\no\\such\\folder");
	wf_git_cache_reset();
	CHECK(WF_GIT_UNAVAILABLE == wf_git_capability(&w, NULL, NULL));
	/* an empty or relative entry means "here": never used, whatever is there */
	SetCurrentDirectoryW(bin);
	SetEnvironmentVariableW(L"PATH", L";.;bin;..\\bin;\"\"");
	wf_git_cache_reset();
	CHECK(WF_GIT_UNAVAILABLE == wf_git_capability(&w, NULL, NULL));
	/* the same folder written as an absolute path is found, quoted or with a
	 * trailing backslash too */
	swprintf(path, ARRAYSIZE(path), L"C:\\no\\such;\"%ls\\\"", bin);
	SetEnvironmentVariableW(L"PATH", path);
	wf_git_cache_reset();
	CHECK(WF_GIT_AVAILABLE == wf_git_capability(&w, NULL, NULL));
	/* a network entry is not used */
	SetEnvironmentVariableW(L"PATH", L"\\\\server\\share\\bin");
	wf_git_cache_reset();
	CHECK(WF_GIT_UNAVAILABLE == wf_git_capability(&w, NULL, NULL));
	SetCurrentDirectoryW(root);
	SetEnvironmentVariableW(L"PATH", saved);
	wf_git_cache_reset();
}

static bool wsl_distribution(char *name, size_t cap)
{
	WfWslDistro d[16];
	size_t n = wf_wsl_distros(d, 16);

	for (size_t i = 0; i < n; i++) {
		wchar_t unc[256];
		DWORD at;

		if (!wcscmp(d[i].name, L"docker-desktop") ||
		        !wcscmp(d[i].name, L"docker-desktop-data")) {
			continue;
		}
		swprintf(unc, 256, L"\\\\wsl.localhost\\%ls\\tmp", d[i].name);
		at = GetFileAttributesW(unc);
		if (INVALID_FILE_ATTRIBUTES != at &&
		        WideCharToMultiByte(CP_UTF8, 0, d[i].name, -1, name,
		                (int)cap, NULL, NULL)) {
			return true;
		}
	}
	return false;
}

static bool wsl_part_ran;

/* Runs `sh -c <script>` in the distribution as the ordinary user. The script
 * must not hold a double quote; the repository is reached by a glob because
 * its name holds characters cmd.exe would read. */
static bool wsl_shell(const char *distro, const char *script)
{
	char cmd[1024];
	FILE *f;

	snprintf(cmd, sizeof cmd, "wsl.exe -d %s -- sh -c \"%s\" 2>nul", distro,
	        script);
	f = _popen(cmd, "r");
	return f && 0 == _pclose(f);
}

/* What Linux has and Windows has not: the executable bit and a symbolic link
 * are changes Git reports from the distribution's own Git. */
static void wsl_mode_and_link_changes(
        const char *distro, const wchar_t *dir, const WfGitEnv *e)
{
	char script[512];
	WfGitStatus s;
	WfGitRunResult r;

	put_text(dir, L"run.sh", "#!/bin/sh\n");
	snprintf(script, sizeof script,
	        "cd /tmp/piero-git-test-%lu-* && git add run.sh && git -c "
	        "user.name=t -c user.email=t@example.com commit -q -m r && "
	        "chmod 755 run.sh && ln -s run.sh link",
	        GetCurrentProcessId());
	if (!wsl_shell(distro, script)) {
		printf("NOT RUN (WSL): wsl.exe cannot run commands (mode and "
		       "link changes)\n");
		return;
	}
	CHECK(WF_OK ==
	        git(e, dir,
	                (const wchar_t *const[]){L"status", L"--porcelain=v2",
	                        L"-z", L"--untracked-files=normal", NULL},
	                &r));
	CHECK(0 == r.exit_code &&
	        WF_OK == wf_gitstatus_parse(r.out, r.out_n, &s));
	CHECK('M' == letter(&s, "run.sh")); /* only the mode changed */
	CHECK('U' == letter(&s, "link"));   /* a new link is untracked */
	wf_gitstatus_free(&s);
	wf_gitproc_result_free(&r);
}

static void wsl_git_works_in_its_own_distribution(void)
{
	char distro[WF_WSL_NAME_MAX];
	char env_skip[8], script[128];
	wchar_t d[WF_WSL_NAME_MAX], dir[512];
	WfGitEnv e;
	WfGitStatus s;
	WfGitRunResult r;

	if (GetEnvironmentVariableA(
	            "PIERO_NO_REAL_WSL", env_skip, sizeof env_skip)) {
		printf("NOT RUN (WSL): PIERO_NO_REAL_WSL is set\n");
		return;
	}
	if (!wsl_distribution(distro, sizeof distro)) {
		printf("NOT RUN (WSL): no running distribution answered\n");
		return;
	}
	MultiByteToWideChar(CP_UTF8, 0, distro, -1, d, WF_WSL_NAME_MAX);
	/* The folder name has a space, Japanese and the characters a shell or
	 * wsl.exe could misread: it reaches wsl.exe as the --cd value. */
	swprintf(dir, 512,
	        L"\\\\wsl.localhost\\%ls\\tmp\\piero-git-test-%lu-a "
	        L"b\x65E5\x672C'q$x&y",
	        d, GetCurrentProcessId());
	CHECK(WF_GENV_WSL == wf_gitenv_classify_text(dir, &e));
	wf_git_hooks_set(NULL);
	if (WF_GIT_AVAILABLE != wf_git_capability(&e, NULL, NULL)) {
		printf("NOT RUN (WSL): Git is not available in %s\n", distro);
		return;
	}
	wsl_part_ran = true;
	CHECK(CreateDirectoryW(dir, NULL));
	CHECK(WF_OK ==
	                git(&e, dir,
	                        (const wchar_t *const[]){L"init", L"-q", NULL},
	                        &r) &&
	        0 == r.exit_code);
	wf_gitproc_result_free(&r);
	CHECK(WF_OK ==
	        git(&e, dir,
	                (const wchar_t *const[]){L"config", L"user.email",
	                        L"t@example.com", NULL},
	                &r));
	wf_gitproc_result_free(&r);
	put_text(dir, L"a b.txt", "x\n");
	put_text(dir, L"\x65E5\x672C.txt", "x\n");
	CHECK(WF_OK ==
	                git(&e, dir,
	                        (const wchar_t *const[]){L"add", L"-A", NULL},
	                        &r) &&
	        0 == r.exit_code);
	wf_gitproc_result_free(&r);
	CHECK(WF_OK ==
	                git(&e, dir,
	                        (const wchar_t *const[]){L"-c", L"user.name=t",
	                                L"commit", L"-q", L"-m", L"c", NULL},
	                        &r) &&
	        0 == r.exit_code);
	wf_gitproc_result_free(&r);
	put_text(dir, L"a b.txt", "y\n");
	put_text(dir, L"new.txt", "n\n");
	CHECK(WF_OK ==
	        git(&e, dir,
	                (const wchar_t *const[]){L"status", L"--porcelain=v2",
	                        L"-z", L"--untracked-files=normal", NULL},
	                &r));
	CHECK(0 == r.exit_code &&
	        WF_OK == wf_gitstatus_parse(r.out, r.out_n, &s));
	CHECK('M' == letter(&s, "a b.txt") && 'U' == letter(&s, "new.txt"));
	wf_gitstatus_free(&s);
	wf_gitproc_result_free(&r);
	wsl_mode_and_link_changes(distro, dir, &e);
	remove_tree(dir);
	/* A link made on the Linux side is not removed from Windows. */
	snprintf(script, sizeof script, "rm -rf /tmp/piero-git-test-%lu-*",
	        GetCurrentProcessId());
	wsl_shell(distro, script);
}

static size_t children_named(const wchar_t *name)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32W pe;
	size_t n = 0;

	if (INVALID_HANDLE_VALUE == snap) {
		return 0;
	}
	pe.dwSize = sizeof pe;
	for (BOOL ok = Process32FirstW(snap, &pe); ok;
	        ok = Process32NextW(snap, &pe)) {
		n += pe.th32ParentProcessID == GetCurrentProcessId() &&
		        !_wcsicmp(pe.szExeFile, name);
	}
	CloseHandle(snap);
	return n;
}

/* Handles and processes: what the first runs load stays; every run after them
 * leaves nothing. */
static void many_git_runs_leave_no_handle_behind(void)
{
	wchar_t a[MAX_PATH * 2];
	DWORD before = 0, after = 0;
	WfGitStatus s;

	join(a, root, L"repo a");
	CHECK(status_of(a, &s)); /* the first run loads what it needs */
	wf_gitstatus_free(&s);
	GetProcessHandleCount(GetCurrentProcess(), &before);
	for (int i = 0; i < 60; i++) {
		CHECK(status_of(a, &s));
		wf_gitstatus_free(&s);
	}
	GetProcessHandleCount(GetCurrentProcess(), &after);
	printf("info: handles %lu before, %lu after 60 git runs\n", before,
	        after);
	CHECK(after <= before + 2);
	CHECK(0 == children_named(L"git.exe"));
}

int main(void)
{
	wchar_t tmp[MAX_PATH];
	DWORD handles_before = 0, handles_after = 0;

	if (WF_GIT_AVAILABLE !=
	        (wf_gitenv_classify_text(L"C:\\x", &win_env),
	                wf_git_capability(&win_env, NULL, NULL))) {
		printf("NOT RUN: Windows Git is not installed\n");
		return SKIP_EXIT;
	}
	GetTempPathW(MAX_PATH, tmp);
	swprintf(root, MAX_PATH, L"%lspiero-git-real-%lu", tmp,
	        GetCurrentProcessId());
	remove_tree(root);
	CHECK(CreateDirectoryW(root, NULL));
	GetProcessHandleCount(GetCurrentProcess(), &handles_before);
	status_reports_every_kind_of_change_and_odd_names();
	a_merge_conflict_is_a_conflict();
	the_nearest_dotgit_finds_roots_worktrees_nested_and_submodules();
	the_head_content_is_raw_and_found_by_the_right_path();
	the_base_of_a_new_file_is_decided_by_git_not_guessed();
	git_is_looked_for_in_absolute_path_entries_only();
	wsl_git_works_in_its_own_distribution();
	many_git_runs_leave_no_handle_behind();
	GetProcessHandleCount(GetCurrentProcess(), &handles_after);
	printf("info: handles %lu at the start, %lu at the end\n",
	        handles_before, handles_after);
	CHECK(0 == children_named(L"git.exe") &&
	        0 == children_named(L"wsl.exe"));
	SetCurrentDirectoryW(tmp);
	remove_tree(root);
	if (!wsl_part_ran) {
		printf("NOTE: the WSL part did not run\n");
	}
	return TEST_RESULT();
}

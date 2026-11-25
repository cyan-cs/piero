/*
 * The Git worker (DR-DRAFT-038) against a Git that is made up: the answers it
 * gives, that an answer which is no longer the newest never arrives, that a newer
 * request ends the process of an older one, that the editor's requests go before
 * a status, and that stopping never waits for a slow Git.
 */
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "git/wf_gitworker.h"
#include "wf_test.h"

static const wchar_t win_git[] = L"C:\\fake\\git.exe";

typedef struct
{
	volatile LONG calls;
	volatile LONG processes_ended_by_cancel;
	bool not_repo, status_fails, status_garbage, base_fails, base_huge,
	        windows_git;
	HANDLE entered; /* set when a status run starts */
	HANDLE release; /* a status run waits for it (NULL: does not wait) */
	bool honour_cancel; /* the fake polls `cancelled` like the real runner */
	wchar_t last_spec[300];
	wchar_t last_root[300];
	volatile LONG status_runs, base_runs, verify_runs;
	int verify_exit; /* what `rev-parse --verify` says: 1 is "not in HEAD", else an error */
	HANDLE base_gate; /* a cat-file run waits for it (NULL: does not) */
} Fake;

static Fake fake;

static bool has_arg(const WfGitRun *run, const wchar_t *a)
{
	for (size_t i = 0; run->args && run->args[i]; i++) {
		if (!wcscmp(run->args[i], a)) {
			return true;
		}
	}
	return false;
}

static const char *status_blob(const wchar_t *root, size_t *n)
{
	static const char one[] =
	        "1 .M N... 100644 100644 100644 a b one.txt\0? two.txt\0";
	static const char two[] =
	        "1 .M N... 100644 100644 100644 a b other.txt\0";

	if (!wcscmp(root, L"C:\\second")) {
		*n = sizeof two - 1;
		return two;
	}
	*n = sizeof one - 1;
	return one;
}

static WfStatus fake_run(void *ctx, const WfGitRun *run, WfGitRunResult *res)
{
	(void)ctx;
	memset(res, 0, sizeof *res);
	InterlockedIncrement(&fake.calls);
	if (run->cwd) {
		wcscpy_s(fake.last_root, 300, run->cwd);
	}
	if (has_arg(run, L"--verify")) { /* is the path in HEAD */
		InterlockedIncrement(&fake.verify_runs);
		res->exit_code = fake.verify_exit ? fake.verify_exit : 128;
		return WF_OK;
	}
	if (has_arg(run, L"rev-parse")) {
		if (fake.not_repo) {
			res->exit_code = 128;
			return WF_OK;
		}
		res->out = (uint8_t *)_strdup("true\n");
		res->out_n = 5;
		return WF_OK;
	}
	if (has_arg(run, L"status")) {
		InterlockedIncrement(&fake.status_runs);
		if (fake.entered) {
			SetEvent(fake.entered);
		}
		while (fake.release &&
		        WAIT_OBJECT_0 != WaitForSingleObject(fake.release, 5)) {
			if (fake.honour_cancel && run->cancelled &&
			        run->cancelled(run->cancel_ctx)) {
				InterlockedIncrement(
				        &fake.processes_ended_by_cancel);
				return WF_E_CANCELLED;
			}
		}
		if (fake.status_fails) {
			res->exit_code = 1;
			return WF_OK;
		}
		if (fake.status_garbage) {
			res->out = (uint8_t *)_strdup("this is not porcelain");
			res->out_n = strlen((char *)res->out);
			return WF_OK;
		}
		{
			size_t n;
			const char *text = status_blob(run->cwd, &n);

			res->out = malloc(n + 1);
			memcpy(res->out, text, n);
			res->out[n] = '\0';
			res->out_n = n;
		}
		return WF_OK;
	}
	if (has_arg(run, L"cat-file")) {
		InterlockedIncrement(&fake.base_runs);
		if (fake.base_gate) {
			WaitForSingleObject(fake.base_gate, 5000);
		}
		for (size_t i = 0; run->args[i]; i++) {
			if (!wcsncmp(run->args[i], L"HEAD:", 5)) {
				wcscpy_s(fake.last_spec, 300, run->args[i]);
			}
		}
		if (fake.base_huge) {
			return WF_E_RANGE;
		}
		if (fake.base_fails) {
			res->exit_code = 128;
			return WF_OK;
		}
		res->out = (uint8_t *)_strdup("line1\nline2\n");
		res->out_n = 12;
		return WF_OK;
	}
	return WF_OK;
}

static bool finder_yes(wchar_t *out, size_t cap)
{
	wcscpy_s(out, cap, win_git);
	return true;
}

static bool finder_no(wchar_t *out, size_t cap)
{
	(void)out;
	(void)cap;
	return false;
}

static void begin(bool windows_git)
{
	WfGitHooks h = {
	        fake_run, NULL, windows_git ? finder_yes : finder_no, NULL};
	HANDLE e = fake.entered, r = fake.release;

	if (e) {
		CloseHandle(e);
	}
	if (r) {
		CloseHandle(r);
	}
	memset(&fake, 0, sizeof fake);
	wf_git_hooks_set(&h);
}

static WfGitEnv win_env(void)
{
	WfGitEnv e;

	CHECK(WF_GENV_WINDOWS == wf_gitenv_classify_text(L"C:\\repo", &e));
	return e;
}

static bool wait_answer(WfGitWorker *w, WfGitAnswer *a, DWORD ms)
{
	ULONGLONG end = GetTickCount64() + ms;

	while (GetTickCount64() < end) {
		WaitForSingleObject(wf_gitworker_event(w), 20);
		if (wf_gitworker_poll(w, a)) {
			return true;
		}
	}
	return false;
}

static bool no_answer_for(WfGitWorker *w, DWORD ms)
{
	WfGitAnswer a;
	ULONGLONG end = GetTickCount64() + ms;

	while (GetTickCount64() < end) {
		if (wf_gitworker_poll(w, &a)) {
			wf_gitanswer_free(&a);
			return false;
		}
		Sleep(10);
	}
	return true;
}

static void a_repository_answers_with_its_status(void)
{
	WfGitWorker *w;
	WfGitAnswer a;
	WfGitEnv e = win_env();

	begin(true);
	CHECK(WF_OK == wf_gitworker_start(&w));
	CHECK(wf_gitworker_idle(w));
	CHECK(WF_OK == wf_gitworker_repo(w, 1, &e, L"C:\\repo", false));
	CHECK(!wf_gitworker_idle(w)); /* asked: not stopped under the request */
	/* the answer is there but not taken: still not idle (it would be lost) */
	CHECK(WAIT_OBJECT_0 ==
	        WaitForSingleObject(wf_gitworker_event(w), 5000));
	Sleep(30);
	CHECK(!wf_gitworker_idle(w));
	CHECK(wait_answer(w, &a, 5000));
	CHECK(WF_GIT_A_REPO == a.kind && 1 == a.gen && WF_GITR_OK == a.state);
	CHECK(a.root && !wcscmp(a.root, L"C:\\repo") && 2 == a.table.n);
	CHECK(wf_gitstatus_find(&a.table, "one.txt", 7) &&
	        wf_gitstatus_find(&a.table, "two.txt", 7));
	wf_gitanswer_free(&a);
	CHECK(wf_gitworker_idle(w)); /* answered and taken */
	CHECK(0 == wf_gitworker_stop(w).status);
}

static void the_answers_for_a_missing_git_a_non_repository_and_a_failure(void)
{
	WfGitWorker *w;
	WfGitAnswer a;
	WfGitEnv e = win_env();

	/* no Windows Git: nothing is run */
	begin(false);
	CHECK(WF_OK == wf_gitworker_start(&w));
	CHECK(WF_OK == wf_gitworker_repo(w, 1, &e, L"C:\\repo", false));
	CHECK(wait_answer(w, &a, 5000) && WF_GITR_UNAVAILABLE == a.state);
	CHECK(0 == fake.calls);
	wf_gitanswer_free(&a);
	CHECK(0 == wf_gitworker_stop(w).status);
	/* a folder Git does not take for a work tree */
	begin(true);
	fake.not_repo = true;
	CHECK(WF_OK == wf_gitworker_start(&w));
	CHECK(WF_OK == wf_gitworker_repo(w, 2, &e, L"C:\\repo", false));
	CHECK(wait_answer(w, &a, 5000) && WF_GITR_NOT_REPO == a.state &&
	        0 == a.table.n);
	/* every answer says which repository it is for, whatever it holds */
	CHECK(a.root && !wcscmp(a.root, L"C:\\repo"));
	CHECK(0 == fake.status_runs); /* no status for it */
	wf_gitanswer_free(&a);
	/* a status that fails, and one that is not porcelain */
	fake.not_repo = false;
	fake.status_fails = true;
	CHECK(WF_OK == wf_gitworker_repo(w, 3, &e, L"C:\\repo", false));
	CHECK(wait_answer(w, &a, 5000) && 3 == a.gen &&
	        WF_GITR_FAILED == a.state);
	wf_gitanswer_free(&a);
	fake.status_fails = false;
	fake.status_garbage = true;
	CHECK(WF_OK == wf_gitworker_repo(w, 4, &e, L"C:\\repo", false));
	CHECK(wait_answer(w, &a, 5000) && 4 == a.gen &&
	        WF_GITR_FAILED == a.state);
	CHECK(a.root && !wcscmp(a.root, L"C:\\repo") && 0 == a.table.n);
	wf_gitanswer_free(&a);
	/* requests that cannot be made */
	{
		WfGitEnv other;

		memset(&other, 0, sizeof other);
		CHECK(WF_E_INVAL ==
		        wf_gitworker_repo(w, 5, &other, L"C:\\repo", false));
		CHECK(WF_E_INVAL ==
		        wf_gitworker_repo(w, 0, &e, L"C:\\repo", false));
		CHECK(WF_E_INVAL == wf_gitworker_repo(w, 5, &e, NULL, false));
	}
	CHECK(0 == wf_gitworker_stop(w).status);
}

static void a_newer_request_ends_the_older_process_and_only_it_answers(void)
{
	WfGitWorker *w;
	WfGitAnswer a;
	WfGitEnv e = win_env();

	begin(true);
	fake.entered = CreateEventW(NULL, FALSE, FALSE, NULL);
	fake.release = CreateEventW(NULL, TRUE, FALSE, NULL);
	fake.honour_cancel = true;
	CHECK(WF_OK == wf_gitworker_start(&w));
	CHECK(WF_OK == wf_gitworker_repo(w, 1, &e, L"C:\\repo", false));
	CHECK(WAIT_OBJECT_0 ==
	        WaitForSingleObject(fake.entered,
	                5000)); /* status of the first is running */
	/* the user went to another repository */
	CHECK(WF_OK == wf_gitworker_repo(w, 2, &e, L"C:\\second", false));
	CHECK(WAIT_OBJECT_0 ==
	        WaitForSingleObject(
	                fake.entered, 5000)); /* the second status runs */
	SetEvent(fake.release);
	CHECK(wait_answer(w, &a, 5000));
	CHECK(2 == a.gen && a.root && !wcscmp(a.root, L"C:\\second") &&
	        1 == a.table.n);
	CHECK(wf_gitstatus_find(&a.table, "other.txt", 9));
	wf_gitanswer_free(&a);
	CHECK(no_answer_for(w, 300)); /* the first never arrives */
	CHECK(1 == fake.processes_ended_by_cancel);
	CHECK(0 == wf_gitworker_stop(w).status);
}

static void an_old_answer_is_dropped_even_when_its_process_was_not_ended(void)
{
	WfGitWorker *w;
	WfGitAnswer a;
	WfGitEnv e = win_env();

	begin(true);
	fake.entered = CreateEventW(NULL, FALSE, FALSE, NULL);
	fake.release = CreateEventW(NULL, TRUE, FALSE, NULL);
	fake.honour_cancel = false; /* it finishes whatever is asked */
	CHECK(WF_OK == wf_gitworker_start(&w));
	CHECK(WF_OK == wf_gitworker_repo(w, 1, &e, L"C:\\repo", false));
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(fake.entered, 5000));
	CHECK(WF_OK == wf_gitworker_repo(w, 2, &e, L"C:\\second", false));
	SetEvent(fake.release);
	CHECK(wait_answer(w, &a, 5000) && 2 == a.gen);
	wf_gitanswer_free(&a);
	CHECK(no_answer_for(w, 300));
	CHECK(0 == wf_gitworker_stop(w).status);
}

static void rapid_switching_delivers_only_the_last_repository(void)
{
	WfGitWorker *w;
	WfGitAnswer a;
	WfGitEnv e = win_env();
	int seen = 0;

	begin(true);
	CHECK(WF_OK == wf_gitworker_start(&w));
	for (uint32_t g = 1; g <= 60; g++) {
		CHECK(WF_OK ==
		        wf_gitworker_repo(w, g, &e,
		                g % 2 ? L"C:\\repo" : L"C:\\second", false));
	}
	/* whatever arrives is the wanted one; the last is the second repository */
	while (wait_answer(w, &a, 1500)) {
		seen++;
		CHECK(60 == a.gen && a.root && !wcscmp(a.root, L"C:\\second"));
		wf_gitanswer_free(&a);
	}
	CHECK(1 == seen);
	CHECK(0 == wf_gitworker_stop(w).status);
}

static void an_unpolled_answer_is_not_delivered_after_a_newer_request(void)
{
	WfGitWorker *w;
	WfGitAnswer a;
	WfGitEnv e = win_env();

	begin(true);
	fake.entered = CreateEventW(NULL, FALSE, FALSE, NULL);
	CHECK(WF_OK == wf_gitworker_start(&w));
	CHECK(WF_OK == wf_gitworker_repo(w, 1, &e, L"C:\\repo", false));
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(fake.entered, 5000));
	while (!wf_gitworker_idle(w) &&
	        WAIT_TIMEOUT ==
	                WaitForSingleObject(wf_gitworker_event(w), 10)) {
	}
	Sleep(50); /* its answer is in the slot */
	CHECK(WF_OK ==
	        wf_gitworker_repo(w, 2, &e, L"C:\\second",
	                false)); /* asked before the poll */
	CHECK(wait_answer(w, &a, 5000) &&
	        2 == a.gen); /* the first, still waiting, was dropped */
	wf_gitanswer_free(&a);
	CHECK(no_answer_for(w, 200));
	CHECK(0 == wf_gitworker_stop(w).status);
}

static void cancel_drops_what_waits_and_ends_what_runs(void)
{
	WfGitWorker *w;
	WfGitEnv e = win_env();

	begin(true);
	fake.entered = CreateEventW(NULL, FALSE, FALSE, NULL);
	fake.release = CreateEventW(NULL, TRUE, FALSE, NULL);
	fake.honour_cancel = true;
	CHECK(WF_OK == wf_gitworker_start(&w));
	CHECK(WF_OK == wf_gitworker_repo(w, 1, &e, L"C:\\repo", false));
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(fake.entered, 5000));
	CHECK(WF_OK == wf_gitworker_repo(w, 2, &e, L"C:\\second", false));
	wf_gitworker_cancel(w);
	{
		ULONGLONG end = GetTickCount64() + 5000;

		while (!wf_gitworker_idle(w) && GetTickCount64() < end) {
			Sleep(10);
		}
	}
	CHECK(wf_gitworker_idle(w));
	CHECK(no_answer_for(w, 300));
	CHECK(1 == fake.status_runs); /* the one that waited never ran */
	CHECK(0 == wf_gitworker_stop(w).status);
}

/* The editor drops its own requests (a settings save makes a new editor) and the
 * status the file list waits for goes on, running or finished and not yet taken. */
static void the_editors_cancel_leaves_the_status_alone(void)
{
	WfGitWorker *w;
	WfGitAnswer a;
	WfGitEnv e = win_env();
	WfGitBlob *base = wf_gitblob_new("a\n", 2);

	begin(true);
	fake.entered = CreateEventW(NULL, FALSE, FALSE, NULL);
	fake.release = CreateEventW(NULL, TRUE, FALSE, NULL);
	fake.honour_cancel = true;
	CHECK(WF_OK == wf_gitworker_start(&w));
	CHECK(WF_OK == wf_gitworker_repo(w, 1, &e, L"C:\\repo", false));
	CHECK(WAIT_OBJECT_0 ==
	        WaitForSingleObject(fake.entered, 5000)); /* running */
	CHECK(WF_OK ==
	        wf_gitworker_base(w, 2, 5, &e, L"C:\\repo", "f.c", NULL,
	                true));                                     /* waits */
	CHECK(WF_OK == wf_gitworker_diff(w, 3, 5, base, "b\n", 2)); /* waits */
	wf_gitworker_cancel_editor(w);
	SetEvent(fake.release);
	CHECK(wait_answer(w, &a, 5000));
	CHECK(WF_GIT_A_REPO == a.kind && 1 == a.gen && WF_GITR_OK == a.state);
	wf_gitanswer_free(&a);
	CHECK(no_answer_for(w, 300)); /* the base and the diff never come */
	CHECK(0 ==
	        fake.processes_ended_by_cancel); /* the status was not ended */
	/* a status that is done and not taken survives it too */
	CHECK(WF_OK == wf_gitworker_repo(w, 4, &e, L"C:\\repo", false));
	CHECK(WAIT_OBJECT_0 ==
	        WaitForSingleObject(wf_gitworker_event(w), 5000));
	Sleep(30);
	wf_gitworker_cancel_editor(w);
	CHECK(wait_answer(w, &a, 5000) && WF_GIT_A_REPO == a.kind &&
	        4 == a.gen);
	wf_gitanswer_free(&a);
	/* the whole cancel does drop it */
	CHECK(WF_OK == wf_gitworker_repo(w, 5, &e, L"C:\\repo", false));
	CHECK(WAIT_OBJECT_0 ==
	        WaitForSingleObject(wf_gitworker_event(w), 5000));
	Sleep(30);
	wf_gitworker_cancel(w);
	CHECK(no_answer_for(w, 200));
	wf_gitworker_cancel_editor(NULL); /* NULL-safe */
	CHECK(0 == wf_gitworker_stop(w).status);
	wf_gitblob_release(base);
}

static void stopping_does_not_wait_for_a_slow_git(void)
{
	WfGitWorker *w;
	WfGitEnv e = win_env();
	ULONGLONG t0;

	begin(true);
	fake.entered = CreateEventW(NULL, FALSE, FALSE, NULL);
	fake.release =
	        CreateEventW(NULL, TRUE, FALSE, NULL); /* never released */
	fake.honour_cancel = true;
	CHECK(WF_OK == wf_gitworker_start(&w));
	CHECK(WF_OK == wf_gitworker_repo(w, 1, &e, L"C:\\repo", false));
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(fake.entered, 5000));
	t0 = GetTickCount64();
	{
		WfWorkerStopReport r = wf_gitworker_stop(w);

		CHECK(WF_OK == r.status && !r.deferred);
	}
	CHECK(GetTickCount64() - t0 < 2500);
	CHECK(1 == fake.processes_ended_by_cancel);
	CHECK(WF_OK == wf_gitworker_stop(NULL).status);
}

static void
the_base_is_fetched_by_the_right_path_and_a_new_file_needs_no_process(void)
{
	WfGitWorker *w;
	WfGitAnswer a;
	WfGitEnv e = win_env();

	begin(true);
	CHECK(WF_OK == wf_gitworker_start(&w));
	CHECK(WF_OK ==
	        wf_gitworker_base(
	                w, 1, 7, &e, L"C:\\repo", "dir/file.c", NULL, false));
	CHECK(wait_answer(w, &a, 5000));
	CHECK(WF_GIT_A_BASE == a.kind && 7 == a.id && 1 == a.gen &&
	        WF_GITR_OK == a.state);
	CHECK(a.base && 12 == a.base->n &&
	        !memcmp(a.base->data, "line1\nline2\n", 12));
	CHECK(!wcscmp(fake.last_spec, L"HEAD:dir/file.c") &&
	        !wcscmp(fake.last_root, L"C:\\repo"));
	wf_gitanswer_free(&a);
	/* a renamed file: the old path's content */
	CHECK(WF_OK ==
	        wf_gitworker_base(w, 2, 7, &e, L"C:\\repo", "new name.c",
	                "old name.c", false));
	CHECK(wait_answer(w, &a, 5000) && WF_GITR_OK == a.state);
	CHECK(!wcscmp(fake.last_spec, L"HEAD:old name.c"));
	wf_gitanswer_free(&a);
	/* Japanese in the path reaches Git as the characters it is */
	CHECK(WF_OK ==
	        wf_gitworker_base(w, 3, 7, &e, L"C:\\repo",
	                "\xE6\x97\xA5\xE6\x9C\xAC/\xE8\xAA\x9E.c", NULL,
	                false));
	CHECK(wait_answer(w, &a, 5000) && WF_GITR_OK == a.state);
	CHECK(!wcscmp(fake.last_spec, L"HEAD:\x65E5\x672C/\x8A9E.c"));
	wf_gitanswer_free(&a);
	/* new: empty, and no process */
	{
		LONG runs = fake.base_runs;

		CHECK(WF_OK ==
		        wf_gitworker_base(
		                w, 4, 7, &e, L"C:\\repo", "u.txt", NULL, true));
		CHECK(wait_answer(w, &a, 5000) && WF_GITR_OK == a.state &&
		        a.base && 0 == a.base->n);
		CHECK(runs == fake.base_runs);
		wf_gitanswer_free(&a);
	}
	/* failures are told apart: too large, not found, Git missing */
	fake.base_huge = true;
	CHECK(WF_OK ==
	        wf_gitworker_base(
	                w, 5, 7, &e, L"C:\\repo", "big", NULL, false));
	CHECK(wait_answer(w, &a, 5000) && WF_GITR_TOO_LARGE == a.state &&
	        !a.base);
	wf_gitanswer_free(&a);
	fake.base_huge = false;
	fake.base_fails = true;
	CHECK(WF_OK ==
	        wf_gitworker_base(
	                w, 6, 7, &e, L"C:\\repo", "missing", NULL, false));
	CHECK(wait_answer(w, &a, 5000) && WF_GITR_FAILED == a.state && !a.base);
	wf_gitanswer_free(&a);
	CHECK(1 ==
	        fake.verify_runs); /* asked whether the path is in HEAD: an error, not "new" */
	/* Git says HEAD has no such path (or no commit): the file is new, though nothing
	 * told the screen so */
	fake.verify_exit = 1;
	CHECK(WF_OK ==
	        wf_gitworker_base(w, 8, 7, &e, L"C:\\repo", "newdir/file.c",
	                NULL, false));
	CHECK(wait_answer(w, &a, 5000) && WF_GITR_OK == a.state && a.base &&
	        0 == a.base->n);
	wf_gitanswer_free(&a);
	/* too large is told before that question is asked, and a blob that is there needs none */
	{
		LONG asked = fake.verify_runs;

		fake.base_fails = false;
		CHECK(WF_OK ==
		        wf_gitworker_base(w, 9, 7, &e, L"C:\\repo", "there.c",
		                NULL, false));
		CHECK(wait_answer(w, &a, 5000) && WF_GITR_OK == a.state &&
		        a.base && 12 == a.base->n);
		wf_gitanswer_free(&a);
		fake.base_huge = true;
		CHECK(WF_OK ==
		        wf_gitworker_base(
		                w, 10, 7, &e, L"C:\\repo", "big", NULL, false));
		CHECK(wait_answer(w, &a, 5000) && WF_GITR_TOO_LARGE == a.state);
		wf_gitanswer_free(&a);
		fake.base_huge = false;
		CHECK(asked == fake.verify_runs);
	}
	CHECK(WF_E_INVAL ==
	        wf_gitworker_base(w, 7, 7, &e, L"C:\\repo", "", NULL, false));
	CHECK(0 == wf_gitworker_stop(w).status);
	begin(false);
	CHECK(WF_OK == wf_gitworker_start(&w));
	CHECK(WF_OK ==
	        wf_gitworker_base(w, 1, 7, &e, L"C:\\repo", "f", NULL, false));
	CHECK(wait_answer(w, &a, 5000) && WF_GITR_UNAVAILABLE == a.state);
	wf_gitanswer_free(&a);
	CHECK(0 == fake.calls);
	CHECK(0 == wf_gitworker_stop(w).status);
}

static void the_diff_is_made_without_a_process_and_before_a_waiting_status(void)
{
	WfGitWorker *w;
	WfGitAnswer a;
	WfGitEnv e = win_env();
	WfGitBlob *base = wf_gitblob_new("a\nb\nc\n", 6);

	begin(true);
	fake.entered = CreateEventW(NULL, FALSE, FALSE, NULL);
	fake.release = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(WF_OK == wf_gitworker_start(&w));
	CHECK(WF_OK == wf_gitworker_diff(w, 1, 9, base, "a\nX\nc\n", 6));
	CHECK(wait_answer(w, &a, 5000));
	CHECK(WF_GIT_A_DIFF == a.kind && 9 == a.id && WF_GITR_OK == a.state &&
	        1 == a.diff.n);
	CHECK(1 == a.diff.h[0].a && 1 == a.diff.h[0].an && 1 == a.diff.h[0].bn);
	wf_gitanswer_free(&a);
	CHECK(0 == fake.calls); /* no process for a diff */
	/* busy with a status: a status and then a diff wait; the diff goes first */
	CHECK(WF_OK == wf_gitworker_repo(w, 1, &e, L"C:\\repo", false));
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(fake.entered, 5000));
	/* the base waits behind a gate, so it is clear which of the two goes first */
	fake.base_gate = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(WF_OK ==
	        wf_gitworker_base(w, 2, 9, &e, L"C:\\repo", "x", NULL,
	                false)); /* waits */
	CHECK(WF_OK ==
	        wf_gitworker_diff(
	                w, 3, 9, base, "a\nb\nc\nd\n", 8)); /* waits */
	SetEvent(fake.release);
	/* the status that was running and the diff are answered while the base is still
	 * held: the diff went first (in which order they are taken is not the point) */
	{
		bool got_repo = false, got_diff = false;

		for (int i = 0; i < 2 && wait_answer(w, &a, 3000); i++) {
			got_repo |= WF_GIT_A_REPO == a.kind && 1 == a.gen;
			got_diff |= WF_GIT_A_DIFF == a.kind && 3 == a.gen;
			wf_gitanswer_free(&a);
		}
		CHECK(got_repo && got_diff);
	}
	SetEvent(fake.base_gate);
	CHECK(wait_answer(w, &a, 3000) && WF_GIT_A_BASE == a.kind &&
	        2 == a.gen);
	wf_gitanswer_free(&a);
	CloseHandle(fake.base_gate);
	fake.base_gate = NULL;
	/* too much to compare */
	{
		char *x = malloc(40000), *y = malloc(40000);
		size_t kx = 0, ky = 0;

		for (int i = 0; i < 3000; i++) {
			kx += (size_t)sprintf_s(x + kx, 40000 - kx, "a%d\n", i);
			ky += (size_t)sprintf_s(y + ky, 40000 - ky, "b%d\n", i);
		}
		wf_gitblob_release(base);
		base = wf_gitblob_new(x, kx);
		CHECK(WF_OK == wf_gitworker_diff(w, 4, 9, base, y, ky));
		CHECK(wait_answer(w, &a, 5000) && 4 == a.gen);
		CHECK(WF_GITR_TOO_LARGE == a.state && a.diff_too_many &&
		        0 == a.diff.n);
		wf_gitanswer_free(&a);
		free(x);
		free(y);
	}
	CHECK(WF_E_INVAL == wf_gitworker_diff(w, 5, 9, NULL, "x", 1));
	CHECK(0 == wf_gitworker_stop(w).status);
	CHECK(1 == base->refs); /* the worker let go of what it held */
	wf_gitblob_release(base);
}

static void a_worker_comes_and_goes_without_leaving_anything(void)
{
	DWORD before = 0, after = 0;
	WfGitEnv e = win_env();

	begin(true);
	{
		WfGitWorker *w;

		CHECK(WF_OK ==
		        wf_gitworker_start(
		                &w)); /* the first start loads what it needs */
		CHECK(0 == wf_gitworker_stop(w).status);
	}
	GetProcessHandleCount(GetCurrentProcess(), &before);
	for (int i = 0; i < 100; i++) {
		WfGitWorker *w;
		WfGitAnswer a;

		CHECK(WF_OK == wf_gitworker_start(&w));
		CHECK(WF_OK == wf_gitworker_repo(w, 1, &e, L"C:\\repo", false));
		CHECK(wait_answer(w, &a, 5000));
		wf_gitanswer_free(&a);
		/* and once with an answer nobody polls, and a request that waits */
		CHECK(WF_OK == wf_gitworker_repo(w, 2, &e, L"C:\\repo", false));
		CHECK(WF_OK ==
		        wf_gitworker_base(
		                w, 3, 1, &e, L"C:\\repo", "f", NULL, false));
		CHECK(0 == wf_gitworker_stop(w).status);
	}
	GetProcessHandleCount(GetCurrentProcess(), &after);
	printf("info: handles %lu before, %lu after 100 worker lives\n", before,
	        after);
	CHECK(after <= before + 2);
}

static void make_tree(wchar_t *top, size_t cap, bool with_git)
{
	wchar_t tmp[MAX_PATH], p[MAX_PATH];

	CHECK(GetTempPathW(MAX_PATH, tmp) > 0);
	swprintf_s(top, cap, L"%lspiero_gw_%lu", tmp, GetCurrentProcessId());
	CreateDirectoryW(top, NULL);
	swprintf_s(p, MAX_PATH, L"%ls\\work", top);
	CreateDirectoryW(p, NULL);
	if (with_git) {
		swprintf_s(p, MAX_PATH, L"%ls\\work\\.git", top);
		CreateDirectoryW(p, NULL);
	}
	swprintf_s(p, MAX_PATH, L"%ls\\work\\sub", top);
	CreateDirectoryW(p, NULL);
	swprintf_s(p, MAX_PATH, L"%ls\\work\\sub\\deep", top);
	CreateDirectoryW(p, NULL);
}

static void drop_tree(const wchar_t *top)
{
	wchar_t p[MAX_PATH];

	swprintf_s(p, MAX_PATH, L"%ls\\work\\sub\\deep", top);
	RemoveDirectoryW(p);
	swprintf_s(p, MAX_PATH, L"%ls\\work\\sub", top);
	RemoveDirectoryW(p);
	swprintf_s(p, MAX_PATH, L"%ls\\work\\.git", top);
	RemoveDirectoryW(p);
	swprintf_s(p, MAX_PATH, L"%ls\\work", top);
	RemoveDirectoryW(p);
	RemoveDirectoryW(top);
}

static void a_folder_is_resolved_to_its_repository_by_the_worker(void)
{
	WfGitWorker *w;
	WfGitAnswer a;
	wchar_t top[MAX_PATH], deep[MAX_PATH], work[MAX_PATH];
	WfGitEnv e;

	begin(true);
	make_tree(top, MAX_PATH, true);
	swprintf_s(deep, MAX_PATH, L"%ls\\work\\sub\\deep", top);
	swprintf_s(work, MAX_PATH, L"%ls\\work", top);
	CHECK(WF_GENV_WINDOWS == wf_gitenv_classify_text(deep, &e));
	CHECK(WF_OK == wf_gitworker_start(&w));
	CHECK(WF_OK == wf_gitworker_repo(w, 1, &e, deep, true));
	CHECK(wait_answer(w, &a, 5000));
	CHECK(WF_GITR_OK == a.state && 1 == a.gen);
	CHECK(a.asked && !wcscmp(a.asked, deep)); /* what was asked about */
	CHECK(a.root && !wcscmp(a.root, work));   /* where the .git is */
	CHECK(!wcscmp(fake.last_root,
	        work)); /* Git ran there, not in the folder asked */
	wf_gitanswer_free(&a);
	/* without `resolve` the folder is taken as the root: nothing is looked for */
	CHECK(WF_OK == wf_gitworker_repo(w, 2, &e, deep, false));
	CHECK(wait_answer(w, &a, 5000));
	CHECK(a.root && !wcscmp(a.root, deep) && !wcscmp(fake.last_root, deep));
	wf_gitanswer_free(&a);
	/* locating: where the nearest .git is, and no Git process for it */
	{
		LONG before = fake.calls;

		CHECK(WF_OK ==
		        wf_gitworker_repo(w, 3, &e, deep, WF_GIT_REPO_LOCATE));
		CHECK(wait_answer(w, &a, 5000));
		CHECK(3 == a.gen && a.located && WF_GITR_OK == a.state);
		CHECK(a.asked && !wcscmp(a.asked, deep));
		CHECK(a.root && !wcscmp(a.root, work));
		CHECK(before == fake.calls);
		CHECK(0 == a.table.n);
		wf_gitanswer_free(&a);
	}
	CHECK(0 == wf_gitworker_stop(w).status);
	drop_tree(top);
}

static void a_folder_in_no_work_tree_is_answered_without_a_process(void)
{
	WfGitWorker *w;
	WfGitAnswer a;
	wchar_t top[MAX_PATH], deep[MAX_PATH];
	WfGitEnv e;

	begin(true);
	make_tree(top, MAX_PATH,
	        false); /* no .git, but one may be above the temp folder */
	swprintf_s(deep, MAX_PATH, L"%ls\\work\\sub\\deep", top);
	CHECK(WF_GENV_WINDOWS == wf_gitenv_classify_text(deep, &e));
	CHECK(WF_OK == wf_gitworker_start(&w));
	CHECK(WF_OK == wf_gitworker_repo(w, 1, &e, deep, true));
	CHECK(wait_answer(w, &a, 5000));
	{
		wchar_t above[MAX_PATH * 4];

		/* a .git above the temp folder would make this a work tree: nothing to test */
		if (!wf_git_nearest_dotgit(deep, above, ARRAYSIZE(above))) {
			CHECK(WF_GITR_NOT_REPO == a.state);
			CHECK(0 == fake.calls &&
			        !a.root); /* not even a probe of Git */
		}
	}
	CHECK(a.asked && !wcscmp(a.asked, deep));
	wf_gitanswer_free(&a);
	CHECK(0 == wf_gitworker_stop(w).status);
	drop_tree(top);
}

static void the_base_of_a_file_is_found_with_its_repository_by_the_worker(void)
{
	WfGitWorker *w;
	WfGitAnswer a;
	wchar_t top[MAX_PATH], file[MAX_PATH], work[MAX_PATH];
	WfGitEnv e;

	begin(true);
	make_tree(top, MAX_PATH, true);
	swprintf_s(file, MAX_PATH, L"%ls\\work\\sub\\f.txt", top);
	swprintf_s(work, MAX_PATH, L"%ls\\work", top);
	CHECK(WF_GENV_WINDOWS == wf_gitenv_classify_text(file, &e));
	CHECK(WF_OK == wf_gitworker_start(&w));
	CHECK(WF_E_INVAL == wf_gitworker_base_of(w, 0, 7, &e, file));
	CHECK(WF_OK == wf_gitworker_base_of(w, 1, 7, &e, file));
	CHECK(wait_answer(w, &a, 5000));
	CHECK(WF_GIT_A_BASE == a.kind && 7 == a.id && WF_GITR_OK == a.state);
	CHECK(!wcscmp(fake.last_spec, L"HEAD:sub/f.txt"));
	CHECK(!wcscmp(fake.last_root, work));
	wf_gitanswer_free(&a);
	CHECK(0 == wf_gitworker_stop(w).status);
	drop_tree(top);
}

int main(void)
{
	a_folder_is_resolved_to_its_repository_by_the_worker();
	a_folder_in_no_work_tree_is_answered_without_a_process();
	the_base_of_a_file_is_found_with_its_repository_by_the_worker();
	a_repository_answers_with_its_status();
	the_answers_for_a_missing_git_a_non_repository_and_a_failure();
	a_newer_request_ends_the_older_process_and_only_it_answers();
	an_old_answer_is_dropped_even_when_its_process_was_not_ended();
	rapid_switching_delivers_only_the_last_repository();
	an_unpolled_answer_is_not_delivered_after_a_newer_request();
	cancel_drops_what_waits_and_ends_what_runs();
	the_editors_cancel_leaves_the_status_alone();
	stopping_does_not_wait_for_a_slow_git();
	the_base_is_fetched_by_the_right_path_and_a_new_file_needs_no_process();
	the_diff_is_made_without_a_process_and_before_a_waiting_status();
	a_worker_comes_and_goes_without_leaving_anything();
	wf_git_hooks_set(NULL);
	return TEST_RESULT();
}

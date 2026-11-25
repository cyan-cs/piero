/*
 * The syntax worker with the real piero-syntax.dll (DR-DRAFT-033): it loads the
 * DLL on its own thread when the first request comes (and not before), answers
 * for the newest request of each slot only, closes a slot's session when told,
 * colours .env without any DLL, reports a language it cannot use, and stops with
 * its sessions closed so the DLL can be let go.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "syntax/wf_synhost_win.h"
#include "syntax/wf_synlang.h"
#include "syntax/wf_synruns.h"
#include "syntax/wf_synworker.h"
#include "wf_syntax_pin.h"
#include "wf_test.h"

static bool wait_answer(WfSynWorker *w, WfSynAnswer *a, DWORD ms)
{
	ULONGLONG t0 = GetTickCount64();

	do {
		if (WAIT_OBJECT_0 ==
		                WaitForSingleObject(
		                        wf_synworker_event(w), 50) &&
		        wf_synworker_poll(w, a)) {
			return true;
		}
	} while (GetTickCount64() - t0 < ms);
	return false;
}

static bool no_answer(WfSynWorker *w, DWORD ms)
{
	WfSynAnswer a;
	ULONGLONG t0 = GetTickCount64();

	do {
		if (wf_synworker_poll(w, &a)) {
			wf_synanswer_free(&a);
			return false;
		}
		Sleep(10);
	} while (GetTickCount64() - t0 < ms);
	return true;
}

static void stop(WfSynWorker *w)
{
	WfWorkerStopReport rep = wf_synworker_stop(w);

	CHECK(WF_OK == rep.status && !rep.deferred);
	if (rep.completion) {
		CloseHandle(rep.completion);
	}
}

static void the_worker_loads_nothing_until_asked_and_answers(void)
{
	WfSynWorker *w = NULL;
	WfSynPort p;
	WfSynAnswer a;
	static const char T[] = "{\"a\": 1, \"b\": true}\n";

	CHECK(WF_OK == wf_synworker_start(&w));
	p = wf_synworker_port(w);
	Sleep(200);
	CHECK(!wf_synhost_loaded()); /* a running worker is not a loaded DLL */
	CHECK(NULL == GetModuleHandleW(L"piero-syntax.dll"));
	CHECK(p.installed(p.ctx, "json") && !p.installed(p.ctx, "nope"));
	CHECK(!wf_synhost_loaded()); /* asking what is installed loads nothing either */
	CHECK(WF_OK ==
	        p.request(p.ctx, WF_SYN_SLOT_PREVIEW, 7, "json", T,
	                sizeof T - 1));
	CHECK(wait_answer(w, &a, 10000));
	CHECK(WF_SYN_SLOT_PREVIEW == a.slot && 7 == a.gen &&
	        WF_SYN_OK == a.status && sizeof T - 1 == a.text_len &&
	        !strcmp(a.lang, "json"));
	CHECK(a.nruns > 3 && a.runs);
	CHECK(WF_SC_NUMBER == wf_syn_runs_class_at(a.runs, a.nruns, 6));
	CHECK(WF_SC_BOOLEAN == wf_syn_runs_class_at(a.runs, a.nruns, 14));
	CHECK(wf_synhost_loaded() && GetModuleHandleW(L"piero-syntax.dll"));
	wf_synanswer_free(&a);
	stop(w);
	wf_synhost_shutdown();
	CHECK(NULL == GetModuleHandleW(L"piero-syntax.dll"));
}

static void only_the_newest_request_of_a_slot_is_answered(void)
{
	WfSynWorker *w = NULL;
	WfSynPort p;
	WfSynAnswer a;
	int got = 0;

	CHECK(WF_OK == wf_synworker_start(&w));
	p = wf_synworker_port(w);
	/* many requests at once: the ones the thread has not taken are replaced */
	for (uint32_t g = 1; g <= 50; g++) {
		char t[64];
		int n = snprintf(t, sizeof t, "[%u]\n", g);

		CHECK(WF_OK ==
		        p.request(p.ctx, WF_SYN_SLOT_PREVIEW, g, "json", t,
		                (size_t)n));
	}
	/* whatever comes, the last gen comes and no older gen comes after it */
	{
		uint32_t last = 0;
		ULONGLONG t0 = GetTickCount64();

		while (GetTickCount64() - t0 < 10000 && last != 50) {
			if (wait_answer(w, &a, 200)) {
				CHECK(a.gen >
				        last); /* never an older one after a newer one */
				last = a.gen;
				got++;
				wf_synanswer_free(&a);
			}
		}
		CHECK(50 == last);
		CHECK(got >= 1 && got <= 50);
	}
	/* a request, then a close of the slot: the answer never arrives */
	CHECK(WF_OK ==
	        p.request(p.ctx, WF_SYN_SLOT_PREVIEW, 60, "json", "[1]\n", 4));
	p.close(p.ctx, WF_SYN_SLOT_PREVIEW);
	CHECK(no_answer(w, 700));
	/* the same while the thread is already working on a large text */
	{
		size_t cap = 1500000, at = 0;
		char *t = malloc(cap);

		CHECK(t);
		if (t) {
			at += (size_t)snprintf(t + at, cap - at, "[\n");
			while (at + 40 < cap) {
				at += (size_t)snprintf(t + at, cap - at,
				        "  {\"k\": [1, 2, 3], \"v\": "
				        "\"text\"},\n");
			}
			at += (size_t)snprintf(t + at, cap - at, "  0\n]\n");
			CHECK(WF_OK ==
			        p.request(p.ctx, WF_SYN_SLOT_PREVIEW, 61,
			                "json", t, at));
			free(t);
			Sleep(40); /* the thread has taken it and is parsing */
			p.close(p.ctx, WF_SYN_SLOT_PREVIEW);
			CHECK(no_answer(w,
			        4000)); /* what it makes is never delivered */
		}
	}
	/* a slot's answer is for its own slot */
	CHECK(WF_OK ==
	        p.request(p.ctx, WF_SYN_SLOT_EDITOR, 71, "json", "[2]\n", 4));
	CHECK(WF_OK ==
	        p.request(p.ctx, WF_SYN_SLOT_PREVIEW, 62, "yaml", "a: 1\n", 5));
	{
		bool seen_editor = false, seen_preview = false;
		ULONGLONG t0 = GetTickCount64();

		while (GetTickCount64() - t0 < 10000 &&
		        !(seen_editor && seen_preview)) {
			if (wait_answer(w, &a, 200)) {
				if (WF_SYN_SLOT_EDITOR == a.slot) {
					seen_editor = 71 == a.gen &&
					        !strcmp(a.lang, "json");
				} else {
					seen_preview = 62 == a.gen &&
					        !strcmp(a.lang, "yaml");
				}
				CHECK(WF_SYN_OK == a.status);
				wf_synanswer_free(&a);
			}
		}
		CHECK(seen_editor && seen_preview);
	}
	/* a slot that changes language: a new session, the old one closed */
	CHECK(WF_OK ==
	        p.request(
	                p.ctx, WF_SYN_SLOT_PREVIEW, 70, "toml", "a = 1\n", 6));
	CHECK(wait_answer(w, &a, 10000) && 70 == a.gen &&
	        WF_SYN_OK == a.status && !strcmp(a.lang, "toml"));
	wf_synanswer_free(&a);
	CHECK(2 >= wf_synhost_stat(
	                   WF_SYN_STAT_SESSIONS)); /* one per slot at most */
	stop(w);
	CHECK(0 ==
	        wf_synhost_stat(
	                WF_SYN_STAT_SESSIONS)); /* all closed by the stop */
	wf_synhost_shutdown();
}

static void an_unknown_language_and_bad_requests_are_answers_not_crashes(void)
{
	WfSynWorker *w = NULL;
	WfSynPort p;
	WfSynAnswer a;
	char big[100];

	CHECK(WF_OK == wf_synworker_start(&w));
	p = wf_synworker_port(w);
	CHECK(WF_E_INVAL == p.request(p.ctx, WF_SYN_SLOTS, 1, "json", "[]", 2));
	CHECK(WF_E_INVAL ==
	        p.request(p.ctx, 0, 0, "json", "[]", 2)); /* gen 0 is none */
	CHECK(WF_E_INVAL == p.request(p.ctx, 0, 1, NULL, "[]", 2));
	CHECK(WF_E_INVAL == p.request(p.ctx, 0, 1, "json", NULL, 2));
	memset(big, 'x', sizeof big - 1);
	big[sizeof big - 1] = '\0';
	CHECK(WF_E_INVAL ==
	        p.request(p.ctx, 0, 1, big, "[]",
	                2)); /* a language name too long */
	p.close(p.ctx, 9);   /* no such slot: nothing */
	CHECK(WF_OK ==
	        p.request(p.ctx, 0, 3, "c", "int x;\n",
	                7)); /* not an official one */
	CHECK(wait_answer(w, &a, 5000) && 3 == a.gen &&
	        WF_SYN_E_UNKNOWN == a.status && 0 == a.nruns && !a.runs);
	wf_synanswer_free(&a);
	CHECK(WF_OK ==
	        p.request(p.ctx, 0, 4, "json", "", 0)); /* nothing to colour */
	CHECK(wait_answer(w, &a, 5000) && 4 == a.gen && WF_SYN_OK == a.status &&
	        0 == a.nruns);
	wf_synanswer_free(&a);
	/* a text over the limit is an answer too, and the next text works again */
	{
		char *t = malloc(3u * 1024u * 1024u);

		CHECK(t);
		if (t) {
			memset(t, ' ', 3u * 1024u * 1024u);
			CHECK(WF_OK ==
			        p.request(p.ctx, 0, 5, "json", t,
			                3u * 1024u * 1024u));
			CHECK(wait_answer(w, &a, 10000) && 5 == a.gen &&
			        WF_SYN_E_LIMIT == a.status);
			wf_synanswer_free(&a);
			free(t);
		}
	}
	CHECK(WF_OK == p.request(p.ctx, 0, 6, "json", "[1]\n", 4));
	CHECK(wait_answer(w, &a, 10000) && 6 == a.gen &&
	        WF_SYN_OK == a.status && 1 <= a.nruns);
	wf_synanswer_free(&a);
	stop(w);
	wf_synhost_shutdown();
}

static void dotenv_is_coloured_without_any_dll(void)
{
	WfSynWorker *w = NULL;
	WfSynPort p;
	WfSynAnswer a;
	static const char T[] = "# c\nA=\"x $B\"\n";

	CHECK(WF_OK == wf_synworker_start(&w));
	p = wf_synworker_port(w);
	CHECK(p.installed(p.ctx, "dotenv")); /* always */
	CHECK(WF_OK ==
	        p.request(p.ctx, WF_SYN_SLOT_EDITOR, 1, "dotenv", T,
	                sizeof T - 1));
	CHECK(wait_answer(w, &a, 5000) && WF_SYN_OK == a.status);
	CHECK(WF_SC_COMMENT == wf_syn_runs_class_at(a.runs, a.nruns, 0));
	CHECK(WF_SC_KEY == wf_syn_runs_class_at(a.runs, a.nruns, 4));
	CHECK(WF_SC_STRING == wf_syn_runs_class_at(a.runs, a.nruns, 7));
	wf_synanswer_free(&a);
	CHECK(!wf_synhost_loaded()); /* no DLL was needed */
	CHECK(NULL == GetModuleHandleW(L"piero-syntax.dll"));
	stop(w);
}

static void a_worker_that_is_stopped_refuses_work(void)
{
	WfSynWorker *w = NULL;
	WfSynPort p;
	WfWorkerStopReport rep;

	CHECK(WF_OK == wf_synworker_start(&w));
	p = wf_synworker_port(w);
	CHECK(WF_OK == p.request(p.ctx, 0, 1, "json", "[1]\n", 4));
	rep = wf_synworker_stop(w);
	CHECK(WF_OK == rep.status);
	if (rep.completion) {
		CloseHandle(rep.completion);
	}
	CHECK(0 == wf_synhost_stat(WF_SYN_STAT_SESSIONS));
	wf_synhost_shutdown();
	CHECK(WF_E_INVAL == wf_synworker_start(NULL));
}

int main(void)
{
	wf_synhost_configure(NULL, WF_SYNTAX_DLL_SHA256);
	the_worker_loads_nothing_until_asked_and_answers();
	wf_synhost_configure(NULL, WF_SYNTAX_DLL_SHA256);
	only_the_newest_request_of_a_slot_is_answered();
	wf_synhost_configure(NULL, WF_SYNTAX_DLL_SHA256);
	an_unknown_language_and_bad_requests_are_answers_not_crashes();
	dotenv_is_coloured_without_any_dll();
	a_worker_that_is_stopped_refuses_work();
	return TEST_RESULT();
}

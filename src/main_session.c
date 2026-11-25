/*
 * Connects session capture and restoration to the main loop (DR-DRAFT-018).
 * --continue reads synchronously at startup; [session] persist submits later
 * checkpoints to the writer thread. Store opening and writer-lock acquisition
 * also perform synchronous I/O. Shutdown waits at most FINAL_WRITE_WAIT_MS for
 * the writer, retaining its store if the thread has not stopped.
 */
#include "main_loop.h"

#include <stdio.h>

#include "config/wf_config_win.h"
#include "path/cwd_neutralize.h"

#define CHECKPOINT_AFTER_MS 1000u /* changes are gathered for this long */
#define FINAL_WRITE_WAIT_MS 2000u /* a normal exit waits this long, no more */

/* A notice that must still be there when the first listing has been drawn. */
static void tell(Loop *lp, const char *text)
{
	lp->app.now_ms = GetTickCount64();
	wf_app_notice(&lp->app, text);
}

static void tell_msg(Loop *lp, WfMsg msg)
{
	tell(lp, wf_msg(msg));
}

/* Reads the saved session and starts putting it back. */
static void continue_from_store(Loop *lp)
{
	WfSession session;
	int version = 0;
	char text[sizeof lp->app.msg];

	switch (wf_session_store_read(lp->session_store, &session, &version)) {
	case WF_SESSION_FOUND:
		if (WF_OK != wf_app_session_restore(&lp->app, &session)) {
			tell_msg(lp, MSG_SESSION_DAMAGED);
		}
		wf_session_free(&session);
		break;
	case WF_SESSION_NONE:    tell_msg(lp, MSG_SESSION_NONE); break;
	case WF_SESSION_DAMAGED: tell_msg(lp, MSG_SESSION_DAMAGED); break;
	case WF_SESSION_UNSUPPORTED:
		(void)wf_msgf(text, sizeof text, MSG_SESSION_UNSUPPORTED,
		        &WF_ARG_N(version), 1);
		tell(lp, text);
		break;
	}
}

void wf_main_session_start(
        Loop *lp, const wchar_t *state_dir, bool continue_session)
{
	bool persist = 0 != lp->config->session_persist;

	if (!continue_session && !persist) {
		return; /* nothing asked for: no file is looked at */
	}
	if (!state_dir ||
	        WF_OK != wf_session_store_open(state_dir, &lp->session_store)) {
		tell_msg(lp, MSG_SESSION_UNAVAILABLE);
		return;
	}
	if (continue_session) {
		continue_from_store(lp);
	}
	if (!persist) {
		return;
	}
	switch (wf_session_writer_start(
	        lp->session_store, &lp->session_writer)) {
	case WF_OK: wf_main_session_note(lp); break;
	case WF_E_ACCESS:
		/* another piero holds the right to write: it is told, but a
		 * notice about the session itself is not covered */
		if (!lp->app.msg[0]) {
			tell_msg(lp, MSG_SESSION_NOT_WRITER);
		}
		break;
	default:
		if (!lp->app.msg[0]) {
			tell_msg(lp, MSG_SESSION_UNAVAILABLE);
		}
		break;
	}
}

void wf_main_session_note(Loop *lp)
{
	if (lp->session_writer && !lp->session_due) {
		lp->session_due = GetTickCount64() + CHECKPOINT_AFTER_MS;
	}
}

uint32_t wf_main_session_timeout(const Loop *lp, uint64_t now_ms)
{
	if (!lp->session_due) {
		return WF_WAIT_FOREVER;
	}
	return lp->session_due > now_ms ? (uint32_t)(lp->session_due - now_ms)
	                                : 0;
}

/* Builds the text of the screen's state; NULL when there is none to save. */
static char *build(Loop *lp, size_t *n)
{
	WfSession session;
	char *text = NULL;

	if (!wf_app_session_capture(&lp->app, &session)) {
		return NULL;
	}
	if (WF_OK != wf_session_encode(&session, &text, n)) {
		text = NULL;
	}
	wf_session_free(&session);
	return text;
}

/* What was handed over last is remembered only to skip a state that is the
 * same; it says "sent", not "saved". When the writer could not take a state or
 * could not write it, the record is dropped, so that the same state is handed
 * over again instead of being thrown away as "the same as before". */
static void forget_last(Loop *lp)
{
	wf_dedupe_forget(&lp->session_last);
}

/* Hands `text` to the writer unless it is what was handed over last. */
static void hand_over(Loop *lp, char *text, size_t n)
{
	if (wf_dedupe_is_repeat(&lp->session_last, text, n)) {
		free(text); /* nothing changed that the file would show */
		return;
	}
	(void)wf_dedupe_remember(&lp->session_last, text, n);
	if (WF_OK != wf_session_writer_submit(lp->session_writer, text, n)) {
		forget_last(
		        lp); /* nothing was taken: it must be offered again */
	}
}

void wf_main_session_tick(Loop *lp)
{
	uint64_t now = GetTickCount64();
	size_t n = 0;
	char *text;

	if (!lp->session_writer) {
		return;
	}
	if (wf_session_writer_take_failure(lp->session_writer)) {
		forget_last(
		        lp); /* what was handed over did not reach the disk */
		if (!lp->session_told) {
			lp->session_told =
			        true; /* once; the rest of the program works */
			tell_msg(lp, MSG_SESSION_SAVE_FAILED);
		}
	}
	if (!lp->session_due || now < lp->session_due) {
		return;
	}
	lp->session_due = 0;
	text = build(lp, &n);
	if (text) {
		hand_over(lp, text, n);
	}
}

void wf_main_session_stop(Loop *lp)
{
	size_t n = 0;
	char *text;
	bool store_in_use = false;

	if (lp->session_writer) {
		WfWorkerStopReport stop;

		/* the last state, written before the program ends */
		text = build(lp, &n);
		if (text) {
			hand_over(lp, text, n);
		}
		stop = wf_session_writer_stop(
		        lp->session_writer, FINAL_WRITE_WAIT_MS);
		lp->session_writer = NULL;
		/* a thread that did not stop in time is still writing and uses the
		 * store: it, and the right to write that it holds, stay until the
		 * process ends */
		store_in_use = 0 != stop.deferred;
		if (stop.completion) {
			CloseHandle(stop.completion);
		}
	}
	forget_last(lp);
	if (!store_in_use) {
		wf_session_store_close(lp->session_store);
	}
	lp->session_store = NULL;
}

/* `piero --forget-session`: empties the saved session. */
int wf_main_forget_session(void)
{
	wchar_t *state = NULL;
	HANDLE pin = INVALID_HANDLE_VALUE;
	DWORD error = 0;
	WfSessionStore *store = NULL;
	int result = 3;
	WfStatus st;

	if (WF_OK !=
	                wf_startup_state_path(
	                        L"LOCALAPPDATA", L"", &state, NULL, &error) ||
	        !wf_private_directory_open(state, false, &pin, &error)) {
		fprintf(stderr,
		        "piero: the state folder cannot be used (Win32 %lu); "
		        "nothing was changed\n",
		        error);
		goto done;
	}
	if (WF_OK != wf_session_store_open(state, &store) ||
	        !wf_session_store_lock(store)) {
		fputs("piero: another piero is running with session saving "
		      "on; nothing was changed\n",
		        stderr);
		goto done;
	}
	st = wf_session_store_forget(store);
	if (WF_OK != st) {
		fprintf(stderr,
		        "piero: the saved session could not be emptied (%s); "
		        "check the files session.a and session.b in the state "
		        "folder\n",
		        wf_status_name(st));
		goto done;
	}
	puts("piero: the saved session is empty now");
	result = 0;
done:
	wf_session_store_close(store);
	if (INVALID_HANDLE_VALUE != pin) {
		CloseHandle(pin);
	}
	free(state);
	return result;
}

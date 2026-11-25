#include "../async/wf_workcore.h"
#include "wf_synworker.h"

#include <stdlib.h>
#include <string.h>

#include "wf_dotenv.h"
#include "wf_synhost_win.h"
#include "wf_synlang.h"

#define STOP_WAIT_MS 2000

typedef struct
{
	bool has;   /* a request waits for the thread */
	bool close; /* ... and it is "end the session" */
	uint32_t gen;
	char lang[24];
	char *text;
	size_t len;
	volatile LONG live; /* the gen that may still be answered; 0: none */
	bool has_ans;
	WfSynAnswer ans;
} Slot;

struct WfSynWorker
{
	WfWorkCore core;
	Slot slot[WF_SYN_SLOTS];
	volatile LONG stopping;
};

static void worker_free(void *owner)
{
	WfSynWorker *w = owner;

	for (int i = 0; i < WF_SYN_SLOTS; i++) {
		free(w->slot[i].text);
		wf_synanswer_free(&w->slot[i].ans);
	}
	free(w);
}

typedef struct
{
	WfSynSession *session;
	char lang[24];
} Held;

static void held_close(Held *h)
{
	if (h->session) {
		wf_synhost_close(h->session);
		h->session = NULL;
	}
	h->lang[0] = '\0';
}

/* Colours one request; fills *ans (the runs are the worker's own copy). */
static void colour(Held *h,
        const char *lang,
        const char *text,
        size_t len,
        WfSynAnswer *ans)
{
	const WfSynLang *l = wf_synlang_by_id(lang);
	WfSynResult r;

	ans->status = WF_SYN_E_UNKNOWN;
	if (!l) {
		return;
	}
	if (l->builtin) {
		held_close(
		        h); /* a lexer inside piero.exe: no DLL, no session */
		ans->status = wf_dotenv_highlight(
		        text, len, NULL, &ans->runs, &ans->nruns);
		return;
	}
	if (!h->session || strcmp(h->lang, lang)) {
		held_close(h);
		ans->status = wf_synhost_open(lang, NULL, &h->session);
		if (WF_SYN_OK != ans->status) {
			h->session = NULL;
			return;
		}
		strcpy_s(h->lang, sizeof h->lang, lang);
	}
	memset(&r, 0, sizeof r);
	ans->status = wf_synhost_highlight(h->session, text, len, &r);
	ans->parse_us = r.parse_us;
	ans->query_us = r.query_us;
	ans->incremental = r.incremental;
	if (WF_SYN_OK == ans->status && r.nruns) {
		/* the DLL has its own heap: the answer is copied to ours */
		ans->runs = malloc((size_t)r.nruns * sizeof *ans->runs);
		if (ans->runs) {
			memcpy(ans->runs, r.runs,
			        (size_t)r.nruns * sizeof *ans->runs);
			ans->nruns = r.nruns;
		} else {
			ans->status = WF_SYN_E_NOMEM;
		}
	}
	wf_synhost_result_free(&r);
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	WfSynWorker *w = arg;
	Held held[WF_SYN_SLOTS];

	memset(held, 0, sizeof held);
	for (;;) {
		int s = -1;
		Slot *sl;
		char lang[24];
		char *text;
		size_t len;
		uint32_t gen;
		bool close;
		WfSynAnswer ans;

		AcquireSRWLockExclusive(&w->core.lock);
		for (;;) {
			if (w->core.stop) {
				break;
			}
			/* the editor first: a key is waiting for it */
			if (w->slot[WF_SYN_SLOT_EDITOR].has) {
				s = WF_SYN_SLOT_EDITOR;
				break;
			}
			if (w->slot[WF_SYN_SLOT_PREVIEW].has) {
				s = WF_SYN_SLOT_PREVIEW;
				break;
			}
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, INFINITE, 0);
		}
		if (w->core.stop) {
			ReleaseSRWLockExclusive(&w->core.lock);
			break;
		}
		sl = &w->slot[s];
		gen = sl->gen;
		close = sl->close;
		len = sl->len;
		text = sl->text;
		sl->text = NULL;
		sl->has = false;
		memcpy(lang, sl->lang, sizeof lang);
		w->core.busy = true;
		ReleaseSRWLockExclusive(&w->core.lock);

		memset(&ans, 0, sizeof ans);
		ans.slot = (uint32_t)s;
		ans.gen = gen;
		ans.text_len = (uint32_t)len;
		memcpy(ans.lang, lang, sizeof ans.lang);
		if (close) {
			held_close(&held[s]);
		} else {
			colour(&held[s], lang, text, len, &ans);
		}
		free(text);

		AcquireSRWLockExclusive(&w->core.lock);
		w->core.busy = false;
		if (!close && (LONG)gen == sl->live && !w->stopping) {
			wf_synanswer_free(&sl->ans);
			sl->ans = ans;
			sl->has_ans = true;
			SetEvent(w->core.ready);
		} else {
			wf_synanswer_free(&ans);
			if (w->core.stats.discarded_results != UINT64_MAX) {
				w->core.stats.discarded_results++;
			}
		}
		if (w->core.stats.completed_requests != UINT64_MAX) {
			w->core.stats.completed_requests++;
		}
		ReleaseSRWLockExclusive(&w->core.lock);
	}
	for (int i = 0; i < WF_SYN_SLOTS; i++) {
		held_close(&held[i]);
	}
	wf_wc_release(&w->core, worker_free, w);
	return 0;
}

WfStatus wf_synworker_start(WfSynWorker **out)
{
	WfSynWorker *w;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	w = calloc(1, sizeof *w);
	if (!w) {
		return WF_E_NOMEM;
	}
	if (WF_OK != wf_wc_start(&w->core, thread_main, w)) {
		free(w);
		return WF_E_OS;
	}
	*out = w;
	return WF_OK;
}

WfWorkerStopReport wf_synworker_stop(WfSynWorker *w)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	InterlockedExchange(&w->stopping, 1);
	return wf_wc_stop(&w->core, STOP_WAIT_MS, false, worker_free, w);
}

HANDLE wf_synworker_event(const WfSynWorker *w)
{
	return w->core.ready;
}

bool wf_synworker_poll(WfSynWorker *w, WfSynAnswer *out)
{
	bool got = false;

	if (!w || !out) {
		return false;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	for (int i = 0; i < WF_SYN_SLOTS; i++) {
		if (w->slot[i].has_ans) {
			*out = w->slot[i].ans;
			memset(&w->slot[i].ans, 0, sizeof w->slot[i].ans);
			w->slot[i].has_ans = false;
			got = true;
			break;
		}
	}
	{
		bool more = false;

		for (int i = 0; i < WF_SYN_SLOTS; i++) {
			more |= w->slot[i].has_ans;
		}
		if (!more) {
			ResetEvent(w->core.ready);
		}
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	return got;
}

static WfStatus port_request(void *ctx,
        uint32_t slot,
        uint32_t gen,
        const char *lang,
        const char *text,
        size_t len)
{
	WfSynWorker *w = ctx;
	char *copy;
	Slot *sl;

	if (!w || slot >= WF_SYN_SLOTS || !gen || !lang || (!text && len) ||
	        strlen(lang) >= sizeof sl->lang) {
		return WF_E_INVAL;
	}
	copy = malloc(len ? len : 1u);
	if (!copy) {
		return WF_E_NOMEM;
	}
	if (len) {
		memcpy(copy, text, len);
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		free(copy);
		return WF_E_INVAL;
	}
	sl = &w->slot[slot];
	if (sl->has && w->core.stats.superseded_requests != UINT64_MAX) {
		w->core.stats.superseded_requests++;
	}
	free(sl->text);
	sl->text = copy;
	sl->len = len;
	sl->gen = gen;
	sl->close = false;
	strcpy_s(sl->lang, sizeof sl->lang, lang);
	sl->has = true;
	wf_synanswer_free(&sl->ans);
	sl->has_ans = false;
	InterlockedExchange(&sl->live, (LONG)gen);
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

static void port_close(void *ctx, uint32_t slot)
{
	WfSynWorker *w = ctx;
	Slot *sl;

	if (!w || slot >= WF_SYN_SLOTS) {
		return;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	sl = &w->slot[slot];
	free(sl->text);
	sl->text = NULL;
	sl->len = 0;
	sl->close = true;
	sl->gen = 0;
	sl->lang[0] = '\0';
	sl->has = true;
	wf_synanswer_free(&sl->ans);
	sl->has_ans = false;
	InterlockedExchange(&sl->live, 0);
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
}

static bool port_installed(void *ctx, const char *lang)
{
	(void)ctx;
	return wf_synhost_installed(wf_synlang_by_id(lang));
}

WfSynPort wf_synworker_port(WfSynWorker *w)
{
	WfSynPort p = {w, port_request, port_close, port_installed};

	return p;
}

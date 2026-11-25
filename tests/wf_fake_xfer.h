#ifndef WF_FAKE_XFER_H
#define WF_FAKE_XFER_H

#include "ui/wf_app.h"
#include "wf_fake_probe.h"

/*
 * A WfXferPort that runs the real capture/preflight code against a
 * FakeProbe. Synchronous by default (answers from inside the call);
 * with defer = 1 it only records the request id so tests control timing.
 */
typedef struct
{
	WfApp *app;
	WfProbeOps ops;
	int defer;
	uint32_t last_gen;
	int n_capture, n_preflight;
	size_t last_n; /* how many names the newest capture was asked for */
} FakeXfer;

static WfStatus fx_capture(void *ctx,
        uint32_t gen,
        WfTransferMode mode,
        const wchar_t *dir,
        const wchar_t *const *names,
        size_t n,
        size_t hidden)
{
	FakeXfer *x = ctx;
	WfTransferSlot s;
	WfStatus st;

	x->n_capture++;
	x->last_n = n;
	x->last_gen = gen;
	if (x->defer) {
		return WF_OK;
	}
	st = wf_xfer_capture(&x->ops, mode, dir, names, n, hidden, &s);
	wf_app_on_capture(x->app, gen, st, &s);
	wf_slot_free(&s);
	return WF_OK;
}

static WfStatus fx_preflight(void *ctx,
        uint32_t gen,
        const WfTransferSlot *slot,
        const wchar_t *dest)
{
	FakeXfer *x = ctx;
	WfPreflightReport rep;
	WfStatus st;

	x->n_preflight++;
	x->last_gen = gen;
	if (x->defer) {
		return WF_OK;
	}
	st = wf_preflight_run(
	        &x->ops, slot, dest, &WF_PREFLIGHT_DEFAULT, NULL, &rep);
	wf_app_on_preflight(x->app, gen, st, &rep);
	return WF_OK;
}

static WfXferPort fx_port(FakeXfer *x)
{
	WfXferPort p = {x, fx_capture, fx_preflight};

	return p;
}

/* An executor that only records how it was called. */
typedef struct
{
	int calls;
	int hydrate;
	size_t count;
	wchar_t dest[260];
	WfStatus result;
} FakeExec;

static WfStatus fe_start(void *ctx,
        const WfTransferSlot *slot,
        const wchar_t *dest,
        const WfPreflightReport *rep,
        bool hydrate)
{
	FakeExec *e = ctx;

	(void)rep;
	e->calls++;
	e->hydrate = hydrate;
	e->count = slot->count;
	wcscpy_s(e->dest, 260, dest);
	return e->result;
}

/* Not every test installs an executor. */
#if defined(__clang__)
__attribute__((unused))
#endif
static WfExecPort fe_port(FakeExec *e)
{
	WfExecPort p = {e, fe_start};

	return p;
}

#endif

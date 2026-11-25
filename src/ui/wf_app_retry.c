/*
 * Offering a job's failed items again. A copy or move that left items
 * undone keeps just those sources (and where they were going) in its job
 * log entry; "r" in the job log puts them back into the transfer slot and
 * opens the destination. Nothing starts by itself: pasting asks everything
 * again (the destination check, the policy for names already there, the
 * confirmation), exactly as for a first attempt.
 */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../path/wf_path.h"
#include "wf_app_priv.h"

#define RETRY_MAX 8

static void slot_entry_free(WfApp *app, size_t i)
{
	wf_slot_free(&app->retry_src[i].slot);
	free(app->retry_src[i].dest);
	app->retry_src[i].dest = NULL;
	app->retry_src_n--;
	/* the rest move down, so that the list stays oldest first: the oldest is the
	 * one given up when it is full */
	memmove(&app->retry_src[i], &app->retry_src[i + 1],
	        (app->retry_src_n - i) * sizeof app->retry_src[0]);
	memset(&app->retry_src[app->retry_src_n], 0, sizeof app->retry_src[0]);
}

void wf_app_retry_note(WfApp *app, uint32_t job, const WfCopySpec *spec)
{
	size_t at;

	if (!spec->dest ||
	        (WF_CJ_COPY != spec->kind && WF_CJ_MOVE_ACROSS != spec->kind)) {
		return;
	}
	if (RETRY_MAX == app->retry_src_n) { /* the oldest is given up */
		slot_entry_free(app, 0);
	}
	at = app->retry_src_n;
	memset(&app->retry_src[at], 0, sizeof app->retry_src[0]);
	wf_slot_init(&app->retry_src[at].slot);
	app->retry_src[at].dest = wf_path_dup(spec->dest);
	if (!app->retry_src[at].dest ||
	        WF_OK != wf_slot_clone(&app->retry_src[at].slot, &spec->slot)) {
		free(app->retry_src[at].dest);
		wf_slot_free(&app->retry_src[at].slot);
		memset(&app->retry_src[at], 0, sizeof app->retry_src[0]);
		return;
	}
	app->retry_src[at].job = job;
	app->retry_src[at].kind = spec->kind;
	app->retry_src_n++;
}

/* Where the part of `path` below `dir` begins (the first component), or NULL
 * when `path` is not below it. Case does not matter, as for names here. */
static const wchar_t *below_dir(const wchar_t *path, const wchar_t *dir)
{
	size_t dn = wcslen(dir), at = dn;

	if (_wcsnicmp(path, dir, dn)) {
		return NULL;
	}
	if (!(dn && L'\\' == dir[dn - 1])) {
		if (L'\\' != path[at]) {
			return NULL;
		}
		at++;
	}
	return path + at;
}

/* Does `path` lie in (or is) the source `name` of a slot in `dir`? */
bool wf_app_path_in_source(
        const wchar_t *path, const wchar_t *dir, const wchar_t *name)
{
	const wchar_t *rest = below_dir(path, dir);
	size_t nn = wcslen(name);

	if (!rest || _wcsnicmp(rest, name, nn)) {
		return false;
	}
	return 0 == rest[nn] || L'\\' == rest[nn];
}

/* The sources of a slot in the order of their names (case ignored), to find the
 * source a failed path is in without asking every source. */
typedef struct
{
	const wchar_t *name;
	size_t i;
} SrcName;

static int src_name_order(const void *a, const void *b)
{
	const SrcName *x = a, *y = b;
	int c = _wcsicmp(x->name, y->name);

	return c ? c : (x->i < y->i ? -1 : x->i > y->i);
}

typedef struct
{
	const wchar_t *p;
	size_t n;
} Component;

/* Orders a component against a source name as the names are ordered: by the
 * characters, case ignored, a shorter one first. */
static int component_vs_name(const void *key, const void *elem)
{
	const Component *k = key;
	const SrcName *e = elem;
	int c = _wcsnicmp(k->p, e->name, k->n);

	if (c) {
		return c;
	}
	return e->name[k->n] ? -1 : 0;
}

/* Called as the result of `r` is entered in the job log. */
void wf_app_retry_take(WfApp *app, const WfCopyResult *r, WfJobLogEntry *e)
{
	size_t i, at = app->retry_src_n, keep = 0;
	bool *failed;
	SrcName *by_name = NULL;
	WfTransferSlot *s;

	for (i = 0; i < app->retry_src_n; i++) {
		if (app->retry_src[i].job == r->job) {
			at = i;
			break;
		}
	}
	if (at == app->retry_src_n) {
		return;
	}
	if (0 == r->failure_total) {
		slot_entry_free(app, at);
		return;
	}
	s = &app->retry_src[at].slot;
	failed = calloc(s->count ? s->count : 1, sizeof *failed);
	by_name = malloc((s->count ? s->count : 1) * sizeof *by_name);
	if (!failed || !by_name) {
		free(failed);
		free(by_name);
		slot_entry_free(app, at);
		return;
	}
	/* a source is offered again when a failure lies in it; when the list of
	 * failures is not complete or names nothing, all of them are. Each failed
	 * path is looked for among the sources by the component below the folder. */
	if (r->fail_all_n && r->fail_all_n >= r->failure_total) {
		for (i = 0; i < s->count; i++) {
			by_name[i].name = s->src[i].name;
			by_name[i].i = i;
		}
		qsort(by_name, s->count, sizeof *by_name, src_name_order);
		for (size_t f = 0; f < r->fail_all_n; f++) {
			const wchar_t *rest =
			        below_dir(r->fail_all[f].path, s->dir);
			Component c;
			const SrcName *hit;

			if (!rest) {
				continue;
			}
			c.p = rest;
			c.n = wcscspn(rest, L"\\");
			hit = bsearch(&c, by_name, s->count, sizeof *by_name,
			        component_vs_name);
			if (!hit) {
				continue;
			}
			/* names that differ only in case are all the same source here.
			 * Going back is by index: a pointer one before the first element
			 * is not a pointer there may be, even unused. */
			for (size_t k = (size_t)(hit - by_name) + 1; k-- > 0 &&
			        0 == component_vs_name(&c, &by_name[k]);) {
				failed[by_name[k].i] = true;
			}
			for (const SrcName *x = hit + 1;
			        x < by_name + s->count &&
			        0 == component_vs_name(&c, x);
			        x++) {
				failed[x->i] = true;
			}
		}
	}
	free(by_name);
	for (i = 0; i < s->count; i++) {
		keep += failed[i];
	}
	if (0 == keep) {
		for (i = 0; i < s->count; i++) {
			failed[i] = true;
		}
		keep = s->count;
	}
	/* keep only those, in order */
	{
		size_t out = 0;

		for (i = 0; i < s->count; i++) {
			if (failed[i]) {
				s->src[out++] = s->src[i];
			} else {
				free(s->src[i].name);
			}
		}
		s->count = out;
	}
	free(failed);
	e->retry = *s;
	e->retry_dest = app->retry_src[at].dest;
	e->retry_kind = app->retry_src[at].kind;
	/* ownership moved to the entry */
	memset(&app->retry_src[at].slot, 0, sizeof app->retry_src[at].slot);
	app->retry_src[at].dest = NULL;
	slot_entry_free(app, at);
}

void wf_app_retry_free(WfApp *app)
{
	while (app->retry_src_n) {
		slot_entry_free(app, app->retry_src_n - 1);
	}
}

/* Puts `src` back in the transfer slot (as `r` in the job log does) and opens
 * `dest`. Nothing starts: pasting asks everything again. */
bool wf_app_retry_stage_slot(WfApp *app,
        const WfTransferSlot *src,
        const wchar_t *dest,
        WfCopyKind kind,
        WfMsg said)
{
	WfTransferSlot copy;
	wchar_t *d;
	char text[160];
	size_t n;

	wf_slot_init(&copy);
	if (WF_OK != wf_slot_clone(&copy, src)) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_NOMEM));
		return false;
	}
	d = wf_path_dup(dest);
	if (!d) {
		wf_slot_free(&copy);
		wf_app_set_msg(app, wf_msg(MSG_JOB_NOMEM));
		return false;
	}
	(void)wf_app_xfer_drop_slot(app);
	app->slot = copy;
	app->slot.mode =
	        WF_CJ_MOVE_ACROSS == kind ? WF_XFER_MOVE : WF_XFER_COPY;
	app->slot.collisions_ok = false;
	n = app->slot.count;
	app->mode = &WF_MODE_NORMAL;
	(void)wf_app_navigate(app, d, NULL, SIZE_MAX);
	free(d);
	(void)wf_msgf(text, sizeof text, said, &WF_ARG_N(n), 1);
	wf_app_set_msg(app, text);
	return true;
}

/* "r" in the job log: the newest job that has something to offer. */
void wf_app_retry_stage(WfApp *app)
{
	for (size_t k = app->jobs.n; k-- > 0;) {
		WfJobLogEntry *e = &app->jobs.log[k];

		if (0 == e->retry.count || !e->retry_dest) {
			continue;
		}
		(void)wf_app_retry_stage_slot(app, &e->retry, e->retry_dest,
		        e->retry_kind, MSG_JOBLOG_RETRY_STAGED);
		app->jobs.lines_ok = false; /* its retry line may be gone */
		return;
	}
	wf_app_set_msg(app, wf_msg(MSG_JOBLOG_RETRY_NONE));
}

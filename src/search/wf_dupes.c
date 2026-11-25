#include "wf_dupes.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
	const WfDupFile *f;
	uint8_t digest[32];
} Item;

typedef struct
{
	const WfDupeOps *ops;
	void *ctx;
	WfDupeStats *st;
	uint64_t total; /* files to read, for the progress */
	uint64_t done;
	bool stop;
	bool nomem; /* no memory for a list: the answer is WF_E_NOMEM */
} Run;

static bool cancelled(Run *r)
{
	if (r->stop) {
		return true;
	}
	if (r->ops->cancelled && r->ops->cancelled(r->ctx)) {
		r->st->cancelled = true;
		r->stop = true;
	}
	return r->stop;
}

static int by_size(const void *a, const void *b)
{
	const WfDupFile *x = *(const WfDupFile *const *)a;
	const WfDupFile *y = *(const WfDupFile *const *)b;

	if (x->size != y->size) {
		return x->size < y->size ? -1 : 1;
	}
	/* the order the files came in, so a group lists them nearest first */
	return x < y ? -1 : (x > y ? 1 : 0);
}

static int by_digest(const void *a, const void *b)
{
	const Item *x = a, *y = b;
	int c = memcmp(x->digest, y->digest, sizeof x->digest);

	if (c) {
		return c;
	}
	return x->f < y->f ? -1 : (x->f > y->f ? 1 : 0);
}

/* Reads `it` (limit bytes) into its digest. false: the file is left out. */
static bool read_one(Run *r, Item *it, uint64_t limit, WfDupePhase phase)
{
	WfStatus st;

	if (cancelled(r)) {
		return false;
	}
	st = r->ops->hash(r->ctx, it->f, limit, it->digest);
	r->st->hashed++;
	r->done++;
	if (r->ops->progress) {
		r->ops->progress(r->ctx, phase, r->done, r->total);
	}
	if (WF_OK != st) {
		r->st->skipped++;
		return false;
	}
	return true;
}

/* Emits each run of equal digests (two or more) of items[0..n) sorted by digest.
 * Every item in the range has a digest: the callers pack the files that could
 * be read into the front of the array and pass only that part. */
static void emit_runs(Run *r, Item *items, size_t n)
{
	size_t i = 0;
	const WfDupFile **out = NULL;

	if (n < 2) {
		return;
	}
	out = malloc(n * sizeof *out);
	if (!out) {
		r->stop = true;
		r->nomem = true;
		return;
	}
	while (i < n && !r->stop) {
		size_t j = i, k = 0;

		/* items[i] equals itself, so the run is never empty and i moves on */
		while (j < n &&
		        !memcmp(items[j].digest, items[i].digest,
		                sizeof items[i].digest)) {
			out[k++] = items[j].f;
			j++;
		}
		if (k >= 2) {
			r->st->groups++;
			r->st->files_in_groups += k;
			if (!r->ops->group(r->ctx, out, k)) {
				r->st->stopped = true;
				r->stop = true;
			}
		}
		i = j;
	}
	free(out);
}

/* Reads every item whole, and emits what is equal. */
static void whole(Run *r, Item *items, size_t n)
{
	size_t ok = 0;

	for (size_t i = 0; i < n; i++) {
		if (read_one(r, &items[i], 0, WF_DUPE_FULL_HASH)) {
			items[ok++] = items[i];
		} else if (r->stop) {
			return;
		}
	}
	qsort(items, ok, sizeof *items, by_digest);
	emit_runs(r, items, ok);
}

/* One size shared by `n` files. */
static void one_size(
        Run *r, const WfDupFile *const *file, size_t n, uint64_t size)
{
	Item *items = malloc(n * sizeof *items);
	size_t ok = 0, i = 0;

	if (!items) {
		r->stop = true;
		r->nomem = true;
		return;
	}
	for (size_t k = 0; k < n; k++) {
		memset(&items[k], 0, sizeof items[k]);
		items[k].f = file[k];
	}
	if (size <= WF_DUPE_PREFIX) { /* the prefix is the file */
		whole(r, items, n);
		free(items);
		return;
	}
	for (size_t k = 0; k < n; k++) {
		if (read_one(r, &items[k], WF_DUPE_PREFIX,
		            WF_DUPE_PREFIX_HASH)) {
			items[ok++] = items[k];
		} else if (r->stop) {
			free(items);
			return;
		}
	}
	qsort(items, ok, sizeof *items, by_digest);
	/* only the files whose first bytes are alike are read whole */
	while (i < ok && !r->stop) {
		size_t j = i;

		while (j < ok &&
		        !memcmp(items[j].digest, items[i].digest,
		                sizeof items[i].digest)) {
			j++;
		}
		if (j - i >= 2) {
			Item *run = &items[i];
			/* (the whole-file pass counts the files of this run again) */
			r->total += (uint64_t)(j - i);
			whole(r, run, j - i);
		}
		i = j;
	}
	free(items);
}

WfStatus wf_dupes_run(const WfDupFile *files,
        size_t n,
        const WfDupeOps *ops,
        void *ctx,
        WfDupeStats *stats)
{
	WfDupeStats local;
	Run r;
	const WfDupFile **idx;
	size_t i = 0;

	if (!ops || !ops->hash || !ops->group || (n && !files)) {
		return WF_E_INVAL;
	}
	if (!stats) {
		stats = &local;
	}
	memset(stats, 0, sizeof *stats);
	memset(&r, 0, sizeof r);
	r.ops = ops;
	r.ctx = ctx;
	r.st = stats;
	if (n < 2) {
		return WF_OK;
	}
	idx = malloc(n * sizeof *idx);
	if (!idx) {
		return WF_E_NOMEM;
	}
	for (size_t k = 0; k < n; k++) {
		idx[k] = &files[k];
	}
	qsort(idx, n, sizeof *idx, by_size);
	/* the files that have a twin by size are the ones to read */
	for (size_t a = 0; a < n;) {
		size_t b = a;

		while (b < n && idx[b]->size == idx[a]->size) {
			b++;
		}
		if (b - a >= 2 && idx[a]->size > 0) {
			stats->candidates += b - a;
		}
		a = b;
	}
	r.total = stats->candidates;
	if (ops->progress) {
		ops->progress(ctx, WF_DUPE_SIZES, 0, r.total);
	}
	while (i < n && !r.stop) {
		size_t j = i;

		while (j < n && idx[j]->size == idx[i]->size) {
			j++;
		}
		if (j - i >= 2 && idx[i]->size > 0) {
			one_size(&r, &idx[i], j - i, idx[i]->size);
		}
		i = j;
	}
	free(idx);
	return r.nomem ? WF_E_NOMEM : WF_OK;
}

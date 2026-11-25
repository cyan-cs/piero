#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"
#include "wf_search.h"

WfHitDir *wf_hitdir_new(const wchar_t *path, WfFileId id)
{
	WfHitDir *d = calloc(1, sizeof *d);

	if (!d) {
		return NULL;
	}
	d->path = wf_path_dup(path);
	if (!d->path) {
		free(d);
		return NULL;
	}
	d->id = id;
	d->refs = 1;
	return d;
}

WfHitDir *wf_hitdir_ref(WfHitDir *d)
{
	if (d) {
		InterlockedIncrement(&d->refs);
	}
	return d;
}

void wf_hitdir_unref(WfHitDir *d)
{
	if (d && 0 == InterlockedDecrement(&d->refs)) {
		free(d->path);
		free(d);
	}
}

void wf_hit_clear(WfHit *h)
{
	if (!h) {
		return;
	}
	free(h->name);
	wf_hitdir_unref(h->dir);
	memset(h, 0, sizeof *h);
}

void wf_search_batch_free(WfSearchBatch *b)
{
	if (!b) {
		return;
	}
	for (size_t i = 0; i < b->n; i++) {
		wf_hit_clear(&b->hit[i]);
	}
	free(b->hit);
	memset(b, 0, sizeof *b);
}

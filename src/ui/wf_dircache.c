#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "wf_dircache.h"
#include "wf_app_priv.h"
#include "../path/wf_path.h"

size_t wf_app_dircache_bytes(const WfApp *app)
{
	const struct WfDirCache *c = app->dircache;
	if (!c) {
		return 0;
	}
	const WfDir *displayed = app->dirprev.dir;
	size_t n = sizeof *c;
	if (displayed) {
		n += displayed->owned_bytes;
		if (app->dirprev.vis) {
			n += displayed->n * sizeof(size_t);
		}
	}
	for (size_t i = 0; i < WF_DIRCACHE_SLOTS; i++) {
		const WfDirCacheEntry *e = &c->entry[i];
		if (e->path) {
			n += (wcslen(e->path) + 1) * sizeof(wchar_t);
		}
		if (e->dir) {
			n += e->dir->owned_bytes;
		}
	}
	return n;
}

static void drop(WfApp *app, size_t i)
{
	struct WfDirCache *c = app->dircache;
	if (c->entry[i].dir) {
		c->evicted++;
	}
	wf_dir_free(c->entry[i].dir);
	free(c->entry[i].path);
	memset(&c->entry[i], 0, sizeof c->entry[i]);
	if (c->flight && c->flight_slot == i) {
		c->flight = 0;
	}
}

static void trim(WfApp *app, size_t extra)
{
	struct WfDirCache *c = app->dircache;
	for (size_t n = c->count; n &&
	        (extra > WF_DIRCACHE_BYTES ||
	                wf_app_dircache_bytes(app) > WF_DIRCACHE_BYTES - extra);
	        n--) {
		WfDirCacheEntry *e = &c->entry[c->order[n - 1]];
		if (e->dir) {
			wf_dir_free(e->dir);
			e->dir = NULL;
			c->evicted++;
		}
	}
	size_t bytes = wf_app_dircache_bytes(app);
	if (bytes > c->peak_bytes) {
		c->peak_bytes = bytes;
	}
}

void wf_app_dircache_displayed(WfApp *app)
{
	if (!app->dircache) {
		return;
	}
	trim(app, 0);
}

void wf_app_dircache_invalidate(WfApp *app)
{
	struct WfDirCache *c = app->dircache;
	if (!c) {
		return;
	}
	for (size_t i = 0; i < WF_DIRCACHE_SLOTS; i++) {
		drop(app, i);
	}
	c->count = 0;
	c->parent.kind = WF_ID_NONE;
	++app->dirprev_gen;
	if (app->dirprev_state == WF_DIRPREV_READY ||
	        app->dirprev_state == WF_DIRPREV_LOADING) {
		app->dirprev_state = WF_DIRPREV_WAIT;
		app->dirprev_due = app->now_ms + WF_DIRPREV_DEBOUNCE_MS;
	}
}

void wf_app_dircache_destroy(WfApp *app)
{
	wf_app_dircache_invalidate(app);
	free(app->dircache);
	app->dircache = NULL;
}

void wf_app_dircache_enable(WfApp *app, bool enabled)
{
	if (!enabled) {
		wf_app_dircache_destroy(app);
		return;
	}
	if (app->dircache) {
		return;
	}
	app->dircache = calloc(1, sizeof *app->dircache);
	if (!app->dircache) {
		return;
	}
	app->dircache->direction = 1;
	if (app->preview_flags == WF_ENT_DIR) {
		wf_app_preview_sync(app, true);
	}
}

/* Invalidate speculative observations without disturbing the selected
 * snapshot or its in-flight request. The next selection rebuilds the window. */
void wf_app_dircache_forget_neighbours(WfApp *app)
{
	struct WfDirCache *c = app->dircache;
	if (!c || !c->count) {
		return;
	}
	if (!(app->preview_flags & WF_ENT_DIR) || !app->preview_path ||
	        !c->entry[c->current].path ||
	        wcscmp(app->preview_path, c->entry[c->current].path)) {
		wf_app_dircache_invalidate(app);
		return;
	}
	for (size_t i = 0; i < WF_DIRCACHE_SLOTS; i++) {
		if (i != c->current) {
			drop(app, i);
		}
	}
	c->order[0] = c->current;
	c->count = 1;
}

void wf_app_dircache_return(WfApp *app)
{
	struct WfDirCache *c = app->dircache;
	if (!c) {
		return;
	}
	WfDir *d = app->dirprev.dir;
	if (!d || !c->count || !app->preview_path) {
		return;
	}
	WfDirCacheEntry *e = &c->entry[c->current];
	if (!e->path || wcscmp(e->path, app->preview_path) ||
	        !wf_fileid_equal(&e->id, &d->identity) || d->truncated ||
	        app->now_ms - e->loaded > WF_DIRCACHE_TTL) {
		return;
	}
	app->dirprev.dir = NULL;
	trim(app, d->owned_bytes);
	if (d->owned_bytes <= WF_DIRCACHE_BYTES &&
	        wf_app_dircache_bytes(app) <=
	                WF_DIRCACHE_BYTES - d->owned_bytes) {
		wf_dir_free(e->dir);
		e->dir = d;
		trim(app, 0);
	} else {
		c->rejected++;
		wf_dir_free(d);
	}
}

static bool eligible(const WfEntry *e)
{
	return (e->flags & (WF_ENT_DIR | WF_ENT_LINK | WF_ENT_CLOUD)) ==
	        WF_ENT_DIR &&
	        (e->identity.kind == WF_ID_STRONG);
}

static void schedule(WfApp *app)
{
	struct WfDirCache *c = app->dircache;
	if (!c->count || c->flight || !app->dirprev_port.submit ||
	        app->want_path || app->dirprev_state != WF_DIRPREV_READY ||
	        wf_app_dircache_bytes(app) > WF_DIRCACHE_BYTES - (256u << 10)) {
		return;
	}
	for (size_t n = 1; n < c->count; n++) {
		size_t i = c->order[n];
		WfDirCacheEntry *e = &c->entry[i];
		if (e->dir || e->attempted) {
			continue;
		}
		e->attempted = true;
		uint32_t gen = ++app->dirprev_gen;
		if (app->dirprev_port.submit(app->dirprev_port.ctx, gen,
		            e->path, NULL) == WF_OK) {
			c->flight = gen;
			c->flight_slot = i;
			c->submitted++;
			c->prefetched++;
			app->preview_stats.directory_requests++;
			return;
		}
		c->rejected++;
	}
}

bool wf_app_dircache_select(WfApp *app)
{
	struct WfDirCache *c = app->dircache;
	const WfView *v = &app->cur;
	if (!c || !v->dir || !(v->dir->identity.kind == WF_ID_STRONG) ||
	        !v->nvis || !eligible(&v->dir->e[v->vis[v->cursor]]) ||
	        !wf_preview_dir_auto_allowed(
	                app->preview_path, app->preview_flags)) {
		return false;
	}
	const WfEntry *wanted[WF_DIRCACHE_SLOTS];
	size_t count = 1;
	wanted[0] = &v->dir->e[v->vis[v->cursor]];
	WfSortKey sort =
	        app->config ? (WfSortKey)app->config->sort_by : WF_SORT_NAME;
	bool descending = app->config && app->config->sort_descending;
	bool dirs_first = !app->config || app->config->directories_first;
	if (!wf_fileid_equal(&c->parent, &v->dir->identity) ||
	        c->hidden != v->show_hidden || wcscmp(c->filter, v->filter) ||
	        c->sort != sort || c->descending != descending ||
	        c->dirs_first != dirs_first) {
		wf_app_dircache_invalidate(app);
		c->cursor = v->cursor;
		c->direction = 1;
	} else if (v->cursor != c->cursor) {
		c->direction = v->cursor > c->cursor ? 1 : -1;
	}
	c->parent = v->dir->identity;
	c->cursor = v->cursor;
	c->hidden = v->show_hidden;
	wcscpy_s(c->filter, WF_FILTER_MAX, v->filter);
	c->sort = sort;
	c->descending = descending;
	c->dirs_first = dirs_first;
	for (unsigned side = 0; side < 2; side++) {
		int direction = side ? -c->direction : c->direction;
		size_t limit = side ? 3 : 5, kept = 0;
		for (size_t at = v->cursor; kept < limit;) {
			if (direction > 0) {
				if (++at >= v->nvis) {
					break;
				}
			} else {
				if (!at) {
					break;
				}
				at--;
			}
			const WfEntry *e = &v->dir->e[v->vis[at]];
			if (eligible(e)) {
				wanted[count++] = e;
				kept++;
			}
		}
	}
	bool keep[WF_DIRCACHE_SLOTS] = {0};
	for (size_t i = 0; i < WF_DIRCACHE_SLOTS; i++) {
		WfDirCacheEntry *e = &c->entry[i];
		for (size_t n = 0; e->path && n < count; n++) {
			if (wf_fileid_equal(&e->id, &wanted[n]->identity)) {
				keep[i] = true;
				break;
			}
		}
		if (!keep[i]) {
			drop(app, i);
		}
	}
	c->count = 0;
	for (size_t n = 0; n < count; n++) {
		size_t i;
		for (i = 0; i < WF_DIRCACHE_SLOTS; i++) {
			if (c->entry[i].path &&
			        wf_fileid_equal(&c->entry[i].id,
			                &wanted[n]->identity)) {
				break;
			}
		}
		if (i == WF_DIRCACHE_SLOTS) {
			for (i = 0; i < WF_DIRCACHE_SLOTS && c->entry[i].path;
			        i++) {
			}
			if (i == WF_DIRCACHE_SLOTS ||
			        wf_path_join(app->cwd, wanted[n]->name,
			                &c->entry[i].path) != WF_OK) {
				continue;
			}
			c->entry[i].id = wanted[n]->identity;
		}
		if (!n) {
			c->current = i;
		}
		c->order[c->count++] = i;
	}
	if (!c->count ||
	        !wf_fileid_equal(
	                &c->entry[c->current].id, &wanted[0]->identity)) {
		c->count = 0;
		return false;
	}
	trim(app, 0);
	WfDirCacheEntry *e = &c->entry[c->current];
	if (e->dir && app->now_ms - e->loaded <= WF_DIRCACHE_TTL) {
		WfDir *d = e->dir;
		e->dir = NULL;
		c->hits++;
		wf_app_dirpreview_show(app, WF_OK, d);
		schedule(app);
		return true;
	}
	wf_dir_free(e->dir);
	e->dir = NULL;
	c->misses++;
	if (c->flight && c->flight_slot == c->current) {
		app->dirprev_state = WF_DIRPREV_LOADING;
		app->dirprev_due = 0;
		return true;
	}
	return false;
}

bool wf_app_dircache_submit(WfApp *app)
{
	struct WfDirCache *c = app->dircache;
	if (!c || !c->count || !app->preview_path ||
	        wcscmp(c->entry[c->current].path, app->preview_path)) {
		return false;
	}
	c->flight = ++app->dirprev_gen;
	c->flight_slot = c->current;
	c->entry[c->current].attempted = true;
	c->submitted++;
	app->preview_stats.directory_requests++;
	app->dirprev_state = WF_DIRPREV_LOADING;
	app->dirprev_due = 0;
	WfStatus st = app->dirprev_port.submit(
	        app->dirprev_port.ctx, c->flight, app->preview_path, NULL);
	if (st != WF_OK) {
		c->flight = 0;
		wf_app_dirpreview_show(app, st, NULL);
	}
	return true;
}

bool wf_app_dircache_result(WfApp *app, uint32_t gen, WfStatus st, WfDir *dir)
{
	struct WfDirCache *c = app->dircache;
	if (!c || !c->flight || gen != c->flight) {
		return false;
	}
	size_t i = c->flight_slot;
	c->flight = 0;
	WfDirCacheEntry *e = &c->entry[i];
	if (st == WF_OK && (!dir || !wf_fileid_equal(&e->id, &dir->identity))) {
		st = WF_E_ACCESS;
	}
	if (i == c->current && app->preview_path &&
	        !wcscmp(e->path, app->preview_path)) {
		e->loaded = app->now_ms;
		wf_app_dirpreview_show(app, st, dir);
	} else {
		if (st != WF_OK || !dir || dir->truncated) {
			wf_dir_free(dir);
			c->rejected++;
		} else {
			trim(app, dir->owned_bytes);
			if (dir->owned_bytes <= WF_DIRCACHE_BYTES &&
			        wf_app_dircache_bytes(app) <=
			                WF_DIRCACHE_BYTES - dir->owned_bytes) {
				e->dir = dir;
				e->loaded = app->now_ms;
				trim(app, 0);
			} else {
				wf_dir_free(dir);
				c->rejected++;
			}
		}
	}
	schedule(app);
	return true;
}

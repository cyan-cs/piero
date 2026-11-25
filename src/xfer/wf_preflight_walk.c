#include <stdlib.h>

#include "../path/wf_path.h"
#include "wf_preflight_internal.h"
#include "base/wf_macro.h"

typedef struct
{
	wchar_t *path;
	WfDir *dir;
	size_t next;
	size_t rel_len; /* length of the path relative to the destination, of
	                   this dir */
} Frame;

static void frames_free(Frame *f, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		free(f[i].path);
		wf_dir_free(f[i].dir);
	}
	free(f);
}

/* Accounts one tree entry (not the top-level item). Returns a refusal
 * reason, or WF_PF_OK. */
static WfPfReason account(WfPfWalk *w, const WfEntry *e, size_t rel_len)
{
	WfPreflightReport *rep = w->rep;
	WfTransferPaths paths;

	if (wcslen(e->name) > w->max_component) {
		return WF_PF_COMPONENT_TOO_LONG;
	}
	if (wf_xfer_path_lengths(w->dest_ext_len, rel_len, w->top_name_units,
	            &paths) != WF_OK) {
		return WF_PF_PATH_TOO_LONG;
	}
	if (paths.final_units > rep->longest_final) {
		rep->longest_final = paths.final_units;
	}
	if (paths.transaction_units > rep->longest_txn) {
		rep->longest_txn = paths.transaction_units;
	}
	if (paths.final_units > rep->max_path_units ||
	        paths.transaction_units > rep->max_path_units) {
		return WF_PF_PATH_TOO_LONG;
	}

	if (e->flags & WF_ENT_LINK) {
		/* Only the fixed transparent allowlist may be copied. */
		if ((e->flags & WF_ENT_DIR) ||
		        WF_REPARSE_TRANSPARENT !=
		                wf_reparse_classify(e->flags, e->reparse_tag)) {
			return WF_PF_REPARSE;
		}
		rep->tree_links++;
	}
	if (e->flags & WF_ENT_DIR) {
		rep->tree_dirs++;
		return WF_PF_OK;
	}
	rep->tree_files++;
	if (rep->total_bytes > UINT64_MAX - e->size) {
		return WF_PF_TREE_TOO_LARGE;
	}
	rep->total_bytes += e->size;
	if (e->flags & WF_ENT_CLOUD) {
		rep->cloud_files++;
		rep->cloud_bytes +=
		        e->size; /* <= total_bytes, which did not overflow */
	}
	return WF_PF_OK;
}

/* Room for one more frame; the stack doubles and the new tail is zeroed so
 * frames_free() can release a partly filled stack. */
static WfStatus stack_reserve(Frame **stack, size_t *cap, size_t depth)
{
	Frame *grown;

	if (depth < *cap) {
		return WF_OK;
	}
	if (*cap > SIZE_MAX / 2 / sizeof *grown) {
		return WF_E_RANGE;
	}
	grown = realloc(*stack, *cap * 2 * sizeof *grown);
	if (!grown) {
		return WF_E_NOMEM;
	}
	*stack = grown;
	*cap *= 2;
	memset(grown + depth, 0, (*cap - depth) * sizeof *grown);
	return WF_OK;
}

/* Pushes a listed directory. Takes ownership of `path` and `dir`. The caller
 * has reserved room. */
static void frame_push(
        Frame *stack, size_t *depth, wchar_t *path, WfDir *dir, size_t rel_len)
{
	Frame *f = &stack[(*depth)++];

	f->path = path;
	f->dir = dir;
	f->next = 0;
	f->rel_len = rel_len;
}

/* Releases the finished top frame; false when it was the last one. */
static bool frame_pop(Frame *stack, size_t *depth)
{
	Frame *f = &stack[*depth - 1];

	free(f->path);
	wf_dir_free(f->dir);
	return --*depth != 0;
}

/* Walks the directory `top` (a top-level source, already counted). Uses an
 * explicit stack: directory depth is attacker-controlled and can reach
 * thousands of levels. */
static WfStatus walk_dir(WfPfWalk *w,
        const wchar_t *top,
        size_t top_name_len,
        size_t index,
        const wchar_t *top_name)
{
	WfPreflightReport *rep = w->rep;
	Frame *stack = NULL;
	size_t depth = 0, cap = 0;
	WfStatus st = WF_OK;
	WfPfReason bad = WF_PF_OK;
	wchar_t bad_entry[64] = L"";

	for (;;) {
		Frame *f;
		const WfEntry *e;
		wchar_t *child;
		size_t child_rel, name_units;

		if (0 == depth) { /* push the root */
			WfDir *d = NULL;

			st = w->ops->list(w->ops->ctx, top, &d);
			if (WF_OK != st) {
				bad = WF_PF_IO_ERROR;
				st = WF_OK;
				goto done;
			}
			cap = 16;
			stack = calloc(cap, sizeof *stack);
			if (!stack) {
				wf_dir_free(d);
				return WF_E_NOMEM;
			}
			frame_push(stack, &depth, wf_path_dup(top), d,
			        top_name_len);
			if (!stack[0].path) {
				frames_free(stack, 1);
				return WF_E_NOMEM;
			}
			if (d->truncated) {
				bad = WF_PF_TREE_TOO_LARGE;
				goto done;
			}
		}

		if (wf_pf_cancelled(w->cancel)) {
			bad = WF_PF_CANCELLED;
			goto done;
		}
		f = &stack[depth - 1];
		if (f->next >= f->dir->n) { /* directory finished: pop */
			if (!frame_pop(stack, &depth)) {
				free(stack);
				return WF_OK;
			}
			continue;
		}

		e = &f->dir->e[f->next++];
		name_units = wcslen(e->name);
		if (SIZE_MAX == f->rel_len ||
		        name_units > SIZE_MAX - f->rel_len - 1) {
			bad = WF_PF_PATH_TOO_LONG;
			goto done;
		}
		child_rel = f->rel_len + 1 + name_units;
		bad = account(w, e, child_rel);
		if (WF_PF_OK != bad) {
			wf_pf_copy_w(
			        bad_entry, WF_ARRAY_LEN(bad_entry), e->name);
			goto done;
		}
		if (!(e->flags & WF_ENT_DIR)) {
			continue;
		}

		if (WF_OK != wf_path_join(f->path, e->name, &child)) {
			st = WF_E_NOMEM;
			goto done;
		}
		st = stack_reserve(&stack, &cap, depth);
		if (WF_OK != st) {
			free(child);
			goto done;
		}
		{
			WfDir *d = NULL;
			WfStatus ls = w->ops->list(w->ops->ctx, child, &d);

			if (WF_OK != ls) { /* unreadable subtree: cannot promise
			                      a copy */
				free(child);
				bad = WF_PF_IO_ERROR;
				wf_pf_copy_w(bad_entry, WF_ARRAY_LEN(bad_entry),
				        e->name);
				goto done;
			}
			frame_push(stack, &depth, child, d,
			        child_rel); /* f may have moved in realloc */
			if (d->truncated) {
				bad = WF_PF_TREE_TOO_LARGE;
				wf_pf_copy_w(bad_entry, WF_ARRAY_LEN(bad_entry),
				        e->name);
				goto done;
			}
		}
	}
done:
	frames_free(stack, depth);
	if (WF_OK != st) {
		return st;
	}
	rep->reason = bad;
	wf_pf_set_bad(rep, index, bad_entry[0] ? bad_entry : top_name);
	return WF_OK;
}

WfStatus wf_pf_walk_sources(const WfTransferSlot *slot, WfPfWalk *w)
{
	WfPreflightReport *rep = w->rep;
	size_t i;
	WfStatus st;

	for (i = 0; i < slot->count; i++) {
		const WfSourceSnap *s = &slot->src[i];
		size_t nl = wcslen(s->name);
		w->top_name_units = nl;

		if (!(s->snap.flags & WF_ENT_DIR)) {
			rep->tree_files++;
			if (rep->total_bytes > UINT64_MAX - s->snap.size) {
				return wf_pf_verdict(
				        rep, WF_PF_TREE_TOO_LARGE, i, s->name);
			}
			rep->total_bytes += s->snap.size;
			if (s->snap.flags & WF_ENT_CLOUD) {
				rep->cloud_files++;
				rep->cloud_bytes += s->snap.size;
			}
			if (s->snap.flags & WF_ENT_LINK) {
				rep->tree_links++;
			}
			continue;
		}
		rep->tree_dirs++;
		{
			wchar_t *full;

			if (WF_OK != wf_path_join(slot->dir, s->name, &full)) {
				return WF_E_NOMEM;
			}
			st = walk_dir(w, full, nl, i, s->name);
			free(full);
			if (WF_OK != st) {
				return st;
			}
			if (WF_PF_OK != rep->reason) {
				return WF_OK;
			}
		}
	}

	return WF_OK;
}

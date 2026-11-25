#include <stdlib.h>
#include <wctype.h>

#include "../path/wf_path.h"
#include "wf_preflight_internal.h"

static wchar_t fold(wchar_t c)
{
	return (wchar_t)towlower((wint_t)c);
}

/* Path equality for display-level comparisons: case-insensitive, trailing
 * backslashes ignored. Never used as ownership proof. */
static size_t trimmed_len(const wchar_t *p)
{
	size_t n = wcslen(p);

	while (n > 0 && L'\\' == p[n - 1]) {
		n--;
	}
	return n;
}

static bool path_eq_ci(const wchar_t *a, const wchar_t *b)
{
	size_t na = trimmed_len(a), nb = trimmed_len(b), i;

	if (na != nb) {
		return false;
	}
	for (i = 0; i < na; i++) {
		if (fold(a[i]) != fold(b[i])) {
			return false;
		}
	}
	return true;
}

/* dest is the same as, or below, src. */
static bool path_within_ci(const wchar_t *dest, const wchar_t *src)
{
	size_t ns = trimmed_len(src), nd = trimmed_len(dest), i;

	if (nd < ns) {
		return false;
	}
	for (i = 0; i < ns; i++) {
		if (fold(dest[i]) != fold(src[i])) {
			return false;
		}
	}
	return nd == ns || L'\\' == dest[ns] ||
	        (ns > 0 && L'\\' == src[ns - 1]);
}

static bool names_eq_ci(const wchar_t *a, const wchar_t *b)
{
	for (; *a && fold(*a) == fold(*b); a++, b++) {
	}
	return *a == *b;
}

/* Units the \\?\ form adds to a Win32 path. */
static size_t extended_prefix(const wchar_t *p)
{
	if (0 == wcsncmp(p, L"\\\\?\\", 4)) {
		return 0;
	}
	return 0 == wcsncmp(p, L"\\\\", 2) ? 6 : 4; /* \\?\UNC\ replaces \\ */
}

/*
 * Looks at `dest` and every ancestor, each as the object itself (lstat):
 * NONE if no component is a reparse point, FOUND if one is, UNKNOWN if one
 * could not be examined (unknown is not "clean"). `leaf` receives the offending
 * component's name.
 */
WfPfLink wf_pf_dest_path_link(const WfProbeOps *ops,
        const wchar_t *dest,
        wchar_t *leaf,
        size_t leaf_cap)
{
	wchar_t *cur;
	WfPfLink verdict = WF_PF_LINK_NONE;

	if (!ops || !dest || !leaf || 0 == leaf_cap) {
		return WF_PF_LINK_UNKNOWN;
	}
	cur = wf_path_dup(dest);
	if (!cur) {
		return WF_PF_LINK_UNKNOWN;
	}
	for (;;) {
		WfNodeInfo n;
		size_t plen;

		if (WF_OK != ops->node(ops->ctx, cur, &n)) {
			verdict = WF_PF_LINK_UNKNOWN;
		} else if (n.flags & WF_ENT_LINK) {
			verdict = WF_PF_LINK_FOUND;
		}
		if (WF_PF_LINK_NONE != verdict) {
			const wchar_t *name;
			size_t len;

			wf_path_leaf(cur, &name, &len);
			if (len >= leaf_cap) {
				len = leaf_cap - 1;
			}
			memcpy(leaf, name, len * sizeof *leaf);
			leaf[len] = L'\0';
			break;
		}
		plen = wf_path_parent_len(cur);
		if (0 == plen) {
			break;
		}
		cur[plen] = L'\0';
	}
	free(cur);
	return verdict;
}

bool wf_pf_volumes_differ(const WfNodeInfo *src,
        const WfVolInfo *sv,
        const WfNodeInfo *dst,
        const WfVolInfo *dv)
{
	if (((src->id.kind == WF_ID_STRONG) &&
	            (dst->id.kind == WF_ID_STRONG)) ||
	        (wf_fileid_is_weak(&src->id) && wf_fileid_is_weak(&dst->id))) {
		return src->id.volume != dst->id.volume;
	}
	return sv->serial != dv->serial || !names_eq_ci(sv->root, dv->root);
}

/* Is any ancestor-or-self of `dest` one of the sources? Each path prefix is
 * resolved through links and compared by file ID, so reaching a source via a
 * junction alias is caught; path text is only a fallback for volumes
 * without file IDs. */
WfStatus wf_pf_dest_inside_source(const WfProbeOps *ops,
        const WfTransferSlot *slot,
        const wchar_t *dest,
        size_t *which,
        bool *inside)
{
	wchar_t *cur;
	size_t i, plen;

	if (!ops || !slot || !dest || !which || !inside) {
		return WF_E_INVAL;
	}
	*inside = false;
	cur = wf_path_dup(dest);
	if (!cur) { /* the ID-based check cannot run: never fall back to text
	              alone */
		return WF_E_NOMEM;
	}
	for (;;) {
		WfFileId id;

		if (WF_OK == ops->resolve_id(ops->ctx, cur, &id)) {
			for (i = 0; i < slot->count; i++) {
				if ((slot->src[i].snap.flags & WF_ENT_DIR) &&
				        !(slot->src[i].snap.flags &
				                WF_ENT_LINK) &&
				        wf_fileid_equal(
				                &id, &slot->src[i].snap.id)) {
					free(cur);
					*which = i;
					*inside = true;
					return WF_OK;
				}
			}
		}
		plen = wf_path_parent_len(cur);
		if (0 == plen) {
			break;
		}
		cur[plen] = L'\0';
	}
	free(cur);

	for (i = 0; i < slot->count; i++) {
		wchar_t *full;
		bool under;

		if (!(slot->src[i].snap.flags & WF_ENT_DIR)) {
			continue;
		}
		if (WF_OK !=
		        wf_path_join(slot->dir, slot->src[i].name, &full)) {
			return WF_E_NOMEM;
		}
		under = path_within_ci(dest, full);
		free(full);
		if (under) {
			*which = i;
			*inside = true;
			return WF_OK;
		}
	}
	return WF_OK;
}

bool wf_pf_same_folder(
        const WfProbeOps *ops, const WfTransferSlot *slot, const wchar_t *dest)
{
	WfFileId dest_real, dir_real;

	return path_eq_ci(dest, slot->dir) ||
	        (WF_OK == ops->resolve_id(ops->ctx, dest, &dest_real) &&
	                WF_OK ==
	                        ops->resolve_id(
	                                ops->ctx, slot->dir, &dir_real) &&
	                wf_fileid_equal(&dest_real, &dir_real));
}

WfStatus wf_pf_dest_units(const wchar_t *dest, size_t *units)
{
	if (trimmed_len(dest) > SIZE_MAX - extended_prefix(dest)) {
		return WF_E_RANGE;
	}
	*units = trimmed_len(dest) + extended_prefix(dest);
	return WF_OK;
}

/* The sources in the order of their names, as names_eq_ci sees them (case
 * folded unit by unit), and among equal names in the order of the slot. */
typedef struct
{
	const wchar_t *name;
	size_t i;
} Named;

static int named_order(const void *a, const void *b)
{
	const Named *x = a, *y = b;
	const wchar_t *p = x->name, *q = y->name;

	for (; *p && fold(*p) == fold(*q); p++, q++) {
	}
	if (fold(*p) != fold(*q)) {
		return fold(*p) < fold(*q) ? -1 : 1;
	}
	return x->i < y->i ? -1 : x->i > y->i;
}

/* The first source (by its place in the slot) whose name an earlier one has
 * already: SIZE_MAX if there is none. The equal names are next to each other
 * once sorted, so the second of each run is the one that collides; the least
 * of those is the first that the slot order would meet. */
static WfStatus first_name_collision(const WfTransferSlot *slot, size_t *first)
{
	Named *by_name;

	*first = SIZE_MAX;
	if (slot->count < 2) {
		return WF_OK;
	}
	by_name = malloc(slot->count * sizeof *by_name);
	if (!by_name) {
		return WF_E_NOMEM;
	}
	for (size_t i = 0; i < slot->count; i++) {
		by_name[i].name = slot->src[i].name;
		by_name[i].i = i;
	}
	qsort(by_name, slot->count, sizeof *by_name, named_order);
	for (size_t k = 1; k < slot->count; k++) {
		if (names_eq_ci(by_name[k - 1].name, by_name[k].name) &&
		        by_name[k].i < *first) {
			*first =
			        by_name[k]
			                .i; /* the second of a run, or a later one */
		}
	}
	free(by_name);
	return WF_OK;
}

WfStatus wf_pf_check_collisions(
        const WfTransferSlot *slot, const wchar_t *dest, bool copy, WfPfWalk *w)
{
	const WfProbeOps *ops = w->ops;
	const WfCancel *cancel = w->cancel;
	WfPreflightReport *rep = w->rep;
	size_t i, twin;
	WfStatus st;

	st = first_name_collision(slot, &twin);
	if (WF_OK != st) {
		return st;
	}
	for (i = 0; i < slot->count; i++) {
		const wchar_t *name = slot->src[i].name;
		WfNodeInfo existing;
		wchar_t *target;

		if (wf_pf_cancelled(cancel)) {
			return wf_pf_verdict(rep, WF_PF_CANCELLED, i, name);
		}
		if (wcslen(name) > w->max_component) {
			return wf_pf_verdict(
			        rep, WF_PF_COMPONENT_TOO_LONG, i, name);
		}
		if (i == twin) { /* two sources that would land on one name */
			return wf_pf_verdict(rep, WF_PF_COLLISION, i, name);
		}
		if (copy) {
			WfTransferPaths paths;
			if (wf_xfer_path_lengths(w->dest_ext_len, wcslen(name),
			            wcslen(name), &paths) != WF_OK) {
				return wf_pf_verdict(
				        rep, WF_PF_PATH_TOO_LONG, i, name);
			}
			if (paths.final_units > rep->longest_final) {
				rep->longest_final = paths.final_units;
			}
			if (paths.transaction_units > rep->longest_txn) {
				rep->longest_txn = paths.transaction_units;
			}
			if (paths.final_units > rep->max_path_units ||
			        paths.transaction_units > rep->max_path_units) {
				return wf_pf_verdict(
				        rep, WF_PF_PATH_TOO_LONG, i, name);
			}
		}
		if (WF_OK != wf_path_join(dest, name, &target)) {
			return WF_E_NOMEM;
		}
		st = ops->node(ops->ctx, target, &existing);
		free(target);
		if (WF_OK == st && w->collisions_ok) {
			continue; /* accepted: the executor skips or replaces */
		}
		if (WF_OK == st) {
			return wf_pf_verdict(rep, WF_PF_COLLISION, i, name);
		}
		if (WF_E_NOENT != st) { /* unknown is not "absent" */
			return wf_pf_verdict(rep, WF_PF_IO_ERROR, i, name);
		}
	}

	return WF_OK;
}

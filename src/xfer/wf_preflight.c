#include <stdlib.h>

#include "../path/wf_path.h"
#include "wf_preflight_internal.h"
#include "base/wf_macro.h"

/* PROVISIONAL (see WfPreflightPolicy). Generous on purpose: running out of
 * space mid-copy is the failure this margin exists to prevent. */
#define PROVISIONAL_RESERVE_BYTES (256ull * 1024 * 1024)
/* Nominal Win32 policy ceiling, not a promise that an API/volume can reach it.
 * Prefix expansion and individual APIs can impose a smaller measured range. */
#define EXTENDED_PATH_LIMIT 32766u

const WfPreflightPolicy WF_PREFLIGHT_DEFAULT = {
        PROVISIONAL_RESERVE_BYTES,
        EXTENDED_PATH_LIMIT,
};

const char *wf_pf_reason_text(WfPfReason r)
{
	switch (r) {
	case WF_PF_OK:             return "ok";
	case WF_PF_CROSS_VOLUME:   return "different volume";
	case WF_PF_EMPTY_SLOT:     return "transfer slot is empty";
	case WF_PF_SOURCE_MISSING: return "source no longer exists";
	case WF_PF_SOURCE_CHANGED: return "source changed since it was marked";
	case WF_PF_SAME_FOLDER:
		return "same folder; copy elsewhere, then rename";
	case WF_PF_COLLISION: return "destination already exists";
	case WF_PF_DEST_INSIDE_SOURCE:
		return "destination is inside the source";
	case WF_PF_DEST_NOT_DIR: return "destination is not a folder";
	case WF_PF_DEST_REPARSE:
		return "link on the destination path (not verified safe)";
	case WF_PF_DEST_ACCESS:        return "no write access to destination";
	case WF_PF_VOLUME_UNSUPPORTED: return "destination volume unusable";
	case WF_PF_REPARSE: return "link or reparse point not supported";
	case WF_PF_PATH_TOO_LONG:
		return "path too long for the transaction area";
	case WF_PF_COMPONENT_TOO_LONG: return "name too long for destination";
	case WF_PF_NO_SPACE:           return "not enough free space";
	case WF_PF_TREE_TOO_LARGE:     return "folder has too many entries";
	case WF_PF_IO_ERROR:  return "cannot read source or destination";
	case WF_PF_CANCELLED: return "cancelled";
	}
	return "unknown";
}

/* The saved snapshot and the live object must describe the same thing.
 * Size and mtime are compared for files: a changed file is refused rather
 * than silently copied in a different state (spec 2.3). */
static bool snapshot_matches(const WfNodeInfo *snap, const WfNodeInfo *now)
{
	/* A newly offline/recall file must not bypass hydration consent by
	 * using the earlier slot's ordinary-file flags for accounting. */
	const uint32_t kind = WF_ENT_DIR | WF_ENT_LINK | WF_ENT_CLOUD;

	if ((snap->flags & kind) != (now->flags & kind)) {
		return false;
	}
	if ((snap->id.kind == WF_ID_STRONG) != (now->id.kind == WF_ID_STRONG) ||
	        wf_fileid_is_weak(&snap->id) != wf_fileid_is_weak(&now->id)) {
		return false;
	}
	if ((snap->id.kind == WF_ID_STRONG) &&
	        !wf_fileid_equal(&snap->id, &now->id)) {
		return false;
	}
	if (wf_fileid_is_weak(&snap->id) &&
	        !wf_fileid_equal_or_weak(&snap->id, &now->id)) {
		return false;
	}
	if ((snap->flags & WF_ENT_LINK) &&
	        snap->reparse_tag != now->reparse_tag) {
		return false;
	}
	if (!(snap->flags & WF_ENT_DIR) &&
	        (snap->size != now->size || snap->mtime != now->mtime)) {
		return false;
	}
	return true;
}

/* Each check below records a verdict in `rep` (wf_pf_verdict) and returns
 * WF_OK, or returns an error status when it could not even decide. The caller
 * stops at the first verdict: `st != WF_OK || rep->reason != WF_PF_OK`. */

/* The destination must be a folder reached without any link, on a writable
 * volume the transaction area fits on. Fills the destination node and volume
 * and the report's volume facts. */
static WfStatus check_destination(const WfProbeOps *ops,
        const wchar_t *dest,
        bool copy,
        WfNodeInfo *dn,
        WfVolInfo *dv,
        WfPreflightReport *rep)
{
	wchar_t leaf[64];
	WfStatus st = ops->node(ops->ctx, dest, dn);

	if (WF_E_NOENT == st) {
		return wf_pf_verdict(rep, WF_PF_DEST_NOT_DIR, 0, L"");
	}
	if (WF_OK != st) {
		return wf_pf_verdict(rep, WF_PF_IO_ERROR, 0, L"");
	}
	if (!(dn->flags & WF_ENT_DIR)) {
		return wf_pf_verdict(rep, WF_PF_DEST_NOT_DIR, 0, L"");
	}
	/* No kind of reparse point has been shown safe as a destination
	 * (H-007 is OPEN), so none is accepted: not the folder itself and not
	 * any folder on the way to it. */
	switch (wf_pf_dest_path_link(ops, dest, leaf, WF_ARRAY_LEN(leaf))) {
	case WF_PF_LINK_NONE: break;
	case WF_PF_LINK_FOUND:
		return wf_pf_verdict(rep, WF_PF_DEST_REPARSE, 0, leaf);
	case WF_PF_LINK_UNKNOWN:
		return wf_pf_verdict(rep, WF_PF_IO_ERROR, 0, leaf);
	}

	if (WF_OK != ops->volume(ops->ctx, dest, dv)) {
		return wf_pf_verdict(rep, WF_PF_VOLUME_UNSUPPORTED, 0, L"");
	}
	wf_pf_copy_w(rep->fs_name, WF_ARRAY_LEN(rep->fs_name), dv->fs_name);
	rep->free_bytes = dv->free_bytes;
	rep->auto_recovery = dv->is_local && dv->supports_open_by_id &&
	        (0 == wcscmp(dv->fs_name, L"NTFS") ||
	                0 == wcscmp(dv->fs_name, L"ReFS"));
	if (dv->read_only || 0 == dv->max_component) {
		return wf_pf_verdict(rep, WF_PF_VOLUME_UNSUPPORTED, 0, L"");
	}
	if (copy && dv->max_component < WF_TXN_NAME_UNITS) {
		return wf_pf_verdict(
		        rep, WF_PF_COMPONENT_TOO_LONG, 0, L".wfile-txn-<UUID>");
	}
	if (WF_OK != ops->dest_access(ops->ctx, dest)) {
		return wf_pf_verdict(rep, WF_PF_DEST_ACCESS, 0, L"");
	}
	return WF_OK;
}

/* A top-level source that is a link: MOVE renames the link itself, so
 * redirect kinds are fine there; unknown kinds never are, and COPY accepts no
 * redirecting link (spec 6.3). */
static bool top_level_link_refused(const WfNodeInfo *now, bool copy)
{
	WfReparseClass rc = wf_reparse_classify(now->flags, now->reparse_tag);

	return WF_REPARSE_UNKNOWN == rc ||
	        (copy &&
	                (WF_REPARSE_REDIRECT == rc ||
	                        ((now->flags & (WF_ENT_DIR | WF_ENT_LINK)) ==
	                                (WF_ENT_DIR | WF_ENT_LINK))));
}

/* Every captured source still exists, is unchanged, is a permitted kind and,
 * for MOVE, lives on the destination's volume. Counts the top-level kinds. */
static WfStatus check_sources(const WfProbeOps *ops,
        const WfTransferSlot *slot,
        bool copy,
        const WfNodeInfo *dn,
        const WfVolInfo *dv,
        const WfCancel *cancel,
        WfPreflightReport *rep)
{
	WfVolInfo sv;
	bool have_sv = false;
	size_t i;

	for (i = 0; i < slot->count; i++) {
		const WfSourceSnap *src = &slot->src[i];
		WfNodeInfo now;
		wchar_t *full;
		WfStatus st;

		if (wf_pf_cancelled(cancel)) {
			return wf_pf_verdict(
			        rep, WF_PF_CANCELLED, i, src->name);
		}
		if (WF_OK != wf_path_join(slot->dir, src->name, &full)) {
			return WF_E_NOMEM;
		}
		st = ops->node(ops->ctx, full, &now);
		free(full);
		if (WF_E_NOENT == st) {
			return wf_pf_verdict(
			        rep, WF_PF_SOURCE_MISSING, i, src->name);
		}
		if (WF_OK != st) {
			return wf_pf_verdict(rep, WF_PF_IO_ERROR, i, src->name);
		}
		if (!snapshot_matches(&src->snap, &now)) {
			return wf_pf_verdict(
			        rep, WF_PF_SOURCE_CHANGED, i, src->name);
		}
		if (top_level_link_refused(&now, copy)) {
			return wf_pf_verdict(rep, WF_PF_REPARSE, i, src->name);
		}

		if (now.flags & WF_ENT_DIR) {
			rep->top_dirs++;
		} else {
			rep->top_files++;
		}
		if (now.flags & WF_ENT_LINK) {
			rep->top_links++;
		}

		if (!copy) {
			if (!have_sv) {
				if (WF_OK !=
				        ops->volume(ops->ctx, slot->dir, &sv)) {
					return wf_pf_verdict(rep,
					        WF_PF_IO_ERROR, i, src->name);
				}
				have_sv = true;
			}
			if (wf_pf_volumes_differ(&now, &sv, dn, dv)) {
				return wf_pf_verdict(
				        rep, WF_PF_CROSS_VOLUME, i, src->name);
			}
		}
	}
	return WF_OK;
}

/* The destination may be neither the sources' own folder nor inside one of
 * the sources. */
static WfStatus check_placement(const WfProbeOps *ops,
        const WfTransferSlot *slot,
        const wchar_t *dest,
        WfPreflightReport *rep)
{
	size_t which = 0;
	bool inside = false;
	WfStatus st;

	if (wf_pf_same_folder(ops, slot, dest)) {
		return wf_pf_verdict(
		        rep, WF_PF_SAME_FOLDER, 0, slot->src[0].name);
	}
	st = wf_pf_dest_inside_source(ops, slot, dest, &which, &inside);
	if (WF_OK != st) {
		return st;
	}
	if (inside) {
		return wf_pf_verdict(rep, WF_PF_DEST_INSIDE_SOURCE, which,
		        slot->src[which].name);
	}
	return WF_OK;
}

/* Enough room for the whole tree plus the safety reserve. need_bytes holds
 * UINT64_MAX when the sum itself overflows: diagnostic only, never a
 * satisfiable number. */
static WfStatus check_space(const WfPreflightPolicy *pol,
        const WfVolInfo *dv,
        WfPreflightReport *rep)
{
	rep->free_bytes = dv->free_bytes;
	if (rep->total_bytes > UINT64_MAX - pol->reserve_bytes) {
		rep->need_bytes = UINT64_MAX;
		return wf_pf_verdict(rep, WF_PF_NO_SPACE, 0, L"");
	}
	rep->need_bytes = rep->total_bytes + pol->reserve_bytes;
	if (dv->free_bytes < rep->need_bytes) {
		return wf_pf_verdict(rep, WF_PF_NO_SPACE, 0, L"");
	}
	return WF_OK;
}

/* Whether the checks stop here: an error, or a verdict already recorded. */
static bool stop_here(WfStatus st, const WfPreflightReport *rep)
{
	return WF_OK != st || WF_PF_OK != rep->reason;
}

WfStatus wf_preflight_run(const WfProbeOps *ops,
        const WfTransferSlot *slot,
        const wchar_t *dest,
        const WfPreflightPolicy *pol,
        const WfCancel *cancel,
        WfPreflightReport *rep)
{
	WfNodeInfo dn;
	WfVolInfo dv;
	bool copy;
	WfPfWalk w;
	WfStatus st;

	if (!rep) {
		return WF_E_INVAL;
	}
	memset(rep, 0, sizeof *rep);
	if (!ops || !dest || !pol) {
		return WF_E_INVAL;
	}
	rep->reason = WF_PF_OK;
	rep->reserve_bytes = pol->reserve_bytes;
	if (!slot || WF_XFER_NONE == slot->mode || 0 == slot->count) {
		return wf_pf_verdict(rep, WF_PF_EMPTY_SLOT, 0, L"");
	}
	copy = WF_XFER_COPY == slot->mode;

	/* order matters: nothing about the sources is trusted before the
	 * destination is, and the expensive tree walk comes last */
	st = check_destination(ops, dest, copy, &dn, &dv, rep);
	if (stop_here(st, rep)) {
		return st;
	}
	st = check_sources(ops, slot, copy, &dn, &dv, cancel, rep);
	if (stop_here(st, rep)) {
		return st;
	}
	st = check_placement(ops, slot, dest, rep);
	if (stop_here(st, rep)) {
		return st;
	}

	w.ops = ops;
	w.cancel = cancel;
	w.rep = rep;
	if (WF_OK != wf_pf_dest_units(dest, &w.dest_ext_len)) {
		return wf_pf_verdict(rep, WF_PF_PATH_TOO_LONG, 0, L"");
	}
	w.max_component = dv.max_component;
	w.collisions_ok = slot->collisions_ok;
	rep->max_path_units = pol->max_path_units < EXTENDED_PATH_LIMIT
	        ? pol->max_path_units
	        : EXTENDED_PATH_LIMIT;
	rep->max_component = dv.max_component;

	/* collisions: any single one stops everything */
	st = wf_pf_check_collisions(slot, dest, copy, &w);
	if (stop_here(st, rep)) {
		return st;
	}

	if (copy) { /* whole-tree policy, path overhead, space */
		st = wf_pf_walk_sources(slot, &w);
		if (stop_here(st, rep)) {
			return st;
		}
		st = check_space(pol, &dv, rep);
		if (stop_here(st, rep)) {
			return st;
		}
	}
	rep->checks_complete = true;
	return WF_OK;
}

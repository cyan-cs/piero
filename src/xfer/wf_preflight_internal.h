#ifndef WF_PREFLIGHT_INTERNAL_H
#define WF_PREFLIGHT_INTERNAL_H

/* Private preflight phases. The public API remains in wf_xfer.h. */
#include <stdbool.h>
#include <string.h>
#include <wchar.h>

#include "wf_xfer.h"
#include "../base/wf_macro.h"

typedef struct
{
	const WfProbeOps *ops;
	const WfCancel *cancel;
	WfPreflightReport *rep;
	size_t dest_ext_len; /* extended, trailing '\' trimmed */
	size_t top_name_units;
	uint32_t max_component;
	bool collisions_ok;
} WfPfWalk;

static inline bool wf_pf_cancelled(const WfCancel *c)
{
	return c && c->is_set && c->is_set(c->ctx);
}

/* Bounded copy, always NUL-terminated (truncates silently). */
static inline void wf_pf_copy_w(wchar_t *dst, size_t cap, const wchar_t *src)
{
	size_t n = wcslen(src);

	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(dst, src, n * sizeof *dst);
	dst[n] = L'\0';
}

static inline void wf_pf_set_bad(
        WfPreflightReport *rep, size_t idx, const wchar_t *name)
{
	rep->bad_index = idx;
	wf_pf_copy_w(rep->bad_name, WF_ARRAY_LEN(rep->bad_name), name);
}

/* Records why the run stops and which entry is at fault. A verdict is not an
 * error: the caller returns WF_OK, so it writes `return wf_pf_verdict(...)`. */
static inline WfStatus wf_pf_verdict(WfPreflightReport *rep,
        WfPfReason reason,
        size_t idx,
        const wchar_t *name)
{
	rep->reason = reason;
	wf_pf_set_bad(rep, idx, name);
	return WF_OK;
}

/* A link on the destination path, or "could not tell": unknown is not clean. */
typedef enum
{
	WF_PF_LINK_UNKNOWN = -1,
	WF_PF_LINK_NONE = 0,
	WF_PF_LINK_FOUND = 1
} WfPfLink;

WfPfLink wf_pf_dest_path_link(const WfProbeOps *ops,
        const wchar_t *dest,
        wchar_t *leaf,
        size_t leaf_cap);
bool wf_pf_volumes_differ(const WfNodeInfo *src,
        const WfVolInfo *sv,
        const WfNodeInfo *dst,
        const WfVolInfo *dv);
bool wf_pf_same_folder(
        const WfProbeOps *ops, const WfTransferSlot *slot, const wchar_t *dest);
/* WF_OK with *inside set is a verdict; any other status (out of memory) means
 * the overlap check did not run and the caller must not proceed. */
WfStatus wf_pf_dest_inside_source(const WfProbeOps *ops,
        const WfTransferSlot *slot,
        const wchar_t *dest,
        size_t *which,
        bool *inside);
WfStatus wf_pf_dest_units(const wchar_t *dest, size_t *units);
WfStatus wf_pf_check_collisions(const WfTransferSlot *slot,
        const wchar_t *dest,
        bool copy,
        WfPfWalk *w);
WfStatus wf_pf_walk_sources(const WfTransferSlot *slot, WfPfWalk *w);

#endif

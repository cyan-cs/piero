#include "wf_savemodel.h"

#include <string.h>

WfSnapVerdict wf_snapshot_compare(const WfSnapshot *b, const WfSnapshot *n)
{
	bool compared = false, weak_time_only = false;

	if (!b || !n || !b->exists) {
		return WF_SNAP_UNKNOWN;
	}
	if (!n->exists || n->is_link || !n->is_regular) {
		return WF_SNAP_GONE;
	}
	/* the same name now stands for another object */
	if (b->have_identity && n->have_identity &&
	        (b->dev != n->dev || b->ino != n->ino)) {
		return WF_SNAP_REPLACED;
	}
	if (b->have_size && n->have_size) {
		compared = true;
		if (b->size != n->size) {
			return WF_SNAP_CHANGED;
		}
	}
	if (b->have_hash && n->have_hash) {
		/* the bytes themselves: nothing weaker can overrule them */
		return memcmp(b->sha256, n->sha256, sizeof b->sha256)
		        ? WF_SNAP_CHANGED
		        : WF_SNAP_SAME;
	}
	if (b->have_mtime && n->have_mtime) {
		if (b->mtime != n->mtime) {
			return WF_SNAP_CHANGED;
		}
		compared = true;
		/* equal times prove little when the clock is coarse or unknown:
		 * a write in the same tick leaves them equal, and the size can
		 * stay the same too */
		weak_time_only = 0 == b->mtime_granularity ||
		        0 == n->mtime_granularity || b->mtime_granularity > 1 ||
		        n->mtime_granularity > 1;
	}
	if (!compared) {
		return WF_SNAP_UNKNOWN;
	}
	/* size alone, or a coarse time: not enough to call it unchanged */
	if (weak_time_only || !(b->have_mtime && n->have_mtime)) {
		return WF_SNAP_UNKNOWN;
	}
	return WF_SNAP_SAME;
}

WfSavePlan wf_save_plan(uint32_t caps,
        bool target_exists,
        bool buffer_from_file,
        WfSnapVerdict verdict,
        WfSaveWhy *why)
{
	WfSaveWhy w = WF_WHY_OK;
	WfSavePlan p = WF_PLAN_REFUSE;

	if (!(caps & (WF_CAP_NEW_FILE | WF_CAP_SAVE_OVER))) {
		w = WF_WHY_READ_ONLY_PLACE;
	} else if (!target_exists) {
		/* a file that was opened and is gone is not a new file */
		if (buffer_from_file) {
			w = WF_WHY_GONE;
		} else if (!wf_cap_has(caps, WF_CAP_NEW_FILE)) {
			w = WF_WHY_NO_NEW_FILE;
		} else {
			p = WF_PLAN_NEW_ONLY;
		}
	} else if (!buffer_from_file) {
		w = WF_WHY_EXISTS; /* never over a file that was not opened */
	} else if (!wf_cap_has(caps, WF_CAP_SAVE_OVER)) {
		w = WF_WHY_NO_REPLACE;
	} else {
		switch (verdict) {
		case WF_SNAP_SAME:     p = WF_PLAN_REPLACE; break;
		case WF_SNAP_CHANGED:  w = WF_WHY_CHANGED; break;
		case WF_SNAP_GONE:     w = WF_WHY_GONE; break;
		case WF_SNAP_REPLACED: w = WF_WHY_REPLACED; break;
		default:               w = WF_WHY_UNVERIFIED; break;
		}
	}
	if (why) {
		*why = w;
	}
	return p;
}

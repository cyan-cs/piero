/*
 * Whether a file is still the one that was opened, and what a save may do:
 * the decisions every backend shares, with the fail-closed defaults.
 */
#include <string.h>

#include "fs/wf_savemodel.h"
#include "wf_test.h"

static WfSnapshot file(uint64_t size, int64_t mtime, uint32_t gran)
{
	WfSnapshot s;

	memset(&s, 0, sizeof s);
	s.exists = s.is_regular = true;
	s.have_size = s.have_mtime = true;
	s.size = size;
	s.mtime = mtime;
	s.mtime_granularity = gran;
	return s;
}

static void with_hash(WfSnapshot *s, uint8_t v)
{
	s->have_hash = true;
	memset(s->sha256, v, sizeof s->sha256);
}

static void with_identity(WfSnapshot *s, uint64_t dev, uint64_t ino)
{
	s->have_identity = true;
	s->dev = dev;
	s->ino = ino;
}

static void the_bytes_decide_when_both_sides_have_them(void)
{
	WfSnapshot a = file(10, 100, 1), b = file(10, 100, 1);

	with_hash(&a, 1);
	with_hash(&b, 1);
	CHECK(WF_SNAP_SAME == wf_snapshot_compare(&a, &b));
	/* same size, same time, different bytes: somebody wrote in the same tick */
	with_hash(&b, 2);
	CHECK(WF_SNAP_CHANGED == wf_snapshot_compare(&a, &b));
	/* a different time does not matter when the bytes are the same ... */
	with_hash(&b, 1);
	b.mtime = 999;
	CHECK(WF_SNAP_SAME == wf_snapshot_compare(&a, &b));
	/* ... but a different size always does */
	b.size = 11;
	CHECK(WF_SNAP_CHANGED == wf_snapshot_compare(&a, &b));
}

static void a_coarse_or_unknown_clock_cannot_prove_it_is_unchanged(void)
{
	WfSnapshot a = file(10, 100, 1), b = file(10, 100, 1);

	CHECK(WF_SNAP_SAME == wf_snapshot_compare(&a, &b)); /* fine clock */
	a.mtime_granularity = 2000; /* a FAT-like two-second clock */
	b.mtime_granularity = 2000;
	CHECK(WF_SNAP_UNKNOWN == wf_snapshot_compare(&a, &b));
	a.mtime_granularity = 0; /* not known (whole seconds over SFTP v3) */
	b.mtime_granularity = 0;
	CHECK(WF_SNAP_UNKNOWN == wf_snapshot_compare(&a, &b));
	/* with a hash the clock no longer matters */
	with_hash(&a, 3);
	with_hash(&b, 3);
	CHECK(WF_SNAP_SAME == wf_snapshot_compare(&a, &b));
	/* a different time is a change whatever the granularity */
	a = file(10, 100, 0);
	b = file(10, 101, 0);
	CHECK(WF_SNAP_CHANGED == wf_snapshot_compare(&a, &b));
}

static void little_evidence_is_unknown_not_same(void)
{
	WfSnapshot a, b;

	memset(&a, 0, sizeof a);
	memset(&b, 0, sizeof b);
	a.exists = b.exists = a.is_regular = b.is_regular = true;
	CHECK(WF_SNAP_UNKNOWN == wf_snapshot_compare(&a, &b)); /* nothing */
	a.have_size = b.have_size = true;
	a.size = b.size = 5;
	CHECK(WF_SNAP_UNKNOWN == wf_snapshot_compare(&a, &b)); /* size only */
	CHECK(WF_SNAP_UNKNOWN == wf_snapshot_compare(NULL, &b));
	CHECK(WF_SNAP_UNKNOWN == wf_snapshot_compare(&a, NULL));
	a.exists = false; /* there was no base file */
	CHECK(WF_SNAP_UNKNOWN == wf_snapshot_compare(&a, &b));
}

static void gone_and_replaced_are_told_from_changed(void)
{
	WfSnapshot a = file(10, 100, 1), b = file(10, 100, 1);

	with_identity(&a, 7, 100);
	with_identity(&b, 7, 100);
	CHECK(WF_SNAP_SAME == wf_snapshot_compare(&a, &b));
	with_identity(&b, 7, 101); /* same name, another file */
	CHECK(WF_SNAP_REPLACED == wf_snapshot_compare(&a, &b));
	with_identity(&b, 8, 100); /* another device */
	CHECK(WF_SNAP_REPLACED == wf_snapshot_compare(&a, &b));
	/* the replacement is identified even if its bytes are the same */
	with_hash(&a, 1);
	with_hash(&b, 1);
	CHECK(WF_SNAP_REPLACED == wf_snapshot_compare(&a, &b));

	b = file(10, 100, 1);
	b.exists = false;
	CHECK(WF_SNAP_GONE == wf_snapshot_compare(&a, &b));
	b.exists = true;
	b.is_link = true; /* a link took the name: not followed */
	CHECK(WF_SNAP_GONE == wf_snapshot_compare(&a, &b));
	b.is_link = false;
	b.is_regular = false; /* a directory, a device, a pipe */
	CHECK(WF_SNAP_GONE == wf_snapshot_compare(&a, &b));
}

static void the_plan_follows_what_the_place_allows(void)
{
	const uint32_t ro = WF_CAP_LIST | WF_CAP_READ;
	const uint32_t new_only = ro | WF_CAP_NEW_FILE;
	const uint32_t full = new_only | WF_CAP_SAVE_OVER;
	WfSaveWhy why;

	/* a read-only place: nothing, whatever else is true */
	CHECK(WF_PLAN_REFUSE ==
	                wf_save_plan(ro, false, false, WF_SNAP_SAME, &why) &&
	        WF_WHY_READ_ONLY_PLACE == why);
	CHECK(WF_PLAN_REFUSE ==
	                wf_save_plan(ro, true, true, WF_SNAP_SAME, &why) &&
	        WF_WHY_READ_ONLY_PLACE == why);

	/* a new file needs the new-file capability, and a free name */
	CHECK(WF_PLAN_NEW_ONLY ==
	                wf_save_plan(new_only, false, false, WF_SNAP_UNKNOWN,
	                        &why) &&
	        WF_WHY_OK == why);
	CHECK(WF_PLAN_REFUSE ==
	                wf_save_plan(
	                        new_only, true, false, WF_SNAP_SAME, &why) &&
	        WF_WHY_EXISTS == why);
	/* a file that was opened and is gone is not saved as new */
	CHECK(WF_PLAN_REFUSE ==
	                wf_save_plan(full, false, true, WF_SNAP_GONE, &why) &&
	        WF_WHY_GONE == why);
	/* replacing needs its own capability */
	CHECK(WF_PLAN_REFUSE ==
	                wf_save_plan(
	                        new_only, true, true, WF_SNAP_SAME, &why) &&
	        WF_WHY_NO_REPLACE == why);

	/* replacing only over the file that was read */
	CHECK(WF_PLAN_REPLACE ==
	                wf_save_plan(full, true, true, WF_SNAP_SAME, &why) &&
	        WF_WHY_OK == why);
	CHECK(WF_PLAN_REFUSE ==
	                wf_save_plan(full, true, true, WF_SNAP_CHANGED, &why) &&
	        WF_WHY_CHANGED == why);
	CHECK(WF_PLAN_REFUSE ==
	                wf_save_plan(
	                        full, true, true, WF_SNAP_REPLACED, &why) &&
	        WF_WHY_REPLACED == why);
	CHECK(WF_PLAN_REFUSE ==
	                wf_save_plan(full, true, true, WF_SNAP_GONE, &why) &&
	        WF_WHY_GONE == why);
	CHECK(WF_PLAN_REFUSE ==
	                wf_save_plan(full, true, true, WF_SNAP_UNKNOWN, &why) &&
	        WF_WHY_UNVERIFIED == why);
	/* an out-of-range verdict is not a yes */
	CHECK(WF_PLAN_REFUSE ==
	        wf_save_plan(full, true, true, (WfSnapVerdict)42, NULL));
}

static void a_place_with_a_partial_proof_never_replaces(void)
{
	WfSaveProof p = {true, true, false, true, true, true}; /* not atomic */
	uint32_t caps =
	        wf_caps_grant_save(WF_BK_SFTP, wf_caps_default(WF_BK_SFTP), &p);
	WfSaveWhy why;

	CHECK(WF_PLAN_REFUSE ==
	                wf_save_plan(caps, true, true, WF_SNAP_SAME, &why) &&
	        WF_WHY_NO_REPLACE == why);
	CHECK(WF_PLAN_NEW_ONLY ==
	        wf_save_plan(caps, false, false, WF_SNAP_SAME, &why));
}

int main(void)
{
	the_bytes_decide_when_both_sides_have_them();
	a_coarse_or_unknown_clock_cannot_prove_it_is_unchanged();
	little_evidence_is_unknown_not_same();
	gone_and_replaced_are_told_from_changed();
	the_plan_follows_what_the_place_allows();
	a_place_with_a_partial_proof_never_replaces();
	return TEST_RESULT();
}

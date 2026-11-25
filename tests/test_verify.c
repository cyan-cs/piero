/* Verification policy and stage order. Pure logic: the property under test is
 * that the source can only become removable through the complete, successful
 * sequence, and never again once anything went wrong. */
#include <string.h>

#include "wf_test.h"
#include "xfer/wf_verify.h"
#include "base/wf_macro.h"

static WfVerifyPlan plan(WfTransferKind kind, WfCopyVerify configured)
{
	WfVerifyPlan p;

	CHECK(WF_OK == wf_verify_plan(kind, configured, &p));
	return p;
}

static void plans(void)
{
	WfVerifyPlan p = plan(WF_KIND_COPY, WF_VERIFY_SHA256);

	CHECK(p.copies_data && p.read_back && !p.removes_source &&
	        WF_VERIFY_SHA256 == p.method);
	p = plan(WF_KIND_COPY, WF_VERIFY_HMAC_SHA256);
	CHECK(p.read_back && WF_VERIFY_HMAC_SHA256 == p.method);
	/* `none` is an explicit opt-out for copies only */
	p = plan(WF_KIND_COPY, WF_VERIFY_NONE);
	CHECK(p.copies_data && !p.read_back && WF_VERIFY_NONE == p.method);

	/* a cross-volume move always reads back, whatever was configured */
	p = plan(WF_KIND_MOVE_CROSS_VOLUME, WF_VERIFY_NONE);
	CHECK(p.copies_data && p.read_back && p.removes_source &&
	        WF_VERIFY_SHA256 == p.method);
	p = plan(WF_KIND_MOVE_CROSS_VOLUME, WF_VERIFY_HMAC_SHA256);
	CHECK(p.read_back && p.removes_source &&
	        WF_VERIFY_HMAC_SHA256 == p.method);

	/* a same-volume move is a rename: no data copied, nothing hashed */
	for (int v = WF_VERIFY_NONE; v <= WF_VERIFY_HMAC_SHA256; v++) {
		p = plan(WF_KIND_MOVE_SAME_VOLUME, (WfCopyVerify)v);
		CHECK(!p.copies_data && !p.read_back && !p.removes_source &&
		        WF_VERIFY_NONE == p.method);
	}
	CHECK(WF_E_INVAL == wf_verify_plan(WF_KIND_COPY, WF_VERIFY_NONE, NULL));
	CHECK(WF_E_INVAL ==
	        wf_verify_plan((WfTransferKind)99, WF_VERIFY_SHA256, &p));
	CHECK(WF_E_INVAL == wf_verify_plan(WF_KIND_COPY, (WfCopyVerify)99, &p));
}

/* Runs the whole successful sequence of a cross-volume move. */
static void run_to(WfStageMachine *m, WfStage last)
{
	if (last >= WF_STAGE_COPIED) {
		CHECK(wf_stage_copied(m, true));
	}
	if (last >= WF_STAGE_SOURCE_ACCEPTED) {
		CHECK(wf_stage_source_accepted(m, true));
	}
	if (last >= WF_STAGE_DESTINATION_DURABLE) {
		CHECK(wf_stage_destination_durable(m, true));
	}
	if (last >= WF_STAGE_VERIFIED) {
		CHECK(wf_stage_verified(m, WF_VERDICT_MATCH));
	}
	if (last >= WF_STAGE_VERIFIED_RECORDED) {
		CHECK(wf_stage_verified_recorded(m, true));
	}
}

static void cross_volume_move_happy_path(void)
{
	WfVerifyPlan p = plan(WF_KIND_MOVE_CROSS_VOLUME, WF_VERIFY_SHA256);
	WfStageMachine m;

	wf_stage_init(&m, &p);
	/* the source becomes removable only at the very end of the chain */
	for (WfStage s = WF_STAGE_NEW; s <= WF_STAGE_VERIFIED; s++) {
		WfStageMachine at;

		wf_stage_init(&at, &p);
		run_to(&at, s);
		CHECK(!wf_stage_may_remove_source(&at));
	}
	run_to(&m, WF_STAGE_VERIFIED_RECORDED);
	CHECK(wf_stage_may_remove_source(&m));
	CHECK(wf_stage_complete(&m) && WF_STAGE_DONE == m.stage);
	CHECK(wf_stage_may_remove_source(&m) && WF_STOP_NONE == m.stop);
}

/* Any failure at any stage stops for good and never authorizes removal. */
static void failures_never_authorize_removal(void)
{
	WfVerifyPlan p = plan(WF_KIND_MOVE_CROSS_VOLUME, WF_VERIFY_SHA256);
	static const struct
	{
		WfStage before; /* stages completed before the failure */
		int which;      /* which report fails */
		WfVerifyVerdict verdict;
		WfStopReason expect;
	} cases[] = {
	        {WF_STAGE_NEW, 0, 0, WF_STOP_COPY_FAILED},
	        {WF_STAGE_COPIED, 1, 0, WF_STOP_SOURCE_REJECTED},
	        {WF_STAGE_SOURCE_ACCEPTED, 2, 0, WF_STOP_NOT_DURABLE},
	        {WF_STAGE_DESTINATION_DURABLE, 3, WF_VERDICT_MISMATCH,
	                WF_STOP_MISMATCH},
	        {WF_STAGE_DESTINATION_DURABLE, 3, WF_VERDICT_CHANGED,
	                WF_STOP_CHANGED},
	        {WF_STAGE_DESTINATION_DURABLE, 3, WF_VERDICT_CANCELLED,
	                WF_STOP_CANCELLED},
	        {WF_STAGE_DESTINATION_DURABLE, 3, WF_VERDICT_ERROR,
	                WF_STOP_IO_ERROR},
	        {WF_STAGE_DESTINATION_DURABLE, 3, (WfVerifyVerdict)77,
	                WF_STOP_IO_ERROR},
	        {WF_STAGE_VERIFIED, 4, 0, WF_STOP_RECORD_FAILED},
	};

	for (size_t i = 0; i < WF_ARRAY_LEN(cases); i++) {
		WfStageMachine m;
		bool went;

		wf_stage_init(&m, &p);
		run_to(&m, cases[i].before);
		switch (cases[i].which) {
		case 0:  went = wf_stage_copied(&m, false); break;
		case 1:  went = wf_stage_source_accepted(&m, false); break;
		case 2:  went = wf_stage_destination_durable(&m, false); break;
		case 3:  went = wf_stage_verified(&m, cases[i].verdict); break;
		default: went = wf_stage_verified_recorded(&m, false); break;
		}
		CHECK(!went && WF_STAGE_STOPPED == m.stage &&
		        m.stop == cases[i].expect);
		CHECK(!wf_stage_may_remove_source(&m));
		/* stopped is final: replaying the good events changes nothing */
		CHECK(!wf_stage_copied(&m, true));
		CHECK(!wf_stage_source_accepted(&m, true));
		CHECK(!wf_stage_destination_durable(&m, true));
		CHECK(!wf_stage_verified(&m, WF_VERDICT_MATCH));
		CHECK(!wf_stage_verified_recorded(&m, true));
		CHECK(!wf_stage_complete(&m));
		CHECK(WF_STAGE_STOPPED == m.stage &&
		        m.stop == cases[i].expect &&
		        !wf_stage_may_remove_source(&m));
	}
}

/* Skipping or repeating a stage is a bug in the caller and stops the machine. */
static void out_of_order_stops(void)
{
	WfVerifyPlan p = plan(WF_KIND_MOVE_CROSS_VOLUME, WF_VERIFY_SHA256);
	WfStageMachine m;

	wf_stage_init(&m, &p);
	CHECK(!wf_stage_verified(
	        &m, WF_VERDICT_MATCH)); /* nothing copied yet */
	CHECK(WF_STOP_OUT_OF_ORDER == m.stop &&
	        !wf_stage_may_remove_source(&m));

	wf_stage_init(&m, &p);
	run_to(&m, WF_STAGE_SOURCE_ACCEPTED);
	CHECK(!wf_stage_verified(&m, WF_VERDICT_MATCH)); /* not made durable */
	CHECK(WF_STOP_OUT_OF_ORDER == m.stop);

	wf_stage_init(&m, &p);
	run_to(&m, WF_STAGE_VERIFIED);
	CHECK(!wf_stage_complete(&m)); /* the record was never made durable */
	CHECK(WF_STAGE_STOPPED == m.stage && !wf_stage_may_remove_source(&m));

	wf_stage_init(&m, &p);
	run_to(&m, WF_STAGE_COPIED);
	CHECK(!wf_stage_copied(&m, true)); /* the same stage twice */
	CHECK(WF_STAGE_STOPPED == m.stage);

	/* an unknown source verdict can never be MATCH by accident */
	wf_stage_init(&m, &p);
	run_to(&m, WF_STAGE_DESTINATION_DURABLE);
	CHECK(!wf_stage_verified(&m, (WfVerifyVerdict)(WF_VERDICT_MATCH + 40)));
}

static void copy_plans(void)
{
	WfVerifyPlan p = plan(WF_KIND_COPY, WF_VERIFY_SHA256);
	WfStageMachine m;

	/* a verified copy never makes the source removable */
	wf_stage_init(&m, &p);
	run_to(&m, WF_STAGE_VERIFIED);
	CHECK(wf_stage_complete(&m) && !wf_stage_may_remove_source(&m));

	/* with read-back on, a copy is not complete before it was verified */
	wf_stage_init(&m, &p);
	run_to(&m, WF_STAGE_DESTINATION_DURABLE);
	CHECK(!wf_stage_complete(&m) && WF_STAGE_STOPPED == m.stage);

	/* the opt-out completes after durability, and reporting a verdict for a
	 * verification that was never wanted is a caller bug */
	p = plan(WF_KIND_COPY, WF_VERIFY_NONE);
	wf_stage_init(&m, &p);
	run_to(&m, WF_STAGE_DESTINATION_DURABLE);
	CHECK(wf_stage_complete(&m) && WF_STAGE_DONE == m.stage);
	wf_stage_init(&m, &p);
	run_to(&m, WF_STAGE_DESTINATION_DURABLE);
	CHECK(!wf_stage_verified(&m, WF_VERDICT_MATCH));
}

/* A rename is not driven through the copy stages at all. */
static void same_volume_move_has_no_stages(void)
{
	WfVerifyPlan p = plan(WF_KIND_MOVE_SAME_VOLUME, WF_VERIFY_SHA256);
	WfStageMachine m;

	wf_stage_init(&m, &p);
	CHECK(WF_STAGE_STOPPED == m.stage);
	CHECK(!wf_stage_copied(&m, true) && !wf_stage_may_remove_source(&m));
	/* an uninitialized plan is not a plan */
	wf_stage_init(&m, NULL);
	CHECK(WF_STAGE_STOPPED == m.stage && !wf_stage_may_remove_source(&m));
}

/* Exercise the removal predicate's stage, stop and read-back requirements. */
static void removal_predicate_is_strict(void)
{
	WfStageMachine m;

	memset(&m, 0, sizeof m);
	m.plan.removes_source = true;
	m.plan.read_back = true;
	m.stage = WF_STAGE_VERIFIED; /* verified but not recorded */
	CHECK(!wf_stage_may_remove_source(&m));
	m.stage = WF_STAGE_VERIFIED_RECORDED;
	CHECK(wf_stage_may_remove_source(&m));
	m.stop = WF_STOP_CANCELLED;
	CHECK(!wf_stage_may_remove_source(&m));
	m.stop = WF_STOP_NONE;
	m.plan.read_back = false; /* removing without a read-back is never ok */
	CHECK(!wf_stage_may_remove_source(&m));
	m.plan.read_back = true;
	m.plan.removes_source = false; /* a plain copy */
	CHECK(!wf_stage_may_remove_source(&m));
	CHECK(NULL != wf_stage_stop_text(WF_STOP_MISMATCH) &&
	        strlen(wf_stage_stop_text((WfStopReason)99)) > 0);
}

int main(void)
{
	plans();
	cross_volume_move_happy_path();
	failures_never_authorize_removal();
	out_of_order_stops();
	copy_plans();
	same_volume_move_has_no_stages();
	removal_predicate_is_strict();
	return TEST_RESULT();
}

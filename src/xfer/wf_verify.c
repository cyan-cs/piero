#include "wf_verify.h"

#include <string.h>

WfStatus wf_verify_plan(
        WfTransferKind kind, WfCopyVerify configured, WfVerifyPlan *out)
{
	if (!out) {
		return WF_E_INVAL;
	}
	memset(out, 0, sizeof *out);
	if (WF_VERIFY_NONE != configured && WF_VERIFY_SHA256 != configured &&
	        WF_VERIFY_HMAC_SHA256 != configured) {
		return WF_E_INVAL;
	}
	switch (kind) {
	case WF_KIND_COPY:
		out->copies_data = true;
		out->method = configured;
		out->read_back = WF_VERIFY_NONE != configured;
		return WF_OK;
	case WF_KIND_MOVE_SAME_VOLUME: return WF_OK; /* rename only */
	case WF_KIND_MOVE_CROSS_VOLUME:
		out->copies_data = true;
		out->removes_source = true;
		out->read_back = true;
		out->method = WF_VERIFY_NONE == configured ? WF_VERIFY_SHA256
		                                           : configured;
		return WF_OK;
	}
	return WF_E_INVAL;
}

void wf_stage_init(WfStageMachine *m, const WfVerifyPlan *plan)
{
	memset(m, 0, sizeof *m);
	if (plan) {
		m->plan = *plan;
	}
	m->stage = WF_STAGE_NEW;
	/* A plan that copies nothing has no stages to run: a rename must not be
	 * driven through the copy stages. */
	if (!m->plan.copies_data) {
		m->stage = WF_STAGE_STOPPED;
		m->stop = WF_STOP_OUT_OF_ORDER;
	}
}

static bool stop(WfStageMachine *m, WfStopReason reason)
{
	m->stage = WF_STAGE_STOPPED;
	m->stop = reason;
	return false;
}

/* One transition: allowed only from `from`, and only if the step succeeded. */
static bool step(WfStageMachine *m,
        WfStage from,
        WfStage to,
        bool succeeded,
        WfStopReason failure)
{
	if (m->stage != from) {
		return stop(m,
		        WF_STAGE_STOPPED == m->stage ? m->stop
		                                     : WF_STOP_OUT_OF_ORDER);
	}
	if (!succeeded) {
		return stop(m, failure);
	}
	m->stage = to;
	return true;
}

bool wf_stage_copied(WfStageMachine *m, bool copy_succeeded)
{
	return step(m, WF_STAGE_NEW, WF_STAGE_COPIED, copy_succeeded,
	        WF_STOP_COPY_FAILED);
}

bool wf_stage_source_accepted(WfStageMachine *m, bool accepted)
{
	return step(m, WF_STAGE_COPIED, WF_STAGE_SOURCE_ACCEPTED, accepted,
	        WF_STOP_SOURCE_REJECTED);
}

bool wf_stage_destination_durable(WfStageMachine *m, bool flushed)
{
	return step(m, WF_STAGE_SOURCE_ACCEPTED, WF_STAGE_DESTINATION_DURABLE,
	        flushed, WF_STOP_NOT_DURABLE);
}

bool wf_stage_verified(WfStageMachine *m, WfVerifyVerdict verdict)
{
	WfStopReason why = WF_STOP_IO_ERROR;

	if (WF_STAGE_DESTINATION_DURABLE == m->stage && !m->plan.read_back) {
		return stop(m, WF_STOP_OUT_OF_ORDER); /* nothing to report */
	}
	/* anything that is not exactly MATCH stops; unknown values are errors */
	if (WF_VERDICT_MISMATCH == verdict) {
		why = WF_STOP_MISMATCH;
	} else if (WF_VERDICT_CHANGED == verdict) {
		why = WF_STOP_CHANGED;
	} else if (WF_VERDICT_CANCELLED == verdict) {
		why = WF_STOP_CANCELLED;
	}
	return step(m, WF_STAGE_DESTINATION_DURABLE, WF_STAGE_VERIFIED,
	        WF_VERDICT_MATCH == verdict, why);
}

bool wf_stage_verified_recorded(WfStageMachine *m, bool record_durable)
{
	return step(m, WF_STAGE_VERIFIED, WF_STAGE_VERIFIED_RECORDED,
	        record_durable, WF_STOP_RECORD_FAILED);
}

bool wf_stage_complete(WfStageMachine *m)
{
	WfStage from;

	if (m->plan.removes_source) {
		from = WF_STAGE_VERIFIED_RECORDED;
	} else if (m->plan.read_back) {
		from = WF_STAGE_VERIFIED;
	} else {
		from = WF_STAGE_DESTINATION_DURABLE;
	}
	return step(m, from, WF_STAGE_DONE, true, WF_STOP_OUT_OF_ORDER);
}

bool wf_stage_may_remove_source(const WfStageMachine *m)
{
	return m->plan.removes_source && m->plan.read_back &&
	        (WF_STAGE_VERIFIED_RECORDED == m->stage ||
	                WF_STAGE_DONE == m->stage) &&
	        WF_STOP_NONE == m->stop;
}

const char *wf_stage_stop_text(WfStopReason reason)
{
	switch (reason) {
	case WF_STOP_NONE:         return "not stopped";
	case WF_STOP_OUT_OF_ORDER: return "stage reported out of order";
	case WF_STOP_COPY_FAILED:  return "copy failed";
	case WF_STOP_SOURCE_REJECTED:
		return "source is not the object that was chosen";
	case WF_STOP_NOT_DURABLE: return "destination was not made durable";
	case WF_STOP_MISMATCH:    return "destination differs from the source";
	case WF_STOP_CHANGED:
		return "source or destination changed during verification";
	case WF_STOP_CANCELLED:     return "cancelled";
	case WF_STOP_IO_ERROR:      return "I/O error during verification";
	case WF_STOP_RECORD_FAILED: return "verified record could not be saved";
	}
	return "unknown";
}

#ifndef WF_VERIFY_H
#define WF_VERIFY_H

#include <stdbool.h>

#include "../base/wf_status.h"
#include "../config/wf_config.h"

/*
 * Which integrity checks a transfer needs, and the order in which its stages
 * may happen. Pure policy: nothing here reads a file. The cryptography is the
 * existing wf_integrity_* code; the identity and ownership proofs are the
 * existing owned_* code. This layer only decides what is required and refuses
 * to let a later stage run before an earlier one has succeeded.
 *
 * What it does NOT claim: a read-back can be served from the cache, so a
 * VERIFIED stage shows that the copy is complete and uncorrupted as the system
 * sees it, not that it survived a power cut. Durability is its own stage
 * (DESTINATION_DURABLE: the destination was flushed) and the crash-resilience
 * evidence (S14) is separate and still outstanding.
 */
typedef enum
{
	WF_KIND_COPY,
	WF_KIND_MOVE_SAME_VOLUME, /* a rename: no data is copied */
	WF_KIND_MOVE_CROSS_VOLUME /* copy, verify, then remove the source */
} WfTransferKind;

typedef struct
{
	bool copies_data;
	bool read_back; /* the destination must be read back and compared */
	WfCopyVerify method; /* WF_VERIFY_NONE iff !read_back */
	bool removes_source; /* the source goes away after a successful verify */
} WfVerifyPlan;

/*
 * COPY follows the configured method (SHA-256 by default; `none` is an explicit
 * opt-out). A cross-volume MOVE always reads back: a configured `none` is
 * raised to SHA-256, because the source is removed afterwards and nothing else
 * would show that the destination matches. A same-volume MOVE never hashes: the
 * object itself is renamed. WF_E_INVAL for an unknown kind or method.
 */
WfStatus wf_verify_plan(
        WfTransferKind kind, WfCopyVerify configured, WfVerifyPlan *out);

typedef enum
{
	WF_VERDICT_MATCH,
	WF_VERDICT_MISMATCH, /* the bytes differ */
	WF_VERDICT_CHANGED,  /* source or destination identity/size changed */
	WF_VERDICT_CANCELLED,
	WF_VERDICT_ERROR /* I/O or provider failure: nothing is known */
} WfVerifyVerdict;

typedef enum
{
	WF_STAGE_NEW,
	WF_STAGE_COPIED,
	WF_STAGE_SOURCE_ACCEPTED, /* the source was the object the user chose */
	WF_STAGE_DESTINATION_DURABLE,
	WF_STAGE_VERIFIED,
	WF_STAGE_VERIFIED_RECORDED, /* the VERIFIED record is durable */
	WF_STAGE_DONE,              /* nothing further may be done */
	WF_STAGE_STOPPED            /* failed or refused: retain everything */
} WfStage;

typedef enum
{
	WF_STOP_NONE,
	WF_STOP_OUT_OF_ORDER,
	WF_STOP_COPY_FAILED,
	WF_STOP_SOURCE_REJECTED,
	WF_STOP_NOT_DURABLE,
	WF_STOP_MISMATCH,
	WF_STOP_CHANGED,
	WF_STOP_CANCELLED,
	WF_STOP_IO_ERROR,
	WF_STOP_RECORD_FAILED
} WfStopReason;

typedef struct
{
	WfVerifyPlan plan;
	WfStage stage;
	WfStopReason stop;
} WfStageMachine;

void wf_stage_init(WfStageMachine *m, const WfVerifyPlan *plan);

/* Each call reports one stage. It returns true when the transfer may go on.
 * Reporting a failure, or reporting a stage out of order, stops the machine for
 * good: every later call returns false and no deletion is ever authorized.
 * Stopped means "keep the source and keep whatever was written, and let the
 * recovery record say so". */
bool wf_stage_copied(WfStageMachine *m, bool copy_succeeded);
bool wf_stage_source_accepted(WfStageMachine *m, bool accepted);
bool wf_stage_destination_durable(WfStageMachine *m, bool flushed);
bool wf_stage_verified(WfStageMachine *m, WfVerifyVerdict verdict);
bool wf_stage_verified_recorded(WfStageMachine *m, bool record_durable);

/* A copy completes after DESTINATION_DURABLE without read-back, or VERIFIED
 * with it. A cross-volume move also requires VERIFIED_RECORDED. */
bool wf_stage_complete(WfStageMachine *m);

/* The only way to learn that deleting the source is allowed: the plan removes
 * it AND the destination was copied from the accepted source, flushed, read
 * back equal, and that result is on record. Never true after a stop. */
bool wf_stage_may_remove_source(const WfStageMachine *m);

const char *wf_stage_stop_text(WfStopReason reason);

#endif

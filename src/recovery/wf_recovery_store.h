#ifndef WF_RECOVERY_STORE_H
#define WF_RECOVERY_STORE_H
#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
typedef struct WfRecoveryStore WfRecoveryStore;
typedef enum
{
	WF_RECOVERY_LOCAL_ABSENT,
	WF_RECOVERY_LOCAL_READY,
	WF_RECOVERY_LOCAL_JOURNAL_ONLY,
	WF_RECOVERY_LOCAL_METADATA_ONLY,
	WF_RECOVERY_LOCAL_ABANDON_INTENT,
	WF_RECOVERY_LOCAL_ABANDON_JOURNAL_DELETED,
	WF_RECOVERY_LOCAL_ABANDON_METADATA_DELETED,
	WF_RECOVERY_LOCAL_ABANDON_AUDIT_PENDING,
	WF_RECOVERY_LOCAL_ABANDONED,
	WF_RECOVERY_LOCAL_MANUAL
} WfRecoveryLocalState;
typedef enum
{
	WF_RECOVERY_ABANDON_HOLD,
	WF_RECOVERY_ABANDON_PAIR,
	WF_RECOVERY_ABANDON_AUDIT_ONLY,
	WF_RECOVERY_ABANDON_DONE
} WfRecoveryAbandonAction;
/* Result of a snapshot lookup: a record, none present/left, or a refusal. */
typedef enum
{
	WF_RECOVERY_LOOKUP_ERROR = -1,
	WF_RECOVERY_LOOKUP_NONE = 0,
	WF_RECOVERY_LOOKUP_FOUND = 1
} WfRecoveryLookup;
typedef struct
{
	wchar_t recovery_id[37];
	wchar_t volume_identity[128];
	bool acknowledged;
	WfRecoveryLocalState state;
	WfRecoveryAbandonAction action;
	DWORD error;
	const char *reason; /* borrowed static diagnostic */
} WfRecoveryLocalInfo;
typedef struct
{
	unsigned journal_deleted, metadata_deleted;
	DWORD error;
	bool intent_logged, logged;
	WfRecoveryLocalState state_before, state_after;
	WfRecoveryAbandonAction action;
} WfRecoveryAbandonReport;
/* root is a dedicated local state directory, never the recorded target.
 * Opening, listing and abandoning perform local metadata I/O only. */
bool wf_recovery_store_open(
        const wchar_t *root, bool create, WfRecoveryStore **out, DWORD *error);
void wf_recovery_store_close(WfRecoveryStore *store);
const char *wf_recovery_local_state_name(WfRecoveryLocalState state);
const char *wf_recovery_abandon_action_name(WfRecoveryAbandonAction action);
/* Inspection snapshots are stable for indexed enumeration; refresh observes
 * external changes. Own put/abandon operations invalidate that snapshot. */
bool wf_recovery_store_refresh(WfRecoveryStore *store, DWORD *error);
WfRecoveryLookup wf_recovery_store_inspect(WfRecoveryStore *store,
        const wchar_t *id,
        WfRecoveryLocalInfo *info,
        DWORD *error);
bool wf_recovery_store_put(WfRecoveryStore *store,
        const wchar_t *id,
        const wchar_t *volume_identity,
        const void *journal,
        size_t journal_bytes,
        DWORD *error);
/* FOUND: record, NONE: enumeration end, ERROR: refused. */
WfRecoveryLookup wf_recovery_store_get(WfRecoveryStore *store,
        size_t index,
        WfRecoveryLocalInfo *info,
        DWORD *error);
/* Exact lowercase UUID is required twice. Wrong confirmation performs no
 * deletion. */
bool wf_recovery_store_abandon(WfRecoveryStore *store,
        const wchar_t *id,
        const wchar_t *typed_id,
        WfRecoveryAbandonReport *report);
#endif

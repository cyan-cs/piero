#ifndef WF_OWNED_INTERNAL_H
#define WF_OWNED_INTERNAL_H

#include "../path/wf_canonical_win.h"
#include "owned_delete.h"

#define OPEN_FLAGS (FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT)
#define GUID_ROOT_UNITS 49

/* All handles and journal strings belong to the token. The parent handle
 * pins the ancestor namespace; owner denies writes/deletes; payload denies
 * writes/deletes and already has DELETE access. No reopen by name takes place
 * during deletion. */
struct WfVerifiedOwned
{
	WfOwnedKind kind;
	WfOwnershipRecord record;
	wchar_t *parent_path, *rel_payload, *final_path;
	HANDLE parent;
	unsigned open_attempts;
	HANDLE directory, owner, payload, destination;
	bool directory_payload;
	bool nonce_proven, owner_deleted;
	uint8_t proof_nonce[32];
	WfOwnedLocalRecord local_record;
	WfOwnedLogRecord log_record;
	WfLiveManifest *live_manifest;
	FILE_ID_INFO live_id;
	wchar_t *live_path;
	uint64_t live_limit;
	bool live_directory;
};

/* Private proof modules share a retained token, never raw path authority. */
bool wfo_nonzero(const void *data, size_t n);
bool wfo_id_equal(const void *a,
        const void *b); /* both are FILE_ID_INFO; may be unaligned */
bool wfo_record_shape(const WfOwnershipRecord *r, bool require_temp_id);
wchar_t *wfo_join(const wchar_t *parent, const wchar_t *child);
WfOwnedResult wfo_set_result(WfOwnedReport *r,
        WfOwnedResult result,
        const char *reason,
        DWORD error);
bool wfo_object_matches(HANDLE h,
        const FILE_ID_INFO *expected,
        bool directory,
        const wchar_t *path,
        WfOwnedReport *report);
HANDLE wfo_open_id(HANDLE hint, const FILE_ID_INFO *id, DWORD access);
bool wfo_contents_match(HANDLE dir,
        bool owner_present,
        bool payload_present,
        WfOwnedReport *report);
bool wfo_pin_parents(WfVerifiedOwned *o, WfOwnedReport *report);
bool wfo_leaf_valid(const wchar_t *leaf);
bool wfo_transaction_matches(WfVerifiedOwned *o, WfOwnedReport *report);
bool wfo_committed_matches(WfVerifiedOwned *o, WfOwnedReport *report);
bool wfo_local_matches(WfVerifiedOwned *o, WfOwnedReport *report);
bool wfo_live_matches(WfVerifiedOwned *o, WfOwnedReport *report);
bool wfo_log_matches(WfVerifiedOwned *o, WfOwnedReport *report);
extern const DWORD wfo_retry_ms[6];

#endif

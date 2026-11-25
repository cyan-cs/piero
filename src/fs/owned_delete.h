#ifndef WF_OWNED_DELETE_H
#define WF_OWNED_DELETE_H

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
	WF_OWNED_COPY_PAYLOAD,
	WF_OWNED_LIVE_TEMP_CHILD,
	WF_OWNED_TXN_OWNER,
	WF_OWNED_TXN_DIRECTORY,
	WF_OWNED_JOURNAL,
	WF_OWNED_JOURNAL_META,
	WF_OWNED_WORKER_BINARY,
	WF_OWNED_IMAGE_CACHE,
	WF_OWNED_LOG
} WfOwnedKind;

typedef enum
{
	WF_OWNED_VERIFIED,
	WF_OWNED_MANUAL,
	WF_OWNED_ABSENT,
	WF_OWNED_DELETED,
	WF_OWNED_RECOVERY_REQUIRED
} WfOwnedResult;

/* Borrowed journal fields. Only the journal reader may assert durability;
 * discovery by pathname, timestamps or a matching ID cannot assert it.
 * parent_path is the recorded volume-GUID final path, not the current path.
 * rel_payload is exactly .wfile-txn-<lowercase UUID>\payload.bin. */
typedef struct
{
	bool intent_durable, temp_id_durable;
	uint8_t intent_txn[16], txn[16], nonce[32];
	uint64_t intent_source_size, source_size;
	FILE_ID_INFO parent, directory, owner, payload;
	const wchar_t *parent_path, *rel_payload;
} WfOwnershipRecord;

typedef struct
{
	DWORD error; /* captured Win32 error, zero for predicate mismatch */
	unsigned attempts;      /* disposition attempts; never more than six */
	unsigned open_attempts; /* DELETE-access acquisition; never more than
	                           six */
	DWORD restore_error;    /* readonly restoration failure, if any */
	DWORD ex_error; /* original extended-disposition error, if any */
	bool used_fallback;
	const char *reason; /* static diagnostic, not authority */
} WfOwnedReport;

typedef struct WfVerifiedOwned WfVerifiedOwned;

/* No deletion. Discovers by ID, acquires a named handle, then fully verifies
 * and retains that same locked handle until deletion/release.
 * ABSENT means the recorded ID AND recorded location are absent. It does not
 * claim ABORTED/COMMITTED: the caller must inspect the final destination.
 * This entry accepts only COPY_PAYLOAD; other kinds need distinct entry proofs.
 * A directory transaction after a crash must never be submitted as a file. */
WfOwnedResult wf_owned_verify(WfOwnedKind kind,
        const WfOwnershipRecord *record,
        WfVerifiedOwned **out,
        WfOwnedReport *report);

/* No pathname or raw HANDLE is accepted as deletion authority. Rechecks the
 * retained object and transaction before disposition on that same handle.
 * Consumes the object handle on success; release the token in every case. */
WfOwnedResult wf_owned_delete(WfVerifiedOwned *owned, WfOwnedReport *report);
void wf_owned_release(WfVerifiedOwned *owned);

/* The editor's save temporary. The caller created `path` with CREATE_NEW a
 * moment ago and took `created` from the creating handle. This reopens the
 * name and deletes it only if the object found is that very file: the same
 * FileId, a plain file, one link, not a reparse point. A name that now holds
 * anything else is left alone (MANUAL), as is a name that is gone (ABSENT). */
WfOwnedResult wf_owned_delete_save_temp(const wchar_t *path,
        const FILE_ID_INFO *created,
        WfOwnedReport *report);

/* The copy engine's staging folder and the folders it made inside it. Same
 * contract as the save temporary, for a directory: `created` is the FileId
 * taken when the folder was made, the name must still hold exactly that
 * folder (not a link), and it is removed only if it is empty. Never
 * recursive. */
WfOwnedResult wf_owned_delete_empty_dir(const wchar_t *path,
        const FILE_ID_INFO *created,
        WfOwnedReport *report);

/* Structural validation, also used before opening any journal-derived path. */
bool wf_owned_record_valid(const WfOwnershipRecord *record);

/* This factory accepts only TXN_OWNER: exact committed destination plus
 * owner nonce and IDs. It retains DELETE access to the transaction directory.
 * After owner deletion, finish consumes its retained nonce receipt and removes
 * that same directory handle only if still empty. Reconstructed empty-dir
 * records are MANUAL; no nonce-free TXN_DIRECTORY factory exists. */
WfOwnedResult wf_owned_verify_committed(WfOwnedKind kind,
        const WfOwnershipRecord *record,
        const wchar_t *final_leaf,
        int directory_payload,
        WfVerifiedOwned **out,
        WfOwnedReport *report);
WfOwnedResult wf_owned_finish_committed(
        WfVerifiedOwned *owned, WfOwnedReport *report);
/* Local journal abandonment never opens a target volume. Dedicated local
 * root marker, exact FileIds, bounded object size and kind-specific headers
 * form a separate proof; caller supplies the recorded local identities. */
#pragma pack(push, 1)
typedef struct
{
	uint8_t magic[8];
	FILE_ID_INFO root;
	uint8_t nonce[32];
} WfOwnedRootMarker;
typedef struct
{
	uint8_t magic[8], root_nonce[32], object_nonce[32];
	wchar_t recovery_id[37];
} WfOwnedLocalHeader;
typedef struct
{
	WfOwnedLocalHeader header;
	wchar_t volume_identity[128];
	FILE_ID_INFO journal;
	uint8_t journal_nonce[32];
	uint64_t journal_size;
} WfOwnedLocalMeta;
#pragma pack(pop)
/* These are written to and read from the recovery store as raw bytes: a
 * changed size means an incompatible on-disk format, not a refactor. */
_Static_assert(sizeof(wchar_t) == 2, "local recovery records use UTF-16");
_Static_assert(sizeof(WfOwnedRootMarker) == 64, "root marker format changed");
_Static_assert(
        sizeof(WfOwnedLocalHeader) == 146, "local header format changed");
_Static_assert(
        sizeof(WfOwnedLocalMeta) == 466, "local metadata format changed");
typedef struct
{
	const wchar_t *root_path;
	FILE_ID_INFO root, marker, object;
	uint8_t root_nonce[32], object_nonce[32];
	wchar_t recovery_id[37];
	uint64_t object_size;
} WfOwnedLocalRecord;
WfOwnedResult wf_owned_verify_local(WfOwnedKind kind,
        const WfOwnedLocalRecord *record,
        WfVerifiedOwned **out,
        WfOwnedReport *report);
/* Live-only manifest: begin requires durable INTENT plus the created txn/owner
 * identities and no payload/TEMP_ID. It atomically creates the empty root; the
 * callback must append and flush TEMP_ID before returning success. Children are
 * atomically CREATE_NEW through a retained manifested parent handle; adopted
 * paths cannot be registered. Cleanup visits only recorded handles in reverse
 * order. Never rehydrate this manifest after a crash: directory recovery
 * remains MANUAL. */
typedef struct WfLiveManifest WfLiveManifest;
typedef bool (*WfLiveDurableTempId)(
        void *context, const WfOwnershipRecord *candidate);
WfOwnedResult wf_owned_live_begin(const WfOwnershipRecord *record,
        WfLiveDurableTempId durable_temp_id,
        void *context,
        WfLiveManifest **out,
        WfOwnedReport *report);
WfOwnedResult wf_owned_live_create(WfLiveManifest *manifest,
        size_t parent_index,
        const wchar_t *leaf,
        int directory,
        const void *bytes,
        DWORD count,
        uint64_t source_limit,
        size_t *index,
        WfOwnedReport *report);
WfOwnedResult wf_owned_live_cleanup(
        WfLiveManifest *manifest, size_t *deleted, WfOwnedReport *report);
void wf_owned_live_release(WfLiveManifest *manifest);
/* Diagnostic rotation has a separate closed proof. All IDs/nonces and the
 * exact local canonical root are recorded at creation; every bounded record
 * CRC and the generation/slot relation are revalidated on the retained handle.
 */
typedef struct
{
	const wchar_t *root_path;
	FILE_ID_INFO root, marker, object;
	uint8_t root_nonce[32], object_nonce[32];
	uint64_t generation, object_size;
	uint32_t slot;
} WfOwnedLogRecord;
WfOwnedResult wf_owned_verify_log(const WfOwnedLogRecord *record,
        WfVerifiedOwned **out,
        WfOwnedReport *report);
#endif

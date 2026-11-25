#ifndef WF_INTEGRITY_WIN_H
#define WF_INTEGRITY_WIN_H

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct WfIntegrityJob WfIntegrityJob;
typedef enum
{
	WF_INTEGRITY_MATCH,
	WF_INTEGRITY_MISMATCH,
	WF_INTEGRITY_CHANGED,
	WF_INTEGRITY_CANCELLED,
	WF_INTEGRITY_ERROR
} WfIntegrityResult;
typedef struct
{
	DWORD error;
	LONG cng_status;
	uint64_t source_bytes, destination_bytes;
} WfIntegrityReport;
typedef bool (*WfIntegrityCancel)(void *context);

/* Worker-confined, noncopyable job. A fresh OS-generated 256-bit key, never
 * serialized. No digest/key leaves this API. *out must initially be NULL.
 * Release on every exit path, including a failed begin: a provider cleanup
 * failure may leave a closing job in *out for explicit cleanup retry. */
bool wf_integrity_begin(WfIntegrityJob **out, WfIntegrityReport *report);
/* Returns true and clears *job on release. A provider cleanup failure returns
 * false, erases the job key and retains ownership in *job for a later bounded
 * retry. Do not discard that pointer. No further comparisons are allowed. */
bool wf_integrity_end(WfIntegrityJob **job);
/* Read-only measurement, NOT commit/deletion/ownership authority. Caller owns
 * synchronous handles, offsets and FILE_SHARE_READ guards throughout. Reads
 * the stream addressed by each handle (unnamed data or a named $DATA stream).
 * The caller must prove the complete stream set; attributes/durability are separate.
 * Expected identities and size are mandatory, rechecked before/after reading.
 * Must run off the UI thread. Cancellation is cooperative between reads. */
WfIntegrityResult wf_integrity_compare(WfIntegrityJob *job,
        HANDLE source,
        const FILE_ID_INFO *source_id,
        HANDLE destination,
        const FILE_ID_INFO *destination_id,
        uint64_t size,
        WfIntegrityCancel cancelled,
        void *context,
        WfIntegrityReport *report);
#endif

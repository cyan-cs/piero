#ifndef WF_DIAG_INTERNAL_H
#define WF_DIAG_INTERNAL_H

#include "../base/wf_buf.h"
#include "../path/wf_canonical_win.h"
#include "wf_diag.h"
#include "wf_diag_format.h"

/* Private logger state. Open owns all handles and the canonical path; writes
 * hold lock across validation, rotation, and append. Stop producers before
 * close. Slot metadata records the verified identity and size used by owned
 * deletion. */
typedef struct
{
	bool present;
	WfDiagDiskHeader header;
	uint64_t bytes;
} LogSlot;
struct WfDiagnostics
{
	SRWLOCK lock;
	HANDLE root, marker, file;
	WfCanonicalPath canonical;
	FILE_ID_INFO root_id, marker_id;
	uint8_t nonce[32];
	LogSlot slots[WF_DIAG_SLOTS];
	unsigned active;
	WfDiagLevel level;
	bool paths, poisoned;
};

wchar_t *wf_diag_path_of(WfDiagnostics *log, const wchar_t *leaf);
wchar_t *wf_diag_slot_path(WfDiagnostics *log, unsigned slot);
bool wf_diag_write_bytes(
        HANDLE h, const void *bytes, DWORD count, DWORD *error);
bool wf_diag_marker_open(WfDiagnostics *log, DWORD *error);
bool wf_diag_slots_open(WfDiagnostics *log, DWORD *error);
bool wf_diag_rotate(WfDiagnostics *log, DWORD *error);
bool wf_diag_equal_id(const void *a,
        const void *b); /* both are FILE_ID_INFO; may be unaligned */
bool wf_diag_nonzero(const uint8_t *p, size_t n);
bool wf_diag_plain(HANDLE h,
        bool directory,
        uint64_t *bytes,
        FILE_ID_INFO *id,
        DWORD *error);
bool wf_diag_exact_path(HANDLE h, const wchar_t *expected, DWORD *error);
bool wf_diag_known_entries(WfDiagnostics *log, bool empty, DWORD *error);
bool wf_diag_root_valid(WfDiagnostics *log, DWORD *error);
bool wf_diag_read_file(
        HANDLE h, unsigned char **out, size_t *count, DWORD *error);
bool wf_diag_header_read(WfDiagnostics *log,
        HANDLE file,
        unsigned slot,
        LogSlot *result,
        DWORD *error);
bool wf_diag_format_event(WfDiagnostics *log,
        const WfDiagEvent *event,
        WfBuf *text,
        DWORD *error);

#endif

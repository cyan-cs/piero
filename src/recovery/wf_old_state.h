#ifndef WF_OLD_STATE_H
#define WF_OLD_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <windows.h>

/*
 * A read-only look at a state folder's recovery information (DR-DRAFT-017).
 * It never creates, repairs, copies, moves or deletes anything. Used for the
 * active folder's journal and, when both %LOCALAPPDATA%\piero and the older
 * \wfile exist, for what the older one still holds. Anything that cannot be
 * read is "unknown", never "nothing".
 */
typedef struct
{
	size_t unresolved;   /* records not proven finished or abandoned */
	unsigned long error; /* the last Win32 error seen, 0 if none */
	bool store_unknown;  /* the store or a record cannot be read/judged */
	bool limit;          /* more records than one look reads */
	bool copy_legacy;    /* receipt inventory in the old format */
	bool copy_invalid;   /* receipt inventory unreadable or damaged */
	bool receipts; /* receipts are listed (an inventory, not a proof) */
	bool drafts;   /* draft-folder entries exist, or their presence could not be checked */
} WfStateReport;

/* The recovery store under `root`. A store that is not there counts nothing. */
void wf_state_look_store(WfStateReport *r, const wchar_t *root);
/* The copy receipt inventory file `path` (a missing file is nothing). */
void wf_state_look_copy_journal(WfStateReport *r, const wchar_t *path);
/* Sets r->drafts for any non-dot entry, including a subdirectory, or when
 * allocation/open failure prevents checking for entries. */
void wf_state_look_drafts(WfStateReport *r, const wchar_t *dir);

/* Copying and replacing saves must stay off: something is unresolved or
 * could not be judged. Receipts and drafts alone only call for a notice. */
bool wf_state_report_blocks(const WfStateReport *r);
/* Anything at all was found. */
bool wf_state_report_any(const WfStateReport *r);

#endif

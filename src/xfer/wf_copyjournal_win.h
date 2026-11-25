#ifndef WF_COPYJOURNAL_WIN_H
#define WF_COPYJOURNAL_WIN_H

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Version 2 stores bounded, percent-escaped UTF-8 receipts for manual review.
 * It is not authenticated ownership proof. Inspection never deletes, renames,
 * restores or truncates anything. Legacy, unknown and partial logs are kept;
 * they cannot be opened for append until explicitly reviewed outside piero. */
typedef struct WfCopyJournal WfCopyJournal;

/* NULL path: a journal that records nothing (tests of the engine alone). */
WfCopyJournal *wf_cj_open(const wchar_t *path);

/* Why a journal could not be opened. A refusal because of what the file holds
 * (an old or unknown format) is a gate working as designed; the others are
 * failures of the system, with the Win32 error taken at the failing call. */
typedef enum
{
	WF_CJ_OPEN_OK,
	WF_CJ_OPEN_NOMEM,
	WF_CJ_OPEN_PROCESS, /* the process times could not be read */
	WF_CJ_OPEN_PATH,    /* the path could not be made absolute */
	WF_CJ_OPEN_FILE,    /* the file could not be opened for append */
	WF_CJ_OPEN_LOCK,    /* the file could not be locked */
	WF_CJ_OPEN_INSPECT, /* the content could not be read */
	WF_CJ_OPEN_FORMAT, /* old, unknown or damaged content: not for append */
	WF_CJ_OPEN_HEADER /* the header of a new journal could not be written */
} WfCopyJournalOpenWhy;

/* The same as wf_cj_open; on failure *why says why and *error is the Win32
 * error (0 when there is none, as for a format that is simply not accepted). */
WfCopyJournal *wf_cj_open_why(
        const wchar_t *path, WfCopyJournalOpenWhy *why, DWORD *error);
void wf_cj_close(WfCopyJournal *j);

bool wf_cj_intent(WfCopyJournal *j, const wchar_t *tmp_path);
bool wf_cj_created(
        WfCopyJournal *j, const wchar_t *tmp_path, const FILE_ID_INFO *id);
bool wf_cj_aside(WfCopyJournal *j,
        const wchar_t *dest,
        const wchar_t *aside,
        const FILE_ID_INFO *old_id);
bool wf_cj_discard(WfCopyJournal *j, const FILE_ID_INFO *old_id);
bool wf_cj_verified(WfCopyJournal *j, const FILE_ID_INFO *id);
bool wf_cj_gone(WfCopyJournal *j, const FILE_ID_INFO *id);

typedef enum
{
	WF_CJ_MISSING,
	WF_CJ_EMPTY,
	WF_CJ_RECEIPTS,
	WF_CJ_LEGACY,
	WF_CJ_INVALID
} WfCopyJournalFormat;

typedef struct
{
	WfCopyJournalFormat format;
	DWORD error;
	uint64_t bytes;
	size_t records; /* inventory entries, not proven recovery operations */
} WfCopyJournalInfo;

/* Read-only, bounded inspection. Missing is success; legacy is readable but
 * not eligible for append. Invalid syntax, unsafe file type and IO fail closed.
 * No supplied path or File ID in a receipt is used as operation authority. */
bool wf_cj_inspect(const wchar_t *path, WfCopyJournalInfo *out);

#endif

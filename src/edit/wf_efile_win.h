#ifndef WF_EFILE_WIN_H
#define WF_EFILE_WIN_H

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>

#include "wf_eenc.h"

/* Characters in the longest path the editor saves (extended form). */
#define WF_EFILE_PATH 2048

/*
 * Loading accepts only text that can be encoded back byte for byte. Replacing
 * saves build and verify a new sibling, preserve supported metadata/streams,
 * then journal two handle-based, no-clobber renames of identified objects.
 * Every replacement preserves its original at <path>~ because sharing guards
 * cannot freeze the named-stream namespace. If that name is occupied the
 * next free one is used (<path>~2 ... <path>~9); a backup is never
 * overwritten or removed, by the save or by anything else in the product.
 * With nine in the way the save is refused and the user clears some. A
 * crash between renames leaves both objects for manual recovery.
 *
 * WF_SV_IO can leave named recovery artifacts; inspect kept_temp/original.
 * WF_SV_COMMITTED_UNVERIFIED means the new bytes are already at the target,
 * but the next-save baseline could not be established. The caller must block
 * blind retries and require reload or Save As. It must not report unchanged.
 *
 * Namespace checks are not a filesystem transaction. Adversarial parent/name
 * changes and failures of recovery still require separate release validation.
 */

typedef struct
{
	bool valid;
	FILE_ID_INFO id;
	uint64_t size;
	uint64_t mtime; /* FILETIME ticks */
	DWORD attributes;
	DWORD links;
	uint8_t sha256[32]; /* of the bytes as they are on disk */
} WfFileStamp;

typedef enum
{
	WF_FL_OK,
	WF_FL_MISSING,
	WF_FL_NOT_FILE,
	WF_FL_LINK,
	WF_FL_CLOUD,
	WF_FL_TOO_BIG,
	WF_FL_BINARY,
	WF_FL_AMBIGUOUS, /* ask: `enc` is the candidate if candidate_ok */
	WF_FL_BAD_ENCODING,
	WF_FL_CHANGED, /* changed while it was being read */
	WF_FL_IO
} WfLoadVerdict;

typedef struct
{
	WfLoadVerdict verdict;
	DWORD error;
	const char *reason;
	size_t bad_offset;
	bool candidate_ok;
	WfEncoding enc;
	char *utf8; /* WF_FL_OK: owned, valid UTF-8 */
	size_t utf8_n;
	WfFileStamp stamp;
} WfLoaded;

/* `force`, when given, is the encoding the user chose for an ambiguous file.
 * Returns WF_OK whenever a verdict was reached; other statuses are
 * allocation failures. */
/* A load or a save can be stopped at two safe places, when the thread it runs
 * on has a cancel token bound (wf_blocking.h) and the token is asked: while a
 * file is being read, and while a save is still writing the new content to
 * its temporary file. Once a save begins to replace the file it cannot be
 * stopped and the token is not looked at. */

WfStatus wf_efile_load(const wchar_t *path,
        size_t max_bytes,
        unsigned fallback_codepage,
        const WfEncoding *force,
        WfLoaded *out);
void wf_efile_loaded_free(WfLoaded *l);

/* A hint that the file at `path` is no longer the one `was` describes (another
 * program changed or replaced it, or it is gone): its id, size or write time
 * differ. It reads attributes only, never the bytes. It authorizes nothing: a
 * save checks the bytes again whatever this said or missed. False when it
 * cannot tell. */
bool wf_efile_probably_changed(const wchar_t *path, const WfFileStamp *was);

typedef enum
{
	WF_SV_OK,
	WF_SV_CONFLICT, /* the file changed on disk since it was read */
	WF_SV_GONE,     /* deleted, or replaced by a different file */
	WF_SV_EXISTS,   /* a new file's name is taken */
	WF_SV_UNSAFE,   /* a replacement would lose something: see reason */
	WF_SV_ENCODE,   /* the text cannot be written in this encoding */
	WF_SV_IO,       /* consult kept_temp/kept_original for partial swaps */
	WF_SV_COMMITTED_UNVERIFIED /* bytes were committed; do not retry without reload */
} WfSaveVerdict;

typedef struct
{
	WfSaveVerdict verdict;
	DWORD error;
	const char *reason;
	size_t bad_offset;                /* ENCODE: UTF-8 offset */
	wchar_t kept_temp[WF_EFILE_PATH]; /* the only good copy, if non-empty */
	/* the previous version, when it could not be put back or removed */
	wchar_t kept_original[WF_EFILE_PATH];
	WfFileStamp stamp; /* OK: the file as it is now */
	bool backup_made;
	bool backup_skipped;
} WfSaveResult;

typedef struct
{
	bool backup; /* Compatibility input: replacements always retain <name>~. */
} WfSaveOptions;

/*
 * Where the swap of a replacing save is recorded. A save moves the original
 * aside and then gives the new file its name; between the two the original
 * name does not exist, so the move is written to this journal first (the copy
 * engine's, see wf_copyjournal_win.h). The journal is an inventory for manual
 * recovery; its contents alone never authorize filesystem mutation.
 *   path    the journal file
 *   NULL    no journal is available: saves that replace a file are refused
 * Never called (unit tests): saves do not journal.
 */
void wf_efile_set_journal(const wchar_t *path);

/* Tests only: treat every path as one in a WSL distribution (no file
 * identity, in-place save with a <path>~ backup). */
extern bool wf_efile_test_force_wsl;

/*
 * Why there is no journal. A replacing save needs one, and "unavailable" is
 * not one fact: an old or damaged record that is kept for review is a gate
 * working as designed (the save is refused, nothing is wrong with the
 * system), while a state folder or a record that cannot be inspected is a
 * failure with its own Win32 error. The save reports each as what it is,
 * instead of one sentence with error 0. None of these unlocks anything.
 *   WF_EFJ_UNKNOWN   no reason known (wf_efile_set_journal(NULL))
 *   WF_EFJ_NO_STATE  the local state folder could not be found or made
 *   WF_EFJ_INSPECT   the record could not be inspected (error is Win32's)
 *   WF_EFJ_FORMAT    the record is in an old or unknown format: kept as it
 *                    is, replacing saves stay locked
 */
typedef enum
{
	WF_EFJ_UNKNOWN,
	WF_EFJ_NO_STATE,
	WF_EFJ_INSPECT,
	WF_EFJ_FORMAT
} WfEfileJournalGate;

void wf_efile_set_journal_unavailable(WfEfileJournalGate why, DWORD error);

/* Tests only: called between the two renames of a replacing save; returning
 * true makes the save stop right there, as a process that died would. */
extern bool (*wf_efile_test_after_aside)(
        const wchar_t *dest, const wchar_t *aside);

/* Saves over the file that was read (`was` is its stamp from the load). */
WfStatus wf_efile_save_over(const wchar_t *path,
        const WfFileStamp *was,
        const char *utf8,
        size_t n,
        WfEncoding enc,
        const WfSaveOptions *opt,
        WfSaveResult *out);

/* Writes a new file; refuses a name that exists. */
WfStatus wf_efile_save_new(const wchar_t *path,
        const char *utf8,
        size_t n,
        WfEncoding enc,
        WfSaveResult *out);

/* Creates `tmp` (CREATE_NEW) with the permissions of the existing file `model`
 * from the start, for the save temporary. The handle is returned open (no delete sharing). False (with *error) if the model's permissions cannot
 * be read or the file cannot be created. */
bool wf_efile_create_temp_like(
        const wchar_t *model, const wchar_t *tmp, HANDLE *out, DWORD *error);

/* SHA-256 of a buffer. */
bool wf_efile_sha256(const void *data, size_t n, uint8_t out[32]);

#endif

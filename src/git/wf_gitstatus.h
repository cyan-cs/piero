#ifndef WF_GITSTATUS_H
#define WF_GITSTATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"

/*
 * What `git status --porcelain=v2 -z` says, as a table that can be looked up
 * (DR-DRAFT-038). Text only, no I/O, no Git.
 *
 * Records are sorted by raw path bytes for binary search. Covering lookups
 * also check ancestor prefixes for untracked directories. Paths are Git's
 * '/'-separated, repository-relative bytes; the parser does not decode or
 * validate their UTF-8. An untracked directory has a trailing '/'.
 * Both letters Git gives are kept, so a change that is staged can be told from
 * one that is not.
 */
typedef struct
{
	const char *path; /* into the table's own copy; NUL-terminated */
	size_t path_n;
	const char *orig; /* a renamed file's old path, else NULL */
	char index;       /* porcelain X code: staged state */
	char work;        /* porcelain Y code; '?' for an untracked record */
	bool conflict;
} WfGitEntry;

typedef struct
{
	WfGitEntry *e; /* sorted by path */
	size_t n;
	char *blob; /* the text the entries point into */
} WfGitStatus;

/* The most records one status is parsed from: the table is sized by the records
 * of the input (never by its bytes), and more than this is WF_E_RANGE, so that
 * output within the byte limit of the Git run cannot ask for gigabytes. */
#define WF_GITSTATUS_MAX_ENTRIES ((size_t)1 << 20)

/* Copies the output and extracts paths and XY states. Empty records, headers
 * and ignored-file records are skipped. Unsupported record types or malformed
 * parsed fields return WF_E_CORRUPT with an empty result. Other porcelain
 * metadata is skipped rather than fully validated. Empty input yields an
 * empty table. More than WF_GITSTATUS_MAX_ENTRIES records: WF_E_RANGE, empty. */
WfStatus wf_gitstatus_parse(const uint8_t *data, size_t n, WfGitStatus *out);
void wf_gitstatus_free(WfGitStatus *s); /* empties it; safe to repeat */

/* The record for exactly this path (a folder: with its trailing '/'), or NULL. */
const WfGitEntry *wf_gitstatus_find(
        const WfGitStatus *s, const char *path, size_t n);

/* The record for the path, or else the record of the untracked folder that holds
 * it: Git reports a new folder as one record ("newdir/"), so a file inside it
 * has no record of its own and is as new as the folder is. NULL for neither. */
const WfGitEntry *wf_gitstatus_find_covering(
        const WfGitStatus *s, const char *path, size_t n);

/* True when some record lies inside the folder `dir` (given with its trailing
 * '/'): a change anywhere below it, however deep. A record for the folder
 * itself counts. The table is sorted by raw bytes, so what lies inside a
 * folder is one run that starts at the folder's own name. */
bool wf_gitstatus_has_under(const WfGitStatus *s, const char *dir, size_t n);

/* The one letter the list shows: '!' conflict, 'U' untracked, 'D' deleted,
 * 'M' modified, 'R' renamed, 'A' added; 0 for nothing to show. Conflict and
 * untracked states win first. Work-tree deletion takes precedence over staged
 * changes; staged addition/rename otherwise outrank work-tree modifications.
 * A staged copy is displayed as 'A'. */
char wf_gitentry_letter(const WfGitEntry *e);

#endif

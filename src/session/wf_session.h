#ifndef WF_SESSION_H
#define WF_SESSION_H

#include <stdbool.h>
#include <stddef.h>

#include "../base/wf_status.h"

/*
 * Navigation and editor-position snapshot for `piero --continue` (DR-DRAFT-018).
 * Encoding and decoding perform no I/O; the UI applies the snapshot through
 * its ordinary navigation and editor-open rules.
 *
 * The text form is one record per line: UTF-8 strings use doubled backslashes
 * and \xHH byte escapes, keeping control characters out of record boundaries.
 * Anything the reader does not
 * know, a singleton record twice, a number out of range or a path that is
 * not absolute makes the whole text "damaged": it is never repaired or half used.
 */
#define WF_SESSION_VERSION 1
#define WF_SESSION_MAX_BYTES (64u * 1024u)
#define WF_SESSION_MAX_PATH 4096u /* UTF-16 units */
#define WF_SESSION_MAX_NAME 255u
#define WF_SESSION_MAX_SELECTED 256u

typedef enum
{
	WF_SESSION_PANE_LIST,    /* the file list has the keys */
	WF_SESSION_PANE_PREVIEW, /* the preview pane has the keys */
	WF_SESSION_PANE_FULL     /* the full-screen preview */
} WfSessionPane;

/* The editor, when it had the screen: the file and where the cursor and the
 * view were in it. The text is never here (a draft keeps it, DR-DRAFT-018). */
typedef struct
{
	wchar_t *path;    /* absolute */
	size_t line, col; /* the cursor: line, byte offset */
	size_t top, left; /* the first line and column shown */
	bool modified; /* dirty buffer with a recorded draft; may lag the last edit */
} WfSessionEdit;

typedef struct
{
	wchar_t *dir;       /* absolute; the folder that was open */
	wchar_t *entry;     /* the name under the cursor, NULL when none */
	size_t cursor, top; /* row and first visible row, as hints */
	wchar_t **selected; /* names selected in `dir` */
	size_t n_selected;
	WfSessionPane pane;
	size_t pv_top, pv_left; /* preview scroll */
	bool has_edit;          /* the editor had the screen */
	WfSessionEdit edit;
} WfSession;

void wf_session_free(WfSession *s); /* NULL-safe; leaves `s` empty */

/* The body of a record (no header). `*text` is malloc'd. Names that cannot be
 * written (not valid UTF-16) are left out; a `dir` that cannot be is an error
 * (WF_E_INVAL). WF_E_RANGE when the result would pass the size limit.
 * Allocation failure returns WF_E_NOMEM with *text = NULL and *n = 0;
 * optional names are never omitted merely because allocation failed. */
WfStatus wf_session_encode(const WfSession *s, char **text, size_t *n);

/* WF_OK, WF_E_CORRUPT (damaged), or WF_E_NOMEM. On failure `*out` is empty. */
WfStatus wf_session_decode(const char *text, size_t n, WfSession *out);

/* Absolute path of a drive ("C:\...") or a share ("\server\..."), with no
 * "." or ".." component and within the limit: the only paths a session holds. */
bool wf_session_path_ok(const wchar_t *path);

#endif

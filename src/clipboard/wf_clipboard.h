#ifndef WF_CLIPBOARD_H
#define WF_CLIPBOARD_H

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"

/*
 * Files on the Windows clipboard, as the Shell shares them (DR-DRAFT-021).
 *
 * This layer reads and publishes names. It never opens, copies, moves or
 * deletes a file: what is done with the names is done by the transfer engine
 * after its own checks, and a path that came from another program is data, not
 * authority. A clipboard that cannot be understood completely is refused
 * completely.
 */

#define WF_CLIPFILES_MAX 1000u  /* files in one clipboard */
#define WF_CLIPFILES_PATH 4096u /* UTF-16 units in one path */
/* The most a file list can take: the header, then every name at its longest with
 * its terminator, and the empty name that ends the list. A block of another
 * program that is larger holds more than the limits allow (or nothing of use at
 * its end) and is refused before it is copied. */
#define WF_CLIP_HDROP_MAX_BYTES                                                \
	(20u + (size_t)WF_CLIPFILES_MAX * (WF_CLIPFILES_PATH + 1u) * 2u + 2u)

typedef enum
{
	WF_CLIP_OK,          /* `path` holds the files */
	WF_CLIP_NONE,        /* nothing there, or no files (text only) */
	WF_CLIP_UNSUPPORTED, /* virtual files (a mail attachment, say) */
	WF_CLIP_BAD,         /* a damaged or hostile list: nothing is used */
	WF_CLIP_TOO_MANY,    /* more than WF_CLIPFILES_MAX */
	WF_CLIP_BUSY,        /* the clipboard is open in another program */
	WF_CLIP_FAILED       /* the system said no (OLE, memory) */
} WfClipReason;

typedef struct
{
	uint32_t gen;   /* the request this answers */
	bool published; /* the answer to a publish, not to a read */
	WfClipReason why;
	DWORD effect; /* DROPEFFECT_* of "Preferred DropEffect"; 0: not given */
	wchar_t **path; /* owned: absolute drive or share paths */
	size_t n;
} WfClipFiles;

void wf_clipfiles_free(WfClipFiles *f); /* leaves it empty */

/* True for a cut: the move bit is set and the copy bit is not. A clipboard that
 * says nothing about the effect is a copy. */
bool wf_clip_effect_is_cut(DWORD effect);

/* The "Preferred DropEffect" block: a DWORD. Anything else is "not given" (0). */
DWORD wf_clip_parse_effect(const void *data, size_t bytes);

/* The CF_HDROP block (a DROPFILES header and double-NUL terminated names, wide
 * or narrow). Every offset is checked against `bytes`. WF_CLIP_OK fills `out`;
 * any other result leaves it empty. Each path must be an absolute drive or share
 * path with no "." or ".." component and valid UTF-16. */
WfClipReason wf_clip_parse_hdrop(
        const void *data, size_t bytes, WfClipFiles *out);

/* The CF_HDROP block for `path[0..n)`: a DROPFILES header and wide names, each
 * ended by a NUL, the list ended by an empty name. `*bytes` is its size. It is
 * malloc'd for the caller. WF_E_RANGE when there are more than WF_CLIPFILES_MAX
 * or a name is not an acceptable path (so what is published is what a reader of
 * this program would accept). */
WfStatus wf_clip_build_hdrop(
        const wchar_t *const *path, size_t n, void **block, size_t *bytes);

/*
 * The service that reads the clipboard (clipboard worker, its own STA thread).
 * read() asks and returns; the answer comes back through wf_app_on_clip_files()
 * with the same generation. A zeroed port means "no clipboard files".
 */
typedef struct
{
	void *ctx;
	WfStatus (*read)(void *ctx, uint32_t gen);
	/* Puts `n` files on the clipboard as a copy (names only: nothing of their
	 * content is held). The answer is a WfClipFiles with `published` set. */
	WfStatus (*publish)(
	        void *ctx, uint32_t gen, const wchar_t *const *path, size_t n);
} WfClipFilesPort;

#endif

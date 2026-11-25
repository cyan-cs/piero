#ifndef WF_EDIT_H
#define WF_EDIT_H

#include <stdbool.h>
#include <stddef.h>

#include "wf_dir.h"

/*
 * A create/rename request as the UI captured it. Pure data: the identities are
 * the ones observed in the listing the user was looking at, and the backend
 * (wf_edit_win.c) refuses to act if the directory or the source is no longer
 * that object. Strings are borrowed by the port's submit(), which copies them.
 */
typedef enum
{
	WF_EDIT_CREATE_FILE,
	WF_EDIT_CREATE_DIR,
	WF_EDIT_RENAME,
	WF_EDIT_MOVE, /* same-volume move into another folder, name unchanged */
	/* WSL distributions (no file identity), run by wf_wslops_win: */
	WF_EDIT_WSL_COPY,   /* parent\old_name -> dest_parent
 ew_name */
	WF_EDIT_WSL_DELETE, /* permanently deletes parent\old_name (and below) */
	WF_EDIT_WSL_REMOVE_EMPTY /* undo of a create: an empty file or folder only */
} WfEditKind;

typedef struct
{
	WfEditKind kind;
	const wchar_t *parent;   /* absolute path of the listed directory */
	WfFileId parent_id;      /* identity observed with that listing */
	const wchar_t *old_name; /* RENAME / MOVE */
	WfFileId source_id;      /* RENAME / MOVE */
	const wchar_t
	        *new_name; /* for MOVE: the same leaf in the destination */
	const wchar_t *
	        dest_parent; /* MOVE only: absolute path of the target folder */
	WfFileId dest_parent_id; /* MOVE only: identity observed for it */
} WfEditRequest;

/* What the backend reported, free of Win32 types. */
typedef struct
{
	WfStatus st;
	unsigned long error; /* GetLastError() at the failing call, 0 if none */
	bool renamed;        /* the object was created/renamed even if a later
	                        post-check failed */
	const char *name_reason; /* static text when the name was rejected */
	const char *detail;      /* static English text: why a WSL step stopped,
	                            or on success what did not matter to the bytes */
	size_t partial;          /* WSL: entries already changed inside the item
	                            that stopped (a folder copied or emptied in
	                            part) */
} WfEditOutcome;

/* Create/rename/same-volume move execution. A zeroed port makes the forms
 * stop without mutation. submit() copies the request without blocking and
 * reports WF_E_INVAL when busy; it never supersedes an accepted execution.
 * During normal service each answer arrives once with the same gen. Worker
 * shutdown has its own queued-request and result-lifetime contract. */
typedef struct
{
	void *ctx;
	WfStatus (*submit)(void *ctx, uint32_t gen, const WfEditRequest *req);
} WfEditPort;

#endif

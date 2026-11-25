#ifndef WF_COMMANDS_H
#define WF_COMMANDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../i18n/wf_i18n.h"

/*
 * What can be asked for, apart from how it is asked for. A command has a
 * number that never changes, a stable name ("fm.nav.up") that is what a
 * configuration file holds, and a description for the people who read it
 * (the help, the keyboard settings, the hints). Keys are bound to commands in
 * wf_keymap.h; the code that carries a command out never sees a key.
 *
 * Numbers: 1..0xFF are the editor's commands (WfEdCmd in wf_ekeymap.h, the
 * values the editor already used), 0x100.. the file manager's, 0x200.. the
 * preview pane's.
 */

typedef enum
{
	/* bindings can be changed in these */
	WF_CTX_FM,
	WF_CTX_PREVIEW,
	WF_CTX_EDIT,       /* the editor with Nano / Piero style keys */
	WF_CTX_VIM_NORMAL, /* the editor, Vim style, normal mode */
	WF_CTX_VIM_INSERT, /* the editor, Vim style, insert mode */
	/* fixed: the keys are what the prompt or screen needs, listed here so
	 * that every place keys mean something has a name */
	WF_CTX_SMART_GO,
	WF_CTX_FILTER,
	WF_CTX_CMDLINE,
	WF_CTX_SETTINGS_NAV,
	WF_CTX_SETTINGS_CAPTURE,
	WF_CTX_SETTINGS_VALUE,
	WF_CTX_CONFIRM,
	WF_CTX_COUNT
} WfKeyContext;

#define WF_CTX_BINDABLE 5u

const char *wf_context_name(WfKeyContext c); /* "fm", "preview", "edit"... */
bool wf_context_from_name(const char *name, WfKeyContext *out);
WfMsg wf_context_label(WfKeyContext c);

enum
{
	WF_FM_NAV_UP = 0x100,
	WF_FM_NAV_DOWN,
	WF_FM_NAV_PARENT,
	WF_FM_NAV_ENTER, /* enter a folder or an archive */
	WF_FM_NAV_RIGHT, /* enter, or look at the file in the preview pane */
	WF_FM_NAV_OPEN,  /* enter, or open the file */
	WF_FM_NAV_FIRST,
	WF_FM_NAV_LAST,
	WF_FM_HISTORY_BACK,
	WF_FM_HISTORY_FORWARD,
	WF_FM_SMART_GO,
	WF_FM_FILTER,
	WF_FM_HIDDEN,
	WF_FM_RELOAD,
	WF_FM_INFO,
	WF_FM_PREVIEW_FULL,
	WF_FM_PREVIEW_LOAD,
	WF_FM_SELECT,
	WF_FM_CANCEL, /* clear the selection, then the copy / move slot */
	WF_FM_CREATE,
	WF_FM_RENAME,
	WF_FM_RECYCLE,
	WF_FM_COPY_STAGE,
	WF_FM_MOVE_STAGE,
	WF_FM_PASTE,
	WF_FM_QUEUE_PAUSE,
	WF_FM_QUEUE_CANCEL,
	WF_FM_UNDO,
	WF_FM_JOBLOG,
	WF_FM_BACKUPS,
	WF_FM_EDIT,
	WF_FM_SORT,
	WF_FM_SETTINGS,
	WF_FM_HELP,
	WF_FM_QUIT,
	WF_FM_CMDLINE,
	WF_FM_SMART_GO_PATH, /* Smart Go with the current path filled in */
	WF_FM_CREATE_FILE,
	WF_FM_CREATE_FOLDER,
	WF_FM_DIRSIZE,    /* count the size of every folder in the list */
	WF_FM_CLIP_PASTE, /* paste the files on the Windows clipboard */
	WF_FM_CLIP_COPY,  /* put the selected files on the Windows clipboard */
	WF_FM_LOCKS, /* which programs have the file open (and close or end them) */
	WF_FM_FUZZY, /* search the names on screen by a few letters */
	WF_FM_BULK_RENAME, /* rename the marked entries by a pattern, after a preview */
	WF_FM_HASH, /* the SHA-256 of the file under the cursor, on request */
	WF_FM_TAB_NEW,   /* a new tab at this folder */
	WF_FM_TAB_CLOSE, /* close this tab */
	WF_FM_TAB_PREV,  /* the previous tab */
	WF_FM_TAB_NEXT,  /* the next tab */
	WF_FM_PLACE_SET, /* keep this folder under a letter (then the letter) */
	WF_FM_PLACE_GO, /* go to the folder kept under a letter (then the letter) */
	WF_FM_DUPES,    /* files below this folder that hold the same bytes */
	WF_FM_TERMINAL, /* a new terminal in the folder on screen */
	WF_FM_LAST
};

enum
{
	WF_PV_UP = 0x200,
	WF_PV_DOWN,
	WF_PV_PAN_LEFT,
	WF_PV_PAN_RIGHT,
	WF_PV_BACK, /* pan back; at the left edge, leave the pane */
	WF_PV_EDGE_LEFT,
	WF_PV_EDGE_RIGHT,
	WF_PV_TOP,
	WF_PV_BOTTOM,
	WF_PV_CLOSE,
	WF_PV_TOGGLE_FULL,
	WF_PV_LOAD,
	WF_PV_EDIT,
	WF_PV_PAGE_NEXT, /* pictures and PDF: next page */
	WF_PV_PAGE_PREV,
	WF_PV_ZOOM_IN,
	WF_PV_ZOOM_OUT,
	WF_PV_FIT, /* fit the page <-> fit the width */
	WF_PV_PASSWORD,
	WF_PV_LAST
};

enum
{
	WF_CF_COUNT = 1u << 0, /* a number before it repeats it (safe moves) */
	WF_CF_REQUIRED = 1u << 1, /* must keep at least one key */
	WF_CF_LINE = 1u << 2      /* with a number: go to that line / row */
};

typedef struct
{
	uint16_t id;
	const char *name; /* stable: "fm.nav.up" */
	WfMsg desc;
	uint8_t ctx_mask; /* bit per bindable context the command exists in */
	uint8_t flags;
} WfCommandInfo;

#define WF_CTX_BIT(c) (1u << (c))

const WfCommandInfo *wf_cmd_info(uint16_t id);
const WfCommandInfo *wf_cmd_by_name(const char *name);
/* Commands that exist in `ctx`, in the order they are listed; fills at most
 * `cap` pointers and returns how many exist. */
size_t wf_cmd_list(WfKeyContext ctx, const WfCommandInfo **out, size_t cap);
size_t wf_cmd_total(void);
const WfCommandInfo *wf_cmd_at(size_t index);

#endif

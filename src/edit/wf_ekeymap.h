#ifndef WF_EKEYMAP_H
#define WF_EKEYMAP_H

#include <stdbool.h>

#include "../input/wf_stroke.h"
#include "../ui/wf_event.h"

/*
 * Key bindings are data. A preset maps an input event to a command; the
 * command is what the editor does. Both presets bind Ctrl+S to save.
 *
 * Only keys the terminal can really tell apart are bound: Ctrl and Alt with a
 * letter, the cursor keys with Shift/Ctrl, and a few function keys.
 * Shift+Ctrl+S exists where the console reports it, and has an Alt
 * alternative where it does not.
 */
typedef enum
{
	WF_KM_NONE = 0,
	WF_KM_PIERO,
	WF_KM_NANO
	/* WF_KM_VIM is reserved: the input modes and the command line it needs
	 * exist, the preset itself is not offered. */
} WfKeymapId;

typedef enum
{
	WF_CMD_NONE = 0,
	/* motion; `extend` selects */
	WF_CMD_LEFT,
	WF_CMD_RIGHT,
	WF_CMD_UP,
	WF_CMD_DOWN,
	WF_CMD_HOME,
	WF_CMD_END,
	WF_CMD_PGUP,
	WF_CMD_PGDN,
	WF_CMD_DOC_START,
	WF_CMD_DOC_END,
	WF_CMD_WORD_LEFT,
	WF_CMD_WORD_RIGHT,
	/* editing */
	WF_CMD_NEWLINE,
	WF_CMD_BACKSPACE,
	WF_CMD_DELETE,
	WF_CMD_TAB,
	WF_CMD_BACKTAB,
	WF_CMD_DELETE_LINE,
	WF_CMD_CUT,
	WF_CMD_COPY,
	WF_CMD_PASTE,
	WF_CMD_SELECT_ALL,
	WF_CMD_MARK,
	WF_CMD_UNDO,
	WF_CMD_REDO,
	WF_CMD_COMMENT,
	WF_CMD_COMPLETE,
	/* things that ask for a prompt or touch files */
	WF_CMD_SAVE,
	WF_CMD_SAVE_AS,
	WF_CMD_EXIT,
	WF_CMD_FIND,
	WF_CMD_FIND_BACK,
	WF_CMD_FIND_NEXT,
	WF_CMD_FIND_PREV,
	WF_CMD_REPLACE,
	WF_CMD_GOTO,
	WF_CMD_INSERT_FILE,
	WF_CMD_OPEN_FILE,
	WF_CMD_NEXT_BUFFER,
	WF_CMD_PREV_BUFFER,
	WF_CMD_HELP,
	WF_CMD_CANCEL, /* Esc: drops a selection */
	/* the modal (Vim-style) profile */
	WF_CMD_VIM_INSERT,       /* i: insert before the cursor */
	WF_CMD_VIM_APPEND,       /* a: insert after the cursor */
	WF_CMD_VIM_OPEN_BELOW,   /* o: a new line below, insert there */
	WF_CMD_VIM_OPEN_ABOVE,   /* O: a new line above, insert there */
	WF_CMD_VIM_NORMAL,       /* Esc in insert: back to normal */
	WF_CMD_VIM_YANK_LINE,    /* yy */
	WF_CMD_VIM_CUT_LINE,     /* dd */
	WF_CMD_VIM_PASTE_AFTER,  /* p */
	WF_CMD_VIM_PASTE_BEFORE, /* P */
	WF_CMD_VIM_CMDLINE,      /* : */
	WF_CMD_VIM_DELETE_CHAR,  /* x */
	/* operators, Visual mode, registers and macros (wf_evim.c); keep in order */
	WF_CMD_VIM_OP_DELETE,   /* d, then a motion */
	WF_CMD_VIM_OP_YANK,     /* y, then a motion */
	WF_CMD_VIM_OP_CHANGE,   /* c, then a motion */
	WF_CMD_VIM_DELETE_EOL,  /* D */
	WF_CMD_VIM_CHANGE_EOL,  /* C */
	WF_CMD_VIM_VISUAL,      /* v */
	WF_CMD_VIM_VISUAL_LINE, /* V */
	WF_CMD_VIM_REGISTER,    /* " */
	WF_CMD_VIM_RECORD,      /* q */
	WF_CMD_VIM_PLAY,        /* @ */
	WF_CMD_VIM_DOT,         /* . */
	WF_CMD_MD_PREVIEW,      /* the Markdown preview pane (DR-DRAFT-025) */
	WF_CMD_LAST
} WfEdCmd;

typedef struct
{
	WfEdCmd cmd;
	bool extend; /* motion with the selection growing */
} WfEdAction;

WfEdAction wf_ekm_lookup(WfKeymapId id, const WfEvent *ev);

/* A preset as strokes, for the keymap that serves every profile. The
 * preset's own bindings come first, then the shared ones it does not
 * shadow. Returns how many were written (at most `cap`). */
typedef struct
{
	WfStroke stroke;
	WfEdCmd cmd;
	bool extend;
} WfEkBinding;

size_t wf_ekm_export(WfKeymapId id, WfEkBinding *out, size_t cap);

/* How the preset names the key for `cmd` ("^S", "M-U", "F3"...), or NULL. */
const char *wf_ekm_key_name(WfKeymapId id, WfEdCmd cmd);
const char *wf_ekm_preset_name(WfKeymapId id);

/* A name from the configuration: "piero" or "nano". "vim" is recognized but
 * not available. Returns false for anything else. */
bool wf_ekm_from_name(const char *name, WfKeymapId *id, bool *reserved);

#endif

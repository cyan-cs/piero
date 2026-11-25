#include "wf_keymap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../edit/wf_ekeymap.h"
#include "../base/wf_macro.h"
#include "../base/wf_mem.h"

typedef struct
{
	uint8_t ctx;
	uint16_t cmd;
	const char *keys; /* "k, Up": sequences separated by commas */
} Def;

/* Standard is a compatibility contract, independent of Nano. */
static const Def FM_STANDARD[] = {{WF_CTX_FM, WF_FM_NAV_UP, "k, Up"},
        {WF_CTX_FM, WF_FM_NAV_DOWN, "j, Down"},
        {WF_CTX_FM, WF_FM_NAV_PARENT, "h, Left"},
        {WF_CTX_FM, WF_FM_NAV_ENTER, "l"},
        {WF_CTX_FM, WF_FM_NAV_RIGHT, "Right"},
        {WF_CTX_FM, WF_FM_NAV_OPEN, "Enter"},
        {WF_CTX_FM, WF_FM_NAV_FIRST, "Ctrl+Up"},
        {WF_CTX_FM, WF_FM_NAV_LAST, "Ctrl+Down"},
        {WF_CTX_FM, WF_FM_HISTORY_BACK, "b"},
        {WF_CTX_FM, WF_FM_HISTORY_FORWARD, "f"},
        {WF_CTX_FM, WF_FM_SMART_GO, "g"},
        {WF_CTX_FM, WF_FM_SMART_GO_PATH, "Ctrl+L"},
        {WF_CTX_FM, WF_FM_CREATE_FILE, "%"},
        {WF_CTX_FM, WF_FM_CREATE_FOLDER, "d"}, {WF_CTX_FM, WF_FM_FILTER, "/"},
        {WF_CTX_FM, WF_FM_HIDDEN, "."}, {WF_CTX_FM, WF_FM_RELOAD, "r"},
        {WF_CTX_FM, WF_FM_INFO, "i"}, {WF_CTX_FM, WF_FM_PREVIEW_FULL, "v"},
        {WF_CTX_FM, WF_FM_PREVIEW_LOAD, "P"},
        {WF_CTX_FM, WF_FM_SELECT, "Space"}, {WF_CTX_FM, WF_FM_CANCEL, "Esc"},
        {WF_CTX_FM, WF_FM_CREATE, "a"}, {WF_CTX_FM, WF_FM_RENAME, "F2, R"},
        {WF_CTX_FM, WF_FM_RECYCLE, "Delete"},
        {WF_CTX_FM, WF_FM_COPY_STAGE, "y"}, {WF_CTX_FM, WF_FM_MOVE_STAGE, "x"},
        {WF_CTX_FM, WF_FM_PASTE, "p"}, {WF_CTX_FM, WF_FM_QUEUE_PAUSE, "z"},
        {WF_CTX_FM, WF_FM_QUEUE_CANCEL, "C"}, {WF_CTX_FM, WF_FM_UNDO, "u"},
        {WF_CTX_FM, WF_FM_JOBLOG, "J"}, {WF_CTX_FM, WF_FM_BACKUPS, "B"},
        {WF_CTX_FM, WF_FM_DIRSIZE, "D"}, {WF_CTX_FM, WF_FM_CLIP_PASTE, "V"},
        {WF_CTX_FM, WF_FM_CLIP_COPY, "Y"}, {WF_CTX_FM, WF_FM_LOCKS, "L"},
        {WF_CTX_FM, WF_FM_EDIT, "e"}, {WF_CTX_FM, WF_FM_FUZZY, "s"},
        {WF_CTX_FM, WF_FM_BULK_RENAME, "M"}, {WF_CTX_FM, WF_FM_HASH, "#"},
        {WF_CTX_FM, WF_FM_TAB_NEW, "t"}, {WF_CTX_FM, WF_FM_TAB_CLOSE, "w"},
        {WF_CTX_FM, WF_FM_TAB_PREV, "[, Ctrl+Left"},
        {WF_CTX_FM, WF_FM_TAB_NEXT, "], Ctrl+Right"},
        {WF_CTX_FM, WF_FM_PLACE_SET, "m"}, {WF_CTX_FM, WF_FM_PLACE_GO, "'"},
        {WF_CTX_FM, WF_FM_DUPES, "F"}, {WF_CTX_FM, WF_FM_TERMINAL, "!"},
        {WF_CTX_FM, WF_FM_SORT, "S"}, {WF_CTX_FM, WF_FM_SETTINGS, "Comma"},
        {WF_CTX_FM, WF_FM_HELP, "?"}, {WF_CTX_FM, WF_FM_QUIT, "q"},
        {0, 0, NULL}};

/* Nano navigation uses arrows; transfer chords stage files, never edit them. */
static const Def FM_NANO[] = {{WF_CTX_FM, WF_FM_NAV_UP, "Up, Ctrl+P"},
        {WF_CTX_FM, WF_FM_NAV_DOWN, "Down, Ctrl+N"},
        {WF_CTX_FM, WF_FM_NAV_PARENT, "Left"},
        {WF_CTX_FM, WF_FM_NAV_ENTER, "Ctrl+Right"},
        {WF_CTX_FM, WF_FM_NAV_RIGHT, "Right"},
        {WF_CTX_FM, WF_FM_NAV_OPEN, "Enter"},
        {WF_CTX_FM, WF_FM_NAV_FIRST, "Ctrl+Up"},
        {WF_CTX_FM, WF_FM_NAV_LAST, "Ctrl+Down"},
        {WF_CTX_FM, WF_FM_HISTORY_BACK, "b"},
        {WF_CTX_FM, WF_FM_HISTORY_FORWARD, "f"},
        {WF_CTX_FM, WF_FM_SMART_GO, "g"},
        {WF_CTX_FM, WF_FM_SMART_GO_PATH, "Ctrl+L"},
        {WF_CTX_FM, WF_FM_CREATE_FILE, "%"},
        {WF_CTX_FM, WF_FM_CREATE_FOLDER, "d"},
        {WF_CTX_FM, WF_FM_FILTER, "Ctrl+F, /"}, {WF_CTX_FM, WF_FM_HIDDEN, "."},
        {WF_CTX_FM, WF_FM_RELOAD, "r"}, {WF_CTX_FM, WF_FM_INFO, "i"},
        {WF_CTX_FM, WF_FM_PREVIEW_FULL, "v"},
        {WF_CTX_FM, WF_FM_PREVIEW_LOAD, "P"},
        {WF_CTX_FM, WF_FM_SELECT, "Space"}, {WF_CTX_FM, WF_FM_CANCEL, "Esc"},
        {WF_CTX_FM, WF_FM_CREATE, "a"}, {WF_CTX_FM, WF_FM_RENAME, "F2, R"},
        {WF_CTX_FM, WF_FM_RECYCLE, "Delete"},
        {WF_CTX_FM, WF_FM_COPY_STAGE, "Alt+6"},
        {WF_CTX_FM, WF_FM_MOVE_STAGE, "Ctrl+K"},
        {WF_CTX_FM, WF_FM_PASTE, "Ctrl+U"}, {WF_CTX_FM, WF_FM_QUEUE_PAUSE, "z"},
        {WF_CTX_FM, WF_FM_QUEUE_CANCEL, "C"}, {WF_CTX_FM, WF_FM_UNDO, "Alt+U"},
        {WF_CTX_FM, WF_FM_JOBLOG, "J"}, {WF_CTX_FM, WF_FM_BACKUPS, "B"},
        {WF_CTX_FM, WF_FM_DIRSIZE, "D"}, {WF_CTX_FM, WF_FM_CLIP_PASTE, "V"},
        {WF_CTX_FM, WF_FM_CLIP_COPY, "Y"}, {WF_CTX_FM, WF_FM_LOCKS, "L"},
        {WF_CTX_FM, WF_FM_EDIT, "e"}, {WF_CTX_FM, WF_FM_FUZZY, "Ctrl+W, s"},
        {WF_CTX_FM, WF_FM_BULK_RENAME, "M"}, {WF_CTX_FM, WF_FM_HASH, "#"},
        {WF_CTX_FM, WF_FM_TAB_NEW, "t"}, {WF_CTX_FM, WF_FM_TAB_CLOSE, "w"},
        {WF_CTX_FM, WF_FM_TAB_PREV, "["}, {WF_CTX_FM, WF_FM_TAB_NEXT, "]"},
        {WF_CTX_FM, WF_FM_PLACE_SET, "m"}, {WF_CTX_FM, WF_FM_PLACE_GO, "'"},
        {WF_CTX_FM, WF_FM_DUPES, "F"}, {WF_CTX_FM, WF_FM_TERMINAL, "!"},
        {WF_CTX_FM, WF_FM_SORT, "S"}, {WF_CTX_FM, WF_FM_SETTINGS, "Comma"},
        {WF_CTX_FM, WF_FM_HELP, "Ctrl+G, ?"},
        {WF_CTX_FM, WF_FM_QUIT, "Ctrl+X, q"}, {0, 0, NULL}};

/* Vim profile: the same commands where the meaning is the same, Vim's way to
 * ask where it differs. dd means "move", not "delete": nothing is deleted
 * by a key sequence; the Recycle Bin has its own key and its own question. */
static const Def FM_VIM[] = {{WF_CTX_FM, WF_FM_NAV_UP, "k, Up"},
        {WF_CTX_FM, WF_FM_NAV_DOWN, "j, Down"},
        {WF_CTX_FM, WF_FM_NAV_PARENT, "h, Left"},
        {WF_CTX_FM, WF_FM_NAV_ENTER, "l"},
        {WF_CTX_FM, WF_FM_NAV_RIGHT, "Right"},
        {WF_CTX_FM, WF_FM_NAV_OPEN, "Enter"},
        {WF_CTX_FM, WF_FM_NAV_FIRST, "g g, Ctrl+Up"},
        {WF_CTX_FM, WF_FM_NAV_LAST, "G, Ctrl+Down"},
        {WF_CTX_FM, WF_FM_HISTORY_BACK, "b"},
        {WF_CTX_FM, WF_FM_HISTORY_FORWARD, "f"},
        {WF_CTX_FM, WF_FM_SMART_GO_PATH, "Ctrl+L"},
        {WF_CTX_FM, WF_FM_CREATE_FILE, "%"}, {WF_CTX_FM, WF_FM_FILTER, "/"},
        {WF_CTX_FM, WF_FM_HIDDEN, "."}, {WF_CTX_FM, WF_FM_RELOAD, "r"},
        {WF_CTX_FM, WF_FM_INFO, "i"}, {WF_CTX_FM, WF_FM_PREVIEW_FULL, "v"},
        {WF_CTX_FM, WF_FM_PREVIEW_LOAD, "P"},
        {WF_CTX_FM, WF_FM_SELECT, "Space"}, {WF_CTX_FM, WF_FM_CANCEL, "Esc"},
        {WF_CTX_FM, WF_FM_CREATE, "a"}, {WF_CTX_FM, WF_FM_RENAME, "F2, R"},
        {WF_CTX_FM, WF_FM_RECYCLE, "Delete"},
        {WF_CTX_FM, WF_FM_COPY_STAGE, "y y"},
        {WF_CTX_FM, WF_FM_MOVE_STAGE, "d d"}, {WF_CTX_FM, WF_FM_PASTE, "p"},
        {WF_CTX_FM, WF_FM_QUEUE_PAUSE, "z"},
        {WF_CTX_FM, WF_FM_QUEUE_CANCEL, "C"}, {WF_CTX_FM, WF_FM_UNDO, "u"},
        {WF_CTX_FM, WF_FM_JOBLOG, "J"}, {WF_CTX_FM, WF_FM_BACKUPS, "B"},
        {WF_CTX_FM, WF_FM_DIRSIZE, "D"}, {WF_CTX_FM, WF_FM_CLIP_PASTE, "V"},
        {WF_CTX_FM, WF_FM_CLIP_COPY, "Y"}, {WF_CTX_FM, WF_FM_LOCKS, "L"},
        {WF_CTX_FM, WF_FM_EDIT, "e"}, {WF_CTX_FM, WF_FM_FUZZY, "s"},
        {WF_CTX_FM, WF_FM_BULK_RENAME, "M"}, {WF_CTX_FM, WF_FM_HASH, "#"},
        {WF_CTX_FM, WF_FM_TAB_NEW, "t"}, {WF_CTX_FM, WF_FM_TAB_CLOSE, "w"},
        {WF_CTX_FM, WF_FM_TAB_PREV, "[, Ctrl+Left"},
        {WF_CTX_FM, WF_FM_TAB_NEXT, "], Ctrl+Right"},
        {WF_CTX_FM, WF_FM_PLACE_SET, "m"}, {WF_CTX_FM, WF_FM_PLACE_GO, "'"},
        {WF_CTX_FM, WF_FM_DUPES, "F"}, {WF_CTX_FM, WF_FM_TERMINAL, "!"},
        {WF_CTX_FM, WF_FM_SORT, "S"}, {WF_CTX_FM, WF_FM_SETTINGS, "Comma"},
        {WF_CTX_FM, WF_FM_HELP, "?"}, {WF_CTX_FM, WF_FM_QUIT, "q"},
        {WF_CTX_FM, WF_FM_CMDLINE, ":"}, {0, 0, NULL}};

static const Def PV_STANDARD[] = {{WF_CTX_PREVIEW, WF_PV_UP, "k, Up"},
        {WF_CTX_PREVIEW, WF_PV_DOWN, "j, Down"},
        {WF_CTX_PREVIEW, WF_PV_PAN_LEFT, "h"},
        {WF_CTX_PREVIEW, WF_PV_PAN_RIGHT, "l, Right"},
        {WF_CTX_PREVIEW, WF_PV_BACK, "Left"},
        {WF_CTX_PREVIEW, WF_PV_EDGE_LEFT, "Home"},
        {WF_CTX_PREVIEW, WF_PV_EDGE_RIGHT, "End"},
        {WF_CTX_PREVIEW, WF_PV_TOP, "Ctrl+Up"},
        {WF_CTX_PREVIEW, WF_PV_BOTTOM, "Ctrl+Down"},
        {WF_CTX_PREVIEW, WF_PV_CLOSE, "Esc, q"},
        {WF_CTX_PREVIEW, WF_PV_TOGGLE_FULL, "v"},
        {WF_CTX_PREVIEW, WF_PV_LOAD, "P"}, {WF_CTX_PREVIEW, WF_PV_EDIT, "e"},
        {WF_CTX_PREVIEW, WF_PV_PAGE_NEXT, "PageDown, n"},
        {WF_CTX_PREVIEW, WF_PV_PAGE_PREV, "PageUp, p"},
        {WF_CTX_PREVIEW, WF_PV_ZOOM_IN, "+, ="},
        {WF_CTX_PREVIEW, WF_PV_ZOOM_OUT, "-"}, {WF_CTX_PREVIEW, WF_PV_FIT, "f"},
        {WF_CTX_PREVIEW, WF_PV_PASSWORD, "Enter"}, {0, 0, NULL}};

static const Def PV_NANO[] = {{WF_CTX_PREVIEW, WF_PV_UP, "Up, Ctrl+P"},
        {WF_CTX_PREVIEW, WF_PV_DOWN, "Down, Ctrl+N"},
        {WF_CTX_PREVIEW, WF_PV_PAN_LEFT, "Shift+Left"},
        {WF_CTX_PREVIEW, WF_PV_PAN_RIGHT, "Right"},
        {WF_CTX_PREVIEW, WF_PV_BACK, "Left"},
        {WF_CTX_PREVIEW, WF_PV_EDGE_LEFT, "Home"},
        {WF_CTX_PREVIEW, WF_PV_EDGE_RIGHT, "End"},
        {WF_CTX_PREVIEW, WF_PV_TOP, "Ctrl+Up"},
        {WF_CTX_PREVIEW, WF_PV_BOTTOM, "Ctrl+Down"},
        {WF_CTX_PREVIEW, WF_PV_CLOSE, "Esc, Ctrl+X"},
        {WF_CTX_PREVIEW, WF_PV_TOGGLE_FULL, "v"},
        {WF_CTX_PREVIEW, WF_PV_LOAD, "P"}, {WF_CTX_PREVIEW, WF_PV_EDIT, "e"},
        {WF_CTX_PREVIEW, WF_PV_PAGE_NEXT, "PageDown, Ctrl+V"},
        {WF_CTX_PREVIEW, WF_PV_PAGE_PREV, "PageUp, Ctrl+Y"},
        {WF_CTX_PREVIEW, WF_PV_ZOOM_IN, "+, ="},
        {WF_CTX_PREVIEW, WF_PV_ZOOM_OUT, "-"}, {WF_CTX_PREVIEW, WF_PV_FIT, "f"},
        {WF_CTX_PREVIEW, WF_PV_PASSWORD, "Enter"}, {0, 0, NULL}};

static const Def PV_VIM[] = {{WF_CTX_PREVIEW, WF_PV_UP, "k, Up"},
        {WF_CTX_PREVIEW, WF_PV_DOWN, "j, Down"},
        {WF_CTX_PREVIEW, WF_PV_PAN_LEFT, "h"},
        {WF_CTX_PREVIEW, WF_PV_PAN_RIGHT, "l, Right"},
        {WF_CTX_PREVIEW, WF_PV_BACK, "Left"},
        {WF_CTX_PREVIEW, WF_PV_EDGE_LEFT, "0, Home"},
        {WF_CTX_PREVIEW, WF_PV_EDGE_RIGHT, "$, End"},
        {WF_CTX_PREVIEW, WF_PV_TOP, "g g, Ctrl+Up"},
        {WF_CTX_PREVIEW, WF_PV_BOTTOM, "G, Ctrl+Down"},
        {WF_CTX_PREVIEW, WF_PV_CLOSE, "Esc, q"},
        {WF_CTX_PREVIEW, WF_PV_TOGGLE_FULL, "v"},
        {WF_CTX_PREVIEW, WF_PV_LOAD, "P"}, {WF_CTX_PREVIEW, WF_PV_EDIT, "e"},
        {WF_CTX_PREVIEW, WF_PV_PAGE_NEXT, "PageDown, Ctrl+F"},
        {WF_CTX_PREVIEW, WF_PV_PAGE_PREV, "PageUp, Ctrl+B"},
        {WF_CTX_PREVIEW, WF_PV_ZOOM_IN, "+, ="},
        {WF_CTX_PREVIEW, WF_PV_ZOOM_OUT, "-"}, {WF_CTX_PREVIEW, WF_PV_FIT, "f"},
        {WF_CTX_PREVIEW, WF_PV_PASSWORD, "Enter"}, {0, 0, NULL}};

/* Nano profile, on top of the editor's Nano preset. */
static const Def ED_NANO_EXTRA[] = {{WF_CTX_EDIT, WF_CMD_FIND, "Ctrl+W"},
        {WF_CTX_EDIT, WF_CMD_REPLACE, "Ctrl+\\"}, {0, 0, NULL}};

static const Def ED_VIM_NORMAL[] = {{WF_CTX_VIM_NORMAL, WF_CMD_LEFT, "h, Left"},
        {WF_CTX_VIM_NORMAL, WF_CMD_RIGHT, "l, Right"},
        {WF_CTX_VIM_NORMAL, WF_CMD_UP, "k, Up"},
        {WF_CTX_VIM_NORMAL, WF_CMD_DOWN, "j, Down"},
        {WF_CTX_VIM_NORMAL, WF_CMD_HOME, "0, Home"},
        {WF_CTX_VIM_NORMAL, WF_CMD_END, "$, End"},
        {WF_CTX_VIM_NORMAL, WF_CMD_PGUP, "PageUp, Ctrl+B"},
        {WF_CTX_VIM_NORMAL, WF_CMD_PGDN, "PageDown, Ctrl+F"},
        {WF_CTX_VIM_NORMAL, WF_CMD_DOC_START, "g g, Ctrl+Home"},
        {WF_CTX_VIM_NORMAL, WF_CMD_DOC_END, "G, Ctrl+End"},
        {WF_CTX_VIM_NORMAL, WF_CMD_WORD_LEFT, "b, Ctrl+Left"},
        {WF_CTX_VIM_NORMAL, WF_CMD_WORD_RIGHT, "w, Ctrl+Right"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_INSERT, "i"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_APPEND, "a"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_OPEN_BELOW, "o"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_OPEN_ABOVE, "O"},
        {WF_CTX_VIM_NORMAL, WF_CMD_UNDO, "u"},
        {WF_CTX_VIM_NORMAL, WF_CMD_REDO, "Ctrl+R"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_OP_DELETE, "d"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_OP_YANK, "y"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_OP_CHANGE, "c"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_DELETE_EOL, "D"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_CHANGE_EOL, "C"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_VISUAL, "v"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_VISUAL_LINE, "V"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_REGISTER, "\""},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_RECORD, "q"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_PLAY, "@"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_DOT, "."},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_PASTE_AFTER, "p"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_PASTE_BEFORE, "P"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_DELETE_CHAR, "x"},
        {WF_CTX_VIM_NORMAL, WF_CMD_FIND, "/"},
        {WF_CTX_VIM_NORMAL, WF_CMD_FIND_NEXT, "n"},
        {WF_CTX_VIM_NORMAL, WF_CMD_FIND_PREV, "N"},
        {WF_CTX_VIM_NORMAL, WF_CMD_VIM_CMDLINE, ":"},
        {WF_CTX_VIM_NORMAL, WF_CMD_NEXT_BUFFER, "g t"},
        {WF_CTX_VIM_NORMAL, WF_CMD_PREV_BUFFER, "g T"},
        {WF_CTX_VIM_NORMAL, WF_CMD_SAVE, "Ctrl+S"},
        {WF_CTX_VIM_NORMAL, WF_CMD_HELP, "F2"},
        {WF_CTX_VIM_NORMAL, WF_CMD_MD_PREVIEW, "Alt+M"},
        {WF_CTX_VIM_NORMAL, WF_CMD_CANCEL, "Esc"}, {0, 0, NULL}};

static const Def ED_VIM_INSERT[] = {
        {WF_CTX_VIM_INSERT, WF_CMD_VIM_NORMAL, "Esc"},
        {WF_CTX_VIM_INSERT, WF_CMD_LEFT, "Left"},
        {WF_CTX_VIM_INSERT, WF_CMD_RIGHT, "Right"},
        {WF_CTX_VIM_INSERT, WF_CMD_UP, "Up"},
        {WF_CTX_VIM_INSERT, WF_CMD_DOWN, "Down"},
        {WF_CTX_VIM_INSERT, WF_CMD_HOME, "Home"},
        {WF_CTX_VIM_INSERT, WF_CMD_END, "End"},
        {WF_CTX_VIM_INSERT, WF_CMD_PGUP, "PageUp"},
        {WF_CTX_VIM_INSERT, WF_CMD_PGDN, "PageDown"},
        {WF_CTX_VIM_INSERT, WF_CMD_DOC_START, "Ctrl+Home"},
        {WF_CTX_VIM_INSERT, WF_CMD_DOC_END, "Ctrl+End"},
        {WF_CTX_VIM_INSERT, WF_CMD_WORD_LEFT, "Ctrl+Left"},
        {WF_CTX_VIM_INSERT, WF_CMD_WORD_RIGHT, "Ctrl+Right"},
        {WF_CTX_VIM_INSERT, WF_CMD_NEWLINE, "Enter"},
        {WF_CTX_VIM_INSERT, WF_CMD_BACKSPACE, "Backspace, Ctrl+Backspace"},
        {WF_CTX_VIM_INSERT, WF_CMD_DELETE, "Delete, Ctrl+Delete"},
        {WF_CTX_VIM_INSERT, WF_CMD_TAB, "Tab"},
        {WF_CTX_VIM_INSERT, WF_CMD_BACKTAB, "Shift+Tab"},
        {WF_CTX_VIM_INSERT, WF_CMD_SAVE, "Ctrl+S"},
        {WF_CTX_VIM_INSERT, WF_CMD_COMPLETE, "Ctrl+N"},
        {WF_CTX_VIM_INSERT, WF_CMD_MD_PREVIEW, "Alt+M"}, {0, 0, NULL}};

typedef struct
{
	WfBinding *b;
	size_t n, cap;
} List;

static bool list_add(List *l, const WfSeq *seq, uint16_t cmd, uint8_t extend)
{
	if (l->n == l->cap) {
		size_t cap = l->cap ? l->cap * 2 : 32;
		WfBinding *p = wf_realloc_array(l->b, cap, sizeof *p);

		if (!p) {
			return false;
		}
		l->b = p;
		l->cap = cap;
	}
	l->b[l->n].seq = *seq;
	l->b[l->n].cmd = cmd;
	l->b[l->n].extend = extend;
	l->n++;
	return true;
}

static bool list_has(const List *l, const WfSeq *seq)
{
	size_t i;

	for (i = 0; i < l->n; i++) {
		if (wf_seq_equal(&l->b[i].seq, seq)) {
			return true;
		}
	}
	return false;
}

/* "k, Up" -> up to two sequences. */
bool wf_keys_parse_list(const char *text, WfKeyOverride *out)
{
	const char *p = text;

	out->n = 0;
	while (' ' == *p) {
		p++;
	}
	if (!strncmp(p, "none", 4) && ('\0' == p[4] || ' ' == p[4])) {
		p += 4;
		while (' ' == *p) {
			p++;
		}
		return '\0' == *p;
	}
	for (;;) {
		const char *end = strchr(p, ',');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		WfSeq q;

		if (2 == out->n || !wf_seq_parse(p, len, &q)) {
			return false;
		}
		out->seq[out->n++] = q;
		if (!end) {
			break;
		}
		p = end + 1;
	}
	return true;
}

size_t wf_keys_format_list(const WfKeyOverride *o, char *out, size_t cap)
{
	size_t at = 0;
	uint8_t i;

	if (!cap) {
		return 0;
	}
	if (!o->n) {
		(void)snprintf(out, cap, "none");
		return strlen(out);
	}
	for (i = 0; i < o->n; i++) {
		char one[64];
		int n;

		wf_seq_format(&o->seq[i], one, sizeof one);
		n = snprintf(out + at, cap - at, "%s%s", i ? ", " : "", one);
		if (n < 0 || (size_t)n >= cap - at) {
			return strlen(out);
		}
		at += (size_t)n;
	}
	return at < cap ? at : cap - 1;
}

static bool add_defs(List *l, const Def *defs, WfKeyContext ctx)
{
	const Def *d;

	for (d = defs; d->keys; d++) {
		WfKeyOverride o;
		uint8_t i;

		if (d->ctx != ctx) {
			continue;
		}
		if (!wf_keys_parse_list(d->keys, &o)) {
			return false; /* a table error: caught by the tests */
		}
		for (i = 0; i < o.n; i++) {
			if (!list_add(l, &o.seq[i], d->cmd, 0)) {
				return false;
			}
		}
	}
	return true;
}

/* The editor's own preset, as bindings. Shared bindings that a preset key
 * shadows are left out, as they are when the preset is looked up. */
static bool add_preset(List *l, WfKeymapId id)
{
	WfEkBinding eb[160];
	size_t n = wf_ekm_export(id, eb, WF_ARRAY_LEN(eb)), i;

	for (i = 0; i < n; i++) {
		WfSeq q;

		q.n = 1;
		q.s[0] = eb[i].stroke;
		if (list_has(l, &q)) {
			continue;
		}
		if (!list_add(l, &q, (uint16_t)eb[i].cmd, eb[i].extend)) {
			return false;
		}
	}
	return true;
}

struct WfKeymap
{
	WfProfile profile;
	WfBase base;
	bool modal;
	bool counts[WF_CTX_BINDABLE];
	List ctx[WF_CTX_BINDABLE];
};

void wf_keymap_free(WfKeymap *m)
{
	unsigned i;

	if (!m) {
		return;
	}
	for (i = 0; i < WF_CTX_BINDABLE; i++) {
		free(m->ctx[i].b);
	}
	free(m);
}

WfProfile wf_keymap_profile(const WfKeymap *m)
{
	return m->profile;
}

WfBase wf_keymap_base(const WfKeymap *m)
{
	return m->base;
}

bool wf_keymap_modal(const WfKeymap *m)
{
	return m->modal;
}

bool wf_keymap_counts(const WfKeymap *m, WfKeyContext ctx)
{
	return (unsigned)ctx < WF_CTX_BINDABLE && m->counts[ctx];
}

const WfBinding *wf_keymap_bindings(
        const WfKeymap *m, WfKeyContext ctx, size_t *n)
{
	if ((unsigned)ctx >= WF_CTX_BINDABLE) {
		*n = 0;
		return NULL;
	}
	*n = m->ctx[ctx].n;
	return m->ctx[ctx].b;
}

static bool fill_defaults(WfKeymap *m, int editor_legacy, int version)
{
	bool vim = WF_BASE_VIM == m->base;
	bool standard = WF_PROFILE_LEGACY == m->profile ||
	        (WF_PROFILE_CUSTOM == m->profile &&
	                WF_BINDINGS_COMPAT == version);
	const Def *fm = FM_NANO, *pv = PV_NANO;

	if (vim) {
		fm = FM_VIM;
		pv = PV_VIM;
	} else if (standard) {
		fm = FM_STANDARD;
		pv = PV_STANDARD;
	}
	if (!add_defs(&m->ctx[WF_CTX_FM], fm, WF_CTX_FM) ||
	        !add_defs(&m->ctx[WF_CTX_PREVIEW], pv, WF_CTX_PREVIEW)) {
		return false;
	}
	m->counts[WF_CTX_FM] = m->counts[WF_CTX_PREVIEW] = vim;
	if (vim) {
		m->modal = true;
		m->counts[WF_CTX_VIM_NORMAL] = true;
		return add_defs(&m->ctx[WF_CTX_VIM_NORMAL], ED_VIM_NORMAL,
		               WF_CTX_VIM_NORMAL) &&
		        add_defs(&m->ctx[WF_CTX_VIM_INSERT], ED_VIM_INSERT,
		                WF_CTX_VIM_INSERT);
	}
	if (WF_PROFILE_LEGACY == m->profile) {
		return add_preset(&m->ctx[WF_CTX_EDIT],
		        WF_KM_NANO == editor_legacy ? WF_KM_NANO : WF_KM_PIERO);
	}
	return add_preset(&m->ctx[WF_CTX_EDIT], WF_KM_NANO) &&
	        add_defs(&m->ctx[WF_CTX_EDIT], ED_NANO_EXTRA, WF_CTX_EDIT);
}

/* Where plain characters are text (the editor with Nano keys, insert mode),
 * a key that is just a character is not available as a command. */
static bool types_text(WfKeyContext c)
{
	return WF_CTX_EDIT == c || WF_CTX_VIM_INSERT == c;
}

static bool plain_char(WfStroke s)
{
	return !s.named && !s.mods;
}

static bool is_digit_start(const WfSeq *q)
{
	return q->n && !q->s[0].named && !q->s[0].mods && q->s[0].code >= '1' &&
	        q->s[0].code <= '9';
}

/* Same keys, or one beginning the other. */
static bool clash(const WfSeq *a, const WfSeq *b)
{
	return wf_seq_equal(a, b) || wf_seq_is_prefix(a, b) ||
	        wf_seq_is_prefix(b, a);
}

static WfKeyIssue make_issue(WfKeyIssueKind k,
        WfKeyContext ctx,
        uint16_t cmd,
        uint16_t other,
        const char *why)
{
	WfKeyIssue i;

	i.kind = k;
	i.ctx = ctx;
	i.cmd = cmd;
	i.other = other;
	i.why = why;
	return i;
}

static WfKeyIssue validate(const WfKeymap *m)
{
	unsigned c;
	size_t i, j, k;
	const WfCommandInfo *ci[160];

	for (c = 0; c < WF_CTX_BINDABLE; c++) {
		const List *l = &m->ctx[c];
		size_t total;

		for (i = 0; i < l->n; i++) {
			const char *why = NULL;

			for (j = 0; j < l->b[i].seq.n; j++) {
				if (!wf_stroke_bindable(l->b[i].seq.s[j],
				            c <= WF_CTX_PREVIEW, &why)) {
					return make_issue(WF_KI_BAD_KEY,
					        (WfKeyContext)c, l->b[i].cmd, 0,
					        why);
				}
			}
			if (types_text((WfKeyContext)c) &&
			        plain_char(l->b[i].seq.s[0])) {
				return make_issue(WF_KI_BAD_KEY,
				        (WfKeyContext)c, l->b[i].cmd, 0,
				        "this mode types the character");
			}
			if (m->counts[c] && is_digit_start(&l->b[i].seq)) {
				return make_issue(WF_KI_COUNT_DIGIT,
				        (WfKeyContext)c, l->b[i].cmd, 0, NULL);
			}
			for (j = i + 1; j < l->n; j++) {
				if (clash(&l->b[i].seq, &l->b[j].seq)) {
					return make_issue(
					        wf_seq_equal(&l->b[i].seq,
					                &l->b[j].seq)
					                ? WF_KI_SAME_KEYS
					                : WF_KI_PREFIX,
					        (WfKeyContext)c, l->b[i].cmd,
					        l->b[j].cmd, NULL);
				}
			}
		}
		if (!l->n) {
			continue; /* a context this profile does not use */
		}
		total = wf_cmd_list((WfKeyContext)c, ci, 160);
		for (k = 0; k < total && k < 160; k++) {
			bool has = false;

			if (!(ci[k]->flags & WF_CF_REQUIRED)) {
				continue;
			}
			for (i = 0; i < l->n; i++) {
				has = has || l->b[i].cmd == ci[k]->id;
			}
			if (!has) {
				return make_issue(WF_KI_REQUIRED,
				        (WfKeyContext)c, ci[k]->id, 0, NULL);
			}
		}
	}
	return make_issue(WF_KI_NONE, WF_CTX_FM, 0, 0, NULL);
}

static void remove_cmd(List *l, uint16_t cmd)
{
	size_t i, out = 0;

	for (i = 0; i < l->n; i++) {
		if (!(l->b[i].cmd == cmd && !l->b[i].extend)) {
			l->b[out++] = l->b[i];
		}
	}
	l->n = out;
}

static WfStatus apply_overrides(
        WfKeymap *m, const WfKeyConfig *cfg, WfKeyIssue *issue)
{
	size_t i;

	for (i = 0; i < cfg->count; i++) {
		const WfKeyOverride *o = &cfg->o[i];
		const WfCommandInfo *ci = wf_cmd_info(o->cmd);
		uint8_t k;

		if (o->ctx >= WF_CTX_BINDABLE || !ci ||
		        !(ci->ctx_mask & WF_CTX_BIT(o->ctx))) {
			*issue = make_issue(WF_KI_UNKNOWN, (WfKeyContext)o->ctx,
			        o->cmd, 0, NULL);
			return WF_E_INVAL;
		}
		if (!m->ctx[o->ctx].n) {
			continue; /* the profile has no such context: inert */
		}
		remove_cmd(&m->ctx[o->ctx], o->cmd);
		for (k = 0; k < o->n; k++) {
			if (!list_add(&m->ctx[o->ctx], &o->seq[k], o->cmd, 0)) {
				return WF_E_NOMEM;
			}
		}
	}
	return WF_OK;
}

static WfKeymap *create(const WfKeyConfig *cfg, int editor_legacy)
{
	WfKeymap *m = calloc(1, sizeof *m);

	if (!m) {
		return NULL;
	}
	m->profile = (WfProfile)cfg->profile;
	if (WF_PROFILE_VIM == m->profile) {
		m->base = WF_BASE_VIM;
	} else if (WF_PROFILE_CUSTOM == m->profile) {
		m->base = WF_BASE_VIM == cfg->base ? WF_BASE_VIM : WF_BASE_NANO;
	} else {
		m->base = WF_BASE_NANO;
	}
	if (!fill_defaults(m, editor_legacy, cfg->version)) {
		wf_keymap_free(m);
		return NULL;
	}
	return m;
}

WfStatus wf_keymap_build_defaults(
        const WfKeyConfig *cfg, int editor_legacy_keymap, WfKeymap **out)
{
	WfKeyConfig bare;

	wf_keyconfig_init(&bare);
	bare.profile = cfg->profile;
	bare.base = cfg->base;
	bare.version = cfg->version;
	*out = create(&bare, editor_legacy_keymap);
	return *out ? WF_OK : WF_E_NOMEM;
}

WfStatus wf_keymap_build(const WfKeyConfig *cfg,
        int editor_legacy_keymap,
        WfKeymap **out,
        WfKeyIssue *issue)
{
	WfKeyIssue local;
	WfKeymap *m = create(cfg, editor_legacy_keymap);

	if (!issue) {
		issue = &local;
	}
	*out = NULL;
	*issue = make_issue(WF_KI_NONE, WF_CTX_FM, 0, 0, NULL);
	if (!m) {
		return WF_E_NOMEM;
	}
	if (WF_PROFILE_CUSTOM == m->profile) {
		WfStatus st = apply_overrides(m, cfg, issue);

		if (WF_OK != st) {
			wf_keymap_free(m);
			return st;
		}
	}
	*issue = validate(m);
	if (WF_KI_NONE != issue->kind) {
		wf_keymap_free(m);
		return WF_E_INVAL;
	}
	*out = m;
	return WF_OK;
}

static size_t keys_text(const List *l, uint16_t cmd, char *out, size_t cap)
{
	size_t i, at = 0, shown = 0;

	if (!cap) {
		return 0;
	}
	out[0] = '\0';
	for (i = 0; i < l->n && shown < 3; i++) {
		char one[64];
		int n;

		if (l->b[i].cmd != cmd || l->b[i].extend) {
			continue;
		}
		wf_seq_format(&l->b[i].seq, one, sizeof one);
		n = snprintf(
		        out + at, cap - at, "%s%s", shown ? ", " : "", one);
		if (n < 0 || (size_t)n >= cap - at) {
			return strlen(out);
		}
		at += (size_t)n;
		shown++;
	}
	return at < cap ? at : (cap ? cap - 1 : 0);
}

size_t wf_keymap_keys_text(const WfKeymap *m,
        WfKeyContext ctx,
        uint16_t cmd,
        char *out,
        size_t cap)
{
	if ((unsigned)ctx >= WF_CTX_BINDABLE) {
		if (cap) {
			out[0] = '\0';
		}
		return 0;
	}
	return keys_text(&m->ctx[ctx], cmd, out, cap);
}

size_t wf_keymap_first_key(const WfKeymap *m,
        WfKeyContext ctx,
        uint16_t cmd,
        char *out,
        size_t cap)
{
	size_t i;

	if (cap) {
		out[0] = '\0';
	}
	if ((unsigned)ctx >= WF_CTX_BINDABLE) {
		return 0;
	}
	for (i = 0; i < m->ctx[ctx].n; i++) {
		if (m->ctx[ctx].b[i].cmd == cmd && !m->ctx[ctx].b[i].extend) {
			return wf_seq_format(&m->ctx[ctx].b[i].seq, out, cap);
		}
	}
	return 0;
}

void wf_help_walk(const WfKeymap *m,
        const WfKeyContext *ctxs,
        size_t n,
        WfHelpSink sink,
        void *user)
{
	size_t c, i, total;
	const WfCommandInfo *list[160];

	for (c = 0; c < n; c++) {
		bool any = false;

		total = wf_cmd_list(ctxs[c], list, 160);
		for (i = 0; i < total && i < 160; i++) {
			char keys[96];

			wf_keymap_keys_text(
			        m, ctxs[c], list[i]->id, keys, sizeof keys);
			if (!keys[0]) {
				continue;
			}
			if (!any) {
				sink(user, "",
				        wf_msg(wf_context_label(ctxs[c])),
				        true);
				any = true;
			}
			sink(user, keys, wf_msg(list[i]->desc), false);
		}
		if (any) {
			sink(user, "", "", false);
		}
	}
}

size_t wf_keymap_default_text(
        WfBase base, WfKeyContext ctx, uint16_t cmd, char *out, size_t cap)
{
	WfKeyConfig cfg;
	WfKeymap *m;
	size_t n = 0;

	wf_keyconfig_init(&cfg);
	cfg.profile = WF_BASE_VIM == base ? WF_PROFILE_VIM : WF_PROFILE_NANO;
	if (cap) {
		out[0] = '\0';
	}
	if (WF_OK == wf_keymap_build_defaults(&cfg, 0, &m)) {
		n = wf_keymap_keys_text(m, ctx, cmd, out, cap);
		wf_keymap_free(m);
	}
	return n;
}

WfKeyIssue wf_keymap_check(
        const WfKeymap *m, WfKeyContext ctx, uint16_t cmd, const WfSeq *seq)
{
	const List *l;
	size_t i;
	uint8_t k;

	if ((unsigned)ctx >= WF_CTX_BINDABLE) {
		return make_issue(WF_KI_UNKNOWN, ctx, cmd, 0, NULL);
	}
	l = &m->ctx[ctx];
	for (k = 0; k < seq->n; k++) {
		const char *why = NULL;

		if (!wf_stroke_bindable(
		            seq->s[k], ctx <= WF_CTX_PREVIEW, &why)) {
			return make_issue(WF_KI_BAD_KEY, ctx, cmd, 0, why);
		}
	}
	if (seq->n && types_text(ctx) && plain_char(seq->s[0])) {
		return make_issue(WF_KI_BAD_KEY, ctx, cmd, 0,
		        "this mode types the character");
	}
	if (m->counts[ctx] && is_digit_start(seq)) {
		return make_issue(WF_KI_COUNT_DIGIT, ctx, cmd, 0, NULL);
	}
	for (i = 0; i < l->n; i++) {
		if (l->b[i].cmd == cmd) {
			continue; /* its own old keys are being replaced */
		}
		if (clash(&l->b[i].seq, seq)) {
			return make_issue(wf_seq_equal(&l->b[i].seq, seq)
			                ? WF_KI_SAME_KEYS
			                : WF_KI_PREFIX,
			        ctx, cmd, l->b[i].cmd, NULL);
		}
	}
	return make_issue(WF_KI_NONE, ctx, cmd, 0, NULL);
}

void wf_keyconfig_init(WfKeyConfig *k)
{
	memset(k, 0, sizeof *k);
	k->version = WF_BINDINGS_CURRENT;
}

bool wf_keyconfig_equal(const WfKeyConfig *a, const WfKeyConfig *b)
{
	size_t i;
	uint8_t k;

	if (a->profile != b->profile || a->base != b->base ||
	        a->version != b->version || a->count != b->count) {
		return false;
	}
	for (i = 0; i < a->count; i++) {
		const WfKeyOverride *x = &a->o[i],
		                    *y = wf_keyconfig_find(
		                            b, (WfKeyContext)x->ctx, x->cmd);

		if (!y || x->n != y->n) {
			return false;
		}
		for (k = 0; k < x->n; k++) {
			if (!wf_seq_equal(&x->seq[k], &y->seq[k])) {
				return false;
			}
		}
	}
	return true;
}

const WfKeyOverride *wf_keyconfig_find(
        const WfKeyConfig *k, WfKeyContext ctx, uint16_t cmd)
{
	size_t i;

	for (i = 0; i < k->count; i++) {
		if (k->o[i].ctx == ctx && k->o[i].cmd == cmd) {
			return &k->o[i];
		}
	}
	return NULL;
}

bool wf_keyconfig_set(WfKeyConfig *k, const WfKeyOverride *o)
{
	size_t i;

	for (i = 0; i < k->count; i++) {
		if (k->o[i].ctx == o->ctx && k->o[i].cmd == o->cmd) {
			k->o[i] = *o;
			return true;
		}
	}
	if (WF_KEY_OVERRIDES == k->count) {
		return false;
	}
	k->o[k->count++] = *o;
	return true;
}

bool wf_keyconfig_remove(WfKeyConfig *k, WfKeyContext ctx, uint16_t cmd)
{
	size_t i;

	for (i = 0; i < k->count; i++) {
		if (k->o[i].ctx == ctx && k->o[i].cmd == cmd) {
			k->o[i] = k->o[--k->count];
			return true;
		}
	}
	return false;
}

void wf_keystate_reset(WfKeyState *st)
{
	memset(st, 0, sizeof *st);
}

bool wf_keystate_active(const WfKeyState *st)
{
	return st->pending.n || st->count;
}

static WfKeyResult result(WfKeyVerdict v)
{
	WfKeyResult r;

	memset(&r, 0, sizeof r);
	r.verdict = v;
	r.count = 1;
	return r;
}

/* Looks for the pending sequence among the bindings. */
static void probe(
        const List *l, const WfSeq *pend, const WfBinding **exact, bool *longer)
{
	size_t i;
	uint8_t k;

	*exact = NULL;
	*longer = false;
	for (i = 0; i < l->n; i++) {
		const WfBinding *b = &l->b[i];

		if (b->seq.n < pend->n) {
			continue;
		}
		for (k = 0; k < pend->n; k++) {
			if (!wf_stroke_equal(b->seq.s[k], pend->s[k])) {
				break;
			}
		}
		if (k != pend->n) {
			continue;
		}
		if (b->seq.n == pend->n) {
			if (!*exact) {
				*exact = b;
			}
		} else {
			*longer = true;
		}
	}
}

WfKeyResult wf_keymap_feed(const WfKeymap *m,
        WfKeyContext ctx,
        WfKeyState *st,
        WfStroke stroke,
        uint64_t now_ms)
{
	const List *l;
	const WfBinding *hit;
	bool longer, retried = false;
	WfKeyResult r;

	if ((unsigned)ctx >= WF_CTX_BINDABLE) {
		return result(WF_KR_NONE);
	}
	l = &m->ctx[ctx];
	if (wf_keystate_active(st) && now_ms >= st->deadline) {
		wf_keystate_reset(st);
	}
	if (wf_keystate_active(st) && stroke.named &&
	        WF_NK_ESC == stroke.code && !stroke.mods) {
		wf_keystate_reset(st);
		return result(WF_KR_CANCELLED);
	}
	/* a count: digits before anything else is pending */
	if (m->counts[ctx] && 0 == st->pending.n && !stroke.named &&
	        !stroke.mods && stroke.code >= '0' && stroke.code <= '9' &&
	        ('0' != stroke.code || st->count)) {
		uint32_t d = stroke.code - '0';

		st->count = st->count * 10 + d;
		if (st->count > WF_COUNT_MAX) {
			st->count = WF_COUNT_MAX;
		}
		st->deadline = now_ms + WF_SEQ_TIMEOUT_MS;
		return result(WF_KR_PENDING);
	}
	for (;;) { /* a key that begins nothing is tried once more on its own */
		if (WF_SEQ_MAX == st->pending.n) {
			wf_keystate_reset(st);
			return result(WF_KR_NONE);
		}
		st->pending.s[st->pending.n++] = stroke;
		probe(l, &st->pending, &hit, &longer);
		if (hit && !longer) {
			r = result(WF_KR_COMMAND);
			r.cmd = hit->cmd;
			r.extend = hit->extend;
			if (st->count) {
				const WfCommandInfo *ci = wf_cmd_info(hit->cmd);

				r.counted = true;
				r.count = st->count;
				if (!ci ||
				        !(ci->flags &
				                (WF_CF_COUNT | WF_CF_LINE))) {
					wf_keystate_reset(st);
					return result(WF_KR_REFUSED);
				}
			}
			wf_keystate_reset(st);
			return r;
		}
		if (longer) {
			st->deadline = now_ms + WF_SEQ_TIMEOUT_MS;
			return result(WF_KR_PENDING);
		}
		/* nothing begins with it: forget the prefix, and give the key one more
		 * chance on its own (a stray g before j must not eat the j) */
		if (st->pending.n > 1 && !retried) {
			uint32_t keep = st->count;

			wf_keystate_reset(st);
			st->count = keep;
			retried = true;
			continue;
		}
		wf_keystate_reset(st);
		return result(WF_KR_NONE);
	}
}

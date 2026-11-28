#include "wf_ekeymap.h"

#include <string.h>

typedef struct
{
	WfEventType type;
	uint32_t ch; /* WF_EV_CTRL / CTRL_SHIFT / ALT */
	WfKey key;   /* WF_EV_KEY */
	WfEdCmd cmd;
	bool extend;
	const char *name; /* for the shortcut bar */
} Bind;

#define CTRL(c, cmd, name) {WF_EV_CTRL, c, WF_KEY_NONE, cmd, false, name}
#define CSHIFT(c, cmd, name)                                                   \
	{WF_EV_CTRL_SHIFT, c, WF_KEY_NONE, cmd, false, name}
#define ALT(c, cmd, name) {WF_EV_ALT, c, WF_KEY_NONE, cmd, false, name}
#define KEY(k, cmd, name) {WF_EV_KEY, 0, k, cmd, false, name}
#define XKEY(k, cmd) {WF_EV_KEY, 0, k, cmd, true, NULL}

/* Bindings both presets share: the cursor keys and the editing keys. */
static const Bind COMMON[] = {KEY(WF_KEY_LEFT, WF_CMD_LEFT, NULL),
        KEY(WF_KEY_RIGHT, WF_CMD_RIGHT, NULL), KEY(WF_KEY_UP, WF_CMD_UP, NULL),
        KEY(WF_KEY_DOWN, WF_CMD_DOWN, NULL),
        KEY(WF_KEY_HOME, WF_CMD_HOME, NULL), KEY(WF_KEY_END, WF_CMD_END, NULL),
        KEY(WF_KEY_PGUP, WF_CMD_PGUP, NULL),
        KEY(WF_KEY_PGDN, WF_CMD_PGDN, NULL),
        KEY(WF_KEY_CTRL_HOME, WF_CMD_DOC_START, NULL),
        KEY(WF_KEY_CTRL_END, WF_CMD_DOC_END, NULL),
        KEY(WF_KEY_CTRL_LEFT, WF_CMD_WORD_LEFT, NULL),
        KEY(WF_KEY_CTRL_RIGHT, WF_CMD_WORD_RIGHT, NULL),
        XKEY(WF_KEY_SHIFT_LEFT, WF_CMD_LEFT),
        XKEY(WF_KEY_SHIFT_RIGHT, WF_CMD_RIGHT),
        XKEY(WF_KEY_SHIFT_UP, WF_CMD_UP), XKEY(WF_KEY_SHIFT_DOWN, WF_CMD_DOWN),
        XKEY(WF_KEY_SHIFT_HOME, WF_CMD_HOME),
        XKEY(WF_KEY_SHIFT_END, WF_CMD_END),
        XKEY(WF_KEY_SHIFT_PGUP, WF_CMD_PGUP),
        XKEY(WF_KEY_SHIFT_PGDN, WF_CMD_PGDN),
        XKEY(WF_KEY_CTRL_SHIFT_LEFT, WF_CMD_WORD_LEFT),
        XKEY(WF_KEY_CTRL_SHIFT_RIGHT, WF_CMD_WORD_RIGHT),
        XKEY(WF_KEY_CTRL_SHIFT_HOME, WF_CMD_DOC_START),
        XKEY(WF_KEY_CTRL_SHIFT_END, WF_CMD_DOC_END),
        KEY(WF_KEY_ENTER, WF_CMD_NEWLINE, NULL),
        KEY(WF_KEY_BACKSPACE, WF_CMD_BACKSPACE, NULL),
        KEY(WF_KEY_DELETE, WF_CMD_DELETE, NULL),
        KEY(WF_KEY_TAB, WF_CMD_TAB, NULL),
        KEY(WF_KEY_SHIFT_TAB, WF_CMD_BACKTAB, NULL),
        KEY(WF_KEY_CTRL_BACKSPACE, WF_CMD_BACKSPACE, NULL),
        KEY(WF_KEY_CTRL_DELETE, WF_CMD_DELETE, NULL),
        KEY(WF_KEY_SHIFT_DELETE, WF_CMD_CUT, NULL),
        KEY(WF_KEY_CTRL_INSERT, WF_CMD_COPY, NULL),
        KEY(WF_KEY_SHIFT_INSERT, WF_CMD_PASTE, NULL),
        KEY(WF_KEY_F3, WF_CMD_FIND_NEXT, "F3"),
        ALT('.', WF_CMD_NEXT_BUFFER, "M-."),
        ALT(',', WF_CMD_PREV_BUFFER, "M-,"), ALT('>', WF_CMD_NEXT_BUFFER, NULL),
        ALT('<', WF_CMD_PREV_BUFFER, NULL), ALT('3', WF_CMD_COMMENT, NULL),
        ALT(';', WF_CMD_COMPLETE, NULL), CTRL('r', WF_CMD_INSERT_FILE, "^R"),
        ALT('m', WF_CMD_MD_PREVIEW, "M-M"), CTRL('s', WF_CMD_SAVE, "^S"),
        CTRL('x', WF_CMD_EXIT, "^X"), KEY(WF_KEY_F2, WF_CMD_HELP, NULL),
        {WF_EV_CHAR, 0, WF_KEY_NONE, WF_CMD_NONE, false, NULL}};

static const Bind PIERO[] = {KEY(WF_KEY_ESC, WF_CMD_EXIT, NULL),
        CSHIFT('s', WF_CMD_SAVE_AS, "^+S"), ALT('s', WF_CMD_SAVE_AS, "M-S"),
        CTRL('f', WF_CMD_FIND, "^F"), CTRL('h', WF_CMD_REPLACE, "^H"),
        CTRL('g', WF_CMD_GOTO, "^G"), CTRL('z', WF_CMD_UNDO, "^Z"),
        CTRL('y', WF_CMD_REDO, "^Y"), CTRL('c', WF_CMD_COPY, "^C"),
        CTRL('v', WF_CMD_PASTE, "^V"), ALT('x', WF_CMD_CUT, "M-X"),
        CTRL('a', WF_CMD_SELECT_ALL, "^A"), CTRL('k', WF_CMD_DELETE_LINE, "^K"),
        CTRL('o', WF_CMD_OPEN_FILE, "^O"), CTRL('n', WF_CMD_COMPLETE, "^N"),
        CTRL('_', WF_CMD_COMMENT, "^/"), ALT('n', WF_CMD_FIND_NEXT, NULL),
        ALT('p', WF_CMD_FIND_PREV, NULL), ALT('/', WF_CMD_COMPLETE, NULL),
        {WF_EV_CHAR, 0, WF_KEY_NONE, WF_CMD_NONE, false, NULL}};

static const Bind NANO[] = {KEY(WF_KEY_ESC, WF_CMD_CANCEL, NULL),
        KEY(WF_KEY_CTRL_DELETE, WF_CMD_DELETE_WORD_RIGHT, NULL),
        KEY(WF_KEY_CTRL_INSERT, WF_CMD_SYSTEM_COPY, NULL),
        KEY(WF_KEY_SHIFT_INSERT, WF_CMD_SYSTEM_PASTE, NULL),
        CTRL('o', WF_CMD_SAVE_AS, "^O"), CTRL('f', WF_CMD_FIND, "^F"),
        CTRL('b', WF_CMD_FIND_BACK, "^B"), ALT('r', WF_CMD_REPLACE, "M-R"),
        CTRL('k', WF_CMD_CUT, "^K"), CTRL('u', WF_CMD_PASTE, "^U"),
        ALT('6', WF_CMD_COPY, "M-6"), CTRL('6', WF_CMD_MARK, "^6"),
        ALT('a', WF_CMD_MARK, NULL), ALT('u', WF_CMD_UNDO, "M-U"),
        ALT('e', WF_CMD_REDO, "M-E"), CTRL('_', WF_CMD_GOTO, "^_"),
        CTRL('a', WF_CMD_HOME, NULL), CTRL('e', WF_CMD_END, NULL),
        CTRL('y', WF_CMD_PGUP, NULL), CTRL('v', WF_CMD_PGDN, NULL),
        CTRL('p', WF_CMD_UP, NULL), CTRL('n', WF_CMD_DOWN, NULL),
        CTRL('d', WF_CMD_DELETE, NULL), CTRL('h', WF_CMD_BACKSPACE, NULL),
        CTRL('c', WF_CMD_CANCEL, NULL), ALT('\\', WF_CMD_DOC_START, NULL),
        ALT('/', WF_CMD_DOC_END, NULL), ALT('n', WF_CMD_FIND_NEXT, NULL),
        ALT('w', WF_CMD_FIND_NEXT, "M-W"), ALT('q', WF_CMD_FIND_PREV, NULL),
        ALT('f', WF_CMD_FIND_NEXT, "M-F"), ALT('b', WF_CMD_FIND_PREV, "M-B"),
        ALT('g', WF_CMD_GOTO, "M-G"), CTRL('g', WF_CMD_HELP, "^G"),
        {WF_EV_CHAR, 0, WF_KEY_NONE, WF_CMD_NONE, false, NULL}};

static bool matches(const Bind *b, const WfEvent *ev)
{
	if (b->type != ev->type) {
		return false;
	}
	return WF_EV_KEY == b->type ? b->key == ev->key : b->ch == ev->ch;
}

static const Bind *find(const Bind *table, const WfEvent *ev)
{
	for (; WF_CMD_NONE != table->cmd; table++) {
		if (matches(table, ev)) {
			return table;
		}
	}
	return NULL;
}

WfEdAction wf_ekm_lookup(WfKeymapId id, const WfEvent *ev)
{
	WfEdAction none = {WF_CMD_NONE, false};
	const Bind *b;

	if (WF_KM_PIERO == id) {
		b = find(PIERO, ev);
	} else if (WF_KM_NANO == id) {
		b = find(NANO, ev);
	} else {
		return none;
	}
	if (!b) {
		b = find(COMMON, ev);
	}
	if (b) {
		WfEdAction a = {b->cmd, b->extend};

		return a;
	}
	return none;
}

static bool export_table(const Bind *t, WfEkBinding *out, size_t cap, size_t *n)
{
	for (; WF_CMD_NONE != t->cmd; t++) {
		WfEvent ev = {t->type, t->ch, t->key, 0};
		WfStroke s;
		size_t i;

		if (!wf_stroke_from_event(&ev, &s)) {
			continue;
		}
		for (i = 0; i < *n; i++) {
			if (wf_stroke_equal(out[i].stroke, s)) {
				break;
			}
		}
		if (i < *n) {
			continue; /* shadowed by the preset's own key */
		}
		if (*n == cap) {
			return false;
		}
		out[*n].stroke = s;
		out[*n].cmd = t->cmd;
		out[*n].extend = t->extend;
		(*n)++;
	}
	return true;
}

size_t wf_ekm_export(WfKeymapId id, WfEkBinding *out, size_t cap)
{
	size_t n = 0;

	if (WF_KM_PIERO != id && WF_KM_NANO != id) {
		return 0;
	}
	if (export_table(WF_KM_NANO == id ? NANO : PIERO, out, cap, &n)) {
		(void)export_table(COMMON, out, cap, &n);
	}
	return n;
}

const char *wf_ekm_key_name(WfKeymapId id, WfEdCmd cmd)
{
	const Bind *t = WF_KM_NANO == id ? NANO : PIERO;
	const Bind *b;

	for (b = t; WF_CMD_NONE != b->cmd; b++) {
		if (b->cmd == cmd && b->name && !b->extend) {
			return b->name;
		}
	}
	for (b = COMMON; WF_CMD_NONE != b->cmd; b++) {
		if (b->cmd == cmd && b->name && !b->extend) {
			return b->name;
		}
	}
	return NULL;
}

const char *wf_ekm_preset_name(WfKeymapId id)
{
	return WF_KM_NANO == id ? "NANO" : "STANDARD";
}

bool wf_ekm_from_name(const char *name, WfKeymapId *id, bool *reserved)
{
	if (reserved) {
		*reserved = false;
	}
	if (!strcmp(name, "piero")) {
		*id = WF_KM_PIERO;
		return true;
	}
	if (!strcmp(name, "nano")) {
		*id = WF_KM_NANO;
		return true;
	}
	if (!strcmp(name, "vim")) {
		if (reserved) {
			*reserved = true;
		}
		return false;
	}
	return false;
}

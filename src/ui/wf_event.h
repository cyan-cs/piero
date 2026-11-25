#ifndef WF_EVENT_H
#define WF_EVENT_H

#include <stdint.h>

typedef enum
{
	WF_EV_CHAR, /* printable Unicode scalar in .ch (judged by character, not
	               VK) */
	WF_EV_KEY,  /* navigation key in .key */
	WF_EV_RELEASE, /* a character key went up (.ch / .vk); used to end
	                  key-hold tracking */
	WF_EV_RESIZE,  /* terminal size changed; the loop re-queries it */
	/* Chords reported in editing mode or with modified-key reporting enabled.
	 * Outside editing mode Ctrl+C remains a console signal. */
	WF_EV_CTRL,       /* Ctrl chord in .ch (letters lower case) */
	WF_EV_CTRL_SHIFT, /* Ctrl + Shift + letter in .ch (lower case) */
	WF_EV_ALT,        /* Alt + character in .ch (lower case) */
	/* File manager only. .ch = x | (y << 16), the cell the pointer was in;
	 * .vk = WF_MOUSE_* flags. */
	WF_EV_MOUSE
} WfEventType;

enum
{
	WF_MOUSE_LEFT = 1,   /* the left button went down */
	WF_MOUSE_RIGHT = 2,  /* the right button went down */
	WF_MOUSE_DOUBLE = 4, /* with LEFT: it was a double click */
	WF_MOUSE_WHEEL_UP = 8,
	WF_MOUSE_WHEEL_DOWN = 16
};
#define WF_MOUSE_X(ev) ((int)((ev)->ch & 0xFFFFu))
#define WF_MOUSE_Y(ev) ((int)((ev)->ch >> 16))

/*
 * Set in .vk of a character, Enter or Tab that arrived as part of pasted
 * text. The console hands a paste over as ordinary key records (the
 * bracketed-paste markers are consumed by the console host and never reach
 * the program), so this is a judgement about how the keys arrived:
 *   - one read holds WF_PASTE_MIN_KEYS or more text keys that are not all the
 *     same key (a held key is not a paste), or an Enter with another text key;
 *   - and every text key that follows within WF_PASTE_TAIL_MS of the last one
 *     (a long paste arrives in several reads).
 * A single pasted character, or two, cannot be told from typing. Text fields
 * take pasted keys as text but a pasted Enter or Tab never confirms; a
 * question never takes them; the file manager's command keys must not run
 * them.
 */
#define WF_VK_PASTED 0x8000u
#define WF_PASTE_MIN_KEYS 3u
#define WF_PASTE_TAIL_MS 30u

typedef enum
{
	WF_KEY_NONE = 0,
	WF_KEY_UP,
	WF_KEY_DOWN,
	WF_KEY_LEFT,
	WF_KEY_RIGHT,
	WF_KEY_ENTER,
	WF_KEY_ESC,
	WF_KEY_BACKSPACE,
	WF_KEY_TAB,
	WF_KEY_F2,
	WF_KEY_CTRL_UP,
	WF_KEY_CTRL_DOWN,
	WF_KEY_DELETE,
	WF_KEY_HOME,
	WF_KEY_END,
	WF_KEY_CTRL_L,
	WF_KEY_PGUP,
	WF_KEY_PGDN,
	WF_KEY_INSERT,
	WF_KEY_F3,
	WF_KEY_SHIFT_TAB,
	WF_KEY_CTRL_HOME,
	WF_KEY_CTRL_END,
	WF_KEY_CTRL_LEFT,
	WF_KEY_CTRL_RIGHT,
	WF_KEY_CTRL_BACKSPACE,
	WF_KEY_CTRL_DELETE,
	WF_KEY_SHIFT_UP,
	WF_KEY_SHIFT_DOWN,
	WF_KEY_SHIFT_LEFT,
	WF_KEY_SHIFT_RIGHT,
	WF_KEY_SHIFT_HOME,
	WF_KEY_SHIFT_END,
	WF_KEY_SHIFT_PGUP,
	WF_KEY_SHIFT_PGDN,
	WF_KEY_CTRL_SHIFT_LEFT,
	WF_KEY_CTRL_SHIFT_RIGHT,
	WF_KEY_CTRL_SHIFT_HOME,
	WF_KEY_CTRL_SHIFT_END,
	WF_KEY_CTRL_INSERT,
	WF_KEY_SHIFT_INSERT,
	WF_KEY_SHIFT_DELETE
} WfKey;

typedef struct
{
	WfEventType type;
	uint32_t ch;
	WfKey key;
	uint16_t vk; /* virtual-key code, 0 if unknown; identifies the physical
	                key */
} WfEvent;

#endif

#include "wf_term.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../text/wf_text.h"
#include "../base/wf_macro.h"
#include "../base/wf_mem.h"

#define CHUNK_UNITS 16384

static const char enter_seq[] = "\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J";
static const char reset_seq[] =
        "\x1b[0m\x1b[?7h\x1b[?25h\x1b[?2026l\x1b[?1049l";

struct WfTerm
{
	HANDLE in, out;
	WfTermCaps caps;
	bool owns_in, owns_out;
	DWORD saved_in_mode, saved_out_mode;
	bool in_mode_saved, out_mode_saved, entered;
	HANDLE shutdown; /* borrowed: g_shutdown lives for the whole process */
	wchar_t pending_high; /* high surrogate waiting for its pair, or 0 */
	WfEvent kept[16];     /* key-ups preserved across wf_term_drain_input */
	size_t nkept;
	/* What a read could not hand back yet: the rest of a key that was held down
	 * (a record says how many times it repeated) and the records after it. */
	WfEvent rep_ev;
	DWORD rep_left;
	INPUT_RECORD pend[64];
	size_t npend;
	bool editing; /* wf_term_set_editing(): modified keys are reported */
	bool modified; /* wf_term_set_modified_keys(): the same, Ctrl+C a signal */
	ULONGLONG cr_tick;   /* when the last Enter (CR) key went down, or 0 */
	DWORD mouse_buttons; /* buttons held at the last mouse record */
	ULONGLONG paste_until; /* keys before this tick continue a paste */
};

/* A pasted line break arrives as Enter and then Ctrl+Enter (CR, LF): the LF
 * of the same press must not be a second Enter. */
#define CRLF_MS 50u

/*
 * Two different contracts, because the two situations differ.
 *
 * Interrupt (Ctrl+C / Ctrl+Break): the process keeps running. The handler only
 * raises the shutdown event and returns; the main thread performs the full,
 * orderly cleanup (workers, journals, terminal) and exits by itself.
 *
 * Forced close (console close / logoff / shutdown): the system ends the
 * process as soon as the handler returns, so this is crash-equivalent. Only
 * the terminal restore is promised, nothing else; anything that must survive
 * has to be safe against the process vanishing at any point. The handler
 * raises the shutdown event and waits, bounded, for wf_term_close() to have
 * restored the console. Measured (P0-T08, ConPTY): if the handler returns at
 * once, the process is gone within milliseconds and the console is never
 * restored. The system gives up on a handler after about 5 s, so the bound
 * stays below that; a stalled main thread therefore costs exactly the bound,
 * never a hang.
 */
#define CLOSE_GRACE_MS 3000

/* Both events are created once and never closed: the console control handler
 * runs on a system thread and must never signal a handle that was closed (and
 * possibly reused) by wf_term_close(). */
static HANDLE g_shutdown;
static HANDLE g_restored; /* manual-reset; set at the end of wf_term_close() */
static bool g_handler_installed;
static volatile LONG
        g_close_reason; /* WfCloseReason; only ever written via Interlocked */

static BOOL WINAPI ctrl_handler(DWORD type)
{
	switch (type) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
		InterlockedCompareExchange(
		        &g_close_reason, WF_CLOSE_INTERRUPT, WF_CLOSE_NONE);
		SetEvent(g_shutdown);
		return TRUE;
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		InterlockedExchange(&g_close_reason,
		        WF_CLOSE_FORCED); /* forced wins over interrupt */
		SetEvent(g_shutdown);
		if (g_restored) {
			WaitForSingleObject(g_restored, CLOSE_GRACE_MS);
		}
		return TRUE;
	}
	return FALSE;
}

WfCloseReason wf_term_close_reason(void)
{
	LONG reason = InterlockedCompareExchange(&g_close_reason, 0, 0);

	return (WfCloseReason)reason;
}

static bool is_console(HANDLE h)
{
	DWORD mode;

	return NULL != h && INVALID_HANDLE_VALUE != h &&
	        GetConsoleMode(h, &mode);
}

/* Uses the std handle if it is a console, else opens CONIN$/CONOUT$. */
static HANDLE acquire(
        DWORD std_id, const wchar_t *name, DWORD access, bool *owned)
{
	HANDLE h = GetStdHandle(std_id);

	*owned = false;
	if (is_console(h)) {
		return h;
	}
	h = CreateFileW(name, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
	        OPEN_EXISTING, 0, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return NULL;
	}
	*owned = true;
	return h;
}

const char *wf_term_reset_sequence(void)
{
	return reset_seq;
}

void wf_term_close(WfTerm *t)
{
	if (!t) {
		return;
	}
	if (t->entered) {
		wf_term_write(t, reset_seq, sizeof reset_seq - 1);
	}
	if (t->out_mode_saved) {
		SetConsoleMode(t->out, t->saved_out_mode);
	}
	if (t->in_mode_saved) {
		SetConsoleMode(t->in, t->saved_in_mode);
	}
	/* The handler stays registered on purpose. Unregistering waits for a
	 * handler that is currently running, and a console-close handler is
	 * waiting for *this* function to finish: measured as a mutual wait that
	 * only the 3 s timeout resolved (P0-T08). It touches nothing but
	 * globals. */
	if (t->owns_in && t->in) {
		CloseHandle(t->in);
	}
	if (t->owns_out && t->out) {
		CloseHandle(t->out);
	}
	free(t);
	if (g_restored) {
		SetEvent(g_restored); /* releases a forced-close handler: the
		                         console is back */
	}
}

WfStatus wf_term_open(WfTerm **out)
{
	WfTerm *t;
	DWORD mode;
	WfStatus st = WF_E_OS;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	t = calloc(1, sizeof *t);
	if (!t) {
		return WF_E_NOMEM;
	}

	t->in = acquire(STD_INPUT_HANDLE, L"CONIN$",
	        GENERIC_READ | GENERIC_WRITE, &t->owns_in);
	t->out = acquire(STD_OUTPUT_HANDLE, L"CONOUT$",
	        GENERIC_READ | GENERIC_WRITE, &t->owns_out);
	if (!t->in || !t->out) {
		goto fail;
	}

	if (!g_shutdown) {
		g_shutdown = CreateEventW(NULL, TRUE, FALSE, NULL);
		if (!g_shutdown) {
			goto fail;
		}
	}
	if (!g_restored) { /* process-lifetime: a handler thread may still be
	                      waiting on it */
		g_restored = CreateEventW(NULL, TRUE, FALSE, NULL);
		if (!g_restored) {
			goto fail;
		}
	}
	ResetEvent(g_restored);
	ResetEvent(g_shutdown);
	InterlockedExchange(&g_close_reason, WF_CLOSE_NONE);
	t->shutdown = g_shutdown;
	if (!g_handler_installed) {
		if (!SetConsoleCtrlHandler(ctrl_handler, TRUE)) {
			goto fail;
		}
		g_handler_installed = true;
	}

	if (!GetConsoleMode(t->in, &t->saved_in_mode) ||
	        !GetConsoleMode(t->out, &t->saved_out_mode)) {
		goto fail;
	}
	t->in_mode_saved = t->out_mode_saved = true;

	/* EXTENDED_FLAGS without QUICK_EDIT turns quick-edit off.
	 * PROCESSED_INPUT stays on so Ctrl+C reaches the handler. No VT input,
	 * no mouse. */
	if (!SetConsoleMode(t->in,
	            ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS |
	                    ENABLE_PROCESSED_INPUT | ENABLE_MOUSE_INPUT)) {
		goto fail;
	}

	mode = ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
	        DISABLE_NEWLINE_AUTO_RETURN;
	if (!SetConsoleMode(t->out, mode) &&
	        !SetConsoleMode(
	                t->out, mode & ~(DWORD)DISABLE_NEWLINE_AUTO_RETURN)) {
		goto fail; /* no VT: refuse rather than print escape garbage */
	}

	t->entered = true;
	st = wf_term_write(t, enter_seq, sizeof enter_seq - 1);
	if (WF_OK != st) {
		goto fail;
	}
	wf_term_probe(t);
	*out = t;
	return WF_OK;
fail:
	wf_term_close(t);
	return st;
}

HANDLE wf_term_input_handle(const WfTerm *t)
{
	return t->in;
}

HANDLE wf_term_shutdown_event(const WfTerm *t)
{
	return t->shutdown;
}

const WfTermCaps *wf_term_capabilities(const WfTerm *t)
{
	return &t->caps;
}

void wf_term_probe(WfTerm *t)
{
	const char *q = wf_term_caps_query();
	wf_term_caps_begin(&t->caps, GetTickCount64());
	(void)wf_term_write(t, q, strlen(q));
}

WfStatus wf_term_size(const WfTerm *t, int *cols, int *rows)
{
	CONSOLE_SCREEN_BUFFER_INFO info;

	if (!GetConsoleScreenBufferInfo(t->out, &info)) {
		return WF_E_OS;
	}
	*cols = info.srWindow.Right - info.srWindow.Left + 1;
	*rows = info.srWindow.Bottom - info.srWindow.Top + 1;
	return WF_OK;
}

WfStatus wf_term_set_editing(WfTerm *t, bool on)
{
	DWORD mode = ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;

	if (!t) {
		return WF_E_INVAL;
	}
	if (!on) {
		/* Ctrl+C is a signal again; the pointer works in the file list */
		mode |= ENABLE_PROCESSED_INPUT | ENABLE_MOUSE_INPUT;
	}
	if (!SetConsoleMode(t->in, mode)) {
		return WF_E_OS;
	}
	t->editing = on;
	t->nkept = 0;
	t->rep_left = 0;
	t->npend = 0;
	t->pending_high = 0;
	return WF_OK;
}

WfStatus wf_term_set_modified_keys(WfTerm *t, bool on)
{
	if (!t) {
		return WF_E_INVAL;
	}
	if (t->modified != on) {
		t->modified = on;
		t->nkept = 0;
		t->rep_left = 0;
		t->npend = 0;
		t->pending_high = 0;
	}
	return WF_OK;
}

WfStatus wf_term_write(WfTerm *t, const char *utf8, size_t n)
{
	wchar_t *w;
	size_t units, pos = 0;
	WfStatus st = WF_OK;

	if (0 == n) {
		return WF_OK;
	}
	if (!t || !utf8) {
		return WF_E_INVAL;
	}
	if (n > (size_t)-1 / sizeof *w) {
		return WF_E_RANGE;
	}
	w = wf_malloc_array(n, sizeof *w);
	if (!w) {
		return WF_E_NOMEM;
	}
	st = wf_utf8_to_utf16(utf8, n, w, n, &units);
	if (WF_OK != st) {
		goto out;
	}

	while (pos < units) {
		size_t len =
		        wf_utf16_chunk_end(w + pos, units - pos, CHUNK_UNITS);
		size_t done = 0;

		while (done < len) {
			DWORD wrote = 0;

			if (!WriteConsoleW(t->out, w + pos + done,
			            (DWORD)(len - done), &wrote, NULL) ||
			        0 == wrote) {
				st = WF_E_OS;
				goto out;
			}
			done += wrote;
		}
		pos += len;
	}
out:
	free(w);
	return st;
}

static WfKey map_vk(WORD vk, DWORD controls)
{
	if ((controls & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) &&
	        !(controls &
	                (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED |
	                        SHIFT_PRESSED))) {
		if (VK_UP == vk) {
			return WF_KEY_CTRL_UP;
		}
		if (VK_DOWN == vk) {
			return WF_KEY_CTRL_DOWN;
		}
		if ('L' == vk) {
			return WF_KEY_CTRL_L;
		}
	}
	switch (vk) {
	case VK_UP:     return WF_KEY_UP;
	case VK_DOWN:   return WF_KEY_DOWN;
	case VK_LEFT:   return WF_KEY_LEFT;
	case VK_RIGHT:  return WF_KEY_RIGHT;
	case VK_RETURN: return WF_KEY_ENTER;
	case VK_ESCAPE: return WF_KEY_ESC;
	case VK_BACK:   return WF_KEY_BACKSPACE;
	case VK_TAB:    return WF_KEY_TAB;
	case VK_F2:     return WF_KEY_F2;
	case VK_DELETE: return WF_KEY_DELETE;
	case VK_HOME:   return WF_KEY_HOME;
	case VK_END:    return WF_KEY_END;
	/* paging: the PDF preview turns pages with these; the editor always had
	 * them (edit_key), the file manager's reader did not deliver them */
	case VK_PRIOR: return WF_KEY_PGUP;
	case VK_NEXT:  return WF_KEY_PGDN;
	}
	return WF_KEY_NONE;
}

static uint32_t lower_ascii(uint32_t c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* Navigation and editing keys with Shift and/or Ctrl held. */
static WfKey edit_key(WORD vk, bool ctrl, bool shift)
{
	switch (vk) {
	case VK_UP:
		return shift   ? WF_KEY_SHIFT_UP
		        : ctrl ? WF_KEY_CTRL_UP
		               : WF_KEY_UP;
	case VK_DOWN:
		return shift   ? WF_KEY_SHIFT_DOWN
		        : ctrl ? WF_KEY_CTRL_DOWN
		               : WF_KEY_DOWN;
	case VK_LEFT:
		return ctrl && shift ? WF_KEY_CTRL_SHIFT_LEFT
		        : ctrl       ? WF_KEY_CTRL_LEFT
		        : shift      ? WF_KEY_SHIFT_LEFT
		                     : WF_KEY_LEFT;
	case VK_RIGHT:
		return ctrl && shift ? WF_KEY_CTRL_SHIFT_RIGHT
		        : ctrl       ? WF_KEY_CTRL_RIGHT
		        : shift      ? WF_KEY_SHIFT_RIGHT
		                     : WF_KEY_RIGHT;
	case VK_HOME:
		return ctrl && shift ? WF_KEY_CTRL_SHIFT_HOME
		        : ctrl       ? WF_KEY_CTRL_HOME
		        : shift      ? WF_KEY_SHIFT_HOME
		                     : WF_KEY_HOME;
	case VK_END:
		return ctrl && shift ? WF_KEY_CTRL_SHIFT_END
		        : ctrl       ? WF_KEY_CTRL_END
		        : shift      ? WF_KEY_SHIFT_END
		                     : WF_KEY_END;
	case VK_PRIOR: return shift ? WF_KEY_SHIFT_PGUP : WF_KEY_PGUP;
	case VK_NEXT:  return shift ? WF_KEY_SHIFT_PGDN : WF_KEY_PGDN;
	case VK_INSERT:
		return ctrl     ? WF_KEY_CTRL_INSERT
		        : shift ? WF_KEY_SHIFT_INSERT
		                : WF_KEY_INSERT;
	case VK_DELETE:
		return ctrl     ? WF_KEY_CTRL_DELETE
		        : shift ? WF_KEY_SHIFT_DELETE
		                : WF_KEY_DELETE;
	case VK_BACK:   return ctrl ? WF_KEY_CTRL_BACKSPACE : WF_KEY_BACKSPACE;
	case VK_TAB:    return shift ? WF_KEY_SHIFT_TAB : WF_KEY_TAB;
	case VK_F3:     return WF_KEY_F3;
	case VK_F2:     return WF_KEY_F2;
	case VK_RETURN: return WF_KEY_ENTER;
	case VK_ESCAPE: return WF_KEY_ESC;
	}
	return WF_KEY_NONE;
}

/*
 * Key-down records in editing mode. Returns true when the record was handled
 * here (*ev filled); false leaves it to the ordinary translation, which also
 * covers plain characters, so typing and IME input behave as before.
 *
 * AltGr arrives as Ctrl+Alt on many layouts and produces real characters; a
 * record that carries a printable character together with Ctrl+Alt is that
 * and is left alone. Alt on its own with a character is an Alt chord.
 */
static bool translate_editing(const KEY_EVENT_RECORD *k, WfEvent *ev)
{
	DWORD m = k->dwControlKeyState;
	bool ctrl = 0 != (m & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
	bool alt = 0 != (m & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED));
	bool shift = 0 != (m & SHIFT_PRESSED);
	wchar_t c = k->uChar.UnicodeChar;
	WORD vk = k->wVirtualKeyCode;
	WfKey key;

	if (ctrl && alt && c >= 0x20) {
		return false; /* AltGr */
	}
	if (ctrl && !alt) {
		if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
			ev->type = shift ? WF_EV_CTRL_SHIFT : WF_EV_CTRL;
			ev->ch = lower_ascii(vk);
			ev->key = WF_KEY_NONE;
			return true;
		}
		if (0x1C == c) {
			ev->type =
			        WF_EV_CTRL; /* Ctrl+\ (the Yen key on a JIS board) */
			ev->ch = '\\';
			ev->key = WF_KEY_NONE;
			return true;
		}
		if (0x1F == c || (VK_OEM_MINUS == vk && shift)) {
			ev->type =
			        WF_EV_CTRL; /* Ctrl+_ (and Ctrl+/ on some layouts) */
			ev->ch = '_';
			ev->key = WF_KEY_NONE;
			return true;
		}
	}
	if (alt && !ctrl && c >= 0x20 && 0x7F != c) {
		ev->type = WF_EV_ALT;
		ev->ch = lower_ascii(c);
		ev->key = WF_KEY_NONE;
		return true;
	}
	if (alt && !ctrl &&
	        ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))) {
		ev->type = WF_EV_ALT;
		ev->ch = lower_ascii(vk);
		ev->key = WF_KEY_NONE;
		return true;
	}
	key = edit_key(vk, ctrl, shift);
	if (WF_KEY_NONE != key && !(c >= 0x20 && 0x7F != c)) {
		ev->type = WF_EV_KEY;
		ev->key = key;
		return true;
	}
	return false;
}

/* True and fills *ev if the record yields an event. */
/* A press of the left or right button, a double click, or a wheel step. Moves
 * and releases are not events. */
static bool translate_mouse(WfTerm *t, const MOUSE_EVENT_RECORD *m, WfEvent *ev)
{
	DWORD down = m->dwButtonState &
	        (FROM_LEFT_1ST_BUTTON_PRESSED | RIGHTMOST_BUTTON_PRESSED);
	DWORD fresh = down & ~t->mouse_buttons;
	unsigned flags = 0;

	if (m->dwEventFlags & MOUSE_WHEELED) {
		flags = (short)HIWORD(m->dwButtonState) > 0
		        ? WF_MOUSE_WHEEL_UP
		        : WF_MOUSE_WHEEL_DOWN;
	} else {
		t->mouse_buttons = down;
		if (fresh & FROM_LEFT_1ST_BUTTON_PRESSED) {
			flags = WF_MOUSE_LEFT;
			if (m->dwEventFlags & DOUBLE_CLICK) {
				flags |= WF_MOUSE_DOUBLE;
			}
		} else if (fresh & RIGHTMOST_BUTTON_PRESSED) {
			flags = WF_MOUSE_RIGHT;
		}
	}
	if (!flags || t->editing || m->dwMousePosition.X < 0 ||
	        m->dwMousePosition.Y < 0) {
		return false;
	}
	ev->type = WF_EV_MOUSE;
	ev->ch = (uint32_t)m->dwMousePosition.X |
	        ((uint32_t)m->dwMousePosition.Y << 16);
	ev->key = WF_KEY_NONE;
	ev->vk = (uint16_t)flags;
	return true;
}

static bool translate(WfTerm *t, const INPUT_RECORD *rec, WfEvent *ev)
{
	const KEY_EVENT_RECORD *k;
	wchar_t c;

	if (MOUSE_EVENT == rec->EventType) {
		return translate_mouse(t, &rec->Event.MouseEvent, ev);
	}
	if (WINDOW_BUFFER_SIZE_EVENT == rec->EventType) {
		wf_term_probe(t);
		ev->type = WF_EV_RESIZE;
		return 1;
	}
	if (FOCUS_EVENT == rec->EventType && rec->Event.FocusEvent.bSetFocus) {
		wf_term_probe(t);
		ev->type = WF_EV_RESIZE;
		return true;
	}
	if (KEY_EVENT != rec->EventType) {
		return 0;
	}
	k = &rec->Event.KeyEvent;
	c = k->uChar.UnicodeChar;
	if (k->bKeyDown && VK_RETURN == k->wVirtualKeyCode) {
		ULONGLONG now = GetTickCount64();

		if (L'\r' == c) {
			t->cr_tick = now ? now : 1;
		} else if (L'\n' == c && t->cr_tick &&
		        now - t->cr_tick <= CRLF_MS) {
			t->cr_tick = 0; /* the LF half of a pasted CRLF */
			return false;
		}
	}
	/* Console VT responses have no virtual key or scan code. Real keys
	 * retain their identity and never enter the response parser. */
	if (!k->wVirtualKeyCode && !k->wVirtualScanCode &&
	        (t->caps.len || t->caps.dropping || c == 27)) {
		if (k->bKeyDown) {
			wf_term_caps_feed(
			        &t->caps, (unsigned)c, GetTickCount64());
		}
		return false;
	}

	ev->vk = k->wVirtualKeyCode;

	if ((t->editing || t->modified) && k->bKeyDown &&
	        translate_editing(k, ev)) {
		t->pending_high =
		        0; /* it yields an event: an orphan is dropped */
		return 1;
	}
	if (!k->bKeyDown) {
		/* Alt+numpad delivers its character on the Alt key-up. */
		if (VK_MENU == k->wVirtualKeyCode && 0 != c) {
			goto as_char;
		}
		if (c >= 0x20 && 0x7F != c && !(c >= 0xD800 && c <= 0xDFFF)) {
			ev->type = WF_EV_RELEASE;
			ev->ch = c;
			return 1;
		}
		return 0;
	}
as_char:
	if (c >= 0xD800 && c <= 0xDBFF) {
		t->pending_high = c;
		return 0;
	}
	if (c >= 0xDC00 && c <= 0xDFFF) {
		uint32_t cp = WF_CP_REPLACEMENT;

		if (t->pending_high) {
			cp = 0x10000 +
			        (((uint32_t)t->pending_high - 0xD800) << 10) +
			        (uint32_t)(c - 0xDC00);
		}
		t->pending_high = 0;
		ev->type = WF_EV_CHAR;
		ev->ch = cp;
		return 1;
	}
	/*
	 * A high surrogate waits for its low half. ConPTY delivers a character
	 * outside the BMP as two Alt-key records (Alt down, character on Alt up,
	 * twice), so a record that yields nothing, such as a bare modifier,
	 * must leave it alone. A record that yields an event ends the wait: the
	 * orphan is dropped and the event is still delivered, so an Enter or an
	 * arrow key is never swallowed.
	 */
	if (c >= 0x20 && 0x7F != c) {
		t->pending_high = 0;
		ev->type = WF_EV_CHAR;
		ev->ch = c;
		return 1;
	}
	ev->key = map_vk(k->wVirtualKeyCode, k->dwControlKeyState);
	if (WF_KEY_NONE == ev->key) {
		return 0;
	}
	t->pending_high = 0;
	ev->type = WF_EV_KEY;
	return 1;
}

static bool is_text_key(const WfEvent *e)
{
	return WF_EV_CHAR == e->type ||
	        (WF_EV_KEY == e->type &&
	                (WF_KEY_ENTER == e->key || WF_KEY_TAB == e->key));
}

/* Which key it is, for "all the same": a character, or a named key. */
static uint32_t key_identity(const WfEvent *e)
{
	return WF_EV_CHAR == e->type ? e->ch : 0x200000u + (uint32_t)e->key;
}

/*
 * Marks the text keys of one read as pasted when they arrived the way a paste
 * does (see WF_VK_PASTED). `now` is the tick of the read.
 */
static void mark_paste(WfTerm *t, WfEvent *ev, size_t n, ULONGLONG now)
{
	size_t text = 0;
	bool differs = false, enter = false;
	uint32_t first = 0;

	for (size_t i = 0; i < n; i++) {
		if (!is_text_key(&ev[i])) {
			continue;
		}
		if (0 == text) {
			first = key_identity(&ev[i]);
		} else if (key_identity(&ev[i]) != first) {
			differs = true;
		}
		enter = enter ||
		        (WF_EV_KEY == ev[i].type && WF_KEY_ENTER == ev[i].key);
		text++;
	}
	if (!(differs && (text >= WF_PASTE_MIN_KEYS || (enter && text >= 2))) &&
	        now >= t->paste_until) {
		return; /* typing */
	}
	for (size_t i = 0; i < n; i++) {
		if (is_text_key(&ev[i])) {
			ev[i].vk |= WF_VK_PASTED;
			t->paste_until = now + WF_PASTE_TAIL_MS;
		}
	}
}

/* How many events a record stands for: a key that went down and was held
 * comes as one record with the number of times it repeated. */
static DWORD times_of(const INPUT_RECORD *rec, const WfEvent *e)
{
	if (KEY_EVENT == rec->EventType && rec->Event.KeyEvent.bKeyDown &&
	        WF_EV_RELEASE != e->type &&
	        rec->Event.KeyEvent.wRepeatCount > 1) {
		return rec->Event.KeyEvent.wRepeatCount;
	}
	return 1;
}

/* Turns the records (at most 64) into events after ev[count], up to `max` in all,
 * and returns the new count. What does not fit stays in `t` (the rest of a
 * repeat, the records not reached) and is the first thing the next call hands
 * back. `rec` may be t->pend. */
static size_t take_records(WfTerm *t,
        const INPUT_RECORD *rec,
        size_t n,
        WfEvent *ev,
        size_t count,
        size_t max)
{
	size_t i = 0;

	while (t->rep_left && count < max) {
		ev[count++] = t->rep_ev;
		t->rep_left--;
	}
	for (; i < n && !t->rep_left && count < max; i++) {
		WfEvent e = {WF_EV_CHAR, 0, WF_KEY_NONE, 0};

		if (!translate(t, &rec[i], &e)) {
			continue;
		}
		t->rep_ev = e;
		t->rep_left = times_of(&rec[i], &e);
		while (t->rep_left && count < max) {
			ev[count++] = e;
			t->rep_left--;
		}
	}
	t->npend = n - i;
	if (i < n) {
		memmove(t->pend, rec + i, (n - i) * sizeof *rec);
	}
	return count;
}

size_t wf_term_read_events(WfTerm *t, WfEvent *ev, size_t max)
{
	INPUT_RECORD rec[64];
	DWORD avail = 0, got = 0;
	size_t count = 0, first;

	while (t->nkept > 0 && count < max) {
		ev[count++] = t->kept[--t->nkept];
	}
	first = count;
	if (t->rep_left || t->npend) {
		count = take_records(t, t->pend, t->npend, ev, count, max);
	}

	/* The handle may be signalled with nothing readable; never block. */
	if (count >= max || t->rep_left || t->npend ||
	        !GetNumberOfConsoleInputEvents(t->in, &avail) || 0 == avail) {
		mark_paste(t, ev + first, count - first, GetTickCount64());
		return count;
	}
	/* read no more than we can hand back, or records would be lost */
	if (avail > 64) {
		avail = 64;
	}
	if (avail > max - count) {
		avail = (DWORD)(max - count);
	}
	if (!ReadConsoleInputW(t->in, rec, avail, &got)) {
		return count;
	}

	count = take_records(t, rec, got, ev, count, max);
	mark_paste(t, ev + first, count - first, GetTickCount64());
	return count;
}

void wf_term_drain_input(WfTerm *t)
{
	INPUT_RECORD rec[64];
	DWORD avail = 0, got = 0, i;

	t->rep_left = 0; /* what was held back is queued input too */
	t->npend = 0;
	while (GetNumberOfConsoleInputEvents(t->in, &avail) && avail > 0) {
		if (avail > 64) {
			avail = 64;
		}
		if (!ReadConsoleInputW(t->in, rec, avail, &got) || 0 == got) {
			return;
		}
		for (i = 0; i < got; i++) {
			WfEvent e = {WF_EV_CHAR, 0, WF_KEY_NONE, 0};

			if (translate(t, &rec[i], &e) &&
			        WF_EV_RELEASE == e.type &&
			        t->nkept < WF_ARRAY_LEN(t->kept)) {
				t->kept[t->nkept++] = e;
			}
		}
	}
}

// piero real-machine E2E UI helper.
//
// One command per run: no server, no listening port, no child process, no shell, no
// registry, no file reads. It can only look at, and send keys to, windows of a small
// allowlist of images, and write PNG screenshots under %TEMP%\piero-e2e-*\.
//
// Policy (usage and build: tools/e2e/README.md):
//  - Read-only commands (list, state, uia, shot) work on allowlisted images.
//  - Commands that change something (focus, key, hold, unicode, resize, close) also need
//    the marker "piero-e2e-" in the window title: only windows the test run itself
//    created and named can be driven.
//  - Keys are sent only while the target is the foreground window, checked before every
//    send. Window titles of non-allowlisted images are never printed.
//  - The process ends itself after a deadline, so a hung UI Automation provider cannot
//    leave an orphan.
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <dwmapi.h>
#include <UIAutomationClient.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum
{
	EXIT_OK = 0,
	EXIT_USAGE = 1,
	EXIT_REFUSED = 2, // policy refused the request; nothing was changed
	EXIT_STATE =
	        3, // the state needed for the request was not (or no longer) there
	EXIT_OS = 4,
	EXIT_DEADLINE = 124,
};

#define MARKER L"piero-e2e-"
#define MAX_KEY_TOKENS 64
#define MAX_HOLD_MS 5000
#define MAX_UNICODE_CHARS 256
#define MAX_UIA_NODES 400
#define MAX_UIA_DEPTH 8
#define MAX_TEXT_OUT 120

static const wchar_t *const k_allowed_images[] = {
        L"WindowsTerminal.exe", L"conhost.exe", L"piero.exe"};

typedef struct
{
	HWND h;
	DWORD pid;
	wchar_t exe[MAX_PATH];
	wchar_t cls[64];
	wchar_t title[256];
	RECT rect;
	bool allowed;
} Win;

static void out(const wchar_t *fmt, ...)
{
	wchar_t w[2048];
	va_list ap;
	va_start(ap, fmt);
	_vsnwprintf_s(w, sizeof w / sizeof w[0] - 2, _TRUNCATE, fmt, ap);
	va_end(ap);
	int n = (int)wcsnlen(w,
	        sizeof w / sizeof w[0] - 2); // a truncated line keeps what fits
	w[n++] = L'\n';
	char u[4 * 2048];
	int bytes = WideCharToMultiByte(
	        CP_UTF8, 0, w, n, u, (int)sizeof u, NULL, NULL);
	DWORD wrote;
	if (bytes > 0) {
		WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), u, (DWORD)bytes,
		        &wrote, NULL);
	}
}

// Control characters and over-long text are removed so one value stays on one line.
static void clean(wchar_t *s, size_t cap)
{
	size_t i = 0;
	if (!cap) {
		return;
	}
	size_t limit = cap - 1 < MAX_TEXT_OUT ? cap - 1 : MAX_TEXT_OUT;
	for (; i < limit && s[i]; i++) {
		if (s[i] < 0x20 || s[i] == 0x7f) {
			s[i] = L' ';
		}
	}
	s[i] = 0;
}

static DWORD WINAPI deadline_thread(void *arg)
{
	Sleep((DWORD)(uintptr_t)arg);
	out(L"status=deadline");
	ExitProcess(EXIT_DEADLINE);
}

static bool image_allowed(const wchar_t *exe)
{
	for (size_t i = 0;
	        i < sizeof k_allowed_images / sizeof k_allowed_images[0]; i++) {
		if (_wcsicmp(exe, k_allowed_images[i]) == 0) {
			return true;
		}
	}
	return false;
}

static bool win_info(HWND h, Win *w)
{
	memset(w, 0, sizeof *w);
	if (!IsWindow(h)) {
		return false;
	}
	w->h = h;
	GetWindowThreadProcessId(h, &w->pid);
	HANDLE p =
	        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, w->pid);
	if (p) {
		wchar_t full[MAX_PATH];
		DWORD n = MAX_PATH;
		if (QueryFullProcessImageNameW(p, 0, full, &n)) {
			const wchar_t *base = wcsrchr(full, L'\\');
			wcsncpy_s(w->exe, MAX_PATH, base ? base + 1 : full,
			        _TRUNCATE);
		}
		CloseHandle(p);
	}
	w->allowed = image_allowed(w->exe);
	GetClassNameW(h, w->cls, (int)(sizeof w->cls / sizeof w->cls[0]));
	GetWindowRect(h, &w->rect);
	// A hung owner must not hang the helper: bounded WM_GETTEXT.
	DWORD_PTR got = 0;
	if (w->allowed &&
	        SendMessageTimeoutW(h, WM_GETTEXT,
	                (WPARAM)(sizeof w->title / sizeof w->title[0]),
	                (LPARAM)w->title, SMTO_ABORTIFHUNG | SMTO_BLOCK, 300,
	                &got) == 0) {
		w->title[0] = 0;
	}
	w->title[(sizeof w->title / sizeof w->title[0]) - 1] = 0;
	clean(w->title, sizeof w->title / sizeof *w->title);
	return true;
}

static void print_win(const Win *w)
{
	out(L"hwnd=0x%llx pid=%lu exe=%ls class=%ls rect=%ld,%ld,%ld,%ld fg=%d "
	    L"title=%ls",
	        (unsigned long long)(uintptr_t)w->h, w->pid,
	        w->exe[0] ? w->exe : L"?", w->cls, w->rect.left, w->rect.top,
	        w->rect.right, w->rect.bottom, GetForegroundWindow() == w->h,
	        w->allowed ? w->title : L"<hidden>");
}

static bool parse_hwnd(const wchar_t *s, HWND *h)
{
	wchar_t *end;
	unsigned long long v = wcstoull(s, &end, 0);
	if (end == s || *end || v == 0) {
		return false;
	}
	*h = (HWND)(uintptr_t)v;
	return true;
}

// Resolves the target and applies the read policy (allowlist) or the write policy
// (allowlist and marker). Returns EXIT_OK or the exit code to use.
static int resolve(const wchar_t *arg, bool writes, Win *w)
{
	HWND h;
	if (!parse_hwnd(arg, &h)) {
		return EXIT_USAGE;
	}
	if (!win_info(h, w)) {
		out(L"status=no-such-window");
		return EXIT_STATE;
	}
	if (!w->allowed) {
		out(L"status=refused reason=image-not-allowed exe=%ls",
		        w->exe[0] ? w->exe : L"?");
		return EXIT_REFUSED;
	}
	if (writes && !wcsstr(w->title, MARKER)) {
		out(L"status=refused reason=no-marker");
		return EXIT_REFUSED;
	}
	return EXIT_OK;
}

typedef struct
{
	const wchar_t *filter;
	bool need_marker;
	int found;
	Win first;
} ListCtx;

static BOOL CALLBACK list_cb(HWND h, LPARAM lp)
{
	ListCtx *c = (ListCtx *)lp;
	if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) {
		return TRUE;
	}
	BOOL cloaked = FALSE;
	DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
	if (cloaked) {
		return TRUE;
	}
	Win w;
	if (!win_info(h, &w)) {
		return TRUE;
	}
	if (c->filter && !(w.allowed && wcsstr(w.title, c->filter))) {
		return TRUE;
	}
	if (c->need_marker && !wcsstr(w.title, MARKER)) {
		return TRUE;
	}
	if (c->found++ == 0) {
		c->first = w;
	}
	if (!c->need_marker) {
		print_win(&w);
	}
	return TRUE;
}

static int cmd_list(const wchar_t *filter)
{
	ListCtx c = {filter, false, 0, {0}};
	EnumWindows(list_cb, (LPARAM)&c);
	out(L"status=ok count=%d", c.found);
	return EXIT_OK;
}

static int cmd_wait(const wchar_t *substr, const wchar_t *ms_arg)
{
	if (!wcsstr(substr, MARKER)) {
		out(L"status=refused reason=filter-needs-marker");
		return EXIT_REFUSED;
	}
	unsigned long ms = wcstoul(ms_arg, NULL, 10);
	if (ms == 0 || ms > 60000) {
		return EXIT_USAGE;
	}
	ULONGLONG until = GetTickCount64() + ms;
	for (;;) {
		ListCtx c = {substr, true, 0, {0}};
		EnumWindows(list_cb, (LPARAM)&c);
		if (c.found > 0) {
			print_win(&c.first);
			out(L"status=ok count=%d", c.found);
			return EXIT_OK;
		}
		if (GetTickCount64() >= until) {
			break;
		}
		Sleep(100);
	}
	out(L"status=timeout");
	return EXIT_STATE;
}

static int cmd_fg(void)
{
	Win w;
	if (!win_info(GetForegroundWindow(), &w)) {
		out(L"status=no-foreground");
		return EXIT_STATE;
	}
	print_win(&w);
	out(L"status=ok");
	return EXIT_OK;
}

static int cmd_state(const wchar_t *hwnd_arg, const wchar_t *pid_arg)
{
	HWND h;
	if (!parse_hwnd(hwnd_arg, &h)) {
		return EXIT_USAGE;
	}
	Win w;
	bool exists = win_info(h, &w);
	out(L"exists=%d hung=%d iconic=%d visible=%d fg=%d", exists,
	        exists && IsHungAppWindow(h), exists && IsIconic(h),
	        exists && IsWindowVisible(h),
	        exists && GetForegroundWindow() == h);
	if (pid_arg) {
		DWORD pid = wcstoul(pid_arg, NULL, 10);
		HANDLE p = OpenProcess(
		        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		DWORD code = 0;
		bool alive = false;
		if (p) {
			alive = GetExitCodeProcess(p, &code) &&
			        code == STILL_ACTIVE;
			CloseHandle(p);
		}
		out(L"process_alive=%d exit_code=%lu", alive,
		        alive ? 0ul : code);
	}
	out(L"status=ok");
	return EXIT_OK;
}

static int cmd_focus(const wchar_t *hwnd_arg)
{
	Win w;
	int rc = resolve(hwnd_arg, true, &w);
	if (rc != EXIT_OK) {
		return rc;
	}
	if (IsIconic(w.h)) {
		ShowWindow(w.h, SW_RESTORE);
	}
	bool ok = SetForegroundWindow(w.h) && GetForegroundWindow() == w.h;
	if (!ok) {
		// The foreground lock refuses a process that is not the foreground one; share the
		// input queue of the current foreground thread for the one call, then detach.
		HWND fg = GetForegroundWindow();
		DWORD ft = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
		DWORD me = GetCurrentThreadId();
		bool attached =
		        ft && ft != me && AttachThreadInput(me, ft, TRUE);
		SetForegroundWindow(w.h);
		if (attached) {
			AttachThreadInput(me, ft, FALSE);
		}
		ok = GetForegroundWindow() == w.h;
	}
	out(L"status=%ls", ok ? L"ok" : L"focus-failed");
	return ok ? EXIT_OK : EXIT_STATE;
}

static int cmd_resize(const wchar_t *hwnd_arg,
        const wchar_t *ww,
        const wchar_t *hh,
        const wchar_t *xx,
        const wchar_t *yy)
{
	Win w;
	int rc = resolve(hwnd_arg, true, &w);
	if (rc != EXIT_OK) {
		return rc;
	}
	long cw = wcstol(ww, NULL, 10), ch = wcstol(hh, NULL, 10);
	if (cw < 200 || cw > 8000 || ch < 150 || ch > 8000) {
		return EXIT_USAGE;
	}
	UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
	long x = 0, y = 0;
	if (xx && yy) {
		x = wcstol(xx, NULL, 10);
		y = wcstol(yy, NULL, 10);
	} else {
		flags |= SWP_NOMOVE;
	}
	if (IsIconic(w.h) || IsZoomed(w.h)) {
		ShowWindow(w.h, SW_RESTORE);
	}
	if (!SetWindowPos(w.h, NULL, (int)x, (int)y, (int)cw, (int)ch, flags)) {
		out(L"status=os-error code=%lu", GetLastError());
		return EXIT_OS;
	}
	win_info(w.h, &w);
	print_win(&w);
	out(L"status=ok");
	return EXIT_OK;
}

static int cmd_close(const wchar_t *hwnd_arg)
{
	Win w;
	int rc = resolve(hwnd_arg, true, &w);
	if (rc != EXIT_OK) {
		return rc;
	}
	if (!PostMessageW(w.h, WM_CLOSE, 0, 0)) {
		out(L"status=os-error code=%lu", GetLastError());
		return EXIT_OS;
	}
	out(L"status=ok");
	return EXIT_OK;
}

enum
{
	KM_CTRL = 1,
	KM_SHIFT = 2,
	KM_ALT = 4
};

typedef struct
{
	unsigned mods;
	WORD vk;
	bool ext;
} Key;

static const struct
{
	const wchar_t *name;
	WORD vk;
	bool ext;
} k_named[] = {
        {L"enter", VK_RETURN, false},
        {L"esc", VK_ESCAPE, false},
        {L"tab", VK_TAB, false},
        {L"bs", VK_BACK, false},
        {L"space", VK_SPACE, false},
        {L"del", VK_DELETE, true},
        {L"ins", VK_INSERT, true},
        {L"up", VK_UP, true},
        {L"down", VK_DOWN, true},
        {L"left", VK_LEFT, true},
        {L"right", VK_RIGHT, true},
        {L"home", VK_HOME, true},
        {L"end", VK_END, true},
        {L"pgup", VK_PRIOR, true},
        {L"pgdn", VK_NEXT, true},
        {L"plus", VK_OEM_PLUS, false},
        {L"f1", VK_F1, false},
        {L"f2", VK_F2, false},
        {L"f3", VK_F3, false},
        {L"f4", VK_F4, false},
        {L"f5", VK_F5, false},
        {L"f6", VK_F6, false},
        {L"f7", VK_F7, false},
        {L"f8", VK_F8, false},
        {L"f9", VK_F9, false},
        {L"f10", VK_F10, false},
        {L"f11", VK_F11, false},
        {L"f12", VK_F12, false},
};

// One base key: a name from the table, or one character mapped by the current layout.
static bool parse_base(const wchar_t *s, Key *k)
{
	for (size_t i = 0; i < sizeof k_named / sizeof k_named[0]; i++) {
		if (_wcsicmp(s, k_named[i].name) == 0) {
			k->vk = k_named[i].vk;
			k->ext = k_named[i].ext;
			return true;
		}
	}
	if (wcslen(s) != 1) {
		return false;
	}
	SHORT r = VkKeyScanW(s[0]);
	if (r == -1) {
		return false;
	}
	k->vk = (WORD)(r & 0xff);
	if (r & 0x100) {
		k->mods |= KM_SHIFT;
	}
	if (r & 0x200) {
		k->mods |= KM_CTRL;
	}
	if (r & 0x400) {
		k->mods |= KM_ALT;
	}
	return true;
}

// "ctrl+shift+a", "enter", "F5", "x". The Windows keys are not in the table, so they
// cannot be sent.
static bool parse_chord(const wchar_t *tok, Key *k)
{
	memset(k, 0, sizeof *k);
	wchar_t buf[64];
	if (wcslen(tok) >= sizeof buf / sizeof buf[0]) {
		return false;
	}
	wcscpy_s(buf, sizeof buf / sizeof buf[0], tok);
	wchar_t *p = buf;
	for (;;) {
		wchar_t *plus = wcschr(p, L'+');
		if (!plus || plus == p || !plus[1]) {
			break; // the last piece (or a lone "+") is the base
		}
		*plus = 0;
		if (_wcsicmp(p, L"ctrl") == 0) {
			k->mods |= KM_CTRL;
		} else if (_wcsicmp(p, L"shift") == 0) {
			k->mods |= KM_SHIFT;
		} else if (_wcsicmp(p, L"alt") == 0) {
			k->mods |= KM_ALT;
		} else {
			return false;
		}
		p = plus + 1;
	}
	return parse_base(p, k);
}

static INPUT key_event(WORD vk, bool ext, bool up)
{
	INPUT in;
	memset(&in, 0, sizeof in);
	in.type = INPUT_KEYBOARD;
	in.ki.wVk = vk;
	in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
	in.ki.dwFlags =
	        (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
	return in;
}

static size_t mod_events(unsigned mods, bool up, INPUT *dst)
{
	size_t n = 0;
	if (mods & KM_CTRL) {
		dst[n++] = key_event(VK_CONTROL, false, up);
	}
	if (mods & KM_SHIFT) {
		dst[n++] = key_event(VK_SHIFT, false, up);
	}
	if (mods & KM_ALT) {
		dst[n++] = key_event(VK_MENU, false, up);
	}
	return n;
}

// The target must still be the foreground window, still carry the marker and not be hung.
static bool still_target(HWND h)
{
	if (GetForegroundWindow() != h || IsHungAppWindow(h)) {
		return false;
	}
	Win w;
	return win_info(h, &w) && w.allowed && wcsstr(w.title, MARKER);
}

static bool send_inputs(INPUT *in, size_t n)
{
	return SendInput((UINT)n, in, sizeof(INPUT)) == n;
}

static int cmd_key(const wchar_t *hwnd_arg, int ntok, wchar_t **toks)
{
	if (ntok < 1 || ntok > MAX_KEY_TOKENS) {
		return EXIT_USAGE;
	}
	Key keys[MAX_KEY_TOKENS];
	for (int i = 0; i < ntok; i++) {
		if (!parse_chord(toks[i], &keys[i])) {
			out(L"status=refused reason=unknown-key token=%ls",
			        toks[i]);
			return EXIT_REFUSED;
		}
	}
	Win w;
	int rc = resolve(hwnd_arg, true, &w);
	if (rc != EXIT_OK) {
		return rc;
	}
	int sent = 0;
	for (; sent < ntok; sent++) {
		if (!still_target(w.h)) {
			out(L"status=not-foreground sent=%d", sent);
			return EXIT_STATE;
		}
		INPUT in[8];
		size_t n = mod_events(keys[sent].mods, false, in);
		in[n++] = key_event(keys[sent].vk, keys[sent].ext, false);
		in[n++] = key_event(keys[sent].vk, keys[sent].ext, true);
		n += mod_events(keys[sent].mods, true, in + n);
		if (!send_inputs(in, n)) {
			out(L"status=os-error code=%lu sent=%d", GetLastError(),
			        sent);
			return EXIT_OS;
		}
		Sleep(15);
	}
	out(L"status=ok sent=%d", sent);
	return EXIT_OK;
}

// Auto-repeat as the application sees it: repeated key-downs without a key-up.
static int cmd_hold(const wchar_t *hwnd_arg,
        const wchar_t *tok,
        const wchar_t *ms_arg,
        const wchar_t *iv_arg)
{
	unsigned long ms = wcstoul(ms_arg, NULL, 10),
	              iv = wcstoul(iv_arg, NULL, 10);
	if (ms == 0 || ms > MAX_HOLD_MS || iv < 10 || iv > 1000) {
		return EXIT_USAGE;
	}
	Key k;
	if (!parse_chord(tok, &k)) {
		out(L"status=refused reason=unknown-key token=%ls", tok);
		return EXIT_REFUSED;
	}
	Win w;
	int rc = resolve(hwnd_arg, true, &w);
	if (rc != EXIT_OK) {
		return rc;
	}
	INPUT in[8];
	size_t n = mod_events(k.mods, false, in);
	INPUT base_down = key_event(k.vk, k.ext, false);
	int downs = 0;
	bool aborted = false;
	ULONGLONG until = GetTickCount64() + ms;
	if (!still_target(w.h)) {
		out(L"status=not-foreground downs=0");
		return EXIT_STATE;
	}
	send_inputs(in, n); // modifiers down once
	while (GetTickCount64() < until) {
		if (!still_target(w.h)) {
			aborted = true;
			break;
		}
		if (send_inputs(&base_down, 1)) {
			downs++;
		}
		Sleep(iv);
	}
	// The key and the modifiers are always released, also after an abort.
	INPUT up[8];
	size_t m = 0;
	up[m++] = key_event(k.vk, k.ext, true);
	m += mod_events(k.mods, true, up + m);
	send_inputs(up, m);
	out(L"status=%ls downs=%d", aborted ? L"not-foreground" : L"ok", downs);
	return aborted ? EXIT_STATE : EXIT_OK;
}

// Direct Unicode injection. It does NOT go through an IME: no composition, no
// conversion, no candidate window. It must never be counted as IME verification.
static int cmd_unicode(const wchar_t *hwnd_arg, const wchar_t *text)
{
	size_t len = wcslen(text);
	if (len == 0 || len > MAX_UNICODE_CHARS) {
		return EXIT_USAGE;
	}
	Win w;
	int rc = resolve(hwnd_arg, true, &w);
	if (rc != EXIT_OK) {
		return rc;
	}
	size_t i = 0;
	for (; i < len; i++) {
		if (!still_target(w.h)) {
			out(L"status=not-foreground sent=%zu ime=bypassed", i);
			return EXIT_STATE;
		}
		INPUT in[2];
		memset(in, 0, sizeof in);
		in[0].type = in[1].type = INPUT_KEYBOARD;
		in[0].ki.wScan = in[1].ki.wScan = text[i];
		in[0].ki.dwFlags = KEYEVENTF_UNICODE;
		in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
		if (!send_inputs(in, 2)) {
			out(L"status=os-error code=%lu sent=%zu ime=bypassed",
			        GetLastError(), i);
			return EXIT_OS;
		}
		Sleep(10);
	}
	out(L"status=ok sent=%zu ime=bypassed", i);
	return EXIT_OK;
}

// Uncompressed PNG (stored deflate blocks): no codec library is loaded, and the file is
// written through the handle that was opened with CREATE_NEW.
static void be32(uint8_t *d, uint32_t v)
{
	d[0] = (uint8_t)(v >> 24);
	d[1] = (uint8_t)(v >> 16);
	d[2] = (uint8_t)(v >> 8);
	d[3] = (uint8_t)v;
}

static uint32_t crc32_bytes(uint32_t crc, const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
		}
	}
	return crc;
}

static bool write_all(HANDLE f, const void *p, size_t n)
{
	DWORD got = 0;
	return n <= 0x7fffffffu && WriteFile(f, p, (DWORD)n, &got, NULL) &&
	        got == n;
}

static bool png_chunk(
        HANDLE f, const char *type, const uint8_t *data, size_t len)
{
	uint8_t head[8], tail[4];
	be32(head, (uint32_t)len);
	memcpy(head + 4, type, 4);
	uint32_t crc = crc32_bytes(0xffffffffu, head + 4, 4);
	crc = crc32_bytes(crc, data, len) ^ 0xffffffffu;
	be32(tail, crc);
	if (!write_all(f, head, 8)) {
		return false;
	}
	if (len && !write_all(f, data, len)) {
		return false;
	}
	return write_all(f, tail, 4);
}

// bits: top-down 32-bit BGRX rows, cx*cy pixels. The caller bounds cx*cy.
static bool write_png(HANDLE f, const uint32_t *bits, int cx, int cy)
{
	size_t row = 1 + (size_t)cx * 3, raw_len = row * (size_t)cy;
	size_t nblk = (raw_len + 65534) / 65535,
	       z_len = 2 + raw_len + 5 * nblk + 4;
	uint8_t *raw = malloc(raw_len), *z = malloc(z_len);
	bool ok = false;
	if (!raw || !z) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		goto done;
	}
	uint32_t a = 1, b = 0;
	uint8_t *d = raw;
	for (int y = 0; y < cy; y++) {
		*d++ = 0; // filter: none
		for (int x = 0; x < cx; x++) {
			uint32_t px = bits[(size_t)y * (size_t)cx + (size_t)x];
			*d++ = (uint8_t)(px >> 16);
			*d++ = (uint8_t)(px >> 8);
			*d++ = (uint8_t)px;
		}
	}
	for (size_t i = 0; i < raw_len; i++) {
		a = (a + raw[i]) % 65521u;
		b = (b + a) % 65521u;
	}
	size_t pos = 0;
	z[pos++] = 0x78;
	z[pos++] = 0x01;
	for (size_t off = 0; off < raw_len;) {
		size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
		z[pos++] = off + n == raw_len;
		z[pos++] = (uint8_t)n;
		z[pos++] = (uint8_t)(n >> 8);
		z[pos++] = (uint8_t)~n;
		z[pos++] = (uint8_t)(~n >> 8);
		memcpy(z + pos, raw + off, n);
		pos += n;
		off += n;
	}
	be32(z + pos, (b << 16) | a);
	pos += 4;
	static const uint8_t sig[8] = {
	        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	uint8_t ihdr[13] = {0};
	be32(ihdr, (uint32_t)cx);
	be32(ihdr + 4, (uint32_t)cy);
	ihdr[8] = 8; // bit depth
	ihdr[9] = 2; // truecolour
	if (!write_all(f, sig, sizeof sig)) {
		goto done;
	}
	if (!png_chunk(f, "IHDR", ihdr, sizeof ihdr)) {
		goto done;
	}
	if (!png_chunk(f, "IDAT", z, pos)) {
		goto done;
	}
	ok = png_chunk(f, "IEND", NULL, 0);
done:
	free(raw);
	free(z);
	return ok;
}

static bool lower_prefix_eq(const wchar_t *a, const wchar_t *b, size_t n)
{
	return _wcsnicmp(a, b, n) == 0;
}

// Only %TEMP%\piero-e2e-*\<name>.png, one level, the directory not a reparse point.
static bool output_path_ok(const wchar_t *arg, wchar_t *full, size_t cap)
{
	wchar_t tmp[MAX_PATH], tmpl[MAX_PATH], abs_[MAX_PATH];
	if (!GetTempPathW(MAX_PATH, tmp) ||
	        !GetLongPathNameW(tmp, tmpl, MAX_PATH)) {
		return false;
	}
	if (!GetFullPathNameW(arg, MAX_PATH, abs_, NULL)) {
		return false;
	}
	// The file does not exist yet, so the directory part is made canonical.
	wchar_t *slash = wcsrchr(abs_, L'\\');
	if (!slash) {
		return false;
	}
	wchar_t dir[MAX_PATH], dirl[MAX_PATH];
	size_t dlen = (size_t)(slash - abs_);
	if (dlen == 0 || dlen >= MAX_PATH) {
		return false;
	}
	wcsncpy_s(dir, MAX_PATH, abs_, dlen);
	dir[dlen] = 0;
	if (!GetLongPathNameW(dir, dirl, MAX_PATH)) {
		return false;
	}
	size_t tl = wcslen(tmpl);
	if (tl && tmpl[tl - 1] == L'\\') {
		tmpl[--tl] = 0;
	}
	size_t pre = wcslen(MARKER);
	if (!lower_prefix_eq(dirl, tmpl, tl) || dirl[tl] != L'\\' ||
	        !lower_prefix_eq(dirl + tl + 1, MARKER, pre) ||
	        wcschr(dirl + tl + 1, L'\\')) {
		return false;
	}
	DWORD attr = GetFileAttributesW(dirl);
	if (attr == INVALID_FILE_ATTRIBUTES ||
	        !(attr & FILE_ATTRIBUTE_DIRECTORY) ||
	        (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
		return false;
	}
	const wchar_t *name = slash + 1;
	size_t nl = wcslen(name);
	if (nl < 5 || _wcsicmp(name + nl - 4, L".png") != 0) {
		return false;
	}
	if (swprintf(full, cap, L"%ls\\%ls", dirl, name) < 0) {
		return false;
	}
	return true;
}

static bool rect_visible(HWND h, const RECT *r)
{
	LONG mx = (r->right - r->left) / 4 < 20 ? (r->right - r->left) / 4 : 20;
	LONG my = (r->bottom - r->top) / 4 < 20 ? (r->bottom - r->top) / 4 : 20;
	LONG xs[3] = {
	        r->left + mx, (r->left + r->right) / 2, r->right - mx - 1};
	LONG ys[3] = {
	        r->top + my, (r->top + r->bottom) / 2, r->bottom - my - 1};
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			POINT p = {xs[i], ys[j]};
			HWND top = WindowFromPoint(p);
			if (!top || GetAncestor(top, GA_ROOT) != h) {
				return false;
			}
		}
	}
	return true;
}

static int cmd_shot(
        const wchar_t *hwnd_arg, const wchar_t *file_arg, const wchar_t *mode)
{
	bool use_print = !mode || _wcsicmp(mode, L"print") == 0;
	if (!use_print && _wcsicmp(mode, L"rect") != 0) {
		return EXIT_USAGE;
	}
	Win w;
	int rc = resolve(hwnd_arg, false, &w);
	if (rc != EXIT_OK) {
		return rc;
	}
	wchar_t path[MAX_PATH];
	if (!output_path_ok(file_arg, path, MAX_PATH)) {
		out(L"status=refused reason=output-path");
		return EXIT_REFUSED;
	}
	RECT r = w.rect;
	if (!use_print) {
		DwmGetWindowAttribute(
		        w.h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r);
	}
	int cx = r.right - r.left, cy = r.bottom - r.top;
	if (IsIconic(w.h) || cx <= 0 || cy <= 0 ||
	        (size_t)cx * (size_t)cy > 16000000u) {
		out(L"status=no-image reason=iconic-or-empty");
		return EXIT_STATE;
	}
	// A screen copy shows whatever is on top. It is taken only when this window is the
	// topmost one at nine points of its rectangle; otherwise nothing is captured.
	if (!use_print && !rect_visible(w.h, &r)) {
		out(L"status=occluded");
		return EXIT_STATE;
	}
	HDC scr = GetDC(NULL);
	HDC mem = CreateCompatibleDC(scr);
	BITMAPINFO bi;
	memset(&bi, 0, sizeof bi);
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = cx;
	bi.bmiHeader.biHeight = -cy;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	uint32_t *bits = NULL;
	HBITMAP bm = CreateDIBSection(
	        scr, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
	int ret = EXIT_OS;
	HANDLE f = INVALID_HANDLE_VALUE;
	if (!bm) {
		goto cleanup;
	}
	HGDIOBJ old = SelectObject(mem, bm);
	bool ok = use_print ? PrintWindow(w.h, mem, PW_RENDERFULLCONTENT)
	                    : BitBlt(mem, 0, 0, cx, cy, scr, r.left, r.top,
	                              SRCCOPY | CAPTUREBLT);
	SelectObject(mem, old);
	if (!ok) {
		out(L"status=os-error code=%lu", GetLastError());
		goto cleanup;
	}
	// Sampled count of non-black pixels: a black PrintWindow result is a failed capture,
	// not a picture of a black window.
	size_t nonblack = 0, total = (size_t)cx * (size_t)cy;
	for (size_t i = 0; i < total; i += 7) {
		if (bits[i] & 0x00ffffff) {
			nonblack++;
		}
	}
	// CREATE_NEW: a screenshot never replaces a file. A failed write removes the file this
	// run created, so no partial image is left behind.
	f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		out(L"status=os-error code=%lu reason=create-new",
		        GetLastError());
		goto cleanup;
	}
	if (!write_png(f, bits, cx, cy)) {
		out(L"status=os-error reason=png-write code=%lu",
		        GetLastError());
		CloseHandle(f);
		f = INVALID_HANDLE_VALUE;
		DeleteFileW(path);
		goto cleanup;
	}
	out(L"status=ok width=%d height=%d source=%ls nonblack_samples=%zu "
	    L"file=%ls",
	        cx, cy,
	        use_print ? L"print-window"
	                  : L"screen-rect(may-include-overlapping-windows)",
	        nonblack, path);
	ret = EXIT_OK;
cleanup:
	if (f != INVALID_HANDLE_VALUE) {
		CloseHandle(f);
	}
	if (bm) {
		DeleteObject(bm);
	}
	DeleteDC(mem);
	ReleaseDC(NULL, scr);
	return ret;
}

typedef struct
{
	IUIAutomationTreeWalker *walker;
	int max_depth;
	int nodes;
} UiaCtx;

static void bstr_out(BSTR b, wchar_t *dst, size_t cap)
{
	dst[0] = 0;
	if (b) {
		wcsncpy_s(dst, cap, b, _TRUNCATE);
		clean(dst, cap);
	}
}

// Reads Name, control type, AutomationId, class and rectangle only: never a Value or
// Text pattern, so no document or terminal content is read.
static void uia_walk(UiaCtx *c, IUIAutomationElement *e, int depth)
{
	if (c->nodes >= MAX_UIA_NODES) {
		return;
	}
	c->nodes++;
	BSTR name = NULL, aid = NULL, cls = NULL;
	CONTROLTYPEID ct = 0;
	RECT r = {0};
	IUIAutomationElement_get_CurrentName(e, &name);
	IUIAutomationElement_get_CurrentAutomationId(e, &aid);
	IUIAutomationElement_get_CurrentClassName(e, &cls);
	IUIAutomationElement_get_CurrentControlType(e, &ct);
	IUIAutomationElement_get_CurrentBoundingRectangle(e, &r);
	wchar_t n[MAX_TEXT_OUT + 1], a[MAX_TEXT_OUT + 1], k[MAX_TEXT_OUT + 1];
	bstr_out(name, n, sizeof n / sizeof n[0]);
	bstr_out(aid, a, sizeof a / sizeof a[0]);
	bstr_out(cls, k, sizeof k / sizeof k[0]);
	SysFreeString(name);
	SysFreeString(aid);
	SysFreeString(cls);
	out(L"depth=%d type=%d name=%ls autoid=%ls class=%ls "
	    L"rect=%ld,%ld,%ld,%ld",
	        depth, (int)ct, n, a, k, r.left, r.top, r.right, r.bottom);
	if (depth >= c->max_depth) {
		return;
	}
	IUIAutomationElement *child = NULL;
	IUIAutomationTreeWalker_GetFirstChildElement(c->walker, e, &child);
	while (child && c->nodes < MAX_UIA_NODES) {
		uia_walk(c, child, depth + 1);
		IUIAutomationElement *next = NULL;
		IUIAutomationTreeWalker_GetNextSiblingElement(
		        c->walker, child, &next);
		IUIAutomationElement_Release(child);
		child = next;
	}
	if (child) {
		IUIAutomationElement_Release(child);
	}
}

static int cmd_uia(const wchar_t *hwnd_arg, const wchar_t *depth_arg)
{
	Win w;
	int rc = resolve(hwnd_arg, false, &w);
	if (rc != EXIT_OK) {
		return rc;
	}
	int depth = depth_arg ? (int)wcstol(depth_arg, NULL, 10) : 3;
	if (depth < 0 || depth > MAX_UIA_DEPTH) {
		return EXIT_USAGE;
	}
	if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) {
		return EXIT_OS;
	}
	IUIAutomation *ua = NULL;
	IUIAutomationElement *root = NULL;
	UiaCtx c = {NULL, depth, 0};
	int ret = EXIT_OS;
	HRESULT hr = CoCreateInstance(&CLSID_CUIAutomation, NULL,
	        CLSCTX_INPROC_SERVER, &IID_IUIAutomation, (void **)&ua);
	if (FAILED(hr)) {
		out(L"status=os-error hresult=0x%08lx", (unsigned long)hr);
		goto cleanup;
	}
	hr = IUIAutomation_ElementFromHandle(ua, (UIA_HWND)w.h, &root);
	if (FAILED(hr) || !root) {
		out(L"status=os-error hresult=0x%08lx", (unsigned long)hr);
		goto cleanup;
	}
	hr = IUIAutomation_get_ControlViewWalker(ua, &c.walker);
	if (FAILED(hr)) {
		out(L"status=os-error hresult=0x%08lx", (unsigned long)hr);
		goto cleanup;
	}
	uia_walk(&c, root, 0);
	out(L"status=ok nodes=%d%ls", c.nodes,
	        c.nodes >= MAX_UIA_NODES ? L" truncated=1" : L"");
	ret = EXIT_OK;
cleanup:
	if (c.walker) {
		IUIAutomationTreeWalker_Release(c.walker);
	}
	if (root) {
		IUIAutomationElement_Release(root);
	}
	if (ua) {
		IUIAutomation_Release(ua);
	}
	CoUninitialize();
	return ret;
}

static int usage(void)
{
	out(L"usage: piero_e2e_ui <command> ...\n"
	    L"  list [title-substring]            allowlisted windows (titles "
	    L"of others hidden)\n"
	    L"  fg                                foreground window\n"
	    L"  state <hwnd> [pid]                exists/hung/iconic/visible, "
	    L"process alive\n"
	    L"  wait <piero-e2e-substr> <ms>      wait for a marked window\n"
	    L"  uia <hwnd> [depth<=8]             UI Automation tree (names "
	    L"only)\n"
	    L"  shot <hwnd> <file.png> [print|rect]  file: "
	    L"%%TEMP%%\\piero-e2e-*\\x.png\n"
	    L"  focus <hwnd>                      marked window only\n"
	    L"  key <hwnd> <chord>...             e.g. ctrl+shift+p enter F5 "
	    L"(max 64)\n"
	    L"  hold <hwnd> <chord> <ms<=5000> <interval-ms>   auto-repeat\n"
	    L"  unicode <hwnd> <text>             direct Unicode, NOT IME "
	    L"composition\n"
	    L"  resize <hwnd> <w> <h> [x y]       marked window only\n"
	    L"  close <hwnd>                      WM_CLOSE, marked window "
	    L"only");
	return EXIT_USAGE;
}

int wmain(int argc, wchar_t **argv)
{
	SetProcessDpiAwarenessContext(
	        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	if (argc < 2) {
		return usage();
	}
	const wchar_t *c = argv[1];
	unsigned long deadline = 30000;
	if (_wcsicmp(c, L"wait") == 0 && argc >= 4) {
		deadline = wcstoul(argv[3], NULL, 10) + 5000;
	}
	HANDLE t = CreateThread(
	        NULL, 0, deadline_thread, (void *)(uintptr_t)deadline, 0, NULL);
	if (t) {
		CloseHandle(t);
	}
	int rc = EXIT_USAGE;
	if (_wcsicmp(c, L"list") == 0 && argc <= 3) {
		rc = cmd_list(argc == 3 ? argv[2] : NULL);
	} else if (_wcsicmp(c, L"fg") == 0 && argc == 2) {
		rc = cmd_fg();
	} else if (_wcsicmp(c, L"state") == 0 && (argc == 3 || argc == 4)) {
		rc = cmd_state(argv[2], argc == 4 ? argv[3] : NULL);
	} else if (_wcsicmp(c, L"wait") == 0 && argc == 4) {
		rc = cmd_wait(argv[2], argv[3]);
	} else if (_wcsicmp(c, L"uia") == 0 && (argc == 3 || argc == 4)) {
		rc = cmd_uia(argv[2], argc == 4 ? argv[3] : NULL);
	} else if (_wcsicmp(c, L"shot") == 0 && (argc == 4 || argc == 5)) {
		rc = cmd_shot(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
	} else if (_wcsicmp(c, L"focus") == 0 && argc == 3) {
		rc = cmd_focus(argv[2]);
	} else if (_wcsicmp(c, L"key") == 0 && argc >= 4) {
		rc = cmd_key(argv[2], argc - 3, argv + 3);
	} else if (_wcsicmp(c, L"hold") == 0 && argc == 6) {
		rc = cmd_hold(argv[2], argv[3], argv[4], argv[5]);
	} else if (_wcsicmp(c, L"unicode") == 0 && argc == 4) {
		rc = cmd_unicode(argv[2], argv[3]);
	} else if (_wcsicmp(c, L"resize") == 0 && (argc == 5 || argc == 7)) {
		rc = cmd_resize(argv[2], argv[3], argv[4],
		        argc == 7 ? argv[5] : NULL, argc == 7 ? argv[6] : NULL);
	} else if (_wcsicmp(c, L"close") == 0 && argc == 3) {
		rc = cmd_close(argv[2]);
	}
	if (rc == EXIT_USAGE) {
		usage();
	}
	return rc;
}

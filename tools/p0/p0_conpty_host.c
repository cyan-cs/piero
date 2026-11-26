/*
 * P0-TERMINAL automation through a Windows pseudo console (ConPTY).
 *
 * IMPORTANT SCOPE NOTE. This host is NOT Windows Terminal and it is not a
 * conhost window. It drives p0_terminal.exe through CreatePseudoConsole, the
 * same API layer Windows Terminal uses underneath, but the terminal side
 * (key encoding, repeat, IME, rendering) is this program. Evidence produced
 * here says how wfile's console layer behaves behind ConPTY; it says nothing
 * about Windows Terminal itself, which stays NOT TESTED until it is operated
 * by hand (docs/p0-manual-terminal-smoke.md).
 *
 * Scenarios (--scenario):
 *   input    P0-T01  raw console records for typed keys, Esc, UTF-8, resize
 *   confirm  P0-T05  type-ahead / guard / Enter / Esc / IME against the modal
 *   resize   P0-T06  modal visibility at several terminal sizes
 *   close    P0-T08  CTRL_CLOSE_EVENT (pseudo console closed) while idle
 *   ctrlc    P0-T01  Ctrl+C policy
 *   shutdown-go     product Ctrl+Break cleanup with Smart Go open
 */
#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>
#include <share.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "input/wf_keymap.h"
#include "render/wf_theme.h"
#include "syntax/wf_syntax_abi.h"
#include "p0_common.h"
#include "p0_lab.h"
#include "../../tests/pdf_samples.h"
#include "../../tests/wf_test_assoc.h"
#include "text/wf_text.h"
#include "base/wf_macro.h"

#define VERSION "1.0"

static LARGE_INTEGER g_freq;
static wchar_t g_clipboard_desktop[64];

static double now_ms(void)
{
	LARGE_INTEGER t;

	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)g_freq.QuadPart;
}

typedef struct
{
	int cols, rows;
	uint32_t *cell;
	int cx, cy;
	int cursor_visible; /* DECTCEM as the terminal last heard it */
	int state; /* 0 ground, 1 esc, 2 csi, 3 osc, 4 esc-intermediate */
	char csi[64];
	int ncsi;
	uint8_t u8[4];
	int u8n, u8need;
} Screen;

static void screen_resize(Screen *s, int cols, int rows)
{
	free(s->cell);
	s->cols = cols;
	s->rows = rows;
	s->cell = calloc((size_t)cols * (size_t)rows, sizeof *s->cell);
	s->cx = s->cy = 0;
}

static void clamp(Screen *s)
{
	if (s->cx < 0) {
		s->cx = 0;
	}
	if (s->cx >= s->cols) {
		s->cx = s->cols - 1;
	}
	if (s->cy < 0) {
		s->cy = 0;
	}
	if (s->cy >= s->rows) {
		s->cy = s->rows - 1;
	}
}

static void put_cp(Screen *s, uint32_t cp)
{
	int w = wf_cp_width(cp);

	if (0 == w) {
		return;
	}
	if (s->cx + w > s->cols) { /* autowrap */
		s->cx = 0;
		if (s->cy + 1 < s->rows) {
			s->cy++;
		}
	}
	s->cell[s->cy * s->cols + s->cx] = cp;
	if (2 == w && s->cx + 1 < s->cols) {
		s->cell[s->cy * s->cols + s->cx + 1] =
		        0xFFFFFFFFu; /* continuation */
	}
	s->cx += w;
}

static int csi_param(const char *p, int idx, int dflt)
{
	int i = 0, v = 0, have = 0;

	if ('?' == *p || '>' == *p) {
		p++;
	}
	for (;; p++) {
		if (*p >= '0' && *p <= '9') {
			v = v * 10 + (*p - '0');
			have = 1;
		} else {
			if (i == idx) {
				return have ? v : dflt;
			}
			if (';' != *p) {
				return dflt;
			}
			i++;
			v = have = 0;
		}
	}
}

static void erase(Screen *s, int from_row, int from_col, int to_row, int to_col)
{
	int y, x;

	for (y = from_row; y <= to_row; y++) {
		for (x = (y == from_row ? from_col : 0);
		        x <= (y == to_row ? to_col : s->cols - 1); x++) {
			s->cell[y * s->cols + x] = 0;
		}
	}
}

static void csi_final(Screen *s, char f)
{
	const char *p = s->csi;
	int n1 = csi_param(p, 0, 1), n0 = csi_param(p, 0, 0);

	switch (f) {
	case 'H':
	case 'f':
		s->cy = csi_param(p, 0, 1) - 1;
		s->cx = csi_param(p, 1, 1) - 1;
		break;
	case 'h':
	case 'l':
		if ('?' == p[0] && 25 == csi_param(p, 0, 0)) {
			s->cursor_visible = 'h' == f;
		}
		break;
	case 'A': s->cy -= n1; break;
	case 'B': s->cy += n1; break;
	case 'C': s->cx += n1; break;
	case 'D': s->cx -= n1; break;
	case 'G': s->cx = n1 - 1; break;
	case 'd': s->cy = n1 - 1; break;
	case 'J':
		clamp(s);
		if (2 == n0 || 3 == n0) {
			erase(s, 0, 0, s->rows - 1, s->cols - 1);
		} else if (0 == n0) {
			erase(s, s->cy, s->cx, s->rows - 1, s->cols - 1);
		} else {
			erase(s, 0, 0, s->cy, s->cx);
		}
		break;
	case 'K':
		clamp(s);
		if (0 == n0) {
			erase(s, s->cy, s->cx, s->cy, s->cols - 1);
		} else if (1 == n0) {
			erase(s, s->cy, 0, s->cy, s->cx);
		} else {
			erase(s, s->cy, 0, s->cy, s->cols - 1);
		}
		break;
	case 'X':
		clamp(s);
		erase(s, s->cy, s->cx, s->cy,
		        s->cx + n1 - 1 < s->cols ? s->cx + n1 - 1
		                                 : s->cols - 1);
		break;
	default:
		break; /* SGR, modes, scrolling regions: irrelevant to the text
		        */
	}
	clamp(s);
}

static void screen_feed(Screen *s, const uint8_t *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		uint8_t c = b[i];

		switch (s->state) {
		case 1:
			if ('[' == c) {
				s->state = 2;
				s->ncsi = 0;
				s->csi[0] = 0;
			} else if (']' == c) {
				s->state = 3;
			} else if ('(' == c || ')' == c || '#' == c) {
				s->state = 4;
			} else {
				s->state = 0;
			}
			continue;
		case 2:
			if (c >= 0x40 && c <= 0x7E) {
				csi_final(s, (char)c);
				s->state = 0;
			} else if (s->ncsi < (int)sizeof s->csi - 1) {
				s->csi[s->ncsi++] = (char)c;
				s->csi[s->ncsi] = 0;
			}
			continue;
		case 3:
			if (7 == c || 0x9C == c) {
				s->state = 0;
			} else if (0x1B == c) {
				s->state = 4; /* ESC \ */
			}
			continue;
		case 4: s->state = 0; continue;
		}
		if (s->u8need) { /* UTF-8 continuation */
			s->u8[s->u8n++] = c;
			if (s->u8n == s->u8need) {
				uint32_t cp;

				wf_utf8_decode((const char *)s->u8,
				        (size_t)s->u8n, &cp);
				put_cp(s, cp);
				s->u8need = s->u8n = 0;
			}
			continue;
		}
		if (0x1B == c) {
			s->state = 1;
		} else if ('\r' == c) {
			s->cx = 0;
		} else if ('\n' == c) {
			if (s->cy + 1 < s->rows) {
				s->cy++;
			}
		} else if ('\b' == c) {
			if (s->cx > 0) {
				s->cx--;
			}
		} else if ('\t' == c) {
			s->cx = (s->cx / 8 + 1) * 8 < s->cols
			        ? (s->cx / 8 + 1) * 8
			        : s->cols - 1;
		} else if (c >= 0x20 && c < 0x7F) {
			put_cp(s, c);
		} else if (c >= 0xC2) {
			s->u8[0] = c;
			s->u8n = 1;
			s->u8need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
		}
	}
}

/* Renders the screen as text rows (UTF-8), trailing blanks trimmed. */
static void screen_text(const Screen *s, char *out, size_t cap)
{
	size_t n = 0;
	int y, x;

	for (y = 0; y < s->rows; y++) {
		int last = -1;

		for (x = 0; x < s->cols; x++) {
			if (s->cell[y * s->cols + x] &&
			        0xFFFFFFFFu != s->cell[y * s->cols + x]) {
				last = x;
			}
		}
		for (x = 0; x <= last; x++) {
			uint32_t cp = s->cell[y * s->cols + x];
			char enc[4];
			size_t k;

			if (0xFFFFFFFFu == cp) {
				continue;
			}
			if (0 == cp) {
				cp = ' ';
			}
			k = wf_utf8_encode(cp, enc);
			if (n + k + 2 < cap) {
				memcpy(out + n, enc, k);
				n += k;
			}
		}
		if (n + 2 < cap) {
			out[n++] = '\n';
		}
	}
	out[n] = '\0';
}

typedef struct
{
	HPCON pc;
	HANDLE in_w, out_r;
	PROCESS_INFORMATION pi;
	HANDLE reader;
	CRITICAL_SECTION lock;
	Screen scr;
	uint8_t *raw; /* everything the child wrote (VT stream) */
	size_t raw_n, raw_cap;
	int eof;
	int capture_failed;
	double started;
} Pty;

static DWORD WINAPI reader_main(LPVOID arg)
{
	Pty *p = arg;
	uint8_t buf[8192];
	DWORD got;

	while (ReadFile(p->out_r, buf, sizeof buf, &got, NULL) && got > 0) {
		EnterCriticalSection(&p->lock);
		if (!p->capture_failed &&
		        p->raw_n + got <= 32u * 1024u * 1024u) {
			if (p->raw_n + got > p->raw_cap) {
				size_t cap = (p->raw_n + got) * 2;
				uint8_t *raw = realloc(p->raw, cap);
				if (!raw) {
					p->capture_failed = 1;
				} else {
					p->raw = raw;
					p->raw_cap = cap;
				}
			}
			if (!p->capture_failed) {
				memcpy(p->raw + p->raw_n, buf, got);
				p->raw_n += got;
			}
		} else {
			p->capture_failed = 1;
		}
		screen_feed(&p->scr, buf, got);
		LeaveCriticalSection(&p->lock);
	}
	EnterCriticalSection(&p->lock);
	p->eof = 1;
	LeaveCriticalSection(&p->lock);
	return 0;
}

static int pty_start(Pty *p, const wchar_t *cmdline, int cols, int rows)
{
	HANDLE in_r = NULL, out_w = NULL;
	COORD sz = {(SHORT)cols, (SHORT)rows};
	STARTUPINFOEXW si = {0};
	SIZE_T attr_sz = 0;
	wchar_t cmd[2048];

	memset(p, 0, sizeof *p);
	InitializeCriticalSection(&p->lock);
	screen_resize(&p->scr, cols, rows);
	if (!CreatePipe(&in_r, &p->in_w, NULL, 0) ||
	        !CreatePipe(&p->out_r, &out_w, NULL, 0)) {
		return 0;
	}
	if (FAILED(CreatePseudoConsole(sz, in_r, out_w, 0, &p->pc))) {
		return 0;
	}
	CloseHandle(in_r);
	CloseHandle(out_w);

	si.StartupInfo.cb = sizeof si;
	si.StartupInfo.lpDesktop =
	        g_clipboard_desktop[0] ? g_clipboard_desktop : NULL;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attr_sz);
	si.lpAttributeList = malloc(attr_sz);
	InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_sz);
	UpdateProcThreadAttribute(si.lpAttributeList, 0,
	        PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, p->pc, sizeof p->pc, NULL,
	        NULL);
	wcscpy_s(cmd, 2048, cmdline);
	p->started = now_ms();
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
	            EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo,
	            &p->pi)) {
		return 0;
	}
	DeleteProcThreadAttributeList(si.lpAttributeList);
	free(si.lpAttributeList);
	p->reader = CreateThread(NULL, 0, reader_main, p, 0, NULL);
	return 1;
}

/*
 * Keys are sent the way a person types them. Text that arrives in one burst
 * is how a paste looks, and pasted keys are never run as commands and a
 * pasted Enter never confirms; a typed line therefore goes two characters at
 * a time with a short pause, and its Enter a little later still. Strings with
 * an escape sequence in them are sent whole.
 */
#define TYPE_GAP_MS 40u
#define TYPED_ENTER_GAP_MS 120u

static size_t utf8_unit(unsigned char lead)
{
	return lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
}

static void pty_write(Pty *p, const char *bytes, size_t n)
{
	DWORD wrote;

	WriteFile(p->in_w, bytes, (DWORD)n, &wrote, NULL);
}

static double pty_send(Pty *p, const char *bytes, size_t n)
{
	double t = now_ms();
	static ULONGLONG text_sent_at; /* GetTickCount64 of the last text */
	const char cr = 13;
	size_t i = 0;

	if (n <= 1 || memchr(bytes, 0x1b, n)) {
		if (n >= 1 && cr == bytes[0] && text_sent_at) {
			ULONGLONG ago = GetTickCount64() - text_sent_at;

			if (ago < TYPED_ENTER_GAP_MS) {
				Sleep((DWORD)(TYPED_ENTER_GAP_MS - ago));
			}
		}
		pty_write(p, bytes, n);
		text_sent_at = n && (unsigned char)bytes[0] >= 0x20
		        ? GetTickCount64()
		        : 0;
		return t;
	}
	while (i < n) {
		size_t end = i, units = 0;
		bool is_cr = cr == bytes[i];

		if (is_cr && text_sent_at) {
			ULONGLONG ago = GetTickCount64() - text_sent_at;

			if (ago < TYPED_ENTER_GAP_MS) {
				Sleep((DWORD)(TYPED_ENTER_GAP_MS - ago));
			}
		}
		/* an Enter and other control keys go alone; text, two at a time */
		if ((unsigned char)bytes[i] < 0x20) {
			end = i + 1;
		} else {
			while (end < n && units < 2 &&
			        (unsigned char)bytes[end] >= 0x20) {
				end += utf8_unit((unsigned char)bytes[end]);
				units++;
			}
			if (end > n) {
				end = n;
			}
		}
		pty_write(p, bytes + i, end - i);
		text_sent_at =
		        (unsigned char)bytes[i] >= 0x20 ? GetTickCount64() : 0;
		i = end;
		if (i < n) {
			Sleep(TYPE_GAP_MS);
		}
	}
	return t;
}

static void pty_resize(Pty *p, int cols, int rows)
{
	COORD sz = {(SHORT)cols, (SHORT)rows};

	EnterCriticalSection(&p->lock);
	screen_resize(&p->scr, cols, rows);
	LeaveCriticalSection(&p->lock);
	ResizePseudoConsole(p->pc, sz);
}

static void pty_snapshot(Pty *p, char *out, size_t cap)
{
	EnterCriticalSection(&p->lock);
	screen_text(&p->scr, out, cap);
	LeaveCriticalSection(&p->lock);
}

/* The first cell showing `cp`, in reading order. */
static bool pty_find_cp(Pty *p, uint32_t cp, int *x, int *y)
{
	bool found = false;
	int cy, cx;

	EnterCriticalSection(&p->lock);
	for (cy = 0; cy < p->scr.rows && !found; cy++) {
		for (cx = 0; cx < p->scr.cols; cx++) {
			if (p->scr.cell[cy * p->scr.cols + cx] == cp) {
				*x = cx;
				*y = cy;
				found = true;
				break;
			}
		}
	}
	LeaveCriticalSection(&p->lock);
	return found;
}

/* The cursor the terminal ends up with: where, and whether it is shown. */
static void pty_cursor(Pty *p, int *x, int *y, int *visible)
{
	EnterCriticalSection(&p->lock);
	*x = p->scr.cx;
	*y = p->scr.cy;
	*visible = p->scr.cursor_visible;
	LeaveCriticalSection(&p->lock);
}

static void pty_finish(Pty *p)
{
	if (p->pc) {
		ClosePseudoConsole(p->pc);
	}
	if (p->reader) {
		WaitForSingleObject(p->reader, 5000);
		CloseHandle(p->reader);
	}
	if (p->pi.hProcess) {
		if (WAIT_OBJECT_0 != WaitForSingleObject(p->pi.hProcess, 100)) {
			TerminateProcess(p->pi.hProcess,
			        99); /* harness cleanup of its own child */
		}
		CloseHandle(p->pi.hProcess);
		CloseHandle(p->pi.hThread);
	}
	CloseHandle(p->in_w);
	CloseHandle(p->out_r);
	free(p->scr.cell);
	free(p->raw);
	DeleteCriticalSection(&p->lock);
}

static int contains(const uint8_t *hay, size_t n, const char *needle)
{
	size_t m = strlen(needle), i;

	for (i = 0; i + m <= n; i++) {
		if (!memcmp(hay + i, needle, m)) {
			return 1;
		}
	}
	return 0;
}

typedef struct
{
	double t;
	char kind[32];
	char rest[160];
} LogLine;

/* Reads the whole CSV; returns number of lines parsed into `out` (max cap). */
static size_t read_log(const wchar_t *path, LogLine *out, size_t cap)
{
	FILE *f = NULL;
	char line[512];
	size_t n = 0;

	f = _wfsopen(path, L"rb", _SH_DENYNO);
	if (!f) {
		return 0;
	}
	fgets(line, sizeof line, f); /* header */
	while (n < cap && fgets(line, sizeof line, f)) {
		char *c1 = strchr(line, ',');
		char *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
		size_t len;

		if (!c1) {
			continue;
		}
		out[n].t = atof(line);
		len = (size_t)((c2 ? c2 : line + strlen(line)) - (c1 + 1));
		if (len >= sizeof out[n].kind) {
			len = sizeof out[n].kind - 1;
		}
		memcpy(out[n].kind, c1 + 1, len);
		out[n].kind[len] = '\0';
		snprintf(out[n].rest, sizeof out[n].rest, "%s",
		        c2 ? c2 + 1 : "");
		out[n].rest[strcspn(out[n].rest, "\r\n")] = '\0';
		n++;
	}
	fclose(f);
	return n;
}

static LogLine g_lines[20000];

/* Waits until the child's log has a `kind` line at or after time `after`. */
static double wait_for(
        const wchar_t *log, const char *kind, double after, DWORD timeout_ms)
{
	double end = now_ms() + timeout_ms;

	for (;;) {
		size_t n = read_log(log, g_lines, WF_ARRAY_LEN(g_lines)), i;

		for (i = 0; i < n; i++) {
			if (g_lines[i].t >= after &&
			        !strcmp(g_lines[i].kind, kind)) {
				return g_lines[i].t;
			}
		}
		if (now_ms() > end) {
			return -1;
		}
		Sleep(10);
	}
}

static int count_kind(
        const wchar_t *log, const char *kind, double from, double to)
{
	size_t n = read_log(log, g_lines, WF_ARRAY_LEN(g_lines)), i;
	int c = 0;

	for (i = 0; i < n; i++) {
		if (g_lines[i].t >= from && g_lines[i].t <= to &&
		        !strcmp(g_lines[i].kind, kind)) {
			c++;
		}
	}
	return c;
}

/* The mode the child reported last at or before `t` ("NORMAL", "CONFIRM"...).
 */
static const char *mode_at(const wchar_t *log, double t)
{
	static char mode[32];
	size_t n = read_log(log, g_lines, WF_ARRAY_LEN(g_lines)), i;

	snprintf(mode, sizeof mode, "?");
	for (i = 0; i < n; i++) {
		if (g_lines[i].t <= t && !strcmp(g_lines[i].kind, "STATE")) {
			const char *c = strchr(g_lines[i].rest, ',');

			if (c) {
				size_t k = strcspn(c + 1, ",");

				snprintf(mode, sizeof mode, "%.*s",
				        (int)(k < 31 ? k : 31), c + 1);
			}
		}
	}
	return mode;
}

typedef struct
{
	wchar_t dir[MAX_PATH * 2];
	char run_id[80];
	FILE *results; /* artifacts/results.csv */
	int fails, passes;
} Ev;

static int ev_open(Ev *e,
        const wchar_t *root,
        const char *test,
        const char *scenario,
        const wchar_t *label)
{
	char ts[20], label8[64];
	wchar_t p[MAX_PATH * 2];

	memset(e, 0, sizeof *e);
	p0_utc_compact(ts);
	WideCharToMultiByte(
	        CP_UTF8, 0, label, -1, label8, sizeof label8, NULL, NULL);
	snprintf(e->run_id, sizeof e->run_id, "%s-%s-%s", ts, scenario, label8);
	swprintf(e->dir, MAX_PATH * 2, L"%ls\\%hs\\%hs", root, test, e->run_id);
	swprintf(p, MAX_PATH * 2, L"%ls\\%hs", root, test);
	if (!p0_mkdirs(p) || !CreateDirectoryW(e->dir, NULL)) {
		return 0;
	}
	for (int i = 0; i < 4; i++) {
		static const wchar_t *const sub[] = {
		        L"traces", L"artifacts", L"eventlog", L"screenshots"};

		swprintf(p, MAX_PATH * 2, L"%ls\\%ls", e->dir, sub[i]);
		if (!p0_mkdirs(p)) {
			return 0;
		}
	}
	swprintf(p, MAX_PATH * 2, L"%ls\\artifacts\\results.csv", e->dir);
	e->results = p0_fopen_w(p);
	if (!e->results) {
		return 0;
	}
	fputs("check,rep,expected,observed,result\n", e->results);
	return 1;
}

static void ev_check(Ev *e,
        const char *check,
        int rep,
        const char *expected,
        const char *observed,
        int pass)
{
	/* free-text columns are quoted: they contain commas */
	fprintf(e->results, "%s,%d,\"%s\",\"%s\",%s\n", check, rep, expected,
	        observed, pass ? "PASS" : "FAIL");
	/* A CTest timeout can kill the host before normal evidence teardown. */
	fflush(e->results);
	if (pass) {
		e->passes++;
	} else {
		e->fails++;
		/* Fixed check identifiers only; free-text evidence stays in the CSV. */
		printf("FAIL check=%s rep=%d\n", check, rep);
		fflush(stdout);
	}
}

static void ev_write_text(const Ev *e, const wchar_t *rel, const char *text)
{
	wchar_t p[MAX_PATH * 2];
	FILE *f;

	swprintf(p, MAX_PATH * 2, L"%ls\\%ls", e->dir, rel);
	f = p0_fopen_w(p);
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

static void ev_write_raw(
        const Ev *e, const wchar_t *rel, const uint8_t *b, size_t n)
{
	wchar_t p[MAX_PATH * 2];
	FILE *f;

	swprintf(p, MAX_PATH * 2, L"%ls\\%ls", e->dir, rel);
	f = p0_fopen_w(p);
	if (f) {
		fwrite(b, 1, n, f);
		fclose(f);
	}
}

static void ev_close(Ev *e,
        const char *test,
        const char *scenario,
        const char *const *hyp,
        const char *const *dec,
        const char *result,
        const char *notes,
        const wchar_t *work)
{
	wchar_t p[MAX_PATH * 2], exe[MAX_PATH];
	char hash[P0_HASH_HEX], bid[64];
	char term[512];

	fclose(e->results);
	swprintf(p, MAX_PATH * 2, L"%ls\\environment.json", e->dir);
	p0_write_environment(p, work, "p0_conpty_host", VERSION);
	snprintf(term, sizeof term,
	        "{\"terminal\": \"ConPTY (CreatePseudoConsole) hosted by "
	        "p0_conpty_host\","
	        " \"is_windows_terminal\": false, \"is_conhost_window\": false,"
	        " \"scenario\": \"%s\"}\n",
	        scenario);
	ev_write_text(e, L"artifacts\\terminal.json", term);
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	p0_sha256_file(exe, hash);
	snprintf(bid, sizeof bid, "p0_conpty_host-%s-%.12s", VERSION, hash);
	swprintf(p, MAX_PATH * 2, L"%ls\\run.json", e->dir);
	p0_write_run_json(
	        p, test, e->run_id, bid, "n/a", hyp, result, dec, notes);
}

static wchar_t g_child[MAX_PATH * 2];

static int product_wait_ms(
        Pty *p, const char *needle, char *screen, size_t cap, DWORD ms)
{
	ULONGLONG deadline = GetTickCount64() + ms;
	ULONGLONG stable_since = GetTickCount64();
	size_t previous = SIZE_MAX;
	do {
		size_t bytes;
		pty_snapshot(p, screen, cap);
		EnterCriticalSection(&p->lock);
		bytes = p->raw_n;
		LeaveCriticalSection(&p->lock);
		if (bytes != previous) {
			previous = bytes;
			stable_since = GetTickCount64();
		}
		/* A substring in a partial VT frame can still belong to the old
		 * mode. Wait for output quiescence before injecting the next
		 * command. */
		if (strstr(screen, needle) &&
		        GetTickCount64() - stable_since >= 60) {
			return 1;
		}
		if (WAIT_OBJECT_0 == WaitForSingleObject(p->pi.hProcess, 0)) {
			break;
		}
		Sleep(10);
	} while (GetTickCount64() < deadline);
	return 0;
}

static int product_wait(Pty *p, const char *needle, char *screen, size_t cap)
{
	return product_wait_ms(p, needle, screen, cap, 4000);
}

static int product_file(P0Lab *lab, const wchar_t *relative, const char *text)
{
	wchar_t path[1024];
	HANDLE h;
	DWORD wrote;
	int ok;
	if (!p0_lab_path(lab, relative, path, 1024)) {
		return 0;
	}
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	ok = WriteFile(h, text, (DWORD)strlen(text), &wrote, NULL) &&
	        wrote == strlen(text);
	CloseHandle(h);
	return ok && p0_area_track(lab->area, relative, 0);
}

/* The same, for bytes (a picture has NUL bytes in it). */
static int product_file_bytes(
        P0Lab *lab, const wchar_t *relative, const void *bytes, size_t n)
{
	wchar_t path[1024];
	HANDLE h;
	DWORD wrote;
	int ok;

	if (!p0_lab_path(lab, relative, path, 1024)) {
		return 0;
	}
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	ok = WriteFile(h, bytes, (DWORD)n, &wrote, NULL) && wrote == n;
	CloseHandle(h);
	return ok && p0_area_track(lab->area, relative, 0);
}

/* The file type the Open scenario registers for the duration of the run. */
static WfTestAssoc open_association;
#define OPEN_EXT open_association.ext

/* Drives only this harness's isolated pseudo console and generated fixtures.
 * The actual product executable is exercised; this is not WT/IME evidence. */
/* One scripted terminal session against the real product: the state its
 * phases share. */
typedef struct
{
	Ev *e;
	P0Lab lab;
	Pty p;
	bool started;
	bool blocked; /* unavailable prerequisite, never PASS */
	wchar_t browse[1024], config[1024], last[1024], source[1024],
	        dest[1024], command[2048];
	wchar_t *saved_local, *saved_roaming;
	char screen[16384], path8[1024], before[65], after[65], output[1024];
} Product;

/* Retain actual decoded terminal cells at every scripted UI transition. */
static void product_snapshot(Product *pr, const char *name)
{
	wchar_t file[160];
	swprintf(file, 160, L"artifacts\\%hs.txt", name);
	ev_write_text(pr->e, file, pr->screen);
}

static void product_idle_measure(Product *pr, double startup_ms)
{
	PROCESS_MEMORY_COUNTERS_EX memory = {0};
	FILETIME created, exited, kernel, user;
	ULARGE_INTEGER before, after;
	double start = now_ms();
	memory.cb = sizeof memory;
	if (!GetProcessTimes(
	            pr->p.pi.hProcess, &created, &exited, &kernel, &user)) {
		return;
	}
	before.LowPart = kernel.dwLowDateTime;
	before.HighPart = kernel.dwHighDateTime;
	before.QuadPart +=
	        ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
	Sleep(250);
	if (!GetProcessTimes(
	            pr->p.pi.hProcess, &created, &exited, &kernel, &user) ||
	        !K32GetProcessMemoryInfo(pr->p.pi.hProcess,
	                (PROCESS_MEMORY_COUNTERS *)&memory, sizeof memory)) {
		return;
	}
	after.LowPart = kernel.dwLowDateTime;
	after.HighPart = kernel.dwHighDateTime;
	after.QuadPart +=
	        ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
	snprintf(pr->output, sizeof pr->output,
	        "startup_to_listing_conpty_ms=%.3f\nworking_set_bytes=%"
	        "zu\nprivate_bytes=%zu\nidle_cpu_ms=%.3f\nidle_window_ms=%."
	        "3f\n",
	        startup_ms, (size_t)memory.WorkingSetSize,
	        (size_t)memory.PrivateUsage,
	        (double)(after.QuadPart - before.QuadPart) / 10000.0,
	        now_ms() - start);
	ev_write_text(pr->e, L"artifacts\\ui-performance.txt", pr->output);
}

/* A fixture failure ends the session; a missing screen text does too. */
#define PRODUCT_REQUIRE(condition)                                             \
	do {                                                                   \
		if (!(condition)) {                                            \
			ev_check(pr->e, #condition, 0, "success",              \
			        "fixture failure", 0);                         \
			return false;                                          \
		}                                                              \
	} while (0)
/* A form key sent while the listing is still reloading is refused with "wait
 * for a current listing"; that is not a product failure, so the key is sent
 * again after a short pause. Any other miss fails as usual. */
#define PRODUCT_STEP_RETRY(keys, needle, name)                                 \
	do {                                                                   \
		int attempt;                                                   \
		Sleep(600); /* the reloads of the last change settle */        \
		ok = 0;                                                        \
		for (attempt = 0; attempt < 6 && !ok; attempt++) {             \
			pty_send(&pr->p, keys, strlen(keys));                  \
			ok = product_wait_ms(&pr->p, needle, pr->screen,       \
			        sizeof pr->screen, 1500);                      \
			if (!ok && !strstr(pr->screen, "current listing"))     \
				break;                                         \
			if (!ok)                                               \
				Sleep(400);                                    \
		}                                                              \
		product_snapshot(pr, name);                                    \
		ev_check(pr->e, name, 0, needle, ok ? "observed" : "missing",  \
		        ok);                                                   \
		if (!ok)                                                       \
			return false;                                          \
	} while (0)
#define PRODUCT_STEP(keys, needle, name)                                       \
	do {                                                                   \
		pty_send(&pr->p, keys, strlen(keys));                          \
		ok = product_wait(                                             \
		        &pr->p, needle, pr->screen, sizeof pr->screen);        \
		product_snapshot(pr, name);                                    \
		ev_check(pr->e, name, 0, needle, ok ? "observed" : "missing",  \
		        ok);                                                   \
		if (!ok)                                                       \
			return false;                                          \
	} while (0)

static bool read_small(const wchar_t *path, char *out, size_t cap);

/* The owned fixture, a private state root, and the product started in a pseudo-console. */
static bool product_setup(Product *pr)
{
	int ok;
	DWORD need;
	double started;

#ifdef WF_PRODUCT_EDIT
	PRODUCT_REQUIRE(p0_area_mkdir(pr->lab.area, L"mvsrc") &&
	        p0_area_mkdir(pr->lab.area, L"mvsrc2") &&
	        p0_area_mkdir(pr->lab.area, L"mvdst"));
	PRODUCT_REQUIRE(product_file(&pr->lab, L"mvsrc\\a.txt", "AAA") &&
	        product_file(&pr->lab, L"mvsrc\\b.txt", "BBB") &&
	        product_file(&pr->lab, L"mvsrc2\\c.txt", "CNEW") &&
	        product_file(&pr->lab, L"mvdst\\c.txt", "COLD"));
#endif
	PRODUCT_REQUIRE(p0_area_mkdir(pr->lab.area, L"opendir"));
	wchar_t associated[128];
	swprintf(associated, 128, L"opendir\\a%ls", OPEN_EXT);
	PRODUCT_REQUIRE(product_file(&pr->lab, associated, "x") &&
	        product_file(&pr->lab, L"opendir\\run.bat",
	                "@echo off\r\necho ran> \"%~dp0ran.txt\"\r\n"));
	PRODUCT_REQUIRE(p0_area_mkdir(pr->lab.area, L"editdir") &&
	        p0_area_mkdir(pr->lab.area, L"browse") &&
	        p0_area_mkdir(pr->lab.area, L"browse\\child") &&
	        p0_area_mkdir(pr->lab.area, L"dest") &&
	        p0_area_mkdir(pr->lab.area, L"state"));
	PRODUCT_REQUIRE(product_file(&pr->lab, L"browse\\doc.txt",
	        "SAFE_PREVIEW_MARKER\x1b[31m\nline two\n"
	        "row03\n"
	        "row04\n"
	        "row05\n"
	        "row06\n"
	        "row07\n"
	        "row08\n"
	        "row09\n"
	        "row10\n"
	        "row11\n"
	        "row12\n"
	        "row13\n"
	        "row14\n"
	        "row15\n"
	        "row16\n"
	        "row17\n"
	        "row18\n"
	        "row19\n"
	        "row20\n"
	        "row21\n"
	        "row22\n"
	        "row23\n"
	        "row24\n"
	        "row25\n"
	        "row26\n"
	        "row27\n"
	        "row28\n"
	        "row29\n"
	        "row30\n"
	        "row31\n"
	        "LAST_PREVIEW_LINE"));
	PRODUCT_REQUIRE(product_file(
	        &pr->lab, L"browse\\child\\inside.txt", "INSIDE_MARKER\n"));
	PRODUCT_REQUIRE(product_file(&pr->lab, L"browse\\child\\inside2.txt",
	        "INSIDE_TWO_MARKER\n"));
	PRODUCT_REQUIRE(product_file(&pr->lab, L"config.ini",
	        "[preview]\ntext_fallback_encoding=utf8\nimages=true\n"
	        "image_style=halfblock\n[ui]\nparent_ratio="
	        "25\ncurrent_ratio=45\npreview_ratio=30\n"));
	PRODUCT_REQUIRE(p0_lab_path(&pr->lab, L"browse", pr->browse, 1024) &&
	        p0_lab_path(&pr->lab, L"config.ini", pr->config, 1024) &&
	        p0_lab_path(&pr->lab, L"last.txt", pr->last, 1024) &&
	        p0_lab_path(&pr->lab, L"browse\\doc.txt", pr->source, 1024) &&
	        p0_lab_path(&pr->lab, L"dest", pr->dest, 1024));
	PRODUCT_REQUIRE(p0_sha256_file(pr->source, pr->before));
	need = GetEnvironmentVariableW(L"LOCALAPPDATA", NULL, 0);
	if (need) {
		pr->saved_local =
		        malloc((size_t)need * sizeof *pr->saved_local);
		PRODUCT_REQUIRE(pr->saved_local);
		PRODUCT_REQUIRE(GetEnvironmentVariableW(
		        L"LOCALAPPDATA", pr->saved_local, need));
	}
	need = GetEnvironmentVariableW(L"APPDATA", NULL, 0);
	if (need) {
		pr->saved_roaming =
		        malloc((size_t)need * sizeof *pr->saved_roaming);
		PRODUCT_REQUIRE(pr->saved_roaming);
		PRODUCT_REQUIRE(GetEnvironmentVariableW(
		        L"APPDATA", pr->saved_roaming, need));
	}
	PRODUCT_REQUIRE(
	        SetEnvironmentVariableW(L"LOCALAPPDATA", pr->lab.root) &&
	        SetEnvironmentVariableW(L"APPDATA", pr->lab.root));
	swprintf(pr->command, 2048,
	        L"\"%ls\" --language en --config \"%ls\" --print-last-dir "
	        L"\"%ls\" \"%ls\"",
	        g_child, pr->config, pr->last, pr->browse);
	started = now_ms();
	pr->started = pty_start(&pr->p, pr->command, 160, 30);
	PRODUCT_REQUIRE(
	        SetEnvironmentVariableW(L"LOCALAPPDATA", pr->saved_local) &&
	        SetEnvironmentVariableW(L"APPDATA", pr->saved_roaming));
	PRODUCT_REQUIRE(pr->started);
	/* The first normal session now requires an explicit network choice.
	 * Exercise the real opt-out; do not bypass consent in the product. */
	ok = product_wait(&pr->p, "Check for updates automatically?",
	        pr->screen, sizeof pr->screen);
	ev_check(pr->e, "updates_consent_present", 0, "permission prompt",
	        ok ? "observed" : "missing", ok);
	PRODUCT_REQUIRE(ok);
	pty_send(&pr->p, "\x1b", 1);
	ok = product_wait(&pr->p, "doc.txt", pr->screen, sizeof pr->screen);
	if (ok) {
		product_idle_measure(pr, now_ms() - started);
	}
	ev_check(pr->e, "product_start_listing", 0, "doc.txt",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		return false;
	}
	product_snapshot(pr, "updates_opt_out");
	bool saved = read_small(pr->config, pr->output, sizeof pr->output) &&
	        strstr(pr->output, "consent_recorded = true") &&
	        !strstr(pr->output, "enabled = true");
	ev_check(pr->e, "updates_opt_out_persisted", 0,
	        "consent saved, automatic checks disabled",
	        saved ? "observed" : "missing", saved);
	if (!saved) {
		return false;
	}
	return true;
}

/* Text preview: sanitized, line-numbered, scrolled to both ends inside the pane. */
static bool product_preview(Product *pr)
{
	int ok;

	PRODUCT_STEP(
	        "j", "SAFE_PREVIEW_MARKER^[[31m", "text_preview_sanitized");
	PRODUCT_STEP("\x1b[C", "PREVIEW_PANE", "focus_preview_pane");
	ev_check(pr->e, "pane_keeps_three_columns", 0,
	        "Current and read-only Preview",
	        strstr(pr->screen, "Current") &&
	                        strstr(pr->screen, "Preview [READ ONLY]")
	                ? "observed"
	                : "missing",
	        strstr(pr->screen, "Current") &&
	                strstr(pr->screen, "Preview [READ ONLY]"));
	ev_check(pr->e, "preview_line_numbers", 0, "1 | SAFE_PREVIEW_MARKER",
	        strstr(pr->screen, "1 | SAFE_PREVIEW_MARKER") ? "observed"
	                                                      : "missing",
	        NULL != strstr(pr->screen, "1 | SAFE_PREVIEW_MARKER"));
	PRODUCT_STEP("\x1b[B", "line two", "pane_scroll_down");
	ev_check(pr->e, "pane_first_line_scrolled", 0, "first line absent",
	        strstr(pr->screen, "SAFE_PREVIEW_MARKER") ? "still visible"
	                                                  : "absent",
	        NULL == strstr(pr->screen, "SAFE_PREVIEW_MARKER"));
	PRODUCT_STEP("\x1b[A", "SAFE_PREVIEW_MARKER", "pane_scroll_up");
	{
		char scroll[121];
		const char *last_line;
		int last_row = 0;

		for (int i = 0; i < 40; i++) {
			memcpy(scroll + i * 3, "\x1b[B", 3);
		}
		scroll[120] = 0;
		PRODUCT_STEP(
		        scroll, "32 | LAST_PREVIEW_LINE", "pane_scroll_to_end");
		last_line = strstr(pr->screen, "32 | LAST_PREVIEW_LINE");
		if (last_line) {
			for (const char *at = pr->screen; at < last_line;
			        at++) {
				last_row += *at == '\n';
			}
		}
		ev_check(pr->e, "preview_last_line_at_bottom", 0, "text row 27",
		        last_line && 27 == last_row ? "bottom" : "wrong row",
		        last_line && 27 == last_row);
		PRODUCT_STEP("\x1b[B", "32 | LAST_PREVIEW_LINE",
		        "pane_end_stays_visible");
		for (int i = 0; i < 40; i++) {
			memcpy(scroll + i * 3, "\x1b[A", 3);
		}
		PRODUCT_STEP(
		        scroll, "SAFE_PREVIEW_MARKER", "pane_return_to_top");
	}
	PRODUCT_STEP("\x1b[1;5B", "32 | LAST_PREVIEW_LINE", "preview_ctrl_end");
	PRODUCT_STEP("\x1b[1;5A", "SAFE_PREVIEW_MARKER", "preview_ctrl_home");
	PRODUCT_STEP("\x1b[D", "NORMAL", "pane_return_to_list");
	return true;
}

/* Full-screen preview, help, and the blocked operations: each shows its gate and changes nothing. */
static bool product_gates(Product *pr)
{
	int ok;

	PRODUCT_STEP("v", "PREVIEW", "fullscreen_preview");
	PRODUCT_STEP("\x1b", "NORMAL", "fullscreen_escape");
	PRODUCT_STEP("?", "HELP", "help");
	PRODUCT_STEP("\x1b", "NORMAL", "help_return");
#ifdef WF_PRODUCT_EDIT
	/* the forms are live; their effect is checked in product_edit() */
	PRODUCT_STEP("a", "d  New folder", "create_menu_visible");
	PRODUCT_STEP("%", "New file name: Enter creates",
	        "create_file_form_visible");
	PRODUCT_STEP("\x1b", "NORMAL", "create_form_escape");
	PRODUCT_STEP("\x1bOQ", "Rename: Enter renames", "rename_form_visible");
	PRODUCT_STEP("\x1b", "NORMAL", "rename_form_escape");
#else
	PRODUCT_STEP("a", "CREATE (execution BLOCKED)", "create_menu_visible");
	PRODUCT_STEP("%", "New file name:", "create_file_form_visible");
	PRODUCT_STEP(
	        "candidate.txt\r", "BLOCKED create:", "create_submit_blocked");
	PRODUCT_STEP("\x1bOQ", "Rename name:", "rename_form_visible");
	PRODUCT_STEP("\r", "BLOCKED rename:", "rename_submit_blocked");
#endif
	/* Delete asks first and says where the file goes; Esc changes nothing */
	PRODUCT_STEP("\x1b[3~", "Recycle Bin", "recycle_asks_first");
	PRODUCT_STEP("\x1b", "NORMAL", "recycle_question_escape");
	ok = p0_sha256_file(pr->source, pr->after) &&
	        !strcmp(pr->before, pr->after) && strstr(pr->screen, "NORMAL");
	ev_check(pr->e, "recycle_refusal_keeps_source", 0,
	        "unchanged source, normal mode", ok ? "preserved" : "changed",
	        ok);
	return true;
}

/* Directory navigation, remembered positions, history, and resizing while navigating. */
static bool product_navigation(Product *pr)
{
	int ok;

	PRODUCT_STEP("k", "child/", "select_directory");
	/* the cursor alone lists the folder in the preview pane; the current
	 * folder is still browse (the title keeps it) */
	PRODUCT_STEP("", "inside.txt", "directory_preview_lists_children");
	ok = NULL != strstr(pr->screen, "item") &&
	        NULL == strstr(pr->screen, "Directory (metadata only)");
	ev_check(pr->e, "directory_preview_replaces_metadata_notice", 0,
	        "count line, no metadata notice", ok ? "observed" : "missing",
	        ok);
	PRODUCT_STEP("l", "inside.txt", "navigate_directory");
	PRODUCT_STEP("\x1b[1;5B", "INSIDE_TWO_MARKER", "three_column_ctrl_end");
	PRODUCT_STEP("\x1b[1;5A", "INSIDE_MARKER", "three_column_ctrl_home");
	PRODUCT_STEP("\x1b[1;5B", "INSIDE_TWO_MARKER",
	        "three_column_ctrl_end_again");
	pty_resize(&pr->p, 70, 24);
	ok = product_wait(&pr->p, "Current", pr->screen, sizeof pr->screen);
	ev_check(pr->e, "navigation_resize_two_columns", 0, "Current",
	        ok ? "observed" : "missing", ok);
	PRODUCT_STEP("\x1b[1;5A", "INSIDE_MARKER", "two_column_ctrl_home");
	PRODUCT_STEP("\x1b[1;5B", "INSIDE_TWO_MARKER", "two_column_ctrl_end");
	pty_resize(&pr->p, 40, 12);
	PRODUCT_STEP("", "Current", "navigation_narrow_40");
	pty_resize(&pr->p, 20, 5);
	PRODUCT_STEP("", "too small", "navigation_short_20x5");
	pty_resize(&pr->p, 160, 30);
	PRODUCT_REQUIRE(
	        product_wait(&pr->p, "Current", pr->screen, sizeof pr->screen));
	PRODUCT_STEP("h", "doc.txt", "memory_parent_return");
	PRODUCT_STEP(
	        "l", "INSIDE_TWO_MARKER", "memory_child_position_restored");
	PRODUCT_STEP("b", "doc.txt", "history_back");
	PRODUCT_STEP("f", "inside.txt", "history_forward");
	PRODUCT_STEP("h", "doc.txt", "navigate_parent");
	return true;
}

static bool go_by_path_bar(Product *pr, const wchar_t *dir, const char *name);

/* Smart Go (Ctrl+L), candidates, Tab completion and the details window,
 * driven through the real terminal input path (Ctrl+L arrives as byte 0x0C). */
static bool product_path_bar_and_details(Product *pr)
{
	int ok;

	PRODUCT_STEP("\x0c", "Go to: ", "ctrl_l_opens_path_bar");
	PRODUCT_STEP("\x1b", "NORMAL", "path_bar_escape");
	PRODUCT_STEP("\x0c"
	             "chi",
	        "Go to: chi", "path_bar_typing_replaces_selection");
	PRODUCT_STEP("", "child/", "path_bar_lists_candidates");
	PRODUCT_STEP("\t", "Go to: child\\", "path_bar_tab_completes_folder");
	PRODUCT_STEP("\x1b", "NORMAL", "path_bar_escape_after_tab");
	/* `g`, then `~`: the first five entries of the real profile folder
	 * (listing only), Down chooses one, Tab completes without going, Enter
	 * goes; the fixture is then visited again. */
	PRODUCT_STEP("g", "Go to: ", "smartgo_g_opens");
	PRODUCT_STEP("~", "Go to: ~", "smartgo_tilde_typed");
	Sleep(300);
	ok = product_wait_ms(
	        &pr->p, "Tab Complete", pr->screen, sizeof pr->screen, 3000);
	ev_check(pr->e, "smartgo_hint_visible", 0, "Tab Complete  Enter Go",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		return false;
	}
	PRODUCT_STEP("\x1b[B", ">", "smartgo_down_chooses_a_candidate");
	PRODUCT_STEP("\t", "Go to: ~\\", "smartgo_tab_completes_without_going");
	ev_check(pr->e, "smartgo_tab_did_not_navigate", 0, "still in prompt",
	        strstr(pr->screen, "PATH") ? "observed" : "missing",
	        NULL != strstr(pr->screen, "PATH"));
	PRODUCT_STEP("\r", "NORMAL", "smartgo_enter_goes");
	if (!go_by_path_bar(pr, pr->browse, "smartgo_return_to_browse")) {
		return false;
	}
	PRODUCT_STEP("i", "INFO", "details_window");
	ev_check(pr->e, "details_show_size_and_dates", 0, "Size and Modified",
	        strstr(pr->screen, "Size:") && strstr(pr->screen, "Modified:")
	                ? "observed"
	                : "missing",
	        strstr(pr->screen, "Size:") && strstr(pr->screen, "Modified:"));
	PRODUCT_STEP("x", "NORMAL", "details_any_key_closes");
	return true;
}

static bool file_exists(const wchar_t *path)
{
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(path);
}

/* Goes to `dir` through the path bar: Ctrl+L, type the path, Enter. */
static bool go_by_path_bar(Product *pr, const wchar_t *dir, const char *name)
{
	char utf8[1024], keys[1100];
	int ok;

	PRODUCT_REQUIRE(WideCharToMultiByte(
	        CP_UTF8, 0, dir, -1, utf8, sizeof utf8, NULL, NULL));
	snprintf(keys, sizeof keys,
	        "\x0c"
	        "%s\r",
	        utf8);
	PRODUCT_STEP(keys, "NORMAL", name);
	return true;
}

/* Registers OPEN_EXT for this user so that opening it runs open_target.exe
 * (next to this program). The key is removed again by open_unregister(). */
static bool open_register(void)
{
	wchar_t exe[1024], *slash;
	DWORD n = GetModuleFileNameW(NULL, exe, 1024);
	if (!n || n >= 1024 || !(slash = wcsrchr(exe, L'\\'))) {
		return false;
	}
	wcscpy_s(slash + 1, (size_t)(exe + 1024 - slash - 1),
	        L"open_target.exe");
	if (!file_exists(exe)) {
		return false;
	}
	return ERROR_SUCCESS == wf_test_assoc_register(&open_association, exe);
}

static LSTATUS open_unregister(void)
{
	return wf_test_assoc_cleanup(&open_association);
}

/* The note open_target.exe leaves: UTF-16, line 1 its working directory. */
static bool open_read_note(const wchar_t *path, wchar_t *out, size_t cap)
{
	DWORD i, got = 0;
	HANDLE h;

	for (i = 0; i < 100; i++) {
		h = CreateFileW(path, GENERIC_READ,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE != h) {
			got = 0;
			if (ReadFile(h, out, (DWORD)((cap - 1) * sizeof *out),
			            &got, NULL) &&
			        got >= 2 * sizeof *out &&
			        L'\n' == out[got / sizeof *out - 1]) {
				CloseHandle(h);
				out[got / sizeof *out] = 0;
				return true;
			}
			CloseHandle(h);
		}
		Sleep(100);
	}
	return false;
}

/* Enter on a file through the real terminal: an associated program is
 * started by the shell from the private neutral directory and receives the
 * file's absolute path; a script asks first and Esc leaves it unrun. */
static bool product_open_files(Product *pr)
{
	wchar_t dir[1024], note[1100], text[2200], neutral[1100], seen_rel[128],
	        *eol;
	int ok;

	PRODUCT_REQUIRE(p0_lab_path(&pr->lab, L"opendir", dir, 1024));
	if (!go_by_path_bar(pr, dir, "open_go_to_folder")) {
		return false;
	}
	/* a type nobody has vetted asks first; only the letter answers */
	PRODUCT_STEP("\r", "Opening can run code", "open_unknown_type_asks");
	Sleep(350); /* the confirmation ignores input for 300 ms */
	char opened[128];
	snprintf(opened, sizeof opened, "Opened: a%ls", OPEN_EXT);
	PRODUCT_STEP("o", opened, "open_program_started");
	swprintf(note, 1100, L"%ls\\a%ls.seen", dir, OPEN_EXT);
	ok = open_read_note(note, text, 2200);
	swprintf(seen_rel, 128, L"opendir\\a%ls.seen", OPEN_EXT);
	ok = ok && p0_area_track(pr->lab.area, seen_rel, 0);
	ev_check(pr->e, "open_program_left_its_note", 0, "note written",
	        ok ? "present" : "absent", ok);
	if (!ok) {
		return false;
	}
	eol = wcschr(text, L'\n');
	if (eol) {
		*eol = 0;
	}
	PRODUCT_REQUIRE(p0_lab_path(&pr->lab, L"piero\\cwd", neutral, 1100));
	ok = eol && !_wcsicmp(text, neutral);
	{
		char shown[1100];

		/* on failure the observed column carries the directory */
		if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, shown,
		            sizeof shown, NULL, NULL)) {
			strcpy_s(shown, sizeof shown, "other");
		}
		ev_check(pr->e, "open_started_from_neutral_directory", 0,
		        "state\\cwd", ok ? "neutral" : shown, ok);
	}
	swprintf(neutral, 1100, L"%ls\\a%ls", dir, OPEN_EXT);
	ok = eol && NULL != wcsstr(eol + 1, neutral);
	ev_check(pr->e, "open_passed_absolute_path", 0, "absolute path",
	        ok ? "passed" : "missing", ok);

	/* a script: the question, never Enter, and Esc runs nothing */
	PRODUCT_STEP("\x1b[1;5B", "run.bat", "open_select_script");
	PRODUCT_STEP("\r", "Opening can run code", "open_script_asks");
	Sleep(350); /* the confirmation ignores input for 300 ms */
	PRODUCT_STEP("\x1b", "NORMAL", "open_script_cancel");
	Sleep(1000);
	swprintf(note, 1100, L"%ls\\ran.txt", dir);
	ev_check(pr->e, "open_script_did_not_run", 0, "no ran.txt",
	        file_exists(note) ? "ran" : "absent", !file_exists(note));
	return go_by_path_bar(pr, pr->browse, "open_return_to_browse");
}

static bool product_open(Product *pr)
{
	bool ok;

	if (!open_register()) {
		pr->blocked = ERROR_ACCESS_DENIED == open_association.error &&
		        ERROR_SUCCESS == open_association.cleanup_error;
		if (pr->blocked) {
			snprintf(pr->output, sizeof pr->output,
			        "BLOCKED: HKCU association registration: Win32 "
			        "%ld; cleanup %ld\n",
			        open_association.error,
			        open_association.cleanup_error);
			fputs(pr->output, stdout);
			ev_write_text(pr->e,
			        L"artifacts\\association-prerequisite.txt",
			        pr->output);
		} else {
			ev_check(pr->e, "open_registration", 0,
			        "isolated association", "failed", 0);
		}
		(void)open_unregister();
		return false;
	}
	ok = product_open_files(pr);
	if (ERROR_SUCCESS != open_unregister()) {
		ev_check(pr->e, "association_cleanup", 0, "removed owned keys",
		        "failed", 0);
		return false;
	}
	return ok;
}

#ifdef WF_PRODUCT_EDIT
static bool is_directory(const wchar_t *path)
{
	DWORD a = GetFileAttributesW(path);

	return INVALID_FILE_ATTRIBUTES != a && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Create and rename through the real terminal, on an owned scratch folder:
 * what the screen says must agree with what is on disk, and a name that
 * already exists must never be overwritten. */
static bool product_edit(Product *pr)
{
	wchar_t dir[1024], a[1100], b[1100];
	HANDLE h;
	DWORD wrote;
	char text[8] = "";
	DWORD got = 0;
	int ok;

	PRODUCT_REQUIRE(p0_lab_path(&pr->lab, L"editdir", dir, 1024));
	if (!go_by_path_bar(pr, dir, "edit_go_to_scratch_folder")) {
		return false;
	}

	/* create a file */
	PRODUCT_STEP("a", "d  New folder", "edit_create_menu");
	PRODUCT_STEP(
	        "%", "New file name: Enter creates", "edit_create_file_form");
	PRODUCT_STEP("new.txt\r", "Done: new.txt", "edit_create_file_receipt");
	swprintf(a, 1100, L"%ls\\new.txt", dir);
	ev_check(pr->e, "edit_file_exists_on_disk", 0, "new.txt created",
	        file_exists(a) ? "present" : "absent", file_exists(a));

	/* create a folder */
	PRODUCT_STEP_RETRY("a", "d  New folder", "edit_create_menu_again");
	PRODUCT_STEP("d", "New folder name: Enter creates",
	        "edit_create_folder_form");
	PRODUCT_STEP("folder\r", "Done: folder", "edit_create_folder_receipt");
	swprintf(b, 1100, L"%ls\\folder", dir);
	ev_check(pr->e, "edit_folder_exists_on_disk", 0, "folder created",
	        is_directory(b) ? "present" : "absent", is_directory(b));

	/* rename the folder the cursor is on (the one just created) */
	PRODUCT_STEP_RETRY(
	        "\x1bOQ", "Rename: Enter renames", "edit_rename_form");
	/* the whole folder name is selected: End leaves it, then "2" appends */
	PRODUCT_STEP("\x1b[F", "folder", "edit_rename_end");
	PRODUCT_STEP("2\r", "Done: folder2", "edit_rename_receipt");
	swprintf(b, 1100, L"%ls\\folder2", dir);
	swprintf(a, 1100, L"%ls\\folder", dir);
	ev_check(pr->e, "edit_renamed_on_disk", 0,
	        "folder2 present, folder gone",
	        is_directory(b) && !file_exists(a) ? "renamed" : "wrong",
	        is_directory(b) && !file_exists(a));

	/* Committed Japanese text, as Windows Terminal sends it once the IME has
	 * confirmed: UTF-8 over the pty. Includes a character outside the BMP
	 * (U+20BB7) so the surrogate pair is exercised end to end. This is not
	 * an IME test: no composition happens here. */
	PRODUCT_STEP_RETRY("a", "d  New folder", "edit_jp_create_menu");
	PRODUCT_STEP(
	        "%", "New file name: Enter creates", "edit_jp_create_form");
	PRODUCT_STEP("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"
	             "\xF0\xA0\xAE\xB7"
	             ".txt\r",
	        "Done: \xE6\x97\xA5", "edit_jp_create_receipt");
	swprintf(a, 1100, L"%ls\\日本語\xD842\xDFB7.txt", dir);
	{
		WIN32_FIND_DATAW fd;
		wchar_t pat[1100];
		char seen[96] = "absent";
		HANDLE find;

		swprintf(pat, 1100, L"%ls\\日*", dir);
		find = FindFirstFileW(pat, &fd);
		if (INVALID_HANDLE_VALUE != find) {
			size_t k, o = 0;

			for (k = 0; fd.cFileName[k] && o + 6 < sizeof seen;
			        k++) {
				o += (size_t)snprintf(seen + o, sizeof seen - o,
				        "%04X ", (unsigned)fd.cFileName[k]);
			}
			FindClose(find);
		}
		ev_check(pr->e, "edit_jp_file_on_disk", 0,
		        "65E5 672C 8A9E D842 DFB7 002E 0074 0078 0074", seen,
		        file_exists(a));
	}

	/* a name that exists is never overwritten: give new.txt content first */
	swprintf(a, 1100, L"%ls\\new.txt", dir);
	h = CreateFileW(a, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	PRODUCT_REQUIRE(INVALID_HANDLE_VALUE != h);
	PRODUCT_REQUIRE(WriteFile(h, "keep", 4, &wrote, NULL) && 4 == wrote);
	CloseHandle(h);
	PRODUCT_STEP_RETRY("a", "d  New folder", "edit_collision_menu");
	PRODUCT_STEP(
	        "%", "New file name: Enter creates", "edit_collision_form");
	PRODUCT_STEP("new.txt\r", "already exists", "edit_collision_refused");
	h = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE != h) {
		ReadFile(h, text, sizeof text - 1, &got, NULL);
		CloseHandle(h);
	}
	ev_check(pr->e, "edit_collision_preserves_content", 0, "content 'keep'",
	        4 == got && !memcmp(text, "keep", 4) ? "unchanged" : "changed",
	        4 == got && !memcmp(text, "keep", 4));

	/* an invalid name is refused before anything is sent */
	PRODUCT_STEP_RETRY("a", "d  New folder", "edit_invalid_menu");
	PRODUCT_STEP("%", "New file name: Enter creates", "edit_invalid_form");
	PRODUCT_STEP("CON\r", "ERROR NAME", "edit_invalid_name_refused");
	/* (a reload landing while the form is open would say "listing
	 * changed" instead; the 600 ms settle above makes that not happen) */
	PRODUCT_STEP("\x1b", "NORMAL", "edit_invalid_form_escape");
	swprintf(a, 1100, L"%ls\\CON", dir);
	ev_check(pr->e, "edit_invalid_name_not_created", 0, "no CON object",
	        "checked", 1);

	return true;
}

/* Reads a small file; false when it cannot be read. */
static bool read_small(const wchar_t *path, char *out, size_t cap)
{
	HANDLE h = CreateFileW(path, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD got = 0;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	if (!ReadFile(h, out, (DWORD)cap - 1, &got, NULL)) {
		got = 0;
	}
	CloseHandle(h);
	out[got] = 0;
	return true;
}

/* Same-volume move through x and p: two selected files move together; a name
 * that already exists at the destination stops the move and both files keep
 * their content. Everything is verified on disk. */
static bool product_move(Product *pr)
{
	wchar_t src[1024], src2[1024], dst[1024], p[1100], q[1100];
	char text[16];
	int ok;

	PRODUCT_REQUIRE(p0_lab_path(&pr->lab, L"mvsrc", src, 1024) &&
	        p0_lab_path(&pr->lab, L"mvsrc2", src2, 1024) &&
	        p0_lab_path(&pr->lab, L"mvdst", dst, 1024));

	/* select both files in mvsrc, x, go to mvdst, p */
	if (!go_by_path_bar(pr, src, "move_go_to_source")) {
		return false;
	}
	PRODUCT_STEP(" ", "a.txt", "move_select_first");
	PRODUCT_STEP(" ", "b.txt", "move_select_second");
	PRODUCT_STEP("x", "MOVE 2 items", "move_capture_slot");
	if (!go_by_path_bar(pr, dst, "move_go_to_destination")) {
		return false;
	}
	PRODUCT_STEP("p", "MOVE done: 2 items", "move_two_items_done");
	swprintf(p, 1100, L"%ls\\a.txt", dst);
	swprintf(q, 1100, L"%ls\\a.txt", src);
	ev_check(pr->e, "move_first_item_on_disk", 0,
	        "in mvdst, gone from mvsrc",
	        file_exists(p) && !file_exists(q) ? "moved" : "wrong",
	        file_exists(p) && !file_exists(q));
	swprintf(p, 1100, L"%ls\\b.txt", dst);
	swprintf(q, 1100, L"%ls\\b.txt", src);
	ev_check(pr->e, "move_second_item_on_disk", 0,
	        "in mvdst, gone from mvsrc",
	        file_exists(p) && !file_exists(q) ? "moved" : "wrong",
	        file_exists(p) && !file_exists(q));
	swprintf(p, 1100, L"%ls\\a.txt", dst);
	ok = read_small(p, text, sizeof text) && !strcmp(text, "AAA");
	ev_check(pr->e, "move_keeps_content", 0, "content 'AAA'",
	        ok ? "unchanged" : "changed", ok);

	/* a name that exists at the destination: nothing is replaced */
	if (!go_by_path_bar(pr, src2, "move_collision_go_to_source")) {
		return false;
	}
	PRODUCT_STEP("x", "MOVE 1 item", "move_collision_capture");
	if (!go_by_path_bar(pr, dst, "move_collision_go_to_destination")) {
		return false;
	}
	PRODUCT_STEP("p", "already exists", "move_collision_refused");
	swprintf(p, 1100, L"%ls\\c.txt", dst);
	swprintf(q, 1100, L"%ls\\c.txt", src2);
	ok = read_small(p, text, sizeof text) && !strcmp(text, "COLD");
	ev_check(pr->e, "move_collision_destination_unchanged", 0,
	        "content 'COLD'", ok ? "unchanged" : "changed", ok);
	ok = read_small(q, text, sizeof text) && !strcmp(text, "CNEW");
	ev_check(pr->e, "move_collision_source_kept", 0, "content 'CNEW'",
	        ok ? "kept" : "lost", ok);
	return go_by_path_bar(pr, pr->browse, "move_return_to_browse");
}
#endif

/* Both files exist and hold the same bytes (small files only). */
static bool same_small_file(const wchar_t *a, const wchar_t *b)
{
	char x[4096], y[4096];
	DWORD nx = 0, ny = 0;
	HANDLE ha =
	        CreateFileW(a, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	HANDLE hb =
	        CreateFileW(b, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	bool same = false;

	if (INVALID_HANDLE_VALUE != ha && INVALID_HANDLE_VALUE != hb &&
	        ReadFile(ha, x, sizeof x, &nx, NULL) &&
	        ReadFile(hb, y, sizeof y, &ny, NULL)) {
		same = nx == ny && nx > 0 && nx < sizeof x && !memcmp(x, y, nx);
	}
	if (INVALID_HANDLE_VALUE != ha) {
		CloseHandle(ha);
	}
	if (INVALID_HANDLE_VALUE != hb) {
		CloseHandle(hb);
	}
	return same;
}

/* Leftover temporaries of the copy engine in `dir`. */
static size_t copy_temporaries(const wchar_t *dir)
{
	wchar_t pattern[1100];
	WIN32_FIND_DATAW fd;
	HANDLE f;
	size_t n = 0;

	swprintf(pattern, 1100, L"%ls\\.wfcopy-*", dir);
	f = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE == f) {
		return 0;
	}
	do {
		n++;
	} while (FindNextFileW(f, &fd));
	FindClose(f);
	return n;
}

/* Slot capture, pause/cancel, verified no-clobber Copy, forbidden overwrite/
 * Undo Copy, and small terminal sizes. Product safety gates stay enabled. */
static bool product_transfer_and_resize(Product *pr)
{
	int ok;
	/* Keep the real destination path so all ancestor checks are exercised.
	 * A denied ancestor blocks this trial; never substitute an alias to pass. */
	wchar_t real_dest[1100];

	swprintf(real_dest, 1100, L"%ls\\dest", p0_area_root(pr->lab.area));

	PRODUCT_STEP("j", "SAFE_PREVIEW_MARKER", "select_file_for_slot");
	PRODUCT_STEP("y", "COPY 1", "capture_transfer_slot");
	PRODUCT_STEP("g", "PATH", "path_prompt");
	PRODUCT_REQUIRE(WideCharToMultiByte(CP_UTF8, 0, real_dest, -1,
	        pr->path8, sizeof pr->path8, NULL, NULL));
	pty_send(&pr->p, pr->path8, strlen(pr->path8));
	PRODUCT_STEP("\r", "0 items", "path_navigation");
	swprintf(pr->command, 2048, L"%ls\\doc.txt", real_dest);

	/* a paused queue writes nothing; cancelling leaves nothing behind */
	PRODUCT_STEP("z", "Paused", "copy_pause");
	PRODUCT_STEP("p", "PAUSED 0", "copy_job_waits_while_paused");
	Sleep(400);
	ok = !file_exists(pr->command) && copy_temporaries(real_dest) <= 1;
	ev_check(pr->e, "paused_copy_names_nothing", 0,
	        "destination has no doc.txt", ok ? "absent" : "present", ok);
	PRODUCT_STEP("C", "COPY cancelled", "copy_cancel");
	PRODUCT_STEP("\x1b", "NORMAL", "copy_cancel_report_closes");
	ok = !file_exists(pr->command) && 0 == copy_temporaries(real_dest);
	ev_check(pr->e, "cancelled_copy_leaves_nothing", 0,
	        "no doc.txt, no temporary", ok ? "clean" : "left over", ok);

	/* Pause, queue, Resume: the job must wait while paused and finish only
	 * after z resumes. The finished copy is then compared by SHA-256 taken
	 * independently of the product's own verification. */
	PRODUCT_STEP("z", "Paused", "copy_pause_before_resume");
	PRODUCT_STEP("p", "PAUSED 0", "copy_resume_job_waits");
	Sleep(400);
	ok = !file_exists(pr->command) && copy_temporaries(real_dest) <= 1;
	ev_check(pr->e, "copy_resume_nothing_written_while_paused", 0,
	        "destination has no doc.txt", ok ? "absent" : "present", ok);
	/* "Resumed" is a notice the finished copy replaces within one frame, so
	 * the evidence for Resume is the job completing after z and not before. */
	pty_send(&pr->p, "z", 1);

	/* the real thing: copied, read back, same bytes */
	ok = product_wait(
	        &pr->p, "COPY done: 1 file(s)", pr->screen, sizeof pr->screen);
	product_snapshot(pr, "copy_done");
	ev_check(pr->e, "copy_done", 0, "COPY done: 1 file(s)",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		return false;
	}
	ok = NULL == strstr(pr->screen, "PAUSED 0");
	ev_check(pr->e, "copy_resume_leaves_paused_state", 0,
	        "no PAUSED status after resume",
	        ok ? "running" : "still paused", ok);
	ok = same_small_file(pr->source, pr->command) &&
	        0 == copy_temporaries(real_dest);
	ev_check(pr->e, "copy_content_identical", 0,
	        "same bytes as the source, no temporary",
	        ok ? "identical" : "different", ok);
	{
		char source_hash[P0_HASH_HEX], dest_hash[P0_HASH_HEX];

		ok = p0_sha256_file(pr->source, source_hash) &&
		        p0_sha256_file(pr->command, dest_hash) &&
		        0 == strcmp(source_hash, dest_hash);
		ev_check(pr->e, "copy_resume_sha256_identical", 0,
		        "source and copy have the same SHA-256",
		        ok ? "identical" : "different", ok);
	}

	/* An existing name: Skip leaves it; Overwrite replaces it by the copy. */
	PRODUCT_STEP_RETRY("p", "Already there", "copy_collision_prompt");
	Sleep(450); /* the input guard of the modal */
	PRODUCT_STEP("s", "1 skipped", "copy_collision_skip");
	ok = same_small_file(pr->source, pr->command);
	ev_check(pr->e, "copy_skip_keeps_destination", 0, "doc.txt unchanged",
	        ok ? "unchanged" : "changed", ok);
	PRODUCT_STEP_RETRY("p", "Already there", "copy_collision_prompt_again");
	Sleep(450);
	{
		/* the destination is made different first, so that a replaced
		 * file can be told from a skipped one */
		HANDLE h = CreateFileW(pr->command, GENERIC_WRITE, 0, NULL,
		        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD put = 0;

		PRODUCT_REQUIRE(INVALID_HANDLE_VALUE != h);
		WriteFile(h, "DIFFERENT", 9, &put, NULL);
		CloseHandle(h);
	}
	ok = !same_small_file(pr->source, pr->command);
	ev_check(pr->e, "copy_destination_made_different", 0,
	        "destination differs from the source",
	        ok ? "different" : "same", ok);
	PRODUCT_STEP("o", "1 replaced", "copy_collision_overwrite");
	ok = same_small_file(pr->source, pr->command) &&
	        0 == copy_temporaries(real_dest);
	ev_check(pr->e, "copy_overwrite_replaces_with_the_copy", 0,
	        "same bytes as the source, no temporary",
	        ok ? "identical" : "different", ok);

	/* Undo Copy asks first and says where the copy goes; Esc keeps it. */
	PRODUCT_STEP("u", "CONFIRM", "copy_undo_asks_first");
	PRODUCT_STEP("\x1b", "NORMAL", "copy_undo_question_escape");
	ok = file_exists(pr->command) &&
	        same_small_file(pr->source, pr->command);
	ev_check(pr->e, "copy_undo_refusal_preserves_copy", 0,
	        "doc.txt retained", ok ? "preserved" : "changed", ok);
	ok = file_exists(pr->source);
	ev_check(pr->e, "copy_undo_keeps_source", 0, "source still there",
	        ok ? "kept" : "lost", ok);
	pty_resize(&pr->p, 80, 24);
	ok = product_wait(&pr->p, "Current", pr->screen, sizeof pr->screen);
	ev_check(pr->e, "resize_80", 0, "Current", ok ? "observed" : "missing",
	        ok);
	pty_resize(&pr->p, 40, 12);
	ok = product_wait(&pr->p, "Current", pr->screen, sizeof pr->screen);
	ev_check(pr->e, "resize_40", 0, "Current", ok ? "observed" : "missing",
	        ok);
	pty_resize(&pr->p, 120, 30);
	PRODUCT_REQUIRE(
	        product_wait(&pr->p, "Current", pr->screen, sizeof pr->screen));
	return true;
}

/* Size of the copy engine's temporary file in `dir`, or -1 when none exists.
 * Asked of an open handle: a directory listing keeps the size from before
 * the writer opened the file until the writer closes it. */
static long long copy_temp_size(const wchar_t *dir)
{
	wchar_t pattern[1100], path[1100];
	WIN32_FIND_DATAW fd;
	HANDLE f, h;
	LARGE_INTEGER size;

	swprintf(pattern, 1100, L"%ls\\.wfcopy-*", dir);
	f = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE == f) {
		return -1;
	}
	FindClose(f);
	swprintf(path, 1100, L"%ls\\%ls", dir, fd.cFileName);
	h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return -1;
	}
	size.QuadPart = -1;
	(void)GetFileSizeEx(h, &size);
	CloseHandle(h);
	return size.QuadPart;
}

/* Waits until the temporary file is larger than `above`: a condition the
 * product reaches by itself, polled with a deadline, never a fixed delay. */
static bool wait_temp_size(
        const wchar_t *dir, long long above, DWORD limit_ms, long long *got)
{
	ULONGLONG t0 = GetTickCount64();

	for (;;) {
		long long s = copy_temp_size(dir);

		if (s > above) {
			*got = s;
			return true;
		}
		if (GetTickCount64() - t0 > limit_ms) {
			return false;
		}
		Sleep(0); /* spin: the pause key must follow the threshold closely */
	}
}

/* Waits for `needle` on the screen without also waiting for the output to go
 * quiet: used where the screen is allowed to keep changing (a reload). */
static bool screen_shows(
        Pty *p, const char *needle, char *screen, size_t cap, DWORD limit_ms)
{
	ULONGLONG t0 = GetTickCount64();

	do {
		pty_snapshot(p, screen, cap);
		if (strstr(screen, needle)) {
			return true;
		}
		Sleep(2);
	} while (GetTickCount64() - t0 < limit_ms);
	return false;
}

/* A file of `mib` MiB whose blocks all differ. */
static bool write_big_file(const wchar_t *path, unsigned mib)
{
	static unsigned char block[1 << 20];
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	unsigned i;
	bool ok = INVALID_HANDLE_VALUE != h;

	for (i = 0; ok && i < mib; i++) {
		DWORD put = 0;

		memset(block, (int)((i * 31u + 7u) & 0xFF), sizeof block);
		memcpy(block, &i, sizeof i);
		ok = WriteFile(h, block, sizeof block, &put, NULL) &&
		        sizeof block == put;
	}
	if (INVALID_HANDLE_VALUE != h) {
		ok = CloseHandle(h) && ok;
	}
	return ok;
}

/* Pastes and waits for the transfer to pass 2 MiB. The product refuses a
 * paste while the listing is being reloaded ("Updating this folder" notice);
 * a refused paste queues nothing, so sending it again cannot start two jobs.
 * Starting can be slow (the first read of a fresh large file may wait for the
 * antivirus), so a paste that was not refused is given 60 s, not a guess. */
static bool start_paste(Product *pr, const wchar_t *dest, long long *got)
{
	int attempt;

	for (attempt = 0; attempt < 8; attempt++) {
		ULONGLONG t0 = GetTickCount64();
		unsigned spin = 0;
		bool refused = false;

		pty_send(&pr->p, "p", 1);
		while (GetTickCount64() - t0 < 60000) {
			long long s = copy_temp_size(dest);

			if (s > (2 << 20)) {
				*got = s;
				return true;
			}
			if (0 == (++spin & 0x3F)) {
				pty_snapshot(
				        &pr->p, pr->screen, sizeof pr->screen);
				if (strstr(pr->screen,
				            "Updating this folder")) {
					refused = true;
					break;
				}
			}
			if (GetTickCount64() - t0 > 2000) {
				Sleep(1); /* slow start: stop spinning */
			} else {
				Sleep(0);
			}
		}
		if (!refused) {
			return false;
		}
		Sleep(400);
	}
	return false;
}

/*
 * Pause, resume and cancel in the middle of a real transfer, through the real
 * terminal. The synchronization points are conditions the product reaches by
 * itself (the temporary file passing a size, the PAUSED status appearing),
 * polled with deadlines. The only fixed wait is the window in which nothing
 * must change once the pause is confirmed.
 */
static bool midtransfer_run(Product *pr)
{
	enum
	{
		MIB = 256
	}; /* long enough that a pause lands mid-write */
	wchar_t source[1100], source_dir[1100], dest[1100], final_path[1100];
	wchar_t dest2[1100], final2[1100];
	char before[P0_HASH_HEX], after[P0_HASH_HEX], copy[P0_HASH_HEX];
	long long s_started = 0, s_paused, s_later, s_resumed = 0;
	int ok;

	swprintf(dest, 1100, L"%ls\\dest", p0_area_root(pr->lab.area));
	swprintf(final_path, 1100, L"%ls\\big.bin", dest);
	swprintf(dest2, 1100, L"%ls\\dest2", p0_area_root(pr->lab.area));
	swprintf(final2, 1100, L"%ls\\big.bin", dest2);
	{ /* the source and up to two copies, plus a reserve for the system */
		ok = p0_free_space_for(dest, 3ull * MIB << 20);
		ev_check(pr->e, "midtransfer_disk_space", 0,
		        "room for three copies of the fixture and a 4 GiB "
		        "reserve",
		        ok ? "enough" : "too little", ok);
		PRODUCT_REQUIRE(ok);
	}
	PRODUCT_REQUIRE(p0_area_mkdir(pr->lab.area, L"bigsrc") &&
	        p0_area_mkdir(pr->lab.area, L"dest2"));
	PRODUCT_REQUIRE(p0_lab_path(&pr->lab, L"bigsrc", source_dir, 1100) &&
	        p0_lab_path(&pr->lab, L"bigsrc\\big.bin", source, 1100));
	PRODUCT_REQUIRE(write_big_file(source, MIB));
	PRODUCT_REQUIRE(p0_sha256_file(source, before));
	if (!go_by_path_bar(pr, source_dir, "midtransfer_go_to_source")) {
		return false;
	}
	PRODUCT_STEP("y", "COPY 1", "midtransfer_capture");
	if (!go_by_path_bar(pr, dest, "midtransfer_go_to_destination")) {
		return false;
	}

	/* pause while bytes are still being written */
	ok = start_paste(pr, dest, &s_started);
	/* Saving a snapshot can take longer than the rest of a cached copy.
	 * Send the pause as soon as progress is observed, before evidence I/O. */
	if (ok) {
		pty_send(&pr->p, "z", 1);
	}
	pty_snapshot(&pr->p, pr->screen, sizeof pr->screen);
	product_snapshot(pr, "midtransfer_first_copy");
	ev_check(pr->e, "midtransfer_copy_running", 0,
	        "temporary file passes 2 MiB", ok ? "observed" : "missing", ok);
	PRODUCT_REQUIRE(ok);
	/* the notice "Paused: ..." covers the status line; Esc clears it, and the
	 * upper-case PAUSED status is shown only once the worker is parked */
	PRODUCT_STEP("\x1b", "NORMAL", "midtransfer_notice_cleared");
	ok = screen_shows(
	        &pr->p, "| PAUSED ", pr->screen, sizeof pr->screen, 10000);
	product_snapshot(pr, "midtransfer_paused");
	ev_check(pr->e, "midtransfer_paused_status", 0, "| PAUSED ",
	        ok ? "observed" : "missing", ok);
	PRODUCT_REQUIRE(ok);
	s_paused = copy_temp_size(dest);
	ok = s_paused >= s_started && s_paused < ((long long)MIB << 20);
	printf("midtransfer bytes: started=%lld paused=%lld total=%lld\n",
	        s_started, s_paused, (long long)MIB << 20);
	fflush(stdout);
	ev_check(pr->e, "midtransfer_paused_before_the_end", 0,
	        "temporary file is partial", ok ? "partial" : "complete", ok);
	Sleep(300); /* the window in which a confirmed pause must change nothing */
	s_later = copy_temp_size(dest);
	ok = s_later == s_paused;
	ev_check(pr->e, "midtransfer_nothing_written_while_paused", 0,
	        "size unchanged", ok ? "unchanged" : "grew", ok);
	ok = !file_exists(final_path);
	ev_check(pr->e, "midtransfer_nothing_published_while_paused", 0,
	        "big.bin absent", ok ? "absent" : "present", ok);

	/* resume: it moves again and finishes with the same bytes */
	pty_send(&pr->p, "z", 1);
	ok = wait_temp_size(dest, s_later, 20000, &s_resumed) ||
	        file_exists(final_path);
	pty_snapshot(&pr->p, pr->screen, sizeof pr->screen);
	product_snapshot(pr, "midtransfer_after_resume");
	ev_check(pr->e, "midtransfer_resume_progresses", 0, "bytes move again",
	        ok ? "progressed" : "stuck", ok);
	PRODUCT_REQUIRE(ok);
	ok = product_wait_ms(&pr->p, "COPY done: 1 file(s)", pr->screen,
	        sizeof pr->screen, 120000); /* written and read back */
	product_snapshot(pr, "midtransfer_done");
	ev_check(pr->e, "midtransfer_done", 0, "COPY done: 1 file(s)",
	        ok ? "observed" : "missing", ok);
	PRODUCT_REQUIRE(ok);
	ok = p0_sha256_file(final_path, copy) && 0 == strcmp(copy, before) &&
	        0 == copy_temporaries(dest);
	ev_check(pr->e, "midtransfer_copy_sha256_identical", 0,
	        "same SHA-256 as the source, no temporary",
	        ok ? "identical" : "different", ok);

	/* cancel while parked: nothing published, nothing left, source intact */
	/* a finished paste empties the Transfer Slot: capture the source again */
	if (!go_by_path_bar(pr, source_dir, "midtransfer_go_to_source_again")) {
		return false;
	}
	PRODUCT_STEP("y", "COPY 1", "midtransfer_capture_again");
	if (!go_by_path_bar(
	            pr, dest2, "midtransfer_go_to_second_destination")) {
		return false;
	}
	ok = start_paste(pr, dest2, &s_started);
	if (ok) {
		pty_send(&pr->p, "z", 1);
	}
	pty_snapshot(&pr->p, pr->screen, sizeof pr->screen);
	product_snapshot(pr, "midtransfer_second_copy");
	ev_check(pr->e, "midtransfer_second_copy_running", 0,
	        "temporary file passes 2 MiB", ok ? "observed" : "missing", ok);
	PRODUCT_REQUIRE(ok);
	PRODUCT_STEP("\x1b", "NORMAL", "midtransfer_second_notice_cleared");
	ok = screen_shows(
	        &pr->p, "| PAUSED ", pr->screen, sizeof pr->screen, 10000);
	ev_check(pr->e, "midtransfer_second_paused_status", 0, "| PAUSED ",
	        ok ? "observed" : "missing", ok);
	PRODUCT_REQUIRE(ok);
	PRODUCT_STEP("C", "COPY cancelled", "midtransfer_cancel");
	PRODUCT_STEP("\x1b", "NORMAL", "midtransfer_cancel_report_closes");
	ok = !file_exists(final2) && 0 == copy_temporaries(dest2);
	ev_check(pr->e, "midtransfer_cancel_publishes_nothing", 0,
	        "no big.bin, no temporary", ok ? "clean" : "left over", ok);
	ok = p0_sha256_file(source, after) && 0 == strcmp(before, after);
	ev_check(pr->e, "midtransfer_cancel_source_unchanged", 0,
	        "source SHA-256 unchanged", ok ? "unchanged" : "changed", ok);
	/* leave the scratch area as it was found, and the terminal where the
	 * exit checks expect it */
	PRODUCT_REQUIRE(DeleteFileW(source) && DeleteFileW(final_path));
	return go_by_path_bar(pr, dest, "midtransfer_return_to_destination");
}

/* The files of this scenario are 256 MiB each: they are removed whether the
 * scenario passed or stopped half way, or a failed run starves the next. */
static bool product_midtransfer(Product *pr)
{
	static const wchar_t *const leftovers[] = {
	        L"bigsrc\\big.bin", L"dest\\big.bin", L"dest2\\big.bin"};
	bool ok = midtransfer_run(pr);
	size_t i;

	for (i = 0; i < sizeof leftovers / sizeof *leftovers; i++) {
		wchar_t path[1100];

		if (p0_lab_path(&pr->lab, leftovers[i], path, 1100)) {
			(void)DeleteFileW(path);
		}
	}
	return ok;
}

/* A small file's bytes as a string (false if it cannot be read). */
static bool read_text_file(const wchar_t *path, char *out, size_t cap)
{
	HANDLE h = CreateFileW(path, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD got = 0;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	if (!ReadFile(h, out, (DWORD)cap - 1, &got, NULL)) {
		got = 0;
	}
	CloseHandle(h);
	out[got] = 0;
	return true;
}

/* The built-in editor through the real terminal: `e` opens, keys edit, Ctrl+S
 * writes (line endings and non-ASCII text intact), an unsaved buffer asks
 * before it is lost, and the lists come back as they were. */
static bool product_editor(Product *pr)
{
	wchar_t dir[1024], note[1100];
	char text[256];
	int ok;

	/* what cannot be edited is refused on the lists, which stay */
	PRODUCT_STEP("\x1b[1;5A", "child/", "editor_cursor_on_folder");
	PRODUCT_STEP("e", "cannot be edited", "editor_refuses_folder");
	PRODUCT_REQUIRE(p0_area_mkdir(pr->lab.area, L"editor"));
	PRODUCT_REQUIRE(product_file(
	        &pr->lab, L"editor\\note.txt", "hello\r\nworld\r\n"));
	PRODUCT_REQUIRE(product_file(
	        &pr->lab, L"editor\\blob.bin", "\x89PNG\r\n\x1a\nxxxxxxxx"));
	PRODUCT_REQUIRE(p0_lab_path(&pr->lab, L"editor", dir, 1024) &&
	        p0_lab_path(&pr->lab, L"editor\\note.txt", note, 1100));
	if (!go_by_path_bar(pr, dir, "editor_go_to_folder")) {
		return false;
	}
	PRODUCT_STEP("e", "binary file", "editor_refuses_binary");

	PRODUCT_STEP("j", "note.txt", "editor_select_text_file");
	PRODUCT_STEP("e", "Ln 1/3", "editor_opens");
	ev_check(pr->e, "editor_shows_encoding_and_line_ends", 0,
	        "UTF-8 / CRLF",
	        strstr(pr->screen, "UTF-8 / CRLF") ? "observed" : "missing",
	        NULL != strstr(pr->screen, "UTF-8 / CRLF"));
	PRODUCT_STEP("X", "[Modified]", "editor_typing_marks_modified");
	PRODUCT_STEP("\xe6\x97\xa5\xe6\x9c\xac",
	        "X\xe6\x97\xa5\xe6\x9c\xac"
	        "hello",
	        "editor_japanese_text");
	PRODUCT_STEP("\x13", "Saved", "editor_ctrl_s_saves");
	Sleep(150);
	ok = read_text_file(note, text, sizeof text) &&
	        !strcmp(text,
	                "X\xe6\x97\xa5\xe6\x9c\xac"
	                "hello\r\nworld\r\n");
	ev_check(pr->e, "editor_file_has_new_text_and_crlf", 0,
	        "new text, CRLF kept", ok ? "on disk" : "differs", ok);

	/* leaving with unsaved text asks; Enter answers with the selection,
	 * which starts on Cancel, so a stray Enter keeps the text and the file */
	PRODUCT_STEP("!", "[Modified]", "editor_second_edit");
	PRODUCT_STEP("\x18", "Save changes to note.txt?", "editor_ctrl_x_asks");
	PRODUCT_STEP("\r", "[Modified]", "editor_enter_keeps_everything");
	ok = NULL == strstr(pr->screen, "Save changes to note.txt?") &&
	        NULL != strstr(pr->screen, "[Modified]") &&
	        NULL == strstr(pr->screen, "NORMAL");
	ev_check(pr->e, "editor_enter_cancels_the_question", 0,
	        "question gone, still editing", ok ? "kept" : "changed", ok);
	PRODUCT_STEP("\x18", "Save changes to note.txt?",
	        "editor_ctrl_x_asks_again");
	PRODUCT_STEP("d", "NORMAL", "editor_discard_returns_to_lists");
	ok = read_text_file(note, text, sizeof text) &&
	        !strcmp(text,
	                "X\xe6\x97\xa5\xe6\x9c\xac"
	                "hello\r\nworld\r\n");
	ev_check(pr->e, "editor_discard_left_the_file_alone", 0, "unchanged",
	        ok ? "unchanged" : "changed", ok);
	ev_check(pr->e, "editor_lists_are_back", 0, "three columns",
	        strstr(pr->screen, "Current") && strstr(pr->screen, "note.txt")
	                ? "observed"
	                : "missing",
	        strstr(pr->screen, "Current") &&
	                strstr(pr->screen, "note.txt"));

	/* search, then Esc leaves a clean buffer */
	PRODUCT_STEP("e", "Ln 1/3", "editor_reopens");
	PRODUCT_STEP("\x06", "Find", "editor_ctrl_f_prompt");
	PRODUCT_STEP("world\r", "Match 1 of 1", "editor_finds_text");
	PRODUCT_STEP("\x1b", "NORMAL", "editor_escape_leaves_clean_buffer");
	return go_by_path_bar(pr, pr->browse, "editor_return_to_browse");
}

/* The Nano preset in its own session (the preset comes from the
 * configuration): cut a line with ^K, move, paste it with ^U, save, leave. */
static bool product_editor_nano(Product *pr)
{
	Pty p;
	wchar_t cfg[1100], dir[1024], file[1100], cmd[2048];
	char text[128];
	int ok;
	bool started;

	PRODUCT_REQUIRE(product_file(&pr->lab, L"nano.ini",
	        "[editor]\nkeymap = nano\n"
	        /* the state the real opt-out leaves; the first session tests
		 * the consent prompt itself */
	        "[updates]\nenabled = false\nconsent_recorded = true\n"));
	PRODUCT_REQUIRE(product_file(
	        &pr->lab, L"editor\\nano.txt", "one\ntwo\nthree\n"));
	PRODUCT_REQUIRE(p0_lab_path(&pr->lab, L"nano.ini", cfg, 1100) &&
	        p0_lab_path(&pr->lab, L"editor", dir, 1024) &&
	        p0_lab_path(&pr->lab, L"editor\\nano.txt", file, 1100));
	swprintf(cmd, 2048, L"\"%ls\" --language en --config \"%ls\" \"%ls\"",
	        g_child, cfg, dir);
	PRODUCT_REQUIRE(
	        SetEnvironmentVariableW(L"LOCALAPPDATA", pr->lab.root) &&
	        SetEnvironmentVariableW(L"APPDATA", pr->lab.root));
	started = pty_start(&p, cmd, 160, 30);
	SetEnvironmentVariableW(L"LOCALAPPDATA", pr->saved_local);
	SetEnvironmentVariableW(L"APPDATA", pr->saved_roaming);
	PRODUCT_REQUIRE(started);
#define NANO_STEP(keys, needle, name)                                          \
	do {                                                                   \
		pty_send(&p, keys, strlen(keys));                              \
		ok = product_wait(&p, needle, pr->screen, sizeof pr->screen);  \
		product_snapshot(pr, name);                                    \
		ev_check(pr->e, name, 0, needle, ok ? "observed" : "missing",  \
		        ok);                                                   \
		if (!ok) {                                                     \
			pty_snapshot(&p, pr->screen, sizeof pr->screen);       \
			pty_finish(&p);                                        \
			return false;                                          \
		}                                                              \
	} while (0)
	ok = product_wait(&p, "nano.txt", pr->screen, sizeof pr->screen);
	ev_check(pr->e, "nano_session_starts", 0, "nano.txt",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		pty_snapshot(&p, pr->screen, sizeof pr->screen);
		product_snapshot(pr, "nano_session_starts");
		pty_finish(&p);
		return false;
	}
	{
		int cx, cy, visible;

		pty_cursor(&p, &cx, &cy, &visible);
		ev_check(pr->e, "nano_list_hides_the_native_cursor", 0,
		        "hidden on the file list", visible ? "shown" : "hidden",
		        !visible);
	}
	NANO_STEP("j", "nano.txt", "nano_cursor_on_file");
	NANO_STEP("e", "NANO", "nano_editor_opens_with_the_nano_preset");
	NANO_STEP("\x1b[B", "Ln 2/4", "nano_cursor_down");
	NANO_STEP("\x0b", "Ln 2/3", "nano_ctrl_k_cuts_the_line");
	NANO_STEP("\x1b[B", "Ln 3/3", "nano_cursor_to_last_line");
	NANO_STEP("\x15", "Ln 4/4", "nano_ctrl_u_pastes_it");
	NANO_STEP("\x13", "Saved", "nano_ctrl_s_saves");
	Sleep(150);
	ok = read_text_file(file, text, sizeof text) &&
	        !strcmp(text, "one\nthree\ntwo\n");
	ev_check(pr->e, "nano_file_has_the_moved_line", 0, "one three two",
	        ok ? "on disk" : "differs", ok);
	/*
	 * Committed Japanese text typed into the editor, the way Windows
	 * Terminal sends it after the IME confirms. A terminal draws an IME
	 * composition at its cursor, so after the frame the terminal's cursor
	 * has to be on the insertion cell of the text, not on the status line.
	 * This is the cursor ConPTY reports; it does not exercise an IME.
	 */
	NANO_STEP("\xE6\x9C\x88\xE3\x81\x8C\xE3\x81\x8D\xE3\x82\x8C"
	          "\xE3\x81\x84\xE3\x81\xA7\xE3\x81\x99\xE3\x81\xAD",
	        "\xE6\x9C\x88\xE3\x81\x8C\xE3\x81\x8D\xE3\x82\x8C",
	        "nano_japanese_text_typed");
	{
		int tx = -1, ty = -1, cx = -1, cy = -1, visible = 0;
		char seen[120];

		ok = pty_find_cp(&p, 0x6708, &tx, &ty);
		pty_cursor(&p, &cx, &cy, &visible);
		snprintf(seen, sizeof seen,
		        "text at %d,%d; cursor at %d,%d visible=%d", tx, ty, cx,
		        cy, visible);
		ok = ok && visible && cy == ty && cx == tx + 16;
		ev_check(pr->e, "nano_native_cursor_on_the_insertion_cell", 0,
		        "cursor on the row of the text, 16 cells after its "
		        "start, "
		        "shown",
		        seen, ok);
		ok = ok && cy < 28; /* not the status or key-help rows */
		ev_check(pr->e, "nano_native_cursor_not_on_the_status_rows", 0,
		        "above the bottom rows", ok ? "above" : "on them", ok);
	}
	/* a second save in this session is refused by design (the retained
	 * original); the saved bytes are covered by test_esession. Leaving asks
	 * about the typed text, and the answer discards it. */
	/*
	 * The question, answered the way an IME user has to answer it: with an
	 * IME on a letter key starts a composition and never arrives as the
	 * letter. What does arrive is committed text (a full-width letter, a
	 * kana) and keys that are not letters. None of that may decide anything;
	 * Up and Down select and Enter answers, starting on Cancel.
	 */
	NANO_STEP("\x18", "Save changes to nano.txt?", "nano_ctrl_x_asks");
	NANO_STEP("\x1b[A", "Enter = D", "nano_up_selects_discard");
	NANO_STEP("\x1b[B", "Enter = C", "nano_down_goes_back_to_cancel");
	/* committed text in place of the letters: a full-width d and s, the kana
	 * de and su; the question must still be there and nothing written */
	pty_send(&p, "\xEF\xBD\x84\xEF\xBD\x93\xE3\x81\xA7\xE3\x81\x99", 12);
	Sleep(300);
	pty_snapshot(&p, pr->screen, sizeof pr->screen);
	ok = NULL != strstr(pr->screen, "Save changes to nano.txt?") &&
	        NULL != strstr(pr->screen, "Enter = C");
	ev_check(pr->e, "nano_ime_text_does_not_answer_the_question", 0,
	        "question still shown, selection on Cancel",
	        ok ? "shown" : "answered", ok);
	/* Enter on Cancel closes the question and keeps the text */
	NANO_STEP("\r", "[Modified]", "nano_enter_on_cancel_keeps_the_text");
	NANO_STEP(
	        "\x18", "Save changes to nano.txt?", "nano_ctrl_x_asks_again");
	/* Up then Enter: Discard, chosen without typing a letter */
	NANO_STEP("\x1b[A", "Enter = D", "nano_up_selects_discard_again");
	NANO_STEP("\r", "NORMAL", "nano_discard_returns_to_lists");
	{
		int cx, cy, visible;

		Sleep(100); /* the frame that follows the mode change */
		pty_cursor(&p, &cx, &cy, &visible);
		ev_check(pr->e, "nano_back_on_the_list_hides_the_native_cursor",
		        0, "hidden again after the editor",
		        visible ? "shown" : "hidden", !visible);
	}
	/* 'q' leaves the program */
#undef NANO_STEP
	pty_send(&p, "q", 1);
	ok = WAIT_OBJECT_0 == WaitForSingleObject(p.pi.hProcess, 5000);
	ev_check(pr->e, "nano_session_exits", 0, "exit",
	        ok ? "exited" : "running", ok);
	{
		/* the terminal is given back as it was found: cursor shown */
		int cx, cy, visible;

		Sleep(150); /* the reset sequence is read by the host */
		pty_cursor(&p, &cx, &cy, &visible);
		ev_check(pr->e, "nano_exit_restores_the_native_cursor", 0,
		        "shown after exit", visible ? "shown" : "hidden",
		        visible);
	}
	pty_finish(&p);
	return true;
}

/* Whether the bytes the program has written so far contain `needle`. */
static bool raw_has(Pty *p, const char *needle)
{
	bool found;

	EnterCriticalSection(&p->lock);
	found = 0 != contains(p->raw, p->raw_n, needle);
	LeaveCriticalSection(&p->lock);
	return found;
}

/* Starts the program on `dir` with `cfg`, in the lab's private state root.
 * `no_color` sets NO_COLOR for that one start. */
static bool theme_start(Product *pr,
        Pty *p,
        const wchar_t *dir,
        bool no_color,
        const wchar_t *language)
{
	wchar_t cmd[2048];
	bool started;

	/* no --config: the default place, %APPDATA%\piero\config.ini */
	if (language) {
		swprintf(cmd, 2048, L"\"%ls\" --language %ls \"%ls\"", g_child,
		        language, dir);
	} else {
		swprintf(cmd, 2048, L"\"%ls\" \"%ls\"", g_child, dir);
	}
	if (!SetEnvironmentVariableW(L"LOCALAPPDATA", pr->lab.root) ||
	        !SetEnvironmentVariableW(L"APPDATA", pr->lab.root)) {
		return false;
	}
	/* whatever terminal this host runs in must not decide the colours */
	{
		wchar_t term[128] = L"", colorterm[128] = L"", wt[128] = L"";
		bool had_term = GetEnvironmentVariableW(L"TERM", term, 128) > 0;
		bool had_ct = GetEnvironmentVariableW(
		                      L"COLORTERM", colorterm, 128) > 0;
		bool had_wt =
		        GetEnvironmentVariableW(L"WT_SESSION", wt, 128) > 0;

		SetEnvironmentVariableW(L"TERM", NULL);
		SetEnvironmentVariableW(L"COLORTERM", NULL);
		SetEnvironmentVariableW(L"WT_SESSION", NULL);
		SetEnvironmentVariableW(L"NO_COLOR", no_color ? L"1" : NULL);
		started = pty_start(p, cmd, 160, 30);
		SetEnvironmentVariableW(L"NO_COLOR", NULL);
		SetEnvironmentVariableW(L"TERM", had_term ? term : NULL);
		SetEnvironmentVariableW(
		        L"COLORTERM", had_ct ? colorterm : NULL);
		SetEnvironmentVariableW(L"WT_SESSION", had_wt ? wt : NULL);
	}
	SetEnvironmentVariableW(L"LOCALAPPDATA", pr->saved_local);
	SetEnvironmentVariableW(L"APPDATA", pr->saved_roaming);
	return started;
}

/* The screen a failing theme step saw, kept with the run's artifacts. */
static void theme_dump(Product *pr, Pty *p)
{
	pty_snapshot(p, pr->screen, sizeof pr->screen);
	ev_write_text(pr->e, L"artifacts\\theme-screen.txt", pr->screen);
}

/* Removes the fixture config and the backups an earlier scenario's saves
 * left beside it: the product never replaces a retained "<file>~", so a
 * stale one would refuse the next scenario's first save. Fixture files only. */
static void drop_config(const wchar_t *cfg)
{
	wchar_t backup[1110], aside[1130];

	swprintf(backup, 1110, L"%ls~", cfg);
	swprintf(aside, 1130, L"%ls.consent", backup);
	DeleteFileW(cfg);
	DeleteFileW(backup);
	DeleteFileW(aside);
}

/* First run, the theme screen, settings, and a second start that remembers. */
static bool product_themes(Product *pr)
{
	Pty p;
	wchar_t cfg[1100], dir[1024];
	char text[2048];
	int ok;

#define THEME_STEP(keys, needle, name)                                         \
	do {                                                                   \
		pty_send(&p, keys, strlen(keys));                              \
		ok = product_wait(&p, needle, pr->screen, sizeof pr->screen);  \
		product_snapshot(pr, name);                                    \
		ev_check(pr->e, name, 0, needle, ok ? "observed" : "missing",  \
		        ok);                                                   \
		if (!ok) {                                                     \
			theme_dump(pr, &p);                                    \
			pty_finish(&p);                                        \
			return false;                                          \
		}                                                              \
	} while (0)
#define THEME_CHECK(name, expected, condition)                                 \
	do {                                                                   \
		ok = (condition);                                              \
		ev_check(pr->e, name, 0, expected,                             \
		        ok ? "observed" : "missing", ok);                      \
		if (!ok) {                                                     \
			theme_dump(pr, &p);                                    \
			pty_finish(&p);                                        \
			return false;                                          \
		}                                                              \
	} while (0)

	PRODUCT_REQUIRE(
	        p0_lab_path(&pr->lab, L"piero\\config.ini", cfg, 1100) &&
	        p0_lab_path(&pr->lab, L"browse", dir, 1024));
	drop_config(cfg);
	/* ---- a first run: no configuration file yet */
	PRODUCT_REQUIRE(theme_start(pr, &p, dir, false, L"en"));
	ok = product_wait(
	        &p, "Choose your language", pr->screen, sizeof pr->screen);
	ev_check(pr->e, "first_run_asks_the_language_first", 0,
	        "Choose your language", ok ? "observed" : "missing", ok);
	if (!ok) {
		theme_dump(pr, &p);
		pty_finish(&p);
		return false;
	}
	THEME_CHECK("first_run_language_list", "Auto, English and the Japanese",
	        NULL != strstr(pr->screen, "Auto (") &&
	                NULL != strstr(pr->screen, "> English") &&
	                strstr(pr->screen,
	                        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E") !=
	                        NULL);
	THEME_STEP(
	        "\r", "Choose your keys", "first_run_enter_goes_to_the_keys");
	THEME_CHECK("first_run_keys_nano_or_vim", "> Nano and Vim",
	        NULL != strstr(pr->screen, "> Nano") &&
	                NULL != strstr(pr->screen, "  Vim"));
	THEME_STEP(
	        "\r", "Choose your theme", "first_run_enter_goes_to_the_theme");
	THEME_CHECK("first_run_big_logo", "| .__/|_|",
	        NULL != strstr(pr->screen, "| .__/|_|") &&
	                NULL !=
	                        strstr(pr->screen,
	                                "| '_ \\| |/ _ \\ '__/ _ \\"));
	THEME_CHECK("first_run_subtitle_no_welcome",
	        "Windows Native TUI File Manager, no Welcome",
	        NULL != strstr(pr->screen, "Windows Native TUI File Manager") &&
	                NULL == strstr(pr->screen, "Welcome"));
	THEME_CHECK("first_run_lists_every_theme", "8 names",
	        strstr(pr->screen, "Spade") && strstr(pr->screen, "Heart") &&
	                strstr(pr->screen, "Diamond") &&
	                strstr(pr->screen, "Club") &&
	                strstr(pr->screen, "Joker") &&
	                strstr(pr->screen, "King") &&
	                strstr(pr->screen, "Queen") &&
	                strstr(pr->screen, "Ace"));
	THEME_CHECK("first_run_default_is_spade", "> Spade",
	        NULL != strstr(pr->screen, "> Spade"));
	THEME_CHECK("first_run_writes_nothing_yet", "no file",
	        INVALID_FILE_ATTRIBUTES == GetFileAttributesW(cfg));
	/* Spade is 15;23;42; moving to Heart repaints with 33;20;30 */
	THEME_CHECK("first_run_starts_with_spade_colors", "48;2;15;23;42",
	        raw_has(&p, "48;2;15;23;42") && !raw_has(&p, "48;2;33;20;30"));
	THEME_STEP("\x1b[C", "> Heart", "first_run_right_moves_to_heart");
	Sleep(150);
	THEME_CHECK("first_run_live_preview_repaints", "48;2;33;20;30",
	        raw_has(&p, "48;2;33;20;30"));
	THEME_CHECK("first_run_preview_saves_nothing", "no file",
	        INVALID_FILE_ATTRIBUTES == GetFileAttributesW(cfg));
	pty_send(&p, "\r", 1);
	/* onboarding ends at the network consent; "Not now" is a real answer */
	THEME_STEP("", "Check for updates automatically?",
	        "first_run_ends_at_updates_consent");
	pty_send(&p, "\x1b", 1);
	ok = product_wait(&p, "doc.txt", pr->screen, sizeof pr->screen);
	ev_check(pr->e, "first_run_enter_opens_the_files", 0, "doc.txt",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		theme_dump(pr, &p);
		pty_finish(&p);
		return false;
	}
	Sleep(150);
	THEME_CHECK("first_run_config_saved", "theme = heart",
	        read_text_file(cfg, text, sizeof text) &&
	                strstr(text, "[appearance]") &&
	                strstr(text, "theme = heart") &&
	                strstr(text, "onboarding_completed = true") &&
	                strstr(text, "schema_version = 1"));
	/* Answering the consent rewrote the file, which keeps the previous
	 * version as "<file>~". The product never replaces that backup, so move
	 * it aside as its own message asks before the next save. */
	{
		wchar_t backup[1110], aside[1130];

		swprintf(backup, 1110, L"%ls~", cfg);
		swprintf(aside, 1130, L"%ls.consent", backup);
		ok = !file_exists(backup) || MoveFileExW(backup, aside, 0) != 0;
		THEME_CHECK("first_run_consent_backup_moved_aside", "no ~ left",
		        ok && !file_exists(backup));
	}
	/* ---- settings: preview, discard, save */
	THEME_STEP(",", "Settings", "settings_open");
	/* Appearance is the third category; Right goes into its details */
	THEME_STEP("\x1b[B\x1b[B\x1b[C", "d duplicate",
	        "settings_open_appearance");
	THEME_STEP(
	        "\x1b[B\r", "unsaved changes", "settings_change_marks_unsaved");
	Sleep(150);
	THEME_CHECK("settings_theme_preview_is_live", "Diamond colors",
	        raw_has(&p, "48;2;248;250;252"));
	THEME_STEP("\x1b", "Unsaved changes", "settings_esc_asks");
	THEME_STEP("d", "doc.txt", "settings_discard_returns");
	THEME_CHECK("settings_discard_wrote_nothing", "still heart",
	        read_text_file(cfg, text, sizeof text) &&
	                strstr(text, "theme = heart") &&
	                !strstr(text, "diamond"));
	THEME_STEP(",", "Settings", "settings_reopen");
	/* Settings comes back where it was left (Appearance, its details, the
	 * second row); one Up and the next step's Down put it on that row again */
	THEME_STEP("\x1b[A", "d duplicate", "settings_reopen_appearance");
	THEME_STEP("\x1b[B\r", "unsaved changes", "settings_change_again");
	THEME_STEP("\x13", "Saved", "settings_ctrl_s_saves");
	Sleep(150);
	THEME_CHECK("settings_saved_to_the_file", "theme = diamond",
	        read_text_file(cfg, text, sizeof text) &&
	                strstr(text, "theme = diamond") &&
	                !strstr(text, "heart"));
	THEME_STEP("\x1b", "doc.txt", "settings_esc_after_save");
	pty_send(&p, "q", 1);
	ok = WAIT_OBJECT_0 == WaitForSingleObject(p.pi.hProcess, 5000);
	ev_check(pr->e, "themes_first_session_exits", 0, "exit",
	        ok ? "exited" : "running", ok);
	pty_finish(&p);
	if (!ok) {
		return false;
	}

	/* ---- the next start: no theme screen, the saved theme */
	PRODUCT_REQUIRE(theme_start(pr, &p, dir, false, L"en"));
	ok = product_wait(&p, "doc.txt", pr->screen, sizeof pr->screen);
	ev_check(pr->e, "second_start_goes_to_the_files", 0, "doc.txt",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		theme_dump(pr, &p);
		pty_finish(&p);
		return false;
	}
	THEME_CHECK("second_start_no_theme_screen", "absent",
	        NULL == strstr(pr->screen, "Choose your theme"));
	THEME_CHECK("second_start_uses_the_saved_theme", "48;2;248;250;252",
	        raw_has(&p, "48;2;248;250;252") &&
	                !raw_has(&p, "48;2;15;23;42"));
	pty_send(&p, "q", 1);
	WaitForSingleObject(p.pi.hProcess, 5000);
	pty_finish(&p);

	/* ---- NO_COLOR: no colour codes at all */
	PRODUCT_REQUIRE(theme_start(pr, &p, dir, true, L"en"));
	ok = product_wait(&p, "doc.txt", pr->screen, sizeof pr->screen);
	ev_check(pr->e, "no_color_start", 0, "doc.txt",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		theme_dump(pr, &p);
		pty_finish(&p);
		return false;
	}
	Sleep(200);
	EnterCriticalSection(&p.lock);
	ev_write_raw(pr->e, L"traces\\nocolor.vt", p.raw, p.raw_n);
	LeaveCriticalSection(&p.lock);
	THEME_CHECK("no_color_writes_no_colors", "no 38;2 / 48;2 / 38;5",
	        !raw_has(&p, ";38;2;") && !raw_has(&p, ";48;2;") &&
	                !raw_has(&p, ";38;5;") && !raw_has(&p, ";48;5;"));
	THEME_CHECK("no_color_selection_still_shows", "reverse video",
	        raw_has(&p,
	                "\x1b[7m")); /* as the pseudo console re-encodes it */
	pty_send(&p, "q", 1);
	WaitForSingleObject(p.pi.hProcess, 5000);
	pty_finish(&p);
	drop_config(cfg);

	PRODUCT_REQUIRE(theme_start(pr, &p, dir, false, L"en"));
	ok = product_wait(
	        &p, "Choose your language", pr->screen, sizeof pr->screen);
	ev_check(pr->e, "language_first_run_english", 0, "Choose your language",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		theme_dump(pr, &p);
		pty_finish(&p);
		return false;
	}
	THEME_STEP("\x1b[B",
	        "\xE8\xA8\x80\xE8\xAA\x9E\xE3\x82\x92\xE9\x81\xB8\xE6\x8A\x9E",
	        "language_down_shows_japanese_live");
	THEME_STEP("\r",
	        "\xE3\x82\xAD\xE3\x83\xBC\xE6\x93\x8D\xE4\xBD\x9C\xE3\x82\x92"
	        "\xE9\x81\xB8\xE6\x8A\x9E",
	        "language_enter_goes_to_the_keys_in_japanese");
	THEME_STEP("\r",
	        "\xE3\x83\x86\xE3\x83\xBC\xE3\x83\x9E\xE3\x82\x92\xE9\x81\xB8"
	        "\xE6\x8A\x9E",
	        "keys_enter_goes_to_the_theme_in_japanese");
	THEME_CHECK("language_theme_names_not_translated", "Spade",
	        NULL != strstr(pr->screen, "Spade") &&
	                NULL != strstr(pr->screen, "Heart"));
	/* the consent prompt follows the theme, in the chosen language */
	THEME_STEP("\r",
	        "\xE6\x9B\xB4\xE6\x96\xB0\xE3\x82\x92\xE8\x87\xAA\xE5\x8B\x95"
	        "\xE3\x81\xA7",
	        "language_first_run_ends_at_updates_consent_in_japanese");
	THEME_STEP("\x1b",
	        "\xE7\x8F\xBE\xE5\x9C\xA8\xE3\x81\xAE\xE3\x83\x95\xE3\x82\xA9"
	        "\xE3\x83\xAB\xE3\x83\x80",
	        "language_main_screen_in_japanese");
	THEME_CHECK("language_file_names_untouched", "doc.txt",
	        NULL != strstr(pr->screen, "doc.txt"));
	Sleep(150);
	THEME_CHECK("language_saved_to_the_file", "language = ja",
	        read_text_file(cfg, text, sizeof text) &&
	                strstr(text, "[general]") &&
	                strstr(text, "language = ja"));
	THEME_STEP(",", "\xE8\xA8\xAD\xE5\xAE\x9A",
	        "language_settings_in_japanese");
	THEME_CHECK("language_settings_item", "display language",
	        strstr(pr->screen,
	                "\xE8\xA1\xA8\xE7\xA4\xBA\xE8\xA8\x80\xE8\xAA\x9E") !=
	                        NULL &&
	                NULL != strstr(pr->screen, "\xE4\xB8\x80\xE8\x88\xAC"));
	THEME_STEP("\x1b", "doc.txt", "language_settings_closes");
	THEME_STEP("g", "\xE7\xA7\xBB\xE5\x8B\x95\xE5\x85\x88: ",
	        "language_smart_go_in_japanese");
	THEME_STEP("\x1b", "doc.txt", "language_smart_go_closes");
	pty_send(&p, "q", 1);
	WaitForSingleObject(p.pi.hProcess, 5000);
	pty_finish(&p);
	/* the next start, with no --language: the file decides */
	PRODUCT_REQUIRE(theme_start(pr, &p, dir, false, NULL));
	ok = product_wait(&p,
	        "\xE7\x8F\xBE\xE5\x9C\xA8\xE3\x81\xAE\xE3\x83\x95\xE3\x82\xA9"
	        "\xE3\x83\xAB\xE3\x83\x80",
	        pr->screen, sizeof pr->screen);
	ev_check(pr->e, "language_next_start_uses_the_file", 0,
	        "Japanese main screen", ok ? "observed" : "missing", ok);
	if (!ok) {
		theme_dump(pr, &p);
		pty_finish(&p);
		return false;
	}
	pty_send(&p, "q", 1);
	WaitForSingleObject(p.pi.hProcess, 5000);
	pty_finish(&p);
	drop_config(cfg);

	PRODUCT_REQUIRE(theme_start(pr, &p, dir, false, NULL));
	ok = product_wait(&p, "Enter  ", pr->screen, sizeof pr->screen);
	ev_check(pr->e, "enter_run_starts_on_the_language_screen", 0, "Enter  ",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		theme_dump(pr, &p);
		pty_finish(&p);
		return false;
	}
	/* the machine's own language is the chosen row, first in the list */
	THEME_CHECK("enter_run_first_row_is_chosen", "> ",
	        NULL != strstr(pr->screen, "> "));
	/* language, keys and theme take three Enters. The consent prompt that
	 * follows has "enable" as its first row, so a fourth Enter would turn
	 * network checks on: answer it with Esc (not now), then keep pressing
	 * Enter on the file lists. */
	pty_send(&p, "\r\r\r", 3);
	Sleep(400);
	pty_send(&p, "\x1b", 1);
	Sleep(200);
	pty_send(&p, "\r\r\r\r\r", 5);
	ok = product_wait(&p, "doc.txt", pr->screen, sizeof pr->screen);
	ev_check(pr->e, "enter_run_reaches_the_file_lists", 0, "doc.txt",
	        ok ? "observed" : "missing", ok);
	Sleep(300);
	pty_snapshot(&p, pr->screen, sizeof pr->screen);
	THEME_CHECK("enter_run_opened_and_entered_nothing", "still in browse",
	        NULL == strstr(pr->screen, "browse\\child") &&
	                NULL == strstr(pr->screen, "Opened"));
	pty_send(&p, "q", 1);
	WaitForSingleObject(p.pi.hProcess, 5000);
	pty_finish(&p);
	drop_config(cfg);
#undef THEME_CHECK
#undef THEME_STEP
	return true;
}

/* The Vim and Nano profiles in the real program: modified keys, the command
 * line, the sort menu, the keyboard page of the settings and the editor's
 * modes, all from a configuration file. Nothing is saved or changed. */
static bool product_keyboard(Product *pr)
{
	Pty p;
	wchar_t cfg[1100], dir[1024];
	int ok;

#define KEY_STEP(keys, needle, name)                                           \
	do {                                                                   \
		pty_send(&p, keys, strlen(keys));                              \
		ok = product_wait(&p, needle, pr->screen, sizeof pr->screen);  \
		product_snapshot(pr, name);                                    \
		ev_check(pr->e, name, 0, needle, ok ? "observed" : "missing",  \
		        ok);                                                   \
		if (!ok) {                                                     \
			theme_dump(pr, &p);                                    \
			pty_finish(&p);                                        \
			return false;                                          \
		}                                                              \
	} while (0)
#define KEY_CHECK(name, expected, condition)                                   \
	do {                                                                   \
		ok = (condition);                                              \
		ev_check(pr->e, name, 0, expected,                             \
		        ok ? "observed" : "missing", ok);                      \
		if (!ok) {                                                     \
			theme_dump(pr, &p);                                    \
			pty_finish(&p);                                        \
			return false;                                          \
		}                                                              \
	} while (0)

	PRODUCT_REQUIRE(
	        p0_lab_path(&pr->lab, L"piero\\config.ini", cfg, 1100) &&
	        p0_lab_path(&pr->lab, L"browse", dir, 1024));
	/* ---- Vim */
	drop_config(cfg);
	PRODUCT_REQUIRE(product_file(&pr->lab, L"piero\\config.ini",
	        "[meta]\r\nschema_version = 1\r\nonboarding_completed = "
	        "true\r\n"
	        "[updates]\r\nenabled = false\r\nconsent_recorded = true\r\n"
	        "[keyboard]\r\nprofile = vim\r\n"));
	PRODUCT_REQUIRE(theme_start(pr, &p, dir, false, L"en"));
	ok = product_wait(&p, "doc.txt", pr->screen, sizeof pr->screen);
	ev_check(pr->e, "vim_starts_on_the_files", 0, "doc.txt",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		theme_dump(pr, &p);
		pty_finish(&p);
		return false;
	}
	/* the command line is at the bottom, and has no shell */
	KEY_STEP(":", "COMMAND", "vim_colon_opens_the_command_line");
	KEY_STEP("sort size desc\r", "Sorted by size, descending",
	        "vim_sort_command");
	KEY_STEP(":", "COMMAND", "vim_colon_opens_again");
	KEY_STEP("set hidden on\r", "Hidden items are shown",
	        "vim_set_hidden_command");
	KEY_STEP(":!echo hi\r", "Unknown command: !echo",
	        "vim_has_no_shell_escape");
	KEY_STEP(":sort created\r", "not available",
	        "vim_created_date_sort_is_not_offered");
	/* a number before a copy is refused; dd stages a move, not a delete */
	KEY_STEP("5yy", "A number cannot be put before that command",
	        "vim_count_before_copy_refused");
	/* Ctrl+L needs the modified-key input of the file manager */
	KEY_STEP("\x0c", "Go to:", "vim_ctrl_l_opens_smart_go");
	KEY_STEP("\x1b", "doc.txt", "vim_esc_closes_smart_go");
	/* the help is made from the keys in force */
	KEY_STEP("?", "gg", "vim_help_lists_the_vim_keys");
	KEY_STEP("\x1b", "doc.txt", "vim_help_closes");
	/* settings: the Keyboard page shows the profile; Esc changes nothing */
	KEY_STEP(",", "Keyboard", "vim_settings_opens");
	KEY_STEP("\x1b[B\x1b[C", "Profile", "vim_keyboard_page");
	KEY_CHECK(
	        "vim_profile_shown", "Vim", NULL != strstr(pr->screen, "Vim"));
	KEY_STEP("\r\r", "unsaved changes", "vim_profile_can_be_cycled");
	KEY_STEP("\x1b", "Unsaved changes", "vim_settings_esc_asks");
	KEY_STEP("d", "doc.txt", "vim_settings_discard_returns");
	{
		char text[2048];

		KEY_CHECK("vim_settings_wrote_nothing", "profile = vim",
		        read_text_file(cfg, text, sizeof text) &&
		                strstr(text, "profile = vim") &&
		                !strstr(text, "custom") &&
		                !strstr(text, "nano"));
	}
	/* the editor is modal: Normal, Insert, and a quit that asks nothing for a
	 * clean file */
	KEY_STEP(":sort name asc\r", "Sorted by name, ascending",
	        "vim_sort_back_to_name");
	KEY_STEP("j", "doc.txt", "vim_j_moves_to_the_file");
	/* The file manager also says NORMAL. Wait for the editor's status
	 * separators before sending g; a quiet old frame is not an open editor. */
	KEY_STEP("e", "|  NORMAL  |", "vim_editor_starts_in_normal_mode");
	KEY_STEP("g", "NORMAL g", "vim_editor_pending_sequence");
	Sleep(WF_SEQ_TIMEOUT_MS + 100);
	pty_snapshot(&p, pr->screen, sizeof pr->screen);
	product_snapshot(pr, "vim_editor_sequence_expired_without_input");
	KEY_CHECK("vim_editor_sequence_expires_without_input", "no pending g",
	        NULL == strstr(pr->screen, "NORMAL g"));
	KEY_STEP("i", "-- INSERT --", "vim_editor_i_inserts");
	KEY_STEP("\x1b", "NORMAL", "vim_editor_esc_returns_to_normal");
	KEY_STEP("u", "NORMAL", "vim_editor_undo_is_harmless");
	KEY_STEP("/", "SEARCH", "vim_editor_search_prompt");
	KEY_STEP("\x1b", "NORMAL", "vim_editor_search_cancel");
	KEY_STEP(":", "COMMAND", "vim_editor_command_prompt");
	KEY_STEP("wq!\r", "Unknown command", "vim_editor_unknown_command");
	KEY_STEP(":q\r", "doc.txt", "vim_editor_q_leaves_a_clean_file");
	/* q is the one way out of the file manager, and it is the same as :q */
	pty_send(&p, ":q\r", 3);
	ok = WAIT_OBJECT_0 == WaitForSingleObject(p.pi.hProcess, 5000);
	ev_check(pr->e, "vim_colon_q_exits", 0, "exit",
	        ok ? "exited" : "running", ok);
	pty_finish(&p);
	if (!ok) {
		return false;
	}

	/* ---- Nano */
	drop_config(cfg);
	PRODUCT_REQUIRE(product_file(&pr->lab, L"piero\\config.ini",
	        "[meta]\r\nschema_version = 1\r\nonboarding_completed = "
	        "true\r\n"
	        "[updates]\r\nenabled = false\r\nconsent_recorded = true\r\n"
	        "[keyboard]\r\nprofile = nano\r\n"));
	PRODUCT_REQUIRE(theme_start(pr, &p, dir, false, L"en"));
	ok = product_wait(&p, "doc.txt", pr->screen, sizeof pr->screen);
	ev_check(pr->e, "nano_starts_on_the_files", 0, "doc.txt",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		theme_dump(pr, &p);
		pty_finish(&p);
		return false;
	}
	KEY_STEP("S", "SORT", "nano_s_opens_the_sort_menu");
	KEY_STEP("\x1b", "doc.txt", "nano_sort_menu_closes");
	KEY_STEP("\x0c", "Go to:", "nano_ctrl_l_opens_smart_go");
	KEY_STEP("\x1b", "doc.txt", "nano_esc_closes_smart_go");
	KEY_STEP("\x1b[B", "doc.txt", "nano_down_moves_to_the_file");
	KEY_STEP("e", "Ctrl+S", "nano_editor_shows_ctrl_keys");
	KEY_STEP("\x18", "doc.txt", "nano_ctrl_x_leaves_a_clean_file");
	pty_send(&p, "q", 1);
	ok = WAIT_OBJECT_0 == WaitForSingleObject(p.pi.hProcess, 5000);
	ev_check(pr->e, "nano_q_exits", 0, "exit", ok ? "exited" : "running",
	        ok);
	pty_finish(&p);
	drop_config(cfg);
#undef KEY_CHECK
#undef KEY_STEP
	return 0 != ok;
}

/* Normal exit, terminal restore, the last-directory file and the untouched source. */
static bool product_exit(Product *pr)
{
	int ok;
	DWORD code = 99, got;
	HANDLE h;

	pty_send(&pr->p, "q", 1);
	ok = WAIT_OBJECT_0 == WaitForSingleObject(pr->p.pi.hProcess, 5000);
	if (ok) {
		GetExitCodeProcess(pr->p.pi.hProcess, &code);
	}
	ev_check(pr->e, "normal_exit", 0, "exit 0",
	        ok && 0 == code ? "exit 0" : "failed", ok && 0 == code);
	Sleep(30); /* let the bounded reader consume the final reset bytes */
	EnterCriticalSection(&pr->p.lock);
	ev_check(pr->e, "terminal_restore", 0, "alternate screen leave",
	        contains(pr->p.raw, pr->p.raw_n, "\x1b[?1049l") ? "present"
	                                                        : "missing",
	        contains(pr->p.raw, pr->p.raw_n, "\x1b[?1049l"));
	ev_check(pr->e, "bounded_capture", 0, "complete bounded VT",
	        pr->p.capture_failed ? "incomplete" : "complete",
	        !pr->p.capture_failed);
	LeaveCriticalSection(&pr->p.lock);
	h = CreateFileW(pr->last, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	got = 0;
	if (INVALID_HANDLE_VALUE != h) {
		ReadFile(h, pr->output, sizeof pr->output - 1, &got, NULL);
		CloseHandle(h);
	}
	pr->output[got] = 0;
	ok = got >= 3 && !memcmp(pr->output, "\xef\xbb\xbf", 3) &&
	        !strcmp(pr->output + 3, pr->path8);
	ev_check(pr->e, "last_directory", 0, "BOM UTF8 final directory",
	        ok ? "matches" : "mismatch", ok);
	ok = p0_sha256_file(pr->source, pr->after) &&
	        !strcmp(pr->before, pr->after);
	ev_check(pr->e, "source_preserved", 0, "same SHA256",
	        ok ? "unchanged" : "changed", ok);
	return true;
}

/* Evidence, the session's own cleanup, and the retained fixture note. */
static void product_teardown(Product *pr)
{
	if (pr->started) {
		pty_snapshot(&pr->p, pr->screen, sizeof pr->screen);
		ev_write_text(
		        pr->e, L"artifacts\\product-screen.txt", pr->screen);
		EnterCriticalSection(&pr->p.lock);
		ev_write_raw(
		        pr->e, L"traces\\product.vt", pr->p.raw, pr->p.raw_n);
		LeaveCriticalSection(&pr->p.lock);
		pty_finish(&pr->p);
	}
	free(pr->saved_local);
	free(pr->saved_roaming);
	snprintf(pr->output, sizeof pr->output, "Retained owned fixture: %ls\n",
	        p0_area_root(pr->lab.area));
	ev_write_text(pr->e, L"artifacts\\fixture.txt", pr->output);
	p0_lab_close(&pr->lab, 1);
}

/* Text-only profile checks need neither an image decoder nor Shell
 * associations. Keep them runnable when those independent prerequisites fail. */
static int scn_keyboard(Ev *e)
{
	Product pr;
	int rc = 1;

	memset(&pr, 0, sizeof pr);
	pr.e = e;
	if (!p0_lab_open(&pr.lab, "keyboard", 1)) {
		ev_check(e, "keyboard_fixture", 0, "owned NTFS root",
		        "unavailable", false);
		return 1;
	}
	if (p0_area_mkdir(pr.lab.area, L"piero") &&
	        p0_area_mkdir(pr.lab.area, L"browse") &&
	        p0_area_mkdir(pr.lab.area, L"browse\\child") &&
	        product_file(
	                &pr.lab, L"browse\\doc.txt", "keyboard fixture\n")) {
		rc = product_keyboard(&pr) && !e->fails ? 0 : 1;
	} else {
		ev_check(e, "keyboard_fixture", 0, "text fixture", "failed",
		        false);
	}
	product_teardown(&pr);
	return rc;
}

/* Archives as folders: a ZIP is listed in the right pane, entered with Enter,
 * browsed, previewed, refused every change, and left with h back onto the file;
 * the cursor of a folder inside is remembered. */
static bool product_archive(Product *pr)
{
	wchar_t dir[1024], zip[1100], from[1100], rel[64];
	int ok;

#ifndef WF_TEST_ARCHIVE_DIR
	(void)dir;
	(void)zip;
	(void)from;
	(void)rel;
	(void)ok;
	ev_check(pr->e, "archive_fixture", 0, "tests/archive", "not built in",
	        0);
	return false;
#else
	PRODUCT_REQUIRE(p0_area_mkdir(pr->lab.area, L"arch"));
	PRODUCT_REQUIRE(p0_lab_path(&pr->lab, L"arch\\basic.zip", zip, 1100) &&
	        p0_lab_path(&pr->lab, L"arch", dir, 1024));
	MultiByteToWideChar(
	        CP_UTF8, 0, WF_TEST_ARCHIVE_DIR "/basic.zip", -1, from, 1100);
	for (wchar_t *c = from; *c; c++) {
		if (L'/' == *c) {
			*c = L'\\';
		}
	}
	PRODUCT_REQUIRE(CopyFileW(from, zip, TRUE));
	swprintf(rel, 64, L"arch\\basic.zip");
	PRODUCT_REQUIRE(p0_area_track(pr->lab.area, rel, 0));
	PRODUCT_REQUIRE(go_by_path_bar(pr, dir, "archive_folder"));
	PRODUCT_STEP("", "basic.zip", "archive_listed_as_a_file");
	/* the cursor rests on it: the right pane lists what is inside */
	PRODUCT_STEP("", "README.md", "archive_right_pane_lists_inside");
	PRODUCT_STEP("\r", "ARCHIVE (read-only)", "archive_enter_enters");
	ok = NULL != strstr(pr->screen, "basic.zip|");
	ev_check(pr->e, "archive_title_shows_the_archive", 0, "basic.zip|",
	        ok ? "observed" : "missing", ok);
	ok = strstr(pr->screen, "docs/") && strstr(pr->screen, "src/") &&
	        strstr(pr->screen, "README.md");
	ev_check(pr->e, "archive_top_lists_folders_and_files", 0,
	        "docs/ src/ README.md", ok ? "observed" : "missing", ok);
	/* docs/ src/ ... : the second line is src; its contents are on the right */
	PRODUCT_STEP("j", "main.c", "archive_cursor_move_lists_the_folder");
	PRODUCT_STEP("\r", "ARCHIVE (read-only)", "archive_enter_inner_folder");
	PRODUCT_STEP("", "int main", "archive_text_preview");
	PRODUCT_STEP("j", "int util", "archive_next_file_previews");
	/* nothing in it can be changed */
	PRODUCT_STEP("a", "Read-only archive", "archive_refuses_create");
	PRODUCT_STEP("R", "Read-only archive", "archive_refuses_rename");
	PRODUCT_STEP("\x1b[3~", "Read-only archive", "archive_refuses_delete");
	PRODUCT_STEP("e", "Read-only archive", "archive_refuses_edit");
	PRODUCT_STEP("x", "Read-only archive", "archive_refuses_move");
	/* a copy out of it is allowed: y picks, p in a folder extracts */
	PRODUCT_STEP(
	        "y", "picked from the archive", "archive_picks_for_extraction");
	PRODUCT_STEP("p", "Read-only archive", "archive_refuses_paste");
	PRODUCT_STEP("\r", "Read-only archive", "archive_refuses_open");
	/* back out and in again: the cursor is where it was left */
	PRODUCT_STEP("h", "docs/", "archive_h_goes_up");
	PRODUCT_STEP(
	        "l", "utils.c", "archive_navigation_memory_in_the_archive");
	ok = NULL != strstr(pr->screen, "int util");
	ev_check(pr->e, "archive_memory_restores_the_cursor_row", 0, "int util",
	        ok ? "observed" : "missing", ok);
	PRODUCT_STEP("h", "docs/", "archive_up_to_the_top");
	PRODUCT_STEP("h", "NORMAL", "archive_h_leaves_the_archive");
	ok = NULL == strstr(pr->screen, "ARCHIVE (read-only)") &&
	        NULL != strstr(pr->screen, "basic.zip");
	ev_check(pr->e, "archive_leaving_returns_to_the_folder", 0,
	        "NORMAL, basic.zip shown", ok ? "observed" : "missing", ok);
	/* the file is untouched */
	{
		HANDLE h = CreateFileW(zip, GENERIC_READ, FILE_SHARE_READ, NULL,
		        OPEN_EXISTING, 0, NULL);
		LARGE_INTEGER size = {0};

		if (INVALID_HANDLE_VALUE != h) {
			GetFileSizeEx(h, &size);
			CloseHandle(h);
		}
		ev_check(pr->e, "archive_file_unchanged", 0, "2299 bytes",
		        2299 == size.QuadPart ? "same" : "changed",
		        2299 == size.QuadPart);
	}
	/* a copy out of it: y inside picks the folder under the cursor, p in the
	 * folder with the archive extracts it there, and the archive stays */
	PRODUCT_STEP("\r", "ARCHIVE (read-only)", "archive_extract_enter");
	PRODUCT_STEP("kkkkk", "ARCHIVE (read-only)", "archive_extract_top");
	PRODUCT_STEP("y", "picked from the archive", "archive_extract_pick");
	PRODUCT_STEP("h", "NORMAL", "archive_extract_leave");
	PRODUCT_STEP("p", "NORMAL", "archive_extract_paste");
	{
		wchar_t any[1100];
		ULONGLONG end = GetTickCount64() + 15000;
		bool there = false, clean = false;

		swprintf(any, 1100, L"%ls\\docs\\*", dir);
		while (!there && GetTickCount64() < end) {
			WIN32_FIND_DATAW fd;
			HANDLE f = FindFirstFileW(any, &fd);

			if (INVALID_HANDLE_VALUE != f) {
				do {
					there = there ||
					        (wcscmp(fd.cFileName, L".") &&
					                wcscmp(fd.cFileName,
					                        L".."));
				} while (FindNextFileW(f, &fd));
				FindClose(f);
			}
			if (!there) {
				Sleep(100);
			}
		}
		swprintf(any, 1100, L"%ls\\.wfextract-*", dir);
		{
			WIN32_FIND_DATAW fd;
			HANDLE f = FindFirstFileW(any, &fd);

			clean = INVALID_HANDLE_VALUE == f;
			if (!clean) {
				FindClose(f);
			}
		}
		ev_check(pr->e, "archive_extract_writes_the_folder", 0,
		        "the docs folder with its files, no staging left",
		        there && clean ? "observed" : "missing",
		        there && clean);
	}
	return true;
#endif
}

static size_t count_text(const char *haystack, const char *needle)
{
	size_t n = 0, len = strlen(needle);

	for (const char *at = haystack; (at = strstr(at, needle)); at += len) {
		n++;
	}
	return n;
}

/* An 8 x 8 24-bit bitmap: red to the right, green downwards. */
static size_t product_bmp(unsigned char *out)
{
	const size_t row = 24, size = 54 + 8 * row;

	memset(out, 0, size);
	out[0] = 'B';
	out[1] = 'M';
	out[2] = (unsigned char)size;
	out[10] = 54;
	out[14] = 40;
	out[18] = 8;
	out[22] = 8;
	out[26] = 1;
	out[28] = 24;
	for (int y = 0; y < 8; y++) {
		for (int x = 0; x < 8; x++) {
			unsigned char *px = out + 54 + (size_t)(7 - y) * row +
			        (size_t)x * 3;

			px[0] = 40;
			px[1] = (unsigned char)(y * 32);
			px[2] = (unsigned char)(x * 32);
		}
	}
	return size;
}

/*
 * Pictures and PDF pages through the real product: the sandboxed decoder host
 * is started (an AppContainer process, staged under the private state root),
 * a page is drawn with half blocks in the pane and full screen, turned with
 * the keys, and a bitmap is shown. Pseudo console only: no Windows Terminal.
 */
static bool product_pictures(Product *pr)
{
	wchar_t dir[1024], staged[1100];
	unsigned char bmp[54 + 8 * 24];
	size_t blocks_pane, blocks_full;
	DWORD attributes;
	int ok;

	PRODUCT_REQUIRE(p0_area_mkdir(pr->lab.area, L"pics"));
	PRODUCT_REQUIRE(product_file_bytes(
	        &pr->lab, L"pics\\a.pdf", pdf_two_page, sizeof pdf_two_page));
	PRODUCT_REQUIRE(product_file_bytes(
	        &pr->lab, L"pics\\b.bmp", bmp, product_bmp(bmp)));
	PRODUCT_REQUIRE(p0_lab_path(&pr->lab, L"pics", dir, 1024));
	PRODUCT_REQUIRE(go_by_path_bar(pr, dir, "pictures_folder"));
	/* the cursor is on a.pdf: the first page arrives from the isolated host,
	 * which has to start first */
	ok = product_wait_ms(
	        &pr->p, "page 1/2", pr->screen, sizeof pr->screen, 30000);
	product_snapshot(pr, "pdf_first_page_in_the_pane");
	ev_check(pr->e, "pdf_first_page_in_the_pane", 0, "page 1/2",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		/* Preserve the failure, but expand its error text: the narrow pane
		 * elides the OS error code needed to diagnose sandbox startup. */
		pty_send(&pr->p, "v", 1);
		(void)product_wait_ms(
		        &pr->p, "error ", pr->screen, sizeof pr->screen, 2000);
		product_snapshot(pr, "pdf_first_page_failure_fullscreen");
		return false;
	}
	blocks_pane = count_text(pr->screen, "\xE2\x96\x80");
	ev_check(pr->e, "pdf_page_is_drawn_with_half_blocks", 0,
	        "more than 40 half-block cells",
	        blocks_pane > 40 ? "observed" : "missing", blocks_pane > 40);
	/* the host is a staged copy of the program under the private state root,
	 * not a decoder inside the application process */
	swprintf(staged, 1100, L"%ls\\piero\\worker", pr->lab.root);
	attributes = GetFileAttributesW(staged);
	ev_check(pr->e, "decoder_host_was_staged_for_the_sandbox", 0,
	        "folder piero\\worker exists",
	        INVALID_FILE_ATTRIBUTES != attributes ? "observed" : "missing",
	        INVALID_FILE_ATTRIBUTES != attributes);
	/* focus the pane, turn the page, turn it back */
	PRODUCT_STEP("\x1b[C", "PREVIEW_PANE", "pictures_focus_pane");
	ok = 0;
	pty_send(&pr->p, "\x1b[6~", 4);
	ok = product_wait_ms(
	        &pr->p, "page 2/2", pr->screen, sizeof pr->screen, 20000);
	product_snapshot(pr, "pdf_page_down");
	ev_check(pr->e, "pdf_page_down_shows_page_two", 0, "page 2/2",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		return false;
	}
	pty_send(&pr->p, "\x1b[5~", 4);
	ok = product_wait_ms(
	        &pr->p, "page 1/2", pr->screen, sizeof pr->screen, 20000);
	product_snapshot(pr, "pdf_page_up");
	ev_check(pr->e, "pdf_page_up_shows_page_one", 0, "page 1/2",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		return false;
	}
	/* full screen asks for a bigger picture */
	pty_send(&pr->p, "v", 1);
	ok = product_wait_ms(
	        &pr->p, "page 1/2", pr->screen, sizeof pr->screen, 20000);
	Sleep(800);
	pty_snapshot(&pr->p, pr->screen, sizeof pr->screen);
	product_snapshot(pr, "pdf_full_screen");
	blocks_full = count_text(pr->screen, "\xE2\x96\x80");
	ev_check(pr->e, "pdf_full_screen_is_bigger_than_the_pane", 0,
	        "more half-block cells than in the pane",
	        blocks_full > blocks_pane ? "observed" : "missing",
	        ok && blocks_full > blocks_pane);
	PRODUCT_STEP("\x1b", "NORMAL", "pictures_leave_full_screen");
	/* a bitmap: next entry */
	pty_send(&pr->p, "j", 1);
	ok = product_wait_ms(
	        &pr->p, "8 x 8", pr->screen, sizeof pr->screen, 20000);
	product_snapshot(pr, "bitmap_in_the_pane");
	ev_check(pr->e, "bitmap_is_shown_with_its_size", 0, "8 x 8",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		return false;
	}
	/* leaving the folder clears it: a text file shows no picture */
	return true;
}

#include "p0_pictures.c"
#include "p0_prerequisites.h"

/* This path omits association registration and the separate copy-queue trial.
 * It never substitutes for the complete product/safety test. */
static bool product_ui_finish(Product *pr)
{
	if (!go_by_path_bar(pr, pr->dest, "ui_final_folder")) {
		return false;
	}
	return WideCharToMultiByte(CP_UTF8, 0, pr->dest, -1, pr->path8,
	               sizeof pr->path8, NULL, NULL) != 0;
}

/* File-operation coverage does not require Shell association registration or
 * decoder startup. The full scenarios retain both independent prerequisites. */
typedef enum
{
	PRODUCT_FULL,
	PRODUCT_UI,
	PRODUCT_FILE_OPS,
	PRODUCT_PICTURES /* pictures and PDF only, the quick one to mutate */
} ProductScope;

/* Only fixed phase identifiers and elapsed time enter the CTest output.
 * Flush now: a killed host will never reach its normal summary. */
static void product_phase_log(
        const char *phase, const char *state, double started)
{
	printf("PRODUCT-TUI phase=%s state=%s elapsed_ms=%.0f\n", phase, state,
	        now_ms() - started);
	fflush(stdout);
}

static bool product_phase_result(const char *phase, bool ok, double started)
{
	product_phase_log(phase, ok ? "done" : "failed", started);
	return ok;
}

static int scn_product(Ev *e, ProductScope scope)
{
	Product pr;
	int rc = 1;
	bool ui_only = scope == PRODUCT_UI;
	bool file_ops = scope == PRODUCT_FILE_OPS;
	int prerequisite;
	double phase_started = now_ms();

#define PRODUCT_PHASE(call)                                                    \
	(product_phase_log(#call, "begin", phase_started),                     \
	        product_phase_result(#call, (call), phase_started))

	product_phase_log("prerequisites", "begin", phase_started);
	if (!file_ops && (prerequisite = require_appcontainer(e)) != 0) {
		product_phase_log("prerequisites",
		        77 == prerequisite ? "skipped" : "failed",
		        phase_started);
		return prerequisite;
	}

	memset(&pr, 0, sizeof pr);
	pr.e = e;
	if (ERROR_SUCCESS != wf_test_assoc_name(&open_association)) {
		product_phase_log("prerequisites", "failed", phase_started);
		return 1;
	}
	if (!PRODUCT_PHASE(p0_lab_open(&pr.lab, "product", 1))) {
		ev_check(e, "isolated_fixture", 0, "owned NTFS root",
		        "unavailable", 0);
		return 1;
	}
	if (file_ops &&
	        (prerequisite = require_readable_ancestors(
	                 e, p0_area_root(pr.lab.area))) != 0) {
		product_phase_log("prerequisites",
		        77 == prerequisite ? "skipped" : "failed",
		        phase_started);
		if (p0_lab_close(&pr.lab, 1)) {
			ev_check(e, "skipped_copy_fixture_cleanup", 0,
			        "test drive released", "cleanup failed", false);
			return 1;
		}
		return prerequisite;
	}
	product_phase_log("prerequisites", "done", phase_started);
	if (PRODUCT_PICTURES == scope) {
		if (PRODUCT_PHASE(product_setup(&pr)) &&
		        PRODUCT_PHASE(product_pictures(&pr)) &&
		        PRODUCT_PHASE(product_picture_failures(&pr)) &&
		        PRODUCT_PHASE(product_ui_finish(&pr)) &&
		        PRODUCT_PHASE(product_exit(&pr))) {
			rc = e->fails ? 1 : 0;
		}
	} else if (PRODUCT_PHASE(product_setup(&pr)) &&
	        PRODUCT_PHASE(product_preview(&pr)) &&
	        PRODUCT_PHASE(product_gates(&pr)) &&
	        PRODUCT_PHASE(product_navigation(&pr)) &&
	        PRODUCT_PHASE(product_path_bar_and_details(&pr)) &&
	        (scope != PRODUCT_FULL || PRODUCT_PHASE(product_open(&pr))) &&
	        PRODUCT_PHASE(product_editor(&pr)) &&
	        PRODUCT_PHASE(product_archive(&pr)) &&
	        (file_ops ||
	                (PRODUCT_PHASE(product_pictures(&pr)) &&
	                        PRODUCT_PHASE(
	                                product_picture_failures(&pr)))) &&
#ifdef WF_PRODUCT_EDIT
	        PRODUCT_PHASE(product_edit(&pr)) &&
	        PRODUCT_PHASE(product_move(&pr)) &&
#endif
	        (ui_only ? PRODUCT_PHASE(product_ui_finish(&pr))
	                 : PRODUCT_PHASE(product_transfer_and_resize(&pr))) &&
	        (ui_only || PRODUCT_PHASE(product_midtransfer(&pr))) &&
	        PRODUCT_PHASE(product_exit(&pr)) &&
	        (file_ops ||
	                (PRODUCT_PHASE(product_editor_nano(&pr)) &&
	                        PRODUCT_PHASE(product_themes(&pr)) &&
	                        PRODUCT_PHASE(product_keyboard(&pr))))) {
		rc = e->fails ? 1 : 0;
	}
	if (pr.blocked && !e->fails) {
		rc = 77;
	}
	product_phase_log("teardown", "begin", phase_started);
	product_teardown(&pr);
	product_phase_log("teardown", "done", phase_started);
	return rc;
#undef PRODUCT_PHASE
}

static BOOL WINAPI keep_test_host_alive(DWORD event)
{
	return event == CTRL_BREAK_EVENT || event == CTRL_C_EVENT;
}

/* Raw bytes the product has written so far. */
static size_t raw_length(Pty *p)
{
	size_t n;

	EnterCriticalSection(&p->lock);
	n = p->raw_n;
	LeaveCriticalSection(&p->lock);
	return n;
}

static int compare_ms(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

/*
 * Key to first output byte through the pseudo console: Up and Down alternate
 * so that every press moves the cursor and so repaints. This is ConPTY's pipe
 * to this host, NOT Windows Terminal's paint time. Reported, never gated on
 * a threshold: a shared machine decides the numbers.
 */
static void product_key_latency(Product *pr)
{
	enum
	{
		KEYS = 60
	};
	double ms[KEYS], sorted[KEYS];
	size_t out_bytes[KEYS], total = 0, got = 0;
	char metrics[512];

	Sleep(300); /* let the earlier repaints settle */
	for (unsigned i = 0; i < KEYS; i++) {
		size_t before = raw_length(&pr->p), after;
		double t0 = now_ms();

		pty_send(&pr->p, i % 2 ? "\x1b[B" : "\x1b[A", 3);
		while (raw_length(&pr->p) == before && now_ms() - t0 < 1000.0) {
			YieldProcessor();
		}
		ms[i] = raw_length(&pr->p) == before ? -1.0 : now_ms() - t0;
		Sleep(40); /* the rest of the frame */
		after = raw_length(&pr->p);
		out_bytes[i] = after - before;
		if (ms[i] >= 0.0) {
			sorted[got++] = ms[i];
			total += out_bytes[i];
		}
	}
	if (!got) {
		snprintf(metrics, sizeof metrics,
		        "keys=%d with_output=0 (no key repainted; nothing "
		        "measured)\n",
		        KEYS);
	} else {
		qsort(sorted, got, sizeof *sorted, compare_ms);
		snprintf(metrics, sizeof metrics,
		        "keys=%d with_output=%zu key_to_first_byte_ms p50=%.3f "
		        "p95=%.3f p99=%.3f max=%.3f "
		        "avg_output_bytes_per_key=%zu\n"
		        "scope=ConPTY pipe to the test host; not Windows "
		        "Terminal "
		        "paint latency\n",
		        KEYS, got, sorted[got / 2],
		        sorted[(got * 95 + 99) / 100 - 1],
		        sorted[(got * 99 + 99) / 100 - 1], sorted[got - 1],
		        total / got);
	}
	ev_write_text(pr->e, L"artifacts\\key-latency.txt", metrics);
	ev_check(pr->e, "key_latency_measured", 0,
	        "at least 90% of keys repaint", metrics,
	        got * 10 >= (size_t)KEYS * 9);
}

/*
 * Synchronized output (mode 2026) in the product's real output: whether this
 * terminal advertised it or not, every begin has its end, so a frame is never
 * left half-open. Reports the counts; "advertised" says only whether the
 * pseudo console answered the capability query.
 */
static void product_sync_output_check(Product *pr)
{
	static const char begin[] = "\x1b[?2026h", end[] = "\x1b[?2026l";
	size_t begins = 0, ends = 0;
	char metrics[160];

	EnterCriticalSection(&pr->p.lock);
	for (size_t i = 0; i + sizeof begin - 1 <= pr->p.raw_n; i++) {
		begins += !memcmp(pr->p.raw + i, begin, sizeof begin - 1);
		ends += !memcmp(pr->p.raw + i, end, sizeof end - 1);
	}
	LeaveCriticalSection(&pr->p.lock);
	snprintf(metrics, sizeof metrics,
	        "sync_begin=%zu sync_end=%zu advertised_by_this_terminal=%s\n",
	        begins, ends, begins ? "yes" : "no (never used)");
	ev_write_text(pr->e, L"artifacts\\sync-output.txt", metrics);
	ev_check(pr->e, "sync_output_begin_end_balanced", 0,
	        "every begin has its end", metrics, begins == ends);
}

/* A measurement against real ConPTY and private generated data. The model's
 * timing assertions live in test_preview_stability and use a fake clock. */
static int scn_preview_idle(Ev *e)
{
	Product pr;
	FILETIME created, exited, kernel, user;
	IO_COUNTERS io_before, io_after;
	PROCESS_MEMORY_COUNTERS_EX memory = {0};
	uint64_t cpu_before, cpu_after;
	size_t idle_begin, idle_end, noise_begin, flashes = 0,
	                                          repeated_text = 0;
	char metrics[1024];
	int rc = 1;
	memset(&pr, 0, sizeof pr);
	pr.e = e;
	if (ERROR_SUCCESS != wf_test_assoc_name(&open_association) ||
	        !p0_lab_open(&pr.lab, "preview-idle", 1)) {
		ev_check(e, "isolated_fixture", 0, "owned NTFS root",
		        "unavailable", 0);
		return 1;
	}
	if (!product_setup(&pr)) {
		goto done;
	}
	pty_send(&pr.p, "\x1b[B", 3);
	if (!product_wait(&pr.p, "SAFE_PREVIEW_MARKER", pr.screen,
	            sizeof pr.screen)) {
		ev_check(e, "preview_ready", 0, "text marker", "missing", 0);
		goto done;
	}
	Sleep(250); /* settle startup/terminal events before the measured window */
	EnterCriticalSection(&pr.p.lock);
	idle_begin = pr.p.raw_n;
	LeaveCriticalSection(&pr.p.lock);
	if (!GetProcessTimes(
	            pr.p.pi.hProcess, &created, &exited, &kernel, &user) ||
	        !GetProcessIoCounters(pr.p.pi.hProcess, &io_before)) {
		ev_check(e, "measurement_api", 0, "process counters", "failed",
		        0);
		goto done;
	}
	cpu_before =
	        ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
	cpu_before +=
	        ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
	double start = now_ms();
	while (now_ms() - start < 30000 &&
	        WaitForSingleObject(pr.p.pi.hProcess, 0) == WAIT_TIMEOUT) {
		Sleep(20);
	}
	double elapsed = now_ms() - start;
	memory.cb = sizeof memory;
	if (!GetProcessTimes(
	            pr.p.pi.hProcess, &created, &exited, &kernel, &user) ||
	        !GetProcessIoCounters(pr.p.pi.hProcess, &io_after) ||
	        !K32GetProcessMemoryInfo(pr.p.pi.hProcess,
	                (PROCESS_MEMORY_COUNTERS *)&memory, sizeof memory)) {
		ev_check(e, "measurement_api", 0, "process counters", "failed",
		        0);
		goto done;
	}
	cpu_after =
	        ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
	cpu_after += ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
	EnterCriticalSection(&pr.p.lock);
	idle_end = noise_begin = pr.p.raw_n;
	LeaveCriticalSection(&pr.p.lock);
	for (unsigned i = 0; i < 20; i++) {
		wchar_t name[128];
		swprintf(name, 128, L"browse\\child\\event%u.txt", i);
		if (!product_file(
		            &pr.lab, name, "unrelated generated data\n")) {
			ev_check(e, "subtree_fixture", 0, "new owned file",
			        "failed", 0);
			goto done;
		}
		Sleep(100);
	}
	Sleep(1500); /* allow the existing bounded notification debounce to settle */
	EnterCriticalSection(&pr.p.lock);
	for (size_t i = noise_begin; i + 12 <= pr.p.raw_n; i++) {
		flashes += !memcmp(pr.p.raw + i, "Loading text", 12);
	}
	for (size_t i = noise_begin; i + 19 <= pr.p.raw_n; i++) {
		repeated_text +=
		        !memcmp(pr.p.raw + i, "SAFE_PREVIEW_MARKER", 19);
	}
	LeaveCriticalSection(&pr.p.lock);
	IO_COUNTERS io_noise;
	if (!GetProcessIoCounters(pr.p.pi.hProcess, &io_noise)) {
		ev_check(e, "measurement_api", 0, "noise process counters",
		        "failed", 0);
		goto done;
	}
	snprintf(metrics, sizeof metrics,
	        "idle_window_ms=%.3f\nidle_cpu_ms=%.3f\nidle_output_bytes=%"
	        "zu\nidle_read_operations=%llu\nidle_read_bytes=%llu\nidle_"
	        "write_operations=%llu\nidle_write_bytes=%llu\nworking_set_"
	        "bytes=%zu\nprivate_bytes=%zu\nsubtree_loading_flashes=%"
	        "zu\nsubtree_repeated_text=%zu\nsubtree_read_operations=%"
	        "llu\nsubtree_read_bytes=%llu\nsubtree_other_operations=%llu\n",
	        elapsed, (double)(cpu_after - cpu_before) / 10000.0,
	        idle_end - idle_begin,
	        (unsigned long long)(io_after.ReadOperationCount -
	                io_before.ReadOperationCount),
	        (unsigned long long)(io_after.ReadTransferCount -
	                io_before.ReadTransferCount),
	        (unsigned long long)(io_after.WriteOperationCount -
	                io_before.WriteOperationCount),
	        (unsigned long long)(io_after.WriteTransferCount -
	                io_before.WriteTransferCount),
	        (size_t)memory.WorkingSetSize, (size_t)memory.PrivateUsage,
	        flashes, repeated_text,
	        (unsigned long long)(io_noise.ReadOperationCount -
	                io_after.ReadOperationCount),
	        (unsigned long long)(io_noise.ReadTransferCount -
	                io_after.ReadTransferCount),
	        (unsigned long long)(io_noise.OtherOperationCount -
	                io_after.OtherOperationCount));
	ev_write_text(e, L"artifacts\\preview-metrics.txt", metrics);
	ev_check(e, "idle_output_zero", 0, "0 output bytes", metrics,
	        idle_end == idle_begin && elapsed >= 30000);
	ev_check(e, "unrelated_subtree_keeps_preview", 0, "0 loading flashes",
	        metrics, !flashes);
	product_key_latency(&pr);
	product_sync_output_check(&pr);
	WideCharToMultiByte(CP_UTF8, 0, pr.browse, -1, pr.path8,
	        sizeof pr.path8, NULL, NULL);
	if (!product_exit(&pr)) {
		goto done;
	}
	rc = e->fails ? 1 : 0;
done:
	product_teardown(&pr);
	return rc;
}

/* Graceful console event, independent of Smart Go's raw Ctrl+C key binding. */
static int scn_shutdown_go(Ev *e)
{
	Product pr;
	DWORD code = 99;
	char outcome[96];
	bool exited;
	int rc = 1;

	memset(&pr, 0, sizeof pr);
	pr.e = e;
	if (ERROR_SUCCESS != wf_test_assoc_name(&open_association) ||
	        !p0_lab_open(&pr.lab, "shutdown-go", 1)) {
		ev_check(e, "isolated_fixture", 0, "owned NTFS root",
		        "unavailable", 0);
		return 1;
	}
	if (!product_setup(&pr)) {
		goto done;
	}
	pty_send(&pr.p, "\x0c", 1);
	if (!product_wait(&pr.p, "Go to:", pr.screen, sizeof pr.screen)) {
		ev_check(e, "shutdown_go_prompt", 0, "Smart Go open", "missing",
		        0);
		goto done;
	}
	product_snapshot(&pr, "shutdown_go_prompt");
	HANDLE saved[3] = {GetStdHandle(STD_INPUT_HANDLE),
	        GetStdHandle(STD_OUTPUT_HANDLE),
	        GetStdHandle(STD_ERROR_HANDLE)};
	FreeConsole();
	BOOL attached = AttachConsole(pr.p.pi.dwProcessId);
	BOOL signalled = attached &&
	        SetConsoleCtrlHandler(keep_test_host_alive, TRUE) &&
	        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
	ev_check(e, "shutdown_event_delivery", 0, "console event sent",
	        signalled ? "sent" : "unavailable", signalled);
	exited = WAIT_OBJECT_0 == WaitForSingleObject(pr.p.pi.hProcess, 8000);
	if (attached) {
		FreeConsole();
	}
	SetStdHandle(STD_INPUT_HANDLE, saved[0]);
	SetStdHandle(STD_OUTPUT_HANDLE, saved[1]);
	SetStdHandle(STD_ERROR_HANDLE, saved[2]);
	if (exited) {
		exited = GetExitCodeProcess(pr.p.pi.hProcess, &code) != 0;
	}
	snprintf(outcome, sizeof outcome, "exited=%d code=0x%08lx", (int)exited,
	        code);
	ev_check(e, "smart_go_interrupt_exit", 0, "exit 0", outcome,
	        exited && !code);
	Sleep(30);
	EnterCriticalSection(&pr.p.lock);
	ev_check(e, "smart_go_interrupt_restore", 0, "alternate screen leave",
	        contains(pr.p.raw, pr.p.raw_n, "\x1b[?1049l") ? "present"
	                                                      : "missing",
	        contains(pr.p.raw, pr.p.raw_n, "\x1b[?1049l"));
	LeaveCriticalSection(&pr.p.lock);
	rc = e->fails ? 1 : 0;
done:
	product_teardown(&pr);
	return rc;
}
/* the recovery-gate scenario uses the macros above */
#include "p0_state_gate.c"
#include "p0_listing.c"
#include "p0_dirsize.c"
#include "p0_session.c"
#include "p0_watch.c"
#include "p0_clipboard.c"
#include "p0_lock.c"
#include "p0_markdown.c"
#include "p0_fuzzy.c"
#include "p0_batch.c"
#include "p0_ntfsfacts.c"
#include "p0_hash.c"
#include "p0_tabs.c"
#include "p0_openterm.c"
#include "p0_resources.c"
#include "p0_git.c"
#include "p0_images.c"
#include "p0_search.c"
#include "p0_syntax.c"

#undef PRODUCT_STEP_RETRY
#undef PRODUCT_STEP
#undef PRODUCT_REQUIRE

static void child_cmd(wchar_t *out,
        size_t cap,
        const char *mode,
        const wchar_t *log,
        DWORD timeout)
{
	swprintf(out, cap, L"\"%ls\" --mode %hs --log \"%ls\" --timeout-ms %lu",
	        g_child, mode, log, timeout);
}

#define SENDS(p, s) pty_send((p), (s), sizeof(s) - 1)

static int scn_input(Ev *e, const wchar_t *tracedir)
{
	Pty pty;
	wchar_t log[MAX_PATH * 2], cmd[2048];
	double t_esc, t_esc_seen;
	size_t n, i;
	int downs = 0, ups = 0, ja = 0, hi = 0, lo = 0, cp_events = 0;

	swprintf(log, MAX_PATH * 2, L"%ls\\input_log.csv", tracedir);
	child_cmd(cmd, 2048, "log-input", log, 30000);
	if (!pty_start(&pty, cmd, 100, 30)) {
		ev_check(e, "start", 0, "child starts", "failed", 0);
		return 1;
	}
	if (wait_for(log, "OPENED", 0, 5000) < 0) {
		ev_check(e, "start", 0, "OPENED logged", "timeout", 0);
		pty_finish(&pty);
		return 1;
	}
	Sleep(300);
	SENDS(&pty, "abc");
	Sleep(150);
	SENDS(&pty, "\x1b[A\x1b[B\x1b[C\x1b[D");
	Sleep(150);
	SENDS(&pty, "\x1bOQ");
	Sleep(150); /* F2 */
	SENDS(&pty, "\r\t\x7f");
	Sleep(150);
	SENDS(&pty, "/?.");
	Sleep(150);
	SENDS(&pty, "\x01");
	Sleep(150); /* Ctrl+A */
	SENDS(&pty, "\xE3\x81\xAF");
	Sleep(150); /* U+306F */
	SENDS(&pty, "\xF0\x9F\x98\x80");
	Sleep(150); /* U+1F600 */
	SENDS(&pty, "yyyyy");
	Sleep(150);
	pty_resize(&pty, 90, 25);
	Sleep(300);
	t_esc = SENDS(&pty, "\x1b");
	t_esc_seen = wait_for(
	        log, "KEY", t_esc, 1500); /* first KEY after the lone Esc */
	Sleep(300);
	SENDS(&pty, "Q");
	WaitForSingleObject(pty.pi.hProcess, 5000);

	n = read_log(log, g_lines, WF_ARRAY_LEN(g_lines));
	for (i = 0; i < n; i++) {
		if (strcmp(g_lines[i].kind, "KEY")) {
			continue;
		}
		{
			int down = '1' == g_lines[i].rest[0];
			unsigned uc = 0;
			const char *p = g_lines[i].rest;
			int commas = 0;

			while (*p && commas < 4) {
				if (*p++ == ',') {
					commas++;
				}
			}
			uc = (unsigned)strtoul(p, NULL, 16); /* "0x...." */
			downs += down;
			ups += !down;
			ja += uc == 0x306F;
			hi += uc == 0xD83D;
			lo += uc == 0xDE00;
			cp_events += uc >= 0x20;
		}
	}
	{
		char o[160], ex[96];

		snprintf(o, sizeof o, "down=%d up=%d", downs, ups);
		ev_check(e, "keyup_records_exist_for_vt_text", 0,
		        "up>0 (informational)", o, ups > 0);
		snprintf(o, sizeof o, "U+306F records=%d", ja);
		ev_check(e, "japanese_char_delivered", 0, ">=1", o, ja >= 1);
		snprintf(o, sizeof o, "hi=%d lo=%d", hi, lo);
		ev_check(e, "emoji_arrives_as_surrogate_pair_records", 0,
		        "hi>=1 and lo>=1", o, hi >= 1 && lo >= 1);
		snprintf(ex, sizeof ex, "Esc latency ms");
		snprintf(o, sizeof o, "%.1f",
		        t_esc_seen < 0 ? -1.0 : t_esc_seen - t_esc);
		ev_check(e, "lone_esc_latency_ms", 0, "<=250", o,
		        t_esc_seen >= 0 && t_esc_seen - t_esc <= 250.0);
		snprintf(o, sizeof o, "%d printable-char records", cp_events);
		ev_check(e, "printable_records", 0, ">0", o, cp_events > 0);
	}
	{
		wchar_t p[MAX_PATH * 2];

		swprintf(p, MAX_PATH * 2, L"%ls\\output.vt", tracedir);
		(void)p;
	}
	EnterCriticalSection(&pty.lock);
	ev_write_raw(e, L"traces\\output.vt", pty.raw, pty.raw_n);
	LeaveCriticalSection(&pty.lock);
	pty_finish(&pty);
	return 0;
}

/* Waits until the modal has been presented after `after`; returns its time or
 * -1. */
static double wait_presented(const wchar_t *log, double after)
{
	return wait_for(log, "PRESENTED", after, 3000);
}

static int scn_confirm(Ev *e, const wchar_t *tracedir, int reps)
{
	Pty pty;
	wchar_t log[MAX_PATH * 2], cmd[2048];
	int r;

	swprintf(log, MAX_PATH * 2, L"%ls\\confirm_log.csv", tracedir);
	child_cmd(cmd, 2048, "confirm-test", log, 120000);
	if (!pty_start(&pty, cmd, 100, 30) ||
	        wait_for(log, "READY", 0, 5000) < 0) {
		ev_check(e, "start", 0, "child ready", "failed", 0);
		return 1;
	}
	Sleep(300);

	for (r = 0; r < reps; r++) {
		double t0, tp, te;
		int yes;
		char o[96];

		/* A: Y typed ahead, in the same write as the trigger */
		t0 = now_ms();
		SENDS(&pty, "xY");
		tp = wait_presented(log, t0);
		Sleep(80);
		te = now_ms();
		yes = count_kind(log, "YES_FIRED", t0, te);
		snprintf(o, sizeof o, "presented=%s yes=%d",
		        tp < 0 ? "no" : "yes", yes);
		ev_check(e, "A_typeahead_Y_same_write", r, "modal open, no YES",
		        o, tp >= 0 && 0 == yes);
		SENDS(&pty, "\x1b");
		Sleep(250);
		if (strcmp(mode_at(log, now_ms()),
		            "NORMAL")) { /* make sure we are reset */
			SENDS(&pty, "\x1b");
			Sleep(250);
		}

		/* B: y inside the guard window */
		t0 = now_ms();
		SENDS(&pty, "x");
		tp = wait_presented(log, t0);
		Sleep(60);
		SENDS(&pty, "y");
		Sleep(120);
		te = now_ms();
		yes = count_kind(log, "YES_FIRED", t0, te);
		snprintf(o, sizeof o, "yes=%d", yes);
		ev_check(e, "B_y_inside_guard", r, "no YES", o,
		        tp >= 0 && 0 == yes);
		/* C: after the guard the same key must work (positive control)
		 */
		Sleep(450);
		SENDS(&pty, "y");
		Sleep(200);
		te = now_ms();
		yes = count_kind(log, "YES_FIRED", t0, te);
		snprintf(o, sizeof o, "yes=%d", yes);
		ev_check(e, "C_y_after_guard_positive_control", r,
		        "exactly 1 YES", o, 1 == yes);
		Sleep(150);

		/* D: Enter never confirms */
		t0 = now_ms();
		SENDS(&pty, "x");
		tp = wait_presented(log, t0);
		Sleep(450);
		SENDS(&pty, "\r");
		Sleep(200);
		yes = count_kind(log, "YES_FIRED", t0, now_ms());
		snprintf(o, sizeof o, "yes=%d mode=%s", yes,
		        mode_at(log, now_ms()));
		ev_check(e, "D_enter_cancels", r, "no YES, back to NORMAL", o,
		        0 == yes && !strcmp(mode_at(log, now_ms()), "NORMAL"));

		/* E: Esc inside the guard cancels */
		t0 = now_ms();
		SENDS(&pty, "x");
		tp = wait_presented(log, t0);
		Sleep(40);
		te = SENDS(&pty, "\x1b");
		Sleep(350);
		snprintf(o, sizeof o, "mode=%s", mode_at(log, now_ms()));
		ev_check(e, "E_esc_inside_guard", r, "NORMAL", o,
		        !strcmp(mode_at(log, now_ms()), "NORMAL"));
		(void)te;

		/* F: trigger key auto-repeat after the modal opened */
		t0 = now_ms();
		SENDS(&pty, "x");
		tp = wait_presented(log, t0);
		SENDS(&pty, "xxxxxxxx");
		Sleep(500);
		yes = count_kind(log, "YES_FIRED", t0, now_ms());
		snprintf(o, sizeof o, "yes=%d mode=%s", yes,
		        mode_at(log, now_ms()));
		ev_check(e, "F_repeated_trigger_does_not_confirm", r, "no YES",
		        o, 0 == yes);
		SENDS(&pty, "\x1b");
		Sleep(250);

		/* G: a confirmed Japanese character where Y/N is expected */
		t0 = now_ms();
		SENDS(&pty, "x");
		tp = wait_presented(log, t0);
		Sleep(450);
		SENDS(&pty, "\xE3\x81\xAF"); /* は */
		Sleep(200);
		yes = count_kind(log, "YES_FIRED", t0, now_ms());
		snprintf(o, sizeof o, "yes=%d mode=%s", yes,
		        mode_at(log, now_ms()));
		ev_check(e, "G_japanese_char_not_yes", r,
		        "no YES, still CONFIRM", o,
		        0 == yes && !strcmp(mode_at(log, now_ms()), "CONFIRM"));
		SENDS(&pty, "\x1b");
		Sleep(300);
	}
	SENDS(&pty, "\x03");
	WaitForSingleObject(pty.pi.hProcess, 3000);
	EnterCriticalSection(&pty.lock);
	ev_write_raw(e, L"traces\\output.vt", pty.raw, pty.raw_n);
	LeaveCriticalSection(&pty.lock);
	pty_finish(&pty);
	return 0;
}

static int scn_resize(Ev *e, const wchar_t *tracedir)
{
	static const int sizes[][2] = {{120, 30}, {80, 24}, {60, 20}, {40, 15},
	        {40, 12}, {44, 20}, {30, 10}, {20, 8}, {20, 7}, {20, 5},
	        {19, 24}};
	Pty pty;
	wchar_t log[MAX_PATH * 2], cmd[2048];
	size_t i;
	static char snap[16384];

	swprintf(log, MAX_PATH * 2, L"%ls\\resize_log.csv", tracedir);
	child_cmd(cmd, 2048, "confirm-test", log, 60000);
	if (!pty_start(&pty, cmd, 100, 30) ||
	        wait_for(log, "READY", 0, 5000) < 0) {
		ev_check(e, "start", 0, "child ready", "failed", 0);
		return 1;
	}
	Sleep(300);
	{
		double t0 = now_ms();

		SENDS(&pty, "x");
		wait_presented(log, t0);
		Sleep(450); /* armed */
	}
	for (i = 0; i < WF_ARRAY_LEN(sizes); i++) {
		int cols = sizes[i][0], rows = sizes[i][1];
		char name[32], o[128];
		wchar_t rel[64];
		int has_verb, has_dest, has_fallback, ok, small;
		double t = now_ms();

		pty_resize(&pty, cols, rows);
		wait_for(log, "PRESENTED", t, 2000);
		Sleep(250);
		pty_snapshot(&pty, snap, sizeof snap);
		snprintf(name, sizeof name, "%dx%d", cols, rows);
		swprintf(rel, 64, L"artifacts\\screen_%hs.txt", name);
		ev_write_text(e, rel, snap);

		has_verb = NULL != strstr(snap, "MOVE");
		has_dest = NULL != strstr(snap, "Archive");
		has_fallback = NULL != strstr(snap, "CONFIRM: Y/N");
		/* T06 PASS (design 5.1): at 20x8 and up the verb and the
		 * destination stay visible; below that the full-screen
		 * "CONFIRM: Y/N" notice replaces the modal. Never a clipped
		 * approval control. */
		small = cols < 20 || rows < 8;
		ok = small ? has_fallback
		           : (has_verb && has_dest && !has_fallback);
		snprintf(o, sizeof o, "verb=%d destination=%d fallback=%d",
		        has_verb, has_dest, has_fallback);
		ev_check(e, name, 0,
		        small ? "full-screen CONFIRM: Y/N"
		              : "verb+destination visible",
		        o, ok);
	}
	SENDS(&pty, "\x03");
	WaitForSingleObject(pty.pi.hProcess, 3000);
	pty_finish(&pty);
	return 0;
}

/* kind: 0 idle, 1 stall (main never answers), 2 slowclean (long teardown after
 * the restore) */
static int scn_close(Ev *e, const wchar_t *tracedir, int reps, int kind)
{
	const int stall = 1 == kind;
	int r;

	for (r = 0; r < reps; r++) {
		Pty pty;
		wchar_t log[MAX_PATH * 2], cmd[2048];
		double t_close, t_exit, t_sd = -1, t_tc = -1;
		DWORD code = 0, wr;
		int restored;
		char o[160];
		size_t n, i;

		swprintf(log, MAX_PATH * 2, L"%ls\\close_log_%02d.csv",
		        tracedir, r);
		child_cmd(cmd, 2048,
		        1 == kind           ? "stall"
		                : 2 == kind ? "slowclean"
		                            : "idle",
		        log, 60000);
		if (!pty_start(&pty, cmd, 100, 30) ||
		        wait_for(log, 1 == kind ? "STALLED" : "READY", 0,
		                5000) < 0) {
			ev_check(e, "start", r, "child ready", "failed", 0);
			continue;
		}
		Sleep(400);
		t_close = now_ms();
		ClosePseudoConsole(
		        pty.pc); /* what closing the terminal tab/window does */
		pty.pc = NULL;
		wr = WaitForSingleObject(pty.pi.hProcess, 8000);
		t_exit = now_ms();
		GetExitCodeProcess(pty.pi.hProcess, &code);
		WaitForSingleObject(pty.reader, 2000);

		n = read_log(log, g_lines, WF_ARRAY_LEN(g_lines));
		for (i = 0; i < n; i++) {
			if (!strcmp(g_lines[i].kind, "SHUTDOWN_EVENT")) {
				t_sd = g_lines[i].t;
			}
			if (!strcmp(g_lines[i].kind, "TERM_CLOSED")) {
				t_tc = g_lines[i].t;
			}
		}
		EnterCriticalSection(&pty.lock);
		restored = contains(pty.raw, pty.raw_n, "\x1b[?1049l");
		LeaveCriticalSection(&pty.lock);

		snprintf(o, sizeof o,
		        "exited=%d exit_ms=%.1f shutdown_event_seen_ms=%.1f "
		        "restore_seq=%d code=%lu",
		        WAIT_OBJECT_0 == wr, t_exit - t_close,
		        t_sd < 0 ? -1.0 : t_sd - t_close, restored, code);
		if (stall) {
			/* main never answers: the handler's bound (3 s) must
			 * end it, not a hang */
			ev_check(e, "stalled_main_exit_bounded", r,
			        "exits in 2500..5000 ms (handler bound)", o,
			        WAIT_OBJECT_0 == wr &&
			                t_exit - t_close >= 2500.0 &&
			                t_exit - t_close < 5000.0);
			pty_finish(&pty);
			continue;
		}
		ev_check(e, "close_event_bounded_exit", r,
		        "exits <5000ms, shutdown event seen", o,
		        WAIT_OBJECT_0 == wr && t_exit - t_close < 5000.0 &&
		                t_sd >= 0);
		snprintf(o, sizeof o,
		        "TERM_CLOSED logged=%d restore sequence emitted=%d",
		        t_tc >= 0, restored);
		ev_check(e, "terminal_restore_attempted", r,
		        "restore attempted", o, t_tc >= 0);
		ev_check(e,
		        2 == kind ? "slow_teardown_does_not_hold_handler"
		                  : "restore_before_exit_fast",
		        r,
		        "forced close exits <1500 ms once the console is "
		        "restored, however long teardown would take",
		        o,
		        WAIT_OBJECT_0 == wr && t_exit - t_close < 1500.0 &&
		                restored);
		pty_finish(&pty);
	}
	return 0;
}

static int scn_ctrlc(Ev *e, const wchar_t *tracedir, int reps)
{
	int r;

	for (r = 0; r < reps; r++) {
		Pty pty;
		wchar_t log[MAX_PATH * 2], cmd[2048];
		double t_send, t_sd, t_exit;
		DWORD wr;
		char o[128];

		swprintf(log, MAX_PATH * 2, L"%ls\\ctrlc_log_%02d.csv",
		        tracedir, r);
		child_cmd(cmd, 2048, "idle", log, 60000);
		if (!pty_start(&pty, cmd, 100, 30) ||
		        wait_for(log, "READY", 0, 5000) < 0) {
			ev_check(e, "start", r, "child ready", "failed", 0);
			continue;
		}
		Sleep(400);
		t_send = SENDS(&pty, "\x03");
		wr = WaitForSingleObject(pty.pi.hProcess, 4000);
		t_exit = now_ms();
		t_sd = wait_for(log, "SHUTDOWN_EVENT", t_send, 100);
		snprintf(o, sizeof o,
		        "exited=%d shutdown_event_ms=%.1f exit_ms=%.1f",
		        WAIT_OBJECT_0 == wr, t_sd < 0 ? -1.0 : t_sd - t_send,
		        t_exit - t_send);
		ev_check(e, "ctrl_c_reaches_handler_and_exits", r,
		        "exit via shutdown event", o,
		        WAIT_OBJECT_0 == wr && t_sd >= 0);
		pty_finish(&pty);
	}
	return 0;
}

int wmain(int argc, wchar_t **argv)
{
	wchar_t results[MAX_PATH * 2] = L"", label[48] = L"conpty-win11",
	                           scenario[16] = L"", work[MAX_PATH * 2];
	int i, reps = 20, rc = 1;
	Ev ev;
	wchar_t trace[MAX_PATH * 2];
	const char *test = "P0-T01";

	QueryPerformanceFrequency(&g_freq);
	for (i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--scenario") && i + 1 < argc) {
			wcscpy_s(scenario, 16, argv[++i]);
		} else if (!wcscmp(argv[i], L"--child") && i + 1 < argc) {
			wcscpy_s(g_child, MAX_PATH * 2, argv[++i]);
		} else if (!wcscmp(argv[i], L"--results") && i + 1 < argc) {
			wcscpy_s(results, MAX_PATH * 2, argv[++i]);
		} else if (!wcscmp(argv[i], L"--helper") && i + 1 < argc) {
			wcscpy_s(g_lock_helper, MAX_PATH * 2, argv[++i]);
		} else if (!wcscmp(argv[i], L"--label") && i + 1 < argc) {
			wcscpy_s(label, 48, argv[++i]);
		} else if (!wcscmp(argv[i], L"--reps") && i + 1 < argc) {
			reps = _wtoi(argv[++i]);
		}
	}
	if (!scenario[0] || !g_child[0] || !results[0]) {
		fputs("usage: p0_conpty_host --scenario "
		      "input|confirm|resize|close|closestall|closeslow|ctrlc|"
		      "shutdown-go|state-gate|first-run|pictures|listing|"
		      "dirsize|continue|watch|clipboard|lock|ntfsfacts|hash|"
		      "tabs|search|dupes|terminal-here|resources|git|images|"
		      "preview-idle"
		      " --child <p0_terminal.exe> --results <root> [--label x] "
		      "[--reps n]\n",
		        stderr);
		return 2;
	}
	GetCurrentDirectoryW(MAX_PATH * 2, work);

	if (!wcscmp(scenario, L"product") || !wcscmp(scenario, L"file-ops") ||
	        !wcscmp(scenario, L"ui-audit") ||
	        !wcscmp(scenario, L"keyboard") ||
	        !wcscmp(scenario, L"shutdown-go") ||
	        !wcscmp(scenario, L"state-gate") ||
	        !wcscmp(scenario, L"first-run") ||
	        !wcscmp(scenario, L"pictures") ||
	        !wcscmp(scenario, L"listing") ||
	        !wcscmp(scenario, L"dirsize") ||
	        !wcscmp(scenario, L"continue") || !wcscmp(scenario, L"watch") ||
	        !wcscmp(scenario, L"clipboard") || !wcscmp(scenario, L"lock") ||
	        !wcscmp(scenario, L"markdown") || !wcscmp(scenario, L"fuzzy") ||
	        !wcscmp(scenario, L"batch") ||
	        !wcscmp(scenario, L"ntfsfacts") || !wcscmp(scenario, L"hash") ||
	        !wcscmp(scenario, L"tabs") || !wcscmp(scenario, L"search") ||
	        !wcscmp(scenario, L"dupes") || !wcscmp(scenario, L"syntax") ||
	        !wcscmp(scenario, L"terminal-here") ||
	        !wcscmp(scenario, L"resources") || !wcscmp(scenario, L"git") ||
	        !wcscmp(scenario, L"images") ||
	        !wcscmp(scenario, L"preview-idle")) {
		test = "PRODUCT-TUI";
	} else if (!wcscmp(scenario, L"confirm")) {
		test = "P0-T05";
	} else if (!wcscmp(scenario, L"resize")) {
		test = "P0-T06";
	} else if (!wcscmp(scenario, L"close") ||
	        !wcscmp(scenario, L"closestall") ||
	        !wcscmp(scenario, L"closeslow")) {
		test = "P0-T08";
	}
	{
		char s8[16];

		WideCharToMultiByte(
		        CP_UTF8, 0, scenario, -1, s8, sizeof s8, NULL, NULL);
		if (!ev_open(&ev, results, test, s8, label)) {
			fputs("refusing existing/incomplete evidence run\n",
			        stderr);
			return 2;
		}
	}
	swprintf(trace, MAX_PATH * 2, L"%ls\\traces", ev.dir);

	if (!wcscmp(scenario, L"product") || !wcscmp(scenario, L"ui-audit")) {
		rc = scn_product(&ev,
		        !wcscmp(scenario, L"ui-audit") ? PRODUCT_UI
		                                       : PRODUCT_FULL);
	} else if (!wcscmp(scenario, L"keyboard")) {
		rc = scn_keyboard(&ev);
	} else if (!wcscmp(scenario, L"file-ops")) {
		rc = scn_product(&ev, PRODUCT_FILE_OPS);
	} else if (!wcscmp(scenario, L"pictures")) {
		rc = scn_product(&ev, PRODUCT_PICTURES);
	} else if (!wcscmp(scenario, L"state-gate")) {
		rc = scn_state_gate(&ev);
	} else if (!wcscmp(scenario, L"first-run")) {
		rc = scn_first_run(&ev);
	} else if (!wcscmp(scenario, L"listing")) {
		rc = scn_listing(&ev);
	} else if (!wcscmp(scenario, L"dirsize")) {
		rc = scn_dirsize(&ev);
	} else if (!wcscmp(scenario, L"continue")) {
		rc = scn_session(&ev);
	} else if (!wcscmp(scenario, L"watch")) {
		rc = scn_watch(&ev);
	} else if (!wcscmp(scenario, L"clipboard")) {
		rc = scn_clipboard(&ev);
	} else if (!wcscmp(scenario, L"lock")) {
		rc = scn_lock(&ev);
	} else if (!wcscmp(scenario, L"markdown")) {
		rc = scn_markdown(&ev);
	} else if (!wcscmp(scenario, L"fuzzy")) {
		rc = scn_fuzzy(&ev);
	} else if (!wcscmp(scenario, L"batch")) {
		rc = scn_batch(&ev);
	} else if (!wcscmp(scenario, L"ntfsfacts")) {
		rc = scn_ntfsfacts(&ev);
	} else if (!wcscmp(scenario, L"hash")) {
		rc = scn_hash(&ev);
	} else if (!wcscmp(scenario, L"tabs")) {
		rc = scn_tabs(&ev);
	} else if (!wcscmp(scenario, L"terminal-here")) {
		rc = scn_openterm(&ev);
	} else if (!wcscmp(scenario, L"resources")) {
		rc = scn_resources(&ev);
	} else if (!wcscmp(scenario, L"git")) {
		rc = scn_git(&ev);
	} else if (!wcscmp(scenario, L"images")) {
		rc = scn_images(&ev);
	} else if (!wcscmp(scenario, L"search")) {
		rc = scn_search(&ev);
	} else if (!wcscmp(scenario, L"dupes")) {
		rc = scn_dupes(&ev);
	} else if (!wcscmp(scenario, L"syntax")) {
		rc = scn_syntax(&ev);
	} else if (!wcscmp(scenario, L"preview-idle")) {
		rc = scn_preview_idle(&ev);
	} else if (!wcscmp(scenario, L"shutdown-go")) {
		rc = scn_shutdown_go(&ev);
	} else if (!wcscmp(scenario, L"input")) {
		rc = scn_input(&ev, trace);
	} else if (!wcscmp(scenario, L"confirm")) {
		rc = scn_confirm(&ev, trace, reps);
	} else if (!wcscmp(scenario, L"resize")) {
		rc = scn_resize(&ev, trace);
	} else if (!wcscmp(scenario, L"close")) {
		rc = scn_close(&ev, trace, reps > 10 ? 10 : reps, 0);
	} else if (!wcscmp(scenario, L"closeslow")) {
		rc = scn_close(&ev, trace, reps > 5 ? 5 : reps, 2);
	} else if (!wcscmp(scenario, L"closestall")) {
		rc = scn_close(&ev, trace, reps > 5 ? 5 : reps, 1);
	} else if (!wcscmp(scenario, L"ctrlc")) {
		rc = scn_ctrlc(&ev, trace, reps > 10 ? 10 : reps);
	}

	{
		static const char *const hyp_t01[] = {"H-023", "H-024", NULL};
		static const char *const hyp_t05[] = {"H-023", "H-024", NULL};
		static const char *const hyp_t06[] = {"H-025", NULL};
		static const char *const hyp_t08[] = {"H-027", NULL};
		static const char *const dec[] = {"D-023", "D-025", NULL};
		const char *const *hyp = !strcmp(test, "P0-T05") ? hyp_t05
		        : !strcmp(test, "P0-T06")                ? hyp_t06
		        : !strcmp(test, "P0-T08")                ? hyp_t08
		                                                 : hyp_t01;
		char notes[640];

		/* The plan's environments are Windows Terminal / conhost. This
		 * run used ConPTY driven by a test host, so it cannot be PASS
		 * evidence for them: BLOCKED, with the measurements kept as
		 * observations. */
		snprintf(notes, sizeof notes,
		        "ConPTY host observations only (checks: %d pass, %d "
		        "fail). NOT Windows Terminal, "
		        "NOT a conhost window; IME and real key repeat are not "
		        "exercised. Plan environments "
		        "remain NOT TESTED.",
		        ev.passes, ev.fails);
		ev_close(&ev, test, "conpty", hyp, dec,
		        !strcmp(test, "PRODUCT-TUI")
		                ? (77 == rc ? "BLOCKED"
		                                  : ev.fails ? "FAIL"
		                                             : "PASS")
		                : "BLOCKED",
		        notes, work);
	}
	printf("%s: %d checks passed, %d failed\nevidence: %ls\n", test,
	        ev.passes, ev.fails, ev.dir);
	return rc;
}

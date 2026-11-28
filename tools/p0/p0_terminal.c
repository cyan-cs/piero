/*
 * P0-TERMINAL evidence collector, the program that runs *inside* the terminal.
 *
 * It uses the production console layer (wf_term) and the production CONFIRM
 * logic (wf_app), so what it observes is what wfile would experience. It only
 * adds timestamped logging.
 *
 *   --mode log-input     log every raw console input record (T01, T02)
 *   --mode confirm-test  a CONFIRM modal opened by 'x'; logs every decision
 * (T05, T06)
 *   --mode idle          sit in the real event loop (T08)
 *   --mode slowclean     idle, then a long teardown after the restore (T08)
 *   --mode stall         never service the shutdown event (T08 bound)
 *
 *   --log <csv>          where to write the event log
 *   --timeout-ms <n>     safety exit (default 120000)
 *
 * Log lines: t_ms,kind,a,b,c,d,e,f  (t_ms from QueryPerformanceCounter).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "p0_common.h"
#include "render/wf_present.h"
#include "term/wf_term.h"
#include "ui/wf_app.h"
#include "base/wf_macro.h"

static FILE *g_log;
static LARGE_INTEGER g_freq;

static double now_ms(void)
{
	LARGE_INTEGER t;

	QueryPerformanceCounter(&t);
	/* absolute QPC time, so the host process can compare it with its own */
	return (double)t.QuadPart * 1000.0 / (double)g_freq.QuadPart;
}

static void logf_(const char *kind, const char *fmt, ...)
{
	va_list ap;

	fprintf(g_log, "%.3f,%s,", now_ms(), kind);
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fputc('\n', g_log);
	fflush(g_log);
}

static void log_record(const INPUT_RECORD *r)
{
	switch (r->EventType) {
	case KEY_EVENT: {
		const KEY_EVENT_RECORD *k = &r->Event.KeyEvent;

		logf_("KEY", "%d,%u,0x%02x,0x%02x,0x%04x,0x%lx",
		        k->bKeyDown ? 1 : 0, k->wRepeatCount,
		        k->wVirtualKeyCode, k->wVirtualScanCode,
		        (unsigned)k->uChar.UnicodeChar, k->dwControlKeyState);
		break;
	}
	case WINDOW_BUFFER_SIZE_EVENT:
		logf_("RESIZE", "%d,%d,,,,",
		        r->Event.WindowBufferSizeEvent.dwSize.X,
		        r->Event.WindowBufferSizeEvent.dwSize.Y);
		break;
	case FOCUS_EVENT:
		logf_("FOCUS", "%d,,,,,",
		        r->Event.FocusEvent.bSetFocus ? 1 : 0);
		break;
	case MENU_EVENT: logf_("MENU", ",,,,,"); break;
	case MOUSE_EVENT:
		logf_("MOUSE",
		        "unexpected: mouse input should be disabled,,,,,");
		break;
	}
}

static int mode_log_input(WfTerm *t, DWORD timeout_ms)
{
	HANDLE w[2] = {wf_term_input_handle(t), wf_term_shutdown_event(t)};
	double deadline = now_ms() + timeout_ms;

	for (;;) {
		double left = deadline - now_ms();
		DWORD r;

		if (left <= 0) {
			return 0;
		}
		r = WaitForMultipleObjects(2, w, FALSE, (DWORD)left);
		if (r == WAIT_OBJECT_0 + 1) {
			logf_("SHUTDOWN_EVENT", ",,,,,");
			return 0;
		}
		if (WAIT_OBJECT_0 == r) {
			INPUT_RECORD rec[64];
			DWORD n = 0, i;

			if (!ReadConsoleInputW(w[0], rec, 64, &n)) {
				return 1;
			}
			for (i = 0; i < n; i++) {
				log_record(&rec[i]);
				/* 'Q' ends a manual session */
				if (KEY_EVENT == rec[i].EventType &&
				        rec[i].Event.KeyEvent.bKeyDown &&
				        rec[i].Event.KeyEvent.uChar
				                        .UnicodeChar == L'Q') {
					return 0;
				}
			}
		}
	}
}

static WfStatus no_answer(
        void *ctx, uint32_t gen, const wchar_t *path, const wchar_t *parent)
{
	(void)ctx;
	(void)gen;
	(void)path;
	(void)parent;
	return WF_OK; /* listings never arrive: the screen stays "loading", the
	                 modal is all we test */
}

static void on_yes(WfApp *app, void *ctx)
{
	(void)app;
	(void)ctx;
	logf_("YES_FIRED", ",,,,,");
}

typedef struct
{
	WfTerm *term;
	WfApp app;
	WfFrame *prev;
	WfBuf out;
} Rig;

static int render(Rig *r)
{
	WfFrame *next = wf_frame_new(r->app.cols, r->app.rows, &WF_STYLE_BASE);
	WfStatus st;

	if (!next) {
		return 0;
	}
	wf_app_draw(&r->app, next);
	wf_buf_clear(&r->out);
	st = wf_present_diff(r->prev, next, &r->out);
	if (WF_OK == st) {
		st = wf_term_write(r->term, wf_buf_cstr(&r->out), r->out.len);
	}
	wf_frame_free(r->prev);
	r->prev = WF_OK == st ? next : NULL;
	if (WF_OK != st) {
		wf_frame_free(next);
	}
	return WF_OK == st;
}

static void log_state(const Rig *r, const char *why)
{
	logf_("STATE", "%s,%s,%d,%d,%d,%d", why, r->app.mode->name,
	        &WF_MODE_CONFIRM == r->app.mode ? (int)r->app.confirm.state
	                                        : -1,
	        r->app.cols, r->app.rows, r->app.flush_input);
}

static void open_modal(Rig *r, const WfEvent *trigger)
{
	static const char *const lines[] = {"MOVE", "Move 2 selected items?",
	        "Destination:", "C:\\Archive\\2026"};

	if (WF_OK ==
	        wf_app_confirm_begin(
	                &r->app, lines, 4, on_yes, NULL, trigger)) {
		logf_("MODAL_OPENED", "trigger_vk=0x%02x,trigger_ch=0x%x,,,,",
		        trigger->vk, trigger->ch);
	}
}

/* Same drain-then-present-then-guard order as src/main.c. */
static int mode_confirm(WfTerm *t, DWORD timeout_ms, int idle_only)
{
	Rig r;
	HANDLE w[2] = {wf_term_input_handle(t), wf_term_shutdown_event(t)};
	double deadline = now_ms() + timeout_ms;
	int rc = 1;

	memset(&r, 0, sizeof r);
	r.term = t;
	wf_buf_init(&r.out);
	{
		WfFsPort fs = {NULL, no_answer};
		int cols = 80, rows = 24;

		wf_term_size(t, &cols, &rows);
		if (WF_OK != wf_app_init(&r.app, cols, rows, fs, L"C:\\")) {
			goto out;
		}
	}
	r.app.now_ms = (uint64_t)now_ms();
	render(&r);
	wf_app_on_presented(&r.app);
	logf_("READY", "%d,%d,,,,", r.app.cols, r.app.rows);

	for (;;) {
		double left = deadline - now_ms();
		DWORD timeout, rv;
		WfEvent ev[64];
		size_t n, i;
		int dirty = 0;

		if (left <= 0) {
			rc = 0;
			break;
		}
		timeout = wf_app_timeout(&r.app);
		if (wf_term_has_pending_input(t)) {
			timeout = 0;
		}
		if (WF_WAIT_FOREVER == timeout || timeout > (DWORD)left) {
			timeout = (DWORD)left;
		}
		rv = WaitForMultipleObjects(2, w, FALSE, timeout);
		r.app.now_ms = (uint64_t)now_ms();
		if (rv == WAIT_OBJECT_0 + 1) {
			logf_("SHUTDOWN_EVENT", ",,,,,");
			rc = 0;
			break;
		}

		n = wf_term_read_events(t, ev, 64);
		for (i = 0; i < n; i++) {
			const WfEvent *e = &ev[i];
			WfAction act;
			const char *what;
			int dropped = 0;

			if (WF_EV_RESIZE == e->type) {
				wf_term_size(t, &r.app.cols, &r.app.rows);
				logf_("RESIZE", "%d,%d,,,,", r.app.cols,
				        r.app.rows);
				dirty = 1;
				continue;
			}
			what = WF_EV_CHAR == e->type   ? "CHAR"
			        : WF_EV_KEY == e->type ? "KEY"
			                               : "RELEASE";
			if (r.app.flush_input && WF_EV_RELEASE != e->type) {
				dropped = 1; /* type-ahead after the modal
				                opened (same batch) */
			}
			logf_("EVENT", "%s,0x%x,%d,0x%02x,%d,%s", what, e->ch,
			        (int)e->key, e->vk, dropped, r.app.mode->name);
			if (dropped) {
				continue;
			}

			if (!idle_only && &WF_MODE_NORMAL == r.app.mode &&
			        WF_EV_CHAR == e->type && 'x' == e->ch) {
				open_modal(&r, e);
				dirty = 1;
				log_state(&r, "after_open");
				/* later events of this batch are type-ahead */
				continue;
			}
			act = r.app.mode->on_event(&r.app, e);
			if (WF_ACT_QUIT == act) {
				rc = 0;
				goto out;
			}
			if (WF_ACT_REDRAW == act) {
				dirty = 1;
			}
			log_state(&r, "after_event");
		}
		if (WF_ACT_REDRAW == wf_app_tick(&r.app)) {
			dirty = 1;
			log_state(&r, "armed");
		}
		if (r.app.flush_input) {
			wf_term_drain_input(t);
			logf_("DRAIN", ",,,,,");
			r.app.flush_input = 0;
		}
		if (dirty) {
			render(&r);
			r.app.now_ms = (uint64_t)now_ms();
			wf_app_on_presented(&r.app);
			if (&WF_MODE_CONFIRM == r.app.mode) {
				logf_("PRESENTED", "guard_deadline=%llu,,,,,",
				        (unsigned long long)
				                r.app.confirm.deadline);
			}
		}
	}
out:
	wf_frame_free(r.prev);
	wf_buf_free(&r.out);
	wf_app_destroy(&r.app);
	return rc;
}

/*
 * Each sample row is drawn twice: "naive" (a marker printed right after the
 * text, so a wrong width estimate drifts it) and "resynced" (the marker is
 * placed with an absolute cursor move, which is what wfile does at every pane
 * boundary). The observer records whether the resynced marker always lands
 * in column 41 and how far the naive one drifts.
 */
static int mode_width_matrix(WfTerm *t, DWORD timeout_ms)
{
	static const struct
	{
		const char *label, *text;
	} rows[] = {
	        {"ASCII", "abcdef"},
	        {"CJK",
	                "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86\xE3\x81\x88\xE3"
	                "\x81\x8A"},
	        {"fullwidth", "\xEF\xBC\xA1\xEF\xBC\xA2\xEF\xBC\xA3"},
	        {"emoji U+1F600", "\xF0\x9F\x98\x80"},
	        {"emoji ZWJ seq",
	                "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80"
	                "\x8D\xF0\x9F\x91\xA7"},
	        {"combining e+U+0301", "e\xCC\x81"},
	        {"ambiguous", "\xE2\x91\xA0\xE2\x84\x83"},
	        {"half-block", "\xE2\x96\x80\xE2\x96\x84\xE2\x96\x88"},
	};
	WfBuf b;
	HANDLE w[2] = {wf_term_input_handle(t), wf_term_shutdown_event(t)};
	double deadline = now_ms() + timeout_ms;
	size_t i;
	char line[256];

	wf_buf_init(&b);
	wf_buf_puts(&b,
	        "\x1b[2J\x1b[1;1HP0 cell-width matrix. Columns 1-40 hold the "
	        "sample; the marker "
	        "'|' must be in column 41.\x1b[2;1HPress Q to quit.");
	for (i = 0; i < WF_ARRAY_LEN(rows); i++) {
		int y = 4 + (int)i * 2;

		snprintf(line, sizeof line, "\x1b[%d;1H%-20.20s: %s|  naive", y,
		        rows[i].label, rows[i].text);
		wf_buf_puts(&b, line);
		snprintf(line, sizeof line,
		        "\x1b[%d;1H%-20.20s: %s\x1b[%d;41H|  resynced", y + 1,
		        rows[i].label, rows[i].text, y + 1);
		wf_buf_puts(&b, line);
	}
	snprintf(line, sizeof line,
	        "\x1b[%d;1Hruler: 1234567890123456789012345678901234567890|",
	        4 + (int)(WF_ARRAY_LEN(rows)) * 2 + 1);
	wf_buf_puts(&b, line);
	wf_term_write(t, wf_buf_cstr(&b), b.len);
	wf_buf_free(&b);
	logf_("MATRIX_DRAWN", ",,,,,");

	for (;;) {
		double left = deadline - now_ms();
		DWORD r;

		if (left <= 0) {
			return 0;
		}
		r = WaitForMultipleObjects(2, w, FALSE, (DWORD)left);
		if (r == WAIT_OBJECT_0 + 1) {
			return 0;
		}
		if (WAIT_OBJECT_0 == r) {
			INPUT_RECORD rec[16];
			DWORD n = 0, k;

			ReadConsoleInputW(w[0], rec, 16, &n);
			for (k = 0; k < n; k++) {
				if (KEY_EVENT == rec[k].EventType &&
				        rec[k].Event.KeyEvent.bKeyDown &&
				        (rec[k].Event.KeyEvent.uChar
				                                .UnicodeChar ==
				                        L'Q' ||
				                rec[k].Event.KeyEvent.uChar
				                                .UnicodeChar ==
				                        L'q')) {
					return 0;
				}
			}
		}
	}
}

int wmain(int argc, wchar_t **argv)
{
	wchar_t log[MAX_PATH * 2] = L"p0_terminal.csv", mode[32] = L"";
	DWORD timeout = 120000;
	WfTerm *t = NULL;
	int rc = 1, i;
	double t_close;

	for (i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--mode") && i + 1 < argc) {
			wcscpy_s(mode, 32, argv[++i]);
		} else if (!wcscmp(argv[i], L"--log") && i + 1 < argc) {
			wcscpy_s(log, MAX_PATH * 2, argv[++i]);
		} else if (!wcscmp(argv[i], L"--timeout-ms") && i + 1 < argc) {
			timeout = (DWORD)_wtoi(argv[++i]);
		}
	}
	if (!mode[0]) {
		fputs("usage: p0_terminal --mode log-input|confirm-test|idle "
		      "--log <csv>"
		      " [--timeout-ms n]\n",
		        stderr);
		return 2;
	}
	QueryPerformanceFrequency(&g_freq);
	g_log = p0_fopen_w(log);
	if (!g_log) {
		return 1;
	}
	fputs("t_ms,kind,a,b,c,d,e,f\n", g_log);

	if (WF_OK != wf_term_open(&t)) {
		logf_("ERROR", "wf_term_open failed,,,,,");
		return 1;
	}
	{
		int cols = 0, rows = 0;
		DWORD inmode = 0, outmode = 0;

		wf_term_size(t, &cols, &rows);
		HANDLE conout =
		        CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
		                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
		                OPEN_EXISTING, 0, NULL);

		GetConsoleMode(wf_term_input_handle(t), &inmode);
		if (INVALID_HANDLE_VALUE != conout) {
			GetConsoleMode(conout, &outmode);
			CloseHandle(conout);
		}
		logf_("OPENED", "%d,%d,in_mode=0x%lx,out_mode=0x%lx,%ls,", cols,
		        rows, inmode, outmode, mode);
	}

	if (!wcscmp(mode, L"log-input")) {
		rc = mode_log_input(t, timeout);
	} else if (!wcscmp(mode, L"confirm-test")) {
		rc = mode_confirm(t, timeout, 0);
	} else if (!wcscmp(mode, L"idle") || !wcscmp(mode, L"slowclean")) {
		rc = mode_confirm(t, timeout, 1);
	} else if (!wcscmp(mode, L"width-matrix")) {
		rc = mode_width_matrix(t, timeout);
	} else if (!wcscmp(mode,
	                   L"stall")) { /* P0-T08: a main thread that never
			                   services the shutdown event */
		logf_("STALLED", ",,,,,");
		Sleep(timeout);
	}

	logf_("LOOP_EXIT", "rc=%d,,,,,", rc);
	t_close = now_ms();
	wf_term_close(t); /* restores the console; writes the reset sequence */
	logf_("TERM_CLOSED", "restore_took_ms=%.3f,,,,,", now_ms() - t_close);
	fclose(g_log);
	if (!wcscmp(mode, L"slowclean")) { /* P0-T08: long teardown AFTER the
	                                     console is restored */
		Sleep(10000);
	}
	return rc;
}

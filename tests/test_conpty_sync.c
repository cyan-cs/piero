#include <windows.h>
#include <string.h>

static ULONGLONG tick;
static ULONGLONG child_exit_at;
static unsigned writes, write_limit;
static int write_fault;
static DWORD write_delay;
static char delivered[32];
static size_t delivered_n;
static void (*advance_output)(ULONGLONG);
static ULONGLONG WINAPI test_ticks(void)
{
	return tick;
}
static VOID WINAPI test_sleep(DWORD ms)
{
	tick += ms;
	if (advance_output) {
		advance_output(tick);
	}
}
static DWORD WINAPI test_wait(HANDLE h, DWORD ms)
{
	(void)h;
	(void)ms;
	return child_exit_at && tick >= child_exit_at ? WAIT_OBJECT_0
	                                              : WAIT_TIMEOUT;
}
static BOOL WINAPI test_write(HANDLE h,
        LPCVOID bytes,
        DWORD n,
        LPDWORD wrote,
        LPOVERLAPPED overlapped)
{
	(void)h;
	(void)overlapped;
	writes++;
	tick += write_delay;
	if (write_fault == 1 || (write_fault == 3 && writes >= 2)) {
		*wrote = 0;
		SetLastError(ERROR_BROKEN_PIPE);
		return FALSE;
	}
	*wrote = write_fault == 2 ? 0 : n < write_limit ? n : write_limit;
	if (*wrote) {
		memcpy(delivered + delivered_n, bytes, *wrote);
		delivered_n += *wrote;
	}
	return TRUE;
}

#define GetTickCount64 test_ticks
#define Sleep test_sleep
#define WaitForSingleObject test_wait
#define WriteFile test_write
#define wmain conpty_harness_main
#include "../tools/p0/p0_conpty_host.c"
#undef wmain
#undef WriteFile
#undef WaitForSingleObject
#undef Sleep
#undef GetTickCount64
#include "wf_test.h"

static Pty *delayed_console;
static ULONGLONG delay_start;
static unsigned next_frame;
static void delayed_frames(ULONGLONG now)
{
	static const ULONGLONG at[] = {50, 90, 150};
	static const char *const frames[] = {"READY", "WAIT ", "READY"};
	if (next_frame < 3 && now - delay_start >= at[next_frame]) {
		const char home[] = "\x1b[1;1H";
		screen_feed(&delayed_console->scr, (const uint8_t *)home,
		        sizeof home - 1);
		screen_feed(&delayed_console->scr,
		        (const uint8_t *)frames[next_frame], 5);
		delayed_console->raw_n += sizeof home - 1 + 5;
		next_frame++;
	}
}

int main(void)
{
	Pty p = {0};
	write_limit = 2;
	write_delay = 300;
	CHECK(pty_write(&p, "abcde", 5));
	CHECK(writes == 3 && delivered_n == 5 && p.input_written == 5);
	CHECK(tick == 900);
	write_delay = 0;
	CHECK(!memcmp(delivered, "abcde", 5));
	write_fault = 1;
	CHECK(!pty_write(&p, "x", 1));
	CHECK(p.input_error == ERROR_BROKEN_PIPE && p.input_written == 5);
	unsigned failed_at = writes;
	CHECK(!pty_write(&p, "x", 1) && writes == failed_at);
	p.input_error = 0;
	write_fault = 2;
	CHECK(!pty_write(&p, "x", 1));
	CHECK(p.input_error == ERROR_WRITE_FAULT);
	Pty partial = {0};
	writes = 0;
	delivered_n = 0;
	write_fault = 3;
	CHECK(!pty_write(&partial, "abcde", 5));
	CHECK(partial.input_written == 2 && delivered_n == 2);
	CHECK(!memcmp(delivered, "ab", 2));
	p.input_error = 0;
	write_fault = 0;
	InitializeCriticalSection(&p.lock);
	screen_resize(&p.scr, 40, 4);
	screen_feed(&p.scr, (const uint8_t *)"READY", 5);
	p.raw_n = 5;
	char text[256];
	CHECK(product_wait_ms(&p, "READY", text, sizeof text, 100));
	CHECK(tick >= 60);
	/* Old visible text must not acknowledge a partial new VT/UTF-8 update. */
	p.scr.state = 2;
	CHECK(!product_wait_ms(&p, "READY", text, sizeof text, 100));
	p.scr.state = 0;
	p.scr.u8need = 3;
	CHECK(!product_wait_ms(&p, "READY", text, sizeof text, 100));
	p.scr.u8need = 0;
	p.capture_failed = 1;
	CHECK(!product_wait_ms(&p, "READY", text, sizeof text, 100));
	p.capture_failed = 0;
	erase(&p.scr, 0, 0, 3, 39);
	delayed_console = &p;
	delay_start = tick;
	advance_output = delayed_frames;
	CHECK(product_wait_ms(&p, "READY", text, sizeof text, 300));
	CHECK(next_frame == 3 && tick - delay_start >= 210);
	advance_output = NULL;
	child_exit_at = tick + 60;
	CHECK(!product_wait_ms(&p, "READY", text, sizeof text, 100));
	free(p.scr.cell);
	DeleteCriticalSection(&p.lock);
	return TEST_RESULT();
}

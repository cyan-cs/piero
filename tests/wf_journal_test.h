#ifndef WF_JOURNAL_TEST_H
#define WF_JOURNAL_TEST_H

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "wf_test.h"

/* Makes the records of this process look like a dead writer's: the pid is one
 * that does not exist. */
static void wf_test_disown(const wchar_t *journal)
{
	HANDLE h = CreateFileW(journal, GENERIC_READ | GENERIC_WRITE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
	        NULL);
	char buf[8192], out[8192], mine[32];
	DWORD got = 0;
	char *p = buf, *q = out;
	size_t k;

	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE == h) {
		return;
	}
	CHECK(ReadFile(h, buf, sizeof buf - 1, &got, NULL));
	buf[got] = 0;
	snprintf(mine, sizeof mine, " %lu ",
	        (unsigned long)GetCurrentProcessId());
	k = strlen(mine);
	while (*p) {
		if (!strncmp(p, mine, k)) {
			q += snprintf(q, 10, " 1999999 ");
			p += k;
		} else {
			*q++ = *p++;
		}
	}
	*q = 0;
	SetFilePointer(h, 0, NULL, FILE_BEGIN);
	SetEndOfFile(h);
	WriteFile(h, out, (DWORD)(q - out), &got, NULL);
	CloseHandle(h);
}

#endif

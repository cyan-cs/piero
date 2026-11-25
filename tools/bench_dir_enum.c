/* Read-only metadata enumeration benchmark; does not flush the OS cache. */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include "fs/wf_enum_win.h"

static double ms(void)
{
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t);
	QueryPerformanceFrequency(&f);
	return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
int wmain(int argc, wchar_t **argv)
{
	if (argc != 2) {
		return 2;
	}
	IO_COUNTERS before = {0}, after = {0};
	GetProcessIoCounters(GetCurrentProcess(), &before);
	for (unsigned i = 0; i < 21; i++) {
		WfDir *d = NULL;
		double start = ms();
		WfStatus st = wf_enum_dir_win(argv[1], &d);
		printf("enum,%u,status=%d,entries=%zu,truncated=%d,ms=%.3f\n",
		        i, st, d ? d->n : 0, d ? (int)d->truncated : 0,
		        ms() - start);
		if (st != WF_OK) {
			wf_dir_free(d);
			return 1;
		}
		wf_dir_free(d);
	}
	GetProcessIoCounters(GetCurrentProcess(), &after);
	printf("io,read_ops=%llu,other_ops=%llu,read_bytes=%llu\n",
	        after.ReadOperationCount - before.ReadOperationCount,
	        after.OtherOperationCount - before.OtherOperationCount,
	        after.ReadTransferCount - before.ReadTransferCount);
	PROCESS_MEMORY_COUNTERS m = {0};
	m.cb = sizeof m;
	GetProcessMemoryInfo(GetCurrentProcess(), &m, sizeof m);
	printf("working_set,%zu,peak=%zu\n", (size_t)m.WorkingSetSize,
	        (size_t)m.PeakWorkingSetSize);
	return 0;
}

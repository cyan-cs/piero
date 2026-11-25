#ifndef WF_DRAFT_WRITE_PROBE_H
#define WF_DRAFT_WRITE_PROBE_H

#include "edit/wf_edraft_win.h"

WfStatus wf_test_draft_write(
        const wchar_t *path, WfEncoding enc, const char *text, size_t n);
BOOL WINAPI wf_test_draft_wait(
        PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD timeout, ULONG flags);

#define wf_draft_write wf_test_draft_write
#define SleepConditionVariableSRW wf_test_draft_wait

#endif

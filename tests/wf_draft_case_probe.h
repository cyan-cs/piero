#ifndef WF_DRAFT_CASE_PROBE_H
#define WF_DRAFT_CASE_PROBE_H

#include <windows.h>

BOOL WINAPI wf_test_draft_case_info(
        HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID info, DWORD size);

#define GetFileInformationByHandleEx wf_test_draft_case_info

#endif

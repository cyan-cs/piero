#include "wf_run_win.h"
#include <stdlib.h>

static BOOL job_empty(HANDLE job, ULONGLONG deadline)
{
	JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info;
	for (;;) {
		if (!QueryInformationJobObject(job,
		            JobObjectBasicAccountingInformation, &info,
		            sizeof info, NULL)) {
			return FALSE;
		}
		if (!info.ActiveProcesses) {
			return TRUE;
		}
		if (GetTickCount64() >= deadline) {
			SetLastError(ERROR_TIMEOUT);
			return FALSE;
		}
		Sleep(10); /* bounded teardown in a test host, never the product UI */
	}
}

WfRunResult wf_run_process(const wchar_t *exe,
        wchar_t *command,
        const wchar_t *out,
        const wchar_t *err,
        DWORD timeout_ms)
{
	WfRunResult result = {WF_RUN_START_FAILED, 0, 0};
	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
	HANDLE streams[3] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE,
	        INVALID_HANDLE_VALUE};
	HANDLE job = NULL;
	STARTUPINFOEXW startup = {0};
	PROCESS_INFORMATION process = {0};
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
	SIZE_T bytes = 0;
	DWORD wait, code;
	BOOL assigned = FALSE;
	startup.StartupInfo.cb = sizeof startup;
	if (!timeout_ms || INFINITE == timeout_ms) {
		result.error = ERROR_INVALID_PARAMETER;
		return result;
	}
	streams[0] = CreateFileW(L"NUL", GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
	        NULL);
	if (INVALID_HANDLE_VALUE == streams[0]) {
		goto failed;
	}
	streams[1] = CreateFileW(
	        out, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW, 0, NULL);
	if (INVALID_HANDLE_VALUE == streams[1]) {
		goto failed;
	}
	streams[2] = CreateFileW(
	        err, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW, 0, NULL);
	if (INVALID_HANDLE_VALUE == streams[2]) {
		goto failed;
	}
	job = CreateJobObjectW(NULL, NULL);
	limits.BasicLimitInformation.LimitFlags =
	        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!job ||
	        !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
	                &limits, sizeof limits)) {
		goto failed;
	}
	(void)InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
	startup.lpAttributeList = malloc(bytes);
	if (!startup.lpAttributeList) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		goto failed;
	}
	if (!InitializeProcThreadAttributeList(
	            startup.lpAttributeList, 1, 0, &bytes)) {
		DWORD error = GetLastError();
		free(startup.lpAttributeList);
		startup.lpAttributeList = NULL;
		SetLastError(error);
		goto failed;
	}
	if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
	            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, streams, sizeof streams,
	            NULL, NULL)) {
		goto failed;
	}
	startup.StartupInfo.dwFlags =
	        STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	startup.StartupInfo.wShowWindow = SW_HIDE;
	startup.StartupInfo.hStdInput = streams[0];
	startup.StartupInfo.hStdOutput = streams[1];
	startup.StartupInfo.hStdError = streams[2];
	if (!CreateProcessW(exe, command, NULL, NULL, TRUE,
	            CREATE_SUSPENDED | CREATE_NO_WINDOW |
	                    EXTENDED_STARTUPINFO_PRESENT,
	            NULL, NULL, &startup.StartupInfo, &process)) {
		goto failed;
	}
	if (!AssignProcessToJobObject(job, process.hProcess)) {
		goto failed;
	}
	assigned = TRUE;
	if (ResumeThread(process.hThread) == (DWORD)-1) {
		goto failed;
	}
	wait = WaitForSingleObject(process.hProcess, timeout_ms);
	if (WAIT_OBJECT_0 == wait) {
		if (!GetExitCodeProcess(process.hProcess, &code)) {
			result.state = WF_RUN_WAIT_FAILED;
			result.error = GetLastError();
			goto terminate;
		}
		result.exit_code = (LONG)code;
		result.state = 0xC0000000u == (code & 0xC0000000u)
		        ? WF_RUN_CRASHED
		        : WF_RUN_EXITED;
		if (job_empty(job, GetTickCount64() + 100)) {
			goto done;
		}
		result.error = GetLastError();
		result.state = ERROR_TIMEOUT == result.error
		        ? WF_RUN_DESCENDANTS
		        : WF_RUN_WAIT_FAILED;
	} else {
		result.state = WAIT_TIMEOUT == wait ? WF_RUN_TIMED_OUT
		                                    : WF_RUN_WAIT_FAILED;
		result.error =
		        WAIT_TIMEOUT == wait ? ERROR_TIMEOUT : GetLastError();
	}
terminate:
	if (!TerminateJobObject(job, ERROR_CANCELLED)) {
		result.state = WF_RUN_TERMINATE_FAILED;
		result.error = GetLastError();
	} else {
		wait = WaitForSingleObject(process.hProcess, 2000);
		if (WAIT_OBJECT_0 != wait ||
		        !job_empty(job, GetTickCount64() + 2000)) {
			result.state = WF_RUN_TERMINATE_FAILED;
			result.error = WAIT_TIMEOUT == wait ? ERROR_TIMEOUT
			                                    : GetLastError();
		}
	}
	goto done;
failed:
	result.error = GetLastError();
	if (process.hProcess) {
		BOOL killed = assigned
		        ? TerminateJobObject(job, ERROR_CANCELLED)
		        : TerminateProcess(process.hProcess, ERROR_CANCELLED);
		DWORD error = killed ? ERROR_SUCCESS : GetLastError();
		wait = killed ? WaitForSingleObject(process.hProcess, 2000)
		              : WAIT_FAILED;
		if (!killed || WAIT_OBJECT_0 != wait) {
			result.state = WF_RUN_TERMINATE_FAILED;
			result.error = !killed         ? error
			        : WAIT_TIMEOUT == wait ? ERROR_TIMEOUT
			                               : GetLastError();
		}
	}
done:
	if (job) {
		CloseHandle(
		        job); /* also kills descendants after a failed explicit termination */
	}
	if (process.hProcess) {
		if (WF_RUN_TERMINATE_FAILED == result.state) {
			(void)WaitForSingleObject(process.hProcess, 2000);
		}
		CloseHandle(process.hProcess);
		CloseHandle(process.hThread);
	}
	if (startup.lpAttributeList) {
		DeleteProcThreadAttributeList(startup.lpAttributeList);
		free(startup.lpAttributeList);
	}
	for (unsigned i = 0; i < 3; i++) {
		if (INVALID_HANDLE_VALUE != streams[i]) {
			CloseHandle(streams[i]);
		}
	}
	return result;
}

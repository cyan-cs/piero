/* No child process or user file is created. Owned kernel handles stand in for
 * the process/thread so a resume failure can be injected deterministically. */
#include <windows.h>
#include <stdbool.h>
#include <userenv.h>
#include "wf_test.h"

static unsigned created, resumed, terminated;
static HANDLE model_process;
static bool token_readable;
static DWORD token_appcontainer;

static BOOL WINAPI fake_open_token(HANDLE process, DWORD access, PHANDLE token)
{
	CHECK(process == model_process && access == TOKEN_QUERY);
	if (!token_readable) {
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	*token = CreateEventW(NULL, TRUE, FALSE, NULL);
	return *token != NULL;
}

static BOOL WINAPI fake_token_information(HANDLE token,
        TOKEN_INFORMATION_CLASS kind,
        LPVOID information,
        DWORD bytes,
        PDWORD used)
{
	CHECK(token != NULL && kind == TokenIsAppContainer);
	CHECK(bytes == sizeof token_appcontainer);
	if (bytes != sizeof token_appcontainer) {
		return FALSE;
	}
	*(DWORD *)information = token_appcontainer;
	*used = sizeof token_appcontainer;
	return TRUE;
}

static HRESULT WINAPI fake_profile(PCWSTR name,
        PCWSTR display,
        PCWSTR description,
        PSID_AND_ATTRIBUTES capabilities,
        DWORD count,
        PSID *sid)
{
	(void)name;
	(void)display;
	(void)description;
	CHECK(!capabilities && count == 0);
	SID_IDENTIFIER_AUTHORITY authority = {{0, 0, 0, 0, 0, 15}};
	return AllocateAndInitializeSid(
	               &authority, 2, 2, 1, 0, 0, 0, 0, 0, 0, sid)
	        ? S_OK
	        : E_FAIL;
}

static BOOL WINAPI fake_create_process(LPCWSTR app,
        LPWSTR command,
        LPSECURITY_ATTRIBUTES process_attributes,
        LPSECURITY_ATTRIBUTES thread_attributes,
        BOOL inherit,
        DWORD flags,
        LPVOID environment,
        LPCWSTR directory,
        LPSTARTUPINFOW startup,
        LPPROCESS_INFORMATION info)
{
	(void)app;
	(void)command;
	(void)process_attributes;
	(void)thread_attributes;
	(void)environment;
	(void)directory;
	(void)startup;
	CHECK(inherit && (flags & CREATE_SUSPENDED));
	created++;
	info->hProcess = CreateEventW(NULL, TRUE, FALSE, NULL);
	model_process = info->hProcess;
	info->hThread = CreateEventW(NULL, TRUE, TRUE, NULL);
	return info->hProcess && info->hThread;
}

static BOOL WINAPI fake_assign_job(HANDLE job, HANDLE process)
{
	CHECK(job != NULL && process != NULL);
	return TRUE;
}

static DWORD WINAPI fake_resume(HANDLE thread)
{
	CHECK(thread != NULL);
	resumed++;
	SetLastError(ERROR_ACCESS_DENIED);
	return (DWORD)-1;
}

static BOOL WINAPI fake_terminate(HANDLE process, UINT code)
{
	(void)code;
	CHECK(process != NULL);
	terminated++;
	return SetEvent(process);
}

static BOOL WINAPI fake_exit_code(HANDLE process, LPDWORD code)
{
	CHECK(process != NULL);
	*code = ERROR_PROCESS_ABORTED;
	return TRUE;
}

static BOOL WINAPI fake_terminate_job(HANDLE job, UINT code)
{
	(void)code;
	CHECK(job != NULL && model_process != NULL);
	terminated++;
	return SetEvent(model_process);
}

#define CreateProcessW fake_create_process
#define AssignProcessToJobObject fake_assign_job
#define ResumeThread fake_resume
#define TerminateProcess fake_terminate
#define TerminateJobObject fake_terminate_job
#define GetExitCodeProcess fake_exit_code
#define CreateAppContainerProfile fake_profile
#define OpenProcessToken fake_open_token
#define GetTokenInformation fake_token_information
#ifndef WF_DECODER_SOURCE
#	define WF_DECODER_SOURCE "../src/preview/wf_dechost_client_win.c"
#endif
#include WF_DECODER_SOURCE

int main(void)
{
	WfDecHostOptions options = {0};
	WfDecHost *host = NULL;
	WfDecStartPhase phase = WF_DEC_START_READY;
	DWORD error = 1;
	CHECK(wf_dechost_start_with_phase(NULL, &host, &error, &phase) ==
	        WF_E_INVAL);
	CHECK(phase == WF_DEC_START_VALIDATE && error == 0 && host == NULL);
	/* This is a syscall model, not an unsandboxed decoder execution. */
	options.exe = L"not-executed.exe";
	CHECK(wf_dechost_start_with_phase(&options, &host, &error, &phase) ==
	        WF_E_OS);
	CHECK(phase == WF_DEC_START_RESUME && error == ERROR_ACCESS_DENIED);
	CHECK(host == NULL && created == 1 && resumed == 1 && terminated == 1);
	/* A suspended child with no inspectable AppContainer token is killed
	 * before any decoder code can run. The model creates only kernel events. */
	options.sandbox = 1;
	CHECK(wf_dechost_start_with_phase(&options, &host, &error, &phase) ==
	        WF_E_OS);
	CHECK(phase == WF_DEC_START_TOKEN && error == ERROR_ACCESS_DENIED);
	CHECK(host == NULL && created == 2 && resumed == 1 && terminated == 2);
	/* An inspectable ordinary token is also rejected before ResumeThread. */
	token_readable = true;
	CHECK(wf_dechost_start_with_phase(&options, &host, &error, &phase) ==
	        WF_E_OS);
	CHECK(phase == WF_DEC_START_TOKEN && error == ERROR_ACCESS_DENIED);
	CHECK(host == NULL && created == 3 && resumed == 1 && terminated == 3);
	/* Only a confirmed AppContainer token reaches the modeled resume call. */
	token_appcontainer = 1;
	CHECK(wf_dechost_start_with_phase(&options, &host, &error, &phase) ==
	        WF_E_OS);
	CHECK(phase == WF_DEC_START_RESUME && error == ERROR_ACCESS_DENIED);
	CHECK(host == NULL && created == 4 && resumed == 2 && terminated == 4);
	return TEST_RESULT();
}

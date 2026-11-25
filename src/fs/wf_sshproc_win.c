#include "wf_sshproc_win.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERR_MAX 4096u
#define PIPE_BUF (64u * 1024u)

struct WfSshProc
{
	HANDLE process, job;
	HANDLE in_w;   /* we write: the child's stdin */
	HANDLE out_r;  /* we read: the child's stdout */
	HANDLE err_r;  /* we read: the child's stderr */
	HANDLE cancel; /* manual-reset event */
	HANDLE io_event;
	/* The child's stderr is read all the time, by a thread of its own: a pipe
	 * that nobody reads fills (64 KiB), and a child that writes more than that
	 * waits for room while this side waits for its answer on stdout. Only the
	 * start of it is kept (ERR_MAX bytes); the rest is read and dropped. */
	HANDLE err_thread;
	HANDLE err_stop; /* manual-reset event: end the thread */
	SRWLOCK err_lock;
	char err_buf[ERR_MAX];
	size_t err_n;
};

static volatile LONG pipe_counter;

/* An overlapped server end for us and a plain inheritable client end for the
 * child. `ours_reads`: the child writes into the client end. */
static bool make_pipe(HANDLE *ours, HANDLE *theirs, bool ours_reads)
{
	wchar_t name[96];
	SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
	LONG n = InterlockedIncrement(&pipe_counter);

	swprintf(name, 96, L"\\\\.\\pipe\\piero-ssh-%lu-%ld-%llu",
	        GetCurrentProcessId(), n, (unsigned long long)GetTickCount64());
	*ours = CreateNamedPipeW(name,
	        (ours_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) |
	                FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
	        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
	        PIPE_BUF, PIPE_BUF, 0, NULL);
	if (INVALID_HANDLE_VALUE == *ours) {
		return false;
	}
	*theirs = CreateFileW(name, ours_reads ? GENERIC_WRITE : GENERIC_READ,
	        0, &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == *theirs) {
		CloseHandle(*ours);
		*ours = INVALID_HANDLE_VALUE;
		return false;
	}
	return true;
}

static DWORD WINAPI drain_stderr(LPVOID arg)
{
	WfSshProc *p = arg;
	HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
	char tmp[4096];

	if (!ev) {
		return 1;
	}
	for (;;) {
		OVERLAPPED ov;
		HANDLE waits[2] = {ev, p->err_stop};
		DWORD got = 0;

		memset(&ov, 0, sizeof ov);
		ov.hEvent = ev;
		ResetEvent(ev);
		if (!ReadFile(p->err_r, tmp, sizeof tmp, &got, &ov)) {
			if (ERROR_IO_PENDING != GetLastError()) {
				break; /* the child's end is closed */
			}
			if (WAIT_OBJECT_0 !=
			        WaitForMultipleObjects(
			                2, waits, FALSE, INFINITE)) {
				CancelIoEx(p->err_r, &ov);
				GetOverlappedResult(p->err_r, &ov, &got, TRUE);
				break; /* told to end */
			}
			if (!GetOverlappedResult(p->err_r, &ov, &got, FALSE)) {
				break;
			}
		}
		if (0 == got) {
			break;
		}
		AcquireSRWLockExclusive(&p->err_lock);
		if (p->err_n < ERR_MAX) {
			size_t room = ERR_MAX - p->err_n;
			size_t take = got < room ? got : room;

			memcpy(p->err_buf + p->err_n, tmp, take);
			p->err_n += take;
		}
		ReleaseSRWLockExclusive(
		        &p->err_lock); /* what is over is dropped */
	}
	CloseHandle(ev);
	return 0;
}

WfStatus wf_sshproc_start(
        const wchar_t *exe, const wchar_t *cmdline, WfSshProc **out)
{
	WfSshProc *p;
	HANDLE in_r = INVALID_HANDLE_VALUE, out_w = INVALID_HANDLE_VALUE,
	       err_w = INVALID_HANDLE_VALUE;
	HANDLE inherit_list[3];
	STARTUPINFOEXW si;
	PROCESS_INFORMATION pi;
	SIZE_T attr_size = 0;
	wchar_t *line;
	size_t line_n;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit;
	bool ok = false;

	if (!exe || !cmdline || !out || !*exe || !*cmdline) {
		return WF_E_INVAL;
	}
	*out = NULL;
	p = calloc(1, sizeof *p);
	if (!p) {
		return WF_E_NOMEM;
	}
	p->in_w = p->out_r = p->err_r = INVALID_HANDLE_VALUE;
	memset(&si, 0, sizeof si);
	memset(&pi, 0, sizeof pi);
	line_n = wcslen(cmdline) + 1;
	line = malloc(line_n * sizeof *line); /* CreateProcess may edit it */
	if (!line) {
		free(p);
		return WF_E_NOMEM;
	}
	memcpy(line, cmdline, line_n * sizeof *line);

	p->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
	p->io_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	p->err_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	p->job = CreateJobObjectW(NULL, NULL);
	if (!p->cancel || !p->io_event || !p->err_stop || !p->job ||
	        !make_pipe(&p->in_w, &in_r, false) ||
	        !make_pipe(&p->out_r, &out_w, true) ||
	        !make_pipe(&p->err_r, &err_w, true)) {
		goto done;
	}
	memset(&limit, 0, sizeof limit);
	limit.BasicLimitInformation.LimitFlags =
	        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!SetInformationJobObject(p->job, JobObjectExtendedLimitInformation,
	            &limit, sizeof limit)) {
		goto done;
	}
	/* only these three handles go to the child */
	inherit_list[0] = in_r;
	inherit_list[1] = out_w;
	inherit_list[2] = err_w;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
	si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, attr_size);
	if (!si.lpAttributeList) {
		goto done;
	}
	if (!InitializeProcThreadAttributeList(
	            si.lpAttributeList, 1, 0, &attr_size)) {
		HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
		si.lpAttributeList = NULL;
		goto done;
	}
	if (!UpdateProcThreadAttribute(si.lpAttributeList, 0,
	            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit_list,
	            sizeof inherit_list, NULL, NULL)) {
		goto done;
	}
	si.StartupInfo.cb = sizeof si;
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = in_r;
	si.StartupInfo.hStdOutput = out_w;
	si.StartupInfo.hStdError = err_w;
	if (!CreateProcessW(exe, line, NULL, NULL, TRUE,
	            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW |
	                    CREATE_SUSPENDED,
	            NULL, NULL, &si.StartupInfo, &pi)) {
		goto done;
	}
	p->process = pi.hProcess;
	if (!AssignProcessToJobObject(p->job, pi.hProcess)) {
		TerminateProcess(pi.hProcess, 1);
		goto done;
	}
	/* before the child runs: nothing it writes can wait for a reader */
	p->err_thread = CreateThread(NULL, 0, drain_stderr, p, 0, NULL);
	if (!p->err_thread) {
		TerminateProcess(pi.hProcess, 1);
		goto done;
	}
	ResumeThread(pi.hThread);
	ok = true;

done:
	if (si.lpAttributeList) {
		DeleteProcThreadAttributeList(si.lpAttributeList);
		HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
	}
	if (pi.hThread) {
		CloseHandle(pi.hThread);
	}
	/* the child's ends belong to the child now */
	if (INVALID_HANDLE_VALUE != in_r) {
		CloseHandle(in_r);
	}
	if (INVALID_HANDLE_VALUE != out_w) {
		CloseHandle(out_w);
	}
	if (INVALID_HANDLE_VALUE != err_w) {
		CloseHandle(err_w);
	}
	free(line);
	if (!ok) {
		wf_sshproc_free(p);
		return WF_E_OS;
	}
	*out = p;
	return WF_OK;
}

void wf_sshproc_free(WfSshProc *p)
{
	if (!p) {
		return;
	}
	if (p->process) {
		if (WAIT_TIMEOUT == WaitForSingleObject(p->process, 0)) {
			TerminateProcess(p->process, 1);
		}
		WaitForSingleObject(p->process, 2000);
		CloseHandle(p->process);
	}
	if (p->err_thread) {
		/* the thread reads p->err_r and p->err_buf: it is ended before either is
		 * released. The child is gone, so its pipe is broken, and the stop
		 * withdraws a read that is waiting. Were it not to end, the memory it
		 * uses stays (a leak, not a read of freed memory). */
		SetEvent(p->err_stop);
		if (WAIT_OBJECT_0 != WaitForSingleObject(p->err_thread, 5000)) {
			return;
		}
		CloseHandle(p->err_thread);
	}
	if (INVALID_HANDLE_VALUE != p->in_w) {
		CloseHandle(p->in_w);
	}
	if (INVALID_HANDLE_VALUE != p->out_r) {
		CloseHandle(p->out_r);
	}
	if (INVALID_HANDLE_VALUE != p->err_r) {
		CloseHandle(p->err_r);
	}
	if (p->err_stop) {
		CloseHandle(p->err_stop);
	}
	if (p->job) {
		CloseHandle(p->job);
	}
	if (p->cancel) {
		CloseHandle(p->cancel);
	}
	if (p->io_event) {
		CloseHandle(p->io_event);
	}
	free(p);
}

void wf_sshproc_cancel(WfSshProc *p)
{
	if (p && p->cancel) {
		SetEvent(p->cancel);
	}
}

/* One overlapped transfer with a time limit and a cancel. On timeout or
 * cancel the transfer is withdrawn and waited for, so the buffer is free. */
static WfStatus transfer(WfSshProc *p,
        const uint8_t *wb,
        uint8_t *rb,
        size_t n,
        DWORD *done,
        DWORD ms)
{
	bool reading = NULL != rb;
	OVERLAPPED ov;
	HANDLE waits[2];
	DWORD w, got = 0;
	BOOL ok;

	memset(&ov, 0, sizeof ov);
	ResetEvent(p->io_event);
	ov.hEvent = p->io_event;
	*done = 0;
	if (WAIT_OBJECT_0 == WaitForSingleObject(p->cancel, 0)) {
		return WF_E_CANCELLED;
	}
	ok = reading ? ReadFile(p->out_r, rb, (DWORD)n, &got, &ov)
	             : WriteFile(p->in_w, wb, (DWORD)n, &got, &ov);
	if (!ok && ERROR_IO_PENDING != GetLastError()) {
		DWORD e = GetLastError();

		/* the child closed its end: for a read that is the end of the stream,
		 * for a write it is a failure */
		return reading &&
		                (ERROR_BROKEN_PIPE == e ||
		                        ERROR_PIPE_NOT_CONNECTED == e)
		        ? WF_OK
		        : WF_E_OS;
	}
	if (!ok) {
		waits[0] = p->io_event;
		waits[1] = p->cancel;
		w = WaitForMultipleObjects(2, waits, FALSE, ms);
		if (WAIT_OBJECT_0 != w) {
			CancelIoEx(reading ? p->out_r : p->in_w, &ov);
			GetOverlappedResult(
			        reading ? p->out_r : p->in_w, &ov, &got, TRUE);
			if (reading && got > 0 && WAIT_TIMEOUT == w) {
				/* bytes came in as the wait ended: they are the stream's, and
				 * a caller that waits in slices must not lose them */
				*done = got;
				return WF_OK;
			}
			return WAIT_TIMEOUT == w ? WF_E_TIMEOUT
			                         : WF_E_CANCELLED;
		}
		if (!GetOverlappedResult(
		            reading ? p->out_r : p->in_w, &ov, &got, FALSE)) {
			DWORD e = GetLastError();

			return reading && (ERROR_BROKEN_PIPE == e) ? WF_OK
			                                           : WF_E_OS;
		}
	}
	*done = got;
	return WF_OK;
}

static WfStatus proc_write(void *ctx, const uint8_t *b, size_t n, uint32_t ms)
{
	WfSshProc *p = ctx;
	size_t at = 0;

	while (at < n) {
		DWORD done = 0;
		DWORD want = n - at > 0x10000 ? 0x10000u : (DWORD)(n - at);
		WfStatus s = transfer(p, b + at, NULL, want, &done, ms);

		if (WF_OK != s) {
			return s;
		}
		if (0 == done) {
			return WF_E_OS;
		}
		at += done;
	}
	return WF_OK;
}

static WfStatus proc_read(
        void *ctx, uint8_t *b, size_t cap, size_t *got, uint32_t ms)
{
	DWORD done = 0;
	WfStatus s = transfer((WfSshProc *)ctx, NULL, b,
	        cap > 0x10000 ? 0x10000u : (DWORD)cap, &done, ms);

	*got = done;
	return s;
}

WfSftpStream wf_sshproc_stream(WfSshProc *p)
{
	WfSftpStream st = {p, proc_write, proc_read};

	return st;
}

bool wf_sshproc_wait(WfSshProc *p, uint32_t ms, int *code)
{
	DWORD c = 0;

	if (!p || !p->process ||
	        WAIT_OBJECT_0 != WaitForSingleObject(p->process, ms)) {
		return false;
	}
	if (code) {
		GetExitCodeProcess(p->process, &c);
		*code = (int)c;
	}
	return true;
}

void wf_sshproc_stderr(WfSshProc *p, char *out, size_t cap)
{
	size_t n;
	char copy[ERR_MAX];

	if (!out || !cap) {
		return;
	}
	out[0] = 0;
	if (!p) {
		return;
	}
	if (p->err_thread && p->process &&
	        WAIT_OBJECT_0 == WaitForSingleObject(p->process, 0)) {
		/* the child has ended: what it wrote last may still be on its way
		 * through the thread (not for long: the pipe is closed with it) */
		WaitForSingleObject(p->err_thread, 500);
	}
	AcquireSRWLockShared(&p->err_lock);
	n = p->err_n;
	memcpy(copy, p->err_buf, n);
	ReleaseSRWLockShared(&p->err_lock);
	for (size_t i = 0; i < n && i + 1 < cap; i++) {
		unsigned char c = (unsigned char)copy[i];

		out[i] = (c < 0x20 && '\r' != c && '\n' != c && '\t' != c) ||
		                127 == c
		        ? '?'
		        : (char)c;
		out[i + 1] = 0;
	}
}

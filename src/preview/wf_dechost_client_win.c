#include "wf_dechost_client_win.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <userenv.h>

#include "../base/wf_state_dir.h"
#include "wf_dechost_win.h"
#ifdef WF_SANITIZER_STAGE
#	include "wf_sanitizer_stage_win.h"
#endif

#define PIPE_BYTES (64u * 1024u)
#define PROFILE_NAME L"piero.decoder"
#define JOB_MEMORY_DEFAULT_MIB 512u

#ifndef PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY
#	define PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY                     \
		ProcThreadAttributeValue(14, FALSE, TRUE, FALSE)
#endif

struct WfDecHost
{
	HANDLE process, job;
	HANDLE to_host, from_host; /* parent ends, overlapped */
	HANDLE read_event, write_event;
	uint64_t next_request;
	int alive;
	int termination_unconfirmed;
	DWORD exit_code; /* STILL_ACTIVE until the process has ended */
};

static PSID container_sid(DWORD *err, WfDecStartPhase *phase)
{
	PSID sid = NULL;
	if (phase) {
		*phase = WF_DEC_START_PROFILE_CREATE;
	}
	HRESULT hr = CreateAppContainerProfile(PROFILE_NAME, PROFILE_NAME,
	        L"piero image and PDF decoder", NULL, 0, &sid);

	if (HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) == hr) {
		if (phase) {
			*phase = WF_DEC_START_PROFILE_DERIVE;
		}
		hr = DeriveAppContainerSidFromAppContainerName(
		        PROFILE_NAME, &sid);
	}
	if (FAILED(hr)) {
		*err = (DWORD)hr;
		return NULL;
	}
	return sid;
}

/* ALL APPLICATION PACKAGES: read and execute, inherited by what is inside. */
static int grant_app_packages(const wchar_t *dir)
{
	PSID packages = NULL;
	PACL old = NULL, merged = NULL;
	PSECURITY_DESCRIPTOR sd = NULL;
	EXPLICIT_ACCESSW ea;
	wchar_t mutable_dir[MAX_PATH * 2]; /* the API wants a writable name */
	int ok = 0;

	memset(&ea, 0, sizeof ea);
	swprintf(mutable_dir, ARRAYSIZE(mutable_dir), L"%ls", dir);
	if (!ConvertStringSidToSidW(L"S-1-15-2-1", &packages)) {
		return 0;
	}
	if (ERROR_SUCCESS !=
	        GetNamedSecurityInfoW(dir, SE_FILE_OBJECT,
	                DACL_SECURITY_INFORMATION, NULL, NULL, &old, NULL,
	                &sd)) {
		goto out;
	}
	ea.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
	ea.grfAccessMode = GRANT_ACCESS;
	ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
	ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
	ea.Trustee.ptstrName = (LPWSTR)packages;
	if (ERROR_SUCCESS != SetEntriesInAclW(1, &ea, old, &merged)) {
		goto out;
	}
	ok = ERROR_SUCCESS ==
	        SetNamedSecurityInfoW(mutable_dir, SE_FILE_OBJECT,
	                DACL_SECURITY_INFORMATION, NULL, NULL, merged, NULL);
out:
	LocalFree(merged);
	LocalFree(sd);
	LocalFree(packages);
	return ok;
}

static int sha256_of_file(const wchar_t *path, char hex[65])
{
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	HANDLE f;
	unsigned char buf[1 << 16];
	unsigned char digest[32];
	DWORD got, error = ERROR_GEN_FAILURE;
	int ok = 0;

	f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	if (INVALID_HANDLE_VALUE == f) {
		return 0;
	}
	if (0 !=
	                BCryptOpenAlgorithmProvider(
	                        &alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) ||
	        0 != BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0)) {
		goto done;
	}
	for (;;) {
		if (!ReadFile(f, buf, sizeof buf, &got, NULL)) {
			error = GetLastError();
			goto done;
		}
		if (!got) {
			break;
		}
		if (0 != BCryptHashData(hash, buf, got, 0)) {
			goto done;
		}
	}
	if (0 != BCryptFinishHash(hash, digest, sizeof digest, 0)) {
		goto done;
	}
	for (int i = 0; i < 32; i++) {
		snprintf(hex + 2 * i, 3, "%02x", digest[i]);
	}
	ok = 1;
done:
	if (hash) {
		BCryptDestroyHash(hash);
	}
	if (alg) {
		BCryptCloseAlgorithmProvider(alg, 0);
	}
	CloseHandle(f);
	if (!ok) {
		SetLastError(error);
	}
	return ok;
}

/* Creates `path` and every folder above it. */
static int make_dirs(wchar_t *path)
{
	for (wchar_t *p = path + 3; *p; p++) {
		if (L'\\' == *p) {
			*p = L'\0';
			if (!CreateDirectoryW(path, NULL) &&
			        ERROR_ALREADY_EXISTS != GetLastError()) {
				*p = L'\\';
				return 0;
			}
			*p = L'\\';
		}
	}
	return CreateDirectoryW(path, NULL) ||
	        ERROR_ALREADY_EXISTS == GetLastError();
}

/* Copies `self` to `tmp`, a name that must not exist yet, and flushes it. On a
 * failure *error is the Win32 error that stopped it. A partly written `tmp` is
 * left as it is: a name that was not ours to begin with is never removed, and an
 * incomplete copy is never run (the caller checks the digest before it renames). */
static bool copy_to_new(const wchar_t *self, const wchar_t *tmp, DWORD *error)
{
	unsigned char buf[1 << 16];
	HANDLE in, dst;
	DWORD got, put;
	bool ok = false;

	in = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	if (INVALID_HANDLE_VALUE == in) {
		*error = GetLastError();
		return false;
	}
	/* A pre-existing name may be residue, a hardlink or a reparse point.
	 * It is never ours to truncate, replace or remove. */
	dst = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == dst) {
		*error = GetLastError();
		goto out_close_in;
	}
	for (;;) {
		BOOL written;

		if (!ReadFile(in, buf, sizeof buf, &got, NULL)) {
			*error = GetLastError();
			goto out_close_dst;
		}
		if (!got) {
			break;
		}
		written = WriteFile(dst, buf, got, &put, NULL);
		if (!written || put != got) {
			*error = written ? ERROR_WRITE_FAULT : GetLastError();
			goto out_close_dst;
		}
	}
	if (!FlushFileBuffers(dst)) {
		*error = GetLastError();
		goto out_close_dst;
	}
	ok = true;
out_close_dst:
	CloseHandle(dst); /* after the error was taken: it does not change it */
out_close_in:
	CloseHandle(in);
	return ok;
}

WfStatus wf_dechost_stage(
        const wchar_t *source, wchar_t *out, size_t cap, DWORD *os_error)
{
	wchar_t self[MAX_PATH * 2], base[MAX_PATH], dir[MAX_PATH * 2],
	        tmp[MAX_PATH * 2], env[MAX_PATH];
	const wchar_t *base_root = NULL;
	PWSTR local = NULL;
	char hex[65], have[65];
	wchar_t whex[65];
	DWORD error = 0;
	WfStatus st = WF_E_OS;

	if (source) {
		swprintf(self, ARRAYSIZE(self), L"%ls", source);
	} else if (!GetModuleFileNameW(NULL, self, ARRAYSIZE(self))) {
		goto fail;
	}
	if (!sha256_of_file(self, hex)) {
		goto fail;
	}
	/* where the program keeps its state: LOCALAPPDATA as the rest of the
	 * program reads it (a tester may point it elsewhere), else the folder */
	{
		DWORD n = GetEnvironmentVariableW(
		        L"LOCALAPPDATA", env, ARRAYSIZE(env));

		if (n >= 3 && n < ARRAYSIZE(env) && L':' == env[1] &&
		        L'\\' == env[2]) {
			if (L'\\' == env[n - 1]) {
				env[n - 1] =
				        L'\0'; /* a drive root: "X:\" -> "X:" */
			}
			base_root = env;
		} else if (S_OK !=
		        SHGetKnownFolderPath(
		                &FOLDERID_LocalAppData, 0, NULL, &local)) {
			goto fail;
		} else {
			base_root = local;
		}
	}
	for (int i = 0; i < 65; i++) {
		whex[i] = (wchar_t)hex[i];
	}
	{
		/* the state folder in use (DR-DRAFT-017): piero, else an
		 * existing wfile; a folder that cannot be told is no place */
		WfStateKind kind;

		if (WF_OK != wf_state_classify(base_root, &kind, &error)) {
			if (!error) {
				error = ERROR_BAD_PATHNAME;
			}
			goto fail;
		}
		swprintf(base, ARRAYSIZE(base), L"%ls\\%ls\\worker", base_root,
		        wf_state_name(kind));
	}
	swprintf(dir, ARRAYSIZE(dir), L"%ls\\%ls", base, whex);
	swprintf(out, cap, L"%ls\\piero.exe", dir);
	swprintf(tmp, ARRAYSIZE(tmp), L"%ls\\piero.exe.tmp", dir);
	if (local) {
		CoTaskMemFree(local);
		local = NULL;
	}
	if (!make_dirs(dir)) {
		goto fail;
	}
	if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(out)) {
		/* already staged: it must be byte-identical, and is never replaced */
		if (sha256_of_file(out, have) && 0 == strcmp(have, hex)
#ifdef WF_SANITIZER_STAGE
		        && wf_sanitizer_stage(dir, &error)
#endif
		        && grant_app_packages(dir)) {
			return WF_OK;
		}
		goto fail;
	}
	if (!copy_to_new(self, tmp, &error)) {
		goto fail;
	}
	if (!sha256_of_file(tmp, have) || 0 != strcmp(have, hex) ||
	        !MoveFileExW(tmp, out, 0)
#ifdef WF_SANITIZER_STAGE
	        || !wf_sanitizer_stage(dir, &error)
#endif
	        || !grant_app_packages(dir)) {
		/* Uncommitted copies stay unexecuted. A later attempt refuses the
		 * occupied temporary name; this code never deletes unknown residue. */
		goto fail;
	}
	return WF_OK;
fail:
	if (!error) {
		error = GetLastError();
	}
	if (os_error) {
		*os_error = error;
	}
	if (local) {
		CoTaskMemFree(local);
	}
	return st;
}

static void kill_and_confirm(WfDecHost *h)
{
	if (!h->alive) {
		if (h->process) {
			GetExitCodeProcess(h->process, &h->exit_code);
		}
		return;
	}
	h->alive = 0;
	if (h->job) {
		TerminateJobObject(h->job, 99);
	} else if (h->process) {
		TerminateProcess(h->process, 99);
	}
	/* a kill REQUEST succeeding is not the process being gone */
	if (h->process &&
	        WAIT_OBJECT_0 != WaitForSingleObject(h->process, 5000)) {
		h->termination_unconfirmed = 1;
	}
	if (h->process) {
		GetExitCodeProcess(h->process, &h->exit_code);
	}
}

static HANDLE make_pipe(const wchar_t *name, int host_reads, HANDLE *host_end)
{
	SECURITY_ATTRIBUTES inh = {sizeof inh, NULL, TRUE};
	HANDLE mine = CreateNamedPipeW(name,
	        (host_reads ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) |
	                FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
	        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
	        PIPE_BYTES, PIPE_BYTES, 0, NULL);

	if (INVALID_HANDLE_VALUE == mine) {
		return NULL;
	}
	*host_end = CreateFileW(name, host_reads ? GENERIC_READ : GENERIC_WRITE,
	        0, &inh, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == *host_end) {
		CloseHandle(mine);
		return NULL;
	}
	return mine;
}

WfStatus wf_dechost_start(
        const WfDecHostOptions *opt, WfDecHost **out, DWORD *os_error)
{
	return wf_dechost_start_with_phase(opt, out, os_error, NULL);
}

WfStatus wf_dechost_start_with_phase(const WfDecHostOptions *opt,
        WfDecHost **out,
        DWORD *os_error,
        WfDecStartPhase *phase)
{
	static volatile LONG counter;
	WfDecHost *h;
	PSID sid = NULL;
	SECURITY_CAPABILITIES caps;
	STARTUPINFOEXW si;
	PROCESS_INFORMATION pi;
	SIZE_T attr_size = 0;
	wchar_t self[MAX_PATH * 2], name_in[96], name_out[96], cmd[1024];
	const wchar_t *exe;
	HANDLE host_in = NULL, host_out = NULL, handles[2];
	DWORD64 mitigation = PROCESS_CREATION_MITIGATION_POLICY_DEP_ENABLE |
	        PROCESS_CREATION_MITIGATION_POLICY_FORCE_RELOCATE_IMAGES_ALWAYS_ON |
	        PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON |
	        PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON |
	        PROCESS_CREATION_MITIGATION_POLICY_STRICT_HANDLE_CHECKS_ALWAYS_ON |
	        PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON |
	        PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON |
	        PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON |
	        PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_PREFER_SYSTEM32_ALWAYS_ON;
	DWORD child_policy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
	int attrs;
	DWORD err = 0;
	DWORD pid = GetCurrentProcessId();

	if (phase) {
		*phase = WF_DEC_START_VALIDATE;
	}
	if (os_error) {
		*os_error = 0;
	}
	if (!opt || !out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	h = calloc(1, sizeof *h);
	if (!h) {
		return WF_E_NOMEM;
	}
	memset(&si, 0, sizeof si);
	memset(&pi, 0, sizeof pi);
	memset(&caps, 0, sizeof caps);
	exe = opt->exe;
	if (!exe) {
		DWORD n = GetModuleFileNameW(NULL, self, ARRAYSIZE(self));

		if (n == 0 || n >= ARRAYSIZE(self)) { /* failed or truncated */
			err = n ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
			goto fail;
		}
		exe = self;
	}
	if (opt->sandbox) {
		sid = container_sid(&err, phase);
		if (!sid) {
			goto fail; /* no weaker host: starting fails */
		}
		caps.AppContainerSid = sid;
	}
	{
		LONG n = InterlockedIncrement(&counter);

		swprintf(name_in, ARRAYSIZE(name_in),
		        L"\\\\.\\pipe\\piero.dec.%lu.%ld.in", pid, n);
		swprintf(name_out, ARRAYSIZE(name_out),
		        L"\\\\.\\pipe\\piero.dec.%lu.%ld.out", pid, n);
	}
	if (phase) {
		*phase = WF_DEC_START_PIPES;
	}
	h->to_host = make_pipe(name_in, 1, &host_in);
	h->from_host = make_pipe(name_out, 0, &host_out);
	h->read_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	h->write_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!h->to_host || !h->from_host || !h->read_event || !h->write_event) {
		err = GetLastError();
		goto fail;
	}

	if (phase) {
		*phase = WF_DEC_START_ATTRIBUTES;
	}
	attrs = opt->sandbox ? 4 : 3;
	InitializeProcThreadAttributeList(NULL, (DWORD)attrs, 0, &attr_size);
	si.lpAttributeList = malloc(attr_size);
	if (!si.lpAttributeList) {
		err = ERROR_NOT_ENOUGH_MEMORY;
		goto fail;
	}
	if (!InitializeProcThreadAttributeList(
	            si.lpAttributeList, (DWORD)attrs, 0, &attr_size)) {
		err = GetLastError();
		free(si.lpAttributeList);
		si.lpAttributeList = NULL;
		goto fail;
	}
	handles[0] = host_in;
	handles[1] = host_out;
	if (!UpdateProcThreadAttribute(si.lpAttributeList, 0,
	            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof handles,
	            NULL, NULL) ||
	        !UpdateProcThreadAttribute(si.lpAttributeList, 0,
	                PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &mitigation,
	                sizeof mitigation, NULL, NULL) ||
	        !UpdateProcThreadAttribute(si.lpAttributeList, 0,
	                PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY,
	                &child_policy, sizeof child_policy, NULL, NULL) ||
	        (opt->sandbox &&
	                !UpdateProcThreadAttribute(si.lpAttributeList, 0,
	                        PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
	                        &caps, sizeof caps, NULL, NULL))) {
		err = GetLastError();
		goto fail;
	}
	si.StartupInfo.cb = sizeof si;
	if (swprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" %ls %llx %llx", exe,
	            WF_DECHOST_ARG, (unsigned long long)(uintptr_t)host_in,
	            (unsigned long long)(uintptr_t)host_out) < 0) {
		err = ERROR_FILENAME_EXCED_RANGE; /* never start a host on a cut command line */
		goto fail;
	}
	if (phase) {
		*phase = WF_DEC_START_PROCESS;
	}
	if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE,
	            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
	                    DETACHED_PROCESS,
	            NULL, NULL, &si.StartupInfo, &pi)) {
		err = GetLastError();
		goto fail;
	}
	h->process = pi.hProcess;
	h->alive = 1;
	h->exit_code = STILL_ACTIVE;
	if (phase) {
		*phase = WF_DEC_START_JOB;
	}
	h->job = CreateJobObjectW(NULL, NULL);
	memset(&limits, 0, sizeof limits);
	limits.BasicLimitInformation.LimitFlags =
	        JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
	        JOB_OBJECT_LIMIT_PROCESS_MEMORY |
	        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
	        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
	limits.BasicLimitInformation.ActiveProcessLimit = 1;
	limits.ProcessMemoryLimit =
	        (SIZE_T)(opt->memory_mib ? opt->memory_mib
	                                 : JOB_MEMORY_DEFAULT_MIB)
	        << 20;
	if (!h->job ||
	        !SetInformationJobObject(h->job,
	                JobObjectExtendedLimitInformation, &limits,
	                sizeof limits) ||
	        !AssignProcessToJobObject(h->job, h->process)) {
		/* running outside its limits is not allowed: kill it, fail */
		err = GetLastError();
		kill_and_confirm(h);
		goto fail;
	}
	if (opt->sandbox) {
		if (phase) {
			*phase = WF_DEC_START_TOKEN;
		}
		if (!wf_dechost_sandboxed(h)) {
			err = ERROR_ACCESS_DENIED;
			kill_and_confirm(h);
			goto fail;
		}
	}
	if (phase) {
		*phase = WF_DEC_START_RESUME;
	}
	if (ResumeThread(pi.hThread) == (DWORD)-1) {
		err = GetLastError();
		kill_and_confirm(h);
		goto fail;
	}
	CloseHandle(pi.hThread);
	/* the host owns its ends now; ours must close or EOF never arrives */
	CloseHandle(host_in);
	CloseHandle(host_out);
	DeleteProcThreadAttributeList(si.lpAttributeList);
	free(si.lpAttributeList);
	if (sid) {
		FreeSid(sid);
	}
	*out = h;
	if (phase) {
		*phase = WF_DEC_START_READY;
	}
	return WF_OK;
fail:
	if (os_error) {
		*os_error = err ? err : GetLastError();
	}
	if (pi.hThread) {
		CloseHandle(pi.hThread);
	}
	if (host_in) {
		CloseHandle(host_in);
	}
	if (host_out) {
		CloseHandle(host_out);
	}
	if (si.lpAttributeList) {
		DeleteProcThreadAttributeList(si.lpAttributeList);
		free(si.lpAttributeList);
	}
	if (sid) {
		FreeSid(sid); /* from CreateAppContainerProfile */
	}
	wf_dechost_stop(h);
	return WF_E_OS;
}

void wf_dechost_stop(WfDecHost *h)
{
	if (!h) {
		return;
	}
	/* closing the request pipe is the polite way: the host reads end of file
	 * between requests and leaves; anything else is killed */
	if (h->to_host) {
		CloseHandle(h->to_host);
		h->to_host = NULL;
	}
	if (h->alive && h->process &&
	        WAIT_OBJECT_0 != WaitForSingleObject(h->process, 1000)) {
		kill_and_confirm(h);
	}
	if (h->from_host) {
		CloseHandle(h->from_host);
	}
	if (h->read_event) {
		CloseHandle(h->read_event);
	}
	if (h->write_event) {
		CloseHandle(h->write_event);
	}
	if (h->job) {
		CloseHandle(h->job); /* KILL_ON_JOB_CLOSE is the last resort */
	}
	if (h->process) {
		CloseHandle(h->process);
	}
	free(h);
}

int wf_dechost_alive(const WfDecHost *h)
{
	return h && h->alive;
}

DWORD wf_dechost_exit_code(const WfDecHost *h)
{
	return h ? h->exit_code : 0;
}

DWORD wf_dechost_pid(const WfDecHost *h)
{
	return h && h->process ? GetProcessId(h->process) : 0;
}

int wf_dechost_sandboxed(const WfDecHost *h)
{
	HANDLE token = NULL;
	DWORD is_container = 0, len = 0;
	int yes = 0;

	if (h && h->process &&
	        OpenProcessToken(h->process, TOKEN_QUERY, &token)) {
		yes = GetTokenInformation(token, TokenIsAppContainer,
		              &is_container, sizeof is_container, &len) &&
		        is_container;
		CloseHandle(token);
	}
	return yes;
}

/* Outcomes of a timed pipe operation. The distinction matters: a late host
 * and a dead host are different findings, and the coarse system tick must not
 * decide which. */
enum
{
	IO_OK = 1,
	IO_FAILED = 0,
	IO_TIMEOUT = -1,
	IO_ABANDONED = -2
};

DWORD wf_dechost_remaining_ms(ULONGLONG deadline, ULONGLONG now)
{
	ULONGLONG left = deadline > now ? deadline - now : 0;

	return left < INFINITE ? (DWORD)left : INFINITE - 1;
}

/* Reads exactly n bytes before `deadline` (a GetTickCount64 value). */
static int read_deadline(WfDecHost *h,
        void *buf,
        size_t n,
        ULONGLONG deadline,
        HANDLE abandon,
        uint32_t grace_ms)
{
	size_t done = 0;
	int abandoned = 0;

	while (done < n) {
		OVERLAPPED ov;
		DWORD got = 0,
		      want = (DWORD)(n - done > (1u << 20) ? (1u << 20)
		                                           : n - done);
		HANDLE wait[3];
		DWORD nwait = 2;
		ULONGLONG now = GetTickCount64();
		DWORD r;

		if (now >= deadline) {
			return abandoned ? IO_ABANDONED : IO_TIMEOUT;
		}
		memset(&ov, 0, sizeof ov);
		ov.hEvent = h->read_event;
		ResetEvent(h->read_event);
		if (!ReadFile(h->from_host, (char *)buf + done, want, NULL,
		            &ov) &&
		        ERROR_IO_PENDING != GetLastError()) {
			return IO_FAILED;
		}
		wait[0] = h->read_event;
		wait[1] = h->process;
		if (abandon && !abandoned) {
			wait[nwait++] = abandon;
		}
		r = WaitForMultipleObjects(nwait, wait, FALSE,
		        wf_dechost_remaining_ms(deadline, now));
		if (WAIT_OBJECT_0 + 2 == r && 3 == nwait) {
			/* the answer is not wanted any more: keep the pending read,
			 * shorten the deadline, wait again */
			ULONGLONG at = GetTickCount64(), limit = at + grace_ms;

			abandoned = 1;
			if (limit < deadline) {
				deadline = limit;
			}
			r = WaitForMultipleObjects(2, wait, FALSE,
			        wf_dechost_remaining_ms(deadline, at));
		}
		if (WAIT_OBJECT_0 != r) {
			/* timeout, or the process ended: take back the pending read; a
			 * reply that arrived in the same instant is still read */
			CancelIoEx(h->from_host, &ov);
			if (!GetOverlappedResult(
			            h->from_host, &ov, &got, TRUE) ||
			        0 == got) {
				return WAIT_TIMEOUT == r
				        ? (abandoned ? IO_ABANDONED
				                     : IO_TIMEOUT)
				        : IO_FAILED;
			}
		} else if (!GetOverlappedResult(
		                   h->from_host, &ov, &got, FALSE) ||
		        0 == got) {
			return IO_FAILED;
		}
		done += got;
	}
	return IO_OK;
}

static int write_deadline(
        WfDecHost *h, const void *buf, size_t n, ULONGLONG deadline)
{
	OVERLAPPED ov;
	DWORD put = 0, r;
	ULONGLONG now = GetTickCount64();

	if (now >= deadline) {
		return IO_TIMEOUT;
	}
	if (n > (size_t)PIPE_BYTES) {
		return IO_FAILED;
	}
	memset(&ov, 0, sizeof ov);
	ov.hEvent = h->write_event;
	ResetEvent(h->write_event);
	if (!WriteFile(h->to_host, buf, (DWORD)n, NULL, &ov) &&
	        ERROR_IO_PENDING != GetLastError()) {
		return IO_FAILED;
	}
	r = WaitForSingleObject(
	        h->write_event, wf_dechost_remaining_ms(deadline, now));
	if (WAIT_OBJECT_0 != r) {
		CancelIoEx(h->to_host, &ov);
		(void)GetOverlappedResult(h->to_host, &ov, &put, TRUE);
		return WAIT_TIMEOUT == r ? IO_TIMEOUT : IO_FAILED;
	}
	return GetOverlappedResult(h->to_host, &ov, &put, FALSE) && put == n
	        ? IO_OK
	        : IO_FAILED;
}

/*
 * One request, one reply. The input is either a file handle (`file`, which is
 * duplicated into the host) or `bytes` (sent on the pipe after the request).
 */
static WfDecodeStatus exchange(WfDecHost *h,
        WfDecodeKind kind,
        HANDLE file,
        const void *bytes,
        uint64_t bytes_n,
        uint32_t box_w,
        uint32_t box_h,
        uint32_t page,
        const wchar_t *password,
        uint32_t timeout_ms,
        HANDLE abandon,
        uint32_t abandon_grace_ms,
        WfPixmap **pm,
        WfDecodeInfo *info)
{
	WfDecodeRequest rq;
	WfDecodeReply rp;
	unsigned char msg[WF_DWIRE_REQUEST_HEADER + 2 * WF_DWIRE_PASSWORD_MAX];
	unsigned char hdr[WF_DWIRE_REPLY_HEADER];
	uint16_t pw[WF_DWIRE_PASSWORD_MAX];
	LARGE_INTEGER size;
	HANDLE remote = NULL;
	ULONGLONG deadline = GetTickCount64() + timeout_ms;
	WfPixmap *got = NULL;
	size_t n, pw_len = password ? wcslen(password) : 0;
	WfDecodeStatus lost = WF_DSTAT_HOST_LOST;
	int io;

	*pm = NULL;
	if (info) {
		memset(info, 0, sizeof *info);
	}
	if (!h || !h->alive) {
		return WF_DSTAT_HOST_LOST;
	}
	if (WAIT_OBJECT_0 == WaitForSingleObject(h->process, 0)) {
		kill_and_confirm(h); /* it ended on its own: record how */
		return WF_DSTAT_HOST_LOST;
	}
	if (file) {
		if (!GetFileSizeEx(file, &size)) {
			return WF_DSTAT_CORRUPT;
		}
		bytes_n = size.QuadPart > 0 ? (uint64_t)size.QuadPart : 0;
	}
	if (0 == bytes_n || bytes_n > WF_DWIRE_INPUT_MAX) {
		return bytes_n ? WF_DSTAT_TOO_LARGE : WF_DSTAT_CORRUPT;
	}
	if (pw_len > WF_DWIRE_PASSWORD_MAX) {
		return WF_DSTAT_UNSUPPORTED;
	}
	if (file) {
		/* the host gets a read-only copy of the handle, valid in ITS table */
		if (!DuplicateHandle(GetCurrentProcess(), file, h->process,
		            &remote, FILE_GENERIC_READ, FALSE, 0)) {
			if (WAIT_OBJECT_0 ==
			        WaitForSingleObject(h->process, 0)) {
				kill_and_confirm(h);
				return WF_DSTAT_HOST_LOST;
			}
			return WF_DSTAT_INTERNAL;
		}
	}
	memset(&rq, 0, sizeof rq);
	rq.request = ++h->next_request;
	rq.kind = (uint32_t)kind;
	rq.box_w = box_w;
	rq.box_h = box_h;
	rq.page = page;
	rq.input_size = bytes_n;
	rq.password_units = (uint32_t)pw_len;
	rq.flags = file ? 0 : WF_DREQ_INLINE;
	rq.input_handle = (uint64_t)(uintptr_t)remote;
	for (size_t i = 0; i < pw_len; i++) {
		pw[i] = (uint16_t)password[i];
	}
	n = wf_dwire_request_encode(&rq, pw, msg, sizeof msg);
	if (0 == n) {
		SecureZeroMemory(pw, sizeof pw);
		SecureZeroMemory(msg, sizeof msg);
		/* nothing was sent, so the host still owns a handle it will never
		 * be told about: closing it needs the host; kill and start anew */
		kill_and_confirm(h);
		return WF_DSTAT_INTERNAL;
	}
	io = write_deadline(h, msg, n, deadline);
	SecureZeroMemory(pw, sizeof pw);
	SecureZeroMemory(msg, sizeof msg);
	/* an inline file goes down the pipe in slices the pipe can take */
	for (uint64_t sent = 0; IO_OK == io && !file && sent < bytes_n;) {
		size_t slice = bytes_n - sent > PIPE_BYTES
		        ? PIPE_BYTES
		        : (size_t)(bytes_n - sent);

		io = write_deadline(h, (const unsigned char *)bytes + sent,
		        slice, deadline);
		sent += slice;
	}
	if (IO_OK == io) {
		io = read_deadline(h, hdr, sizeof hdr, deadline, abandon,
		        abandon_grace_ms);
	}
	if (IO_OK != io || !wf_dwire_reply_header(hdr, &rq, &rp)) {
		/* no valid reply: late, gone, or wrong; in every case the host is
		 * no longer trusted to be in step with us */
		lost = IO_TIMEOUT == io      ? WF_DSTAT_TIMEOUT
		        : IO_ABANDONED == io ? WF_DSTAT_ABORTED
		                             : WF_DSTAT_HOST_LOST;
		goto kill;
	}
	if (info) {
		info->page_count = rp.page_count;
		info->src_w = rp.src_w;
		info->src_h = rp.src_h;
		info->flags = rp.flags;
	}
	if (WF_DSTAT_OK != rp.status) {
		return (WfDecodeStatus)rp.status;
	}
	if (WF_OK != wf_pixmap_new(rp.out_w, rp.out_h, &got)) {
		goto kill;
	}
	io = read_deadline(h, got->px, (size_t)rp.payload, deadline, abandon,
	        abandon_grace_ms);
	if (IO_OK != io) {
		wf_pixmap_free(got);
		lost = IO_TIMEOUT == io      ? WF_DSTAT_TIMEOUT
		        : IO_ABANDONED == io ? WF_DSTAT_ABORTED
		                             : WF_DSTAT_HOST_LOST;
		goto kill;
	}
	*pm = got;
	return WF_DSTAT_OK;
kill:
	kill_and_confirm(h);
	return lost;
}

WfDecodeStatus wf_dechost_decode(WfDecHost *h,
        WfDecodeKind kind,
        HANDLE file,
        uint32_t box_w,
        uint32_t box_h,
        uint32_t page,
        const wchar_t *password,
        uint32_t timeout_ms,
        HANDLE abandon,
        uint32_t abandon_grace_ms,
        WfPixmap **pm,
        WfDecodeInfo *info)
{
	if (!file || INVALID_HANDLE_VALUE == file) {
		*pm = NULL;
		return WF_DSTAT_CORRUPT;
	}
	return exchange(h, kind, file, NULL, 0, box_w, box_h, page, password,
	        timeout_ms, abandon, abandon_grace_ms, pm, info);
}

WfDecodeStatus wf_dechost_decode_bytes(WfDecHost *h,
        WfDecodeKind kind,
        const void *bytes,
        size_t n,
        uint32_t box_w,
        uint32_t box_h,
        uint32_t page,
        const wchar_t *password,
        uint32_t timeout_ms,
        HANDLE abandon,
        uint32_t abandon_grace_ms,
        WfPixmap **pm,
        WfDecodeInfo *info)
{
	if (!bytes || 0 == n) {
		*pm = NULL;
		return WF_DSTAT_CORRUPT;
	}
	return exchange(h, kind, NULL, bytes, n, box_w, box_h, page, password,
	        timeout_ms, abandon, abandon_grace_ms, pm, info);
}

#include "wf_sftpplace_win.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../base/wf_macro.h"
#include "wf_sftpc.h"
#include "wf_sshcmd.h"
#include "wf_sshproc_win.h"

#define WAIT_DEFAULT_MS 20000u
#define HOST_MAX 64
#define HEAD L"\\\\sftp.invalid\\"

static SRWLOCK lock = SRWLOCK_INIT;
static char session_host[HOST_MAX];
static WfSshProc *proc;
static WfSftpClient *client;
static char note[240];
static wchar_t *test_exe, *test_cmd;
static unsigned wait_ms = WAIT_DEFAULT_MS;

#if defined(__clang__) || defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
static void say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(note, sizeof note, fmt, ap);
	va_end(ap);
}

bool wf_sftp_place_is(const wchar_t *path)
{
	size_t n = wcslen(HEAD);

	return path && 0 == _wcsnicmp(path, HEAD, n);
}

void wf_sftp_place_note(char *out, size_t cap)
{
	if (!out || !cap) {
		return;
	}
	AcquireSRWLockShared(&lock);
	snprintf(out, cap, "%s", note);
	ReleaseSRWLockShared(&lock);
}

void wf_sftp_place_test_command(const wchar_t *exe, const wchar_t *cmdline)
{
	AcquireSRWLockExclusive(&lock);
	free(test_exe);
	free(test_cmd);
	test_exe = exe ? _wcsdup(exe) : NULL;
	test_cmd = cmdline ? _wcsdup(cmdline) : NULL;
	ReleaseSRWLockExclusive(&lock);
}

void wf_sftp_place_test_wait(unsigned ms)
{
	AcquireSRWLockExclusive(&lock);
	wait_ms = ms ? ms : WAIT_DEFAULT_MS;
	ReleaseSRWLockExclusive(&lock);
}

static void drop_session(void)
{
	wf_sftpc_close(client);
	client = NULL;
	if (proc) {
		wf_sshproc_cancel(proc);
		wf_sshproc_free(proc);
		proc = NULL;
	}
	session_host[0] = 0;
}

void wf_sftp_place_close(void)
{
	AcquireSRWLockExclusive(&lock);
	drop_session();
	ReleaseSRWLockExclusive(&lock);
}

static const char *why_text(WfSshFail f)
{
	switch (f) {
	case WF_SSH_FAIL_HOSTKEY_UNKNOWN:
		return "the host key is not known: run ssh <host> once to "
		       "check "
		       "and accept it (piero never accepts a key)";
	case WF_SSH_FAIL_HOSTKEY_CHANGED:
		return "the host key has CHANGED: not connecting (possible "
		       "interception)";
	case WF_SSH_FAIL_AUTH:
		return "login refused (keys and agent only; no password "
		       "prompt)";
	case WF_SSH_FAIL_DNS:     return "the host name cannot be found";
	case WF_SSH_FAIL_REFUSED: return "the connection was refused";
	case WF_SSH_FAIL_TIMEOUT: return "the connection timed out";
	case WF_SSH_FAIL_NO_SFTP: return "the server has no sftp service";
	default:                  return "could not connect (try: ssh <host>)";
	}
}

static bool to_wide(const char *s, wchar_t *out, int cap)
{
	return MultiByteToWideChar(
	               CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap) > 0;
}

/* Starts ssh.exe for `host` and opens the SFTP session. Caller holds the lock.
 * `cancelled` is asked while it waits for the server. */
static WfStatus connect_host(
        const char *host, bool (*cancelled)(void *), void *cctx)
{
	wchar_t exe[MAX_PATH + 40], wcmd[1400];
	char nexe[MAX_PATH * 3], ncmd[1200];
	const wchar_t *use_exe, *use_cmd;
	WfStatus st;
	WfSftpStream stream;
	int code = 1;
	char err[512] = "";

	if (test_exe && test_cmd) {
		use_exe = test_exe;
		use_cmd = test_cmd;
	} else {
		wchar_t root[MAX_PATH];
		DWORD n =
		        GetEnvironmentVariableW(L"SystemRoot", root, MAX_PATH);

		if (0 == n || n >= MAX_PATH) {
			say("the Windows folder is unknown");
			return WF_E_OS;
		}
		swprintf(exe, MAX_PATH + 40, L"%ls\\System32\\OpenSSH\\ssh.exe",
		        root);
		if (INVALID_FILE_ATTRIBUTES == GetFileAttributesW(exe)) {
			say("the OpenSSH client (ssh.exe) is not installed");
			return WF_E_NOENT;
		}
		if (!WideCharToMultiByte(CP_UTF8, 0, exe, -1, nexe,
		            (int)sizeof nexe, NULL, NULL)) {
			return WF_E_OS;
		}
		st = wf_ssh_command(nexe, host, 0, NULL, ncmd, sizeof ncmd);
		if (WF_OK != st) {
			say("that is not a usable ssh host name");
			return WF_E_INVAL;
		}
		if (!to_wide(ncmd, wcmd, (int)WF_ARRAY_LEN(wcmd))) {
			return WF_E_OS;
		}
		use_exe = exe;
		use_cmd = wcmd;
	}
	st = wf_sshproc_start(use_exe, use_cmd, &proc);
	if (WF_OK != st) {
		say("ssh.exe could not be started");
		return st;
	}
	stream = wf_sshproc_stream(proc);
	st = wf_sftpc_open_ex(&stream, wait_ms, cancelled, cctx, &client);
	if (WF_OK != st) {
		if (!wf_sshproc_wait(proc, 3000, &code)) {
			code = 1;
		}
		wf_sshproc_stderr(proc, err, sizeof err);
		say("%s", why_text(wf_ssh_classify(code, err)));
		drop_session();
		return WF_E_ACCESS;
	}
	snprintf(session_host, sizeof session_host, "%s", host);
	return WF_OK;
}

/* "\\sftp.invalid\host\a\b" -> host "host", POSIX path "/a/b". */
static bool split(const wchar_t *path,
        char host[HOST_MAX],
        char *posix,
        size_t cap,
        size_t *posix_n)
{
	const wchar_t *p = path + wcslen(HEAD), *h = p;
	size_t hn = 0, n = 0;

	while (*p && L'\\' != *p) {
		/* the alphabet of an ssh host name; nothing that could be read
		 * as an option or split into words */
		bool ok = (*p >= L'0' && *p <= L'9') ||
		        (*p >= L'a' && *p <= L'z') ||
		        (*p >= L'A' && *p <= L'Z') || L'.' == *p ||
		        L'_' == *p || (L'-' == *p && hn > 0);

		if (!ok || hn + 1 >= HOST_MAX) {
			return false;
		}
		host[hn++] = (char)*p++;
	}
	host[hn] = 0;
	if (0 == hn || h == p) {
		return false;
	}
	while (*p) {
		wchar_t comp[256];
		size_t cn = 0;
		int bytes;

		while (L'\\' == *p) {
			p++;
		}
		while (*p && L'\\' != *p && cn + 1 < WF_ARRAY_LEN(comp)) {
			comp[cn++] = *p++;
		}
		if (*p && L'\\' != *p) {
			return false; /* a component longer than a name can be */
		}
		comp[cn] = 0;
		if (0 == cn) {
			continue;
		}
		bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, comp,
		        (int)cn, posix + n + 1, (int)(cap - n - 2), NULL, NULL);
		if (bytes <= 0) {
			return false;
		}
		posix[n] = '/';
		n += 1 + (size_t)bytes;
	}
	if (0 == n) {
		posix[n++] = '/';
	}
	posix[n] = 0;
	*posix_n = n;
	return true;
}

static WfFileTime unix_to_filetime(uint32_t secs)
{
	return ((WfFileTime)secs + 11644473600ull) * 10000000ull;
}

static WfStatus fill(WfDir *d, const WfSftpListing *l, size_t *skipped)
{
	for (size_t i = 0; i < l->n; i++) {
		const WfSftpEntry *e = &l->e[i];
		wchar_t name[1024];
		int wn;
		uint32_t flags = 0, type = e->attrs.perm & WF_SFTP_S_IFMT;
		WfStatus st;

		wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		        (const char *)e->name, (int)e->name_n, name,
		        (int)WF_ARRAY_LEN(name) - 1);
		if (wn <= 0 || wmemchr(name, L'\\', (size_t)wn)) {
			(*skipped)++;
			continue;
		}
		name[wn] = 0;
		if (e->attrs.has_perm && WF_SFTP_S_IFDIR == type) {
			flags |= WF_ENT_DIR;
		} else if (e->attrs.has_perm && WF_SFTP_S_IFLNK == type) {
			flags |= WF_ENT_LINK;
		}
		if (L'.' == name[0]) {
			flags |= WF_ENT_HIDDEN;
		}
		st = wf_dir_add(d, name, (size_t)wn, flags,
		        e->attrs.has_size ? e->attrs.size : 0,
		        e->attrs.has_time ? unix_to_filetime(e->attrs.mtime)
		                          : 0);
		if (WF_OK != st) {
			return st;
		}
		/* a remote file called x.zip is not an archive we can open */
		if (d->n) {
			d->e[d->n - 1].flags &= ~(uint32_t)WF_ENT_ARCHIVE;
		}
	}
	return WF_OK;
}

static WfStatus list_once(const char *host,
        const char *posix,
        size_t posix_n,
        size_t limit,
        bool (*cancelled)(void *),
        void *cctx,
        WfSftpListing *l)
{
	WfStatus st;

	if (!client || strcmp(host, session_host)) {
		drop_session();
		st = connect_host(host, cancelled, cctx);
		if (WF_OK != st) {
			return st;
		}
	}
	/* the question belongs to this request: the session outlives it */
	wf_sftpc_set_cancel(client, cancelled, cctx);
	st = wf_sftpc_list(client, (const uint8_t *)posix, posix_n, limit, l);
	wf_sftpc_set_cancel(client, NULL, NULL);
	if (WF_OK != st && wf_sftpc_broken(client)) {
		drop_session(); /* a reply left unread, or a stopped call: not reused */
	}
	return st;
}

WfStatus wf_sftp_place_list(const wchar_t *path,
        size_t limit,
        bool (*cancelled)(void *),
        void *cctx,
        WfDir **out)
{
	char host[HOST_MAX], posix[2048];
	size_t posix_n = 0, skipped = 0;
	WfSftpListing l;
	WfDir *d = NULL;
	WfStatus st;

	if (!out || !wf_sftp_place_is(path)) {
		return WF_E_INVAL;
	}
	*out = NULL;
	if (!split(path, host, posix, sizeof posix, &posix_n)) {
		AcquireSRWLockExclusive(&lock);
		say("that is not a usable SFTP path");
		ReleaseSRWLockExclusive(&lock);
		return WF_E_INVAL;
	}
	AcquireSRWLockExclusive(&lock);
	note[0] = 0;
	memset(&l, 0, sizeof l);
	st = list_once(host, posix, posix_n, limit, cancelled, cctx, &l);
	if (WF_OK != st && WF_E_CANCELLED != st && !client && note[0] == 0) {
		/* the session broke mid-way: one fresh attempt, no loop */
		wf_sftp_listing_free(&l);
		memset(&l, 0, sizeof l);
		st = list_once(
		        host, posix, posix_n, limit, cancelled, cctx, &l);
	}
	if (WF_OK != st) {
		if (!note[0]) {
			say(WF_E_NOENT == st ? "no such folder"
			                : WF_E_ACCESS == st
			                ? "permission denied"
			                : "the listing failed");
		}
		wf_sftp_listing_free(&l);
		ReleaseSRWLockExclusive(&lock);
		return st;
	}
	d = wf_dir_new();
	st = d ? fill(d, &l, &skipped) : WF_E_NOMEM;
	if (WF_OK == st && skipped) {
		say("%zu name(s) left out: not valid UTF-8 or containing a "
		    "backslash",
		        skipped);
	}
	if (WF_OK == st && l.truncated) {
		d->truncated = true;
	}
	wf_sftp_listing_free(&l);
	ReleaseSRWLockExclusive(&lock);
	if (WF_OK != st) {
		wf_dir_free(d);
		return st;
	}
	*out = d;
	return WF_OK;
}

WfStatus wf_sftp_place_read(const wchar_t *path,
        size_t max_bytes,
        uint8_t **out,
        size_t *n,
        uint32_t *mtime)
{
	char host[HOST_MAX], posix[2048];
	size_t posix_n = 0, got = 0;
	WfSftpAttrs at;
	uint8_t *buf;
	WfStatus st;

	if (!out || !n || !mtime || !wf_sftp_place_is(path)) {
		return WF_E_INVAL;
	}
	*out = NULL;
	*n = 0;
	*mtime = 0;
	if (!split(path, host, posix, sizeof posix, &posix_n) || 1 == posix_n) {
		return WF_E_INVAL;
	}
	AcquireSRWLockExclusive(&lock);
	note[0] = 0;
	if (!client || strcmp(host, session_host)) {
		drop_session();
		st = connect_host(host, NULL,
		        NULL); /* a preview read: not a worker's wait */
		if (WF_OK != st) {
			ReleaseSRWLockExclusive(&lock);
			return st;
		}
	}
	st = wf_sftpc_stat(client, (const uint8_t *)posix, posix_n, false, &at);
	if (WF_OK == st &&
	        (!at.has_perm || !at.has_size ||
	                WF_SFTP_S_IFREG != (at.perm & WF_SFTP_S_IFMT))) {
		st = WF_E_INVAL;
	}
	if (WF_OK == st &&
	        ((max_bytes && at.size > max_bytes) ||
	                at.size > (uint64_t)SIZE_MAX / 2)) {
		st = WF_E_RANGE;
	}
	if (WF_OK != st) {
		if (wf_sftpc_broken(client)) {
			drop_session();
		}
		ReleaseSRWLockExclusive(&lock);
		return st;
	}
	buf = malloc(at.size ? (size_t)at.size : 1);
	if (!buf) {
		ReleaseSRWLockExclusive(&lock);
		return WF_E_NOMEM;
	}
	st = wf_sftpc_read_range(client, (const uint8_t *)posix, posix_n, 0,
	        (size_t)at.size, buf, &got);
	if (WF_OK == st && got != at.size) {
		st = WF_E_CHANGED; /* it changed while it was being read */
	}
	if (WF_OK != st && wf_sftpc_broken(client)) {
		drop_session();
	}
	ReleaseSRWLockExclusive(&lock);
	if (WF_OK != st) {
		free(buf);
		return st;
	}
	*out = buf;
	*n = got;
	*mtime = at.has_time ? at.mtime : 0;
	return WF_OK;
}

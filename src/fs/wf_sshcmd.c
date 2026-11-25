#include "wf_sshcmd.h"

#include <stdio.h>
#include <string.h>

static bool token_ok(const char *s, size_t max)
{
	size_t n = s ? strlen(s) : 0;

	if (n < 1 || n > max || '-' == s[0]) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
		            (c >= 'A' && c <= 'Z') || '.' == c || '_' == c ||
		            '-' == c || '+' == c)) {
			return false;
		}
	}
	return true;
}

/* A file path that can sit inside double quotes on a Windows command line. */
static bool path_ok(const char *s)
{
	size_t n = s ? strlen(s) : 0;

	if (n < 1 || n > 1024 || '\\' == s[n - 1]) {
		return false; /* a trailing backslash would escape the quote */
	}
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c < 0x20 || '"' == c || '%' == c) {
			return false;
		}
	}
	return true;
}

WfStatus wf_ssh_command(const char *exe,
        const char *alias,
        unsigned port,
        const char *identity,
        char *out,
        size_t cap)
{
	int n;

	if (!out || cap < 2 || !path_ok(exe) || !token_ok(alias, 100) ||
	        port > 65535u || (identity && !path_ok(identity))) {
		return WF_E_INVAL;
	}
	n = snprintf(out, cap,
	        "\"%s\" -o BatchMode=yes -o StrictHostKeyChecking=yes"
	        " -o UpdateHostKeys=no -o VerifyHostKeyDNS=no"
	        " -o PreferredAuthentications=publickey"
	        " -o PasswordAuthentication=no -o "
	        "KbdInteractiveAuthentication=no"
	        " -o ClearAllForwardings=yes -o ForwardAgent=no"
	        " -o ForwardX11=no -o "
	        "ConnectTimeout=10"
	        " -p %u%s%s%s -s -- %s sftp",
	        exe, port ? port : 22u, identity ? " -i \"" : "",
	        identity ? identity : "", identity ? "\"" : "", alias);
	return n < 0 || (size_t)n >= cap ? WF_E_RANGE : WF_OK;
}

WfSshFail wf_ssh_classify(int exit_code, const char *err)
{
	if (0 == exit_code) {
		return WF_SSH_FAIL_NONE;
	}
	if (!err) {
		return WF_SSH_FAIL_OTHER;
	}
	if (strstr(err, "REMOTE HOST IDENTIFICATION HAS CHANGED") ||
	        strstr(err, "Offending key") || strstr(err, "Host key for ")) {
		return WF_SSH_FAIL_HOSTKEY_CHANGED;
	}
	if (strstr(err, "Host key verification failed") ||
	        strstr(err, "host key is known")) {
		return WF_SSH_FAIL_HOSTKEY_UNKNOWN;
	}
	if (strstr(err, "Permission denied")) {
		return WF_SSH_FAIL_AUTH;
	}
	if (strstr(err, "Could not resolve hostname")) {
		return WF_SSH_FAIL_DNS;
	}
	if (strstr(err, "Connection refused")) {
		return WF_SSH_FAIL_REFUSED;
	}
	if (strstr(err, "timed out") || strstr(err, "Connection timed out")) {
		return WF_SSH_FAIL_TIMEOUT;
	}
	if (strstr(err, "subsystem request failed")) {
		return WF_SSH_FAIL_NO_SFTP;
	}
	return WF_SSH_FAIL_OTHER;
}

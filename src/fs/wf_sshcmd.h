#ifndef WF_SSHCMD_H
#define WF_SSHCMD_H

#include <stdbool.h>
#include <stddef.h>

#include "../base/wf_status.h"

/*
 * How piero starts the user's own OpenSSH client for SFTP, and how it reads
 * the client's refusals. Pure string work; nothing is started here.
 *
 * Host-key and authentication policy are fixed on the command line and are
 * not options: an unknown or changed key stops the connection (piero never
 * accepts a key and never writes known_hosts), only keys and agents are
 * used (no password or keyboard prompt that nobody could answer), and
 * forwarding is switched off. The host is an alias the user chose in their
 * own ssh configuration, validated to a small alphabet so that nothing in it
 * can be read as an option or split into words.
 */

/* ssh.exe -s ... into `out` (a CreateProcess command line). `exe` is the full
 * path of ssh.exe found by the caller; `identity` (optional) a key file path
 * the user named. WF_E_INVAL for a bad alias, port, exe or identity;
 * WF_E_RANGE if it does not fit. */
WfStatus wf_ssh_command(const char *exe,
        const char *alias,
        unsigned port,
        const char *identity,
        char *out,
        size_t cap);

typedef enum
{
	WF_SSH_FAIL_NONE,
	WF_SSH_FAIL_HOSTKEY_UNKNOWN, /* not in known_hosts: connect once with ssh */
	WF_SSH_FAIL_HOSTKEY_CHANGED, /* possible man-in-the-middle: never go on */
	WF_SSH_FAIL_AUTH,
	WF_SSH_FAIL_DNS,
	WF_SSH_FAIL_REFUSED,
	WF_SSH_FAIL_TIMEOUT,
	WF_SSH_FAIL_NO_SFTP, /* the server has no sftp subsystem */
	WF_SSH_FAIL_OTHER
} WfSshFail;

/* Reads ssh's exit code and what it wrote to stderr. A changed key is
 * checked before an unknown one because both end in "Host key verification
 * failed". Exit code 0 is NONE whatever stderr says. */
WfSshFail wf_ssh_classify(int exit_code, const char *err_text);

#endif

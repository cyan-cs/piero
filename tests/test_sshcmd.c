/*
 * The ssh.exe command line and the reading of its refusals. The policy that
 * keeps a changed host key out is on the command line itself, so it is
 * asserted there.
 */
#include <string.h>

#include "fs/wf_sshcmd.h"
#include "wf_test.h"

#define EXE "C:\\Windows\\System32\\OpenSSH\\ssh.exe"

static void the_policy_is_on_the_command_line(void)
{
	char c[1024];

	CHECK(WF_OK == wf_ssh_command(EXE, "box", 2200, NULL, c, sizeof c));
	CHECK(strstr(c, "StrictHostKeyChecking=yes"));
	CHECK(strstr(c, "UpdateHostKeys=no"));
	CHECK(strstr(c, "VerifyHostKeyDNS=no"));
	CHECK(strstr(c, "PreferredAuthentications=publickey"));
	CHECK(strstr(c, "BatchMode=yes"));
	CHECK(strstr(c, "PasswordAuthentication=no"));
	CHECK(strstr(c, "KbdInteractiveAuthentication=no"));
	CHECK(strstr(c, "ClearAllForwardings=yes"));
	CHECK(strstr(c, "ForwardAgent=no"));
	CHECK(strstr(c, "ForwardX11=no"));
	CHECK(!strstr(c, "StrictHostKeyChecking=no"));
	CHECK(!strstr(c, "accept-new") && !strstr(c, "UserKnownHostsFile"));
	CHECK(strstr(c, " -p 2200 ") && strstr(c, " -s -- box sftp"));
	CHECK(c == strstr(c, "\"" EXE "\" "));

	CHECK(WF_OK == wf_ssh_command(EXE, "box", 0, NULL, c, sizeof c));
	CHECK(strstr(c, " -p 22 "));
	CHECK(WF_OK ==
	        wf_ssh_command(EXE, "box", 22, "C:\\k\\id key", c, sizeof c));
	CHECK(strstr(c, " -i \"C:\\k\\id key\" -s"));
}

static void nothing_in_a_name_can_become_an_option_or_a_second_word(void)
{
	char c[1024];
	static const char *const bad[] = {"", "-oProxyCommand=calc", "a b",
	        "a;b", "a\"b", "a&b", "a\nb", "a|b", "a$b", "a`b", "-", "h:22"};

	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
		CHECK(WF_E_INVAL ==
		        wf_ssh_command(EXE, bad[i], 22, NULL, c, sizeof c));
	}
	CHECK(WF_E_INVAL ==
	        wf_ssh_command(EXE, "box", 65536, NULL, c, sizeof c));
	CHECK(WF_E_INVAL == wf_ssh_command(NULL, "box", 22, NULL, c, sizeof c));
	CHECK(WF_E_INVAL ==
	        wf_ssh_command(
	                "C:\\a\"b\\ssh.exe", "box", 22, NULL, c, sizeof c));
	CHECK(WF_E_INVAL ==
	        wf_ssh_command(EXE, "box", 22, "k\" -o X=y", c, sizeof c));
	CHECK(WF_E_INVAL ==
	        wf_ssh_command(EXE, "box", 22, "C:\\k\\", c, sizeof c));
	CHECK(WF_E_INVAL ==
	        wf_ssh_command(EXE, "box", 22, "%USERPROFILE%", c, sizeof c));
	CHECK(WF_E_RANGE == wf_ssh_command(EXE, "box", 22, NULL, c, 40));
}

static void refusals_are_told_apart_and_a_changed_key_is_never_unknown(void)
{
	CHECK(WF_SSH_FAIL_NONE ==
	        wf_ssh_classify(0, "Host key verification failed."));
	CHECK(WF_SSH_FAIL_HOSTKEY_UNKNOWN ==
	        wf_ssh_classify(255,
	                "No ED25519 host key is known for box and you have "
	                "requested strict checking.\r\nHost key verification "
	                "failed.\r\n"));
	CHECK(WF_SSH_FAIL_HOSTKEY_CHANGED ==
	        wf_ssh_classify(255,
	                "@@@@\r\n@    WARNING: REMOTE HOST IDENTIFICATION HAS "
	                "CHANGED!     @\r\n...\r\nHost key verification "
	                "failed.\r\n"));
	CHECK(WF_SSH_FAIL_HOSTKEY_CHANGED ==
	        wf_ssh_classify(255,
	                "Host key for box has changed and you have requested "
	                "strict checking.\r\nHost key verification "
			"failed.\r\n"));
	CHECK(WF_SSH_FAIL_AUTH ==
	        wf_ssh_classify(
	                255, "u@box: Permission denied (publickey).\r\n"));
	CHECK(WF_SSH_FAIL_DNS ==
	        wf_ssh_classify(
	                255, "ssh: Could not resolve hostname box: x\r\n"));
	CHECK(WF_SSH_FAIL_REFUSED ==
	        wf_ssh_classify(255,
	                "ssh: connect to host box port 22: Connection "
	                "refused\r\n"));
	CHECK(WF_SSH_FAIL_TIMEOUT ==
	        wf_ssh_classify(255,
	                "ssh: connect to host box port 22: Connection timed "
	                "out\r\n"));
	CHECK(WF_SSH_FAIL_NO_SFTP ==
	        wf_ssh_classify(
	                1, "subsystem request failed on channel 0\r\n"));
	CHECK(WF_SSH_FAIL_OTHER == wf_ssh_classify(1, "something else"));
	CHECK(WF_SSH_FAIL_OTHER == wf_ssh_classify(1, NULL));
}

int main(void)
{
	the_policy_is_on_the_command_line();
	nothing_in_a_name_can_become_an_option_or_a_second_word();
	refusals_are_told_apart_and_a_changed_key_is_never_unknown();
	return TEST_RESULT();
}

/*
 * An SFTP place in the file list: the Go prompt scheme and that nothing can be
 * changed or opened there. (The listing through a real child process is in
 * test_sftpplace_proc.)
 *
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/wf_efile_win.h"
#include "fs/wf_dir.h"
#include "fs/wf_sftpplace_win.h"
#include "path/wf_smartgo_path.h"
#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

static wchar_t *no_home(void *ctx)
{
	(void)ctx;
	return NULL;
}

static wchar_t *no_known(void *ctx, const wchar_t *alias)
{
	(void)ctx;
	(void)alias;
	return NULL;
}

static wchar_t *no_env(void *ctx, const wchar_t *name)
{
	(void)ctx;
	(void)name;
	return NULL;
}

static const WfSmartEnv env = {NULL, no_home, no_known, no_env};

static bool goes_to(const wchar_t *text, const wchar_t *target)
{
	WfSmartResult r;
	bool ok;

	CHECK(WF_OK ==
	        wf_smart_resolve(
	                &env, L"C:\\Users\\dev", NULL, text, wcslen(text), &r));
	ok = WF_SG_OK == r.error && r.target && !wcscmp(r.target, target);
	wf_smart_result_free(&r);
	return ok;
}

static bool refused(const wchar_t *text)
{
	WfSmartResult r;
	bool ok;

	CHECK(WF_OK ==
	        wf_smart_resolve(
	                &env, L"C:\\Users\\dev", NULL, text, wcslen(text), &r));
	ok = WF_SG_SFTP_NAME == r.error && !r.target;
	wf_smart_result_free(&r);
	return ok;
}

static void the_go_prompt_knows_the_scheme(void)
{
	CHECK(goes_to(L"sftp:box", L"\\\\sftp.invalid\\box"));
	CHECK(goes_to(L"SFTP:box/var/log", L"\\\\sftp.invalid\\box\\var\\log"));
	CHECK(goes_to(L"sftp:Box.example-1/Home/Me",
	        L"\\\\sftp.invalid\\Box.example-1\\Home\\Me"));
	CHECK(refused(L"sftp:"));
	CHECK(refused(L"sftp:/etc"));
	CHECK(refused(L"sftp:-oProxyCommand=x"));
	CHECK(refused(L"sftp:a b"));
	CHECK(refused(L"sftp:a;b"));
	CHECK(refused(L"sftp:a:b"));
}

static void expect_read_only(WfApp *a, const char *what)
{
	if (strcmp(a->msg, wf_msg(MSG_SFTP_READONLY))) {
		printf("  %s: message [%s]\n", what, a->msg);
	}
	CHECK(!strcmp(a->msg, wf_msg(MSG_SFTP_READONLY)));
	a->msg[0] = 0;
}

static void nothing_can_be_changed_or_opened_there(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfEvent del = {WF_EV_KEY, 0, WF_KEY_DELETE, 0};

	CHECK(WF_OK ==
	        wf_app_init(&a, 100, 30, fake_port(&fp), L"C:\\Users\\dev"));
	free(a.cwd);
	a.cwd = _wcsdup(L"\\\\sftp.invalid\\box\\home");
	CHECK(NULL != a.cwd);
	a.msg[0] = 0;
	wf_app_cmd_create(&a);
	expect_read_only(&a, "create");
	wf_app_cmd_create_kind(&a, true);
	expect_read_only(&a, "new file");
	wf_app_cmd_rename(&a);
	expect_read_only(&a, "rename");
	wf_app_cmd_recycle(&a, &del);
	expect_read_only(&a, "delete");
	wf_app_cmd_stage(&a, false);
	expect_read_only(&a, "copy");
	wf_app_cmd_stage(&a, true);
	expect_read_only(&a, "cut");
	wf_app_cmd_paste(&a);
	expect_read_only(&a, "paste");
	wf_app_open_selected(&a);
	expect_read_only(&a, "open");
	CHECK(&WF_MODE_NORMAL == a.mode);
	wf_app_destroy(&a);
}

int main(void)
{
	the_go_prompt_knows_the_scheme();
	nothing_can_be_changed_or_opened_there();
	return TEST_RESULT();
}

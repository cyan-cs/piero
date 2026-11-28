#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <io.h>
#include <stdbool.h>
#include <string.h>

#include "../tools/p0/p0_area.h"
#include "../tools/p0/p0_common.h"
#include "../tools/p0/p0_journal.h"
#include "../src/base/wf_macro.h"
#include "wf_test.h"

static void private_acl_at(const wchar_t *path, PSID user, bool protected_acl)
{
	HANDLE h = CreateFileW(path, READ_CONTROL,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	CHECK(h != INVALID_HANDLE_VALUE);
	if (h == INVALID_HANDLE_VALUE) {
		return;
	}
	PSECURITY_DESCRIPTOR sd = NULL;
	PACL acl = NULL;
	PSID owner = NULL;
	DWORD status = GetSecurityInfo(h, SE_FILE_OBJECT,
	        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
	        NULL, &acl, NULL, &sd);
	CHECK(status == ERROR_SUCCESS);
	if (status == ERROR_SUCCESS) {
		SECURITY_DESCRIPTOR_CONTROL control;
		DWORD revision;
		CHECK(GetSecurityDescriptorControl(sd, &control, &revision));
		CHECK(!!(control & SE_DACL_PROTECTED) == protected_acl);
		CHECK(acl && acl->AceCount == 1);
		if (protected_acl) {
			CHECK(owner && EqualSid(owner, user));
		}
		ACCESS_ALLOWED_ACE *ace = NULL;
		if (acl && acl->AceCount == 1 &&
		        GetAce(acl, 0, (void **)&ace)) {
			CHECK(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
			CHECK(EqualSid(&ace->SidStart, user));
			CHECK((ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS);
		}
	}
	LocalFree(sd);
	CloseHandle(h);
}

static void content_is(const wchar_t *path, const char *want)
{
	HANDLE h = CreateFileW(path, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, 0, NULL);
	char got[32] = {0};
	DWORD n = 0;
	CHECK(h != INVALID_HANDLE_VALUE);
	if (h == INVALID_HANDLE_VALUE) {
		return;
	}
	CHECK(ReadFile(h, got, sizeof got - 1, &n, NULL));
	CHECK(n == strlen(want) && !memcmp(got, want, n));
	CloseHandle(h);
}

int main(void)
{
	wchar_t temp[MAX_PATH], base[MAX_PATH * 2], file[MAX_PATH * 2],
	        alias[MAX_PATH * 2], child[MAX_PATH * 2], report[512];
	PSECURITY_DESCRIPTOR broad = NULL;
	HANDLE token = NULL;
	TOKEN_USER *user = NULL;
	DWORD bytes = 0;
	P0Area *area = NULL;
	bool created = false;
	CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
	if (!token) {
		goto done;
	}
	GetTokenInformation(token, TokenUser, NULL, 0, &bytes);
	user = malloc(bytes);
	CHECK(user);
	if (!user) {
		goto done;
	}
	if (!GetTokenInformation(token, TokenUser, user, bytes, &bytes)) {
		CHECK_FAIL("TokenUser unavailable");
		goto done;
	}
	CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(
	        L"D:P(A;OICI;FA;;;WD)", SDDL_REVISION_1, &broad, NULL));
	if (!broad) {
		goto done;
	}
	SECURITY_ATTRIBUTES sa = {sizeof sa, broad, FALSE};
	DWORD temp_n = GetTempPathW(WF_ARRAY_LEN(temp), temp);
	if (!temp_n || temp_n >= WF_ARRAY_LEN(temp)) {
		CHECK_FAIL("temporary path unavailable");
		goto done;
	}
	swprintf(base, WF_ARRAY_LEN(base), L"%lspiero-acl-%lu", temp,
	        GetCurrentProcessId());
	created = !!CreateDirectoryW(base, &sa);
	CHECK(created);
	if (!created) {
		goto done;
	}
	swprintf(file, WF_ARRAY_LEN(file), L"%ls\\report.txt", base);
	FILE *f = p0_fopen_w(file);
	CHECK(f);
	if (!f) {
		goto done;
	}
	CHECK(fputs("one", f) >= 0 && !fflush(f));
	private_acl_at(file, user->User.Sid, true);
	content_is(file, "one"); /* readers can observe a live writer */
	HANDLE writer = CreateFileW(file, GENERIC_WRITE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, 0, NULL);
	CHECK(writer == INVALID_HANDLE_VALUE &&
	        GetLastError() == ERROR_SHARING_VIOLATION);
	if (writer != INVALID_HANDLE_VALUE) {
		CloseHandle(writer);
	}
	CHECK(!fclose(f));
	f = p0_fappend_w(file);
	CHECK(f);
	if (f) {
		CHECK(fputs("two", f) >= 0);
		CHECK(!fclose(f));
	}
	content_is(file, "onetwo");
	swprintf(alias, WF_ARRAY_LEN(alias), L"%ls\\alias.txt", base);
	CHECK(CreateHardLinkW(alias, file, NULL));
	f = p0_fopen_w(alias);
	CHECK(!f);
	if (f) {
		fclose(f);
	}
	content_is(file, "onetwo");
	CHECK(DeleteFileW(alias));
	CHECK(DeleteFileW(file));
	/* An existing deliberately broad file retains its DACL, even though new
	 * files under this same parent must not inherit that broad access. */
	HANDLE h = CreateFileW(
	        file, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW, 0, NULL);
	CHECK(h != INVALID_HANDLE_VALUE);
	if (h != INVALID_HANDLE_VALUE) {
		CloseHandle(h);
	}
	f = p0_fopen_w(file);
	CHECK(f);
	if (f) {
		fclose(f);
	}
	PSECURITY_DESCRIPTOR sd = NULL;
	PACL acl = NULL;
	CHECK(GetNamedSecurityInfoW(file, SE_FILE_OBJECT,
	              DACL_SECURITY_INFORMATION, NULL, NULL, &acl, NULL,
	              &sd) == ERROR_SUCCESS);
	ACCESS_ALLOWED_ACE *ace = NULL;
	BYTE world[SECURITY_MAX_SID_SIZE];
	DWORD world_n = sizeof world;
	CHECK(CreateWellKnownSid(WinWorldSid, NULL, world, &world_n));
	CHECK(acl && acl->AceCount == 1 && GetAce(acl, 0, (void **)&ace));
	if (ace) {
		CHECK(EqualSid(&ace->SidStart, world));
	}
	LocalFree(sd);
	CHECK(DeleteFileW(file));
	P0Journal journal;
	int journal_opened = p0j_open(&journal, file);
	CHECK(journal_opened);
	if (journal_opened) {
		P0Journal existing;
		private_acl_at(file, user->User.Sid, true);
		CHECK(!p0j_open(&existing, file));
		p0j_close(&existing);
		CHECK(journal.written == 8);
		p0j_close(&journal);
	}
	CHECK(DeleteFileW(file));
	area = p0_area_create(base, "permissions");
	CHECK(area);
	if (area) {
		private_acl_at(p0_area_root(area), user->User.Sid, true);
		CHECK(p0_area_path(
		        area, L"child.bin", child, WF_ARRAY_LEN(child)));
		h = CreateFileW(
		        child, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
		CHECK(h != INVALID_HANDLE_VALUE);
		if (h != INVALID_HANDLE_VALUE) {
			CloseHandle(h);
		}
		private_acl_at(child, user->User.Sid, false);
		CHECK(p0_area_track(area, L"child.bin", 0));
		CHECK(p0_area_destroy(area, report, WF_ARRAY_LEN(report)) == 0);
		area = NULL;
	}
done:
	if (area) {
		p0_area_destroy(area, report, WF_ARRAY_LEN(report));
	}
	if (created) {
		DeleteFileW(file);
		CHECK(RemoveDirectoryW(base));
	}
	LocalFree(broad);
	free(user);
	if (token) {
		CloseHandle(token);
	}
	return TEST_RESULT();
}

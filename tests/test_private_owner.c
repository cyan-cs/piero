#include <windows.h>
#include <aclapi.h>

#include "../tools/p0/p0_lab.h"
#include "wf_test.h"

static BOOL checked_create(LPCWSTR path, LPSECURITY_ATTRIBUTES security)
{
	PSID owner = NULL;
	BOOL defaulted = TRUE;

	CHECK(security && security->lpSecurityDescriptor);
	if (security && security->lpSecurityDescriptor) {
		CHECK(GetSecurityDescriptorOwner(
		        security->lpSecurityDescriptor, &owner, &defaulted));
		CHECK(owner && IsValidSid(owner) && !defaulted);
	}
	return CreateDirectoryW(path, security);
}

#define CreateDirectoryW checked_create
#include "../src/path/cwd_neutralize.c"
#undef CreateDirectoryW

int main(void)
{
	P0Lab lab;
	wchar_t path[1024];
	HANDLE pin = INVALID_HANDLE_VALUE, token = NULL;
	DWORD error, bytes = 0;
	TOKEN_USER *user = NULL;
	TOKEN_OWNER *token_owner = NULL;
	PSID owner = NULL;
	PACL dacl = NULL;
	PSECURITY_DESCRIPTOR sd = NULL;

	CHECK(p0_lab_open(&lab, "private-owner", 1));
	if (!lab.area) {
		return 1;
	}
	CHECK(p0_lab_path(&lab, L"private", path, 1024));
	CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
	GetTokenInformation(token, TokenUser, NULL, 0, &bytes);
	user = malloc(bytes);
	CHECK(user &&
	        GetTokenInformation(token, TokenUser, user, bytes, &bytes));
	GetTokenInformation(token, TokenOwner, NULL, 0, &bytes);
	token_owner = malloc(bytes);
	CHECK(token_owner &&
	        GetTokenInformation(
	                token, TokenOwner, token_owner, bytes, &bytes));
	CHECK(wf_private_directory_open(path, true, &pin, &error));
	CHECK(ERROR_SUCCESS ==
	        GetSecurityInfo(pin, SE_FILE_OBJECT,
	                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
	                &owner, NULL, &dacl, NULL, &sd));
	if (user && token_owner && owner && dacl) {
		printf("private-owner: token_owner_is_user=%d owner_is_user=%d "
		       "aces=%u\n",
		        EqualSid(user->User.Sid, token_owner->Owner),
		        EqualSid(user->User.Sid, owner),
		        (unsigned)dacl->AceCount);
		CHECK(EqualSid(user->User.Sid, owner));
		CHECK(private_acl(pin, user->User.Sid));
	}
	if (sd) {
		LocalFree(sd);
	}
	if (INVALID_HANDLE_VALUE != pin) {
		CloseHandle(pin);
	}
	if (token) {
		CloseHandle(token);
	}
	free(token_owner);
	free(user);
	CHECK(p0_area_track(lab.area, L"private", 1));
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}

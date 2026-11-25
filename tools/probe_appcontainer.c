/* Diagnostic-only: inspect the effective profile and exercise a unique probe
 * profile only with --create. Never delete or repair the product's profile. */
#include <windows.h>
#include <userenv.h>
#include <shlobj.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static int private_output;

static void path_status(const wchar_t *label, const wchar_t *path)
{
	DWORD a = GetFileAttributesW(path);
	DWORD error = a == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
	wprintf(L"%ls attributes=0x%08lx error=%lu\n", label, a, error);
	if (private_output) {
		wprintf(L"private_%ls=%ls\n", label, path);
	}
}

static TOKEN_USER *token_user(HANDLE token)
{
	DWORD bytes = 0;
	(void)GetTokenInformation(token, TokenUser, NULL, 0, &bytes);
	if (!bytes || bytes > 65536) {
		return NULL;
	}
	TOKEN_USER *user = malloc(bytes);
	if (user &&
	        !GetTokenInformation(token, TokenUser, user, bytes, &bytes)) {
		free(user);
		return NULL;
	}
	return user;
}

static void effective_identity(HANDLE process)
{
	HANDLE thread = NULL;
	BOOL has_thread =
	        OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &thread);
	DWORD thread_error = has_thread ? 0 : GetLastError();
	printf("ThreadToken present=%d error=%lu\n", (int)has_thread,
	        thread_error);
	if (!has_thread && thread_error != ERROR_NO_TOKEN) {
		return; /* Do not mistake an unreadable thread identity for process identity. */
	}
	HANDLE effective = has_thread ? thread : process;
	/* Read-only: an on-disk UsrClass.dat is not proof that this user's
	 * classes hive is loaded and available to profile registration. */
	HKEY classes_root = NULL;
	LSTATUS classes_status =
	        RegOpenUserClassesRoot(effective, 0, KEY_READ, &classes_root);
	printf("EffectiveClassesRoot open=%ld\n", classes_status);
	if (classes_root) {
		RegCloseKey(classes_root);
	}
	TOKEN_USER *user = token_user(effective), *owner = token_user(process);
	printf("EffectiveUser readable=%d matches_process=%d\n", user != NULL,
	        user && owner ? (int)EqualSid(user->User.Sid, owner->User.Sid)
	                      : 0);
	DWORD value = 0, bytes = sizeof value;
	BOOL ok = GetTokenInformation(
	        effective, TokenSessionId, &value, bytes, &bytes);
	printf("EffectiveSession readable=%d id=%lu\n", (int)ok, value);
	value = 0;
	bytes = sizeof value;
	ok = GetTokenInformation(
	        effective, TokenElevationType, &value, bytes, &bytes);
	printf("ElevationType readable=%d value=%lu\n", (int)ok, value);
	wchar_t profile[32768], env_profile[32768];
	bytes = 32768;
	ok = GetUserProfileDirectoryW(effective, profile, &bytes);
	printf("UserProfileDirectory ok=%d error=%lu\n", (int)ok,
	        ok ? 0 : GetLastError());
	if (ok) {
		path_status(L"EffectiveProfile", profile);
		DWORD n = GetEnvironmentVariableW(
		        L"USERPROFILE", env_profile, 32768);
		printf("EnvironmentProfile matches_effective=%d\n",
		        n && n < 32768 &&
		                CompareStringOrdinal(profile, -1, env_profile,
		                        -1, TRUE) == CSTR_EQUAL);
		DWORD capacity = 32768;
		BOOL default_ok =
		        GetDefaultUserProfileDirectoryW(env_profile, &capacity);
		printf("EffectiveProfile matches_default_template=%d "
		       "default_query_ok=%d\n",
		        default_ok &&
		                CompareStringOrdinal(profile, -1, env_profile,
		                        -1, TRUE) == CSTR_EQUAL,
		        (int)default_ok);
	}
	LPWSTR sid = NULL;
	if (user && ConvertSidToStringSidW(user->User.Sid, &sid)) {
		wchar_t classes[256];
		if (private_output) {
			wprintf(L"private_effective_sid=%ls\n", sid);
		}
		int n = swprintf(classes, 256, L"%ls_Classes", sid);
		if (n > 0 && n < 256) {
			HKEY key = NULL;
			LSTATUS status = RegOpenKeyExW(
			        HKEY_USERS, classes, 0, KEY_READ, &key);
			printf("EffectiveUserClasses open=%ld\n", status);
			if (key) {
				RegCloseKey(key);
			}
		}
		wchar_t profile_key[384];
		n = swprintf(profile_key, 384,
		        L"Software\\Microsoft\\Windows "
		        L"NT\\CurrentVersion\\ProfileList\\%ls",
		        sid);
		if (n > 0 && n < 384) {
			HKEY key = NULL;
			LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
			        profile_key, 0, KEY_READ, &key);
			printf("EffectiveProfileList open=%ld\n", status);
			if (key) {
				RegCloseKey(key);
			}
		}
		LocalFree(sid);
	}
	free(user);
	free(owner);
	if (thread) {
		CloseHandle(thread);
	}
}

int wmain(int argc, wchar_t **argv)
{
	int create = 0;
	for (int i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--create")) {
			create = 1;
		} else if (!wcscmp(argv[i], L"--private")) {
			private_output = 1;
		} else {
			fputs("usage: probe_appcontainer [--create] "
			      "[--private]\n",
			        stderr);
			return 2;
		}
	}
	wchar_t env[32768], name[64];
	PWSTR known = NULL;
	HANDLE token = NULL;
	DWORD size, value = 0;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		SetLastError(ERROR_SUCCESS);
		BOOL restricted = IsTokenRestricted(token);
		DWORD restricted_error = restricted ? 0 : GetLastError();
		printf("TokenRestricted=%d query_error=%lu\n", (int)restricted,
		        restricted_error);
		BOOL ok = GetTokenInformation(token, TokenIsAppContainer,
		        &value, sizeof value, &size);
		printf("TokenIsAppContainer=%lu query_ok=%d\n", value, (int)ok);
		effective_identity(token);
		CloseHandle(token);
	}
	DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", env, 32768);
	if (n && n < 32768) {
		path_status(L"LOCALAPPDATA", env);
	}
	HRESULT hr =
	        SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &known);
	printf("KnownFolder HRESULT=0x%08lx\n", (unsigned long)hr);
	if (SUCCEEDED(hr) && known) {
		path_status(L"KnownLocalAppData", known);
	}
	CoTaskMemFree(known);
	HKEY mappings = NULL;
	LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER,
	        L"Software\\Classes\\Local Settings\\Software\\Microsoft\\"
	        L"Windows\\CurrentVersion\\AppContainer\\Mappings",
	        0, KEY_READ, &mappings);
	printf("ProfileMappings open=%ld\n", status);
	if (mappings) {
		RegCloseKey(mappings);
	}
	PSID sid = NULL;
	hr = DeriveAppContainerSidFromAppContainerName(L"piero.decoder", &sid);
	printf("DeriveProductSid HRESULT=0x%08lx (does not prove profile "
	       "exists)\n",
	        (unsigned long)hr);
	if (sid) {
		FreeSid(sid);
		sid = NULL;
	}
	if (!create) {
		return 0;
	}
	swprintf(name, 64, L"piero.probe.%lu.%llu", GetCurrentProcessId(),
	        GetTickCount64());
	wprintf(L"Create name=%ls capabilities=NULL count=0\n", name);
	hr = CreateAppContainerProfile(
	        name, name, L"piero temporary isolation probe", NULL, 0, &sid);
	printf("Create HRESULT=0x%08lx\n", (unsigned long)hr);
	if (sid) {
		FreeSid(sid);
	}
	if (FAILED(hr)) {
		return 1;
	}
	hr = DeleteAppContainerProfile(name);
	printf("DeleteCreatedProbe HRESULT=0x%08lx\n", (unsigned long)hr);
	return FAILED(hr) ? 1 : 0;
}

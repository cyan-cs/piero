#include <windows.h>

#include "wf_smartgo_env_win.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>
#include "../base/wf_mem.h"

static wchar_t *known_folder(REFKNOWNFOLDERID id)
{
	PWSTR path = NULL;
	wchar_t *copy = NULL;

	if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, NULL, &path)) &&
	        path) {
		copy = _wcsdup(path);
	}
	CoTaskMemFree(path);
	return copy;
}

static wchar_t *home(void *ctx)
{
	(void)ctx;
	return known_folder(&FOLDERID_Profile);
}

static wchar_t *known(void *ctx, const wchar_t *alias)
{
	(void)ctx;
	if (!wcscmp(alias, L"desktop")) {
		return known_folder(&FOLDERID_Desktop);
	}
	if (!wcscmp(alias, L"downloads")) {
		return known_folder(&FOLDERID_Downloads);
	}
	if (!wcscmp(alias, L"documents")) {
		return known_folder(&FOLDERID_Documents);
	}
	if (!wcscmp(alias, L"pictures")) {
		return known_folder(&FOLDERID_Pictures);
	}
	if (!wcscmp(alias, L"music")) {
		return known_folder(&FOLDERID_Music);
	}
	if (!wcscmp(alias, L"videos")) {
		return known_folder(&FOLDERID_Videos);
	}
	if (!wcscmp(alias, L"appdata")) {
		return known_folder(&FOLDERID_RoamingAppData);
	}
	return NULL;
}

static wchar_t *get_env(void *ctx, const wchar_t *name)
{
	DWORD need = GetEnvironmentVariableW(name, NULL, 0), got;
	wchar_t *value;

	(void)ctx;
	if (!need) {
		return NULL; /* not defined (or defined empty: treated alike) */
	}
	value = wf_malloc_array((size_t)need, sizeof *value);
	if (!value) {
		return NULL;
	}
	got = GetEnvironmentVariableW(name, value, need);
	if (!got || got >= need) {
		free(value);
		return NULL;
	}
	return value;
}

WfSmartEnv wf_smartgo_env_win(void)
{
	WfSmartEnv e = {NULL, home, known, get_env};

	return e;
}

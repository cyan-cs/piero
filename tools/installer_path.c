/* Per-user installer helper. Tests do not invoke it against the host registry.
 * Rechecks the append snapshot before restoring. External registry writers can
 * still race the final read/write; the mutex coordinates only this helper.
 * No file deletion, elevation, machine PATH or config cleanup. */
#include <windows.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include "path_entry.h"

static const wchar_t ledger[] = L"Software\\cyan-cs\\piero\\InstallerPath";
static wchar_t *read_string(
        HKEY key, const wchar_t *name, DWORD *kind, bool *absent)
{
	DWORD bytes = 0, type = 0;
	LSTATUS st = RegQueryValueExW(key, name, NULL, &type, NULL, &bytes);
	if (absent) {
		*absent = st == ERROR_FILE_NOT_FOUND;
	}
	if (st == ERROR_FILE_NOT_FOUND && absent) {
		wchar_t *empty = calloc(1, sizeof *empty);
		*kind = REG_EXPAND_SZ;
		return empty;
	}
	if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
	        bytes < sizeof(wchar_t) || bytes > 65536 ||
	        bytes % sizeof(wchar_t)) {
		return NULL;
	}
	wchar_t *value = malloc(bytes);
	if (!value) {
		return NULL;
	}
	DWORD capacity = bytes;
	DWORD expected_type = type;
	if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)value,
	            &capacity) != ERROR_SUCCESS ||
	        type != expected_type || capacity != bytes ||
	        value[bytes / sizeof(wchar_t) - 1] != 0 ||
	        (wcslen(value) + 1) * sizeof(wchar_t) != bytes) {
		free(value);
		return NULL;
	}
	*kind = type;
	return value;
}
static bool put(HKEY key, const wchar_t *name, const wchar_t *value, DWORD kind)
{
	return RegSetValueExW(key, name, 0, kind, (const BYTE *)value,
	               (DWORD)((wcslen(value) + 1) * sizeof *value)) ==
	        ERROR_SUCCESS;
}
int wmain(int argc, wchar_t **argv)
{
	static wchar_t directory[32768];
	HKEY env = NULL, record = NULL;
	int result = 1;
	wchar_t *current = NULL, *before = NULL, *snapshot = NULL,
	        *owner = NULL, *again = NULL;
	DWORD kind = 0, record_kind = 0, owner_kind = 0, again_kind = 0,
	      existed = 0, bytes = sizeof existed;
	bool absent = false, add;
	HANDLE mutex = NULL;
	if (argc != 2 ||
	        (wcscmp(argv[1], L"--add") && wcscmp(argv[1], L"--remove"))) {
		return 2;
	}
	add = !wcscmp(argv[1], L"--add");
	DWORD n = GetModuleFileNameW(NULL, directory, 32768);
	if (!n || n >= 32768) {
		return 1;
	}
	wchar_t *slash = wcsrchr(directory, '\\');
	if (!slash) {
		return 1;
	}
	if (slash > directory && slash[-1] == L':') {
		slash[1] =
		        0; /* A drive root must remain absolute, including \\?\ paths. */
	} else {
		*slash = 0;
	}
	mutex = CreateMutexW(NULL, FALSE, L"Local\\piero.installer.user-path");
	if (!mutex) {
		return 1;
	}
	DWORD wait = WaitForSingleObject(mutex, 5000);
	if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
		CloseHandle(mutex);
		return 1;
	}
	if (!add &&
	        RegOpenKeyExW(HKEY_CURRENT_USER, ledger, 0,
	                KEY_QUERY_VALUE | KEY_SET_VALUE,
	                &record) != ERROR_SUCCESS) {
		result = 0;
		goto done;
	}
	if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Environment", 0, NULL, 0,
	            KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &env,
	            NULL) != ERROR_SUCCESS) {
		goto done;
	}
	current = read_string(env, L"Path", &kind, &absent);
	if (!current) {
		goto done;
	}
	if (add) {
		if (piero_path_contains(current, directory)) {
			result = 0;
			goto done;
		}
		snapshot = piero_path_append(current, directory);
		if (!snapshot ||
		        RegCreateKeyExW(HKEY_CURRENT_USER, ledger, 0, NULL, 0,
		                KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &record,
		                NULL) != ERROR_SUCCESS) {
			goto done;
		}
		existed = !absent;
		if (!put(record, L"Before", current, kind) ||
		        !put(record, L"Snapshot", snapshot, kind) ||
		        !put(record, L"Directory", directory, REG_SZ) ||
		        RegSetValueExW(record, L"Existed", 0, REG_DWORD,
		                (BYTE *)&existed,
		                sizeof existed) != ERROR_SUCCESS ||
		        RegFlushKey(record) != ERROR_SUCCESS) {
			goto done;
		}
		again = read_string(env, L"Path", &again_kind, &absent);
		if (!again || again_kind != kind || wcscmp(again, current)) {
			result = 3;
			goto done;
		}
		if (!put(env, L"Path", snapshot, kind)) {
			goto done;
		}
	} else {
		before = read_string(record, L"Before", &record_kind, NULL);
		snapshot = read_string(record, L"Snapshot", &owner_kind, NULL);
		owner = read_string(record, L"Directory", &again_kind, NULL);
		DWORD type = 0;
		if (!before || !snapshot || !owner || kind != record_kind ||
		        kind != owner_kind || wcscmp(owner, directory) ||
		        RegQueryValueExW(record, L"Existed", NULL, &type,
		                (BYTE *)&existed, &bytes) != ERROR_SUCCESS ||
		        type != REG_DWORD || bytes != sizeof existed ||
		        existed > 1 ||
		        !piero_path_can_restore(
		                current, snapshot, before, directory)) {
			result = 3;
			goto done;
		}
		again = read_string(env, L"Path", &again_kind, &absent);
		if (!again || again_kind != kind || wcscmp(again, current)) {
			result = 3;
			goto done;
		}
		if (!existed && *before) {
			goto done;
		}
		if (existed ? !put(env, L"Path", before, kind)
		            : RegDeleteValueW(env, L"Path") != ERROR_SUCCESS) {
			goto done;
		}
		/* Keep a failed/changed receipt for review; retire only these known values. */
		RegDeleteValueW(record, L"Before");
		RegDeleteValueW(record, L"Snapshot");
		RegDeleteValueW(record, L"Directory");
		RegDeleteValueW(record, L"Existed");
	}
	result = 0;
	SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
	        (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 2000, NULL);
done:
	free(current);
	free(before);
	free(snapshot);
	free(owner);
	free(again);
	if (record) {
		RegCloseKey(record);
	}
	if (env) {
		RegCloseKey(env);
	}
	ReleaseMutex(mutex);
	CloseHandle(mutex);
	return result;
}

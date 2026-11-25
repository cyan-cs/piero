#ifndef WF_TEST_ASSOC_H
#define WF_TEST_ASSOC_H

/* Test-only HKCU associations. Never adopt, overwrite or delete an existing
 * key. Ownership survives a failed rollback so callers can report/retry it. */
#include <windows.h>
#include <bcrypt.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

typedef struct
{
	wchar_t ext[64], progid[96];
	bool ext_owned, progid_owned;
	LSTATUS error, cleanup_error;
} WfTestAssoc;

static inline LSTATUS wf_test_assoc_cleanup(WfTestAssoc *a)
{
	wchar_t path[200];
	LSTATUS result = ERROR_SUCCESS, st;
	bool *owned[2] = {&a->ext_owned, &a->progid_owned};
	const wchar_t *names[2] = {a->ext, a->progid};
	for (unsigned i = 0; i < 2; i++) {
		if (!*owned[i]) {
			continue;
		}
		swprintf(path, 200, L"Software\\Classes\\%ls", names[i]);
		st = RegDeleteTreeW(HKEY_CURRENT_USER, path);
		if (ERROR_SUCCESS == st || ERROR_FILE_NOT_FOUND == st) {
			*owned[i] = false;
		} else {
			result = st;
		}
	}
	a->cleanup_error = result;
	return result;
}

static inline LSTATUS wf_test_assoc_name(WfTestAssoc *a)
{
	uint64_t nonce;
	memset(a, 0, sizeof *a);
	if (BCryptGenRandom(NULL, (PUCHAR)&nonce, sizeof nonce,
	            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
		return ERROR_GEN_FAILURE;
	}
	swprintf(a->ext, 64, L".wfopen%lu_%016llx", GetCurrentProcessId(),
	        (unsigned long long)nonce);
	swprintf(a->progid, 96, L"wfopen%lu_%016llx.file",
	        GetCurrentProcessId(), (unsigned long long)nonce);
	return ERROR_SUCCESS;
}

static inline LSTATUS wf_test_assoc_register(WfTestAssoc *a, const wchar_t *exe)
{
	wchar_t path[240], command[1200];
	HKEY key = NULL;
	DWORD disposition;
	LSTATUS st;
	if (!a->ext[0] || !a->progid[0] || wcslen(exe) > 1024 || a->ext_owned ||
	        a->progid_owned) {
		return ERROR_INVALID_PARAMETER;
	}
	swprintf(command, 1200, L"\"%ls\" \"%%1\"", exe);
	for (unsigned i = 0; i < 3; i++) {
		const wchar_t *value = 0 == i ? a->progid : command;
		swprintf(path, 240, L"Software\\Classes\\%ls%ls",
		        0 == i ? a->ext : a->progid,
		        2 == i ? L"\\shell\\open\\command" : L"");
		st = RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0,
		        KEY_SET_VALUE, NULL, &key, &disposition);
		if (ERROR_SUCCESS != st) {
			goto fail;
		}
		if (REG_CREATED_NEW_KEY != disposition) {
			RegCloseKey(key);
			st = ERROR_ALREADY_EXISTS;
			goto fail;
		}
		if (0 == i) {
			a->ext_owned = true;
		} else if (1 == i) {
			a->progid_owned = true;
		}
		/* The ProgID root intentionally has no default command value. */
		st = 1 == i
		        ? ERROR_SUCCESS
		        : RegSetValueExW(key, NULL, 0, REG_SZ,
		                  (const BYTE *)value,
		                  (DWORD)((wcslen(value) + 1) * sizeof *value));
		RegCloseKey(key);
		if (ERROR_SUCCESS != st) {
			goto fail;
		}
	}
	a->error = ERROR_SUCCESS;
	return ERROR_SUCCESS;
fail:
	a->error = st;
	(void)wf_test_assoc_cleanup(a);
	return st;
}

#endif

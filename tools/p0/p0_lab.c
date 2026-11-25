#include <windows.h>

#include "p0_lab.h"

#include <bcrypt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "p0_common.h"

int p0_lab_path(
        const P0Lab *lab, const wchar_t *relative, wchar_t *out, size_t cap)
{
	/* Validate every component through the existing scratch authority
	 * first. */
	wchar_t validated[32768];
	if (!p0_area_path(lab->area, relative, validated, 32768)) {
		return 0;
	}
	return (size_t)swprintf(out, cap, L"%ls%ls%ls", lab->root,
	               L'\\' == lab->root[wcslen(lab->root) - 1] ? L"" : L"\\",
	               relative) < cap;
}

/*
 * A drive letter and the machine-wide mutex outlive the process that took
 * them. A test that leaves through exit() (the mutation harness stops a
 * mutant at its first failing check) never reaches p0_lab_close, and every
 * leaked letter is gone for all later tests: after 23 of them none can open a
 * lab. So what must be given back is kept here by value, not by pointer (the
 * caller's P0Lab may be gone), and handed back by an atexit handler.
 * A crash still leaks; that is not made better or worse by this.
 */
#define LAB_LEAK_MAX 4u
static struct
{
	bool used;
	wchar_t drive[3], target[1024];
	HANDLE mutex, pin;
	bool locked;
} lab_leak[LAB_LEAK_MAX];
static bool lab_atexit_registered;

static void lab_give_back_at_exit(void)
{
	for (size_t i = 0; i < LAB_LEAK_MAX; i++) {
		if (!lab_leak[i].used) {
			continue;
		}
		if (lab_leak[i].pin &&
		        INVALID_HANDLE_VALUE != lab_leak[i].pin) {
			CloseHandle(lab_leak[i].pin);
		}
		DefineDosDeviceW(DDD_REMOVE_DEFINITION |
		                DDD_EXACT_MATCH_ON_REMOVE |
		                DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM,
		        lab_leak[i].drive, lab_leak[i].target);
		if (lab_leak[i].locked) {
			ReleaseMutex(lab_leak[i].mutex);
		}
		lab_leak[i].used = false;
	}
}

static void lab_note_mapped(const P0Lab *lab)
{
	for (size_t i = 0; i < LAB_LEAK_MAX; i++) {
		if (lab_leak[i].used) {
			continue;
		}
		lab_leak[i].used = true;
		wcscpy_s(lab_leak[i].drive, 3, lab->drive);
		wcscpy_s(lab_leak[i].target, 1024, lab->target);
		lab_leak[i].mutex = lab->mutex;
		lab_leak[i].pin = lab->pin;
		lab_leak[i].locked = 0 != lab->locked;
		if (!lab_atexit_registered) {
			lab_atexit_registered =
			        0 == atexit(lab_give_back_at_exit);
		}
		return;
	}
}

static void lab_note_closed(const P0Lab *lab)
{
	for (size_t i = 0; i < LAB_LEAK_MAX; i++) {
		if (lab_leak[i].used &&
		        !wcscmp(lab_leak[i].drive, lab->drive) &&
		        !wcscmp(lab_leak[i].target, lab->target)) {
			lab_leak[i].used = false;
		}
	}
}

int p0_lab_open(P0Lab *lab, const char *label, int use_drive)
{
	wchar_t base[1024], observed[1024];
	char id[80];
	BYTE nonce[8];
	FILE_ID_INFO actual_id, alias_id;
	HANDLE actual = INVALID_HANDLE_VALUE;
	DWORD wait_result;
	int letter;
	memset(lab, 0, sizeof *lab);
	lab->pin = INVALID_HANDLE_VALUE;
	{
		DWORD units = GetModuleFileNameW(NULL, base, 1024);
		if (!units) {
			goto fail;
		}
		if (units >= 1024) {
			SetLastError(ERROR_FILENAME_EXCED_RANGE);
			goto fail;
		}
	}
	*wcsrchr(base, L'\\') = 0;
	wcscat_s(base, 1024, L"\\preflight-scratch");
	if (!p0_mkdirs(base)) {
		goto fail;
	}
	if (BCryptGenRandom(NULL, nonce, sizeof nonce,
	            BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
		SetLastError(ERROR_GEN_FAILURE);
		goto fail;
	}
	snprintf(id, sizeof id, "%s-%lu-%02x%02x%02x%02x%02x%02x%02x%02x",
	        label, GetCurrentProcessId(), nonce[0], nonce[1], nonce[2],
	        nonce[3], nonce[4], nonce[5], nonce[6], nonce[7]);
	lab->area = p0_area_create(base, id);
	if (!lab->area) {
		goto fail;
	}
	wcscpy_s(lab->root, 1024, p0_area_root(lab->area));
	if (!use_drive) {
		return 1;
	}
	lab->mutex =
	        CreateMutexW(NULL, FALSE, L"Local\\wfile-p0-isolated-drive");
	if (!lab->mutex) {
		goto fail;
	}
	wait_result = WaitForSingleObject(lab->mutex, 30000);
	if (WAIT_OBJECT_0 != wait_result && WAIT_ABANDONED != wait_result) {
		SetLastError(WAIT_TIMEOUT == wait_result ? ERROR_TIMEOUT
		                                         : GetLastError());
		goto fail;
	}
	lab->locked = 1;
	swprintf(lab->target, 1024, L"\\??\\%ls", p0_area_root(lab->area));
	for (letter = L'Z'; letter >= L'D'; letter--) {
		lab->drive[0] = (wchar_t)letter;
		lab->drive[1] = L':';
		lab->drive[2] = 0;
		if (QueryDosDeviceW(lab->drive, observed, 1024)) {
			continue;
		}
		if (ERROR_FILE_NOT_FOUND != GetLastError()) {
			goto fail;
		}
		if (!DefineDosDeviceW(
		            DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM,
		            lab->drive, lab->target)) {
			goto fail;
		}
		lab->mapped = 1;
		break;
	}
	if (!lab->mapped) {
		SetLastError(ERROR_NO_MORE_FILES);
		goto fail;
	}
	if (!QueryDosDeviceW(lab->drive, observed, 1024) ||
	        wcscmp(observed, lab->target)) {
		SetLastError(ERROR_INVALID_DATA);
		goto fail;
	}
	swprintf(lab->root, 1024, L"%ls\\", lab->drive);
	actual = CreateFileW(p0_area_root(lab->area), FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS, NULL);
	lab->pin = CreateFileW(lab->root, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == actual ||
	        INVALID_HANDLE_VALUE == lab->pin ||
	        !GetFileInformationByHandleEx(
	                actual, FileIdInfo, &actual_id, sizeof actual_id) ||
	        !GetFileInformationByHandleEx(
	                lab->pin, FileIdInfo, &alias_id, sizeof alias_id)) {
		goto fail;
	}
	if (actual_id.VolumeSerialNumber != alias_id.VolumeSerialNumber ||
	        memcmp(actual_id.FileId.Identifier, alias_id.FileId.Identifier,
	                16)) {
		SetLastError(ERROR_INVALID_DATA);
		goto fail;
	}
	CloseHandle(actual);
	lab_note_mapped(lab);
	return 1;
fail:
	lab->error = GetLastError();
	if (INVALID_HANDLE_VALUE != actual) {
		CloseHandle(actual);
	}
	/* Failure does not sweep an area whose setup/proof is incomplete. */
	p0_lab_close(lab, 1);
	return 0;
}

int p0_lab_close(P0Lab *lab, int keep)
{
	int left = 0;
	wchar_t report[1024];
	if (lab->mapped) {
		lab_note_closed(lab);
	}
	if (INVALID_HANDLE_VALUE != lab->pin) {
		CloseHandle(lab->pin);
		lab->pin = INVALID_HANDLE_VALUE;
	}
	if (lab->mapped) {
		if (!DefineDosDeviceW(DDD_REMOVE_DEFINITION |
		                    DDD_EXACT_MATCH_ON_REMOVE |
		                    DDD_RAW_TARGET_PATH |
		                    DDD_NO_BROADCAST_SYSTEM,
		            lab->drive, lab->target)) {
			lab->error = GetLastError();
			keep = 1;
			left++;
		}
		lab->mapped = 0;
	}
	if (lab->area) {
		if (keep) {
			p0_area_forget(lab->area);
		} else {
			int remaining =
			        p0_area_destroy(lab->area, report, 1024);
			if (remaining) {
				fwprintf(stderr, L"P0 cleanup: %ls\n", report);
			}
			left += remaining;
		}
		lab->area = NULL;
	}
	if (lab->locked) {
		ReleaseMutex(lab->mutex);
		lab->locked = 0;
	}
	if (lab->mutex) {
		CloseHandle(lab->mutex);
		lab->mutex = NULL;
	}
	return left;
}

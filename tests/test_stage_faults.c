#include <windows.h>
#include <bcrypt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "wf_test.h"

static LONG reads, fail_read, hashes;
static bool parallel_hash;
static HANDLE second_hash, first_hashed;

static BOOL stage_read(
        HANDLE file, LPVOID bytes, DWORD size, LPDWORD got, LPOVERLAPPED ov)
{
	if (InterlockedIncrement(&reads) == fail_read) {
		*got = 0;
		SetLastError(ERROR_CRC);
		return FALSE;
	}
	return ReadFile(file, bytes, size, got, ov);
}

/* Make two hash calls overlap after both reads, before the first consumes
 * its buffer. A shared static read buffer hashes the second file twice. */
static NTSTATUS stage_hash(
        BCRYPT_HASH_HANDLE hash, PUCHAR bytes, ULONG size, ULONG flags)
{
	LONG order = parallel_hash ? InterlockedIncrement(&hashes) : 0;
	NTSTATUS result;
	if (order == 1) {
		if (WAIT_OBJECT_0 != WaitForSingleObject(second_hash, 3000)) {
			return (NTSTATUS)0xc0000001L;
		}
	} else if (order == 2) {
		SetEvent(second_hash);
		if (WAIT_OBJECT_0 != WaitForSingleObject(first_hashed, 3000)) {
			return (NTSTATUS)0xc0000001L;
		}
	}
	result = BCryptHashData(hash, bytes, size, flags);
	if (order == 1) {
		SetEvent(first_hashed);
	}
	return result;
}

#define ReadFile stage_read
#define BCryptHashData stage_hash
#ifndef WF_STAGE_SOURCE
#	define WF_STAGE_SOURCE "../src/preview/wf_dechost_client_win.c"
#endif
#include WF_STAGE_SOURCE
#undef BCryptHashData
#undef ReadFile

static bool create_file(const wchar_t *path, const char *text)
{
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD put;
	bool ok;
	if (INVALID_HANDLE_VALUE == file) {
		return false;
	}
	ok = WriteFile(file, text, (DWORD)strlen(text), &put, NULL) &&
	        put == strlen(text);
	CloseHandle(file);
	return ok;
}

typedef struct
{
	const wchar_t *path;
	char hex[65];
	int ok;
} HashCall;

static DWORD WINAPI hash_thread(void *ctx)
{
	HashCall *call = ctx;
	call->ok = sha256_of_file(call->path, call->hex);
	return 0;
}

int main(void)
{
	P0Lab lab;
	wchar_t source[1024] = {0}, other[1024] = {0}, state[1024] = {0},
	        out[1024] = {0}, dir[1024], tmp[1100], sentinel[1024], whex[65];
	char hex[65], other_hex[65], saved[65] = {0};
	DWORD error = 0, got = 0, need;
	wchar_t *old_local = NULL;
	HANDLE file;
	int prerequisites;
	CHECK(p0_lab_open(&lab, "stage-faults", 1));
	if (!lab.area) {
		return 1;
	}
	CHECK(p0_lab_path(&lab, L"source.bin", source, 1024));
	CHECK(p0_lab_path(&lab, L"other.bin", other, 1024));
	CHECK(create_file(source, "first executable bytes"));
	CHECK(create_file(other, "other executable bytes"));
	CHECK(sha256_of_file(source, hex));
	CHECK(sha256_of_file(other, other_hex));
	if (wf_test_failures) {
		p0_lab_close(&lab, 1);
		return 1;
	}
	reads = 0;
	fail_read = 2; /* all bytes read, then an I/O error instead of EOF */
	CHECK(!sha256_of_file(source, saved));
	CHECK(ERROR_CRC == GetLastError());
	fail_read = 0;
	/* Concurrent hashing must retain each call's own read bytes. */
	second_hash = CreateEventW(NULL, TRUE, FALSE, NULL);
	first_hashed = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(second_hash && first_hashed);
	if (second_hash && first_hashed) {
		HashCall calls[2] = {{source, "", 0}, {other, "", 0}};
		HANDLE threads[2];
		parallel_hash = true;
		threads[0] =
		        CreateThread(NULL, 0, hash_thread, &calls[0], 0, NULL);
		threads[1] =
		        CreateThread(NULL, 0, hash_thread, &calls[1], 0, NULL);
		CHECK(threads[0] && threads[1]);
		for (size_t i = 0; i < 2; i++) {
			if (threads[i]) {
				DWORD waited =
				        WaitForSingleObject(threads[i], 5000);
				CHECK(WAIT_OBJECT_0 == waited);
				if (WAIT_OBJECT_0 != waited) {
					/* Do not unwind stack arguments while a thread can
					 * still borrow them. Keep the failed fixture. */
					ExitProcess(1);
				}
				CloseHandle(threads[i]);
			}
		}
		parallel_hash = false;
		CHECK(calls[0].ok && !strcmp(hex, calls[0].hex));
		CHECK(calls[1].ok && !strcmp(other_hex, calls[1].hex));
	}
	if (second_hash) {
		CloseHandle(second_hash);
	}
	if (first_hashed) {
		CloseHandle(first_hashed);
	}
	need = GetEnvironmentVariableW(L"LOCALAPPDATA", NULL, 0);
	if (need) {
		old_local = malloc((size_t)need * sizeof *old_local);
		CHECK(old_local != NULL);
		if (!old_local ||
		        !GetEnvironmentVariableW(
		                L"LOCALAPPDATA", old_local, need)) {
			free(old_local);
			p0_lab_close(&lab, 1);
			return 1;
		}
	}
	prerequisites = wf_test_failures;
	CHECK(p0_lab_path(&lab, L"copy-state", state, 1024));
	CHECK(CreateDirectoryW(state, NULL));
	CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA", state));
	reads = 0;
	if (wf_test_failures != prerequisites) {
		goto done;
	}
	fail_read =
	        4; /* source hash succeeds; copy gets all bytes then fails */
	CHECK(WF_OK != wf_dechost_stage(source, out, 1024, &error));
	CHECK(ERROR_CRC == error);
	CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(out));
	fail_read = 0;
	/* A pre-existing hardlink at the temporary name must never be truncated. */
	prerequisites = wf_test_failures;
	CHECK(p0_lab_path(&lab, L"link-state", state, 1024));
	CHECK(CreateDirectoryW(state, NULL));
	CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA", state));
	for (size_t i = 0; i < 65; i++) {
		whex[i] = (wchar_t)hex[i];
	}
	swprintf(dir, 1024, L"%ls\\piero\\worker\\%ls", state, whex);
	CHECK(make_dirs(dir));
	swprintf(tmp, 1100, L"%ls\\piero.exe.tmp", dir);
	CHECK(p0_lab_path(&lab, L"sentinel.bin", sentinel, 1024));
	CHECK(create_file(sentinel, "KEEP THESE BYTES"));
	CHECK(CreateHardLinkW(tmp, sentinel, NULL));
	if (wf_test_failures != prerequisites) {
		goto done;
	}
	CHECK(WF_OK != wf_dechost_stage(source, out, 1024, &error));
	CHECK(ERROR_FILE_EXISTS == error || ERROR_ALREADY_EXISTS == error);
	{ /* a refused copy closes what it opened: the handles do not add up */
		DWORD before = 0, after = 0;

		GetProcessHandleCount(GetCurrentProcess(), &before);
		for (int i = 0; i < 100; i++) {
			CHECK(WF_OK !=
			        wf_dechost_stage(source, out, 1024, &error));
		}
		GetProcessHandleCount(GetCurrentProcess(), &after);
		CHECK(after <= before + 2);
	}
	file = CreateFileW(sentinel, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != file);
	memset(saved, 0, sizeof saved);
	if (INVALID_HANDLE_VALUE != file) {
		CHECK(ReadFile(file, saved, sizeof saved - 1, &got, NULL));
		CloseHandle(file);
	}
	CHECK(!strcmp(saved, "KEEP THESE BYTES"));
done:
	CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA", old_local));
	free(old_local);
	/* Keep the exact fixture, including uncommitted stage residue. */
	CHECK(0 == p0_lab_close(&lab, 1));
	return TEST_RESULT();
}

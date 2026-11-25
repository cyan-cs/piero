#include <windows.h>
#include <bcrypt.h>

#include <stdlib.h>
#include <string.h>

#include "wf_sha256_win.h"

#include "../fs/wf_weakid_win.h"
#include "wf_xfer.h"

struct WfSha
{
	BCRYPT_ALG_HANDLE alg;
	BCRYPT_HASH_HANDLE hash;
	UCHAR *object;
};

void wf_sha_free(WfSha *s)
{
	if (!s) {
		return;
	}
	if (s->hash) {
		BCryptDestroyHash(s->hash);
	}
	if (s->alg) {
		BCryptCloseAlgorithmProvider(s->alg, 0);
	}
	free(s->object);
	free(s);
}

WfSha *wf_sha_new(void)
{
	WfSha *s = calloc(1, sizeof *s);
	DWORD object_len = 0, got = 0;

	if (!s) {
		return NULL;
	}
	if (BCryptOpenAlgorithmProvider(
	            &s->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
	        BCryptGetProperty(s->alg, BCRYPT_OBJECT_LENGTH,
	                (PUCHAR)&object_len, sizeof object_len, &got, 0) < 0) {
		wf_sha_free(s);
		return NULL;
	}
	s->object = malloc(object_len);
	if (!s->object ||
	        BCryptCreateHash(s->alg, &s->hash, s->object, object_len, NULL,
	                0, 0) < 0) {
		wf_sha_free(s);
		return NULL;
	}
	return s;
}

bool wf_sha_update(WfSha *s, const void *bytes, size_t n)
{
	/* BCrypt's input is not declared const; it is only read */
	UCHAR *p = (UCHAR *)(uintptr_t)bytes;

	/* BCrypt takes a ULONG at a time */
	while (n) {
		ULONG take = n > 0x40000000u ? 0x40000000u : (ULONG)n;

		if (BCryptHashData(s->hash, (PUCHAR)p, take, 0) < 0) {
			return false;
		}
		p += take;
		n -= take;
	}
	return true;
}

bool wf_sha_finish(WfSha *s, uint8_t digest[32])
{
	bool ok = BCryptFinishHash(s->hash, digest, 32, 0) >= 0;

	wf_sha_free(s);
	return ok;
}

typedef struct
{
	uint64_t size, last_write;
} Fingerprint;

static bool fingerprint(HANDLE h, Fingerprint *f, DWORD *attributes)
{
	BY_HANDLE_FILE_INFORMATION bi;

	if (!GetFileInformationByHandle(h, &bi)) {
		return false;
	}
	f->size = ((uint64_t)bi.nFileSizeHigh << 32) | bi.nFileSizeLow;
	f->last_write = ((uint64_t)bi.ftLastWriteTime.dwHighDateTime << 32) |
	        bi.ftLastWriteTime.dwLowDateTime;
	if (attributes) {
		*attributes = bi.dwFileAttributes;
	}
	return true;
}

static void fail(WfShaResult *out, WfStatus st, DWORD error)
{
	out->st = st;
	out->error = error;
	memset(out->digest, 0, sizeof out->digest);
}

static WfStatus status_of(DWORD error)
{
	switch (error) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:    return WF_E_NOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION: return WF_E_ACCESS;
	default:                      return WF_E_OS;
	}
}

/* `limit` 0: the whole file; else the first `limit` bytes (the whole file when it
 * is shorter). The file must be the same before and after either way. */
static void hash_file(const wchar_t *path,
        uint64_t limit,
        const WfShaSeen *seen,
        WfShaCancel cancel,
        void *cancel_ctx,
        WfShaProgress progress,
        void *progress_ctx,
        WfShaResult *out)
{
	HANDLE h;
	uint64_t expected;
	Fingerprint before, after;
	DWORD attributes = 0;
	WfSha *sha;
	UCHAR *buf;
	uint64_t done = 0;

	memset(out, 0, sizeof *out);
	if (!path) {
		fail(out, WF_E_INVAL, 0);
		return;
	}
	h = CreateFileW(path, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT |
	                FILE_FLAG_BACKUP_SEMANTICS, /* so a folder opens to be refused */
	        NULL);
	if (INVALID_HANDLE_VALUE == h) {
		DWORD error = GetLastError();

		fail(out, status_of(error), error);
		return;
	}
	if (!fingerprint(h, &before, &attributes)) {
		DWORD error = GetLastError();

		CloseHandle(h);
		fail(out, WF_E_OS, error);
		return;
	}
	if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
		CloseHandle(h);
		fail(out, WF_E_INVAL, 0);
		return;
	}
	if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		CloseHandle(h); /* a link is not followed, and not hashed */
		fail(out, WF_E_ACCESS, 0);
		return;
	}
	if (seen) {
		/* the file the listing showed, on this very handle */
		bool same = before.size == seen->size &&
		        before.last_write == seen->last_write;

		if (same && wf_fileid_usable_to_read(&seen->id)) {
			WfFileId now;
			DWORD error = 0;

			same = wf_handle_identity_any(h, &now, &error) &&
			        wf_fileid_equal_or_weak(&seen->id, &now);
		}
		if (!same) {
			CloseHandle(h);
			fail(out, WF_E_CHANGED, 0);
			return;
		}
	}
	out->size = before.size;
	out->last_write = before.last_write;
	expected = limit && limit < before.size ? limit : before.size;
	sha = wf_sha_new();
	buf = malloc(WF_SHA_BUFFER);
	if (!sha || !buf) {
		wf_sha_free(sha);
		free(buf);
		CloseHandle(h);
		fail(out, WF_E_NOMEM, 0);
		return;
	}
	for (;;) {
		DWORD got = 0;

		if (cancel && cancel(cancel_ctx)) {
			wf_sha_free(sha);
			free(buf);
			CloseHandle(h);
			fail(out, WF_E_CANCELLED, 0);
			return;
		}
		if (limit && done >= expected) {
			break; /* a prefix: not one byte more than was asked */
		}
		if (!ReadFile(h, buf,
		            limit && expected - done < WF_SHA_BUFFER
		                    ? (DWORD)(expected - done)
		                    : WF_SHA_BUFFER,
		            &got, NULL)) {
			DWORD error = GetLastError();

			wf_sha_free(sha);
			free(buf);
			CloseHandle(h);
			fail(out, WF_E_OS, error);
			return;
		}
		if (0 == got) {
			break;
		}
		if (!wf_sha_update(sha, buf, got)) {
			wf_sha_free(sha);
			free(buf);
			CloseHandle(h);
			fail(out, WF_E_OS, 0);
			return;
		}
		done += got;
		if (progress) {
			progress(progress_ctx, done, before.size);
		}
	}
	free(buf);
	if (!fingerprint(h, &after, NULL) || after.size != before.size ||
	        after.last_write != before.last_write || done != expected) {
		/* not the file that was opened any more: no digest is claimed */
		wf_sha_free(sha);
		CloseHandle(h);
		fail(out, WF_E_CHANGED, 0);
		return;
	}
	CloseHandle(h);
	if (!wf_sha_finish(sha, out->digest)) {
		fail(out, WF_E_OS, 0);
		return;
	}
	out->st = WF_OK;
}

void wf_sha256_file(const wchar_t *path,
        WfShaCancel cancel,
        void *cancel_ctx,
        WfShaProgress progress,
        void *progress_ctx,
        WfShaResult *out)
{
	hash_file(
	        path, 0, NULL, cancel, cancel_ctx, progress, progress_ctx, out);
}

void wf_sha256_file_prefix(const wchar_t *path,
        uint64_t limit,
        WfShaCancel cancel,
        void *cancel_ctx,
        WfShaResult *out)
{
	hash_file(path, limit, NULL, cancel, cancel_ctx, NULL, NULL, out);
}

void wf_sha256_file_seen(const wchar_t *path,
        uint64_t limit,
        const WfShaSeen *seen,
        WfShaCancel cancel,
        void *cancel_ctx,
        WfShaResult *out)
{
	hash_file(path, limit, seen, cancel, cancel_ctx, NULL, NULL, out);
}

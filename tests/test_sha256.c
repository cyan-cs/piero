/*
 * SHA-256 of a file on request (DR-DRAFT-027): the known vectors, pieces of every
 * size, files around the read size, progress, cancel, a file that changes while it
 * is read (no digest), what is refused (a folder, a link, a missing file, a file
 * held without sharing), and the worker: one request at a time, a newer one ends
 * the older, cancel delivers nothing, stopping a running hash returns at once.
 * Everything is made in a folder of the temporary directory.
 */
#include <windows.h>
#include <winioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xfer/wf_hashworker.h"
#include "xfer/wf_sha256_win.h"
#include "wf_test.h"

static wchar_t root[MAX_PATH + 40];
static int skips;

static void skip(const char *what)
{
	printf("SKIP %s\n", what);
	skips++;
}

static wchar_t *path_of(const wchar_t *name)
{
	static wchar_t buf[8][MAX_PATH + 80];
	static int next;
	wchar_t *p = buf[next++ % 8];

	swprintf(p, MAX_PATH + 80, L"%ls\\%ls", root, name);
	return p;
}

static void hex_of(const uint8_t d[32], char out[65])
{
	wf_sha256_hex(d, out);
}

static bool sha_of(const void *bytes, size_t n, char hex[65])
{
	WfSha *s = wf_sha_new();
	uint8_t d[32];

	if (!s || !wf_sha_update(s, bytes, n) || !wf_sha_finish(s, d)) {
		return false;
	}
	hex_of(d, hex);
	return true;
}

static void vectors(void)
{
	char hex[65];
	char *million = malloc(1000000);

	CHECK(sha_of("", 0, hex));
	CHECK(!strcmp(hex,
	        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b8"
	        "55"));
	CHECK(sha_of("abc", 3, hex));
	CHECK(!strcmp(hex,
	        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015"
	        "ad"));
	CHECK(sha_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
	        56, hex));
	CHECK(!strcmp(hex,
	        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06"
	        "c1"));
	CHECK(NULL != million);
	if (million) {
		memset(million, 'a', 1000000);
		CHECK(sha_of(million, 1000000, hex));
		CHECK(!strcmp(hex,
		        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39"
		        "ccc7112cd0"));
		free(million);
	}
	{
		uint8_t d[32] = {0x00, 0x01, 0xab, 0xff};
		char h[65];

		memset(d + 4, 0x10, 28);
		wf_sha256_hex(d, h);
		CHECK(!strncmp(h, "0001abff1010", 12) && 64 == strlen(h));
	}
}

static void pieces(void)
{
	unsigned char data[300];
	char whole[65], split[65];

	for (size_t i = 0; i < sizeof data; i++) {
		data[i] = (unsigned char)(i * 7 + 3);
	}
	CHECK(sha_of(data, sizeof data, whole));
	for (size_t step = 1; step <= 130; step++) {
		WfSha *s = wf_sha_new();
		uint8_t d[32];

		CHECK(NULL != s);
		for (size_t at = 0; s && at < sizeof data; at += step) {
			size_t n = sizeof data - at < step ? sizeof data - at
			                                   : step;

			CHECK(wf_sha_update(s, data + at, n));
		}
		CHECK(s && wf_sha_finish(s, d));
		hex_of(d, split);
		CHECK(!strcmp(whole, split));
	}
}

static bool write_file(const wchar_t *path, const void *bytes, size_t n)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD done;
	bool ok = INVALID_HANDLE_VALUE != h;

	for (size_t at = 0; ok && at < n;) {
		DWORD chunk = n - at > 1u << 20 ? 1u << 20 : (DWORD)(n - at);

		ok = WriteFile(
		             h, (const char *)bytes + at, chunk, &done, NULL) &&
		        done == chunk;
		at += chunk;
	}
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	return ok;
}

static unsigned char *pattern(size_t n)
{
	unsigned char *b = malloc(n ? n : 1);

	for (size_t i = 0; b && i < n; i++) {
		b[i] = (unsigned char)((i * 131 + (i >> 8) * 17) & 0xff);
	}
	return b;
}

static void file_sizes(void)
{
	static const size_t sizes[] = {0, 1, 63, 64, 65, WF_SHA_BUFFER - 1,
	        WF_SHA_BUFFER, WF_SHA_BUFFER + 1, 2 * WF_SHA_BUFFER,
	        (1u << 20) + 17};

	for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
		unsigned char *b = pattern(sizes[k]);
		WfShaResult r;
		char want[65], got[65];

		CHECK(NULL != b);
		CHECK(write_file(path_of(L"f.bin"), b, sizes[k]));
		CHECK(sha_of(b, sizes[k], want));
		wf_sha256_file(path_of(L"f.bin"), NULL, NULL, NULL, NULL, &r);
		CHECK(WF_OK == r.st);
		hex_of(r.digest, got);
		if (strcmp(want, got)) {
			printf("  size %zu: got %s want %s\n", sizes[k], got,
			        want);
		}
		CHECK(!strcmp(want, got));
		CHECK(r.size == sizes[k]);
		CHECK(r.last_write != 0);
		free(b);
	}
}

/* The hash of the first bytes of a file: what the duplicate search compares before it
 * reads the rest. A limit of 0 is the whole file; a longer one is the whole file too. */
static void prefixes(void)
{
	size_t n = 3 * WF_SHA_BUFFER + 77;
	unsigned char *b = pattern(n);
	static const size_t limits[] = {1, 63, 64, 65, WF_SHA_BUFFER - 1,
	        WF_SHA_BUFFER, WF_SHA_BUFFER + 1, 2 * WF_SHA_BUFFER + 5};

	CHECK(write_file(path_of(L"x.bin"), b, n));
	for (size_t k = 0; k < sizeof limits / sizeof limits[0]; k++) {
		WfShaResult r;
		char want[65], got[65];

		wf_sha256_file_prefix(
		        path_of(L"x.bin"), limits[k], NULL, NULL, &r);
		CHECK(WF_OK == r.st);
		CHECK(sha_of(b, limits[k], want));
		hex_of(r.digest, got);
		if (strcmp(want, got)) {
			printf("  prefix %zu: got %s want %s"
			       "\n",
			        limits[k], got, want);
		}
		CHECK(!strcmp(want, got));
		CHECK(r.size ==
		        n); /* the size of the file, not of what was read */
	}
	{
		WfShaResult pr, whole;
		char a[65], c[65];

		wf_sha256_file(
		        path_of(L"x.bin"), NULL, NULL, NULL, NULL, &whole);
		wf_sha256_file_prefix(path_of(L"x.bin"), 0, NULL, NULL, &pr);
		hex_of(pr.digest, a);
		hex_of(whole.digest, c);
		CHECK(WF_OK == pr.st && !strcmp(a, c));
		wf_sha256_file_prefix(path_of(L"x.bin"), n, NULL, NULL, &pr);
		hex_of(pr.digest, a);
		CHECK(WF_OK == pr.st && !strcmp(a, c));
		wf_sha256_file_prefix(
		        path_of(L"x.bin"), n * 5, NULL, NULL, &pr);
		hex_of(pr.digest, a);
		CHECK(WF_OK == pr.st && !strcmp(a, c));
	}
	/* two files that begin alike and end differently have the same prefix hash and
	 * different whole hashes */
	{
		unsigned char *d = pattern(n);
		WfShaResult p1, p2, w1, w2;

		d[n - 1] ^= 0x5a;
		CHECK(write_file(path_of(L"y.bin"), d, n));
		wf_sha256_file_prefix(
		        path_of(L"x.bin"), WF_SHA_BUFFER, NULL, NULL, &p1);
		wf_sha256_file_prefix(
		        path_of(L"y.bin"), WF_SHA_BUFFER, NULL, NULL, &p2);
		wf_sha256_file(path_of(L"x.bin"), NULL, NULL, NULL, NULL, &w1);
		wf_sha256_file(path_of(L"y.bin"), NULL, NULL, NULL, NULL, &w2);
		CHECK(!memcmp(p1.digest, p2.digest, 32));
		CHECK(memcmp(w1.digest, w2.digest, 32));
		free(d);
	}
	/* the same refusals as for the whole file */
	{
		WfShaResult r;

		wf_sha256_file_prefix(
		        path_of(L"missing-x.bin"), 10, NULL, NULL, &r);
		CHECK(WF_E_NOENT == r.st);
		wf_sha256_file_prefix(root, 10, NULL, NULL, &r);
		CHECK(WF_E_INVAL == r.st);
	}
	free(b);
}

typedef struct
{
	uint64_t last, calls, total;
	bool backwards;
} Seen;

static void on_progress(void *ctx, uint64_t done, uint64_t total)
{
	Seen *s = ctx;

	if (done < s->last) {
		s->backwards = true;
	}
	s->last = done;
	s->total = total;
	s->calls++;
}

static void progress_and_cancel(void)
{
	size_t n = 5 * WF_SHA_BUFFER + 100;
	unsigned char *b = pattern(n);
	Seen seen = {0};
	WfShaResult r;

	CHECK(write_file(path_of(L"p.bin"), b, n));
	wf_sha256_file(path_of(L"p.bin"), NULL, NULL, on_progress, &seen, &r);
	CHECK(WF_OK == r.st);
	CHECK(6 == seen.calls && !seen.backwards);
	CHECK(seen.last == n && seen.total == n);
	free(b);
}

static bool cancel_after_two(void *ctx)
{
	int *calls = ctx;

	return ++*calls > 2;
}

static void cancelling(void)
{
	size_t n = 8 * WF_SHA_BUFFER;
	unsigned char *b = pattern(n);
	int calls = 0;
	WfShaResult r;

	CHECK(write_file(path_of(L"c.bin"), b, n));
	wf_sha256_file(
	        path_of(L"c.bin"), cancel_after_two, &calls, NULL, NULL, &r);
	CHECK(WF_E_CANCELLED == r.st);
	CHECK(3 ==
	        calls); /* asked before each read, stopped before the third */
	{
		uint8_t zero[32] = {0};

		CHECK(!memcmp(
		        r.digest, zero, 32)); /* no digest of a stopped read */
	}
	free(b);
}

/* The file grows while it is being read: the digest would be of bytes that were
 * never one file, so there is none. */
static void grow_on_first_read(void *ctx, uint64_t done, uint64_t total)
{
	int *grown = ctx;

	(void)done;
	(void)total;
	if (!*grown) {
		HANDLE h = CreateFileW(path_of(L"g.bin"), FILE_APPEND_DATA,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD done_bytes;

		if (INVALID_HANDLE_VALUE != h) {
			WriteFile(h, "more", 4, &done_bytes, NULL);
			CloseHandle(h);
			*grown = 1;
		}
	}
}

static void changed_while_read(void)
{
	size_t n = 3 * WF_SHA_BUFFER;
	unsigned char *b = pattern(n);
	int grown = 0;
	WfShaResult r;

	CHECK(write_file(path_of(L"g.bin"), b, n));
	wf_sha256_file(
	        path_of(L"g.bin"), NULL, NULL, grow_on_first_read, &grown, &r);
	CHECK(grown);
	CHECK(WF_E_CHANGED == r.st);
	{
		uint8_t zero[32] = {0};

		CHECK(!memcmp(r.digest, zero, 32));
	}
	free(b);
}

/* Rewritten in place, to the same size: the first bytes were read before the
 * change, so no digest is given. The time is set in the past first, so that the
 * rewrite is seen even when it is quick. */
static void rewrite_on_first_read(void *ctx, uint64_t done, uint64_t total)
{
	int *rewritten = ctx;

	(void)done;
	(void)total;
	if (!*rewritten) {
		HANDLE h = CreateFileW(path_of(L"r.bin"), GENERIC_WRITE,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD n;

		if (INVALID_HANDLE_VALUE != h) {
			WriteFile(h, "X", 1, &n, NULL);
			CloseHandle(h);
			*rewritten = 1;
		}
	}
}

static void rewritten_in_place(void)
{
	size_t n = 3 * WF_SHA_BUFFER;
	unsigned char *b = pattern(n);
	int rewritten = 0;
	WfShaResult r;
	HANDLE h;
	FILETIME old = {0xD53E8000u, 0x01B21DD2u}; /* 2001 */

	CHECK(write_file(path_of(L"r.bin"), b, n));
	h = CreateFileW(path_of(L"r.bin"), FILE_WRITE_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		CHECK(SetFileTime(h, NULL, NULL, &old));
		CloseHandle(h);
	}
	wf_sha256_file(path_of(L"r.bin"), NULL, NULL, rewrite_on_first_read,
	        &rewritten, &r);
	CHECK(rewritten);
	CHECK(WF_E_CHANGED == r.st);
	free(b);
}

static void refusals(void)
{
	WfShaResult r;
	HANDLE h;
	wchar_t dir[MAX_PATH + 80], link[MAX_PATH + 80];

	swprintf(dir, MAX_PATH + 80, L"%ls\\adir", root);
	CreateDirectoryW(dir, NULL);
	wf_sha256_file(dir, NULL, NULL, NULL, NULL, &r);
	CHECK(WF_E_INVAL == r.st);

	wf_sha256_file(path_of(L"missing.bin"), NULL, NULL, NULL, NULL, &r);
	CHECK(WF_E_NOENT == r.st && ERROR_FILE_NOT_FOUND == r.error);

	wf_sha256_file(NULL, NULL, NULL, NULL, NULL, &r);
	CHECK(WF_E_INVAL == r.st);

	/* held with no sharing: it cannot be read, and the answer says why */
	CHECK(write_file(path_of(L"held.bin"), "held", 4));
	h = CreateFileW(path_of(L"held.bin"), GENERIC_READ | GENERIC_WRITE, 0,
	        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	wf_sha256_file(path_of(L"held.bin"), NULL, NULL, NULL, NULL, &r);
	CHECK(WF_E_ACCESS == r.st && ERROR_SHARING_VIOLATION == r.error);
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}

	/* a link is not followed and not hashed */
	CHECK(write_file(path_of(L"target.bin"), "t", 1));
	swprintf(link, MAX_PATH + 80, L"%ls\\link.bin", root);
	if (CreateSymbolicLinkW(link, L"target.bin",
	            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
		wf_sha256_file(link, NULL, NULL, NULL, NULL, &r);
		CHECK(WF_E_ACCESS == r.st);
	} else {
		skip("symbolic link: the account may not create one");
	}
}

static bool wait_update(
        WfHashWorker *w, WfHashUpdate *u, bool want_finished, DWORD ms)
{
	ULONGLONG until = GetTickCount64() + ms;

	while (GetTickCount64() < until) {
		if (WAIT_OBJECT_0 ==
		                WaitForSingleObject(
		                        wf_hashworker_event(w), 50) &&
		        wf_hashworker_poll(w, u) &&
		        (!want_finished || u->finished)) {
			return true;
		}
	}
	return false;
}

/* A 3 GiB sparse file: reading it takes about a second, so a request can be
 * ended while it is running. False when the volume cannot make one. */
static bool make_huge(const wchar_t *path)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD got;
	LARGE_INTEGER end;
	bool ok = INVALID_HANDLE_VALUE != h;

	end.QuadPart = 3ll << 30;
	ok = ok &&
	        DeviceIoControl(
	                h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &got, NULL);
	ok = ok && SetFilePointerEx(h, end, NULL, FILE_BEGIN) &&
	        SetEndOfFile(h);
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	return ok;
}

static void worker(void)
{
	WfHashWorker *w = NULL;
	WfHashPort port;
	WfHashUpdate u;
	size_t big = 64u << 20;
	unsigned char *b = pattern(big);
	char want[65], got[65];

	CHECK(WF_OK == wf_hashworker_start(&w));
	if (!w) {
		free(b);
		return;
	}
	port = wf_hashworker_port(w);
	CHECK(write_file(path_of(L"big.bin"), b, big));
	CHECK(write_file(path_of(L"small.txt"), "abc", 3));

	/* a request is answered, with the digest of the file */
	CHECK(WF_OK == port.start(port.ctx, 1, path_of(L"small.txt")));
	CHECK(wait_update(w, &u, true, 10000));
	CHECK(1 == u.gen && u.finished && WF_OK == u.res.st);
	hex_of(u.res.digest, got);
	CHECK(!strcmp(got,
	        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015"
	        "ad"));

	/* a large file: progress comes, then the end, equal to the memory hash */
	CHECK(sha_of(b, big, want));
	CHECK(WF_OK == port.start(port.ctx, 2, path_of(L"big.bin")));
	{
		bool finished = false;

		for (int k = 0; k < 2000 && !finished; k++) {
			if (!wait_update(w, &u, false, 100)) {
				continue;
			}
			CHECK(2 == u.gen);
			if (u.finished) {
				finished = true;
			} else {
				/* progress is every 100 ms at most: a fast disk may
				 * show none; what is shown must be consistent */
				CHECK(u.done <= u.total && u.total == big);
			}
		}
		CHECK(finished && WF_OK == u.res.st);
		hex_of(u.res.digest, got);
		CHECK(!strcmp(got, want));
	}

	/* a newer request ends the older one: only the newer is delivered */
	CHECK(WF_OK == port.start(port.ctx, 3, path_of(L"big.bin")));
	CHECK(WF_OK == port.start(port.ctx, 4, path_of(L"small.txt")));
	{
		bool saw_three = false, saw_four = false;

		for (int k = 0; k < 200 && !saw_four; k++) {
			if (wait_update(w, &u, true, 100)) {
				saw_three |= 3 == u.gen;
				saw_four |= 4 == u.gen;
			}
		}
		CHECK(saw_four && !saw_three);
	}

	/* cancel delivers nothing, also when the hash is running */
	if (make_huge(path_of(L"huge.bin"))) {
		bool running = false;

		CHECK(WF_OK == port.start(port.ctx, 5, path_of(L"huge.bin")));
		for (int k = 0; k < 50 && !running; k++) {
			running = wait_update(w, &u, false, 100) && 5 == u.gen;
		}
		CHECK(running); /* progress came: it is being read */
		port.cancel(port.ctx);
		CHECK(!wait_update(w, &u, true, 4000));
	} else {
		skip("cancel of a running hash: no 3 GiB sparse file here");
		CHECK(WF_OK == port.start(port.ctx, 5, path_of(L"big.bin")));
		port.cancel(port.ctx);
		CHECK(!wait_update(w, &u, true, 800));
	}

	/* a failure is an answer, with its reason */
	CHECK(WF_OK == port.start(port.ctx, 6, path_of(L"nothing.bin")));
	CHECK(wait_update(w, &u, true, 5000));
	CHECK(6 == u.gen && WF_E_NOENT == u.res.st);

	CHECK(WF_E_INVAL == port.start(port.ctx, 0, path_of(L"small.txt")));
	CHECK(WF_E_INVAL == port.start(port.ctx, 7, NULL));

	/* stopping in the middle of a hash returns within the bounded wait */
	CHECK(WF_OK == port.start(port.ctx, 8, path_of(L"big.bin")));
	Sleep(20);
	{
		ULONGLONG t0 = GetTickCount64();
		WfWorkerStopReport rep = wf_hashworker_stop(w);

		CHECK(WF_OK == rep.status);
		CHECK(GetTickCount64() - t0 < 3000);
		if (rep.completion) {
			WaitForSingleObject(rep.completion, 5000);
			CloseHandle(rep.completion);
		}
	}
	free(b);
}

static void cleanup(void)
{
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(path_of(L"*"), &fd);

	if (INVALID_HANDLE_VALUE != h) {
		do {
			if (wcscmp(fd.cFileName, L".") &&
			        wcscmp(fd.cFileName, L"..")) {
				wchar_t *p = path_of(fd.cFileName);

				if (fd.dwFileAttributes &
				        FILE_ATTRIBUTE_DIRECTORY) {
					RemoveDirectoryW(p);
				} else {
					DeleteFileW(p);
				}
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(root);
}

int main(void)
{
	wchar_t tmp[MAX_PATH];

	GetTempPathW(MAX_PATH, tmp);
	swprintf(root, MAX_PATH + 40, L"%lssha256_%lu", tmp,
	        (unsigned long)GetCurrentProcessId());
	CreateDirectoryW(root, NULL);

	vectors();
	pieces();
	file_sizes();
	progress_and_cancel();
	prefixes();
	cancelling();
	changed_while_read();
	rewritten_in_place();
	refusals();
	worker();
	cleanup();

	if (wf_test_failures) {
		printf("test_sha256: %d failure(s)\n", wf_test_failures);
		return 1;
	}
	printf("test_sha256: all checks passed (%d skipped)\n", skips);
	return 0;
}

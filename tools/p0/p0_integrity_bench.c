/* Isolated measurement only. Results grant no commit/deletion authority.
 * Buffered reads can hit cache; process I/O counters are not physical disk I/O.
 * SHA readback excludes the production HMAC identity/stability checks. */
#include <windows.h>

#include <bcrypt.h>
#include <psapi.h>
#include <string.h>

#include "../../src/xfer/wf_integrity_win.h"
#include "p0_common.h"
#include "p0_lab.h"

#define CHUNK 65536u
#define SMALL_FILES 1001u
#define FILES (SMALL_FILES + 3u)
#define LARGE_BYTES (UINT64_C(64) * 1024u * 1024u)

typedef struct
{
	LARGE_INTEGER tick;
	uint64_t cpu;
	IO_COUNTERS io;
	SIZE_T working_set;
} Sample;

typedef struct
{
	BCRYPT_ALG_HANDLE algorithm;
	ULONG object_bytes;
	UCHAR key[32];
	int hmac;
} Hash;

static uint64_t filetime_value(FILETIME t)
{
	return ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime;
}

static int sample(Sample *s)
{
	FILETIME created, exited, kernel, user;
	PROCESS_MEMORY_COUNTERS memory = {0};
	memory.cb = sizeof memory;
	if (!QueryPerformanceCounter(&s->tick) ||
	        !GetProcessTimes(GetCurrentProcess(), &created, &exited,
	                &kernel, &user) ||
	        !GetProcessIoCounters(GetCurrentProcess(), &s->io) ||
	        !GetProcessMemoryInfo(
	                GetCurrentProcess(), &memory, sizeof memory)) {
		return 0;
	}
	s->cpu = filetime_value(kernel) + filetime_value(user);
	s->working_set = memory.WorkingSetSize;
	return 1;
}

static void row(const char *phase,
        const char *algorithm,
        unsigned files,
        uint64_t bytes,
        uint64_t hashed,
        const Sample *a,
        const Sample *b,
        LONGLONG frequency,
        int ok)
{
	printf("%s,%s,%u,%llu,%llu,%.6f,%.6f,%llu,%llu,%llu,%llu,%llu,%llu,%"
	       "s\n",
	        phase, algorithm, files, (unsigned long long)bytes,
	        (unsigned long long)hashed,
	        (double)(b->tick.QuadPart - a->tick.QuadPart) /
	                (double)frequency,
	        (double)(b->cpu - a->cpu) / 10000000.0,
	        (unsigned long long)(b->io.ReadOperationCount -
	                a->io.ReadOperationCount),
	        (unsigned long long)(b->io.ReadTransferCount -
	                a->io.ReadTransferCount),
	        (unsigned long long)(b->io.WriteOperationCount -
	                a->io.WriteOperationCount),
	        (unsigned long long)(b->io.WriteTransferCount -
	                a->io.WriteTransferCount),
	        (unsigned long long)a->working_set,
	        (unsigned long long)b->working_set, ok ? "PASS" : "FAIL");
}

static void hash_close(Hash *h)
{
	if (h->algorithm) {
		BCryptCloseAlgorithmProvider(h->algorithm, 0);
	}
	SecureZeroMemory(h, sizeof *h);
}

static int hash_open(Hash *h, int hmac)
{
	ULONG got = 0;
	memset(h, 0, sizeof *h);
	h->hmac = hmac;
	if (hmac &&
	        BCryptGenRandom(NULL, h->key, sizeof h->key,
	                BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
		return 0;
	}
	if (BCryptOpenAlgorithmProvider(&h->algorithm, BCRYPT_SHA256_ALGORITHM,
	            NULL, hmac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0) < 0) {
		return 0;
	}
	return BCryptGetProperty(h->algorithm, BCRYPT_OBJECT_LENGTH,
	               (PUCHAR)&h->object_bytes, sizeof h->object_bytes, &got,
	               0) >= 0 &&
	        got == sizeof h->object_bytes && h->object_bytes;
}

static int hash_stream(Hash *h, HANDLE file, uint64_t size, UCHAR digest[32])
{
	BCRYPT_HASH_HANDLE hash = NULL;
	UCHAR *object = NULL, *buffer = NULL;
	LARGE_INTEGER zero = {0};
	uint64_t total = 0;
	int ok = 0;
	DWORD got;
	object = calloc(1, h->object_bytes);
	buffer = malloc(CHUNK);
	if (!object || !buffer) {
		goto done;
	}
	memset(buffer, 0x5a, CHUNK);
	if (BCryptCreateHash(h->algorithm, &hash, object, h->object_bytes,
	            h->hmac ? h->key : NULL, h->hmac ? (ULONG)sizeof h->key : 0,
	            0) < 0) {
		goto done;
	}
	if (INVALID_HANDLE_VALUE != file &&
	        !SetFilePointerEx(file, zero, NULL, FILE_BEGIN)) {
		goto done;
	}
	while (total < size) {
		DWORD want = (DWORD)((size - total) > CHUNK ? CHUNK
		                                            : (size - total));
		got = want;
		if (INVALID_HANDLE_VALUE != file &&
		        (!ReadFile(file, buffer, want, &got, NULL) || !got)) {
			goto done;
		}
		if (BCryptHashData(hash, buffer, got, 0) < 0) {
			goto done;
		}
		total += got;
	}
	if (INVALID_HANDLE_VALUE != file &&
	        (!ReadFile(file, buffer, 1, &got, NULL) || got)) {
		goto done;
	}
	if (BCryptFinishHash(hash, digest, 32, 0) < 0) {
		goto done;
	}
	ok = 1;
done:
	if (hash && BCryptDestroyHash(hash) < 0) {
		object = NULL;
		ok = 0;
	}
	if (object) {
		SecureZeroMemory(object, h->object_bytes);
		free(object);
	}
	if (buffer) {
		SecureZeroMemory(buffer, CHUNK);
		free(buffer);
	}
	return ok;
}

static int relative(
        unsigned i, const wchar_t *side, wchar_t out[1024], uint64_t *size)
{
	if (i < SMALL_FILES) {
		*size = 4096u + i % 31u;
		return swprintf(out, 1024, L"%ls\\small-%04u.bin", side, i) > 0;
	}
	if (SMALL_FILES == i) {
		*size = 0;
		return swprintf(out, 1024, L"%ls\\zero.bin", side) > 0;
	}
	if (i == SMALL_FILES + 1) {
		*size = LARGE_BYTES;
		return swprintf(out, 1024, L"%ls\\large-64MiB.bin", side) > 0;
	}
	{
		wchar_t directory[121], name[201];
		unsigned j;
		for (j = 0; j < 120; j++) {
			directory[j] = L'長';
		}
		directory[120] = 0;
		for (j = 0; j < 200; j++) {
			name[j] = L'漢';
		}
		name[200] = 0;
		*size = 65537;
		return swprintf(out, 1024, L"%ls\\%ls\\%ls", side, directory,
		               name) > 0;
	}
}

static int prepare(P0Lab *lab, uint64_t *total)
{
	UCHAR *buffer = malloc(CHUNK);
	wchar_t rel[1024], path[2048];
	unsigned i;
	int ok = 0;
	HANDLE file = INVALID_HANDLE_VALUE;
	*total = 0;
	if (!buffer) {
		return 0;
	}
	if (!p0_area_mkdir(lab->area, L"source") ||
	        !p0_area_mkdir(lab->area, L"destination")) {
		goto done;
	}
	for (i = 0; i < 2; i++) {
		uint64_t unused;
		wchar_t *leaf;
		if (!relative(FILES - 1, i ? L"destination" : L"source", rel,
		            &unused)) {
			goto done;
		}
		leaf = wcsrchr(rel, L'\\');
		*leaf = 0;
		if (!p0_area_mkdir(lab->area, rel)) {
			goto done;
		}
	}
	for (i = 0; i < FILES; i++) {
		uint64_t size, left;
		DWORD written;
		if (!relative(i, L"source", rel, &size) ||
		        !p0_area_xpath(lab->area, rel, path, 2048)) {
			goto done;
		}
		file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
		        FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE == file ||
		        !p0_area_track(lab->area, rel, 0)) {
			goto done;
		}
		memset(buffer, (int)(i % 251), CHUNK);
		for (left = size; left;) {
			DWORD n = (DWORD)(left > CHUNK ? CHUNK : left);
			if (!WriteFile(file, buffer, n, &written, NULL) ||
			        written != n) {
				goto done;
			}
			left -= n;
		}
		if (!CloseHandle(file)) {
			file = INVALID_HANDLE_VALUE;
			goto done;
		}
		file = INVALID_HANDLE_VALUE;
		*total += size;
	}
	ok = 1;
done:
	if (INVALID_HANDLE_VALUE != file) {
		CloseHandle(file);
	}
	free(buffer);
	return ok;
}

static int copy_all(P0Lab *lab)
{
	unsigned i;
	wchar_t src_rel[1024], dst_rel[1024], src[2048], dst[2048];
	uint64_t size;
	for (i = 0; i < FILES; i++) {
		BOOL cancel = FALSE;
		if (!relative(i, L"source", src_rel, &size) ||
		        !relative(i, L"destination", dst_rel, &size) ||
		        !p0_area_xpath(lab->area, src_rel, src, 2048) ||
		        !p0_area_xpath(lab->area, dst_rel, dst, 2048)) {
			return 0;
		}
		if (!CopyFileExW(src, dst, NULL, NULL, &cancel,
		            COPY_FILE_FAIL_IF_EXISTS)) {
			return 0;
		}
		if (!p0_area_track(lab->area, dst_rel, 0)) {
			return 0;
		}
	}
	return 1;
}

static int compare_all(P0Lab *lab, Hash *sha, WfIntegrityJob *job)
{
	unsigned i;
	wchar_t rel[1024], path[2048];
	uint64_t size;
	int ok = 0;
	HANDLE files[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
	for (i = 0; i < FILES; i++) {
		FILE_ID_INFO id[2];
		UCHAR digest[2][32] = {{0}};
		unsigned j;
		for (j = 0; j < 2; j++) {
			if (!relative(i, j ? L"destination" : L"source", rel,
			            &size) ||
			        !p0_area_xpath(lab->area, rel, path, 2048)) {
				goto done;
			}
			files[j] = CreateFileW(path, GENERIC_READ,
			        FILE_SHARE_READ, NULL, OPEN_EXISTING,
			        FILE_FLAG_SEQUENTIAL_SCAN, NULL);
			if (INVALID_HANDLE_VALUE == files[j] ||
			        !GetFileInformationByHandleEx(files[j],
			                FileIdInfo, &id[j], sizeof id[j])) {
				goto done;
			}
		}
		if (job) {
			WfIntegrityReport report;
			if (wf_integrity_compare(job, files[0], &id[0],
			            files[1], &id[1], size, NULL, NULL,
			            &report) != WF_INTEGRITY_MATCH ||
			        report.source_bytes != size ||
			        report.destination_bytes != size) {
				goto done;
			}
		} else {
			if (!hash_stream(sha, files[0], size, digest[0]) ||
			        !hash_stream(sha, files[1], size, digest[1]) ||
			        memcmp(digest[0], digest[1], 32)) {
				goto done;
			}
		}
		SecureZeroMemory(digest, sizeof digest);
		CloseHandle(files[1]);
		CloseHandle(files[0]);
		files[0] = files[1] = INVALID_HANDLE_VALUE;
	}
	ok = 1;
done:
	if (INVALID_HANDLE_VALUE != files[1]) {
		CloseHandle(files[1]);
	}
	if (INVALID_HANDLE_VALUE != files[0]) {
		CloseHandle(files[0]);
	}
	return ok;
}

int main(void)
{
	P0Lab lab;
	Hash sha = {0}, hmac = {0};
	WfIntegrityJob *job = NULL;
	WfIntegrityReport report;
	LARGE_INTEGER frequency;
	Sample a, b, pipeline_a, pipeline_b;
	uint64_t total = 0;
	UCHAR digest[32] = {0};
	int ok = 0, phase = 0;
	unsigned j;
	char os[256];
	DWORD error = 0;
	memset(&lab, 0, sizeof lab);
	lab.pin = INVALID_HANDLE_VALUE;
	puts("# fixed_buffer_bytes=65536; cached_buffered_IO; "
	     "SHA_file_rows_exclude_production_stability_checks");
	puts("# single_pass_fixed_order; cache_not_flushed; "
	     "wall_times_include_open_close; no_cold_disk_or_ratio_claim");
	puts("# xxHash=NOT_TESTED; dependency_not_present; "
	     "no_dependency_installed");
	p0_os_summary(os, sizeof os);
	printf("# os=%s\n", os);
	puts("phase,algorithm,files,payload_bytes,hashed_bytes,wall_seconds,"
	     "cpu_seconds,read_operations,read_bytes,write_operations,write_"
	     "bytes,working_set_before,working_set_after,result");
	if (!QueryPerformanceFrequency(&frequency) || !hash_open(&sha, 0) ||
	        !hash_open(&hmac, 1)) {
		goto done;
	}
	for (j = 0; j < 2; j++) {
		if (!sample(&a)) {
			goto done;
		}
		phase = hash_stream(j ? &hmac : &sha, INVALID_HANDLE_VALUE,
		        LARGE_BYTES, digest);
		if (!sample(&b)) {
			goto done;
		}
		row("memory", j ? "HMAC-SHA256" : "SHA256", 0, LARGE_BYTES,
		        LARGE_BYTES, &a, &b, frequency.QuadPart, phase);
		if (!phase) {
			goto done;
		}
	}
	if (!p0_lab_open(&lab, "integrity-bench", 0) ||
	        !prepare(&lab, &total)) {
		goto done;
	}
	if (!sample(&pipeline_a) || !sample(&a)) {
		goto done;
	}
	phase = copy_all(&lab);
	if (!sample(&b)) {
		goto done;
	}
	row("copy", "CopyFileExW", FILES, total, 0, &a, &b, frequency.QuadPart,
	        phase);
	if (!phase) {
		goto done;
	}
	if (!wf_integrity_begin(&job, &report) || !sample(&a)) {
		goto done;
	}
	phase = compare_all(&lab, NULL, job);
	if (!sample(&b)) {
		goto done;
	}
	row("file_readback", "production-HMAC-SHA256", FILES, total, total * 2,
	        &a, &b, frequency.QuadPart, phase);
	if (!phase || !sample(&pipeline_b)) {
		goto done;
	}
	row("copy_plus_verify", "CopyFileExW+production-HMAC", FILES, total,
	        total * 2, &pipeline_a, &pipeline_b, frequency.QuadPart, 1);
	if (!sample(&a)) {
		goto done;
	}
	phase = compare_all(&lab, &sha, NULL);
	if (!sample(&b)) {
		goto done;
	}
	row("file_readback", "SHA256", FILES, total, total * 2, &a, &b,
	        frequency.QuadPart, phase);
	if (!phase) {
		goto done;
	}
	if (!sample(&a)) {
		goto done;
	}
	phase = compare_all(&lab, &hmac, NULL);
	if (!sample(&b)) {
		goto done;
	}
	row("file_readback", "CNG-HMAC-SHA256", FILES, total, total * 2, &a, &b,
	        frequency.QuadPart, phase);
	if (!phase) {
		goto done;
	}
	ok = 1;
done:
	if (!ok) {
		error = GetLastError();
	}
	if (!wf_integrity_end(&job)) {
		ok = 0;
		/* One bounded cleanup retry, without any further comparisons.
		 */
		if (!wf_integrity_end(&job)) {
			puts("# HMAC_job_cleanup=FAIL");
		}
	}
	hash_close(&hmac);
	hash_close(&sha);
	SecureZeroMemory(digest, sizeof digest);
	if (0 != p0_lab_close(&lab, !ok)) {
		ok = 0;
	}
	printf("# cleanup=%s; result=%s; win32_error=%lu; small_file_pairs=%u; "
	       "single_HMAC_job=1\n",
	        ok ? "manifested_complete" : "preserved_or_incomplete",
	        ok ? "PASS" : "FAIL", error, SMALL_FILES);
	return ok ? 0 : 1;
}

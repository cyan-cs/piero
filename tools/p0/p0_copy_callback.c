/*
 * P0-S01 evidence collector: CopyFileExW callback / destination-handle
 * characterization (hypotheses H-001..H-004).
 *
 * Not production code. It never uses PROGRESS_CANCEL: stopping is done with
 * PROGRESS_STOP only (invariant I-009). Files it creates live under --work
 * and are removed afterwards as test-fixture cleanup.
 *
 * Output (per test plan section 13): <results>/P0-S01/<run_id>/
 *   run.json environment.json  traces/callbacks.csv traces/iterations.csv
 *   artifacts/summary.csv artifacts/matrix.csv artifacts/dataset_hashes.csv
 */
#include <windows.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <winioctl.h>

#include "p0_area.h"
#include "p0_common.h"
#include "../../src/base/wf_macro.h"

#define VERSION "1.0"
#define TEST_ID "P0-S01"
#define ADS_NAME L":wf_test"
#define ADS_BYTES 4096

typedef enum
{
	K_PLAIN,
	K_READONLY,
	K_ADS,
	K_SPARSE,
	K_COMPRESSED,
	K_COLLISION, /* destination pre-exists: copy must fail, dest untouched
	              */
	K_STOP /* PROGRESS_STOP when the callback with index stop_at fires */
} Kind;

typedef struct
{
	const char *name;
	Kind kind;
	uint64_t size;
	int iterations;
	int stop_at; /* K_STOP: callback index that returns PROGRESS_STOP */
} Case;

/* Plan: >= 100 runs for the 0-byte and 1-byte cases. The rest are run 100x
 * too unless the data volume makes that silly. */
static const Case CASES[] = {
        {"zero", K_PLAIN, 0, 100, -1},
        {"one_byte", K_PLAIN, 1, 100, -1},
        {"mib1", K_PLAIN, 1ull << 20, 100, -1},
        {"readonly_mib1", K_READONLY, 1ull << 20, 100, -1},
        {"ads_mib1", K_ADS, 1ull << 20, 100, -1},
        {"sparse_8mib", K_SPARSE, 8ull << 20, 100, -1},
        {"compressed_mib1", K_COMPRESSED, 1ull << 20, 100, -1},
        {"gib1", K_PLAIN, 1ull << 30, 3, -1},
        {"collision", K_COLLISION, 4096, 100, -1},
        /* PROGRESS_STOP at different points (never PROGRESS_CANCEL). What is
	 * left behind decides how a stopped copy can be cleaned up (I-009). */
        {"stop_cb0_mib1", K_STOP, 1ull << 20, 20, 0},
        {"stop_cb1_mib1", K_STOP, 1ull << 20, 20, 1},
        {"stop_cb0_64mib", K_STOP, 64ull << 20, 20, 0},
        {"stop_cb1_64mib", K_STOP, 64ull << 20, 20, 1},
        {"stop_cb4_64mib", K_STOP, 64ull << 20, 20, 4},
        {"stop_cb16_64mib", K_STOP, 64ull << 20, 20, 16},
};

static FILE *g_cb; /* callbacks.csv */
static FILE *g_it; /* iterations.csv */
static char g_run_id[64];

typedef struct
{
	int null_handle;
	DWORD type, type_err;
	int id_ok;
	DWORD id_err;
	char id_hex[64];
	int info_ok;
	DWORD info_err;
	DWORD attrs, nlinks;
	uint64_t size;
} HProbe;

typedef struct
{
	Kind kind;
	wchar_t dst[MAX_PATH * 2];
	const char *case_name;
	int iteration;

	int cb_count;
	DWORD first_reason;
	int first_dst_id_ok;
	int first_valid_idx; /* callback index of the first valid dest FileId,
	                        -1 */
	char first_id[64];
	int id_stable; /* every valid dest FileId equals the first one */
	int src_id_ok_all;
	HANDLE first_dst_handle;
	int dst_handle_same; /* the same HANDLE value on every callback */
	int stopped;
	int stop_at;
} Trace;

static const char *hex_of_id(const FILE_ID_INFO *f, char *out, size_t cap)
{
	size_t i;
	int n = snprintf(out, cap,
	        "%016llx:", (unsigned long long)f->VolumeSerialNumber);

	for (i = 0; i < 16 && (size_t)n + 3 < cap; i++) {
		n += snprintf(out + n, cap - (size_t)n, "%02x",
		        f->FileId.Identifier[i]);
	}
	return out;
}

static void probe_handle(HANDLE h, HProbe *p)
{
	FILE_ID_INFO fid;
	BY_HANDLE_FILE_INFORMATION bi;

	memset(p, 0, sizeof *p);
	if (NULL == h || INVALID_HANDLE_VALUE == h) {
		p->null_handle = 1;
		return;
	}
	SetLastError(0);
	p->type = GetFileType(h);
	if (FILE_TYPE_UNKNOWN == p->type) {
		p->type_err = GetLastError();
	}
	if (GetFileInformationByHandleEx(h, FileIdInfo, &fid, sizeof fid)) {
		p->id_ok = 1;
		hex_of_id(&fid, p->id_hex, sizeof p->id_hex);
	} else {
		p->id_err = GetLastError();
	}
	if (GetFileInformationByHandle(h, &bi)) {
		p->info_ok = 1;
		p->attrs = bi.dwFileAttributes;
		p->nlinks = bi.nNumberOfLinks;
		p->size = ((uint64_t)bi.nFileSizeHigh << 32) | bi.nFileSizeLow;
	} else {
		p->info_err = GetLastError();
	}
}

static DWORD CALLBACK progress(LARGE_INTEGER total,
        LARGE_INTEGER xfer,
        LARGE_INTEGER ssize,
        LARGE_INTEGER sxfer,
        DWORD sno,
        DWORD reason,
        HANDLE hsrc,
        HANDLE hdst,
        LPVOID data)
{
	Trace *t = data;
	HProbe sp, dp;
	int dst_exists = INVALID_FILE_ATTRIBUTES != GetFileAttributesW(t->dst);
	DWORD ret = PROGRESS_CONTINUE;

	probe_handle(hsrc, &sp);
	probe_handle(hdst, &dp);

	if (0 == t->cb_count) {
		t->first_reason = reason;
		t->first_dst_id_ok = dp.id_ok;
		t->first_dst_handle = hdst;
		t->dst_handle_same = 1;
	} else if (hdst != t->first_dst_handle) {
		t->dst_handle_same = 0;
	}
	if (dp.id_ok) {
		if (t->first_valid_idx < 0) {
			t->first_valid_idx = t->cb_count;
			strcpy_s(t->first_id, sizeof t->first_id, dp.id_hex);
		} else if (0 != strcmp(t->first_id, dp.id_hex)) {
			t->id_stable = 0;
		}
	}
	if (!sp.id_ok) {
		t->src_id_ok_all = 0;
	}

	/* PROGRESS_STOP only; PROGRESS_CANCEL would make the OS delete the
	 * destination behind wfile's back (invariant I-009). */
	if (K_STOP == t->kind && t->cb_count == t->stop_at) {
		ret = PROGRESS_STOP;
		t->stopped = 1;
	}

	fprintf(g_cb,
	        "%s,%s,%d,%d,%lu,%lu,%lld,%lld,%lld,%lld,"
	        "%d,%lu,%d,%lu,"
	        "%d,%lu,%lu,%d,%lu,%s,%d,%lu,0x%lx,%llu,%lu,%d,%d,%lu\n",
	        g_run_id, t->case_name, t->iteration, t->cb_count, reason, sno,
	        total.QuadPart, xfer.QuadPart, ssize.QuadPart, sxfer.QuadPart,
	        sp.null_handle, sp.type, sp.id_ok, sp.id_ok ? 0ul : sp.id_err,
	        dp.null_handle, dp.type, dp.type_err, dp.id_ok,
	        dp.id_ok ? 0ul : dp.id_err, dp.id_ok ? dp.id_hex : "",
	        dp.info_ok, dp.info_ok ? 0ul : dp.info_err, dp.attrs,
	        (unsigned long long)dp.size, dp.nlinks, dst_exists,
	        t->dst_handle_same, ret);
	t->cb_count++;
	return ret;
}

static uint64_t rng_next(uint64_t *s)
{
	*s ^= *s >> 12;
	*s ^= *s << 25;
	*s ^= *s >> 27;
	return *s * 0x2545F4914F6CDD1Dull;
}

/* Deterministic content: the same (seed, size) always gives the same bytes. */
static int write_deterministic(
        HANDLE f, uint64_t size, uint64_t seed, P0Hash *h)
{
	static uint64_t block[(1 << 20) / 8];
	uint64_t s = seed ? seed : 0x9E3779B97F4A7C15ull;

	while (size > 0) {
		DWORD n = size > sizeof block ? (DWORD)sizeof block
		                              : (DWORD)size,
		      wrote, i;

		for (i = 0; i < (n + 7) / 8; i++) {
			block[i] = rng_next(&s);
		}
		if (!WriteFile(f, block, n, &wrote, NULL) || wrote != n) {
			return 0;
		}
		if (h) {
			p0_hash_update(h, block, n);
		}
		size -= n;
	}
	return 1;
}

static int make_source(const wchar_t *path,
        const Case *c,
        char hash[P0_HASH_HEX],
        char ads_hash[P0_HASH_HEX])
{
	HANDLE f;
	int ok = 1;

	ads_hash[0] = '\0';
	f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == f) {
		return 0;
	}

	if (K_SPARSE == c->kind) {
		DWORD ret;
		LARGE_INTEGER pos = {.QuadPart = 4ll << 20};
		static const char tail[4096] = "sparse-tail";

		DeviceIoControl(
		        f, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &ret, NULL);
		SetFilePointerEx(f, pos, NULL, FILE_BEGIN);
		WriteFile(f, tail, sizeof tail, &ret, NULL);
		pos.QuadPart = (LONGLONG)c->size;
		SetFilePointerEx(f, pos, NULL, FILE_BEGIN);
		SetEndOfFile(f);
	} else {
		if (K_COMPRESSED == c->kind) {
			USHORT fmt = COMPRESSION_FORMAT_DEFAULT;
			DWORD ret;

			DeviceIoControl(f, FSCTL_SET_COMPRESSION, &fmt,
			        sizeof fmt, NULL, 0, &ret, NULL);
		}
		ok = write_deterministic(f, c->size, 0x1234 + c->size, NULL);
	}
	CloseHandle(f);
	if (!ok) {
		return 0;
	}

	if (K_ADS == c->kind) {
		wchar_t ads[MAX_PATH * 2];
		HANDLE a;
		P0Hash *ah = p0_hash_begin();

		swprintf(ads, MAX_PATH * 2, L"%ls%ls", path, ADS_NAME);
		a = CreateFileW(ads, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		        FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE == a || !ah) {
			return 0;
		}
		ok = write_deterministic(a, ADS_BYTES, 0xADA, ah);
		CloseHandle(a);
		p0_hash_end(ah, ads_hash);
	}
	if (K_READONLY == c->kind) {
		SetFileAttributesW(path, FILE_ATTRIBUTE_READONLY);
	}

	/* the source hash is taken from disk, after all attributes are final */
	return p0_sha256_file(path, hash) && ok;
}

static P0Area *g_area;

/* Maps an absolute path inside the run's area to its relative name. */
static const wchar_t *area_rel(const wchar_t *path)
{
	const wchar_t *root = p0_area_root(g_area);
	size_t n = wcslen(root);

	return (0 == _wcsnicmp(path, root, n) && L'\\' == path[n])
	        ? path + n + 1
	        : NULL;
}

/*
 * Removes an object THIS run created. Only exact recorded paths inside the
 * run's own area are ever deleted (p0_area.h); anything else is refused.
 */
static void remove_fixture(const wchar_t *path)
{
	const wchar_t *rel = area_rel(path);

	if (!rel || INVALID_FILE_ATTRIBUTES == GetFileAttributesW(path)) {
		return;
	}
	if (!p0_area_track(g_area, rel, 0) || !p0_area_remove(g_area, rel)) {
		fprintf(stderr, "  cleanup refused for %ls: %s\n", path,
		        p0_area_last_refusal(g_area));
	}
}

typedef struct
{
	int copy_ok;
	DWORD copy_err;
	int indep_id_ok;
	char indep_id[64];
	int id_equals;
	int size_match;
	int hash_match;
	int ads_match;
	int dst_exists_after;
	DWORD dst_attrs;
	uint64_t dst_size, dst_alloc;
	int collision_untouched;
} Outcome;

static void independent_oracle(const wchar_t *dst, Outcome *o)
{
	HANDLE h = CreateFileW(dst, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, 0, NULL);
	FILE_ID_INFO fid;
	BY_HANDLE_FILE_INFORMATION bi;

	o->dst_exists_after = INVALID_HANDLE_VALUE != h;
	if (INVALID_HANDLE_VALUE == h) {
		return;
	}
	if (GetFileInformationByHandleEx(h, FileIdInfo, &fid, sizeof fid)) {
		o->indep_id_ok = 1;
		hex_of_id(&fid, o->indep_id, sizeof o->indep_id);
	}
	if (GetFileInformationByHandle(h, &bi)) {
		o->dst_attrs = bi.dwFileAttributes;
		o->dst_size =
		        ((uint64_t)bi.nFileSizeHigh << 32) | bi.nFileSizeLow;
	}
	CloseHandle(h);
	{
		DWORD hi = 0, lo = GetCompressedFileSizeW(dst, &hi);

		o->dst_alloc =
		        INVALID_FILE_SIZE == lo && NO_ERROR != GetLastError()
		        ? 0
		        : ((uint64_t)hi << 32) | lo;
	}
}

static void run_iteration(const Case *c,
        int it,
        const wchar_t *src,
        const wchar_t *dstdir,
        const char *src_hash,
        const char *ads_hash,
        uint64_t src_size,
        const char *collision_hash,
        Trace *t,
        Outcome *o)
{
	BOOL ok;

	memset(t, 0, sizeof *t);
	memset(o, 0, sizeof *o);
	t->kind = c->kind;
	t->stop_at = c->stop_at;
	t->case_name = c->name;
	t->iteration = it;
	t->first_valid_idx = -1;
	t->id_stable = 1;
	t->src_id_ok_all = 1;
	swprintf(t->dst, MAX_PATH * 2, L"%ls\\%hs_%04d.bin", dstdir, c->name,
	        it);

	if (K_COLLISION == c->kind) {
		HANDLE f = CreateFileW(t->dst, GENERIC_WRITE, 0, NULL,
		        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);

		write_deterministic(f, 777, 0xC011, NULL);
		CloseHandle(f);
	}

	SetLastError(0);
	ok = CopyFileExW(
	        src, t->dst, progress, t, NULL, COPY_FILE_FAIL_IF_EXISTS);
	o->copy_ok = 0 != ok;
	o->copy_err = ok ? 0 : GetLastError();
	independent_oracle(t->dst, o);

	o->id_equals = t->first_valid_idx >= 0 && o->indep_id_ok &&
	        0 == strcmp(t->first_id, o->indep_id);
	o->size_match = o->dst_size == src_size;

	if (K_COLLISION == c->kind) {
		char hx[P0_HASH_HEX];

		p0_sha256_file(t->dst, hx);
		o->collision_untouched = 0 == strcmp(hx, collision_hash);
	} else if (o->copy_ok) {
		char hx[P0_HASH_HEX];

		p0_sha256_file(t->dst, hx);
		o->hash_match = 0 == strcmp(hx, src_hash);
		if (K_ADS == c->kind) {
			wchar_t ads[MAX_PATH * 2];

			swprintf(
			        ads, MAX_PATH * 2, L"%ls%ls", t->dst, ADS_NAME);
			o->ads_match = p0_sha256_file(ads, hx) &&
			        0 == strcmp(hx, ads_hash);
		}
	}

	fprintf(g_it,
	        "%s,%s,%d,%d,%lu,%d,%lu,%d,%d,%d,%s,%s,%d,%d,%d,%d,%d,0x%lx,%"
	        "llu,%llu\n",
	        g_run_id, c->name, it, o->copy_ok, o->copy_err, t->cb_count,
	        t->first_reason, t->first_dst_id_ok, t->first_valid_idx,
	        t->id_stable, t->first_valid_idx >= 0 ? t->first_id : "",
	        o->indep_id_ok ? o->indep_id : "", o->id_equals, o->size_match,
	        o->hash_match, o->ads_match,
	        K_COLLISION == c->kind ? o->collision_untouched
	                               : o->dst_exists_after,
	        o->dst_attrs, (unsigned long long)o->dst_size,
	        (unsigned long long)o->dst_alloc);
	fflush(g_it);
	fflush(g_cb);

	remove_fixture(t->dst);
}

typedef struct
{
	int runs, copy_ok, first_cb_dst_valid, id_stable, id_equals, hash_match,
	        ads_match;
	int size_match, stop_expected, partial_present, collision_ok, src_id_ok;
	int partial_id_equals, partial_full_size;
	int cb_min, cb_max, idx_min, idx_max, dst_handle_same;
	DWORD first_reason_min, first_reason_max;
} Summary;

static void accumulate(
        Summary *s, const Case *c, const Trace *t, const Outcome *o)
{
	if (0 == s->runs) {
		s->cb_min = s->idx_min = 1 << 30;
		s->cb_max = s->idx_max = -1;
		s->first_reason_min = 0xFFFFFFFF;
	}
	s->runs++;
	s->copy_ok += o->copy_ok;
	s->first_cb_dst_valid += t->first_dst_id_ok;
	s->id_stable += t->id_stable;
	s->id_equals += o->id_equals;
	s->hash_match += o->hash_match;
	s->ads_match += o->ads_match;
	s->size_match += o->size_match;
	s->src_id_ok += t->src_id_ok_all;
	s->dst_handle_same += t->dst_handle_same;
	if (t->cb_count < s->cb_min) {
		s->cb_min = t->cb_count;
	}
	if (t->cb_count > s->cb_max) {
		s->cb_max = t->cb_count;
	}
	if (t->first_valid_idx < s->idx_min) {
		s->idx_min = t->first_valid_idx;
	}
	if (t->first_valid_idx > s->idx_max) {
		s->idx_max = t->first_valid_idx;
	}
	if (t->cb_count > 0) {
		if (t->first_reason < s->first_reason_min) {
			s->first_reason_min = t->first_reason;
		}
		if (t->first_reason > s->first_reason_max) {
			s->first_reason_max = t->first_reason;
		}
	}
	if (K_STOP == c->kind) {
		int aborted =
		        !o->copy_ok && ERROR_REQUEST_ABORTED == o->copy_err;

		s->stop_expected += aborted;
		s->partial_present += aborted && o->dst_exists_after;
		s->partial_id_equals +=
		        aborted && o->dst_exists_after && o->id_equals;
		s->partial_full_size +=
		        aborted && o->dst_exists_after && o->size_match;
	}
	if (K_COLLISION == c->kind) {
		s->collision_ok += !o->copy_ok &&
		        ERROR_FILE_EXISTS == o->copy_err &&
		        o->collision_untouched;
	}
}

/* The decision a case supports, judged only from its own numbers. */
static const char *partial_policy(const Case *c, const Summary *s)
{
	if (K_STOP != c->kind) {
		return "n/a";
	}
	if (0 == s->stop_expected) {
		return "STOP_NOT_REACHED";
	}
	if (0 == s->partial_present) {
		return "DELETED_BY_OS";
	}
	return s->partial_present == s->stop_expected ? "LEFT_IN_PLACE"
	                                              : "MIXED";
}

static const char *verdict(const Case *c, const Summary *s)
{
	int n = s->runs;

	switch (c->kind) {
	case K_COLLISION: return s->collision_ok == n ? "PASS" : "FAIL";
	case K_STOP:
		/* Deterministic = every run aborted the same way (1235) and
		 * left the destination either always or never; when it is left
		 * behind, its handle-derived FileId must match an independent
		 * open. What the behaviour MEANS for cleanup is read from
		 * partial_policy. */
		if (s->stop_expected != n) {
			return "CONDITIONAL";
		}
		if (0 == s->partial_present) {
			return "PASS";
		}
		return s->partial_present == n && s->partial_id_equals == n
		        ? "PASS"
		        : "CONDITIONAL";
	default: break;
	}
	if (s->copy_ok != n || s->hash_match != n) {
		return "FAIL";
	}
	if (s->first_cb_dst_valid == n && s->id_stable == n &&
	        s->id_equals == n && s->idx_min == s->idx_max) {
		return "PASS";
	}
	/* identity is obtainable only sometimes or only later: usable, but
	 * TEMP_ID timing must then be defined by the first VALID callback and
	 * the case must be MANUAL when no valid handle was ever seen */
	return s->id_equals == n ? "CONDITIONAL" : "FAIL";
}

typedef struct
{
	wchar_t work[MAX_PATH * 2], results[MAX_PATH * 2], label[64];
	wchar_t dir[MAX_PATH * 2], srcdir[MAX_PATH * 2], dstdir[MAX_PATH * 2];
	char label8[64];
	bool quick;
	FILE *dataset; /* artifacts/dataset_hashes.csv */
	Summary sums[WF_ARRAY_LEN(CASES)];
	char verdicts[WF_ARRAY_LEN(CASES)][16];
	bool any_fail;
	int leftovers;
} Experiment;

static bool parse_options(int argc, wchar_t **argv, Experiment *x)
{
	int i;

	wcscpy_s(x->label, 64, L"win11-ntfs");
	for (i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--work-base") && i + 1 < argc) {
			wcscpy_s(x->work, MAX_PATH * 2, argv[++i]);
		} else if (!wcscmp(argv[i], L"--results") && i + 1 < argc) {
			wcscpy_s(x->results, MAX_PATH * 2, argv[++i]);
		} else if (!wcscmp(argv[i], L"--label") && i + 1 < argc) {
			wcscpy_s(x->label, 64, argv[++i]);
		} else if (!wcscmp(argv[i], L"--quick")) {
			x->quick = true;
		}
	}
	if (!x->work[0] || !x->results[0]) {
		fputs("usage: p0_copy_callback --work-base <existing dir on "
		      "the FS under test> --results <root>"
		      " [--label win11-ntfs] [--quick]\n",
		        stderr);
		return false;
	}
	return true;
}

/* Evidence directories, the run's own scratch area, environment.json and the
 * two trace CSVs with their headers. */
static int open_experiment(Experiment *x)
{
	static const wchar_t *const sub[] = {
	        L"traces", L"artifacts", L"eventlog", L"screenshots"};
	wchar_t p[MAX_PATH * 2];
	char ts[20];
	size_t i;

	p0_utc_compact(ts);
	WideCharToMultiByte(CP_UTF8, 0, x->label, -1, x->label8,
	        sizeof x->label8, NULL, NULL);
	snprintf(g_run_id, sizeof g_run_id, "%s-S01-%s%s", ts, x->label8,
	        x->quick ? "-QUICK" : "");
	swprintf(x->dir, MAX_PATH * 2, L"%ls\\%hs\\%hs", x->results, TEST_ID,
	        g_run_id);
	for (i = 0; i < WF_ARRAY_LEN(sub); i++) {
		swprintf(p, MAX_PATH * 2, L"%ls\\%ls", x->dir, sub[i]);
		p0_mkdirs(p);
	}
	/* The run's own scratch area: everything it creates lives here and only
	 * recorded objects are ever deleted (see p0_area.h). */
	g_area = p0_area_create(x->work, g_run_id);
	if (!g_area || !p0_area_mkdir(g_area, L"src") ||
	        !p0_area_mkdir(g_area, L"dst")) {
		fputs("cannot create the scratch area under --work-base\n",
		        stderr);
		return 1;
	}
	swprintf(x->srcdir, MAX_PATH * 2, L"%ls\\src", p0_area_root(g_area));
	swprintf(x->dstdir, MAX_PATH * 2, L"%ls\\dst", p0_area_root(g_area));
	swprintf(p, MAX_PATH * 2, L"%ls\\environment.json", x->dir);
	if (!p0_write_environment(
	            p, p0_area_root(g_area), "p0_copy_callback", VERSION)) {
		fputs("cannot write environment.json\n", stderr);
		return 1;
	}
	swprintf(p, MAX_PATH * 2, L"%ls\\traces\\callbacks.csv", x->dir);
	g_cb = p0_fopen_w(p);
	swprintf(p, MAX_PATH * 2, L"%ls\\traces\\iterations.csv", x->dir);
	g_it = p0_fopen_w(p);
	if (!g_cb || !g_it) {
		return 1;
	}
	fputs("run_id,case,iteration,cb_index,reason,stream_number,total_size,"
	      "total_transferred,"
	      "stream_size,stream_transferred,src_handle_null,src_handle_type,"
	      "src_fileid_ok,"
	      "src_fileid_err,dst_handle_null,dst_handle_type,dst_type_err,dst_"
	      "fileid_ok,"
	      "dst_fileid_err,dst_fileid,dst_info_ok,dst_info_err,dst_attrs,"
	      "dst_size,dst_nlinks,"
	      "dst_path_exists,dst_handle_same_as_first,return_value\n",
	        g_cb);
	fputs("run_id,case,iteration,copy_ok,copy_err,callbacks,first_reason,"
	      "first_cb_dst_fileid_ok,"
	      "first_valid_dst_cb_index,fileid_stable,handle_fileid,"
	      "independent_fileid,"
	      "handle_id_equals_independent,size_match,hash_match,ads_match,"
	      "dst_exists_or_untouched,"
	      "dst_attrs,dst_size,dst_allocated\n",
	        g_it);
	printf("run_id: %s\nwork:   %ls\nquick:  %d\n", g_run_id, x->work,
	        x->quick);
	swprintf(
	        p, MAX_PATH * 2, L"%ls\\artifacts\\dataset_hashes.csv", x->dir);
	x->dataset = p0_fopen_w(p);
	fputs("case,kind,size,iterations,source_sha256,ads_sha256\n",
	        x->dataset);
	return 0;
}

/* What a pre-existing destination will contain in the collision case. */
static void collision_reference_hash(const Experiment *x, char ch[P0_HASH_HEX])
{
	wchar_t tmp[MAX_PATH * 2];
	HANDLE f;
	P0Hash *h = p0_hash_begin();

	swprintf(tmp, MAX_PATH * 2, L"%ls\\collision_ref.bin", x->dstdir);
	f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	write_deterministic(f, 777, 0xC011, h);
	CloseHandle(f);
	p0_hash_end(h, ch);
	remove_fixture(tmp);
}

/* One case: build its source, run its iterations, judge, print. */
static void run_case(Experiment *x, int i)
{
	const Case *c = &CASES[i];
	wchar_t src[MAX_PATH * 2];
	char sh[P0_HASH_HEX], ah[P0_HASH_HEX], ch[P0_HASH_HEX] = "";
	int iters = x->quick ? (c->iterations > 5 ? 5 : c->iterations)
	                     : c->iterations,
	    it;
	WIN32_FILE_ATTRIBUTE_DATA ad;
	Trace t;
	Outcome o;
	uint64_t srcsize;

	memset(&x->sums[i], 0, sizeof x->sums[i]);
	swprintf(src, MAX_PATH * 2, L"%ls\\%hs.bin", x->srcdir, c->name);
	if (!make_source(src, c, sh, ah)) {
		printf("  %-16s SETUP FAILED (%lu)\n", c->name, GetLastError());
		strcpy_s(x->verdicts[i], sizeof x->verdicts[i], "BLOCKED");
		x->any_fail = true;
		return;
	}
	GetFileAttributesExW(src, GetFileExInfoStandard, &ad);
	srcsize = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
	if (K_COLLISION == c->kind) {
		collision_reference_hash(x, ch);
	}
	fprintf(x->dataset, "%s,%d,%llu,%d,%s,%s\n", c->name, c->kind,
	        (unsigned long long)srcsize, iters, sh, ah);
	for (it = 0; it < iters; it++) {
		run_iteration(
		        c, it, src, x->dstdir, sh, ah, srcsize, ch, &t, &o);
		accumulate(&x->sums[i], c, &t, &o);
	}
	strcpy_s(
	        x->verdicts[i], sizeof x->verdicts[i], verdict(c, &x->sums[i]));
	printf("  %-16s runs=%-3d copy_ok=%-3d dst_valid@1st=%-3d "
	       "id==indep=%-3d hash=%-3d "
	       "valid_idx=[%d..%d] cbs=[%d..%d] partial=%s -> %s\n",
	        c->name, x->sums[i].runs, x->sums[i].copy_ok,
	        x->sums[i].first_cb_dst_valid, x->sums[i].id_equals,
	        x->sums[i].hash_match, x->sums[i].idx_min, x->sums[i].idx_max,
	        x->sums[i].cb_min, x->sums[i].cb_max,
	        partial_policy(c, &x->sums[i]), x->verdicts[i]);
	remove_fixture(src);
	if (strcmp(x->verdicts[i], "PASS")) {
		x->any_fail = true;
	}
}

static void write_summary(const Experiment *x)
{
	wchar_t p[MAX_PATH * 2];
	FILE *sum;
	int i;

	swprintf(p, MAX_PATH * 2, L"%ls\\artifacts\\summary.csv", x->dir);
	sum = p0_fopen_w(p);
	fputs("case,runs,copy_ok,first_cb_dst_fileid_ok,fileid_stable,handle_"
	      "id_equals_independent,"
	      "hash_match,ads_match,size_match,src_fileid_ok_all,dst_handle_"
	      "same,cb_min,cb_max,"
	      "first_valid_idx_min,first_valid_idx_max,first_reason_min,first_"
	      "reason_max,"
	      "stop_aborted_1235,partial_present,partial_id_equals,partial_"
	      "full_size,"
	      "collision_ok,partial_policy,verdict\n",
	        sum);
	for (i = 0; i < (int)(WF_ARRAY_LEN(CASES)); i++) {
		const Summary *s = &x->sums[i];

		fprintf(sum,
		        "%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%lu,%lu,%"
		        "d,%d,%d,%d,%d,%s,%s\n",
		        CASES[i].name, s->runs, s->copy_ok,
		        s->first_cb_dst_valid, s->id_stable, s->id_equals,
		        s->hash_match, s->ads_match, s->size_match,
		        s->src_id_ok, s->dst_handle_same, s->cb_min, s->cb_max,
		        s->idx_min, s->idx_max, s->first_reason_min,
		        s->first_reason_max, s->stop_expected,
		        s->partial_present, s->partial_id_equals,
		        s->partial_full_size, s->collision_ok,
		        partial_policy(&CASES[i], s), x->verdicts[i]);
	}
	fclose(sum);
}

/* Only what this run measured is listed as TESTED. Everything else in the
 * plan's matrix is NOT TESTED until its environment exists. */
static void write_matrix(const Experiment *x)
{
	wchar_t p[MAX_PATH * 2];
	FILE *mat;

	swprintf(p, MAX_PATH * 2, L"%ls\\artifacts\\matrix.csv", x->dir);
	mat = p0_fopen_w(p);
	fputs("environment,status,note\n", mat);
	fprintf(mat, "%s,%s,\"this run\"\n", x->label8,
	        x->quick ? "QUICK_NOT_RECORDED" : "TESTED");
	fputs("cloud_placeholder,NOT_TESTED,\"no placeholder available to this "
	      "harness; separate policy test\"\n",
	        mat);
	fclose(mat);
}

static void write_run_record(const Experiment *x)
{
	static const char *const hyp[] = {
	        "H-001", "H-002", "H-003", "H-004", NULL};
	static const char *const dec[] = {"D-021", "D-028", NULL};
	char fs[32] = "?", notes[512], bid[48], os[256], eh[P0_HASH_HEX];
	wchar_t exe[MAX_PATH], rj[MAX_PATH * 2], root[MAX_PATH], fsw[32] = L"";

	GetModuleFileNameW(NULL, exe, MAX_PATH);
	p0_sha256_file(exe, eh);
	snprintf(bid, sizeof bid, "p0_copy_callback-%s-%.12s", VERSION, eh);
	p0_os_summary(os, sizeof os);
	snprintf(notes, sizeof notes,
	        "Single environment (%s, label %s). Other matrix members NOT "
	        "TESTED. Cloud placeholder NOT TESTED. PROGRESS_CANCEL never "
	        "used. Scratch-area leftovers: %d.%s",
	        os, x->label8, x->leftovers,
	        x->quick ? " QUICK RUN - NOT PLAN EVIDENCE." : "");
	if (GetVolumePathNameW(x->work, root, MAX_PATH) && /* work = the base
	                                                      dir */
	        GetVolumeInformationW(
	                root, NULL, 0, NULL, NULL, NULL, fsw, 32)) {
		WideCharToMultiByte(
		        CP_UTF8, 0, fsw, -1, fs, sizeof fs, NULL, NULL);
	}
	swprintf(rj, MAX_PATH * 2, L"%ls\\run.json", x->dir);
	p0_write_run_json(rj, TEST_ID, g_run_id, bid, fs, hyp,
	        x->quick ? "BLOCKED" : (x->any_fail ? "FAIL" : "PASS"), dec,
	        notes);
}

int wmain(int argc, wchar_t **argv)
{
	Experiment x;
	wchar_t cleanup_report[1024] = L"";
	int i, status;

	memset(&x, 0, sizeof x);
	if (!parse_options(argc, argv, &x)) {
		return 2;
	}
	if (0 != (status = open_experiment(&x))) {
		return status;
	}
	for (i = 0; i < (int)(WF_ARRAY_LEN(CASES)); i++) {
		run_case(&x, i);
	}
	fclose(x.dataset);
	fclose(g_cb);
	fclose(g_it);
	write_summary(&x);
	write_matrix(&x);

	/* the run's area: recorded objects only; strays are reported, never
	 * deleted */
	x.leftovers = p0_area_destroy(g_area, cleanup_report, 1024);
	g_area = NULL;
	printf("scratch area cleanup: %d leftover(s)%ls%ls\n", x.leftovers,
	        x.leftovers ? L": " : L"", cleanup_report);
	if (x.leftovers) {
		x.any_fail = true;
	}
	write_run_record(&x);
	printf("result: %s\nevidence: %ls\n",
	        x.any_fail ? "NOT ALL PASS" : "PASS", x.dir);
	return x.any_fail ? 1 : 0;
}

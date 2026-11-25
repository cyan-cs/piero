/*
 * P0-S01 follow-up: what exactly does CopyFileExW leave behind after the
 * progress routine returns PROGRESS_STOP, and who removes it?
 *
 * Never uses PROGRESS_CANCEL. Every file it creates lives in a per-run scratch
 * area (p0_area.h); nothing outside it is touched, nothing is deleted by
 * pattern.
 *
 * For every (file kind, flags, stop point) it repeats the copy and records:
 *   the callback trace, CopyFileExW's return and GetLastError, whether the
 *   destination exists afterwards, its size / allocated size / FileId (and
 *   whether that FileId equals the one the callback saw on the handle), its
 *   content (prefix equal to the source? rest all zero?), its ADS, and a
 *   timeline of directory-change notifications to place any disappearance
 *   inside the CopyFileExW call (the harness itself deletes nothing until it
 *   has recorded the state).
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "p0_area.h"
#include "p0_common.h"
#include "../../src/base/wf_macro.h"

/* stream notifications, absent from some SDK headers */
#ifndef FILE_NOTIFY_CHANGE_STREAM_NAME
#	define FILE_NOTIFY_CHANGE_STREAM_NAME 0x00000200
#	define FILE_NOTIFY_CHANGE_STREAM_SIZE 0x00000400
#	define FILE_NOTIFY_CHANGE_STREAM_WRITE 0x00000800
#endif

#define VERSION "1.0"
#define TEST_ID "P0-S01"
#define ADS_NAME L":wf_test"
#define ADS_BYTES 4096

typedef enum
{
	K_ZERO,
	K_ONE,
	K_MIB,
	K_BIG,
	K_ADS
} Kind;

static const struct
{
	const char *name;
	Kind kind;
	uint64_t size;
} KINDS[] = {
        {"zero_byte", K_ZERO, 0},
        {"one_byte", K_ONE, 1},
        {"mib1", K_MIB, 1u << 20},
        {"mib64", K_BIG, 64u << 20},
        {"ads_mib1", K_ADS, 1u << 20},
};

/* stop_at: callback index that returns PROGRESS_STOP; -1 = never (control) */
static const int STOPS[] = {-1, 0, 1, 2, 3, 4, 8, 16};

static const struct
{
	const char *name;
	DWORD flags;
	int exploratory; /* not the flag set the design uses */
} FLAGSETS[] = {
        {"fail_if_exists", COPY_FILE_FAIL_IF_EXISTS, 0},
        /* Exploratory only: a restartable copy is documented to keep the
	 * partial destination on stop. Not in the design; measured so the
	 * decision record can say what it would cost. */
        {"fail_if_exists+restartable",
                COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_RESTARTABLE, 1},
};

static FILE *g_calls, *g_runs, *g_events;
static char g_run_id[96];
static LARGE_INTEGER g_freq;
static P0Area *g_area;

static double now_ms(void)
{
	LARGE_INTEGER t;

	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)g_freq.QuadPart;
}

typedef struct
{
	HANDLE dir;
	HANDLE thread;
	volatile LONG stop;
	char tag[96]; /* current run, for the log */
} Watch;

static DWORD WINAPI watch_main(LPVOID arg)
{
	Watch *w = arg;
	DWORD buf[4096];
	DWORD got;

	while (!w->stop) {
		FILE_NOTIFY_INFORMATION *n;
		BYTE *p = (BYTE *)buf;

		if (!ReadDirectoryChangesW(w->dir, buf, sizeof buf, FALSE,
		            FILE_NOTIFY_CHANGE_FILE_NAME |
		                    FILE_NOTIFY_CHANGE_SIZE |
		                    FILE_NOTIFY_CHANGE_LAST_WRITE |
		                    FILE_NOTIFY_CHANGE_STREAM_SIZE |
		                    FILE_NOTIFY_CHANGE_STREAM_WRITE |
		                    FILE_NOTIFY_CHANGE_STREAM_NAME,
		            &got, NULL, NULL)) {
			break;
		}
		if (0 == got) {
			continue;
		}
		do {
			char name[260] = "";
			const char *what[] = {"?", "ADDED", "REMOVED",
			        "MODIFIED", "RENAMED_OLD", "RENAMED_NEW"};

			n = (FILE_NOTIFY_INFORMATION *)p;
			WideCharToMultiByte(CP_UTF8, 0, n->FileName,
			        (int)(n->FileNameLength / 2), name,
			        sizeof name - 1, NULL, NULL);
			fprintf(g_events, "%.3f,%s,%s,%s\n", now_ms(), w->tag,
			        n->Action < 6 ? what[n->Action] : "?", name);
			p += n->NextEntryOffset;
		} while (n->NextEntryOffset);
		fflush(g_events);
	}
	return 0;
}

typedef struct
{
	const char *kind, *flags;
	int rep, stop_at;
	wchar_t dst[MAX_PATH * 2];

	int cb_count;
	int stopped_at_cb; /* index that returned STOP, -1 */
	double t_stop_returned;
	char stop_id[64]; /* FileId seen on the handle at the stop callback */
	LONGLONG transferred_at_stop;
} Trace;

static void id_of(HANDLE h, char *out, size_t cap)
{
	FILE_ID_INFO f;

	out[0] = '\0';
	if (h && INVALID_HANDLE_VALUE != h &&
	        GetFileInformationByHandleEx(h, FileIdInfo, &f, sizeof f)) {
		size_t i;
		int n = snprintf(out, cap,
		        "%016llx:", (unsigned long long)f.VolumeSerialNumber);

		for (i = 0; i < 16 && (size_t)n + 3 < cap; i++) {
			n += snprintf(out + n, cap - (size_t)n, "%02x",
			        f.FileId.Identifier[i]);
		}
	}
}

static DWORD CALLBACK progress(LARGE_INTEGER total,
        LARGE_INTEGER xfer,
        LARGE_INTEGER ssize,
        LARGE_INTEGER sxfer,
        DWORD sno,
        DWORD reason,
        HANDLE hs,
        HANDLE hd,
        LPVOID data)
{
	Trace *t = data;
	DWORD ret = PROGRESS_CONTINUE;
	char id[64];
	BY_HANDLE_FILE_INFORMATION bi = {0};
	int info_ok;

	(void)hs;
	id_of(hd, id, sizeof id);
	info_ok = hd && GetFileInformationByHandle(hd, &bi);
	if (t->stop_at >= 0 && t->cb_count == t->stop_at) {
		ret = PROGRESS_STOP; /* never PROGRESS_CANCEL (invariant I-009)
		                      */
		t->stopped_at_cb = t->cb_count;
		t->transferred_at_stop = xfer.QuadPart;
		strcpy_s(t->stop_id, sizeof t->stop_id, id);
	}
	fprintf(g_calls,
	        "%.3f,%s,%s,%s,%d,%d,%lu,%lu,%lld,%lld,%lld,%lld,%s,%d,%llu,%"
	        "lu,%lu\n",
	        now_ms(), g_run_id, t->kind, t->flags, t->rep, t->cb_count,
	        reason, sno, total.QuadPart, xfer.QuadPart, ssize.QuadPart,
	        sxfer.QuadPart, id, info_ok,
	        info_ok ? (((unsigned long long)bi.nFileSizeHigh << 32) |
	                          bi.nFileSizeLow)
	                : 0ull,
	        info_ok ? bi.dwFileAttributes : 0ul, ret);
	t->cb_count++;
	if (PROGRESS_STOP == ret) {
		t->t_stop_returned = now_ms();
	}
	return ret;
}

static uint64_t rng_next(uint64_t *s)
{
	*s ^= *s >> 12;
	*s ^= *s << 25;
	*s ^= *s >> 27;
	return *s * 0x2545F4914F6CDD1Dull;
}

static int write_det(HANDLE f, uint64_t size, uint64_t seed)
{
	static uint64_t block[(1 << 20) / 8];
	uint64_t s = seed;

	while (size) {
		DWORD n = size > sizeof block ? (DWORD)sizeof block
		                              : (DWORD)size,
		      wr, i;

		for (i = 0; i < (n + 7) / 8; i++) {
			block[i] = rng_next(&s);
		}
		if (!WriteFile(f, block, n, &wr, NULL) || wr != n) {
			return 0;
		}
		size -= n;
	}
	return 1;
}

static int make_source(
        const wchar_t *path, Kind k, uint64_t size, char hash[P0_HASH_HEX])
{
	HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);

	if (INVALID_HANDLE_VALUE == f) {
		return 0;
	}
	if (!write_det(f, size, 0xFEED0000ull + size)) {
		CloseHandle(f);
		return 0;
	}
	CloseHandle(f);
	if (K_ADS == k) {
		wchar_t ads[MAX_PATH * 2];

		swprintf(ads, MAX_PATH * 2, L"%ls%ls", path, ADS_NAME);
		f = CreateFileW(ads, GENERIC_WRITE, 0, NULL, CREATE_NEW,
		        FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE == f ||
		        !write_det(f, ADS_BYTES, 0xADA)) {
			if (INVALID_HANDLE_VALUE != f) {
				CloseHandle(f);
			}
			return 0;
		}
		CloseHandle(f);
	}
	return p0_sha256_file(path, hash);
}

typedef struct
{
	int copy_ok;
	DWORD copy_err;
	int exists;
	uint64_t size, alloc;
	char indep_id[64];
	int id_matches_callback;
	int prefix_equal;  /* first `transferred` bytes equal the source's */
	int rest_all_zero; /* everything after that is zero */
	int content_equal; /* whole file equals the source */
	int ads_present;
	int ads_equal;
	double removed_event_ms; /* receipt time of a REMOVED event, or 0 */
} Outcome;

static void inspect(const wchar_t *src,
        const wchar_t *dst,
        Kind kind,
        const Trace *t,
        Outcome *o,
        const char *src_hash,
        const char *ads_hash)
{
	HANDLE h = CreateFileW(dst, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, 0, NULL);
	BY_HANDLE_FILE_INFORMATION bi;

	o->exists = INVALID_HANDLE_VALUE != h;
	if (!o->exists) {
		return;
	}
	id_of(h, o->indep_id, sizeof o->indep_id);
	if (GetFileInformationByHandle(h, &bi)) {
		o->size = ((uint64_t)bi.nFileSizeHigh << 32) | bi.nFileSizeLow;
	}
	CloseHandle(h);
	{
		DWORD hi = 0, lo = GetCompressedFileSizeW(dst, &hi);

		o->alloc =
		        (INVALID_FILE_SIZE == lo && NO_ERROR != GetLastError())
		        ? 0
		        : ((uint64_t)hi << 32) | lo;
	}
	o->id_matches_callback =
	        t->stop_id[0] && !strcmp(t->stop_id, o->indep_id);

	/* content: compare with the source in 1 MiB blocks */
	{
		HANDLE a = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ, NULL,
		        OPEN_EXISTING, 0, NULL);
		HANDLE b = CreateFileW(dst, GENERIC_READ,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        0, NULL);
		static BYTE ba[1 << 20], bb[1 << 20];
		uint64_t pos = 0,
		         limit = (uint64_t)(t->transferred_at_stop > 0
		                         ? t->transferred_at_stop
		                         : 0);
		int prefix_ok = 1, zero_ok = 1, full_eq = (0 != o->size || 1);
		DWORD ga, gb;

		if (INVALID_HANDLE_VALUE != a && INVALID_HANDLE_VALUE != b) {
			while (ReadFile(a, ba, sizeof ba, &ga, NULL) && ga) {
				DWORD i;

				if (!ReadFile(b, bb, ga, &gb, NULL)) {
					gb = 0;
				}
				if (gb < ga) {
					memset(bb + gb, 0, ga - gb),
					        full_eq = 0;
				}
				for (i = 0; i < ga; i++, pos++) {
					if (ba[i] != bb[i]) {
						full_eq = 0;
						if (pos < limit) {
							prefix_ok = 0;
						}
					}
					if (pos >= limit && 0 != bb[i]) {
						zero_ok = 0;
					}
				}
			}
			o->prefix_equal = prefix_ok;
			o->rest_all_zero = zero_ok;
			o->content_equal = full_eq && o->size == pos;
		}
		if (INVALID_HANDLE_VALUE != a) {
			CloseHandle(a);
		}
		if (INVALID_HANDLE_VALUE != b) {
			CloseHandle(b);
		}
	}
	(void)src_hash;
	if (K_ADS == kind) {
		wchar_t ads[MAX_PATH * 2];
		char hx[P0_HASH_HEX];

		swprintf(ads, MAX_PATH * 2, L"%ls%ls", dst, ADS_NAME);
		o->ads_present =
		        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(ads);
		o->ads_equal = o->ads_present && p0_sha256_file(ads, hx) &&
		        !strcmp(hx, ads_hash);
	}
}

static const char *outcome_class(
        const Outcome *o, int stopped, uint64_t src_size)
{
	if (!stopped) {
		return o->copy_ok && o->exists && o->content_equal
		        ? "COMPLETED"
		        : "COMPLETED_UNVERIFIED";
	}
	if (o->copy_ok) {
		return "STOP_IGNORED_COMPLETED";
	}
	if (!o->exists) {
		return "DELETED_BY_OS";
	}
	if (o->size == src_size && o->rest_all_zero && o->prefix_equal) {
		return o->content_equal ? "LEFT_COMPLETE_CONTENT"
		                        : "LEFT_FULL_SIZE_UNWRITTEN_TAIL";
	}
	return "LEFT_PARTIAL";
}

typedef struct
{
	int runs;
	int aborted; /* CopyFileExW failed with ERROR_REQUEST_ABORTED */
	int exists;
	int id_matches;
	int tail_zero;
	int removed_inside_call; /* REMOVED event between stop-return and
	                            CopyFileExW return */
	char first_class[40];
	int classes_differ;
	uint64_t size_min, size_max;
	int cb_min, cb_max;
} Group;

typedef struct
{
	wchar_t base[MAX_PATH * 2], results[MAX_PATH * 2], label[48];
	wchar_t dir[MAX_PATH * 2], srcdir[MAX_PATH * 2], dstdir[MAX_PATH * 2];
	char label8[48];
	bool quick, any_nondeterministic;
	int reps;
	Watch w;
	FILE *sum; /* artifacts/summary.csv */
} Experiment;

static bool parse_options(int argc, wchar_t **argv, Experiment *x)
{
	int i;

	wcscpy_s(x->label, 48, L"win11-ntfs");
	x->reps = 20;
	for (i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--work-base") && i + 1 < argc) {
			wcscpy_s(x->base, MAX_PATH * 2, argv[++i]);
		} else if (!wcscmp(argv[i], L"--results") && i + 1 < argc) {
			wcscpy_s(x->results, MAX_PATH * 2, argv[++i]);
		} else if (!wcscmp(argv[i], L"--label") && i + 1 < argc) {
			wcscpy_s(x->label, 48, argv[++i]);
		} else if (!wcscmp(argv[i], L"--reps") && i + 1 < argc) {
			x->reps = _wtoi(argv[++i]);
		} else if (!wcscmp(argv[i], L"--quick")) {
			x->quick = true;
		}
	}
	if (!x->base[0] || !x->results[0]) {
		fputs("usage: p0_stop_repro --work-base <existing dir> "
		      "--results <root> [--label x] [--reps n] [--quick]\n",
		        stderr);
		return false;
	}
	if (x->quick) {
		x->reps = 3;
	}
	return true;
}

/* Evidence directories, scratch area, the three trace CSVs with their
 * headers, the directory watcher and the summary header. */
static int open_experiment(Experiment *x)
{
	static const wchar_t *const sub[] = {
	        L"traces", L"artifacts", L"eventlog", L"screenshots"};
	wchar_t p[MAX_PATH * 2];
	char ts[20];
	int i;

	p0_utc_compact(ts);
	WideCharToMultiByte(CP_UTF8, 0, x->label, -1, x->label8,
	        sizeof x->label8, NULL, NULL);
	snprintf(g_run_id, sizeof g_run_id, "%s-S01-stop-repro-%s%s", ts,
	        x->label8, x->quick ? "-QUICK" : "");
	swprintf(x->dir, MAX_PATH * 2, L"%ls\\%hs\\%hs", x->results, TEST_ID,
	        g_run_id);
	for (i = 0; i < 4; i++) {
		swprintf(p, MAX_PATH * 2, L"%ls\\%ls", x->dir, sub[i]);
		p0_mkdirs(p);
	}
	g_area = p0_area_create(x->base, g_run_id);
	if (!g_area || !p0_area_mkdir(g_area, L"src") ||
	        !p0_area_mkdir(g_area, L"dst")) {
		fputs("cannot create the scratch area\n", stderr);
		return 1;
	}
	swprintf(x->srcdir, MAX_PATH * 2, L"%ls\\src", p0_area_root(g_area));
	swprintf(x->dstdir, MAX_PATH * 2, L"%ls\\dst", p0_area_root(g_area));
	swprintf(p, MAX_PATH * 2, L"%ls\\environment.json", x->dir);
	p0_write_environment(p, p0_area_root(g_area), "p0_stop_repro", VERSION);

	swprintf(p, MAX_PATH * 2, L"%ls\\traces\\stop_callbacks.csv", x->dir);
	g_calls = p0_fopen_w(p);
	swprintf(p, MAX_PATH * 2, L"%ls\\traces\\stop_runs.csv", x->dir);
	g_runs = p0_fopen_w(p);
	swprintf(p, MAX_PATH * 2, L"%ls\\traces\\dir_events.csv", x->dir);
	g_events = p0_fopen_w(p);
	fputs("t_ms,run_id,kind,flagset,rep,cb_index,reason,stream,total,"
	      "transferred,stream_size,stream_transferred,"
	      "dst_fileid,info_ok,dst_size,dst_attrs,return\n",
	        g_calls);
	fputs("run_id,kind,flagset,exploratory,rep,stop_at,callbacks,stopped_"
	      "at_cb,copy_ok,last_error,exists_after,"
	      "dst_size,dst_alloc,callback_fileid,independent_fileid,fileid_"
	      "equal,prefix_equal,rest_all_zero,"
	      "content_equal,ads_present,ads_equal,t_stop_returned_ms,t_copy_"
	      "returned_ms,removed_event_ms,"
	      "removed_inside_call,class\n",
	        g_runs);
	fputs("t_ms,run,action,name\n", g_events);

	x->w.dir = CreateFileW(x->dstdir, FILE_LIST_DIRECTORY,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	x->w.thread = CreateThread(NULL, 0, watch_main, &x->w, 0, NULL);

	swprintf(p, MAX_PATH * 2, L"%ls\\artifacts\\summary.csv", x->dir);
	x->sum = p0_fopen_w(p);
	fputs("kind,flagset,exploratory,stop_at,runs,aborted_1235,dest_exists,"
	      "fileid_equal,rest_all_zero,"
	      "removed_inside_call,dst_size_min,dst_size_max,callbacks_min,"
	      "callbacks_max,outcome,deterministic\n",
	        x->sum);
	return 0;
}

/* Time of the REMOVED directory event recorded for `name`, 0 when none was. */
static double removed_event_ms(const Experiment *x, const char *name)
{
	wchar_t p[MAX_PATH * 2];
	char line[512];
	FILE *ef;
	double ms = 0;

	swprintf(p, MAX_PATH * 2, L"%ls\\traces\\dir_events.csv", x->dir);
	ef = _wfsopen(p, L"rb", 0x40);
	if (!ef) {
		return 0;
	}
	while (fgets(line, sizeof line, ef)) {
		char *comma = strchr(line, ',');

		if (comma && strstr(comma + 1, name)) {
			ms = atof(line);
			break;
		}
	}
	fclose(ef);
	return ms;
}

/* One stopped copy: run it, observe the destination first, then judge, record
 * and clean up exactly what this repetition made. Returns its class. */
static const char *run_repetition(Experiment *x,
        int ki,
        int fi,
        int si,
        int rep,
        const wchar_t *src,
        const char *sh,
        const char *ah,
        Group *g)
{
	Trace t;
	Outcome o;
	wchar_t dstrel[128];
	char name[160];
	BOOL ok;
	double t_ret;
	const char *c;
	int inside;

	memset(&t, 0, sizeof t);
	memset(&o, 0, sizeof o);
	t.kind = KINDS[ki].name;
	t.flags = FLAGSETS[fi].name;
	t.rep = rep;
	t.stop_at = STOPS[si];
	t.stopped_at_cb = -1;
	swprintf(dstrel, 128, L"dst\\%hs_%d_%d_%d.bin", KINDS[ki].name, fi, si,
	        rep);
	swprintf(
	        t.dst, MAX_PATH * 2, L"%ls\\%ls", p0_area_root(g_area), dstrel);
	snprintf(x->w.tag, sizeof x->w.tag, "%s/%s/stop%d/rep%d", t.kind,
	        t.flags, t.stop_at, rep);

	SetLastError(0);
	ok = CopyFileExW(src, t.dst, progress, &t, NULL, FLAGSETS[fi].flags);
	t_ret = now_ms();
	o.copy_ok = 0 != ok;
	o.copy_err = ok ? 0 : GetLastError();
	/* observation first, then (and only then) the run's own cleanup */
	inspect(src, t.dst, KINDS[ki].kind, &t, &o, sh, ah);
	Sleep(15); /* let late directory notifications arrive */

	/* was there a REMOVED event for this name between the stop and the
	 * return? */
	snprintf(name, sizeof name, "%s/%s/stop%d/rep%d,REMOVED", t.kind,
	        t.flags, t.stop_at, rep);
	o.removed_event_ms = removed_event_ms(x, name);
	c = outcome_class(&o, t.stopped_at_cb >= 0, KINDS[ki].size);
	inside = o.removed_event_ms > 0 && t.t_stop_returned > 0 &&
	        o.removed_event_ms >= t.t_stop_returned &&
	        o.removed_event_ms <= t_ret + 25.0;
	fprintf(g_runs,
	        "%s,%s,%s,%d,%d,%d,%d,%d,%d,%lu,%d,%llu,%llu,%s,%s,%d,%d,%d,%d,"
	        "%d,%d,%.3f,%.3f,%.3f,%d,%s\n",
	        g_run_id, t.kind, t.flags, FLAGSETS[fi].exploratory, rep,
	        t.stop_at, t.cb_count, t.stopped_at_cb, o.copy_ok, o.copy_err,
	        o.exists, (unsigned long long)o.size,
	        (unsigned long long)o.alloc, t.stop_id, o.indep_id,
	        o.id_matches_callback, o.prefix_equal, o.rest_all_zero,
	        o.content_equal, o.ads_present, o.ads_equal, t.t_stop_returned,
	        t_ret, o.removed_event_ms, inside, c);
	g->removed_inside_call += inside;
	fflush(g_runs);
	fflush(g_calls);

	g->runs++;
	g->aborted += !o.copy_ok && ERROR_REQUEST_ABORTED == o.copy_err;
	g->exists += o.exists;
	g->id_matches += o.id_matches_callback;
	g->tail_zero += o.rest_all_zero;
	if (o.exists) {
		if (o.size < g->size_min) {
			g->size_min = o.size;
		}
		if (o.size > g->size_max) {
			g->size_max = o.size;
		}
	}
	if (t.cb_count < g->cb_min) {
		g->cb_min = t.cb_count;
	}
	if (t.cb_count > g->cb_max) {
		g->cb_max = t.cb_count;
	}
	if (0 == rep) {
		strcpy_s(g->first_class, sizeof g->first_class, c);
	} else if (strcmp(g->first_class, c)) {
		g->classes_differ = 1;
	}

	/* the run's own cleanup: exact recorded names in its own area */
	if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(t.dst)) {
		p0_area_track(g_area, dstrel, 0);
		p0_area_remove(g_area, dstrel);
	}
	return g->first_class;
}

/* One cell: (source kind, flag set, stop point) over all repetitions. */
static void run_cell(Experiment *x,
        int ki,
        int fi,
        int si,
        const wchar_t *src,
        const char *sh,
        const char *ah)
{
	Group g = {0};
	const char *cls = "";
	int rep;

	g.cb_min = 1 << 30;
	g.size_min = UINT64_MAX;
	for (rep = 0; rep < x->reps; rep++) {
		cls = run_repetition(x, ki, fi, si, rep, src, sh, ah, &g);
	}
	fprintf(x->sum, "%s,%s,%d,%d,%d,%d,%d,%d,%d,%d,%llu,%llu,%d,%d,%s,%s\n",
	        KINDS[ki].name, FLAGSETS[fi].name, FLAGSETS[fi].exploratory,
	        STOPS[si], g.runs, g.aborted, g.exists, g.id_matches,
	        g.tail_zero, g.removed_inside_call,
	        (unsigned long long)(UINT64_MAX == g.size_min ? 0 : g.size_min),
	        (unsigned long long)g.size_max, g.cb_min, g.cb_max, cls,
	        g.classes_differ ? "NO" : "YES");
	if (g.classes_differ) {
		x->any_nondeterministic = true;
	}
	printf("  %-10s %-26s stop@%-2d runs=%-3d aborted=%-3d exists=%-3d "
	       "fileid==%-3d removed_in_call=%-3d cbs=[%d..%d] -> %s%s\n",
	        KINDS[ki].name, FLAGSETS[fi].name, STOPS[si], g.runs, g.aborted,
	        g.exists, g.id_matches, g.removed_inside_call, g.cb_min,
	        g.cb_max, cls,
	        g.classes_differ ? "  *** NON-DETERMINISTIC ***" : "");
}

/* One source kind across every flag set and stop point. */
static void run_kind(Experiment *x, int ki)
{
	wchar_t src[MAX_PATH * 2], srcrel[96];
	char sh[P0_HASH_HEX] = "", ah[P0_HASH_HEX] = "";
	int fi, si;

	swprintf(srcrel, 96, L"src\\%hs.bin", KINDS[ki].name);
	swprintf(src, MAX_PATH * 2, L"%ls\\%ls", p0_area_root(g_area), srcrel);
	if (!make_source(src, KINDS[ki].kind, KINDS[ki].size, sh)) {
		printf("  %s: SETUP FAILED\n", KINDS[ki].name);
		return;
	}
	p0_area_track(g_area, srcrel, 0);
	if (K_ADS == KINDS[ki].kind) {
		wchar_t ads[MAX_PATH * 2];

		swprintf(ads, MAX_PATH * 2, L"%ls%ls", src, ADS_NAME);
		p0_sha256_file(ads, ah);
	}
	for (fi = 0; fi < (int)(WF_ARRAY_LEN(FLAGSETS)); fi++) {
		for (si = 0; si < (int)(WF_ARRAY_LEN(STOPS)); si++) {
			run_cell(x, ki, fi, si, src, sh, ah);
		}
	}
	p0_area_remove(g_area, srcrel);
}

static void close_experiment(Experiment *x)
{
	fclose(x->sum);
	x->w.stop = 1;
	CancelIoEx(x->w.dir, NULL);
	WaitForSingleObject(x->w.thread, 2000);
	CloseHandle(x->w.thread);
	CloseHandle(x->w.dir);
	fclose(g_calls);
	fclose(g_runs);
	fclose(g_events);
}

static void write_run_record(const Experiment *x, int leftovers, bool fail)
{
	static const char *const hyp[] = {"H-001", "H-002", "H-003", NULL};
	static const char *const dec[] = {"D-021", "D-028", NULL};
	char notes[600], bid[64], os[256], eh[P0_HASH_HEX];
	wchar_t exe[MAX_PATH], p[MAX_PATH * 2];

	GetModuleFileNameW(NULL, exe, MAX_PATH);
	p0_sha256_file(exe, eh);
	snprintf(bid, sizeof bid, "p0_stop_repro-%s-%.12s", VERSION, eh);
	p0_os_summary(os, sizeof os);
	snprintf(notes, sizeof notes,
	        "Characterization of PROGRESS_STOP (never PROGRESS_CANCEL) on "
	        "%s, NTFS only; other FS NOT TESTED. Per (kind, flagset, stop "
	        "point) the outcome is %s across %d reps. Scratch-area "
	        "leftovers: %d. Result is PASS when each cell is "
	        "deterministic; the outcomes themselves are the finding (see "
	        "summary.csv and docs/decision-records).%s",
	        os,
	        x->any_nondeterministic ? "NOT deterministic" : "deterministic",
	        x->reps, leftovers,
	        x->quick ? " QUICK RUN - NOT PLAN EVIDENCE." : "");
	swprintf(p, MAX_PATH * 2, L"%ls\\run.json", x->dir);
	p0_write_run_json(p, TEST_ID, g_run_id, bid, "NTFS", hyp,
	        x->quick ? "BLOCKED" : (fail ? "FAIL" : "PASS"), dec, notes);
}

int wmain(int argc, wchar_t **argv)
{
	Experiment x;
	wchar_t rep_buf[1024] = L"";
	int ki, status, leftovers;
	bool fail;

	QueryPerformanceFrequency(&g_freq);
	memset(&x, 0, sizeof x);
	if (!parse_options(argc, argv, &x)) {
		return 2;
	}
	if (0 != (status = open_experiment(&x))) {
		return status;
	}
	printf("run_id: %s  reps=%d\n", g_run_id, x.reps);
	for (ki = 0; ki < (int)(WF_ARRAY_LEN(KINDS)); ki++) {
		run_kind(&x, ki);
	}
	close_experiment(&x);
	leftovers = p0_area_destroy(g_area, rep_buf, 1024);
	printf("scratch area cleanup: %d leftover(s)%ls%ls\n", leftovers,
	        leftovers ? L": " : L"", rep_buf);
	fail = x.any_nondeterministic || leftovers;
	write_run_record(&x, leftovers, fail);
	printf("result: %s\nevidence: %ls\n",
	        fail ? "NOT DETERMINISTIC / LEFTOVERS" : "PASS (deterministic)",
	        x.dir);
	return fail ? 1 : 0;
}

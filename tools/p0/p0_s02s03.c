/*
 * P0-S02 (INTENT durability ordering) and P0-S03 (TEMP_ID acquisition window),
 * by process-crash injection.
 *
 * Each repetition runs p0_copy_op in its own process, which kills itself at an
 * exact named point. The harness then runs the READ-ONLY recovery scanner on
 * what is left, under two models of what the journal contains:
 *
 *   A  "process crash":  the journal exactly as the file is (the OS cache
 *                         survives a killed process).
 *   B  "tail lost":      only the prefix that had crossed FlushFileBuffers, as
 *                         the child's trace recorded it. A deterministic
 *                         stand-in for losing the unflushed tail. It is NOT a
 *                         power-loss test (that is P0-S14); it is what lets the
 *                         ordering rules be checked without pulling a plug.
 *
 * The oracles are in the Pt table below, taken from the plan's S02/S03 text.
 */
#include <windows.h>

#include <bcrypt.h>
#include <share.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "p0_area.h"
#include "p0_common.h"
#include "p0_journal.h"
#include "base/wf_macro.h"

#define VERSION "1.0"
#define CRASH_EXIT_CODE 0xC0DE0001u

typedef struct
{
	const char *test;  /* "P0-S02" or "P0-S03" */
	const char *point; /* crash point, or "NONE" for the no-crash control */
	int expect_txn;
	int expect_payload;     /* -1: don't care */
	int intent_a, intent_b; /* expected INTENT visibility in each model */
	P0ScanClass cls_a, cls_b;
} Pt;

#define C_END P0S_END
#define C_MAN P0S_MANUAL
#define C_AUTO P0S_AUTO_CANDIDATE

static const Pt PTS[] = {
        /* S02: no mutation before a durable INTENT */
        {"P0-S02", "BEFORE_INTENT_WRITE", 0, 0, 0, 0, C_END, C_END},
        {"P0-S02", "AFTER_INTENT_WRITE_BEFORE_FLUSH", 0, 0, 1, 0, C_END, C_END},
        {"P0-S02", "AFTER_INTENT_FLUSH", 0, 0, 1, 1, C_END, C_END},
        {"P0-S02", "BEFORE_TXN_CREATE", 0, 0, 1, 1, C_END, C_END},
        {"P0-S02", "AFTER_TXN_CREATE", 1, 0, 1, 1, C_MAN, C_MAN},
        {"P0-S02", "NONE", 1, 1, 1, 1, C_AUTO, C_AUTO},
        /* S03: no durable TEMP_ID => MANUAL; durable TEMP_ID => may proceed */
        {"P0-S03", "AFTER_TXN_CREATE", 1, 0, 1, 1, C_MAN, C_MAN},
        {"P0-S03", "AFTER_COPY_START", 1, 1, 1, 1, C_MAN, C_MAN},
        {"P0-S03", "ON_FIRST_DEST_HANDLE", 1, 1, 1, 1, C_MAN, C_MAN},
        {"P0-S03", "AFTER_ID_QUERY", 1, 1, 1, 1, C_MAN, C_MAN},
        {"P0-S03", "AFTER_TEMP_ID_WRITE", 1, 1, 1, 1, C_AUTO, C_MAN},
        {"P0-S03", "AFTER_TEMP_ID_FLUSH", 1, 1, 1, 1, C_AUTO, C_AUTO},
        {"P0-S03", "AFTER_SOURCE_ACCEPT", 1, 1, 1, 1, C_AUTO, C_AUTO},
        {"P0-S03", "BEFORE_HMAC_VERIFY", 1, 1, 1, 1, C_AUTO, C_AUTO},
        {"P0-S03", "AFTER_HMAC_VERIFY", 1, 1, 1, 1, C_AUTO, C_AUTO},
        {"P0-S03", "NONE", 1, 1, 1, 1, C_AUTO, C_AUTO},
};

typedef struct
{
	int reps, ok;
	char first_failure[300];
} Tally;

static void rm(P0Area *a, const wchar_t *rel)
{
	if (!p0_area_remove(a, rel)) {
		fprintf(stderr, "  not removed: %ls (%s)\n", rel,
		        p0_area_last_refusal(a));
	}
}

static void make_uuid(char out[40], wchar_t wout[40])
{
	uint8_t u[16];

	BCryptGenRandom(NULL, u, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	snprintf(out, 40,
	        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%"
	        "02x%02x",
	        u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9],
	        u[10], u[11], u[12], u[13], u[14], u[15]);
	MultiByteToWideChar(CP_UTF8, 0, out, -1, wout, 40);
}

static uint8_t *read_all(const wchar_t *path, size_t *n)
{
	HANDLE h = CreateFileW(path, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
	        NULL);
	LARGE_INTEGER sz;
	uint8_t *b = NULL;
	DWORD got = 0;

	*n = 0;
	if (INVALID_HANDLE_VALUE == h) {
		return NULL;
	}
	if (GetFileSizeEx(h, &sz) && sz.QuadPart < (1 << 24)) {
		b = malloc((size_t)sz.QuadPart + 1);
		if (b && ReadFile(h, b, (DWORD)sz.QuadPart, &got, NULL)) {
			*n = got;
		}
	}
	CloseHandle(h);
	return b;
}

/* Parses the child's trace; reports the facts the oracle needs. */
typedef struct
{
	int flush_seen, txn_begin_seen, intent_appended;
	int flush_before_txn; /* a JOURNAL_FLUSHED precedes TXN_CREATE_BEGIN */
	uint64_t flushed_len; /* from the last JOURNAL_FLUSHED line */
	int reached_point;
	int hmac_match, commit_blocked;
} TraceFacts;

static void read_trace(const wchar_t *path, const char *point, TraceFacts *f)
{
	FILE *fp = _wfsopen(path, L"rb", _SH_DENYNO);
	char line[256];

	memset(f, 0, sizeof *f);
	if (!fp) {
		return;
	}
	fgets(line, sizeof line, fp);
	while (fgets(line, sizeof line, fp)) {
		const char *ev = strchr(
		        strchr(line, ',') ? strchr(line, ',') + 1 : line, ',');

		ev = ev ? ev + 1 : line;
		if (!strncmp(ev, "JOURNAL_FLUSHED,flushed=", 24)) {
			f->flush_seen = 1;
			f->flushed_len = strtoull(ev + 24, NULL, 10);
		} else if (!strncmp(ev, "TXN_CREATE_BEGIN", 16)) {
			f->txn_begin_seen = 1;
			f->flush_before_txn = f->flush_seen;
		} else if (!strncmp(ev, "INTENT_APPENDED", 15)) {
			f->intent_appended = 1;
		} else if (!strncmp(ev, "HMAC_RESULT,status=0,", 21)) {
			f->hmac_match = 1;
		} else if (!strncmp(ev, "COMMIT_BLOCKED,", 15)) {
			f->commit_blocked = 1;
		} else if (!strncmp(ev, "POINT,", 6)) {
			line[strcspn(line, "\r\n")] = '\0';
			if (point && !strcmp(ev + 6, point)) {
				f->reached_point = 1;
			}
		}
	}
	fclose(fp);
}

static void hex_dump(FILE *f, const uint8_t *b, size_t n)
{
	size_t i, j;

	for (i = 0; i < n; i += 16) {
		fprintf(f, "%06zx  ", i);
		for (j = 0; j < 16; j++) {
			fprintf(f, j + i < n ? "%02x " : "   ",
			        j + i < n ? b[i + j]
			                  : 0); /* never read past n */
		}
		fputc(' ', f);
		for (j = 0; j < 16 && i + j < n; j++) {
			fputc(b[i + j] >= 32 && b[i + j] < 127 ? b[i + j] : '.',
			        f);
		}
		fputc('\n', f);
	}
}

typedef struct
{
	wchar_t dir[MAX_PATH * 2];
	FILE *runs;
	char run_id[96];
} Evidence;

static void open_evidence(Evidence *e,
        const wchar_t *results,
        const char *test,
        const char *label)
{
	char ts[20];
	wchar_t p[MAX_PATH * 2];
	static const wchar_t *const sub[] = {L"traces", L"artifacts",
	        L"eventlog", L"screenshots", L"traces\\samples"};
	int i;

	p0_utc_compact(ts);
	snprintf(e->run_id, sizeof e->run_id, "%s-%s-%s", ts, test + 3, label);
	swprintf(e->dir, MAX_PATH * 2, L"%ls\\%hs\\%hs", results, test,
	        e->run_id);
	for (i = 0; i < 5; i++) {
		swprintf(p, MAX_PATH * 2, L"%ls\\%ls", e->dir, sub[i]);
		p0_mkdirs(p);
	}
	swprintf(p, MAX_PATH * 2, L"%ls\\traces\\reps.csv", e->dir);
	e->runs = p0_fopen_w(p);
	fputs("test,point,rep,child_exit,exit_as_expected,txn_exists,payload_"
	      "exists,intent_model_a,intent_model_b,"
	      "class_model_a,class_model_b,expected_a,expected_b,txn_names,"
	      "flush_before_txn,payload_id_matches,ok,failure\n",
	        e->runs);
}

typedef struct
{
	P0Area *area;
	wchar_t child[MAX_PATH * 2], srcp[MAX_PATH * 2];
	Evidence ev[2];
	Tally tally[WF_ARRAY_LEN(PTS)];
	int reps, control_reps, rep_counter;
} Run;

/* Everything one repetition names, launches, observes and concludes. */
typedef struct
{
	wchar_t repdir[64], destrel[96], jrel[96], trel[96], uuidw[40],
	        txnrel[160], ownrel[180], payrel[180];
	wchar_t jpath[MAX_PATH * 2], tpath[MAX_PATH * 2], dpath[MAX_PATH * 2],
	        pay[MAX_PATH * 2];
	char failure[300];
	DWORD code;
	TraceFacts tf;
	P0Scan sa, sb;
	uint8_t *jb;
	size_t jn, bn;
	int ok, txn_exists, payload_exists, id_match, exit_ok;
	const char *cname_a, *cname_b;
} Trial;

static void fail(Trial *t, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(t->failure, sizeof t->failure, fmt, ap);
	va_end(ap);
	t->ok = 0;
}

/* Names and directories of this repetition. */
static void trial_prepare(Run *r, Trial *t, char uuid[40])
{
	memset(t, 0, sizeof *t);
	t->code = 0xFFFF;
	t->ok = 1;
	t->id_match = -1;
	t->cname_a = t->cname_b = "-";
	make_uuid(uuid, t->uuidw);
	swprintf(t->repdir, 64, L"r%04d", r->rep_counter++);
	swprintf(t->destrel, 96, L"%ls\\dest", t->repdir);
	swprintf(t->jrel, 96, L"%ls\\j.wjr", t->repdir);
	swprintf(t->trel, 96, L"%ls\\trace.csv", t->repdir);
	p0_area_mkdir(r->area, t->repdir);
	p0_area_mkdir(r->area, t->destrel);
	p0_area_path(r->area, t->jrel, t->jpath, MAX_PATH * 2);
	p0_area_path(r->area, t->trel, t->tpath, MAX_PATH * 2);
	p0_area_path(r->area, t->destrel, t->dpath, MAX_PATH * 2);
}

/* Runs the child (crashing at the point under test, or to completion for the
 * control). False when it could not even be started. */
static bool trial_launch(Run *r, const Pt *pt, Trial *t)
{
	wchar_t cmd[4096], pw[64] = L"";
	STARTUPINFOW si = {.cb = sizeof si};
	PROCESS_INFORMATION pr;
	bool crash = 0 != strcmp(pt->point, "NONE");

	if (crash) {
		MultiByteToWideChar(CP_UTF8, 0, pt->point, -1, pw, 64);
	}
	swprintf(cmd, 4096,
	        L"\"%ls\" --journal \"%ls\" --src \"%ls\" --dest-parent "
	        L"\"%ls\" --txn %ls --trace \"%ls\"%ls%ls",
	        r->child, t->jpath, r->srcp, t->dpath, t->uuidw, t->tpath,
	        crash ? L" --crash-at " : L"", pw);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	            NULL, NULL, &si, &pr)) {
		fail(t, "CreateProcess failed (%lu)", GetLastError());
		return false;
	}
	if (WAIT_OBJECT_0 != WaitForSingleObject(pr.hProcess, 30000)) {
		TerminateProcess(pr.hProcess, 99);
		fail(t, "child timed out");
	}
	GetExitCodeProcess(pr.hProcess, &t->code);
	CloseHandle(pr.hProcess);
	CloseHandle(pr.hThread);
	return true;
}

/* The recorded TEMP_ID must name the payload that is really on disk. */
static int payload_identity_matches(const Trial *t)
{
	HANDLE h = CreateFileW(t->pay, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	uint8_t id[P0J_ID_BYTES];
	int match = INVALID_HANDLE_VALUE != h && p0j_id_of_handle(h, id) &&
	        !memcmp(id, t->sb.temp_payload_id, P0J_ID_BYTES);

	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	return match;
}

/* What the crash left: observation only, then the run's own tracking. Scans
 * the journal as on disk (model A) and cut to the flushed length (model B). */
static void trial_observe(Run *r, const Pt *pt, Trial *t)
{
	wchar_t txn[MAX_PATH * 2];

	swprintf(t->txnrel, 160, L"%ls\\.wfile-txn-%ls", t->destrel, t->uuidw);
	swprintf(t->ownrel, 180, L"%ls\\owner.bin", t->txnrel);
	swprintf(t->payrel, 180, L"%ls\\payload.bin", t->txnrel);
	p0_area_path(r->area, t->txnrel, txn, MAX_PATH * 2);
	t->txn_exists = INVALID_FILE_ATTRIBUTES != GetFileAttributesW(txn);
	p0_area_path(r->area, t->payrel, t->pay, MAX_PATH * 2);
	t->payload_exists =
	        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(t->pay);
	read_trace(t->tpath, pt->point, &t->tf);
	t->jb = read_all(t->jpath, &t->jn);
	t->bn = t->jb ? (size_t)(t->tf.flushed_len < t->jn ? t->tf.flushed_len
	                                                   : t->jn)
	              : 0;
	if (!t->tf.flush_seen) {
		t->bn = 0; /* nothing had been flushed: model B sees an empty
		              file */
	}
	if (t->jb) {
		p0r_scan(t->jb, t->jn, t->dpath, &t->sa);
		p0r_scan(t->jb, t->bn, t->dpath, &t->sb);
	}
	if (t->sb.has_temp_id && t->payload_exists) {
		t->id_match = payload_identity_matches(t);
	}
	t->exit_ok = !strcmp(pt->point, "NONE") ? 0 == t->code
	                                        : CRASH_EXIT_CODE == t->code;
	t->cname_a = p0r_class_name(t->sa.cls);
	t->cname_b = p0r_class_name(t->sb.cls);
}

/* Compares what was observed with the oracle for this crash point. The first
 * violated rule is the failure text. */
static void trial_judge(const Pt *pt, Trial *t)
{
	bool control = !strcmp(pt->point, "NONE");

	if (t->ok && !t->exit_ok) {
		fail(t, "child exit 0x%lx not as expected", t->code);
	}
	if (t->ok && control && !(t->tf.hmac_match && t->tf.commit_blocked)) {
		fail(t, "control missing HMAC match or Storage commit block");
	}
	if (t->ok && t->txn_exists != pt->expect_txn) {
		fail(t, "txn_exists=%d expected %d", t->txn_exists,
		        pt->expect_txn);
	}
	if (t->ok && pt->expect_payload >= 0 &&
	        t->payload_exists != pt->expect_payload) {
		fail(t, "payload_exists=%d expected %d", t->payload_exists,
		        pt->expect_payload);
	}
	/* I-006 / FAIL condition of S02: a transaction object exists while no
	 * durable INTENT does */
	if (t->ok && t->txn_exists &&
	        !(t->tf.flush_before_txn && t->sb.has_intent)) {
		fail(t,
		        "I-006 VIOLATED: transaction object exists without a "
		        "durable INTENT");
	}
	if (t->ok && t->txn_exists && !t->sb.txn_names_found) {
		fail(t, "transaction exists but the name scan did not find it");
	}
	if (t->ok && t->sa.has_intent != pt->intent_a) {
		fail(t, "INTENT visible in model A=%d expected %d",
		        t->sa.has_intent, pt->intent_a);
	}
	if (t->ok && t->sb.has_intent != pt->intent_b) {
		fail(t, "INTENT visible in model B=%d expected %d",
		        t->sb.has_intent, pt->intent_b);
	}
	if (t->ok && t->sa.cls != pt->cls_a) {
		fail(t, "model A class %s expected %s (%s)", t->cname_a,
		        p0r_class_name(pt->cls_a), t->sa.reason);
	}
	if (t->ok && t->sb.cls != pt->cls_b) {
		fail(t, "model B class %s expected %s (%s)", t->cname_b,
		        p0r_class_name(pt->cls_b), t->sb.reason);
	}
	if (t->ok && 0 == t->id_match) {
		fail(t, "TEMP_ID payload FileId differs from the real payload");
	}
	/* the S03 PASS rule stated as an invariant: automatic eligibility
	 * needs a durable TEMP_ID */
	if (t->ok && P0S_AUTO_CANDIDATE == t->sb.cls && !t->sb.has_temp_id) {
		fail(t, "AUTO_CANDIDATE without a durable TEMP_ID");
	}
}

/* Sample artifacts of the first repetition of every point. */
static void trial_samples(const Evidence *e, const Pt *pt, const Trial *t)
{
	wchar_t sp[MAX_PATH * 2], pat[MAX_PATH * 2];
	WIN32_FIND_DATAW fd;
	HANDLE fh;
	FILE *f;

	swprintf(sp, MAX_PATH * 2, L"%ls\\traces\\samples\\%hs_%hs_journal.hex",
	        e->dir, pt->test + 3, pt->point);
	f = p0_fopen_w(sp);
	if (f) {
		fprintf(f,
		        "# journal bytes as on disk (model A), %zu bytes; "
		        "model "
		        "B sees the first %zu\n",
		        t->jn, t->bn);
		hex_dump(f, t->jb, t->jn);
		fclose(f);
	}
	swprintf(sp, MAX_PATH * 2, L"%ls\\traces\\samples\\%hs_%hs_listing.txt",
	        e->dir, pt->test + 3, pt->point);
	f = p0_fopen_w(sp);
	if (f) {
		fprintf(f,
		        "# read-only listing of the destination parent after "
		        "the crash\n");
		swprintf(pat, MAX_PATH * 2, L"%ls\\*", t->dpath);
		fh = FindFirstFileW(pat, &fd);
		if (INVALID_HANDLE_VALUE != fh) {
			do {
				fprintf(f, "%ls\n", fd.cFileName);
			} while (FindNextFileW(fh, &fd));
			FindClose(fh);
		}
		fprintf(f, "# scanner A: %s (%s)\n# scanner B: %s (%s)\n",
		        t->cname_a, t->sa.reason, t->cname_b, t->sb.reason);
		fclose(f);
	}
	swprintf(sp, MAX_PATH * 2, L"%ls\\traces\\samples\\%hs_%hs_trace.csv",
	        e->dir, pt->test + 3, pt->point);
	CopyFileW(t->tpath, sp, FALSE);
}

/* One row of reps.csv and the point's tally. */
static void trial_record(
        Run *r, Evidence *e, size_t pi, const Pt *pt, int rep, const Trial *t)
{
	Tally *tally = &r->tally[pi];

	fprintf(e->runs,
	        "%s,%s,%d,0x%lx,%d,%d,%d,%d,%d,%s,%s,%s,%s,%d,%d,%d,%d,\"%"
	        "s\"\n",
	        pt->test, pt->point, rep, t->code, t->exit_ok, t->txn_exists,
	        t->payload_exists, t->sa.has_intent, t->sb.has_intent,
	        t->cname_a, t->cname_b, p0r_class_name(pt->cls_a),
	        p0r_class_name(pt->cls_b), t->sa.txn_names_found,
	        t->tf.flush_before_txn, t->id_match, t->ok, t->failure);
	fflush(e->runs);
	tally->reps++;
	tally->ok += t->ok;
	if (!t->ok && !tally->first_failure[0]) {
		snprintf(tally->first_failure, sizeof tally->first_failure,
		        "rep %d: %s", rep, t->failure);
	}
}

/* The run's own cleanup: exactly what this repetition made, recorded, in
 * order, then removed in reverse (files, txn dir, dest, rep dir). */
static void trial_cleanup(Run *r, const Trial *t)
{
	p0_area_track(r->area, t->jrel, 0);
	p0_area_track(r->area, t->trel, 0);
	if (t->txn_exists) {
		p0_area_track(r->area, t->txnrel, 1);
		p0_area_track(r->area, t->ownrel, 0);
		if (t->payload_exists) {
			p0_area_track(r->area, t->payrel, 0);
		}
	}
	if (t->payload_exists) {
		rm(r->area, t->payrel);
	}
	if (t->txn_exists) {
		rm(r->area, t->ownrel);
		rm(r->area, t->txnrel);
	}
	rm(r->area, t->jrel);
	rm(r->area, t->trel);
	rm(r->area, t->destrel);
	rm(r->area, t->repdir);
}

static void run_trial(Run *r, size_t pi, int rep)
{
	const Pt *pt = &PTS[pi];
	Evidence *e = &r->ev[!strcmp(pt->test, "P0-S03")];
	Trial t;
	char uuid[40];

	trial_prepare(r, &t, uuid);
	if (trial_launch(r, pt, &t)) {
		trial_observe(r, pt, &t);
		trial_judge(pt, &t);
		if (0 == rep && t.jb) {
			trial_samples(e, pt, &t);
		}
	}
	free(t.jb);
	trial_record(r, e, pi, pt, rep, &t);
	trial_cleanup(r, &t);
}

typedef struct
{
	wchar_t base[MAX_PATH * 2], results[MAX_PATH * 2], child[MAX_PATH * 2],
	        label[48];
	int reps, control_reps;
	bool quick;
} Options;

static bool parse_options(int argc, wchar_t **argv, Options *o)
{
	int i;

	memset(o, 0, sizeof *o);
	wcscpy_s(o->label, 48, L"win11-ntfs");
	o->reps = 100;
	o->control_reps = 10;
	for (i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--work-base") && i + 1 < argc) {
			wcscpy_s(o->base, MAX_PATH * 2, argv[++i]);
		} else if (!wcscmp(argv[i], L"--results") && i + 1 < argc) {
			wcscpy_s(o->results, MAX_PATH * 2, argv[++i]);
		} else if (!wcscmp(argv[i], L"--child") && i + 1 < argc) {
			wcscpy_s(o->child, MAX_PATH * 2, argv[++i]);
		} else if (!wcscmp(argv[i], L"--label") && i + 1 < argc) {
			wcscpy_s(o->label, 48, argv[++i]);
		} else if (!wcscmp(argv[i], L"--reps") && i + 1 < argc) {
			o->reps = _wtoi(argv[++i]);
		} else if (!wcscmp(argv[i], L"--quick")) {
			o->quick = true;
		}
	}
	if (!o->base[0] || !o->results[0] || !o->child[0]) {
		fputs("usage: p0_s02s03 --work-base <dir> --results <root> "
		      "--child <p0_copy_op.exe> [--reps 100] [--quick]\n",
		        stderr);
		return false;
	}
	if (o->quick) {
		o->reps = 5;
		o->control_reps = 2;
	}
	return true;
}

/* One 4 MiB deterministic source for every repetition. */
static void write_source(P0Area *area, wchar_t srcp[MAX_PATH * 2])
{
	HANDLE h;
	static uint8_t blk[1 << 20];
	uint64_t s = 0x1234ABCDull;
	int b;
	size_t k;
	DWORD wr;

	p0_area_mkdir(area, L"src");
	p0_area_path(area, L"src\\source.bin", srcp, MAX_PATH * 2);
	h = CreateFileW(srcp, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	for (b = 0; b < 4; b++) {
		for (k = 0; k < sizeof blk; k++) {
			s ^= s >> 12;
			s ^= s << 25;
			s ^= s >> 27;
			blk[k] = (uint8_t)(s * 0x2545F4914F6CDD1Dull >> 56);
		}
		WriteFile(h, blk, sizeof blk, &wr, NULL);
	}
	CloseHandle(h);
	p0_area_track(area, L"src\\source.bin", 0);
}

/* summary.csv, environment.json and run.json of one test; returns the totals
 * through *total / *good. */
static void write_summary(
        const Run *r, int which, const Options *o, int *total, int *good)
{
	const char *test = which ? "P0-S03" : "P0-S02";
	wchar_t p[MAX_PATH * 2], exe[MAX_PATH];
	const Evidence *e = &r->ev[which];
	FILE *f;
	int k;
	char bid[64], eh[P0_HASH_HEX], notes[640], os[256];
	static const char *const hyp_s02[] = {"H-001", NULL};
	static const char *const hyp_s03[] = {"H-001", "H-011", NULL};
	static const char *const dec[] = {"D-028", "I-006", NULL};

	*total = *good = 0;
	swprintf(p, MAX_PATH * 2, L"%ls\\artifacts\\summary.csv", e->dir);
	f = p0_fopen_w(p);
	fputs("test,point,reps,ok,fail,first_failure\n", f);
	for (k = 0; k < (int)(WF_ARRAY_LEN(PTS)); k++) {
		if (strcmp(PTS[k].test, test)) {
			continue;
		}
		fprintf(f, "%s,%s,%d,%d,%d,\"%s\"\n", test, PTS[k].point,
		        r->tally[k].reps, r->tally[k].ok,
		        r->tally[k].reps - r->tally[k].ok,
		        r->tally[k].first_failure);
		*total += r->tally[k].reps;
		*good += r->tally[k].ok;
	}
	fclose(f);
	swprintf(p, MAX_PATH * 2, L"%ls\\environment.json", e->dir);
	p0_write_environment(p, p0_area_root(r->area), "p0_s02s03", VERSION);
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	p0_sha256_file(exe, eh);
	snprintf(bid, sizeof bid, "p0_s02s03-%s-%.12s", VERSION, eh);
	p0_os_summary(os, sizeof os);
	snprintf(notes, sizeof notes,
	        "%d/%d repetitions matched the oracle (process-crash "
	        "injection, %d per crash point + control) on %s, NTFS only. "
	        "Model B (tail lost = journal cut to the flushed length) is a "
	        "deterministic stand-in, NOT a power-loss test: durability "
	        "itself is P0-S14. Scanner is read-only.%s",
	        *good, *total, o->reps, os,
	        o->quick ? " QUICK RUN - NOT PLAN EVIDENCE." : "");
	swprintf(p, MAX_PATH * 2, L"%ls\\run.json", e->dir);
	p0_write_run_json(p, test, e->run_id, bid, "NTFS",
	        which ? hyp_s03 : hyp_s02,
	        o->quick ? "BLOCKED" : (*good == *total ? "PASS" : "FAIL"), dec,
	        notes);
	printf("%s: %d/%d  evidence: %ls\n", test, *good, *total, e->dir);
}

int wmain(int argc, wchar_t **argv)
{
	Options o;
	Run run;
	char label8[48], run_label[96];
	wchar_t rep_buf[2048] = L"";
	int i, leftovers;
	size_t pi;

	if (!parse_options(argc, argv, &o)) {
		return 2;
	}
	memset(&run, 0, sizeof run);
	run.reps = o.reps;
	run.control_reps = o.control_reps;
	wcscpy_s(run.child, MAX_PATH * 2, o.child);
	WideCharToMultiByte(
	        CP_UTF8, 0, o.label, -1, label8, sizeof label8, NULL, NULL);
	snprintf(run_label, sizeof run_label, "%s%s", label8,
	        o.quick ? "-QUICK" : "");
	{
		char ts[20], id[96];

		p0_utc_compact(ts);
		snprintf(id, sizeof id, "%s-S02S03-%s", ts, run_label);
		run.area = p0_area_create(o.base, id);
	}
	if (!run.area) {
		fputs("cannot create the scratch area\n", stderr);
		return 1;
	}
	open_evidence(&run.ev[0], o.results, "P0-S02", run_label);
	open_evidence(&run.ev[1], o.results, "P0-S03", run_label);
	write_source(run.area, run.srcp);

	printf("run: %s  reps=%d control=%d\n", run_label, o.reps,
	        o.control_reps);
	for (pi = 0; pi < WF_ARRAY_LEN(PTS); pi++) {
		int n = !strcmp(PTS[pi].point, "NONE") ? o.control_reps
		                                       : o.reps,
		    rep;

		for (rep = 0; rep < n; rep++) {
			run_trial(&run, pi, rep);
		}
		printf("  %-7s %-34s reps=%-3d ok=%-3d %s\n", PTS[pi].test,
		        PTS[pi].point, run.tally[pi].reps, run.tally[pi].ok,
		        run.tally[pi].ok == run.tally[pi].reps
		                ? ""
		                : run.tally[pi].first_failure);
	}
	for (i = 0; i < 2; i++) {
		int total, good;

		fclose(run.ev[i].runs);
		write_summary(&run, i, &o, &total, &good);
	}
	leftovers = p0_area_destroy(run.area, rep_buf, 2048);
	printf("scratch area cleanup: %d leftover(s)%ls%ls\n", leftovers,
	        leftovers ? L": " : L"", rep_buf);
	if (leftovers) {
		return 1;
	}
	for (i = 0; i < (int)(WF_ARRAY_LEN(PTS)); i++) {
		if (!run.tally[i].reps ||
		        run.tally[i].ok != run.tally[i].reps) {
			return 1;
		}
	}
	return 0;
}

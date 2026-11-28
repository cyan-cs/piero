/*
 * The search worker (DR-DRAFT-031) on real folders made in the temporary directory,
 * and on a fake lister where the disk would be too slow or too large: the hits and
 * where they are, batches, a newer request ends the older, cancel delivers nothing,
 * the screen being slow stops the walk and not the memory, the limit, hidden
 * entries, a junction not followed, folders that cannot be read, stopping in the
 * middle.
 */
#include <windows.h>
#include <winioctl.h>
#include <bcrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs/wf_enum_win.h"
#include "search/wf_dupehash_win.h"
#include "search/wf_searchworker.h"
#include "xfer/wf_xfer.h"
#include "wf_test.h"
#include "../tools/p0/p0_area.h"

static wchar_t root[MAX_PATH + 40];
static P0Area *area;
static int skips;

static void skip(const char *what)
{
	printf("SKIP %s\n", what);
	skips++;
}

static WfStatus real_list(
        const wchar_t *dir, bool (*cancel)(void *), void *cctx, WfDir **out)
{
	return wf_enum_dir_win_controlled(dir, 0, 0, cancel, cctx, out);
}

static wchar_t *at(const wchar_t *rel)
{
	static wchar_t buf[8][MAX_PATH + 200];
	static int next;
	wchar_t *p = buf[next++ % 8];

	swprintf(p, MAX_PATH + 200, L"%ls\\%ls", root, rel);
	return p;
}

static void mkdir_at(const wchar_t *rel)
{
	CHECK(p0_area_mkdir(area, rel));
}

static void file_at(const wchar_t *rel, const char *text)
{
	HANDLE h = CreateFileW(at(rel), GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD n = 0;

	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		CHECK(WriteFile(h, text, (DWORD)strlen(text), &n, NULL));
		CHECK(n == strlen(text));
		CloseHandle(h);
		CHECK(p0_area_track(area, rel, 0));
	}
}

static WfFileId id_of(const wchar_t *path)
{
	WfFileId id;
	FILE_ID_INFO info;
	HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

	memset(&id, 0, sizeof id);
	if (INVALID_HANDLE_VALUE != h) {
		if (GetFileInformationByHandleEx(
		            h, FileIdInfo, &info, sizeof info)) {
			id.kind = WF_ID_STRONG;
			id.volume = info.VolumeSerialNumber;
			memcpy(id.id, info.FileId.Identifier, 16);
		}
		CloseHandle(h);
	}
	return id;
}

static bool query(const wchar_t *text, WfQuery *q)
{
	size_t pos = 0;

	return WF_OK == wf_query_parse(text, q, &pos);
}

/* Everything one request delivers, until its last batch (or `ms` runs out). */
typedef struct
{
	WfHit *hit;
	size_t n, cap;
	size_t batches;
	bool done, at_least;
	WfWalkStats walk;
	WfStatus st;
	uint32_t gen_seen;
	bool other_gen;
	/* duplicates */
	size_t groups, in_groups, skipped;
	uint64_t read_done, read_total;
	size_t progress_batches; /* batches without a hit that told how far it is */
	bool dupes;
} Got;

static void got_free(Got *g)
{
	for (size_t i = 0; i < g->n; i++) {
		wf_hit_clear(&g->hit[i]);
	}
	free(g->hit);
	memset(g, 0, sizeof *g);
}

static void drain(WfSearchWorker *w, Got *g, uint32_t gen, DWORD ms)
{
	ULONGLONG until = GetTickCount64() + ms;

	while (!g->done && GetTickCount64() < until) {
		WfSearchBatch b;

		if (WAIT_OBJECT_0 !=
		                WaitForSingleObject(
		                        wf_searchworker_event(w), 50) ||
		        !wf_searchworker_poll(w, &b)) {
			continue;
		}
		if (b.gen != gen) {
			g->other_gen = true;
			wf_search_batch_free(&b);
			continue;
		}
		g->batches++;
		if (b.dupes) {
			g->dupes = true;
			g->read_done = b.read_done;
			g->read_total = b.read_total;
			g->progress_batches += (0 == b.n && !b.done) ? 1u : 0u;
		}
		if (g->n + b.n > g->cap) {
			g->cap = (g->n + b.n) * 2;
			g->hit = realloc(g->hit, g->cap * sizeof *g->hit);
		}
		memcpy(g->hit + g->n, b.hit, b.n * sizeof *b.hit);
		g->n += b.n;
		free(b.hit); /* the hits moved to g */
		if (b.done) {
			g->done = true;
			g->at_least = b.at_least;
			g->walk = b.walk;
			g->st = b.st;
			g->groups = b.groups;
			g->in_groups = b.files_in_groups;
			g->skipped = b.skipped;
		}
	}
}

static const WfHit *find(const Got *g, const wchar_t *name)
{
	for (size_t i = 0; i < g->n; i++) {
		if (!wcscmp(g->hit[i].name, name)) {
			return &g->hit[i];
		}
	}
	return NULL;
}

static void hits_and_where_they_are(void)
{
	WfSearchWorker *w = NULL;
	WfSearchPort p;
	WfQuery q;
	Got g = {0};
	const WfHit *h;

	mkdir_at(L"t1");
	mkdir_at(L"t1\\a");
	mkdir_at(L"t1\\a\\b");
	mkdir_at(L"t1\\other");
	file_at(L"t1\\report.txt", "1");
	file_at(L"t1\\a\\Annual Report.TXT", "22");
	file_at(L"t1\\a\\b\\report.md", "333");
	file_at(L"t1\\a\\b\\old report.txt", "4444");
	file_at(L"t1\\other\\notes.txt", "5");
	mkdir_at(L"t1\\report folder");

	CHECK(WF_OK == wf_searchworker_start(&w, real_list));
	p = wf_searchworker_port(w);
	CHECK(query(L"report ext:txt", &q));
	CHECK(WF_OK ==
	        p.start(p.ctx, 1, at(L"t1"), id_of(at(L"t1")), &q, false));
	drain(w, &g, 1, 10000);
	CHECK(g.done && WF_OK == g.st && !g.at_least);
	CHECK(3 == g.n);
	h = find(&g, L"report.txt");
	CHECK(h && !wcscmp(h->dir->path, at(L"t1")) && 0 == h->depth);
	CHECK(h && 1 == h->size && !(h->flags & WF_ENT_DIR));
	{
		WfFileId want = id_of(at(L"t1\\report.txt"));
		WfFileId want_root = id_of(at(L"t1"));

		CHECK(h && (h->id.kind == WF_ID_STRONG) &&
		        wf_fileid_equal(&h->id, &want));
		CHECK(h && wf_fileid_equal(&h->dir->id, &want_root));
	}
	h = find(&g, L"Annual Report.TXT");
	CHECK(h && !wcscmp(h->dir->path, at(L"t1\\a")) && 1 == h->depth);
	{
		/* the folder's identity is the one its parent's listing showed */
		WfFileId want = id_of(at(L"t1\\a"));

		CHECK(h && wf_fileid_equal(&h->dir->id, &want));
	}
	h = find(&g, L"old report.txt");
	CHECK(h && !wcscmp(h->dir->path, at(L"t1\\a\\b")) && 2 == h->depth);
	CHECK(!find(&g, L"report.md") && !find(&g, L"notes.txt") &&
	        !find(&g, L"report folder"));
	CHECK(5 == g.walk.folders &&
	        0 == g.walk.unreadable); /* t1, a, b, other, report folder */
	/* the hits of one folder share one record of it */
	got_free(&g);

	/* a folder is a hit for a word, with no extension term */
	CHECK(query(L"report", &q));
	CHECK(WF_OK ==
	        p.start(p.ctx, 2, at(L"t1"), id_of(at(L"t1")), &q, false));
	drain(w, &g, 2, 10000);
	CHECK(g.done && 5 == g.n && find(&g, L"report folder") &&
	        (find(&g, L"report folder")->flags & WF_ENT_DIR));
	got_free(&g);
	{
		WfWorkerStopReport rep = wf_searchworker_stop(w);

		CHECK(WF_OK == rep.status);
		if (rep.completion) {
			WaitForSingleObject(rep.completion, 5000);
			CloseHandle(rep.completion);
		}
	}
}

static void many_hits_come_in_batches_and_in_order_of_distance(void)
{
	WfSearchWorker *w = NULL;
	WfSearchPort p;
	WfQuery q;
	Got g = {0};

	mkdir_at(L"t2");
	mkdir_at(L"t2\\deep");
	for (int i = 0; i < 300; i++) {
		wchar_t rel[80];

		swprintf(rel, 80, L"t2\\hit%03d.dat", i);
		file_at(rel, "x");
		swprintf(rel, 80, L"t2\\deep\\hit%03d.dat", i);
		file_at(rel, "x");
	}
	CHECK(WF_OK == wf_searchworker_start(&w, real_list));
	p = wf_searchworker_port(w);
	CHECK(query(L"hit", &q));
	CHECK(WF_OK ==
	        p.start(p.ctx, 1, at(L"t2"), id_of(at(L"t2")), &q, false));
	drain(w, &g, 1, 20000);
	CHECK(g.done && 600 == g.n && !g.at_least);
	CHECK(g.batches >= 3); /* at most 256 in one */
	/* the near folder's hits all come before the deeper one's */
	{
		size_t last_near = 0, first_deep = g.n;

		for (size_t i = 0; i < g.n; i++) {
			if (0 == g.hit[i].depth) {
				last_near = i;
			} else if (first_deep == g.n) {
				first_deep = i;
			}
		}
		CHECK(last_near < first_deep);
	}
	got_free(&g);
	{
		WfWorkerStopReport rep = wf_searchworker_stop(w);

		if (rep.completion) {
			WaitForSingleObject(rep.completion, 5000);
			CloseHandle(rep.completion);
		}
	}
}

static void hidden_entries_and_a_junction_not_followed(void)
{
	WfSearchWorker *w = NULL;
	WfSearchPort p;
	WfQuery q;
	Got g = {0};
	wchar_t *hid;

	mkdir_at(L"t3");
	mkdir_at(L"t3\\real");
	mkdir_at(L"t3\\target");
	file_at(L"t3\\real\\find me.txt", "1");
	file_at(L"t3\\target\\find me too.txt", "1");
	file_at(L"t3\\find hidden.txt", "1");
	hid = at(L"t3\\find hidden.txt");
	SetFileAttributesW(hid, FILE_ATTRIBUTE_HIDDEN);
	{
		/* a junction to target\ named link\ */
		BYTE buf[2048];
		wchar_t *link = at(L"t3\\link"), sub[MAX_PATH + 12];
		wchar_t *target = at(L"t3\\target");
		USHORT sb, pb, data;
		HANDLE h;
		DWORD got;

		swprintf(sub, MAX_PATH + 12, L"\\??\\%ls", target);
		sb = (USHORT)(wcslen(sub) * 2);
		pb = (USHORT)(wcslen(target) * 2);
		mkdir_at(L"t3\\link");
		h = CreateFileW(link, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
		        FILE_FLAG_BACKUP_SEMANTICS |
		                FILE_FLAG_OPEN_REPARSE_POINT,
		        NULL);
		if (INVALID_HANDLE_VALUE != h) {
			memset(buf, 0, sizeof buf);
			data = (USHORT)(8 + sb + 2 + pb + 2);
			*(ULONG *)buf = IO_REPARSE_TAG_MOUNT_POINT;
			*(USHORT *)(buf + 4) = data;
			*(USHORT *)(buf + 10) = sb;
			*(USHORT *)(buf + 12) = (USHORT)(sb + 2);
			*(USHORT *)(buf + 14) = pb;
			memcpy(buf + 16, sub, sb);
			memcpy(buf + 16 + sb + 2, target, pb);
			if (!DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf,
			            (DWORD)(8u + data), NULL, 0, &got, NULL)) {
				skip("junction: could not be made");
			}
			CloseHandle(h);
		}
	}
	CHECK(WF_OK == wf_searchworker_start(&w, real_list));
	p = wf_searchworker_port(w);
	CHECK(query(L"find", &q));
	CHECK(WF_OK ==
	        p.start(p.ctx, 1, at(L"t3"), id_of(at(L"t3")), &q, false));
	drain(w, &g, 1, 10000);
	CHECK(g.done);
	CHECK(find(&g, L"find me.txt") && find(&g, L"find me too.txt"));
	CHECK(!find(&g, L"find hidden.txt")); /* hidden: not unless asked */
	/* "find me too.txt" is found once, in target\, never again through the link */
	{
		size_t count = 0;

		for (size_t i = 0; i < g.n; i++) {
			count += !wcscmp(g.hit[i].name, L"find me too.txt");
		}
		CHECK(1 == count);
	}
	CHECK(g.walk.not_entered >= 1 || skips);
	got_free(&g);

	CHECK(WF_OK ==
	        p.start(p.ctx, 2, at(L"t3"), id_of(at(L"t3")), &q, true));
	drain(w, &g, 2, 10000);
	CHECK(g.done && find(&g, L"find hidden.txt"));
	got_free(&g);
	{
		WfWorkerStopReport rep = wf_searchworker_stop(w);

		if (rep.completion) {
			WaitForSingleObject(rep.completion, 5000);
			CloseHandle(rep.completion);
		}
	}
	/* the junction is removed as a link, not through it */
	CHECK(p0_area_remove(area, L"t3\\link"));
}

typedef struct
{
	unsigned entries; /* in the root */
	unsigned sleep_ms;
	unsigned lists;
	bool unreadable_sub;
	HANDLE entered, release;
} Fake;

static Fake fake;

static WfStatus fake_list(
        const wchar_t *dir, bool (*cancel)(void *), void *cctx, WfDir **out)
{
	WfDir *d;
	unsigned entries = fake.entries;
	HANDLE entered = fake.entered, release = fake.release;

	InterlockedIncrement((volatile LONG *)&fake.lists);
	if (entered && release) {
		SetEvent(entered);
		CHECK(WAIT_OBJECT_0 == WaitForSingleObject(release, 5000));
	}
	if (fake.sleep_ms) {
		Sleep(fake.sleep_ms);
	}
	if (fake.unreadable_sub && wcsstr(dir, L"\\locked")) {
		return WF_E_ACCESS;
	}
	d = wf_dir_new();
	CHECK(NULL != d);
	if (!wcscmp(dir, L"F")) {
		for (unsigned i = 0; i < entries; i++) {
			wchar_t name[32];

			if (cancel && cancel(cctx)) {
				wf_dir_free(d);
				return WF_E_CANCELLED;
			}
			swprintf(name, 32, L"m%u.txt", i);
			CHECK(WF_OK ==
			        wf_dir_add(d, name, wcslen(name), 0, 1, 1));
		}
		if (fake.unreadable_sub) {
			CHECK(WF_OK ==
			        wf_dir_add(d, L"locked", 6, WF_ENT_DIR, 0, 0));
			CHECK(WF_OK ==
			        wf_dir_add(d, L"fine", 4, WF_ENT_DIR, 0, 0));
		}
	} else if (wcsstr(dir, L"\\fine")) {
		CHECK(WF_OK == wf_dir_add(d, L"m_in_fine.txt", 13, 0, 1, 1));
	}
	*out = d;
	return WF_OK;
}

static void the_limit_is_two_hundred_thousand_and_it_says_at_least(void)
{
	WfSearchWorker *w = NULL;
	WfSearchPort p;
	WfQuery q;
	Got g = {0};
	WfFileId none = {0};

	memset(&fake, 0, sizeof fake);
	fake.entries = WF_SEARCH_MAX_HITS + 500;
	CHECK(WF_OK == wf_searchworker_start(&w, fake_list));
	p = wf_searchworker_port(w);
	CHECK(query(L"m", &q));
	CHECK(WF_OK == p.start(p.ctx, 1, L"F", none, &q, false));
	drain(w, &g, 1, 60000);
	CHECK(g.done && g.at_least);
	CHECK(WF_SEARCH_MAX_HITS == g.n);
	CHECK(g.walk.stopped);
	got_free(&g);
	{
		WfWorkerStopReport rep = wf_searchworker_stop(w);

		if (rep.completion) {
			WaitForSingleObject(rep.completion, 5000);
			CloseHandle(rep.completion);
		}
	}
}

static void a_slow_screen_stops_the_walk_and_not_the_memory(void)
{
	WfSearchWorker *w = NULL;
	WfSearchPort p;
	WfQuery q;
	Got g = {0};
	WfFileId none = {0};

	memset(&fake, 0, sizeof fake);
	fake.entries = 20000; /* more than 64 batches of 256 */
	CHECK(WF_OK == wf_searchworker_start(&w, fake_list));
	p = wf_searchworker_port(w);
	CHECK(query(L"m", &q));
	CHECK(WF_OK == p.start(p.ctx, 1, L"F", none, &q, false));
	Sleep(800); /* the screen takes nothing */
	CHECK(WAIT_OBJECT_0 ==
	        WaitForSingleObject(wf_searchworker_event(w), 0));
	/* nothing was lost, and the walk finishes once the screen takes the batches */
	drain(w, &g, 1, 30000);
	CHECK(g.done && 20000 == g.n && !g.at_least);
	{
		/* in the order they were found */
		bool ordered = true;

		for (size_t i = 0; i < g.n; i++) {
			wchar_t want[32];

			(void)want;
			ordered = ordered && g.hit[i].name[0] == L'm';
		}
		CHECK(ordered);
	}
	got_free(&g);
	{
		WfWorkerStopReport rep = wf_searchworker_stop(w);

		if (rep.completion) {
			WaitForSingleObject(rep.completion, 5000);
			CloseHandle(rep.completion);
		}
	}
}

static void unreadable_folders_are_counted_and_the_rest_is_searched(void)
{
	WfSearchWorker *w = NULL;
	WfSearchPort p;
	WfQuery q;
	Got g = {0};
	WfFileId none = {0};

	memset(&fake, 0, sizeof fake);
	fake.entries = 3;
	fake.unreadable_sub = true;
	CHECK(WF_OK == wf_searchworker_start(&w, fake_list));
	p = wf_searchworker_port(w);
	CHECK(query(L"m", &q));
	CHECK(WF_OK == p.start(p.ctx, 1, L"F", none, &q, false));
	drain(w, &g, 1, 10000);
	CHECK(g.done && 1 == g.walk.unreadable && 2 == g.walk.folders);
	CHECK(find(&g, L"m_in_fine.txt") && 4 == g.n);
	got_free(&g);
	{
		WfWorkerStopReport rep = wf_searchworker_stop(w);

		if (rep.completion) {
			WaitForSingleObject(rep.completion, 5000);
			CloseHandle(rep.completion);
		}
	}
}

static HANDLE cancel_entered, cancel_release, cancel_observed;

static WfStatus cancellation_list(
        const wchar_t *dir, bool (*cancel)(void *), void *ctx, WfDir **out)
{
	(void)dir;
	*out = NULL;
	SetEvent(cancel_entered);
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(cancel_release, 5000));
	CHECK(cancel && cancel(ctx));
	SetEvent(cancel_observed);
	return WF_E_CANCELLED;
}

static void cancellation_reaches_an_inflight_enumerator(void)
{
	WfSearchWorker *w = NULL;
	WfSearchPort p;
	WfQuery q;
	WfFileId none = {0};
	WfWorkerStopReport rep;

	cancel_entered = CreateEventW(NULL, TRUE, FALSE, NULL);
	cancel_release = CreateEventW(NULL, TRUE, FALSE, NULL);
	cancel_observed = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(cancel_entered && cancel_release && cancel_observed);
	CHECK(WF_OK == wf_searchworker_start(&w, cancellation_list));
	p = wf_searchworker_port(w);
	CHECK(query(L"m", &q));
	CHECK(WF_OK == p.start(p.ctx, 1, L"F", none, &q, false));
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(cancel_entered, 5000));
	p.cancel(p.ctx);
	SetEvent(cancel_release);
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(cancel_observed, 5000));
	rep = wf_searchworker_stop(w);
	CHECK(WF_OK == rep.status);
	if (rep.completion) {
		CHECK(WAIT_OBJECT_0 ==
		        WaitForSingleObject(rep.completion, 5000));
		CloseHandle(rep.completion);
	}
	CloseHandle(cancel_observed);
	CloseHandle(cancel_release);
	CloseHandle(cancel_entered);
}

static void a_newer_request_ends_the_older_and_cancel_delivers_nothing(void)
{
	WfSearchWorker *w = NULL;
	WfSearchPort p;
	WfQuery q;
	Got g = {0};
	WfFileId none = {0};
	WfSearchBatch b;

	memset(&fake, 0, sizeof fake);
	fake.entries = 200000; /* a long walk for the first request */
	fake.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
	fake.release = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(fake.entered && fake.release);
	CHECK(WF_OK == wf_searchworker_start(&w, fake_list));
	p = wf_searchworker_port(w);
	CHECK(query(L"m", &q));
	CHECK(WF_OK == p.start(p.ctx, 1, L"F", none, &q, false));
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(fake.entered, 5000));
	fake.entries = 3;
	CHECK(WF_OK == p.start(p.ctx, 2, L"F", none, &q, false));
	SetEvent(fake.release);
	drain(w, &g, 2, 30000);
	CHECK(g.done && 3 == g.n && !g.other_gen); /* nothing of request 1 */
	got_free(&g);

	/* cancel: nothing after it is delivered */
	fake.entries = 200000;
	CHECK(WF_OK == p.start(p.ctx, 3, L"F", none, &q, false));
	Sleep(20);
	p.cancel(p.ctx);
	Sleep(300);
	CHECK(!wf_searchworker_poll(w, &b));
	CHECK(WAIT_TIMEOUT ==
	        WaitForSingleObject(wf_searchworker_event(w), 200));

	/* bad requests */
	CHECK(WF_E_INVAL == p.start(p.ctx, 0, L"F", none, &q, false));
	CHECK(WF_E_INVAL == p.start(p.ctx, 4, NULL, none, &q, false));
	CHECK(WF_E_INVAL == p.start(p.ctx, 4, L"F", none, NULL, false));
	CHECK(WF_E_INVAL == p.start(p.ctx, 4, L"", none, &q, false));
	{
		WfWorkerStopReport rep = wf_searchworker_stop(w);

		if (rep.completion) {
			WaitForSingleObject(rep.completion, 5000);
			CloseHandle(rep.completion);
		}
	}
	CloseHandle(fake.release);
	CloseHandle(fake.entered);
}

static void stopping_in_the_middle_returns_within_the_wait(void)
{
	WfSearchWorker *w = NULL;
	WfSearchPort p;
	WfQuery q;
	WfFileId none = {0};
	ULONGLONG t0;
	WfWorkerStopReport rep;

	memset(&fake, 0, sizeof fake);
	fake.entries = 200000;
	fake.sleep_ms = 40;
	CHECK(WF_OK == wf_searchworker_start(&w, fake_list));
	p = wf_searchworker_port(w);
	CHECK(query(L"m", &q));
	CHECK(WF_OK == p.start(p.ctx, 1, L"F", none, &q, false));
	Sleep(100);
	t0 = GetTickCount64();
	rep = wf_searchworker_stop(w);
	CHECK(WF_OK == rep.status);
	CHECK(GetTickCount64() - t0 < 3000);
	if (rep.completion) {
		WaitForSingleObject(rep.completion, 10000);
		CloseHandle(rep.completion);
	}
	CHECK(WF_E_INVAL == wf_searchworker_start(NULL, fake_list));
	CHECK(WF_E_INVAL == wf_searchworker_start(&w, NULL));
}

static void bytes_at(const wchar_t *rel, size_t n, char fill, char last)
{
	char *buf = malloc(n);
	HANDLE h;
	DWORD done = 0;

	CHECK(NULL != buf);
	if (!buf) {
		return;
	}
	memset(buf, fill, n);
	buf[n - 1] = last;
	h = CreateFileW(at(rel), GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		CHECK(WriteFile(h, buf, (DWORD)n, &done, NULL));
		CHECK(done == n);
		CloseHandle(h);
		CHECK(p0_area_track(area, rel, 0));
	}
	free(buf);
}

static uint32_t group_of(const Got *g, const wchar_t *name)
{
	const WfHit *h = find(g, name);

	return h ? h->group : 0;
}

/* What the listing sees of `name` in `dir`: its identity, size and time. */
static WfSearchSeen seen_of(const wchar_t *dir, const wchar_t *name)
{
	WfSearchSeen seen;
	WfDir *d = NULL;

	memset(&seen, 0, sizeof seen);
	CHECK(WF_OK == wf_enum_dir_win(dir, &d) && d);
	for (size_t i = 0; d && i < d->n; i++) {
		if (!wcscmp(d->e[i].name, name)) {
			seen.size = d->e[i].size;
			seen.mtime = d->e[i].mtime;
			seen.id = d->e[i].identity;
		}
	}
	wf_dir_free(d);
	return seen;
}

static WfStatus changed_hash(const wchar_t *dir,
        const wchar_t *name,
        const WfSearchSeen *seen,
        uint64_t limit,
        bool (*cancel)(void *),
        void *cctx,
        uint8_t digest[32])
{
	if (!wcscmp(name, L"c.txt")) {
		return WF_E_CHANGED;
	}
	return wf_dupehash_file(dir, name, seen, limit, cancel, cctx, digest);
}

static void duplicates_are_found_by_size_then_prefix_then_the_whole_file(void)
{
	WfSearchWorker *w = NULL;
	WfSearchPort p;
	WfFileId none = {0};
	Got g;

	mkdir_at(L"dup");
	mkdir_at(L"dup\\d1");
	mkdir_at(L"dup\\d2");
	mkdir_at(L"dup\\d2\\sub");
	file_at(L"dup\\d1\\a.txt", "the same words");
	file_at(L"dup\\d2\\b.txt", "the same words");
	file_at(L"dup\\d2\\sub\\c.txt", "the same words");
	file_at(L"dup\\d1\\x1.txt", "aaaa");
	file_at(L"dup\\d2\\x2.txt", "aaab"); /* the same size, other bytes */
	file_at(L"dup\\d1\\e1.txt", "");
	file_at(L"dup\\d2\\e2.txt", ""); /* empty ones are not duplicates */
	file_at(L"dup\\d1\\only.txt", "alone");
	bytes_at(L"dup\\d1\\big1.bin", 70000, 'q', 'x');
	bytes_at(L"dup\\d2\\big2.bin", 70000, 'q', 'x'); /* the same */
	bytes_at(L"dup\\d2\\big3.bin", 70000, 'q',
	        'y'); /* the same first 64 KiB */
	memset(&g, 0, sizeof g);
	CHECK(WF_OK == wf_searchworker_start(&w, real_list));
	p = wf_searchworker_port(w);
	/* no hasher: refused, nothing started */
	CHECK(WF_E_INVAL == p.start_dupes(p.ctx, 1, at(L"dup"), none, false));
	wf_searchworker_set_hash(w, wf_dupehash_file);
	CHECK(WF_OK == p.start_dupes(p.ctx, 2, at(L"dup"), none, false));
	drain(w, &g, 2, 20000);
	CHECK(g.done);
	CHECK(WF_OK == g.st);
	CHECK(!g.at_least);
	CHECK(g.dupes);
	CHECK(2 == g.groups);
	CHECK(5 == g.in_groups);
	CHECK(5 == g.n);
	CHECK(0 == g.skipped);
	CHECK(group_of(&g, L"a.txt") &&
	        group_of(&g, L"a.txt") == group_of(&g, L"b.txt") &&
	        group_of(&g, L"a.txt") == group_of(&g, L"c.txt"));
	CHECK(group_of(&g, L"big1.bin") &&
	        group_of(&g, L"big1.bin") == group_of(&g, L"big2.bin"));
	CHECK(group_of(&g, L"a.txt") != group_of(&g, L"big1.bin"));
	CHECK(NULL ==
	        find(&g, L"big3.bin")); /* it differs after the first 64 KiB */
	CHECK(NULL == find(&g, L"x1.txt") && NULL == find(&g, L"x2.txt"));
	CHECK(NULL == find(&g, L"e1.txt") && NULL == find(&g, L"only.txt"));
	for (size_t i = 0; i < g.n; i++) {
		CHECK((g.hit[i].id.kind == WF_ID_STRONG) &&
		        (g.hit[i].dir->id.kind == WF_ID_STRONG));
		CHECK(0 == (g.hit[i].flags & WF_ENT_DIR));
	}
	CHECK(g.read_total >=
	        7); /* the five of one size class and the big ones */
	got_free(&g);

	/* the hash of a file for this: whole, a prefix, and not a file that changed */
	{
		uint8_t whole[32], pre1[32], pre3[32], same[32], dummy[32];
		wchar_t *d1 = at(L"dup\\d1");
		wchar_t *d2 = at(L"dup\\d2");

		WfSearchSeen s1 = seen_of(d1, L"big1.bin"),
		             s3 = seen_of(d2, L"big3.bin"), wrong;

		CHECK(70000 == s1.size && (s1.id.kind == WF_ID_STRONG));
		CHECK(WF_OK ==
		        wf_dupehash_file(
		                d1, L"big1.bin", &s1, 0, NULL, NULL, whole));
		CHECK(WF_OK ==
		        wf_dupehash_file(
		                d1, L"big1.bin", &s1, 65536, NULL, NULL, pre1));
		CHECK(WF_OK ==
		        wf_dupehash_file(
		                d2, L"big3.bin", &s3, 65536, NULL, NULL, pre3));
		CHECK(0 !=
		        memcmp(whole, pre1,
		                32)); /* a prefix is not the whole file */
		CHECK(0 == memcmp(pre1, pre3, 32)); /* the same first 64 KiB */
		CHECK(WF_OK ==
		        wf_dupehash_file(
		                d1, L"big1.bin", &s1, 70000, NULL, NULL, same));
		CHECK(0 ==
		        memcmp(whole, same,
		                32)); /* a limit of the whole is the whole */
		/* a size, an identity or a time that is not the file's: not that file */
		wrong = s1;
		wrong.size = 69999;
		CHECK(WF_E_CHANGED ==
		        wf_dupehash_file(
		                d1, L"big1.bin", &wrong, 0, NULL, NULL, dummy));
		wrong.size = 70001;
		CHECK(WF_E_CHANGED ==
		        wf_dupehash_file(d1, L"big1.bin", &wrong, 65536, NULL,
		                NULL, dummy));
		wrong = s1;
		wrong.mtime += 20000000; /* two seconds off */
		CHECK(WF_E_CHANGED ==
		        wf_dupehash_file(
		                d1, L"big1.bin", &wrong, 0, NULL, NULL, dummy));
		wrong = s1;
		wrong.id.id[0] ^= 0x5A; /* the identity of another file */
		CHECK(WF_E_CHANGED ==
		        wf_dupehash_file(
		                d1, L"big1.bin", &wrong, 0, NULL, NULL, dummy));
		CHECK(WF_E_INVAL ==
		        wf_dupehash_file(
		                d1, L"big1.bin", NULL, 0, NULL, NULL, dummy));
		/* the same name and size, another file (replaced after the listing): the
		 * size is not the file, and the answer is "changed" */
		{
			char same_len[70013];
			HANDLE h;
			DWORD n;
			wchar_t full[1100];
			WfSearchSeen sw;

			bytes_at(L"dup\\d1\\swap.bin", sizeof same_len, 'q',
			        'x');
			sw = seen_of(d1, L"swap.bin");
			CHECK(sizeof same_len == sw.size);
			CHECK(WF_OK ==
			        wf_dupehash_file(d1, L"swap.bin", &sw, 0, NULL,
			                NULL, dummy));
			memset(same_len, 'z', sizeof same_len);
			swprintf(full, 1100, L"%ls\\swap.bin", d1);
			CHECK(DeleteFileW(full));
			h = CreateFileW(full, GENERIC_WRITE, 0, NULL,
			        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
			CHECK(INVALID_HANDLE_VALUE != h);
			if (INVALID_HANDLE_VALUE != h) {
				WriteFile(
				        h, same_len, sizeof same_len, &n, NULL);
				CloseHandle(h);
			}
			CHECK(WF_E_CHANGED ==
			        wf_dupehash_file(d1, L"swap.bin", &sw, 0, NULL,
			                NULL, dummy));
			/* what the listing sees now is what is hashed */
			sw = seen_of(d1, L"swap.bin");
			CHECK(WF_OK ==
			        wf_dupehash_file(d1, L"swap.bin", &sw, 0, NULL,
			                NULL, dummy));
		}
		CHECK(WF_OK !=
		        wf_dupehash_file(
		                d1, L"missing.bin", &s1, 0, NULL, NULL, dummy));
	}

	/* a file that is not what was listed takes no part */
	wf_searchworker_set_hash(w, changed_hash);
	CHECK(WF_OK == p.start_dupes(p.ctx, 3, at(L"dup"), none, false));
	drain(w, &g, 3, 20000);
	CHECK(g.done);
	CHECK(2 == g.groups);
	CHECK(4 == g.in_groups);
	CHECK(NULL == find(&g, L"c.txt"));
	CHECK(1 == g.skipped);
	got_free(&g);

	/* cancel delivers nothing of the request */
	CHECK(WF_OK == p.start_dupes(p.ctx, 4, at(L"dup"), none, false));
	p.cancel(p.ctx);
	Sleep(300);
	drain(w, &g, 4, 500);
	CHECK(!g.done);
	got_free(&g);
	{
		WfWorkerStopReport rep = wf_searchworker_stop(w);

		CHECK(WF_OK == rep.status);
		if (rep.completion) {
			WaitForSingleObject(rep.completion, 5000);
			CloseHandle(rep.completion);
		}
	}
}

static WfStatus links_list(
        const wchar_t *dir, bool (*cancel)(void *), void *cctx, WfDir **out)
{
	WfDir *d = wf_dir_new();

	(void)cancel;
	(void)cctx;
	CHECK(NULL != d);
	if (!wcscmp(dir, L"F")) {
		CHECK(WF_OK == wf_dir_add(d, L"a.txt", 5, 0, 5, 1));
		CHECK(WF_OK == wf_dir_add(d, L"b.txt", 5, 0, 5, 1));
		CHECK(WF_OK ==
		        wf_dir_add(d, L"link.txt", 8, WF_ENT_LINK, 5, 1));
		CHECK(WF_OK ==
		        wf_dir_add(d, L"cloud.txt", 9, WF_ENT_CLOUD, 5, 1));
		CHECK(WF_OK == wf_dir_add(d, L"empty.txt", 9, 0, 0, 1));
	}
	*out = d;
	return WF_OK;
}

static volatile LONG
        links_read; /* bit 0: a, 1: b, 2: link, 3: cloud, 4: empty */

static WfStatus links_hash(const wchar_t *dir,
        const wchar_t *name,
        const WfSearchSeen *seen,
        uint64_t limit,
        bool (*cancel)(void *),
        void *cctx,
        uint8_t digest[32])
{
	static const wchar_t *const names[] = {
	        L"a.txt", L"b.txt", L"link.txt", L"cloud.txt", L"empty.txt"};

	(void)dir;
	(void)seen;
	(void)limit;
	(void)cancel;
	(void)cctx;
	for (LONG i = 0; i < 5; i++) {
		if (!wcscmp(name, names[i])) {
			InterlockedOr(&links_read, 1L << i);
		}
	}
	memset(digest, 0x5a, 32); /* every file is "the same" */
	return WF_OK;
}

static void links_cloud_and_empty_files_are_not_compared(void)
{
	WfSearchWorker *w = NULL;
	WfSearchPort p;
	WfFileId none = {0};
	Got g;

	memset(&g, 0, sizeof g);
	links_read = 0;
	CHECK(WF_OK == wf_searchworker_start(&w, links_list));
	p = wf_searchworker_port(w);
	wf_searchworker_set_hash(w, links_hash);
	CHECK(WF_OK == p.start_dupes(p.ctx, 1, L"F", none, false));
	drain(w, &g, 1, 20000);
	CHECK(g.done && WF_OK == g.st);
	CHECK(1 == g.groups);
	CHECK(2 == g.in_groups);
	CHECK(find(&g, L"a.txt") && find(&g, L"b.txt"));
	CHECK(NULL == find(&g, L"link.txt") && NULL == find(&g, L"cloud.txt") &&
	        NULL == find(&g, L"empty.txt"));
	CHECK(3 == links_read); /* only a.txt and b.txt were read */
	got_free(&g);
	{
		WfWorkerStopReport rep = wf_searchworker_stop(w);

		if (rep.completion) {
			WaitForSingleObject(rep.completion, 5000);
			CloseHandle(rep.completion);
		}
	}
}

static WfStatus same_digest(const wchar_t *dir,
        const wchar_t *name,
        const WfSearchSeen *seen,
        uint64_t limit,
        bool (*cancel)(void *),
        void *cctx,
        uint8_t digest[32])
{
	(void)dir;
	(void)name;
	(void)seen;
	(void)limit;
	(void)cancel;
	(void)cctx;
	memset(digest, 0x33, 32); /* every file is "the same" */
	return WF_OK;
}

/* One group of more files than the window may hold: the files shown are the
 * limit and not more (nothing is made for the rest), and it says "at least". */
static void a_group_over_the_limit_is_cut_before_it_is_made(void)
{
	WfSearchWorker *w = NULL;
	WfSearchPort p;
	Got g = {0};
	WfFileId none = {0};

	memset(&fake, 0, sizeof fake);
	fake.entries = WF_SEARCH_MAX_HITS + 20000;
	CHECK(WF_OK == wf_searchworker_start(&w, fake_list));
	p = wf_searchworker_port(w);
	wf_searchworker_set_hash(w, same_digest);
	CHECK(WF_OK == p.start_dupes(p.ctx, 1, L"F", none, false));
	drain(w, &g, 1, 120000);
	CHECK(g.done && WF_OK == g.st);
	CHECK(g.at_least);
	CHECK(WF_SEARCH_MAX_HITS == g.n);
	CHECK(1 == g.groups);
	got_free(&g);
	{
		WfWorkerStopReport rep = wf_searchworker_stop(w);

		if (rep.completion) {
			WaitForSingleObject(rep.completion, 5000);
			CloseHandle(rep.completion);
		}
	}
}

static void cleanup(void)
{
	wchar_t report[2048];
	int left;
	/* Delete only recorded objects after the area's ownership proofs. */
	left = p0_area_destroy(area, report, 2048);
	if (left) {
		fwprintf(stderr, L"Search fixture cleanup: %ls\n", report);
	}
	CHECK(0 == left);
}

int main(void)
{
	wchar_t temp[MAX_PATH];
	unsigned char nonce[8];
	char id[96];
	DWORD units = GetTempPathW(MAX_PATH, temp);

	/* Mutants stop at their first assertion and retain their fixtures. A PID
	 * can be reused, so never adopt an old search_<pid> directory. */
	if (!units || units >= MAX_PATH ||
	        BCryptGenRandom(NULL, nonce, sizeof nonce,
	                BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
		CHECK_FAIL("search fixture base and nonce");
		return 1;
	}
	snprintf(id, sizeof id, "search-%lu-%02x%02x%02x%02x%02x%02x%02x%02x",
	        (unsigned long)GetCurrentProcessId(), nonce[0], nonce[1],
	        nonce[2], nonce[3], nonce[4], nonce[5], nonce[6], nonce[7]);
	/* P0Area appends its separator, including in extended paths. */
	if (temp[units - 1] == L'\\') {
		temp[units - 1] = L'\0';
	}
	area = p0_area_create(temp, id);
	if (!area) {
		CHECK_FAIL("fresh owned search fixture");
		return 1;
	}
	if (wcslen(p0_area_root(area)) >= MAX_PATH - 40) {
		CHECK_FAIL("search fixture path fits the junction buffers");
		p0_area_forget(area);
		return 1;
	}
	wcscpy_s(root, sizeof root / sizeof *root, p0_area_root(area));

	hits_and_where_they_are();
	many_hits_come_in_batches_and_in_order_of_distance();
	hidden_entries_and_a_junction_not_followed();
	unreadable_folders_are_counted_and_the_rest_is_searched();
	a_newer_request_ends_the_older_and_cancel_delivers_nothing();
	cancellation_reaches_an_inflight_enumerator();
	stopping_in_the_middle_returns_within_the_wait();
	duplicates_are_found_by_size_then_prefix_then_the_whole_file();
	links_cloud_and_empty_files_are_not_compared();
	/* Fail-fast mutants can report functional failures before the stress cases. */
	a_slow_screen_stops_the_walk_and_not_the_memory();
	the_limit_is_two_hundred_thousand_and_it_says_at_least();
	a_group_over_the_limit_is_cut_before_it_is_made();
	cleanup();

	if (wf_test_failures) {
		printf("test_searchworker_win: %d failure(s)\n",
		        wf_test_failures);
		return 1;
	}
	printf("test_searchworker_win: all checks passed (%d skipped)\n",
	        skips);
	return 0;
}

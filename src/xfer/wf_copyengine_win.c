#include "wf_copyengine_win.h"
#include "../fs/wf_weakid_win.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../fs/owned_delete.h"
#include "../path/wf_path.h"
#include "wf_recycle_win.h"
#include "wf_verify.h"
#include "wf_verify_win.h"
#include "../base/wf_macro.h"
#include "../base/wf_mem.h"

#define CHUNK (1u << 20)
#define STREAM_MAX_COUNT 16u
#define STREAM_MAX_BYTES (1u << 20)
#define FREE_MARGIN (1u << 20)
#define MAX_DEPTH 64u
#define SOURCE_PROOF_MAX 128u
typedef struct SourceProof SourceProof;
/* attributes that describe the file itself and are carried over */
#define CARRIED_ATTRIBUTES                                                     \
	(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |                     \
	        FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |               \
	        FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)
#define CLOUD_ATTRIBUTES                                                       \
	(FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN |              \
	        FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)

/* What a folder copy made inside its staging folder, in creation order, so
 * that an unfinished copy can be taken apart again, each object only on the
 * strength of the FileId it was created with. */
typedef struct
{
	wchar_t *path_x; /* extended form */
	FILE_ID_INFO id;
	bool dir;
	/* A file is sealed when it is whole and verified: what it was then.
	 * Anything that later writes to it, however briefly, changes one of
	 * these (ChangeTime moves on every write, even one that puts the old
	 * LastWriteTime back). Checked before every renewed attempt to publish. */
	bool sealed;
	uint64_t size, change;
	DWORD attributes;
} Made;

typedef struct
{
	Made *v;
	size_t n, cap;
	bool lost; /* something could not be recorded: it cannot be undone */
} Manifest;

typedef struct
{
	const WfCopySpec *spec;
	const WfCopyControl *ctl;
	WfCopyJournal *jr;
	WfCopyResult *res;
	WfVerifyPlan plan;
	uint8_t *buffer;
	bool cancelled;
	size_t files_done;
	uint64_t bytes_done;
	bool tree_clean; /* nothing under the current top-level item was skipped or failed */
	bool tree_may_remove; /* every file under it passed the stage machine */
	Manifest *stage; /* non-NULL while a top-level folder is being staged */
	size_t hard_errors; /* failures that are errors, not declared omissions */
	/* What was listed in the source folder while it was copied (names,
	 * attributes, sizes, times), summed: a move removes the source only if
	 * listing it again gives the same sum. */
	uint64_t src_sum;
	size_t src_n;
	SourceProof *proof;
	size_t proof_n;
	/* The identity of the top-level object this job itself made (the first
	 * file published, or the folder created), so that what is registered
	 * for Undo is that object and not whatever has the name later. */
	FILE_ID_INFO made_id;
	bool made_ok;
	size_t created_cap; /* room in res->created, in items (0: not known yet) */
} Run;

static bool proofs_valid(Run *r);

typedef enum
{
	OUT_OK,
	OUT_SKIPPED,
	OUT_FAILED,
	OUT_CANCELLED
} Outcome;

static bool keep_going(Run *r)
{
	if (r->cancelled) {
		return false;
	}
	if (r->ctl && r->ctl->checkpoint && !r->ctl->checkpoint(r->ctl->ctx)) {
		r->cancelled = true;
	}
	return !r->cancelled;
}

static bool cancelled_cb(void *ctx)
{
	return !keep_going(ctx);
}

static bool ready_control(void *ctx)
{
	return keep_going(ctx);
}

static void report_progress(Run *r, const wchar_t *current)
{
	if (r->ctl && r->ctl->progress) {
		r->ctl->progress(
		        r->ctl->ctx, r->files_done, r->bytes_done, current);
	}
}

/* a + b + c in one allocation: the paths here are as long as the preflight
 * lets them be (32766 units), so no fixed buffer holds them. NULL: out of
 * memory, or too long to be sized. */
static wchar_t *cat3(const wchar_t *a, const wchar_t *b, const wchar_t *c)
{
	size_t na = wcslen(a), nb = wcslen(b), nc = wcslen(c);
	wchar_t *out;

	if (na > SIZE_MAX / sizeof *out - nb - nc - 1) {
		return NULL;
	}
	out = malloc((na + nb + nc + 1) * sizeof *out);
	if (!out) {
		return NULL;
	}
	memcpy(out, a, na * sizeof *out);
	memcpy(out + na, b, nb * sizeof *out);
	memcpy(out + na + nb, c, (nc + 1) * sizeof *out);
	return out;
}

static void note_failure(
        Run *r, const wchar_t *rel, WfCopyFailKind k, DWORD error)
{
	WfCopyResult *res = r->res;

	if (res->failure_total < WF_CJ_FAILURES) {
		WfCopyFailure *f = &res->failures[res->failure_total];

		wcsncpy_s(f->name, WF_ARRAY_LEN(f->name), rel, _TRUNCATE);
		f->kind = k;
		f->error = error;
	}
	if (res->fail_all_n < WF_CJ_FAIL_ALL_MAX && r->spec->slot.dir) {
		/* rel is "a\b\c": the path is the folder, a separator and all of it */
		const wchar_t *dir = r->spec->slot.dir;
		wchar_t *full = cat3(
		        dir, L'\\' == dir[wcslen(dir) - 1] ? L"" : L"\\", rel);

		if (full) {
			if (0 == (res->fail_all_n & 31u)) {
				WfCopyFailFull *grown = realloc(res->fail_all,
				        (res->fail_all_n + 32) *
				                sizeof *res->fail_all);

				if (!grown) {
					free(full);
					full = NULL;
				} else {
					res->fail_all = grown;
				}
			}
			if (full) {
				WfCopyFailFull *f =
				        &res->fail_all[res->fail_all_n++];

				f->path = full;
				f->kind = k;
				f->error = error;
			}
		}
	}
	res->failure_total++;
	r->tree_clean = false;
	r->tree_may_remove = false;
	if (WF_CF_LINK_SKIPPED != k && WF_CF_CLOUD_SKIPPED != k &&
	        WF_CF_ENCRYPTED_SKIPPED != k) {
		r->hard_errors++;
	}
}

static wchar_t *xp(const wchar_t *path)
{
	wchar_t *x = NULL;

	return WF_OK == wf_path_to_extended(path, &x) ? x : NULL;
}

typedef struct
{
	FILE_ID_INFO id;
	uint64_t size, mtime, change;
	DWORD attributes, links;
} Stamp;

static bool stamp_of(HANDLE h, Stamp *s)
{
	FILE_BASIC_INFO b;
	FILE_STANDARD_INFO st;

	memset(s, 0, sizeof *s);
	if (!wf_handle_file_id_info(h, &s->id) ||
	        !GetFileInformationByHandleEx(h, FileBasicInfo, &b, sizeof b) ||
	        !GetFileInformationByHandleEx(
	                h, FileStandardInfo, &st, sizeof st)) {
		return false;
	}
	s->size = (uint64_t)st.EndOfFile.QuadPart;
	s->mtime = (uint64_t)b.LastWriteTime.QuadPart;
	s->change = (uint64_t)b.ChangeTime.QuadPart;
	s->attributes = b.FileAttributes;
	s->links = st.NumberOfLinks;
	return true;
}

static bool same_id(const FILE_ID_INFO *a, const FILE_ID_INFO *b)
{
	return 0 == memcmp(a, b, sizeof *a);
}

/* The state of a finished file, read through a handle of its own: closing the
 * handle that wrote it may still move ChangeTime, so the seal is taken after
 * that, from what is really there. False unless it is the file with `id`. */
static bool seal_of(const wchar_t *path_x, const FILE_ID_INFO *id, Stamp *s)
{
	HANDLE h = CreateFileW(path_x, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	bool ok;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = stamp_of(h, s) && same_id(&s->id, id) &&
	        !(s->attributes & FILE_ATTRIBUTE_REPARSE_POINT);
	CloseHandle(h);
	return ok;
}

static bool snap_matches(const WfNodeInfo *snap, const Stamp *s)
{
	return wf_info_matches(&snap->id, &s->id) && snap->size == s->size &&
	        snap->mtime == s->mtime;
}

/* The path of `leaf` below `parent`, both relative to the copied folder. */
static wchar_t *rel_join(const wchar_t *parent, const wchar_t *leaf)
{
	return parent[0] ? cat3(parent, L"\\", leaf) : cat3(leaf, L"", L"");
}

static uint64_t listing_hash(const wchar_t *rel, const WIN32_FIND_DATAW *fd)
{
	uint64_t h = 1469598103934665603ull;
	const unsigned char *p = (const unsigned char *)rel;
	size_t i, n = wcslen(rel) * sizeof *rel;
	uint64_t tail[3];

	tail[0] = fd->dwFileAttributes;
	tail[1] = (uint64_t)fd->nFileSizeHigh << 32 | fd->nFileSizeLow;
	tail[2] = (uint64_t)fd->ftLastWriteTime.dwHighDateTime << 32 |
	        fd->ftLastWriteTime.dwLowDateTime;
	for (i = 0; i < n; i++) {
		h = (h ^ p[i]) * 1099511628211ull;
	}
	p = (const unsigned char *)tail;
	for (i = 0; i < sizeof tail; i++) {
		h = (h ^ p[i]) * 1099511628211ull;
	}
	return h;
}

static void listing_add(Run *r, const wchar_t *rel, const WIN32_FIND_DATAW *fd)
{
	r->src_sum += listing_hash(rel, fd);
	r->src_n++;
}

/*
 * Lists the folder again and adds up the same way. False when the sum is not
 * the one taken during the copy, or the listing cannot be read to its end:
 * something was added, removed or changed since, and the source is kept.
 */
static bool listing_sum(const wchar_t *dir_x,
        const wchar_t *rel,
        unsigned depth,
        uint64_t *sum,
        size_t *n)
{
	wchar_t *pattern =
	        wf_malloc_array((wcslen(dir_x) + 3), sizeof *pattern);
	WIN32_FIND_DATAW fd;
	HANDLE find;
	bool ok = true;

	if (!pattern || depth >= MAX_DEPTH) {
		free(pattern);
		return false;
	}
	swprintf(pattern, wcslen(dir_x) + 3, L"%ls\\*", dir_x);
	find = FindFirstFileExW(pattern, FindExInfoBasic, &fd,
	        FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
	free(pattern);
	if (INVALID_HANDLE_VALUE == find) {
		return ERROR_FILE_NOT_FOUND == GetLastError();
	}
	do {
		wchar_t *child_rel;

		if (!wcscmp(fd.cFileName, L".") ||
		        !wcscmp(fd.cFileName, L"..")) {
			continue;
		}
		child_rel = rel_join(rel, fd.cFileName);
		if (!child_rel) {
			ok = false;
			break;
		}
		*sum += listing_hash(child_rel, &fd);
		(*n)++;
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
		        !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
			wchar_t *child = NULL;

			if (WF_OK !=
			                wf_path_join(
			                        dir_x, fd.cFileName, &child) ||
			        !listing_sum(
			                child, child_rel, depth + 1, sum, n)) {
				ok = false;
			}
			free(child);
		}
		free(child_rel);
		if (!ok) {
			break;
		}
	} while (FindNextFileW(find, &fd));
	if (ok && ERROR_NO_MORE_FILES != GetLastError()) {
		ok = false;
	}
	FindClose(find);
	return ok;
}

/* A moved folder is removed only if it still holds what was copied. */
static bool source_listing_same(
        Run *r, const wchar_t *src, const wchar_t *rel, bool is_dir)
{
	wchar_t *src_x;
	uint64_t sum = 0;
	size_t n = 0;
	bool same;

	if (!is_dir) {
		return proofs_valid(r);
	}
	src_x = xp(src);
	same = src_x && listing_sum(src_x, rel, 0, &sum, &n) &&
	        sum == r->src_sum && n == r->src_n;
	free(src_x);
	return same && proofs_valid(r);
}

typedef struct
{
	wchar_t name[MAX_PATH + 36];
	uint64_t size;
	HANDLE source, destination;
} StreamItem;

typedef struct
{
	StreamItem v[STREAM_MAX_COUNT];
	size_t n;
	bool supported;
} Streams;

static void streams_close(Streams *s)
{
	for (size_t i = 0; i < s->n; i++) {
		if (s->v[i].source) {
			CloseHandle(s->v[i].source);
		}
		if (s->v[i].destination) {
			CloseHandle(s->v[i].destination);
		}
	}
	memset(s, 0, sizeof *s);
}

/* Enumeration is a complete manifest or an error, never a partial success.
 * ERROR_INVALID_PARAMETER is the documented no-stream-support result. */
static bool list_streams(const wchar_t *x_path, Streams *out, DWORD *error)
{
	WIN32_FIND_STREAM_DATA data;
	HANDLE f = FindFirstStreamW(x_path, FindStreamInfoStandard, &data, 0);

	memset(out, 0, sizeof *out);
	*error = 0;
	if (INVALID_HANDLE_VALUE == f) {
		*error = GetLastError();
		if (ERROR_HANDLE_EOF == *error ||
		        ERROR_INVALID_PARAMETER == *error) {
			out->supported = ERROR_HANDLE_EOF == *error;
			*error = 0;
			return true;
		}
		return false;
	}
	out->supported = true;
	do {
		if (!wcscmp(data.cStreamName, L"::$DATA")) {
			continue;
		}
		if (out->n == STREAM_MAX_COUNT ||
		        data.StreamSize.QuadPart < 0 ||
		        (uint64_t)data.StreamSize.QuadPart > STREAM_MAX_BYTES) {
			*error = ERROR_NOT_SUPPORTED;
			FindClose(f);
			return false;
		}
		StreamItem *item = &out->v[out->n++];
		wcscpy_s(
		        item->name, WF_ARRAY_LEN(item->name), data.cStreamName);
		item->size = (uint64_t)data.StreamSize.QuadPart;
	} while (FindNextStreamW(f, &data));
	*error = GetLastError();
	FindClose(f);
	if (ERROR_HANDLE_EOF != *error) {
		return false;
	}
	*error = 0;
	return true;
}

static bool streams_same(const Streams *a, const Streams *b)
{
	if (a->n != b->n) {
		return false;
	}
	for (size_t i = 0; i < a->n; i++) {
		bool found = false;
		for (size_t j = 0; j < b->n; j++) {
			if (!wcscmp(a->v[i].name, b->v[j].name) &&
			        a->v[i].size == b->v[j].size) {
				found = true;
			}
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

static bool compare_stream(Run *r,
        HANDLE source,
        HANDLE destination,
        const FILE_ID_INFO *sid,
        const FILE_ID_INFO *did,
        uint64_t size,
        DWORD *error)
{
	WfVerifyPlan plan;
	WfStageMachine stage;
	WfIntegrityReport report;
	(void)wf_verify_plan(WF_KIND_COPY, WF_VERIFY_SHA256, &plan);
	wf_stage_init(&stage, &plan);
	wf_stage_copied(&stage, true);
	wf_stage_source_accepted(&stage, true);
	wf_stage_destination_durable(&stage, true);
	bool ok = wf_verify_handles(&stage, source, sid, destination, did, size,
	        cancelled_cb, r, &report);
	*error = report.error ? report.error : (ok ? 0 : ERROR_CRC);
	return ok;
}

static bool copy_one_stream(Run *r,
        const wchar_t *from_x,
        const wchar_t *to_x,
        StreamItem *s,
        const FILE_ID_INFO *sid,
        const FILE_ID_INFO *did,
        DWORD *error)
{
	wchar_t *a = cat3(from_x, L"", s->name), *b = cat3(to_x, L"", s->name);
	HANDLE in = INVALID_HANDLE_VALUE, out = INVALID_HANDLE_VALUE;
	FILE_ID_INFO identity;
	uint8_t *buf = NULL;
	DWORD got = 0, put = 0;
	bool ok = false;
	*error = 0;
	buf = a && b ? malloc(s->size ? (size_t)s->size : 1) : NULL;
	if (!buf) {
		free(a);
		free(b);
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return false;
	}
	in = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
	        NULL, OPEN_EXISTING, 0, NULL);
	if (in == INVALID_HANDLE_VALUE) {
		*error = GetLastError();
		goto done;
	}
	if (!wf_handle_file_id_info(in, &identity)) {
		*error = GetLastError();
		goto done;
	}
	if (!same_id(&identity, sid)) {
		*error = ERROR_INVALID_DATA;
		goto done;
	}
	if (s->size && !ReadFile(in, buf, (DWORD)s->size, &got, NULL)) {
		*error = GetLastError();
		goto done;
	}
	if (got != s->size) {
		*error = ERROR_HANDLE_EOF;
		goto done;
	}
	out = CreateFileW(b, GENERIC_READ | GENERIC_WRITE,
	        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_NEW, 0, NULL);
	if (out == INVALID_HANDLE_VALUE) {
		*error = GetLastError();
		goto done;
	}
	if (s->size && !WriteFile(out, buf, (DWORD)s->size, &put, NULL)) {
		*error = GetLastError();
		goto done;
	}
	if (put != s->size) {
		*error = ERROR_WRITE_FAULT;
		goto done;
	}
	if (!FlushFileBuffers(out)) {
		*error = GetLastError();
		goto done;
	}
	if (!compare_stream(r, in, out, sid, did, s->size, error)) {
		goto done;
	}
	CloseHandle(out);
	out = INVALID_HANDLE_VALUE;
	s->destination = CreateFileW(b, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0,
	        NULL);
	if (s->destination == INVALID_HANDLE_VALUE) {
		*error = GetLastError();
		s->destination = NULL;
		goto done;
	}
	s->source = in;
	in = INVALID_HANDLE_VALUE;
	ok = true;
done:
	if (out != INVALID_HANDLE_VALUE) {
		CloseHandle(out);
	}
	if (in != INVALID_HANDLE_VALUE) {
		CloseHandle(in);
	}
	free(buf);
	free(a);
	free(b);
	return ok;
}

/* Known streams are held without write sharing through the recycle receipt.
 * Delete sharing is necessary for IFileOperation. It leaves a path-swap and
 * new-stream namespace race: the shell API is not a handle-based transaction.
 * The bounded proof checks bindings, complete stream sets and contents again
 * immediately before invoking it; any uncertainty keeps the source. */
struct SourceProof
{
	SourceProof *next;
	wchar_t *path, *destination_path;
	HANDLE source, destination;
	Stamp before, made;
	Streams streams;
};

static void proofs_close(Run *r)
{
	while (r->proof) {
		SourceProof *p = r->proof;
		r->proof = p->next;
		streams_close(&p->streams);
		if (p->source) {
			CloseHandle(p->source);
		}
		if (p->destination) {
			CloseHandle(p->destination);
		}
		free(p->path);
		free(p->destination_path);
		free(p);
	}
	r->proof_n = 0;
}

static bool proof_add(Run *r,
        const wchar_t *path,
        const wchar_t *destination_path,
        HANDLE *source,
        HANDLE destination,
        const Stamp *before,
        const Stamp *made,
        Streams *streams)
{
	SourceProof *p =
	        r->proof_n < SOURCE_PROOF_MAX ? calloc(1, sizeof *p) : NULL;
	if (!p) {
		return false;
	}
	p->path = _wcsdup(path);
	p->destination_path =
	        destination_path ? _wcsdup(destination_path) : NULL;
	if (!p->path || (destination_path && !p->destination_path)) {
		free(p->path);
		free(p->destination_path);
		free(p);
		return false;
	}
	p->source = *source;
	*source = INVALID_HANDLE_VALUE;
	p->destination = destination;
	p->before = *before;
	if (made) {
		p->made = *made;
	}
	if (streams) {
		p->streams = *streams;
		memset(streams, 0, sizeof *streams);
	}
	p->next = r->proof;
	r->proof = p;
	r->proof_n++;
	return true;
}

/* NTFS may refuse a directory rename while any child file is open. Only
 * destination guards are released for publication; source guards stay held.
 * Reopened destinations must pass the complete identity/content proof before
 * the source can be retired. A publication failure never authorizes removal. */
static void proofs_release_destinations(Run *r)
{
	for (SourceProof *p = r->proof; p; p = p->next) {
		if (p->destination) {
			CloseHandle(p->destination);
			p->destination = NULL;
		}
		for (size_t i = 0; i < p->streams.n; i++) {
			StreamItem *v = &p->streams.v[i];
			if (v->destination) {
				CloseHandle(v->destination);
				v->destination = NULL;
			}
		}
	}
}

static bool proofs_reopen_destinations(
        Run *r, const wchar_t *stage, const wchar_t *final)
{
	size_t prefix = wcslen(stage);
	for (SourceProof *p = r->proof; p; p = p->next) {
		wchar_t *path;

		if (!p->destination_path) {
			continue;
		}
		if (wcsncmp(p->destination_path, stage, prefix) ||
		        p->destination_path[prefix] != L'\\' ||
		        !(path = cat3(
		                  final, L"", p->destination_path + prefix))) {
			return false;
		}
		p->destination = CreateFileW(path, GENERIC_READ,
		        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
		        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
		if (p->destination == INVALID_HANDLE_VALUE) {
			p->destination = NULL;
			free(path);
			return false;
		}
		for (size_t i = 0; i < p->streams.n; i++) {
			StreamItem *v = &p->streams.v[i];
			wchar_t *stream = cat3(path, L"", v->name);

			if (!stream) {
				free(path);
				return false;
			}
			v->destination = CreateFileW(stream, GENERIC_READ,
			        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
			        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT,
			        NULL);
			free(stream);
			if (v->destination == INVALID_HANDLE_VALUE) {
				v->destination = NULL;
				free(path);
				return false;
			}
		}
		free(path);
	}
	return true;
}

/* The normalized path a handle is open on, allocated to the length it has. */
static wchar_t *final_path_of(HANDLE h)
{
	DWORD n = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED);
	DWORD got;
	wchar_t *path;

	if (!n) {
		return NULL;
	}
	path = malloc(((size_t)n + 1) * sizeof *path);
	if (!path) {
		return NULL;
	}
	got = GetFinalPathNameByHandleW(h, path, n + 1, FILE_NAME_NORMALIZED);
	if (!got || got > n) {
		free(path); /* the lookup failed or grew between the two calls */
		return NULL;
	}
	return path;
}

static bool proofs_valid(Run *r)
{
	DWORD error = 0;
	for (SourceProof *p = r->proof; p; p = p->next) {
		Stamp bound;
		Streams now;
		HANDLE h = CreateFileW(p->path, FILE_READ_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		        NULL, OPEN_EXISTING,
		        FILE_FLAG_BACKUP_SEMANTICS |
		                FILE_FLAG_OPEN_REPARSE_POINT,
		        NULL);
		bool same = h != INVALID_HANDLE_VALUE && stamp_of(h, &bound) &&
		        same_id(&bound.id, &p->before.id) &&
		        !(bound.attributes & FILE_ATTRIBUTE_REPARSE_POINT);
		if (h != INVALID_HANDLE_VALUE) {
			CloseHandle(h);
		}
		if (!same || !list_streams(p->path, &now, &error) ||
		        !streams_same(&p->streams, &now) || !keep_going(r)) {
			return false;
		}
		if (!p->destination_path) {
			continue;
		}
		if (!p->destination) {
			return false;
		}
		wchar_t *dest = final_path_of(p->destination);
		bool good = dest && list_streams(dest, &now, &error) &&
		        streams_same(&p->streams, &now) &&
		        compare_stream(r, p->source, p->destination,
		                &p->before.id, &p->made.id, p->before.size,
		                &error);

		free(dest);
		if (!good) {
			return false;
		}
		for (size_t i = 0; i < p->streams.n; i++) {
			StreamItem *s = &p->streams.v[i];
			if (!compare_stream(r, s->source, s->destination,
			            &p->before.id, &p->made.id, s->size,
			            &error)) {
				return false;
			}
		}
	}
	return r->proof_n != 0;
}

typedef struct
{
	Run *run;
	const wchar_t *source, *relative;
	bool directory;
} Removal;

static bool ready_to_recycle(void *ctx)
{
	Removal *p = ctx;
	return keep_going(p->run) &&
	        source_listing_same(
	                p->run, p->source, p->relative, p->directory);
}

/* A name for a temporary in `dir` (and `suffix`), allocated; NULL when it
 * cannot be made. */
static wchar_t *temp_path(const wchar_t *dir, const wchar_t *suffix)
{
	unsigned char rnd[8] = {0};
	unsigned long long v = 0;
	wchar_t name[40];
	size_t i;

	(void)BCryptGenRandom(
	        NULL, rnd, sizeof rnd, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	for (i = 0; i < sizeof rnd; i++) {
		v = v << 8 | rnd[i];
	}
	swprintf(name, WF_ARRAY_LEN(name), L"\\.wfcopy-%016llx.tmp", v);
	return cat3(dir, name, suffix);
}

static void drop_temp(Run *r, const wchar_t *tmp, const FILE_ID_INFO *id)
{
	WfOwnedResult res = wf_owned_delete_save_temp(tmp, id, NULL);

	(void)res; /* a temporary we cannot remove stays in the journal as ours */
	if (WF_OWNED_DELETED == res || WF_OWNED_ABSENT == res) {
		wf_cj_gone(r->jr, id);
	}
}

/* `seal` is the state of a finished file (NULL for a folder). */
static bool manifest_add(Run *r,
        const wchar_t *path_x,
        const FILE_ID_INFO *id,
        bool dir,
        const Stamp *seal)
{
	Manifest *m = r->stage;
	wchar_t *copy;

	if (!m) {
		return true;
	}
	if (m->n == m->cap) {
		size_t cap = m->cap ? m->cap * 2 : 64;
		Made *grown = wf_realloc_array(m->v, cap, sizeof *grown);

		if (!grown) {
			m->lost = true;
			return false;
		}
		m->v = grown;
		m->cap = cap;
	}
	copy = _wcsdup(path_x);
	if (!copy) {
		m->lost = true;
		return false;
	}
	m->v[m->n].path_x = copy;
	m->v[m->n].id = *id;
	m->v[m->n].dir = dir;
	m->v[m->n].sealed = NULL != seal;
	m->v[m->n].size = seal ? seal->size : 0;
	m->v[m->n].change = seal ? seal->change : 0;
	m->v[m->n].attributes = seal ? seal->attributes : 0;
	m->n++;
	return true;
}

/* A folder the engine has just made: its FileId is read from the folder. */
static bool manifest_add_folder(Run *r, const wchar_t *path_x)
{
	FILE_ID_INFO id;
	HANDLE h = CreateFileW(path_x, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	bool ok;

	if (INVALID_HANDLE_VALUE == h) {
		if (r->stage) {
			r->stage->lost = true;
		}
		return false;
	}
	ok = wf_handle_file_id_info(h, &id);
	CloseHandle(h);
	if (!ok) {
		if (r->stage) {
			r->stage->lost = true;
		}
		return false;
	}
	return manifest_add(r, path_x, &id, true, NULL);
}

static void manifest_free(Manifest *m)
{
	size_t i;

	for (i = 0; i < m->n; i++) {
		free(m->v[i].path_x);
	}
	free(m->v);
	memset(m, 0, sizeof *m);
}

/* Takes the unfinished folder apart, newest first, each file and each (by
 * then empty) folder only on its own FileId. True when nothing is left. */
static bool manifest_undo(Manifest *m)
{
	bool clean = !m->lost;
	size_t i;

	for (i = m->n; i-- > 0;) {
		WfOwnedResult res = m->v[i].dir
		        ? wf_owned_delete_empty_dir(
		                  m->v[i].path_x, &m->v[i].id, NULL)
		        : wf_owned_delete_save_temp(
		                  m->v[i].path_x, &m->v[i].id, NULL);

		if (WF_OWNED_DELETED != res && WF_OWNED_ABSENT != res) {
			clean = false;
		}
	}
	return clean;
}

static bool set_dest_times(HANDLE dest, HANDLE source)
{
	FILE_BASIC_INFO b;

	if (!GetFileInformationByHandleEx(
	            source, FileBasicInfo, &b, sizeof b)) {
		return false;
	}
	b.FileAttributes &= CARRIED_ATTRIBUTES;
	if (!b.FileAttributes) {
		b.FileAttributes = FILE_ATTRIBUTE_NORMAL;
	}
	b.ChangeTime.QuadPart = 0;
	return 0 !=
	        SetFileInformationByHandle(dest, FileBasicInfo, &b, sizeof b);
}

static bool rename_noclobber(HANDLE h, const wchar_t *target_x, DWORD *error)
{
	size_t bytes = wcslen(target_x) * sizeof(wchar_t);
	FILE_RENAME_INFO *info = calloc(1, sizeof *info + bytes);
	bool ok;

	if (!info) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return false;
	}
	info->ReplaceIfExists = FALSE;
	info->RootDirectory = NULL;
	info->FileNameLength = (DWORD)bytes;
	memcpy(info->FileName, target_x, bytes);
	ok = SetFileInformationByHandle(h, FileRenameInfo, info,
	             (DWORD)(sizeof *info + bytes)) != 0;
	if (!ok) {
		*error = GetLastError();
	}
	free(info);
	return ok;
}

/*
 * A folder the job works in or from is held open for the whole job, without
 * delete sharing, and must be the object that was checked. While it is held it
 * and everything above it cannot be renamed, replaced by a link or removed, so
 * a path through it keeps meaning the same place.
 */
static bool pin_dir(
        const wchar_t *path, const WfFileId *want, HANDLE *out, DWORD *error)
{
	wchar_t *x = xp(path);
	HANDLE h;
	Stamp st;

	*out = INVALID_HANDLE_VALUE;
	if (!x || !wf_fileid_usable_to_read(want)) {
		free(x);
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}
	h = CreateFileW(x, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	free(x);
	if (INVALID_HANDLE_VALUE == h) {
		*error = GetLastError();
		return false;
	}
	if (!stamp_of(h, &st) || !(st.attributes & FILE_ATTRIBUTE_DIRECTORY) ||
	        (st.attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
	        !wf_info_matches(want, &st.id)) {
		*error = ERROR_INVALID_DATA;
		CloseHandle(h);
		return false;
	}
	*out = h;
	return true;
}

/* The old file's permissions on the new one, which is about to take its
 * place: an overwrite keeps who may read it. */
static bool copy_dacl(HANDLE from, HANDLE to, DWORD *error)
{
	PSECURITY_DESCRIPTOR sd = NULL;
	PACL dacl = NULL;
	DWORD e = GetSecurityInfo(from, SE_FILE_OBJECT,
	        DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd);

	if (ERROR_SUCCESS == e) {
		e = SetSecurityInfo(to, SE_FILE_OBJECT,
		        DACL_SECURITY_INFORMATION |
		                PROTECTED_DACL_SECURITY_INFORMATION,
		        NULL, NULL, dacl, NULL);
	}
	if (sd) {
		LocalFree(sd);
	}
	if (ERROR_SUCCESS != e) {
		*error = e;
	}
	return ERROR_SUCCESS == e;
}

/* Whether the file at `dst_x` may be replaced, and what it is now. */
static bool replace_target(
        const wchar_t *dst_x, Stamp *now, WfCopyFailKind *why, DWORD *error)
{
	HANDLE h = CreateFileW(dst_x, FILE_READ_ATTRIBUTES | READ_CONTROL,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
	        NULL);

	if (INVALID_HANDLE_VALUE == h) {
		*why = WF_CF_DEST_CHANGED;
		*error = GetLastError();
		return false;
	}
	if (!stamp_of(h, now)) {
		*error = GetLastError();
		CloseHandle(h);
		*why = WF_CF_DEST_UNSAFE;
		return false;
	}
	CloseHandle(h);
	if (now->attributes &
	                (FILE_ATTRIBUTE_DIRECTORY |
	                        FILE_ATTRIBUTE_REPARSE_POINT |
	                        FILE_ATTRIBUTE_READONLY |
	                        FILE_ATTRIBUTE_ENCRYPTED | CLOUD_ATTRIBUTES) ||
	        now->links > 1) {
		*why = WF_CF_DEST_UNSAFE;
		*error = 0;
		return false;
	}
	return true;
}

static Outcome copy_file(Run *r,
        const wchar_t *src,
        const wchar_t *dst_dir,
        const wchar_t *leaf,
        const WfNodeInfo *snap,
        const wchar_t *rel)
{
	wchar_t *src_x = NULL, *dst = NULL, *dst_x = NULL, *dir_x = NULL;
	wchar_t *tmp = NULL, *tmp_x = NULL;
	HANDLE in = INVALID_HANDLE_VALUE, out = INVALID_HANDLE_VALUE;
	Stamp s0, s1, d0, tmp_stamp, sealed;
	bool have_seal = false;
	Outcome result = OUT_FAILED;
	DWORD error = 0;
	bool tmp_made = false, dest_exists = false, replace = false, identified;

	memset(&d0, 0, sizeof d0);
	memset(&tmp_stamp, 0, sizeof tmp_stamp);
	uint64_t copied = 0;
	WfStageMachine m;
	ULARGE_INTEGER avail;
	Streams streams = {0}, again;
	size_t verified_streams = 0;
	WfIntegrityReport irep;
	WfCopyFailKind why = WF_CF_WRITE_FAILED;

	if (WF_OK != wf_path_join(dst_dir, leaf, &dst)) {
		note_failure(
		        r, rel, WF_CF_WRITE_FAILED, ERROR_NOT_ENOUGH_MEMORY);
		r->res->files_failed++;
		return OUT_FAILED;
	}
	src_x = xp(src);
	dst_x = xp(dst);
	dir_x = xp(dst_dir);
	if (!src_x || !dst_x || !dir_x) {
		note_failure(
		        r, rel, WF_CF_WRITE_FAILED, ERROR_NOT_ENOUGH_MEMORY);
		goto done;
	}
	report_progress(r, leaf);
	if (!keep_going(r)) {
		result = OUT_CANCELLED;
		goto done;
	}
	/* the destination name */
	dest_exists = INVALID_FILE_ATTRIBUTES != GetFileAttributesW(dst_x);
	if (dest_exists) {
		if (WF_COLLIDE_SKIP == r->spec->collide) {
			r->res->files_skipped++;
			r->tree_clean = r->tree_may_remove = false;
			result = OUT_SKIPPED;
			goto done;
		}
		if (WF_COLLIDE_OVERWRITE != r->spec->collide) {
			/* stop, or keep both whose free name was taken since */
			note_failure(r, rel, WF_CF_EXISTS, 0);
			goto done;
		}
		if (!replace_target(dst_x, &d0, &why, &error)) {
			note_failure(r, rel, why, error);
			goto done;
		}
		replace = true;
	}
	/* the source */
	in = CreateFileW(src_x, GENERIC_READ,
	        FILE_SHARE_READ |
	                (r->plan.removes_source ? FILE_SHARE_DELETE : 0),
	        NULL, OPEN_EXISTING,
	        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (INVALID_HANDLE_VALUE == in) {
		note_failure(r, rel, WF_CF_SOURCE_UNREADABLE, GetLastError());
		goto done;
	}
	if (!stamp_of(in, &s0)) {
		note_failure(r, rel, WF_CF_SOURCE_UNREADABLE, GetLastError());
		goto done;
	}
	if (s0.attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		r->res->links_skipped++;
		note_failure(r, rel, WF_CF_LINK_SKIPPED, 0);
		goto done;
	}
	if ((s0.attributes & CLOUD_ATTRIBUTES) && !r->spec->hydrate) {
		note_failure(r, rel, WF_CF_CLOUD_SKIPPED, 0);
		goto done;
	}
	if (s0.attributes & FILE_ATTRIBUTE_ENCRYPTED) {
		note_failure(r, rel, WF_CF_ENCRYPTED_SKIPPED, 0);
		goto done;
	}
	if (snap && !snap_matches(snap, &s0)) {
		note_failure(r, rel, WF_CF_SOURCE_CHANGED, 0);
		goto done;
	}
	if (GetDiskFreeSpaceExW(dir_x, &avail, NULL, NULL) &&
	        avail.QuadPart < s0.size + FREE_MARGIN) {
		note_failure(r, rel, WF_CF_NO_SPACE, ERROR_DISK_FULL);
		goto done;
	}
	/* the temporary: announced, created, recorded */
	tmp = temp_path(dst_dir, L"");
	tmp_x = tmp ? xp(tmp) : NULL;
	if (!tmp_x || !wf_cj_intent(r->jr, tmp)) {
		note_failure(r, rel, WF_CF_WRITE_FAILED, GetLastError());
		goto done;
	}
	out = CreateFileW(tmp_x,
	        GENERIC_READ | GENERIC_WRITE | DELETE | WRITE_DAC,
	        FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == out) {
		note_failure(r, rel, WF_CF_WRITE_FAILED, GetLastError());
		goto done;
	}
	if (wf_copyengine_test_tmp_id_unreadable &&
	        wf_copyengine_test_tmp_id_unreadable()) {
		identified =
		        false; /* a test says the identity cannot be read */
	} else {
		identified = stamp_of(out, &tmp_stamp);
	}
	if (!identified) {
		/* a file we made but cannot identify: left for recovery, which
		 * reports it as unresolved rather than guessing. It is not removed
		 * here: nothing is deleted except through the owned-delete proof
		 * (CI-DEL-01), and without an identity there is none. */
		note_failure(r, rel, WF_CF_WRITE_FAILED, GetLastError());
		goto done;
	}
	tmp_made = true;
	if (!wf_cj_created(r->jr, tmp, &tmp_stamp.id)) {
		note_failure(r, rel, WF_CF_WRITE_FAILED, GetLastError());
		goto done;
	}
	/* the data */
	for (;;) {
		DWORD got = 0, put = 0;

		if (!keep_going(r)) {
			result = OUT_CANCELLED;
			goto done;
		}
		if (!ReadFile(in, r->buffer, CHUNK, &got, NULL)) {
			note_failure(r, rel, WF_CF_SOURCE_UNREADABLE,
			        GetLastError());
			goto done;
		}
		if (!got) {
			break;
		}
		if (!WriteFile(out, r->buffer, got, &put, NULL) || put != got) {
			error = GetLastError();
			note_failure(r, rel,
			        ERROR_DISK_FULL == error ? WF_CF_NO_SPACE
			                                 : WF_CF_WRITE_FAILED,
			        error);
			goto done;
		}
		copied += got;
		r->bytes_done += got;
		report_progress(r, leaf);
	}
	if (!list_streams(src_x, &streams, &error)) {
		note_failure(r, rel, WF_CF_SOURCE_UNREADABLE, error);
		goto done;
	}
	for (size_t i = 0; i < streams.n; i++) {
		if (!copy_one_stream(r, src_x, tmp_x, &streams.v[i], &s0.id,
		            &tmp_stamp.id, &error)) {
			note_failure(r, rel, WF_CF_VERIFY_FAILED, error);
			goto done;
		}
	}
	if (!list_streams(src_x, &again, &error) ||
	        !streams_same(&streams, &again) ||
	        !list_streams(tmp_x, &again, &error) ||
	        !streams_same(&streams, &again)) {
		note_failure(r, rel, WF_CF_VERIFY_FAILED,
		        error ? error : ERROR_INVALID_DATA);
		goto done;
	}
	/* attributes and times, then make it durable */
	if (!set_dest_times(out, in)) {
		note_failure(r, rel, WF_CF_METADATA_FAILED, GetLastError());
		goto done;
	}
	wf_stage_init(&m, &r->plan);
	wf_stage_copied(&m, copied == s0.size);
	if (copied != s0.size) {
		note_failure(r, rel, WF_CF_SOURCE_CHANGED, 0);
		goto done;
	}
	/* the source must still be the object that was chosen, unchanged */
	if (!stamp_of(in, &s1) || !same_id(&s0.id, &s1.id) ||
	        s1.size != s0.size || s1.mtime != s0.mtime) {
		wf_stage_source_accepted(&m, false);
		note_failure(r, rel, WF_CF_SOURCE_CHANGED, 0);
		goto done;
	}
	wf_stage_source_accepted(&m, true);
	if (!wf_stage_destination_durable(&m, 0 != FlushFileBuffers(out))) {
		note_failure(r, rel, WF_CF_WRITE_FAILED, GetLastError());
		goto done;
	}
	if (r->plan.read_back) {
		if (!wf_verify_handles(&m, in, &s0.id, out, &tmp_stamp.id,
		            s0.size, cancelled_cb, r, &irep)) {
			if (r->cancelled) {
				result = OUT_CANCELLED;
			} else {
				note_failure(r, rel, WF_CF_VERIFY_FAILED,
				        irep.error);
			}
			goto done;
		}
		/* only a move needs the verified result on record before its source
		 * may go; a copy is complete once it reads back equal */
		if (r->plan.removes_source &&
		        !wf_stage_verified_recorded(
		                &m, wf_cj_verified(r->jr, &tmp_stamp.id))) {
			note_failure(r, rel, WF_CF_WRITE_FAILED, 0);
			goto done;
		}
	}
	if (!wf_stage_complete(&m)) {
		note_failure(r, rel, WF_CF_VERIFY_FAILED, 0);
		goto done;
	}
	if (!wf_stage_may_remove_source(&m) && r->plan.removes_source) {
		r->tree_may_remove = false;
	}
	/* give it its name */
	if (!replace) {
		if (!rename_noclobber(out, dst_x, &error)) {
			note_failure(r, rel,
			        ERROR_ALREADY_EXISTS == error ||
			                        ERROR_FILE_EXISTS == error
			                ? WF_CF_EXISTS
			                : WF_CF_COMMIT_FAILED,
			        error);
			goto done;
		}
		tmp_made = false;
		wf_cj_gone(r->jr, &tmp_stamp.id);
	} else {
		/*
		 * Overwrite, on the objects that were checked and held, never on a
		 * name looked up again: the file being replaced is opened, must still
		 * be what was checked, and is moved aside by its handle; the verified
		 * temporary (still held) takes the name by its handle; only then is
		 * the old file removed, on its own FileId. Nothing replaces anything:
		 * if the name was taken meanwhile, the old file is put back.
		 */
		Stamp now;
		HANDLE old = INVALID_HANDLE_VALUE;
		wchar_t *aside = NULL, *aside_x = NULL;
		bool journalled = false;

		old = CreateFileW(dst_x,
		        DELETE | FILE_READ_ATTRIBUTES | READ_CONTROL,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		        NULL, OPEN_EXISTING,
		        FILE_FLAG_OPEN_REPARSE_POINT |
		                FILE_FLAG_BACKUP_SEMANTICS,
		        NULL);
		if (INVALID_HANDLE_VALUE == old || !stamp_of(old, &now) ||
		        (now.attributes &
		                (FILE_ATTRIBUTE_DIRECTORY |
		                        FILE_ATTRIBUTE_REPARSE_POINT |
		                        FILE_ATTRIBUTE_READONLY |
		                        FILE_ATTRIBUTE_ENCRYPTED |
		                        CLOUD_ATTRIBUTES)) ||
		        now.links > 1 || !same_id(&now.id, &d0.id) ||
		        now.size != d0.size || now.mtime != d0.mtime) {
			error = INVALID_HANDLE_VALUE == old ? GetLastError()
			                                    : 0;
			if (INVALID_HANDLE_VALUE != old) {
				CloseHandle(old);
			}
			note_failure(r, rel, WF_CF_DEST_CHANGED, error);
			goto done;
		}
		if (!copy_dacl(old, out, &error)) {
			CloseHandle(old);
			note_failure(r, rel, WF_CF_COMMIT_FAILED, error);
			goto done;
		}
		aside = temp_path(dst_dir, L".old");
		aside_x = aside ? xp(aside) : NULL;
		/* The old version is the user's, not ours: it is recorded as such
		 * (A) before it is moved, so that a crash between the two renames
		 * puts it back instead of removing it. If it cannot be recorded
		 * nothing is moved. */
		journalled = aside_x && wf_cj_aside(r->jr, dst, aside, &d0.id);
		if (!aside_x || !journalled ||
		        !rename_noclobber(old, aside_x, &error)) {
			if (!error) {
				error = GetLastError();
			}
			CloseHandle(old);
			free(aside_x);
			free(aside);
			if (journalled) {
				(void)wf_cj_gone(r->jr, &d0.id);
			}
			note_failure(r, rel, WF_CF_COMMIT_FAILED, error);
			goto done;
		}
		if (wf_copyengine_test_after_aside) {
			wf_copyengine_test_after_aside(dst, aside);
		}
		if (!rename_noclobber(out, dst_x, &error)) {
			DWORD back = 0;

			if (rename_noclobber(old, dst_x, &back)) {
				wf_cj_gone(r->jr, &d0.id);
				note_failure(
				        r, rel, WF_CF_COMMIT_FAILED, error);
			} else {
				/* both survive, the old one under the aside name */
				note_failure(
				        r, rel, WF_CF_COMMIT_FAILED, error);
				tmp_made =
				        false; /* the new copy is kept, journalled */
			}
			CloseHandle(old);
			free(aside_x);
			free(aside);
			goto done;
		}
		CloseHandle(old);
		tmp_made = false;
		wf_cj_gone(r->jr, &tmp_stamp.id);
		/* the replacement is in place: the previous version may now go
		 * (D), and is removed on its own FileId; if that is not possible
		 * it stays under its aside name, still recorded */
		if (wf_cj_discard(r->jr, &d0.id)) {
			WfOwnedResult gone =
			        wf_owned_delete_save_temp(aside, &d0.id, NULL);

			if (WF_OWNED_DELETED == gone ||
			        WF_OWNED_ABSENT == gone) {
				wf_cj_gone(r->jr, &d0.id);
			}
		}
		free(aside_x);
		free(aside);
		r->res->overwritten++;
	}
	verified_streams = streams.n;
	if (r->stage && !r->plan.removes_source &&
	        INVALID_HANDLE_VALUE != out) {
		/* finished and verified: what it is now is what may be published */
		CloseHandle(out);
		out = INVALID_HANDLE_VALUE;
		have_seal = seal_of(dst_x, &tmp_stamp.id, &sealed);
	}
	if (r->plan.removes_source) {
		CloseHandle(out);
		out = INVALID_HANDLE_VALUE;
		HANDLE guard = CreateFileW(dst_x, GENERIC_READ,
		        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
		        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
		Stamp made = {0};
		bool ok = guard != INVALID_HANDLE_VALUE &&
		        stamp_of(guard, &made) &&
		        same_id(&made.id, &tmp_stamp.id) &&
		        compare_stream(r, in, guard, &s0.id, &made.id, s0.size,
		                &error);
		if (!ok ||
		        !proof_add(r, src_x, dst_x, &in, guard, &s0, &made,
		                &streams)) {
			if (guard != INVALID_HANDLE_VALUE) {
				CloseHandle(guard);
			}
			note_failure(r, rel, WF_CF_VERIFY_FAILED,
			        error ? error : ERROR_NOT_ENOUGH_MEMORY);
		} else {
			sealed = made;
			have_seal = true;
		}
	}
	r->res->streams_kept += verified_streams;
	if (!r->made_ok) {
		r->made_id = tmp_stamp.id;
		r->made_ok = true;
	}
	r->res->files_copied++;
	r->res->bytes += copied;
	r->files_done++;
	report_progress(r, leaf);
	result = OUT_OK;
	if (r->stage && !have_seal) {
		/* a file that cannot be sealed cannot be shown unchanged later:
		 * the folder is not published on its word */
		note_failure(r, rel, WF_CF_VERIFY_FAILED, GetLastError());
	}
	if (!manifest_add(r, dst_x, &tmp_stamp.id, false,
	            have_seal ? &sealed : NULL)) {
		note_failure(
		        r, rel, WF_CF_WRITE_FAILED, ERROR_NOT_ENOUGH_MEMORY);
	}
done:
	streams_close(&streams);
	if (INVALID_HANDLE_VALUE != out) {
		CloseHandle(out);
	}
	if (INVALID_HANDLE_VALUE != in) {
		CloseHandle(in);
	}
	if (tmp_made && tmp_x) {
		drop_temp(r, tmp, &tmp_stamp.id);
	}
	free(src_x);
	free(dst);
	free(dst_x);
	free(dir_x);
	free(tmp_x);
	free(tmp);
	if (OUT_FAILED == result) {
		r->res->files_failed++;
	}
	return result;
}

void (*wf_copyengine_test_after_aside)(
        const wchar_t *dest, const wchar_t *aside);
bool (*wf_copyengine_test_tmp_id_unreadable)(void);

static Outcome copy_tree_item(Run *r,
        const wchar_t *src,
        const wchar_t *dst_dir,
        const wchar_t *leaf,
        const WfNodeInfo *snap,
        bool is_dir,
        const wchar_t *rel,
        unsigned depth);

/* What stands at a name: nothing, a plain folder (not a link), or something
 * else (a file, a link, or what could not be told: it is taken). */
typedef enum
{
	DEST_NONE,
	DEST_FOLDER,
	DEST_OTHER
} DestKind;

static DestKind dest_kind(const wchar_t *path_x)
{
	DWORD a = GetFileAttributesW(path_x), e;

	if (INVALID_FILE_ATTRIBUTES == a) {
		e = GetLastError();
		return ERROR_FILE_NOT_FOUND == e || ERROR_PATH_NOT_FOUND == e
		        ? DEST_NONE
		        : DEST_OTHER;
	}
	return (a & FILE_ATTRIBUTE_DIRECTORY) &&
	                !(a & FILE_ATTRIBUTE_REPARSE_POINT)
	        ? DEST_FOLDER
	        : DEST_OTHER;
}

static DestKind dest_kind_at(const wchar_t *dir, const wchar_t *leaf)
{
	wchar_t *p = NULL, *x;
	DestKind k = DEST_OTHER; /* when unsure, it is taken */

	if (WF_OK != wf_path_join(dir, leaf, &p)) {
		return DEST_OTHER;
	}
	x = xp(p);
	if (x) {
		k = dest_kind(x);
		free(x);
	}
	free(p);
	return k;
}

/* Whether a folder that is already there is entered: only when the job asks
 * for it, only for a copy, never under "stop", and never inside a staged tree
 * (a tree being staged is new). */
static bool merging(const Run *r)
{
	return r->spec->merge_folders && WF_CJ_COPY == r->spec->kind &&
	        WF_COLLIDE_STOP != r->spec->collide && !r->stage;
}

static wchar_t *unique_leaf(
        const wchar_t *dest, const wchar_t *name, bool is_dir);
static void add_created(Run *r,
        const wchar_t *dst_dir,
        const WfFileId *parent,
        const wchar_t *leaf,
        bool dir);

static Outcome copy_dir_into(Run *r,
        const wchar_t *src,
        const wchar_t *dst_dir,
        const wchar_t *leaf,
        const WfNodeInfo *snap,
        const wchar_t *rel,
        unsigned depth)
{
	wchar_t *dst = NULL, *dst_x = NULL, *src_x = NULL, *pattern = NULL;
	HANDLE sh = INVALID_HANDLE_VALUE, dh = INVALID_HANDLE_VALUE, find;
	WIN32_FIND_DATAW fd;
	Outcome result = OUT_FAILED;
	Stamp ss;
	DWORD error;
	Streams streams;
	bool merge = false; /* the folder was there: entered, not made */
	WfFileId here = {
	        0}; /* the folder entered, as it was held (merge only) */

	if (depth >= MAX_DEPTH) {
		note_failure(r, rel, WF_CF_TOO_DEEP, 0);
		r->res->files_failed++;
		return OUT_FAILED;
	}
	if (WF_OK != wf_path_join(dst_dir, leaf, &dst) || !(dst_x = xp(dst)) ||
	        !(src_x = xp(src))) {
		note_failure(
		        r, rel, WF_CF_WRITE_FAILED, ERROR_NOT_ENOUGH_MEMORY);
		goto done;
	}
	/* held without delete sharing: while its contents are read it cannot be
	 * renamed or swapped for a link, so the paths below it stay in it */
	sh = CreateFileW(src_x, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == sh || !stamp_of(sh, &ss)) {
		note_failure(r, rel, WF_CF_SOURCE_UNREADABLE, GetLastError());
		goto done;
	}
	if (ss.attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		r->res->links_skipped++;
		note_failure(r, rel, WF_CF_LINK_SKIPPED, 0);
		goto done;
	}
	if (!list_streams(src_x, &streams, &error) || streams.n) {
		note_failure(r, rel, WF_CF_SOURCE_UNREADABLE,
		        error ? error : ERROR_NOT_SUPPORTED);
		goto done;
	}
	if (snap && !wf_info_matches(&snap->id, &ss.id)) {
		note_failure(r, rel, WF_CF_SOURCE_CHANGED, 0);
		goto done;
	}
	if (DEST_FOLDER == dest_kind(dst_x) && merging(r)) {
		Stamp there;

		/* An existing folder is entered, as it is: held without delete
		 * sharing while it is filled, shown to be a plain folder and not the
		 * folder being copied. Nothing of it is made, changed or recorded. */
		merge = true;
		dh = CreateFileW(dst_x,
		        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        FILE_FLAG_BACKUP_SEMANTICS |
		                FILE_FLAG_OPEN_REPARSE_POINT,
		        NULL);
		if (INVALID_HANDLE_VALUE == dh || !stamp_of(dh, &there)) {
			note_failure(r, rel, WF_CF_DEST_UNSAFE, GetLastError());
			goto done;
		}
		if (!(there.attributes & FILE_ATTRIBUTE_DIRECTORY) ||
		        (there.attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
		        same_id(&there.id, &ss.id)) {
			note_failure(
			        r, rel, WF_CF_DEST_UNSAFE, ERROR_INVALID_DATA);
			goto done;
		}
		here.kind = WF_ID_STRONG;
		here.volume = there.id.VolumeSerialNumber;
		memcpy(here.id, there.id.FileId.Identifier, sizeof here.id);
	} else if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(dst_x)) {
		if (WF_COLLIDE_SKIP == r->spec->collide) {
			r->res->files_skipped++;
			r->tree_clean = r->tree_may_remove = false;
			result = OUT_SKIPPED;
			goto done;
		}
		note_failure(r, rel, WF_CF_EXISTS,
		        0); /* not merged: see merging() */
		goto done;
	}
	if (!merge) {
		if (!CreateDirectoryW(dst_x, NULL)) {
			note_failure(
			        r, rel, WF_CF_WRITE_FAILED, GetLastError());
			goto done;
		}
		r->res->dirs_created++;
		/* held from the moment it exists, so nothing can be put in its
		 * place while it is filled; it must be a plain folder */
		dh = CreateFileW(dst_x,
		        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES |
		                FILE_WRITE_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        FILE_FLAG_BACKUP_SEMANTICS |
		                FILE_FLAG_OPEN_REPARSE_POINT,
		        NULL);
		{
			Stamp made;

			if (INVALID_HANDLE_VALUE != dh && stamp_of(dh, &made) &&
			        !r->made_ok) {
				r->made_id = made.id;
				r->made_ok = true;
			}
			if (INVALID_HANDLE_VALUE == dh ||
			        !stamp_of(dh, &made) ||
			        !(made.attributes & FILE_ATTRIBUTE_DIRECTORY) ||
			        (made.attributes &
			                FILE_ATTRIBUTE_REPARSE_POINT)) {
				note_failure(r, rel, WF_CF_WRITE_FAILED,
				        INVALID_HANDLE_VALUE == dh
				                ? GetLastError()
				                : ERROR_INVALID_DATA);
				goto done;
			}
		}
		if (!manifest_add_folder(r, dst_x) && r->stage) {
			note_failure(r, rel, WF_CF_WRITE_FAILED,
			        ERROR_NOT_ENOUGH_MEMORY);
			goto done;
		}
	}
	pattern = wf_malloc_array((wcslen(src_x) + 3), sizeof *pattern);
	if (!pattern) {
		note_failure(
		        r, rel, WF_CF_WRITE_FAILED, ERROR_NOT_ENOUGH_MEMORY);
		goto done;
	}
	swprintf(pattern, wcslen(src_x) + 3, L"%ls\\*", src_x);
	find = FindFirstFileExW(pattern, FindExInfoBasic, &fd,
	        FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
	if (INVALID_HANDLE_VALUE == find) {
		error = GetLastError();
		if (ERROR_FILE_NOT_FOUND != error) {
			note_failure(r, rel, WF_CF_SOURCE_UNREADABLE, error);
			goto done;
		}
	} else {
		do {
			wchar_t *child_src, *child_rel;
			Outcome o;

			if (!wcscmp(fd.cFileName, L".") ||
			        !wcscmp(fd.cFileName, L"..")) {
				continue;
			}
			if (!keep_going(r)) {
				FindClose(find);
				result = OUT_CANCELLED;
				goto done;
			}
			child_rel = rel_join(rel, fd.cFileName);
			child_src = NULL;
			if (!child_rel ||
			        WF_OK !=
			                wf_path_join(src, fd.cFileName,
			                        &child_src)) {
				/* a listing that is not whole is not a copy of the folder */
				free(child_rel);
				free(child_src);
				FindClose(find);
				note_failure(r, rel, WF_CF_WRITE_FAILED,
				        ERROR_NOT_ENOUGH_MEMORY);
				goto done;
			}
			listing_add(r, child_rel, &fd);
			if (fd.dwFileAttributes &
			        FILE_ATTRIBUTE_REPARSE_POINT) {
				r->res->links_skipped++;
				note_failure(
				        r, child_rel, WF_CF_LINK_SKIPPED, 0);
				free(child_rel);
				free(child_src);
				continue;
			}
			{
				bool child_dir =
				        (fd.dwFileAttributes &
				                FILE_ATTRIBUTE_DIRECTORY) != 0;
				const wchar_t *leaf_use = fd.cFileName;
				wchar_t *renamed = NULL;
				size_t replaced_before = r->res->overwritten;

				if (merge &&
				        WF_COLLIDE_RENAME == r->spec->collide) {
					DestKind there =
					        dest_kind_at(dst, fd.cFileName);

					/* keep both: a name that is taken, other than a folder
					 * meeting a folder (which is entered), gets a free one */
					if (DEST_NONE != there &&
					        !(child_dir &&
					                DEST_FOLDER == there)) {
						renamed = unique_leaf(dst,
						        fd.cFileName,
						        child_dir);
						if (!renamed) {
							note_failure(r,
							        child_rel,
							        WF_CF_EXISTS,
							        0);
							r->res->files_failed++;
							free(child_rel);
							free(child_src);
							continue;
						}
						leaf_use = renamed;
					}
				}
				if (merge) {
					r->made_ok =
					        false; /* each child proves its own making */
				}
				o = copy_tree_item(r, child_src, dst, leaf_use,
				        NULL, child_dir, child_rel, depth + 1);
				/* what a merge made is registered one by one: the folder it
				 * went into was there before and is not ours to undo */
				if (merge && OUT_OK == o &&
				        r->res->overwritten ==
				                replaced_before) {
					add_created(r, dst, &here, leaf_use,
					        child_dir);
				}
				free(renamed);
			}
			free(child_rel);
			free(child_src);
			if (OUT_CANCELLED == o) {
				FindClose(find);
				result = OUT_CANCELLED;
				goto done;
			}
		} while (FindNextFileW(find, &fd));
		/* the listing ended: because it was complete, or because reading
		 * it failed. A folder that was only partly listed is not a copy of
		 * the folder, and must never be taken for one (a move would then
		 * remove the source with what was not copied). */
		error = GetLastError();
		FindClose(find);
		if (ERROR_NO_MORE_FILES != error) {
			note_failure(r, rel, WF_CF_SOURCE_UNREADABLE, error);
			goto done;
		}
	}
	/* the folder takes the source's times and attributes once it is full (one
	 * that was there keeps its own) */
	if (!merge && !set_dest_times(dh, sh)) {
		note_failure(r, rel, WF_CF_METADATA_FAILED, GetLastError());
		goto done;
	}
	if (r->plan.removes_source) {
		HANDLE guard = CreateFileW(src_x, FILE_READ_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		        NULL, OPEN_EXISTING,
		        FILE_FLAG_BACKUP_SEMANTICS |
		                FILE_FLAG_OPEN_REPARSE_POINT,
		        NULL);
		if (guard == INVALID_HANDLE_VALUE ||
		        !proof_add(r, src_x, NULL, &guard, NULL, &ss, NULL,
		                NULL)) {
			if (guard != INVALID_HANDLE_VALUE) {
				CloseHandle(guard);
			}
			note_failure(r, rel, WF_CF_VERIFY_FAILED,
			        ERROR_NOT_ENOUGH_MEMORY);
			goto done;
		}
	}
	result = OUT_OK;
done:
	if (INVALID_HANDLE_VALUE != dh) {
		CloseHandle(dh);
	}
	if (INVALID_HANDLE_VALUE != sh) {
		CloseHandle(sh);
	}
	free(pattern);
	free(dst);
	free(dst_x);
	free(src_x);
	return result;
}

static void staging_leaf(wchar_t *out, size_t cap)
{
	unsigned char rnd[8] = {0};
	unsigned long long v = 0;
	size_t i;

	(void)BCryptGenRandom(
	        NULL, rnd, sizeof rnd, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	for (i = 0; i < sizeof rnd; i++) {
		v = v << 8 | rnd[i];
	}
	swprintf(out, cap, L".wfcopy-%016llx.dir", v);
}

/* The staging folder gets the final name by one rename that cannot replace. */
static bool commit_staging(const wchar_t *stage_x,
        const FILE_ID_INFO *stage_id,
        const wchar_t *final_x,
        DWORD *error)
{
	FILE_ID_INFO now;
	HANDLE h = CreateFileW(stage_x, DELETE | FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	bool ok = false;

	if (INVALID_HANDLE_VALUE == h) {
		*error = GetLastError();
		return false;
	}
	if (!wf_handle_file_id_info(h, &now) || !same_id(&now, stage_id)) {
		*error = ERROR_INVALID_DATA;
		CloseHandle(h);
		return false;
	}
	ok = rename_noclobber(h, final_x, error);
	CloseHandle(h);
	return ok;
}

/* Errors a scanner, an indexer or a sync client cause by holding a file of
 * the finished tree for a moment; they pass by themselves. */
static bool commit_error_is_transient(DWORD error)
{
	return ERROR_SHARING_VIOLATION == error ||
	        ERROR_LOCK_VIOLATION == error || ERROR_ACCESS_DENIED == error;
}

/*
 * Everything the folder copy made is still what it was when it was finished:
 * the same object under the same name (not a link, not another file put in
 * its place) and, for a file, the same size, attributes and ChangeTime. A
 * writer that sets LastWriteTime back still moves ChangeTime. Reading, which
 * is all a scanner does, changes none of this.
 */
static bool staged_unchanged(const Manifest *m, DWORD *error)
{
	for (size_t i = 0; i < m->n; i++) {
		const Made *v = &m->v[i];
		Stamp now;
		HANDLE h = CreateFileW(v->path_x, FILE_READ_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		        NULL, OPEN_EXISTING,
		        FILE_FLAG_BACKUP_SEMANTICS |
		                FILE_FLAG_OPEN_REPARSE_POINT,
		        NULL);
		bool same;

		if (INVALID_HANDLE_VALUE == h) {
			*error = GetLastError();
			return false;
		}
		same = stamp_of(h, &now) && same_id(&now.id, &v->id) &&
		        !(now.attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
		        (!v->sealed ||
		                (now.size == v->size &&
		                        now.change == v->change &&
		                        now.attributes == v->attributes));
		CloseHandle(h);
		if (!same) {
			*error = ERROR_INVALID_DATA;
			return false;
		}
	}
	return true;
}

/* Waits `ms` in short steps so that a cancel is answered at once. */
static bool wait_keeping_going(Run *r, DWORD ms)
{
	while (ms) {
		DWORD step = ms < 50 ? ms : 50;

		Sleep(step);
		ms -= step;
		if (!keep_going(r)) {
			return false;
		}
	}
	return true;
}

/*
 * commit_staging() again after a short wait while the error is transient.
 * A failed rename changed nothing, so trying again cannot replace or lose
 * anything; what it avoids is taking a whole, verified copy apart because
 * something else had a file open for a few milliseconds. Before every
 * renewed attempt the staged tree is shown to be what was verified, and the
 * staging folder is opened and checked by FileId again inside
 * commit_staging(), so the folder that is published is the one that was
 * verified, never one put in its place or changed during the wait. The wait
 * is bounded (about 1.5 s), answers a cancel within 50 ms, and honours a
 * pause.
 */
static bool commit_staging_retry(Run *r,
        const Manifest *m,
        const wchar_t *stage_x,
        const FILE_ID_INFO *stage_id,
        const wchar_t *final_x,
        DWORD *error)
{
	static const DWORD wait_ms[] = {0, 20, 50, 100, 200, 400, 800};

	for (size_t i = 0; i < WF_ARRAY_LEN(wait_ms); i++) {
		if (i) {
			if (!wait_keeping_going(r, wait_ms[i])) {
				*error = ERROR_OPERATION_ABORTED;
				return false;
			}
			if (!staged_unchanged(m, error)) {
				return false;
			}
		} else if (!keep_going(r)) {
			*error = ERROR_OPERATION_ABORTED;
			return false;
		}
		if (commit_staging(stage_x, stage_id, final_x, error)) {
			return true;
		}
		if (!commit_error_is_transient(*error)) {
			return false;
		}
	}
	return false;
}

/*
 * A top-level folder is built under a hidden name next to its destination and
 * only given its real name once it is whole: the real name never shows a
 * half-copied folder. If anything goes wrong (or the job is cancelled) the
 * staged tree is taken apart again, object by object, each on its own FileId;
 * if that cannot be done completely the rest stays under the hidden name and
 * the result says so.
 */
static Outcome copy_dir_staged(Run *r,
        const wchar_t *src,
        const wchar_t *dst_dir,
        const wchar_t *leaf,
        const WfNodeInfo *snap,
        const wchar_t *rel,
        unsigned depth)
{
	wchar_t stage_name[40], *final_p = NULL, *final_x = NULL,
	                        *stage_p = NULL, *stage_x = NULL;
	Manifest m;
	WfCopyResult before = *r->res;
	size_t hard_before = r->hard_errors;
	Outcome o = OUT_FAILED;
	DWORD error = 0;

	memset(&m, 0, sizeof m);
	if (WF_OK != wf_path_join(dst_dir, leaf, &final_p) ||
	        !(final_x = xp(final_p))) {
		note_failure(
		        r, rel, WF_CF_WRITE_FAILED, ERROR_NOT_ENOUGH_MEMORY);
		goto done;
	}
	if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(final_x)) {
		if (WF_COLLIDE_SKIP == r->spec->collide) {
			r->res->files_skipped++;
			r->tree_clean = r->tree_may_remove = false;
			o = OUT_SKIPPED;
		} else {
			note_failure(r, rel, WF_CF_EXISTS,
			        0); /* folders are never merged */
		}
		goto done;
	}
	staging_leaf(stage_name, WF_ARRAY_LEN(stage_name));
	if (WF_OK != wf_path_join(dst_dir, stage_name, &stage_p) ||
	        !(stage_x = xp(stage_p))) {
		note_failure(
		        r, rel, WF_CF_WRITE_FAILED, ERROR_NOT_ENOUGH_MEMORY);
		goto done;
	}
	r->stage = &m;
	o = copy_dir_into(r, src, dst_dir, stage_name, snap, rel, depth);
	r->stage = NULL;
	if (OUT_OK == o && r->hard_errors == hard_before && !r->cancelled) {
		if (r->plan.removes_source) {
			proofs_release_destinations(r);
		}
		if (m.n && !m.lost &&
		        commit_staging_retry(
		                r, &m, stage_x, &m.v[0].id, final_x, &error)) {
			if (r->plan.removes_source &&
			        !proofs_reopen_destinations(
			                r, stage_x, final_x)) {
				note_failure(r, rel, WF_CF_VERIFY_FAILED,
				        ERROR_INVALID_DATA);
			}
			goto done; /* whole, and now under its own name */
		}
		if (!r->cancelled) { /* a cancel is not a failed commit */
			/* the name was taken; or what was verified is no longer what
			 * is there (another object, changed content): not published */
			note_failure(r, rel,
			        ERROR_ALREADY_EXISTS == error ||
			                        ERROR_FILE_EXISTS == error
			                ? WF_CF_EXISTS
			                : ERROR_INVALID_DATA == error
			                ? WF_CF_VERIFY_FAILED
			                : WF_CF_COMMIT_FAILED,
			        error);
		}
		o = OUT_FAILED;
	}
	if (OUT_OK == o) {
		o = OUT_FAILED;
	}
	/* not whole: nothing of it may stay, and the counts say nothing was
	 * copied from this folder */
	{
		bool gone = m.n ? manifest_undo(&m) : !m.lost;

		r->res->files_copied = before.files_copied;
		r->res->bytes = before.bytes;
		r->res->dirs_created = before.dirs_created;
		r->res->overwritten = before.overwritten;
		r->res->streams_kept = before.streams_kept;
		r->res->streams_skipped = before.streams_skipped;
		if (!gone) {
			note_failure(r, rel, WF_CF_STAGING_LEFT, 0);
		}
	}
	if (r->cancelled) {
		o = OUT_CANCELLED;
	}
done:
	manifest_free(&m);
	free(final_p);
	free(final_x);
	free(stage_p);
	free(stage_x);
	return o;
}

static Outcome copy_dir(Run *r,
        const wchar_t *src,
        const wchar_t *dst_dir,
        const wchar_t *leaf,
        const WfNodeInfo *snap,
        const wchar_t *rel,
        unsigned depth)
{
	/* inside a staged tree the folders are built in place. In a merge, a folder
	 * that is there is entered where it is (nothing is renamed over it), and a
	 * folder that is new, at any depth, is staged like a top-level one: the
	 * name never shows it half made, and a cancel or a failure takes it apart. */
	if (r->stage || (depth > 0 && !merging(r)) ||
	        (merging(r) && DEST_FOLDER == dest_kind_at(dst_dir, leaf))) {
		return copy_dir_into(r, src, dst_dir, leaf, snap, rel, depth);
	}
	return copy_dir_staged(r, src, dst_dir, leaf, snap, rel, depth);
}

static Outcome copy_tree_item(Run *r,
        const wchar_t *src,
        const wchar_t *dst_dir,
        const wchar_t *leaf,
        const WfNodeInfo *snap,
        bool is_dir,
        const wchar_t *rel,
        unsigned depth)
{
	return is_dir ? copy_dir(r, src, dst_dir, leaf, snap, rel, depth)
	              : copy_file(r, src, dst_dir, leaf, snap, rel);
}

/* Registers what this job made, for Undo, only if the object at the name is
 * the very one that was made: the identity taken when it was made must be the
 * identity found now. A name that was swapped meanwhile is not registered. */
static void add_created(Run *r,
        const wchar_t *dst_dir,
        const WfFileId *parent,
        const wchar_t *leaf,
        bool dir)
{
	WfCopyResult *res = r->res;
	WfCopyCreated *grown = res->created;
	wchar_t *path = NULL, *x;
	HANDLE h;
	FILE_ID_INFO id;

	if (r->created_cap < res->created_n) {
		r->created_cap =
		        res->created_n; /* what the array already holds */
	}
	if (res->created_n ==
	        r->created_cap) { /* grows by doubling, not by one */
		size_t cap = r->created_cap ? r->created_cap * 2 : 16;

		grown = wf_realloc_array(res->created, cap, sizeof *grown);
		if (!grown) {
			return; /* res->created is as it was */
		}
		res->created = grown;
		r->created_cap = cap;
	}
	if (WF_OK != wf_path_join(dst_dir, leaf, &path)) {
		return;
	}
	x = xp(path);
	h = x ? CreateFileW(x, FILE_READ_ATTRIBUTES,
	                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                NULL, OPEN_EXISTING,
	                FILE_FLAG_BACKUP_SEMANTICS |
	                        FILE_FLAG_OPEN_REPARSE_POINT,
	                NULL)
	      : INVALID_HANDLE_VALUE;
	free(x);
	if (INVALID_HANDLE_VALUE == h || !wf_handle_file_id_info(h, &id)) {
		if (INVALID_HANDLE_VALUE != h) {
			CloseHandle(h);
		}
		free(path);
		return;
	}
	CloseHandle(h);
	if (!r->made_ok ||
	        id.VolumeSerialNumber != r->made_id.VolumeSerialNumber ||
	        memcmp(id.FileId.Identifier, r->made_id.FileId.Identifier,
	                sizeof id.FileId.Identifier)) {
		free(path);
		return; /* not proven to be ours: no Undo entry for it */
	}
	grown[res->created_n].path = path;
	/* a weak identity keeps its kind: it is never the authority for Undo, which
	 * removes */
	wf_fileid_from_info(&id, &grown[res->created_n].id);
	grown[res->created_n].parent = *parent;
	grown[res->created_n].dir = dir;
	res->created_n++;
}

/* Whether `dest`\`leaf` is taken. */
static bool name_taken(const wchar_t *dest, const wchar_t *leaf)
{
	wchar_t *p = NULL, *x;
	bool taken = true; /* when unsure, it is */

	if (WF_OK != wf_path_join(dest, leaf, &p)) {
		return true;
	}
	x = xp(p);
	if (x) {
		taken = INVALID_FILE_ATTRIBUTES != GetFileAttributesW(x) ||
		        ERROR_FILE_NOT_FOUND != GetLastError();
		free(x);
	}
	free(p);
	return taken;
}

/* Keep both: "a.txt" -> "a (2).txt", "docs" -> "docs (2)", up to (999): the
 * first name nothing has. A leading dot is part of the stem. */
static wchar_t *unique_leaf(
        const wchar_t *dest, const wchar_t *name, bool is_dir)
{
	size_t n = wcslen(name), dot = n;

	if (!is_dir) {
		for (size_t i = n; i > 1; i--) {
			if (L'.' == name[i - 1]) {
				dot = i - 1;
				break;
			}
		}
	}
	for (unsigned k = 2; k <= 999; k++) {
		wchar_t *leaf = malloc((n + 16) * sizeof *leaf);

		if (!leaf) {
			return NULL;
		}
		swprintf(leaf, n + 16, L"%.*ls (%u)%ls", (int)dot, name, k,
		        name + dot);
		if (!name_taken(dest, leaf)) {
			return leaf;
		}
		free(leaf);
	}
	return NULL;
}

/* A FAT volume gives only a weak identity (wf_weakid_win.h), which can repeat
 * after a delete. What it may take part in is a copy that leaves what exists
 * as it is: reading from it, and making new names on it. Not a move (the
 * source is removed, and FAT has no Recycle Bin), not a recycle, not an
 * extraction, and not a replacement of a file on a weak volume. */
static bool weak_job_refused(const WfCopySpec *spec)
{
	bool weak = wf_fileid_is_weak(&spec->dest_id) ||
	        wf_fileid_is_weak(&spec->slot.dir_id);
	bool folder = false;

	for (size_t i = 0; i < spec->slot.count; i++) {
		weak = weak || wf_fileid_is_weak(&spec->slot.src[i].snap.id);
		folder = folder || (spec->slot.src[i].snap.flags & WF_ENT_DIR);
	}
	/* a folder goes in under a hidden name and is renamed into place, and a
	 * weak identity does not survive a rename */
	return (weak && WF_CJ_COPY != spec->kind) ||
	        (wf_fileid_is_weak(&spec->dest_id) &&
	                (folder || WF_COLLIDE_OVERWRITE == spec->collide));
}

WfStatus wf_copyengine_run(const WfCopySpec *spec,
        const WfCopyControl *control,
        WfCopyJournal *journal,
        WfCopyResult *out)
{
	Run run;
	size_t i;
	bool com = false;
	WfTransferKind kind;
	HANDLE src_pin = INVALID_HANDLE_VALUE, dest_pin = INVALID_HANDLE_VALUE;

	if (!spec || !out || (!spec->dest && WF_CJ_RECYCLE != spec->kind) ||
	        !spec->slot.count) {
		return WF_E_INVAL;
	}
	memset(out, 0, sizeof *out);
	out->kind = spec->kind;
	out->verify = spec->verify;
	out->dest_id = spec->dest_id;
	out->state = WF_CS_FAILED;
#if !defined(WF_COPYENGINE_QUARANTINE_TEST)
	/* Source retirement and the Shell recycle work on names, which cannot be
	 * bound atomically to the verified identities (DR-DRAFT-015). What the
	 * product permits is decided in one place, wf_copyjob_allowed. */
	if (!wf_copyjob_allowed(spec->kind, spec->collide)) {
		out->files_failed = spec->slot.count;
		out->sources_untouched = spec->slot.count;
		return WF_E_ACCESS;
	}
#endif
	if (weak_job_refused(spec)) {
		out->files_failed = spec->slot.count;
		out->sources_untouched = spec->slot.count;
		return WF_E_ACCESS;
	}
	memset(&run, 0, sizeof run);
	run.spec = spec;
	run.ctl = control;
	run.jr = journal;
	run.res = out;
	kind = WF_CJ_MOVE_ACROSS == spec->kind ? WF_KIND_MOVE_CROSS_VOLUME
	                                       : WF_KIND_COPY;
	if (WF_OK != wf_verify_plan(kind, spec->verify, &run.plan)) {
		return WF_E_INVAL;
	}
	run.buffer = malloc(CHUNK);
	if (!run.buffer) {
		return WF_E_NOMEM;
	}
	/*
	 * The folders the job reads from and writes to are the ones that were
	 * checked, and are held until it ends (an id that was only proven in the
	 * listing is worth nothing if the path is looked up again later).
	 */
	{
		DWORD pin_error = 0;
		bool ok = pin_dir(spec->slot.dir, &spec->slot.dir_id, &src_pin,
		        &pin_error);

		if (ok && WF_CJ_RECYCLE != spec->kind) {
			ok = pin_dir(spec->dest, &spec->dest_id, &dest_pin,
			        &pin_error);
		}
		if (!ok) {
			for (i = 0; i < spec->slot.count; i++) {
				note_failure(&run, spec->slot.src[i].name,
				        WF_CF_DEST_CHANGED, pin_error);
				out->files_failed++;
			}
			out->state = WF_CS_FAILED;
			if (INVALID_HANDLE_VALUE != src_pin) {
				CloseHandle(src_pin);
			}
			free(run.buffer);
			return WF_OK;
		}
	}
	if (WF_CJ_MOVE_ACROSS == spec->kind || WF_CJ_RECYCLE == spec->kind) {
		com = wf_recycle_thread_init();
	}
	out->state = WF_CS_RUNNING;
	if (WF_CJ_RECYCLE == spec->kind && spec->slot.count) {
		out->item_done = calloc(spec->slot.count, 1);
		out->item_n = out->item_done ? spec->slot.count : 0;
	}
	for (i = 0; i < spec->slot.count && !run.cancelled; i++) {
		const WfSourceSnap *item = &spec->slot.src[i];
		wchar_t *src = NULL, *renamed = NULL;
		const wchar_t *leaf_use = item->name;
		size_t replaced_before;
		Outcome o;
		bool is_dir = 0 != (item->snap.flags & WF_ENT_DIR), merge_top;

		proofs_close(&run);
		if (!keep_going(&run)) {
			break;
		}
		if (WF_OK != wf_path_join(spec->slot.dir, item->name, &src)) {
			note_failure(&run, item->name, WF_CF_SOURCE_UNREADABLE,
			        ERROR_NOT_ENOUGH_MEMORY);
			out->files_failed++;
			continue;
		}
		run.tree_clean = run.tree_may_remove = true;
		run.src_sum = 0;
		run.src_n = 0;
		run.made_ok = false;
		if (WF_CJ_RECYCLE == spec->kind) {
			WfRecycleReport rr = {0};
			WfStatus st = com
			        ? wf_recycle_guarded(src, &item->snap.id,
			                  &spec->slot.dir_id, ready_control,
			                  &run, &rr)
			        : WF_E_OS;

			if (WF_OK == st) {
				out->sources_recycled++;
				if (out->item_done) {
					out->item_done[i] = 1;
				}
			} else {
				out->sources_kept++;
				if (rr.attempted) {
					out->sources_uncertain++;
				} else {
					out->sources_untouched++;
				}
				note_failure(&run, item->name,
				        rr.swapped ? WF_CF_REPLACED_IN_BIN
				                   : WF_CF_NOT_REMOVED,
				        rr.error ? rr.error
				                 : (unsigned long)rr.hresult);
			}
			(void)keep_going(&run);
			free(src);
			continue;
		}
		if (item->snap.flags & WF_ENT_LINK) {
			out->links_skipped++;
			note_failure(&run, item->name, WF_CF_LINK_SKIPPED, 0);
			free(src);
			continue;
		}
		merge_top = is_dir && merging(&run) &&
		        DEST_FOLDER == dest_kind_at(spec->dest, item->name);
		if (WF_COLLIDE_RENAME == spec->collide && !merge_top &&
		        name_taken(spec->dest, item->name)) {
			renamed = unique_leaf(spec->dest, item->name, is_dir);
			if (!renamed) {
				note_failure(&run, item->name, WF_CF_EXISTS, 0);
				out->files_failed++;
				free(src);
				continue;
			}
			leaf_use = renamed;
		}
		replaced_before = out->overwritten;
		o = copy_tree_item(&run, src, spec->dest, leaf_use, &item->snap,
		        is_dir, item->name, 0);
		/* A file that replaced another cannot be put back by Undo (the
		 * old one is gone): it is not offered for Undo at all. */
		if (OUT_OK == o && out->overwritten == replaced_before &&
		        !merge_top) {
			add_created(&run, spec->dest, &spec->dest_id, leaf_use,
			        is_dir);
		}
		if (WF_CJ_MOVE_ACROSS == spec->kind && OUT_OK == o &&
		        run.tree_clean && run.tree_may_remove &&
		        !run.cancelled && keep_going(&run) &&
		        source_listing_same(&run, src, item->name, is_dir)) {
			WfRecycleReport rr = {0};
			Removal removal = {&run, src, item->name, is_dir};
			WfStatus st = com
			        ? wf_recycle_guarded(src, &item->snap.id,
			                  &spec->slot.dir_id, ready_to_recycle,
			                  &removal, &rr)
			        : WF_E_OS;

			if (WF_OK == st) {
				out->sources_recycled++;
			} else {
				out->sources_kept++;
				if (rr.attempted) {
					out->sources_uncertain++;
				} else {
					out->sources_untouched++;
				}
				note_failure(&run, item->name,
				        rr.swapped ? WF_CF_REPLACED_IN_BIN
				                   : WF_CF_NOT_REMOVED,
				        rr.error ? rr.error
				                 : (unsigned long)rr.hresult);
			}
		} else if (WF_CJ_MOVE_ACROSS == spec->kind &&
		        OUT_CANCELLED != o) {
			out->sources_kept++;
			out->sources_untouched++;
			if (OUT_OK == o && run.tree_clean &&
			        run.tree_may_remove) {
				note_failure(&run, item->name,
				        WF_CF_SOURCE_CHANGED, 0);
			}
		}
		free(renamed);
		free(src);
	}
	proofs_close(&run);
	if (com) {
		wf_recycle_thread_done();
	}
	if (INVALID_HANDLE_VALUE != dest_pin) {
		CloseHandle(dest_pin);
	}
	if (INVALID_HANDLE_VALUE != src_pin) {
		CloseHandle(src_pin);
	}
	free(run.buffer);
	out->state = run.cancelled ? WF_CS_CANCELLED : WF_CS_DONE;
	return WF_OK;
}

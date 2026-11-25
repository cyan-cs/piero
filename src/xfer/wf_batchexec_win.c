#include "wf_batchexec_win.h"

#include <bcrypt.h>

#include <stdlib.h>
#include <string.h>

#include "../fs/wf_edit_win.h"
#include "../path/wf_canonical_win.h"
#include "../path/wf_path.h"

#define MAX_NAMES_PER_FOLDER 2000000u

static uint64_t now_ms(void)
{
	LARGE_INTEGER f, c;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (uint64_t)(c.QuadPart * 1000 / f.QuadPart);
}

static void hook(const WfBatchHooks *h, const char *point, size_t step)
{
	if (h && h->at) {
		h->at(h->ctx, point, step);
	}
}

static bool id_equal(const WfFileId *a, const WfFileId *b)
{
	return (a->kind == WF_ID_STRONG) && (b->kind == WF_ID_STRONG) &&
	        a->volume == b->volume && !memcmp(a->id, b->id, sizeof a->id);
}

/* The identity of what the handle is, and whether it is a reparse point. */
static bool id_of(
        HANDLE h, WfFileId *id, bool *reparse, bool *dir, WfBatchReport *rep)
{
	FILE_ID_INFO info;
	FILE_ATTRIBUTE_TAG_INFO tag;

	rep->calls.id_queries++;
	if (!GetFileInformationByHandleEx(h, FileIdInfo, &info, sizeof info) ||
	        !GetFileInformationByHandleEx(
	                h, FileAttributeTagInfo, &tag, sizeof tag)) {
		rep->error = GetLastError();
		return false;
	}
	id->kind = WF_ID_STRONG;
	id->volume = info.VolumeSerialNumber;
	memcpy(id->id, info.FileId.Identifier, sizeof id->id);
	if (reparse) {
		*reparse = 0 !=
		        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
	}
	if (dir) {
		*dir = 0 != (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY);
	}
	return true;
}

typedef enum
{
	RN_OK,
	RN_NOT_THERE,   /* the source is not there */
	RN_NOT_THE_ONE, /* what is there is not the planned file */
	RN_FAILED       /* the rename itself was refused */
} RenameResult;

/* Renames `from` to `to` if `from` is the file with `expected`. *renamed is true
 * once the file system accepted it (even if the check after it fails). */
static RenameResult rename_checked(const wchar_t *from,
        const wchar_t *to,
        const WfFileId *expected,
        bool *renamed,
        WfBatchReport *rep)
{
	HANDLE h;
	WfFileId seen;
	bool reparse;
	FILE_RENAME_INFO *info;
	size_t bytes = wcslen(to) * sizeof *to;
	RenameResult r = RN_FAILED;

	*renamed = false;
	rep->calls.opens++;
	h = CreateFileW(from, DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
	        NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == h) {
		rep->error = GetLastError();
		return ERROR_FILE_NOT_FOUND == rep->error ||
		                ERROR_PATH_NOT_FOUND == rep->error
		        ? RN_NOT_THERE
		        : RN_FAILED;
	}
	if (!id_of(h, &seen, &reparse, NULL, rep) || reparse ||
	        !id_equal(&seen, expected)) {
		r = RN_NOT_THE_ONE;
		goto done;
	}
	info = calloc(1, sizeof *info + bytes);
	if (!info) {
		rep->error = ERROR_NOT_ENOUGH_MEMORY;
		goto done;
	}
	info->ReplaceIfExists = FALSE;
	info->FileNameLength = (DWORD)bytes;
	memcpy(info->FileName, to, bytes);
	rep->calls.renames++;
	if (!SetFileInformationByHandle(
	            h, FileRenameInfo, info, (DWORD)(sizeof *info + bytes))) {
		rep->error = GetLastError();
		free(info);
		goto done;
	}
	free(info);
	*renamed = true;
	if (!id_of(h, &seen, &reparse, NULL, rep) || reparse ||
	        !id_equal(&seen, expected)) {
		r = RN_NOT_THE_ONE;
		goto done;
	}
	r = RN_OK;
done:
	CloseHandle(h);
	return r;
}

/* Is `path` the file with `id`? (open, compare, close) */
static bool is_the_one(
        const wchar_t *path, const WfFileId *id, WfBatchReport *rep)
{
	HANDLE h;
	WfFileId seen;
	bool ok;

	rep->calls.opens++;
	h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = id_of(h, &seen, NULL, NULL, rep) && id_equal(&seen, id);
	CloseHandle(h);
	return ok;
}

typedef struct
{
	WfBjItem *item;
	size_t n_items;
	WfBjStep *step;
	size_t n_steps;
	WfBjStepState *state;
	const wchar_t *
	        *from; /* owned array, n_steps long: see run_index_from */
} Run;

static WfFileId run_id(const Run *r, size_t item)
{
	WfFileId id;

	memset(&id, 0, sizeof id);
	id.kind = WF_ID_STRONG;
	id.volume = r->item[item].volume;
	memcpy(id.id, r->item[item].id, sizeof id.id);
	return id;
}

/* Where the item of step k is before the step: the last earlier target of the
 * same item, else where it began. Worked out for every step in one pass over
 * the steps, with the place each item is at, so that no step looks back through
 * all that went before it. Not owned by the array: the paths are the run's. A
 * step that names an item the run does not have is refused. */
static bool run_index_from(Run *r)
{
	const wchar_t **at = malloc((r->n_items ? r->n_items : 1) * sizeof *at);

	r->from = malloc((r->n_steps ? r->n_steps : 1) * sizeof *r->from);
	if (!at || !r->from) {
		free(at);
		return false;
	}
	for (size_t i = 0; i < r->n_items; i++) {
		at[i] = r->item[i].path;
	}
	for (size_t k = 0; k < r->n_steps; k++) {
		size_t item = r->step[k].item;

		if (item >= r->n_items) {
			free(at);
			return false;
		}
		r->from[k] = at[item];
		at[item] = r->step[k].to;
	}
	free(at);
	return true;
}

static const wchar_t *step_from(const Run *r, size_t k)
{
	return r->from[k];
}

/* Takes back the steps that were done, last first, each only if the file at its
 * new name is the planned one. Writes R lines. Returns the number it could not
 * take back. */
static size_t roll_back(
        Run *r, WfBatchLog *log, const WfBatchHooks *hooks, WfBatchReport *rep)
{
	size_t failed = 0;
	uint64_t t0 = now_ms();

	for (size_t k = r->n_steps; k-- > 0;) {
		WfFileId id;
		bool renamed;
		RenameResult res;

		if (WF_BJ_STEP_DONE != r->state[k]) {
			continue;
		}
		id = run_id(r, r->step[k].item);
		res = rename_checked(
		        r->step[k].to, step_from(r, k), &id, &renamed, rep);
		if (!renamed && RN_NOT_THERE == res &&
		        is_the_one(step_from(r, k), &id, rep)) {
			/* already back (an earlier rollback was cut before its R line) */
			(void)wf_batchlog_mark_lazy(log, 'R', k);
			r->state[k] = WF_BJ_STEP_RETURNED;
			continue;
		}
		if (!renamed) {
			failed++; /* not the planned file, or refused: left as it is */
			continue;
		}
		hook(hooks, "in_rollback", k);
		(void)wf_batchlog_mark_lazy(log, 'R', k);
		r->state[k] = WF_BJ_STEP_RETURNED;
		rep->rolled_back++;
		failed += RN_OK !=
		        res; /* moved, but the check after it did not hold */
	}
	rep->not_restored += failed;
	rep->ms_rollback += now_ms() - t0;
	return failed;
}

static void run_free(Run *r)
{
	for (size_t i = 0; i < r->n_items; i++) {
		free(r->item[i].path);
	}
	for (size_t i = 0; i < r->n_steps; i++) {
		free(r->step[i].to);
	}
	free(r->item);
	free(r->step);
	free(r->state);
	free(r->from);
	memset(r, 0, sizeof *r);
}

typedef struct
{
	HANDLE h; /* FILE_READ_ATTRIBUTES, no write or delete sharing: it stays put */
	WfCanonicalPath canon;
	wchar_t *given; /* the path as it was asked for */
	wchar_t **names;
	size_t n_names;
	bool case_sensitive, link;
	DWORD component;
	WfFileId id;
} Folder;

struct WfBatchJob
{
	Folder folder[WF_BATCH_MAX_PARENTS];
	size_t n_folder;
	WfBatchParent parent[WF_BATCH_MAX_PARENTS];
	WfBatchItem *item;
	wchar_t **old_name, **new_name;
	size_t n;
	WfBatchPlan plan;
	bool ready;
	uint8_t id[16];
};

void wf_batch_job_free(WfBatchJob *job)
{
	if (!job) {
		return;
	}
	for (size_t i = 0; i < job->n_folder; i++) {
		Folder *f = &job->folder[i];

		for (size_t k = 0; k < f->n_names; k++) {
			free(f->names[k]);
		}
		free(f->names);
		free(f->given);
		wf_canonical_free(&f->canon);
		if (f->h && INVALID_HANDLE_VALUE != f->h) {
			CloseHandle(f->h);
		}
	}
	for (size_t i = 0; i < job->n; i++) {
		if (job->old_name) {
			free(job->old_name[i]);
		}
		if (job->new_name) {
			free(job->new_name[i]);
		}
	}
	free(job->old_name);
	free(job->new_name);
	free(job->item);
	wf_batch_plan_free(&job->plan);
	free(job);
}

static bool names_add(Folder *f, const wchar_t *name, size_t *cap)
{
	if (f->n_names == *cap) {
		size_t c = *cap ? *cap * 2 : 64;
		wchar_t **grown = realloc(f->names, c * sizeof *grown);

		if (!grown) {
			return false;
		}
		f->names = grown;
		*cap = c;
	}
	f->names[f->n_names] = wf_path_dup(name);
	if (!f->names[f->n_names]) {
		return false;
	}
	f->n_names++;
	return true;
}

static volatile LONG fail_listing_after = -1;

void wf_batch_fail_listing_after(long names)
{
	InterlockedExchange(&fail_listing_after, names);
}

/* FindNextFileW, or (for a test) an access error once `seen` names are read. */
static bool next_entry(HANDLE find, WIN32_FIND_DATAW *data, size_t seen)
{
	LONG after = fail_listing_after;

	if (after >= 0 && seen >= (size_t)after) {
		SetLastError(ERROR_ACCESS_DENIED);
		return false;
	}
	return FindNextFileW(find, data);
}

/* Every name in the folder, once. A listing that stops for any reason but its
 * end is not the folder: the names a collision could be with are missing. */
static WfBatchPre read_names(Folder *f, WfBatchReport *rep)
{
	wchar_t *pattern = NULL;
	WIN32_FIND_DATAW data;
	HANDLE find;
	size_t cap = 0, seen = 0;
	WfBatchPre pre = WF_BATCH_READY;

	if (WF_OK != wf_path_join(f->canon.path, L"*", &pattern)) {
		return WF_BATCH_PRE_NOMEM;
	}
	rep->calls.enumerations++;
	find = FindFirstFileExW(pattern, FindExInfoBasic, &data,
	        FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
	free(pattern);
	if (INVALID_HANDLE_VALUE == find) {
		rep->error = GetLastError();
		return ERROR_FILE_NOT_FOUND == rep->error ? WF_BATCH_READY
		                                          : WF_BATCH_PRE_ACCESS;
	}
	do {
		seen++;
		if (!wcscmp(data.cFileName, L".") ||
		        !wcscmp(data.cFileName, L"..")) {
			continue;
		}
		if (f->n_names >= MAX_NAMES_PER_FOLDER) {
			pre = WF_BATCH_PRE_LIMITS;
			break;
		}
		if (!names_add(f, data.cFileName, &cap)) {
			pre = WF_BATCH_PRE_NOMEM;
			break;
		}
	} while (next_entry(find, &data, seen));
	if (WF_BATCH_READY == pre && ERROR_NO_MORE_FILES != GetLastError()) {
		rep->error = GetLastError();
		pre = WF_BATCH_PRE_ACCESS;
	}
	FindClose(find);
	return pre;
}

/* The folder at `path`, opened once (the same folder by another spelling is the
 * same entry, by identity). */
static WfBatchPre folder_get(
        WfBatchJob *job, const wchar_t *path, size_t *index, WfBatchReport *rep)
{
	wchar_t *ext = NULL;
	Folder *f;
	HANDLE h;
	WfFileId id;
	bool reparse, dir;
	FILE_CASE_SENSITIVE_INFO cs;
	DWORD flags;
	WfBatchPre pre;

	if (WF_OK != wf_path_to_extended(path, &ext)) {
		return WF_BATCH_PRE_UNSUPPORTED;
	}
	rep->calls.opens++;
	h = CreateFileW(ext, FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	free(ext);
	if (INVALID_HANDLE_VALUE == h) {
		rep->error = GetLastError();
		return ERROR_FILE_NOT_FOUND == rep->error ||
		                ERROR_PATH_NOT_FOUND == rep->error
		        ? WF_BATCH_PRE_MISSING
		        : WF_BATCH_PRE_ACCESS;
	}
	if (!id_of(h, &id, &reparse, &dir, rep) || !dir) {
		CloseHandle(h);
		return WF_BATCH_PRE_UNSUPPORTED;
	}
	for (size_t i = 0; i < job->n_folder; i++) {
		if (id_equal(&job->folder[i].id, &id)) {
			CloseHandle(h);
			*index = i;
			return WF_BATCH_READY;
		}
	}
	if (job->n_folder >= WF_BATCH_MAX_PARENTS) {
		CloseHandle(h);
		return WF_BATCH_PRE_LIMITS;
	}
	f = &job->folder[job->n_folder];
	memset(f, 0, sizeof *f);
	f->h = h;
	f->id = id;
	job->n_folder++; /* owned by the job from here: freed with it */
	f->given = wf_path_dup(path);
	if (!f->given) {
		return WF_BATCH_PRE_NOMEM;
	}
	if (!wf_canonical_from_handle(h, &f->canon, &rep->error)) {
		return WF_BATCH_PRE_UNSUPPORTED;
	}
	{
		unsigned long err = 0;

		f->link =
		        reparse || !wf_edit_path_chain_ok(f->canon.path, &err);
	}
	if (!GetVolumeInformationByHandleW(
	            h, NULL, 0, NULL, &f->component, &flags, NULL, 0)) {
		rep->error = GetLastError();
		return WF_BATCH_PRE_ACCESS;
	}
	if (flags & FILE_READ_ONLY_VOLUME) {
		rep->error = ERROR_WRITE_PROTECT;
		return WF_BATCH_PRE_ACCESS;
	}
	memset(&cs, 0, sizeof cs);
	f->case_sensitive = GetFileInformationByHandleEx(
	                            h, FileCaseSensitiveInfo, &cs, sizeof cs) &&
	        (cs.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR);
	pre = read_names(f, rep);
	if (WF_BATCH_READY != pre) {
		return pre;
	}
	*index = job->n_folder - 1;
	return WF_BATCH_READY;
}

static bool random_id(uint8_t id[16])
{
	return 0 ==
	        BCryptGenRandom(NULL, id, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

/* One source: its folder, its identity now against the listing, its kind. */
static WfBatchPre check_source(WfBatchJob *job,
        size_t i,
        const WfBatchSource *src,
        const wchar_t *dest_dir,
        size_t dest_index,
        WfBatchReport *rep)
{
	size_t parent_len = wf_path_parent_len(src->path), folder;
	const wchar_t *leaf;
	size_t leaf_len;
	wchar_t *parent_path, *full = NULL;
	WfBatchPre pre;
	HANDLE h;
	WfFileId seen;
	bool reparse, dir;
	WfBatchItem *it = &job->item[i];

	wf_path_leaf(src->path, &leaf, &leaf_len);
	if (!parent_len || !leaf_len || !(src->expected.kind == WF_ID_STRONG)) {
		return WF_BATCH_PRE_UNSUPPORTED;
	}
	parent_path = wf_path_dup_n(src->path, parent_len);
	if (!parent_path) {
		return WF_BATCH_PRE_NOMEM;
	}
	pre = folder_get(job, parent_path, &folder, rep);
	free(parent_path);
	if (WF_BATCH_READY != pre) {
		return pre;
	}
	job->old_name[i] = wf_path_dup_n(leaf, leaf_len);
	job->new_name[i] = src->to_name ? wf_path_dup(src->to_name)
	                                : wf_path_dup_n(leaf, leaf_len);
	if (!job->old_name[i] || !job->new_name[i]) {
		return WF_BATCH_PRE_NOMEM;
	}
	if (WF_OK !=
	        wf_path_join(job->folder[folder].canon.path, job->old_name[i],
	                &full)) {
		return WF_BATCH_PRE_NOMEM;
	}
	rep->calls.opens++;
	h = CreateFileW(full, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	free(full);
	if (INVALID_HANDLE_VALUE == h) {
		rep->error = GetLastError();
		return ERROR_FILE_NOT_FOUND == rep->error ||
		                ERROR_PATH_NOT_FOUND == rep->error
		        ? WF_BATCH_PRE_MISSING
		        : WF_BATCH_PRE_ACCESS;
	}
	if (!id_of(h, &seen, &reparse, &dir, rep)) {
		CloseHandle(h);
		return WF_BATCH_PRE_ACCESS;
	}
	CloseHandle(h);
	if (!id_equal(&seen, &src->expected)) {
		return WF_BATCH_PRE_CHANGED;
	}
	it->parent = (uint32_t)folder;
	it->name = job->old_name[i];
	it->dir = dir;
	it->link = reparse;
	it->id = seen;
	it->to_parent = dest_dir ? (uint32_t)dest_index : (uint32_t)folder;
	it->to_name = job->new_name[i];
	return WF_BATCH_READY;
}

WfBatchJob *wf_batch_prepare(const WfBatchSource *src,
        size_t n,
        const wchar_t *dest_dir,
        WfBatchReport *rep)
{
	WfBatchJob *job;
	size_t dest_index = 0;
	uint64_t t0 = now_ms();

	memset(rep, 0, sizeof *rep);
	rep->result = WF_BATCH_FAILED_BEFORE_EXECUTION;
	if (!src || !n || n > WF_BATCH_MAX_ITEMS) {
		rep->pre = WF_BATCH_PRE_LIMITS;
		return NULL;
	}
	job = calloc(1, sizeof *job);
	if (!job) {
		rep->pre = WF_BATCH_PRE_NOMEM;
		return NULL;
	}
	job->n = n;
	job->item = calloc(n, sizeof *job->item);
	job->old_name = calloc(n, sizeof *job->old_name);
	job->new_name = calloc(n, sizeof *job->new_name);
	if (!job->item || !job->old_name || !job->new_name ||
	        !random_id(job->id)) {
		rep->pre = WF_BATCH_PRE_NOMEM;
		return job;
	}
	if (dest_dir) {
		rep->pre = folder_get(job, dest_dir, &dest_index, rep);
		if (WF_BATCH_READY != rep->pre) {
			return job;
		}
	}
	for (size_t i = 0; i < n; i++) {
		rep->pre = check_source(
		        job, i, &src[i], dest_dir, dest_index, rep);
		if (WF_BATCH_READY != rep->pre) {
			rep->pre_item = i;
			return job;
		}
	}
	for (size_t i = 0; i < job->n_folder; i++) {
		Folder *f = &job->folder[i];
		WfBatchParent *p = &job->parent[i];

		p->path = f->canon.path;
		p->id = f->id;
		p->case_sensitive = f->case_sensitive;
		p->link = f->link;
		p->component_max = f->component;
		p->names = (const wchar_t *const *)f->names;
		p->n_names = f->n_names;
	}
	switch (wf_batch_plan(job->parent, job->n_folder, job->item, n, job->id,
	        &job->plan)) {
	case WF_OK:      break;
	case WF_E_NOMEM: rep->pre = WF_BATCH_PRE_NOMEM; return job;
	default:         rep->pre = WF_BATCH_PRE_LIMITS; return job;
	}
	if (job->plan.n_refused) {
		rep->pre = WF_BATCH_PRE_PLAN;
		for (size_t i = 0; i < n; i++) {
			if (job->plan.row[i].why != WF_BP_OK &&
			        job->plan.row[i].why != WF_BP_UNCHANGED) {
				rep->pre_item = i;
				break;
			}
		}
		return job;
	}
	rep->pre = WF_BATCH_READY;
	job->ready = true;
	rep->steps_total = job->plan.n_steps;
	rep->ms_prepare = now_ms() - t0;
	return job;
}

bool wf_batch_job_ready(const WfBatchJob *job)
{
	return job && job->ready;
}

const WfBatchPlan *wf_batch_job_plan(const WfBatchJob *job)
{
	return job ? &job->plan : NULL;
}

size_t wf_batch_job_items(const WfBatchJob *job)
{
	return job ? job->n : 0;
}

const wchar_t *wf_batch_job_old_name(const WfBatchJob *job, size_t item)
{
	return job && item < job->n ? job->old_name[item] : NULL;
}

const wchar_t *wf_batch_job_new_name(const WfBatchJob *job, size_t item)
{
	return job && item < job->n ? job->new_name[item] : NULL;
}

static bool build_run(const WfBatchJob *job, Run *r)
{
	const WfBatchPlan *p = &job->plan;

	memset(r, 0, sizeof *r);
	r->n_items = job->n;
	r->n_steps = p->n_steps;
	r->item = calloc(job->n, sizeof *r->item);
	r->step = calloc(p->n_steps ? p->n_steps : 1, sizeof *r->step);
	r->state = calloc(p->n_steps ? p->n_steps : 1, sizeof *r->state);
	if (!r->item || !r->step || !r->state) {
		return false;
	}
	for (size_t i = 0; i < job->n; i++) {
		const WfBatchItem *it = &job->item[i];

		r->item[i].volume = it->id.volume;
		memcpy(r->item[i].id, it->id.id, 16);
		r->item[i].dir = it->dir;
		if (WF_OK !=
		        wf_path_join(job->folder[it->parent].canon.path,
		                it->name, &r->item[i].path)) {
			return false;
		}
	}
	for (size_t k = 0; k < p->n_steps; k++) {
		const WfBatchStep *s = &p->step[k];

		r->step[k].item = s->item;
		r->step[k].temp = s->temp;
		if (WF_OK !=
		        wf_path_join(job->folder[s->to_parent].canon.path,
		                s->to_name, &r->step[k].to)) {
			return false;
		}
	}
	return run_index_from(r);
}

static void finish(WfBatchLog *log,
        WfBatchEnd end,
        WfBatchResult result,
        WfBatchReport *rep)
{
	rep->result = result;
	if (WF_OK != wf_batchlog_end(log, end) && WF_BATCH_SUCCESS == result) {
		/* the work is done but its record is not closed: not a success to show */
		rep->result = WF_BATCH_RECOVERY_REQUIRED;
	}
	rep->log = wf_batchlog_stats(log);
}

void wf_batch_execute(WfBatchJob *job,
        const wchar_t *journal_dir,
        const WfBatchHooks *hooks,
        const volatile LONG *cancel,
        WfBatchReport *rep)
{
	Run run;
	WfBatchLog *log = NULL;
	uint64_t t0;
	size_t k;
	bool failed = false;

	memset(&run, 0, sizeof run);
	rep->result = WF_BATCH_FAILED_BEFORE_EXECUTION;
	if (!job || !job->ready || !journal_dir) {
		return;
	}
	if (!job->plan.n_steps) {
		rep->result =
		        WF_BATCH_SUCCESS; /* every name was already as wanted */
		return;
	}
	if (!build_run(job, &run)) {
		run_free(&run);
		rep->pre = WF_BATCH_PRE_NOMEM;
		return;
	}
	t0 = now_ms();
	if (WF_OK != wf_batchlog_create(journal_dir, job->id, &log) ||
	        WF_OK !=
	                wf_batchlog_plan(log, run.item, run.n_items, run.step,
	                        run.n_steps)) {
		wf_batchlog_close(log);
		run_free(&run);
		rep->error = GetLastError();
		return; /* nothing was changed */
	}
	rep->ms_journal = now_ms() - t0;
	hook(hooks, "after_plan", 0);
	t0 = now_ms();
	for (k = 0; k < run.n_steps; k++) {
		WfFileId id = run_id(&run, run.step[k].item);
		bool renamed = false;
		RenameResult r;

		if (cancel && *cancel) {
			failed = true;
			rep->failed_step = k;
			rep->error = ERROR_CANCELLED;
			break;
		}
		if (WF_OK != wf_batchlog_mark(log, 'B', k)) {
			failed = true;
			rep->failed_step = k;
			rep->error = GetLastError();
			break;
		}
		hook(hooks, "after_B", k);
		r = rename_checked(
		        step_from(&run, k), run.step[k].to, &id, &renamed, rep);
		if (renamed) {
			hook(hooks, "after_rename", k);
			run.state[k] = WF_BJ_STEP_DONE;
			if (WF_OK != wf_batchlog_mark_lazy(log, 'D', k)) {
				failed = true;
				rep->failed_step = k;
				rep->steps_done++;
				break;
			}
			hook(hooks, "after_D", k);
			rep->steps_done++;
			if (RN_OK != r) {
				failed =
				        true; /* the check after the rename did not hold */
				rep->failed_step = k;
				break;
			}
			continue;
		}
		/* the rename was not made: nothing of this step to take back */
		(void)wf_batchlog_mark_lazy(log, 'R', k);
		run.state[k] = WF_BJ_STEP_RETURNED;
		failed = true;
		rep->failed_step = k;
		break;
	}
	rep->ms_rename = now_ms() - t0;
	if (!failed) {
		hook(hooks, "before_E", 0);
		finish(log, WF_BJ_SUCCESS, WF_BATCH_SUCCESS, rep);
	} else {
		hook(hooks, "after_failure", rep->failed_step);
		if (0 == roll_back(&run, log, hooks, rep)) {
			hook(hooks, "before_E", 0);
			finish(log, WF_BJ_ROLLED_BACK, WF_BATCH_ROLLED_BACK,
			        rep);
		} else {
			finish(log, WF_BJ_RECOVERY_REQUIRED,
			        WF_BATCH_RECOVERY_REQUIRED, rep);
		}
	}
	wf_batchlog_close(log);
	run_free(&run);
}

/* The journal's own arrays become the run. */
static bool run_from_journal(WfBatchJournal *j, Run *r)
{
	memset(r, 0, sizeof *r);
	r->item = j->item;
	r->n_items = j->n_items;
	r->step = j->step;
	r->n_steps = j->n_steps;
	r->state = j->state;
	return true;
}

void wf_batch_recover(
        const wchar_t *file, const WfBatchHooks *hooks, WfBatchReport *rep)
{
	WfBatchJournal j;
	WfBatchLog *log = NULL;
	Run run;
	size_t done = 0;
	bool unknown = false;

	memset(rep, 0, sizeof *rep);
	rep->result = WF_BATCH_RECOVERY_REQUIRED;
	switch (wf_batchlog_read(file, &j)) {
	case WF_BJ_READ_OK: break;
	case WF_BJ_READ_NO_PLAN:
		rep->result =
		        WF_BATCH_FAILED_BEFORE_EXECUTION; /* never began: nothing changed */
		return;
	default: return; /* damaged or another version: kept, nothing done */
	}
	if (j.ended) {
		rep->result = WF_BJ_SUCCESS == j.end ? WF_BATCH_SUCCESS
		        : WF_BJ_ROLLED_BACK == j.end
		        ? WF_BATCH_ROLLED_BACK
		        : WF_BATCH_RECOVERY_REQUIRED;
		wf_batchjournal_free(&j);
		return;
	}
	if (WF_OK != wf_batchlog_resume(file, j.valid_bytes, &log)) {
		rep->error = GetLastError();
		wf_batchjournal_free(&j);
		return;
	}
	run_from_journal(&j, &run);
	if (!run_index_from(
	            &run)) { /* a step of an item that is not there, or no memory */
		free(run.from);
		wf_batchlog_close(log);
		wf_batchjournal_free(&j);
		return; /* the journal is kept: someone must look */
	}
	rep->steps_total = run.n_steps;
	for (size_t k = 0; k < run.n_steps; k++) {
		WfFileId id = run_id(&run, run.step[k].item);

		if (WF_BJ_STEP_BEGUN != run.state[k]) {
			continue;
		}
		/* the disk says whether the rename happened */
		if (is_the_one(run.step[k].to, &id, rep)) {
			run.state[k] = WF_BJ_STEP_DONE;
			(void)wf_batchlog_mark_lazy(log, 'D', k);
		} else if (is_the_one(step_from(&run, k), &id, rep)) {
			run.state[k] = WF_BJ_STEP_RETURNED;
			(void)wf_batchlog_mark_lazy(log, 'R', k);
		} else {
			unknown =
			        true; /* neither name holds the file: do not guess */
		}
	}
	for (size_t k = 0; k < run.n_steps; k++) {
		done += WF_BJ_STEP_DONE == run.state[k];
	}
	rep->steps_done = done;
	if (unknown) {
		finish(log, WF_BJ_RECOVERY_REQUIRED, WF_BATCH_RECOVERY_REQUIRED,
		        rep);
	} else if (done == run.n_steps) {
		finish(log, WF_BJ_SUCCESS, WF_BATCH_SUCCESS, rep);
	} else if (0 == roll_back(&run, log, hooks, rep)) {
		finish(log, WF_BJ_ROLLED_BACK, WF_BATCH_ROLLED_BACK, rep);
	} else {
		finish(log, WF_BJ_RECOVERY_REQUIRED, WF_BATCH_RECOVERY_REQUIRED,
		        rep);
	}
	wf_batchlog_close(log);
	free(run.from);
	wf_batchjournal_free(&j);
}

size_t wf_batch_count_unfinished(const wchar_t *dir)
{
	wchar_t *pattern = NULL;
	WIN32_FIND_DATAW data;
	HANDLE find;
	size_t count = 0;

	if (WF_OK != wf_path_join(dir, L"*.pbj", &pattern)) {
		return 0;
	}
	find = FindFirstFileExW(pattern, FindExInfoBasic, &data,
	        FindExSearchNameMatch, NULL, 0);
	free(pattern);
	if (INVALID_HANDLE_VALUE == find) {
		return 0;
	}
	do {
		wchar_t *path = NULL;
		WfBatchJournal j;
		WfBjReadResult r;

		if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
		        WF_OK != wf_path_join(dir, data.cFileName, &path)) {
			continue;
		}
		r = wf_batchlog_read(path, &j);
		free(path);
		if (WF_BJ_READ_OK == r) {
			count += !j.ended || WF_BJ_RECOVERY_REQUIRED == j.end;
			wf_batchjournal_free(&j);
		} else if (WF_BJ_READ_NO_PLAN != r) {
			count++; /* damaged or of another version: someone must look */
		}
	} while (FindNextFileW(find, &data));
	FindClose(find);
	return count;
}

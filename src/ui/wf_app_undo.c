/*
 * Undo (u): what the screen remembers of what was done, and how it is taken
 * back. An entry names the objects it will touch by their identities; the
 * action is refused if one is no longer that object. Entries are found by a
 * uid, never by their place in the stack, so a push or a removal while a
 * request is running cannot make it update the wrong one. The questions put
 * before an Undo, and the Recycle jobs that remove the copies a copy made,
 * are here too; the Recycle job itself is wf_app_copy.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"

void wf_app_undo_free(WfUndoEntry *u)
{
	size_t i;

	for (i = 0; i < u->n; i++) {
		free(u->item[i].name);
		free(u->item[i].old_name);
	}
	free(u->item);
	free(u->folder);
	free(u->origin);
	memset(u, 0, sizeof *u);
}

/* What the history row of an entry holds: the uid of its group, or its own. */
static uint32_t undo_row_key(const WfUndoEntry *u)
{
	return u->group ? u->group : u->uid;
}

/* The entry at `i` leaves the stack: undone, or pushed out. Its row in the
 * history changes only when no other entry of the same operation is left: a
 * copy undone in part is not "undone". If one of the group was pushed out, the
 * rest says so, and the row ends as expired, never as undone. */
static void undo_row_left(WfApp *app, size_t i, bool undone)
{
	uint32_t row = undo_row_key(&app->undo[i]);
	bool lost = app->undo[i].group_lost || !undone;
	bool others = false;

	for (size_t k = 0; k < app->undo_n; k++) {
		if (k != i && undo_row_key(&app->undo[k]) == row) {
			others = true;
			if (lost) {
				app->undo[k].group_lost = true;
			}
		}
	}
	if (!others) {
		wf_app_hist_undo_gone(app, row, undone && !lost);
	}
}

/* Takes the entry's contents (u is left empty). The oldest entry goes when the
 * stack is full. */
uint32_t wf_app_undo_push(WfApp *app, WfUndoEntry *u)
{
	uint32_t uid;

	if (0 == ++app->undo_uid_next) {
		++app->undo_uid_next; /* 0 means "none" */
	}
	uid = u->uid = app->undo_uid_next;
	if (WF_UNDO_MAX == app->undo_n) {
		/* its row in the history says it can no longer be undone */
		undo_row_left(app, 0, false);
		wf_app_undo_free(&app->undo[0]);
		memmove(&app->undo[0], &app->undo[1],
		        (WF_UNDO_MAX - 1) * sizeof app->undo[0]);
		app->undo_n--;
	}
	app->undo[app->undo_n++] = *u;
	memset(u, 0, sizeof *u);
	return uid;
}

/* The index of the entry with this uid now, or SIZE_MAX: entries move when
 * others are added, pushed out or removed, their uids do not. */
static size_t undo_find(const WfApp *app, uint32_t uid)
{
	for (size_t i = 0; uid && i < app->undo_n; i++) {
		if (app->undo[i].uid == uid) {
			return i;
		}
	}
	return SIZE_MAX;
}

/* Takes the items marked (non-zero) in `gone` out of the entry. Nothing is
 * allocated: this is written down after the work was done, and a failure to
 * write it down would leave an entry for what is no longer there. */
static void undo_drop_items(WfUndoEntry *u, const unsigned char *gone)
{
	size_t to = 0;

	for (size_t i = 0; i < u->n; i++) {
		if (gone[i]) {
			free(u->item[i].name);
			free(u->item[i].old_name);
			continue;
		}
		u->item[to++] = u->item[i];
	}
	u->n = to;
}

/* The first `done` items are back where they were: they go, the rest stay. */
static void undo_drop_first(WfUndoEntry *u, size_t done)
{
	for (size_t i = 0; i < done && i < u->n; i++) {
		free(u->item[i].name);
		free(u->item[i].old_name);
	}
	if (done > u->n) {
		done = u->n;
	}
	memmove(&u->item[0], &u->item[done], (u->n - done) * sizeof u->item[0]);
	u->n -= done;
}

/* The entry was undone whole: it goes, and its row in the history says so. */
static void undo_remove(WfApp *app, size_t i)
{
	undo_row_left(app, i, true);
	wf_app_undo_free(&app->undo[i]);
	memmove(&app->undo[i], &app->undo[i + 1],
	        (app->undo_n - i - 1) * sizeof app->undo[0]);
	app->undo_n--;
}

void wf_app_undo_retire(WfApp *app, uint32_t uid)
{
	size_t at = undo_find(app, uid);

	if (SIZE_MAX != at) {
		undo_remove(app, at);
	}
}

/* The Undo entry whose items are exactly the ones in the slot, or SIZE_MAX. */
size_t wf_app_undo_entry_of_slot(const WfApp *app, const WfTransferSlot *s)
{
	for (size_t i = app->undo_n; i-- > 0;) {
		const WfUndoEntry *u = &app->undo[i];
		size_t j;

		if (WF_UNDO_RECYCLE_CREATED != u->kind || u->n != s->count) {
			continue;
		}
		for (j = 0; j < u->n; j++) {
			if (!wf_fileid_equal(
			            &u->item[j].id, &s->src[j].snap.id)) {
				break;
			}
		}
		if (j == u->n) {
			return i;
		}
	}
	return SIZE_MAX;
}

#define UNDO_GROUPS_MAX 8 /* of the WF_UNDO_MAX entries the stack holds */

typedef struct
{
	size_t at; /* index in the result: keeps the order within a folder */
	size_t parent_len; /* length of the folder part of the path */
	const wchar_t *path;
} UndoKey;

static int undo_key_cmp(const void *pa, const void *pb)
{
	const UndoKey *a = pa, *b = pb;
	size_t n =
	        a->parent_len < b->parent_len ? a->parent_len : b->parent_len;
	int c = wcsncmp(a->path, b->path, n);

	if (c) {
		return c;
	}
	if (a->parent_len != b->parent_len) {
		return a->parent_len < b->parent_len ? -1 : 1;
	}
	return a->at < b->at ? -1 : a->at > b->at;
}

/* One Undo entry for the items [from, to) of `key`, which share a folder. The
 * folder's identity is the one the job held (the destination for what went in
 * at the top, the folder entered for what a merge added below it). False, and
 * `u` empty, when it cannot be proven. */
static bool undo_build_group(WfUndoEntry *out,
        const WfCopyResult *r,
        const UndoKey *key,
        size_t from,
        size_t to)
{
	WfUndoEntry u;
	const WfCopyCreated *first = &r->created[key[from].at];

	memset(&u, 0, sizeof u);
	u.kind = WF_UNDO_RECYCLE_CREATED;
	u.folder_id = (first->parent.kind == WF_ID_STRONG) ? first->parent
	                                                   : r->dest_id;
	u.item = calloc(to - from, sizeof *u.item);
	u.folder = wf_path_dup_n(first->path, key[from].parent_len);
	if (!u.item || !u.folder) {
		wf_app_undo_free(&u);
		return false;
	}
	for (size_t i = from; i < to; i++) {
		const WfCopyCreated *c = &r->created[key[i].at];
		const wchar_t *leaf;
		size_t leaf_len;
		WfUndoItem *it = &u.item[i - from];

		wf_path_leaf(c->path, &leaf, &leaf_len);
		if (!(c->id.kind == WF_ID_STRONG) ||
		        ((c->parent.kind == WF_ID_STRONG) &&
		                !(u.folder_id.kind == WF_ID_STRONG)) ||
		        ((c->parent.kind == WF_ID_STRONG) &&
		                !wf_fileid_equal(&c->parent, &u.folder_id))) {
			wf_app_undo_free(&u);
			return false; /* not provable: no undo rather than a guess */
		}
		it->name = wf_path_dup_n(leaf, leaf_len);
		it->id = c->id;
		it->is_dir = c->dir;
		u.n = i - from +
		        1; /* counted first: a failure frees this name too */
		if (!it->name) {
			wf_app_undo_free(&u);
			return false;
		}
	}
	*out = u;
	return true;
}

/* Takes the created items for undo (copy only: after a move the originals are
 * in the Bin, so recycling the copies would leave nothing). A copy that merged
 * into folders made items at several levels: they are grouped by the folder
 * they are in, one entry for each, because an entry proves its folder and
 * every item in it. The entries of one copy share a group, and the row of the
 * history, which holds the uid returned (the first entry's), is marked undone
 * only when the last of them is. All or none: a copy that made items in more
 * than UNDO_GROUPS_MAX folders, or whose items cannot all be proven, is not
 * offered for Undo at all (the history says so), never in part. */
uint32_t wf_app_undo_keep_created(WfApp *app, WfCopyResult *r)
{
	UndoKey *key;
	WfUndoEntry ent[UNDO_GROUPS_MAX];
	size_t from = 0, groups = 0;
	uint32_t uid = 0;

	if (WF_CJ_COPY != r->kind || !r->created_n) {
		return 0;
	}
	key = calloc(r->created_n, sizeof *key);
	if (!key) {
		return 0;
	}
	for (size_t i = 0; i < r->created_n; i++) {
		key[i].at = i;
		key[i].path = r->created[i].path;
		key[i].parent_len = wf_path_parent_len(r->created[i].path);
	}
	qsort(key, r->created_n, sizeof *key, undo_key_cmp);
	for (size_t i = 1; i <= r->created_n; i++) {
		if (i < r->created_n &&
		        key[i].parent_len == key[from].parent_len &&
		        !wcsncmp(key[i].path, key[from].path,
		                key[from].parent_len)) {
			continue;
		}
		if (groups == UNDO_GROUPS_MAX ||
		        !undo_build_group(&ent[groups], r, key, from, i)) {
			while (groups) {
				wf_app_undo_free(&ent[--groups]);
			}
			free(key);
			return 0;
		}
		groups++;
		from = i;
	}
	free(key);
	for (size_t g = 0; g < groups; g++) {
		ent[g].group = uid;
		if (!g) {
			uid = wf_app_undo_push(app, &ent[g]);
			app->undo[app->undo_n - 1].group = uid;
		} else {
			wf_app_undo_push(app, &ent[g]);
		}
	}
	return uid;
}

/* A Recycle job that was removing the items of an Undo entry has finished:
 * the entry loses what is gone and keeps what is not, so the next Undo
 * tries only the rest. What the result does not name stays (a retry of an
 * item that is gone is refused by its identity, not acted on). */
void wf_app_undo_recycle_result(WfApp *app, const WfCopyResult *r)
{
	size_t k, at;
	uint32_t uid = 0;
	WfUndoEntry *u;

	for (k = 0; k < app->recycling_n; k++) {
		if (app->recycling[k].job == r->job) {
			uid = app->recycling[k].uid;
			memmove(&app->recycling[k], &app->recycling[k + 1],
			        (app->recycling_n - k - 1) *
			                sizeof app->recycling[0]);
			app->recycling_n--;
			break;
		}
	}
	at = undo_find(app, uid);
	if (SIZE_MAX == at || WF_UNDO_RECYCLE_CREATED != app->undo[at].kind) {
		return;
	}
	u = &app->undo[at];
	/* the job reports each of its sources: taken, or not (failed, kept or
	 * never reached). Without that report the entry stays whole. */
	if (!r->item_done || r->item_n != u->n) {
		return;
	}
	undo_drop_items(u, r->item_done);
	if (0 == u->n) {
		undo_remove(app, at);
	}
}

/* "In: <folder>", the folder as text, cut to fit the question. */
static void where_line(char *out, size_t cap, WfMsg id, const wchar_t *folder)
{
	WfBuf b;
	char cut[WF_CONFIRM_LINE_BYTES];

	wf_buf_init(&b);
	(void)wf_sanitize_utf16(folder, wcslen(folder), &b);
	wf_utf8_elide(cut, sizeof cut, wf_buf_cstr(&b), 56);
	wf_buf_free(&b);
	(void)wf_msgf(out, cap, id, &WF_ARG_S(cut), 1);
}

/* Fills an empty slot with the entry's folder and items (names, identities and
 * kinds). false: out of memory, and the slot is left empty. */
static bool slot_of_entry(WfTransferSlot *s, const WfUndoEntry *u)
{
	wf_slot_init(s);
	s->src = calloc(u->n, sizeof *s->src);
	s->dir = wf_path_dup(u->folder);
	s->dir_id = u->folder_id;
	if (!s->src || !s->dir) {
		wf_slot_free(s);
		return false;
	}
	for (size_t i = 0; i < u->n; i++) {
		s->src[i].name = wf_path_dup(u->item[i].name);
		if (!s->src[i].name) {
			wf_slot_free(s); /* frees the first s->count names */
			return false;
		}
		s->src[i].snap.id = u->item[i].id;
		s->src[i].snap.flags = u->item[i].is_dir ? WF_ENT_DIR : 0;
		s->count = i + 1;
	}
	return true;
}

static void undo_recycle_copies(WfApp *app, WfUndoEntry *u)
{
	WfTransferSlot *s = &app->recycle_slot;
	char l1[WF_CONFIRM_LINE_BYTES], l2[WF_CONFIRM_LINE_BYTES];
	const char *lines[4];
	WfStatus st;

	wf_slot_free(s);
	if (!slot_of_entry(s, u)) { /* the folder the copies went into */
		wf_app_set_msg(app, wf_msg(MSG_JOB_NOMEM));
		return;
	}
	(void)wf_msgf(
	        l1, sizeof l1, MSG_JOB_UNDO_COPY_LINE, &WF_ARG_N(u->n), 1);
	where_line(l2, sizeof l2, MSG_JOB_UNDO_WHERE, u->folder);
	lines[0] = wf_msg(MSG_JOB_UNDO_COPY_TITLE);
	lines[1] = l1;
	lines[2] = l2;
	lines[3] = wf_msg(MSG_JOB_UNDO_COPY_NOTE);
	st = wf_app_confirm_begin(
	        app, lines, 4, wf_app_recycle_confirmed, NULL, NULL);
	if (WF_OK != st) {
		wf_slot_free(s);
		wf_app_set_error(app, st);
	}
}

static void undo_rename_back(WfApp *app, WfUndoEntry *u)
{
	WfEditRequest req;

	if (!app->edit.submit || app->edit_busy || app->move.active) {
		wf_app_set_msg(app, wf_msg(MSG_OPS_BUSY));
		return;
	}
	memset(&req, 0, sizeof req);
	req.kind = WF_EDIT_RENAME;
	req.parent = u->folder;
	req.parent_id = u->folder_id;
	req.old_name = u->item[0].name;
	req.source_id = u->item[0].id;
	req.new_name = u->item[0].old_name;
	if (WF_OK != app->edit.submit(app->edit.ctx, ++app->edit_gen, &req)) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_UNDO_START_FAILED));
		return;
	}
	app->edit_busy = true;
	app->edit_is_undo = true;
	wf_app_set_msg(app, wf_msg(MSG_JOB_UNDOING_RENAME));
	app->undo_inflight = u->uid; /* removed when done */
}

static void undo_move_back(WfApp *app, WfUndoEntry *u)
{
	WfTransferSlot s;

	if (!slot_of_entry(&s, u)) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_NOMEM));
		return;
	}
	s.mode = WF_XFER_MOVE;
	bool ok = wf_app_move_begin(app, &s, u->origin, &u->origin_id, true);

	wf_slot_free(&s);
	if (ok) {
		app->undo_inflight = u->uid;
	}
}

void wf_app_undo_done(WfApp *app, size_t done)
{
	size_t at = undo_find(app, app->undo_inflight);
	WfUndoEntry *u;

	app->undo_inflight = 0;
	if (SIZE_MAX == at) {
		return; /* the entry is no longer there: nothing to update */
	}
	u = &app->undo[at];
	if (done >= u->n) {
		undo_remove(app, at);
		return;
	}
	if (0 == done) {
		return; /* nothing came back: the entry stays whole */
	}
	/* the first `done` are back; the rest stay for another try */
	undo_drop_first(u, done);
}

/* Rename and move are taken back only after the question names what, how
 * many and where; the entry is looked up again by its id when it is answered. */
static void undo_go(WfApp *app, void *ctx)
{
	size_t at = undo_find(app, app->undo_ask_uid);

	(void)ctx;
	app->undo_ask_uid = 0;
	if (SIZE_MAX == at) {
		return;
	}
	if (WF_UNDO_RENAME == app->undo[at].kind) {
		undo_rename_back(app, &app->undo[at]);
	} else if (WF_UNDO_MOVE == app->undo[at].kind) {
		undo_move_back(app, &app->undo[at]);
	} else if (WF_UNDO_BATCH == app->undo[at].kind) {
		(void)wf_app_batch_undo_begin(app, &app->undo[at]);
	}
}

static void undo_ask(WfApp *app, WfUndoEntry *u)
{
	char l1[WF_CONFIRM_LINE_BYTES], l2[WF_CONFIRM_LINE_BYTES];
	const char *lines[4];
	WfStatus st;

	/* nothing is asked while an earlier request is still being carried out:
	 * the answer would start a second one */
	if (app->edit_busy || app->move.active || app->undo_inflight ||
	        wf_app_batch_busy(app)) {
		wf_app_set_msg(app, wf_msg(MSG_OPS_BUSY));
		return;
	}
	if (WF_UNDO_BATCH == u->kind) {
		(void)wf_msgf(l1, sizeof l1, MSG_JOB_UNDO_BATCH_LINE,
		        &WF_ARG_N(u->n), 1);
		where_line(l2, sizeof l2, MSG_JOB_UNDO_WHERE, u->folder);
		lines[0] = wf_msg(MSG_JOB_UNDO_BATCH_TITLE);
	} else if (WF_UNDO_RENAME == u->kind) {
		WfBuf now, before;
		char a[48], b[48];

		wf_buf_init(&now);
		wf_buf_init(&before);
		(void)wf_sanitize_utf16(
		        u->item[0].name, wcslen(u->item[0].name), &now);
		(void)wf_sanitize_utf16(u->item[0].old_name,
		        wcslen(u->item[0].old_name), &before);
		wf_utf8_elide(a, sizeof a, wf_buf_cstr(&now), 30);
		wf_utf8_elide(b, sizeof b, wf_buf_cstr(&before), 30);
		(void)wf_msgf(l1, sizeof l1, MSG_JOB_UNDO_RENAME_LINE,
		        (WfMsgArg[]){WF_ARG_S(a), WF_ARG_S(b)}, 2);
		wf_buf_free(&now);
		wf_buf_free(&before);
		where_line(l2, sizeof l2, MSG_JOB_UNDO_WHERE, u->folder);
		lines[0] = wf_msg(MSG_JOB_UNDO_RENAME_TITLE);
	} else {
		(void)wf_msgf(l1, sizeof l1, MSG_JOB_UNDO_MOVE_LINE,
		        &WF_ARG_N(u->n), 1);
		where_line(l2, sizeof l2, MSG_JOB_UNDO_TO, u->origin);
		lines[0] = wf_msg(MSG_JOB_UNDO_MOVE_TITLE);
	}
	lines[1] = l1;
	lines[2] = l2;
	app->undo_ask_uid = u->uid;
	st = wf_app_confirm_begin(app, lines, 3, undo_go, NULL, NULL);
	if (WF_OK != st) {
		app->undo_ask_uid = 0;
		wf_app_set_error(app, st);
	}
}

static void undo_last(WfApp *app)
{
	WfUndoEntry *u;

	if (WF_XB_IDLE != app->xfer_busy || app->copy_jobs) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_UNDO_WAIT));
		return;
	}
	if (!app->undo_n) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_UNDO_NOTHING));
		return;
	}
	u = &app->undo[app->undo_n - 1];
	if (u->kind == WF_UNDO_RECYCLE_CREATED &&
	        !wf_copyjob_allowed(WF_CJ_RECYCLE, WF_COLLIDE_STOP)) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_RECYCLE_UNAVAILABLE));
		return; /* retain the receipt; never retire an unverified object */
	}
	switch (u->kind) {
	case WF_UNDO_RENAME:          undo_ask(app, u); break;
	case WF_UNDO_MOVE:            undo_ask(app, u); break;
	case WF_UNDO_BATCH:           undo_ask(app, u); break;
	case WF_UNDO_RECYCLE_CREATED: undo_recycle_copies(app, u); break;
	}
}

void wf_app_cmd_undo(WfApp *app)
{
	if (wf_app_wsl_undo(app)) {
		return;
	}
	undo_last(app);
}

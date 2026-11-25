#include "wf_treewalk.h"

#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"

typedef struct
{
	wchar_t *path; /* owned */
	WfFileId id;
	size_t depth;
	size_t bytes; /* of the path, with its NUL: what it adds to Queue.bytes */
} Folder;

/* A queue of folders to read: first in, first out, so nearer folders come first. */
typedef struct
{
	Folder *f;
	size_t head, n, cap;
	size_t bytes; /* of the paths that wait */
} Queue;

static bool queue_push(Queue *q, wchar_t *path, WfFileId id, size_t depth)
{
	if (q->head && q->head == q->n) { /* all taken: start over */
		q->head = q->n = 0;
	}
	if (q->n == q->cap) {
		size_t live = q->n - q->head;

		if (q->head > q->cap / 2) { /* slide down before growing */
			memmove(q->f, q->f + q->head, live * sizeof *q->f);
			q->n = live;
			q->head = 0;
		} else {
			size_t cap = q->cap ? q->cap * 2 : 64;
			Folder *grown = realloc(q->f, cap * sizeof *grown);

			if (!grown) {
				return false;
			}
			q->f = grown;
			q->cap = cap;
		}
	}
	q->f[q->n].path = path;
	q->f[q->n].id = id;
	q->f[q->n].depth = depth;
	q->f[q->n].bytes = (wcslen(path) + 1) * sizeof *path;
	q->bytes += q->f[q->n].bytes;
	q->n++;
	return true;
}

static size_t queue_live(const Queue *q)
{
	return q->n - q->head;
}

static void queue_free(Queue *q)
{
	for (size_t i = q->head; i < q->n; i++) {
		free(q->f[i].path);
	}
	free(q->f);
	memset(q, 0, sizeof *q);
}

WfStatus wf_walk(const WfWalkOps *ops,
        void *ctx,
        const wchar_t *root,
        WfFileId root_id,
        WfWalkStats *stats)
{
	Queue q = {0};
	WfWalkStats local = {0};
	wchar_t *start;
	WfStatus st = WF_OK;
	size_t budget;

	if (!ops || !ops->list || !ops->visit || !root || !*root) {
		return WF_E_INVAL;
	}
	if (!stats) {
		stats = &local;
	}
	memset(stats, 0, sizeof *stats);
	budget = ops->max_pending_bytes ? ops->max_pending_bytes
	                                : WF_WALK_MAX_PENDING_BYTES;
	start = wf_path_dup(root);
	if (!start || !queue_push(&q, start, root_id, 0)) {
		free(start);
		queue_free(&q);
		return WF_E_NOMEM;
	}
	while (queue_live(&q) > 0 && WF_OK == st) {
		Folder cur = q.f[q.head++];
		WfDir *d = NULL;

		q.bytes -= cur.bytes;
		WfStatus ls;

		if (ops->cancelled && ops->cancelled(ctx)) {
			stats->cancelled = true;
			free(cur.path);
			break;
		}
		ls = ops->list(ctx, cur.path, &d);
		if (WF_OK != ls || !d) {
			wf_dir_free(d);
			free(cur.path);
			/* What a folder can say about itself and be passed by (it is
			 * refused, gone, not readable) is counted and the walk goes on.
			 * Running out of memory is not about the folder: the walk is
			 * incomplete for a reason of its own, and says so. A listing that
			 * was cancelled is the walk being cancelled. */
			if (WF_E_NOMEM == ls) {
				st = WF_E_NOMEM;
			} else if (WF_E_CANCELLED == ls) {
				stats->cancelled = true;
				break;
			} else {
				stats->unreadable++;
			}
			continue;
		}
		stats->folders++;
		stats->partial += d->truncated ? 1u : 0u;
		for (size_t i = 0; i < d->n; i++) {
			const WfEntry *e = &d->e[i];
			WfWalkItem item;
			bool descend;

			if (!ops->hidden && (e->flags & WF_ENT_HIDDEN)) {
				continue;
			}
			if (ops->cancelled && 0 == (i & 63u) &&
			        ops->cancelled(ctx)) {
				stats->cancelled = true;
				break;
			}
			item.dir = cur.path;
			item.dir_id = cur.id;
			item.depth = cur.depth;
			item.e = e;
			stats->entries++;
			if (!ops->visit(ctx, &item)) {
				stats->stopped = true;
				break;
			}
			descend = (e->flags & WF_ENT_DIR) &&
			        !(e->flags & WF_ENT_LINK) &&
			        !(e->flags & WF_ENT_CLOUD);
			if ((e->flags & WF_ENT_DIR) && !descend) {
				stats->not_entered++;
			}
			if (descend) {
				wchar_t *child = NULL;
				size_t dn = wcslen(cur.path);
				/* the length the joined path would have, known before it
				 * is built: a path that is too long, or a queue that is
				 * full, costs no allocation */
				size_t len = dn +
				        (dn > 0 && L'\\' != cur.path[dn - 1]
				                        ? 1u
				                        : 0u) +
				        wcslen(e->name);

				if (len > WF_WALK_MAX_PATH) {
					stats->not_entered++;
				} else if (queue_live(&q) >=
				                WF_WALK_MAX_PENDING ||
				        q.bytes + (len + 1) * sizeof *child >
				                budget) {
					stats->pending_full = true;
				} else if (WF_OK !=
				        wf_path_join(
				                cur.path, e->name, &child)) {
					st = WF_E_NOMEM;
					break;
				} else if (!queue_push(&q, child, e->identity,
				                   cur.depth + 1)) {
					free(child);
					st = WF_E_NOMEM;
					break;
				}
			}
		}
		wf_dir_free(d);
		free(cur.path);
		if (stats->cancelled || stats->stopped) {
			break;
		}
	}
	queue_free(&q);
	return st;
}

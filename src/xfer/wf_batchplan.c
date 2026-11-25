#include "wf_batchplan.h"

#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"

/* The key under which two names of one folder are the same name: the name
 * itself in a case sensitive folder, its upper case form otherwise (the system's
 * own table). Owned by the caller. */
static wchar_t *fold_key(const wchar_t *name, bool case_sensitive)
{
	size_t n = wcslen(name);
	wchar_t *k = malloc((n + 1) * sizeof *k);

	if (!k) {
		return NULL;
	}
	memcpy(k, name, (n + 1) * sizeof *k);
	if (!case_sensitive && n) {
		CharUpperBuffW(k, (DWORD)n);
	}
	return k;
}

typedef enum
{
	K_EXISTING, /* a name that exists in the folder now */
	K_SOURCE,   /* the name of an item of the batch now */
	K_TARGET    /* the name an item of the batch is to get */
} Kind;

typedef struct
{
	uint32_t parent;
	Kind kind;
	uint32_t item; /* K_SOURCE, K_TARGET */
	bool stays; /* K_SOURCE: the item is not renamed, it vacates nothing */
	wchar_t *key;
} Entry;

static int entry_cmp(const void *pa, const void *pb)
{
	const Entry *a = pa, *b = pb;
	int c;

	if (a->parent != b->parent) {
		return a->parent < b->parent ? -1 : 1;
	}
	c = wcscmp(a->key, b->key);
	if (c) {
		return c;
	}
	return (int)a->kind - (int)b->kind;
}

static bool same_group(const Entry *a, const Entry *b)
{
	return a->parent == b->parent && !wcscmp(a->key, b->key);
}

static bool id_same(const WfFileId *a, const WfFileId *b)
{
	return a->volume == b->volume && !memcmp(a->id, b->id, sizeof a->id);
}

typedef struct
{
	const WfFileId *id;
	uint32_t item;
} IdRef;

static int idref_cmp(const void *pa, const void *pb)
{
	const IdRef *a = pa, *b = pb;

	if (a->id->volume != b->id->volume) {
		return a->id->volume < b->id->volume ? -1 : 1;
	}
	return memcmp(a->id->id, b->id->id, sizeof a->id->id);
}

static bool refused(WfBatchWhy why)
{
	return WF_BP_OK != why && WF_BP_UNCHANGED != why;
}

static void mark(WfBatchRow *r, WfBatchWhy why)
{
	if (!refused(r->why)) {
		r->why = why; /* the first reason stays */
	}
}

/* `path` is `dir` or inside it, comparing the way a folder does. */
static bool path_inside(const wchar_t *path, const wchar_t *dir)
{
	size_t n = wcslen(dir);

	if (n > wcslen(path)) {
		return false;
	}
	if (CSTR_EQUAL !=
	        CompareStringOrdinal(path, (int)n, dir, (int)n, TRUE)) {
		return false;
	}
	return L'\0' == path[n] || L'\\' == path[n] || L'\\' == dir[n - 1];
}

static const wchar_t HEX[] = L"0123456789abcdef";

static wchar_t *temp_name(const uint8_t id[16], uint32_t item)
{
	wchar_t buf[13 + 32 + 1 + 10 + 1];
	size_t n = 0;
	static const wchar_t prefix[] = L".piero-batch-";
	char digits[10];
	int d = 0;

	for (size_t i = 0; i + 1 < sizeof prefix / sizeof *prefix; i++) {
		buf[n++] = prefix[i];
	}
	for (size_t i = 0; i < 16; i++) {
		buf[n++] = HEX[id[i] >> 4];
		buf[n++] = HEX[id[i] & 15];
	}
	buf[n++] = L'-';
	do {
		digits[d++] = (char)('0' + item % 10);
		item /= 10;
	} while (item);
	while (d) {
		buf[n++] = (wchar_t)digits[--d];
	}
	buf[n] = L'\0';
	return wf_path_dup_n(buf, n);
}

static WfStatus check_input(const WfBatchParent *parent,
        size_t n_parent,
        const WfBatchItem *item,
        size_t n_item)
{
	if (!parent || !item || !n_parent || !n_item ||
	        n_item > WF_BATCH_MAX_ITEMS ||
	        n_parent > WF_BATCH_MAX_PARENTS) {
		return n_item > WF_BATCH_MAX_ITEMS ? WF_E_RANGE : WF_E_INVAL;
	}
	for (size_t i = 0; i < n_parent; i++) {
		if (!parent[i].path ||
		        (parent[i].n_names && !parent[i].names)) {
			return WF_E_INVAL;
		}
		for (size_t j = 0; j < i; j++) {
			if ((parent[i].id.kind == WF_ID_STRONG) &&
			        (parent[j].id.kind == WF_ID_STRONG) &&
			        id_same(&parent[i].id, &parent[j].id)) {
				return WF_E_INVAL; /* one folder given twice */
			}
		}
	}
	for (size_t i = 0; i < n_item; i++) {
		if (item[i].parent >= n_parent ||
		        item[i].to_parent >= n_parent || !item[i].name ||
		        !item[i].to_name) {
			return WF_E_INVAL;
		}
	}
	return WF_OK;
}

/* Everything about one item that needs nothing of the others. */
static void check_each(const WfBatchParent *parent,
        const WfBatchItem *item,
        size_t n,
        WfBatchRow *row)
{
	uint64_t volume = parent[0].id.volume;

	for (size_t i = 0; i < n; i++) {
		const WfBatchItem *it = &item[i];
		const WfBatchParent *to = &parent[it->to_parent];
		size_t len = wcslen(it->to_name);

		if (!(it->id.kind == WF_ID_STRONG) ||
		        !(parent[it->parent].id.kind == WF_ID_STRONG) ||
		        !(to->id.kind == WF_ID_STRONG)) {
			mark(&row[i], WF_BP_NO_ID);
			continue;
		}
		if (it->id.volume != volume || to->id.volume != volume ||
		        parent[it->parent].id.volume != volume) {
			mark(&row[i], WF_BP_VOLUME);
			continue;
		}
		if (parent[it->parent].link || to->link) {
			mark(&row[i], WF_BP_LINK_PARENT);
			continue;
		}
		if (it->link) {
			mark(&row[i], WF_BP_LINK_ITEM);
			continue;
		}
		if (it->parent == it->to_parent &&
		        !wcscmp(it->name, it->to_name)) {
			row[i].why = WF_BP_UNCHANGED;
			continue;
		}
		row[i].name =
		        wf_name_validate(it->to_name, len, to->component_max);
		if (WF_NAME_OK != row[i].name) {
			mark(&row[i], WF_BP_NAME);
			continue;
		}
		if (wcslen(to->path) + 1 + len > WF_BATCH_PATH_MAX) {
			mark(&row[i], WF_BP_PATH_LONG);
		}
	}
}

static WfStatus check_same_file(
        const WfBatchItem *item, size_t n, WfBatchRow *row)
{
	IdRef *ref = malloc(n * sizeof *ref);

	if (!ref) {
		return WF_E_NOMEM;
	}
	for (size_t i = 0; i < n; i++) {
		ref[i].id = &item[i].id;
		ref[i].item = (uint32_t)i;
	}
	qsort(ref, n, sizeof *ref, idref_cmp);
	for (size_t i = 1; i < n; i++) {
		if ((ref[i - 1].id->kind == WF_ID_STRONG) &&
		        !idref_cmp(&ref[i - 1], &ref[i])) {
			mark(&row[ref[i - 1].item], WF_BP_DUP_ITEM);
			mark(&row[ref[i].item], WF_BP_DUP_ITEM);
		}
	}
	free(ref);
	return WF_OK;
}

/* An item inside a folder that is itself an item, and a move into one. */
static WfStatus check_nested(const WfBatchParent *parent,
        size_t n_parent,
        const WfBatchItem *item,
        size_t n,
        WfBatchRow *row)
{
	for (size_t d = 0; d < n; d++) {
		wchar_t *full;

		if (!item[d].dir || WF_BP_UNCHANGED == row[d].why) {
			continue;
		}
		if (WF_OK !=
		        wf_path_join(parent[item[d].parent].path, item[d].name,
		                &full)) {
			return WF_E_NOMEM;
		}
		for (size_t q = 0; q < n_parent; q++) {
			if (!path_inside(parent[q].path, full)) {
				continue;
			}
			for (size_t i = 0; i < n; i++) {
				if (i != d &&
				        (item[i].parent == q ||
				                item[i].to_parent == q)) {
					mark(&row[i], WF_BP_NESTED);
				}
			}
			/* a folder moved into itself: its own target is in it */
			if (item[d].to_parent == q) {
				mark(&row[d], WF_BP_NESTED);
			}
		}
		free(full);
	}
	return WF_OK;
}

typedef struct
{
	Entry *e;
	size_t n;
} Entries;

static void entries_free(Entries *s)
{
	for (size_t i = 0; i < s->n; i++) {
		free(s->e[i].key);
	}
	free(s->e);
	memset(s, 0, sizeof *s);
}

static bool entry_add(Entries *s,
        uint32_t parent,
        Kind kind,
        uint32_t item,
        bool stays,
        const wchar_t *name,
        bool case_sensitive)
{
	Entry *e = &s->e[s->n];

	e->key = fold_key(name, case_sensitive);
	if (!e->key) {
		return false;
	}
	e->parent = parent;
	e->kind = kind;
	e->item = item;
	e->stays = stays;
	s->n++;
	return true;
}

/* Names against names, per folder: duplicates among the targets, targets that
 * exist, and who must leave a name before another may take it (dep[i]). */
static WfStatus check_names(const WfBatchParent *parent,
        size_t n_parent,
        const WfBatchItem *item,
        size_t n,
        WfBatchRow *row,
        uint32_t *dep)
{
	Entries s = {0};
	size_t total = 2 * n, a = 0;
	WfStatus st = WF_OK;

	for (size_t q = 0; q < n_parent; q++) {
		total += parent[q].n_names;
	}
	s.e = calloc(total ? total : 1, sizeof *s.e);
	if (!s.e) {
		return WF_E_NOMEM;
	}
	for (size_t q = 0; q < n_parent && WF_OK == st; q++) {
		for (size_t k = 0; k < parent[q].n_names; k++) {
			if (!entry_add(&s, (uint32_t)q, K_EXISTING, 0, false,
			            parent[q].names[k],
			            parent[q].case_sensitive)) {
				st = WF_E_NOMEM;
				break;
			}
		}
	}
	for (size_t i = 0; i < n && WF_OK == st; i++) {
		const WfBatchParent *from = &parent[item[i].parent];
		const WfBatchParent *to = &parent[item[i].to_parent];
		bool stays =
		        WF_BP_UNCHANGED == row[i].why || refused(row[i].why);

		if (!entry_add(&s, item[i].parent, K_SOURCE, (uint32_t)i, stays,
		            item[i].name, from->case_sensitive)) {
			st = WF_E_NOMEM;
			break;
		}
		if (!stays &&
		        !entry_add(&s, item[i].to_parent, K_TARGET, (uint32_t)i,
		                false, item[i].to_name, to->case_sensitive)) {
			st = WF_E_NOMEM;
		}
	}
	if (WF_OK != st) {
		entries_free(&s);
		return st;
	}
	qsort(s.e, s.n, sizeof *s.e, entry_cmp);
	while (a < s.n) {
		size_t b = a, targets = 0;
		const Entry *source = NULL;
		bool existing = false;

		while (b < s.n && same_group(&s.e[a], &s.e[b])) {
			targets += K_TARGET == s.e[b].kind;
			if (K_SOURCE == s.e[b].kind) {
				source = &s.e[b];
			}
			existing |= K_EXISTING == s.e[b].kind;
			b++;
		}
		for (size_t k = a; k < b; k++) {
			uint32_t i;

			if (K_TARGET != s.e[k].kind) {
				continue;
			}
			i = s.e[k].item;
			if (targets > 1) {
				mark(&row[i], WF_BP_DUP_TARGET);
			} else if (source) {
				if (source->item == i) {
					continue; /* only the case of its own name */
				}
				if (source->stays) {
					mark(&row[i], WF_BP_EXISTS);
				} else {
					dep[i] = source->item;
				}
			} else if (existing) {
				mark(&row[i], WF_BP_EXISTS);
			}
		}
		a = b;
	}
	entries_free(&s);
	return WF_OK;
}

static bool step_add(WfBatchPlan *p,
        size_t *cap,
        uint32_t item,
        uint32_t to_parent,
        const wchar_t *name,
        bool temp)
{
	WfBatchStep *s;

	if (p->n_steps == *cap) {
		size_t c = *cap ? *cap * 2 : 64;
		WfBatchStep *grown = realloc(p->step, c * sizeof *grown);

		if (!grown) {
			return false;
		}
		p->step = grown;
		*cap = c;
	}
	s = &p->step[p->n_steps];
	s->item = item;
	s->to_parent = to_parent;
	s->temp = temp;
	s->to_name = wf_path_dup(name);
	if (!s->to_name) {
		return false;
	}
	p->n_steps++;
	return true;
}

#define NONE UINT32_MAX

/* Orders the steps: an item waits for the item whose name it takes. What
 * results are chains, which run from the free end, and cycles, which are broken
 * by sending one member to a temporary name and back at the end. */
static WfStatus order_steps(const WfBatchItem *item,
        size_t n,
        const WfBatchRow *row,
        const uint32_t *dep,
        const uint8_t batch_id[16],
        WfBatchPlan *p)
{
	uint8_t *state = calloc(n, 1); /* 0 new, 1 on the path, 2 emitted */
	uint32_t *path = malloc(n * sizeof *path);
	size_t cap = 0;
	WfStatus st = WF_OK;

	if (!state || !path) {
		free(state);
		free(path);
		return WF_E_NOMEM;
	}
	for (size_t start = 0; start < n && WF_OK == st; start++) {
		size_t depth = 0, top;
		uint32_t at = (uint32_t)start;
		size_t cut = NONE;

		if (WF_BP_UNCHANGED == row[start].why || state[start]) {
			continue;
		}
		while (NONE != at && 0 == state[at]) {
			state[at] = 1;
			path[depth++] = at;
			at = dep[at];
		}
		if (NONE != at && 1 == state[at]) {
			/* a cycle: at .. path[depth-1]; `at` goes aside first */
			wchar_t *tmp = temp_name(batch_id, at);

			for (cut = 0; cut < depth && path[cut] != at; cut++) {
			}
			if (!tmp ||
			        !step_add(p, &cap, at, item[at].parent, tmp,
			                true)) {
				free(tmp);
				st = WF_E_NOMEM;
				break;
			}
			free(tmp);
			p->row[at].temp = true;
			p->n_cycles++;
		}
		/* what waits at the end of the path goes first */
		top = depth;
		while (top > 0 && WF_OK == st) {
			uint32_t i = path[--top];

			if (NONE != cut && top == cut) {
				continue; /* the item that went aside: last */
			}
			if (!step_add(p, &cap, i, item[i].to_parent,
			            item[i].to_name, false)) {
				st = WF_E_NOMEM;
			}
			state[i] = 2;
		}
		if (NONE != cut && WF_OK == st) {
			uint32_t i = path[cut];

			if (!step_add(p, &cap, i, item[i].to_parent,
			            item[i].to_name, false)) {
				st = WF_E_NOMEM;
			}
			state[i] = 2;
		}
	}
	free(state);
	free(path);
	return st;
}

WfStatus wf_batch_plan(const WfBatchParent *parent,
        size_t n_parent,
        const WfBatchItem *item,
        size_t n_item,
        const uint8_t batch_id[16],
        WfBatchPlan *out)
{
	uint32_t *dep = NULL;
	WfStatus st;

	if (!out || !batch_id) {
		return WF_E_INVAL;
	}
	memset(out, 0, sizeof *out);
	st = check_input(parent, n_parent, item, n_item);
	if (WF_OK != st) {
		return st;
	}
	out->n = n_item;
	out->row = calloc(n_item, sizeof *out->row);
	dep = malloc(n_item * sizeof *dep);
	if (!out->row || !dep) {
		free(dep);
		wf_batch_plan_free(out);
		return WF_E_NOMEM;
	}
	for (size_t i = 0; i < n_item; i++) {
		dep[i] = NONE;
	}
	check_each(parent, item, n_item, out->row);
	st = check_same_file(item, n_item, out->row);
	if (WF_OK == st) {
		st = check_nested(parent, n_parent, item, n_item, out->row);
	}
	if (WF_OK == st) {
		st = check_names(parent, n_parent, item, n_item, out->row, dep);
	}
	for (size_t i = 0; i < n_item && WF_OK == st; i++) {
		out->n_refused += refused(out->row[i].why);
		out->n_skipped += WF_BP_UNCHANGED == out->row[i].why;
	}
	if (WF_OK == st && 0 == out->n_refused) {
		st = order_steps(item, n_item, out->row, dep, batch_id, out);
	}
	free(dep);
	if (WF_OK != st) {
		wf_batch_plan_free(out);
	}
	return st;
}

void wf_batch_plan_free(WfBatchPlan *p)
{
	if (!p) {
		return;
	}
	for (size_t i = 0; i < p->n_steps; i++) {
		free(p->step[i].to_name);
	}
	free(p->step);
	free(p->row);
	memset(p, 0, sizeof *p);
}

const char *wf_batch_why_text(WfBatchWhy why)
{
	switch (why) {
	case WF_BP_OK:         return "ok";
	case WF_BP_UNCHANGED:  return "the name is not changed";
	case WF_BP_NAME:       return "the new name is not a valid name";
	case WF_BP_PATH_LONG:  return "the new path is too long";
	case WF_BP_DUP_TARGET: return "another item gets the same name";
	case WF_BP_EXISTS:     return "a file or folder of that name exists";
	case WF_BP_DUP_ITEM:   return "the same file is selected twice";
	case WF_BP_NESTED:
		return "it is inside another selected folder, or moved into "
		       "itself";
	case WF_BP_VOLUME:      return "it is not on the same volume";
	case WF_BP_NO_ID:       return "its identity could not be read";
	case WF_BP_LINK_PARENT: return "its folder is a link";
	case WF_BP_LINK_ITEM: return "it is a link, and links are not renamed";
	}
	return "refused";
}

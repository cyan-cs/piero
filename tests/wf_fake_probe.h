#ifndef WF_FAKE_PROBE_H
#define WF_FAKE_PROBE_H

#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "path/wf_path.h"
#include "xfer/wf_xfer.h"

/* An in-memory filesystem for preflight tests. Paths compare
 * case-insensitively, like NTFS defaults. */
#define FP_MAX_NODES 4096

typedef struct
{
	wchar_t path[300];
	uint32_t flags, tag;
	uint64_t size, mtime;
	uint64_t vol;
	uint64_t id;         /* 0 = no usable file ID (FAT-like) */
	uint64_t resolve_to; /* link: id of the object it resolves to (node()
	                        shows its own id) */
	int io_error;        /* node() fails with WF_E_OS */
	int list_truncated;
	int list_error;
} FpNode;

typedef struct
{
	wchar_t prefix[64];
	WfVolInfo info;
} FpVol;

typedef struct
{
	FpNode n[FP_MAX_NODES];
	size_t count;
	FpVol vols[4];
	size_t nvols;
	WfStatus access_status;
	int node_calls;
} FakeProbe;

static int fp_eq(const wchar_t *a, const wchar_t *b)
{
	size_t na = wcslen(a), nb = wcslen(b);

	while (na > 1 && L'\\' == a[na - 1]) {
		na--;
	}
	while (nb > 1 && L'\\' == b[nb - 1]) {
		nb--;
	}
	if (na != nb) {
		return 0;
	}
	for (size_t i = 0; i < na; i++) {
		if (towlower(a[i]) != towlower(b[i])) {
			return 0;
		}
	}
	return 1;
}

static FpNode *fp_find(FakeProbe *p, const wchar_t *path)
{
	for (size_t i = 0; i < p->count; i++) {
		if (fp_eq(p->n[i].path, path)) {
			return &p->n[i];
		}
	}
	return NULL;
}

static FpNode *fp_add(FakeProbe *p,
        const wchar_t *path,
        uint32_t flags,
        uint64_t size,
        uint64_t id)
{
	FpNode *n = &p->n[p->count++];

	memset(n, 0, sizeof *n);
	wcscpy_s(n->path, 300, path);
	n->flags = flags;
	n->size = size;
	n->mtime = 1000;
	n->vol = 1;
	n->id = id;
	return n;
}

static void fp_init(FakeProbe *p)
{
	memset(p, 0, sizeof *p);
	p->nvols = 1;
	wcscpy_s(p->vols[0].prefix, 64, L"C:");
	wcscpy_s(p->vols[0].info.root, 260, L"C:\\");
	wcscpy_s(p->vols[0].info.fs_name, 16, L"NTFS");
	p->vols[0].info.serial = 0xAAAA;
	p->vols[0].info.max_component = 255;
	p->vols[0].info.is_local = 1;
	p->vols[0].info.supports_open_by_id = 1;
	p->vols[0].info.free_bytes = 1ull << 40;
	fp_add(p, L"C:\\", WF_ENT_DIR, 0, 1);
}

static WfStatus fp_node(void *ctx, const wchar_t *path, WfNodeInfo *out)
{
	FakeProbe *p = ctx;
	FpNode *n = fp_find(p, path);

	p->node_calls++;
	if (!n) {
		return WF_E_NOENT;
	}
	if (n->io_error) {
		return WF_E_OS;
	}
	memset(out, 0, sizeof *out);
	out->flags = n->flags;
	out->reparse_tag = n->tag;
	out->size = n->size;
	out->mtime = n->mtime;
	out->nlinks = 1;
	if (n->id) {
		out->id.kind = WF_ID_STRONG;
		out->id.volume = n->vol;
		memcpy(out->id.id, &n->id, sizeof n->id);
	}
	return WF_OK;
}

static WfStatus fp_volume(void *ctx, const wchar_t *path, WfVolInfo *out)
{
	FakeProbe *p = ctx;
	size_t best = (size_t)-1, bestlen = 0;

	for (size_t i = 0; i < p->nvols; i++) {
		size_t l = wcslen(p->vols[i].prefix);

		if (0 == _wcsnicmp(path, p->vols[i].prefix, l) &&
		        l >= bestlen) {
			best = i;
			bestlen = l;
		}
	}
	if (best == (size_t)-1) {
		return WF_E_NOENT;
	}
	*out = p->vols[best].info;
	return WF_OK;
}

static WfStatus fp_resolve(void *ctx, const wchar_t *path, WfFileId *out)
{
	FakeProbe *p = ctx;
	FpNode *n = fp_find(p, path);
	uint64_t id;

	memset(out, 0, sizeof *out);
	if (!n) {
		return WF_E_NOENT;
	}
	id = n->resolve_to ? n->resolve_to : n->id;
	if (id) {
		out->kind = WF_ID_STRONG;
		out->volume = n->vol;
		memcpy(out->id, &id, sizeof id);
	}
	return WF_OK;
}

static WfStatus fp_list(void *ctx, const wchar_t *dir, WfDir **out)
{
	FakeProbe *p = ctx;
	FpNode *self = fp_find(p, dir);
	WfDir *d;

	if (!self || self->list_error) {
		return WF_E_ACCESS;
	}
	d = wf_dir_new();
	for (size_t i = 0; i < p->count; i++) {
		size_t pl = wf_path_parent_len(p->n[i].path);
		wchar_t parent[300];

		if (0 == pl) {
			continue;
		}
		wcsncpy_s(parent, 300, p->n[i].path, pl);
		if (!fp_eq(parent, dir)) {
			continue;
		}
		{
			const wchar_t *leaf;
			size_t ln;

			wf_path_leaf(p->n[i].path, &leaf, &ln);
			wf_dir_add_ex(d, leaf, ln, p->n[i].flags, p->n[i].size,
			        p->n[i].mtime, p->n[i].tag);
		}
	}
	d->truncated = self->list_truncated;
	*out = d;
	return WF_OK;
}

static WfStatus fp_access(void *ctx, const wchar_t *dir)
{
	(void)dir;
	return ((FakeProbe *)ctx)->access_status;
}

static WfProbeOps fp_ops(FakeProbe *p)
{
	WfProbeOps ops = {
	        p, fp_node, fp_volume, fp_resolve, fp_list, fp_access};

	return ops;
}

/* Convenience: add a directory/file under an existing path. */
static void fp_dir(FakeProbe *p, const wchar_t *path, uint64_t id)
{
	fp_add(p, path, WF_ENT_DIR, 0, id);
}

static void fp_file(
        FakeProbe *p, const wchar_t *path, uint64_t size, uint64_t id)
{
	fp_add(p, path, 0, size, id);
}

#endif

#include "wf_xfer.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../path/wf_path.h"

bool wf_fileid_equal(const WfFileId *a, const WfFileId *b)
{
	return wf_fileid_proven(a) && wf_fileid_proven(b) &&
	        a->volume == b->volume &&
	        0 == memcmp(a->id, b->id, sizeof a->id);
}

bool wf_fileid_equal_or_weak(const WfFileId *a, const WfFileId *b)
{
	return wf_fileid_equal(a, b) ||
	        (wf_fileid_is_weak(a) && wf_fileid_is_weak(b) &&
	                a->volume == b->volume &&
	                0 == memcmp(a->id, b->id, sizeof a->id));
}

bool wf_fileid_usable_to_read(const WfFileId *id)
{
	return id && WF_ID_NONE != id->kind;
}

/* Tags, from winnt.h. Name-surrogate kinds that redirect elsewhere. */
#define TAG_MOUNT_POINT 0xA0000003u
#define TAG_SYMLINK 0xA000000Cu
/* Transparent: content still reads as the file's own bytes. */
#define TAG_DEDUP 0x80000013u
#define TAG_WOF 0x80000017u
#define TAG_CLOUD_BASE                                                         \
	0x9000001Au /* CLOUD, CLOUD_1 .. CLOUD_F vary bits 12-15 */

WfReparseClass wf_reparse_classify(uint32_t flags, uint32_t tag)
{
	if (!(flags & WF_ENT_LINK)) {
		return WF_REPARSE_NONE;
	}
	if (TAG_MOUNT_POINT == tag || TAG_SYMLINK == tag) {
		return WF_REPARSE_REDIRECT;
	}
	if (TAG_DEDUP == tag || TAG_WOF == tag ||
	        TAG_CLOUD_BASE == (tag & 0xFFFF0FFFu)) {
		return WF_REPARSE_TRANSPARENT;
	}
	return WF_REPARSE_UNKNOWN;
}

void wf_slot_init(WfTransferSlot *s)
{
	memset(s, 0, sizeof *s);
}

void wf_slot_free(WfTransferSlot *s)
{
	size_t i;

	if (!s) {
		return;
	}
	for (i = 0; i < s->count && s->src; i++) {
		free(s->src[i].name);
	}
	free(s->src);
	free(s->dir);
	wf_slot_init(s);
}

static wchar_t *dup_w(const wchar_t *p)
{
	return wf_path_dup(p);
}

WfStatus wf_slot_clone(WfTransferSlot *dst, const WfTransferSlot *src)
{
	WfTransferSlot c;
	size_t i;

	if (!dst || !src || dst == src || (src->count && !src->src)) {
		return WF_E_INVAL;
	}
	for (i = 0; i < src->count; i++) {
		if (!src->src[i].name) {
			return WF_E_INVAL;
		}
	}
	wf_slot_init(&c);
	c.mode = src->mode;
	c.dir_id = src->dir_id;
	c.hidden_excluded = src->hidden_excluded;
	c.collisions_ok = src->collisions_ok;
	if (src->dir && !(c.dir = dup_w(src->dir))) {
		goto nomem;
	}
	if (src->count) {
		c.src = calloc(src->count, sizeof *c.src);
		if (!c.src) {
			goto nomem;
		}
	}
	for (i = 0; i < src->count; i++) {
		c.src[i].snap = src->src[i].snap;
		if (!(c.src[i].name = dup_w(src->src[i].name))) {
			goto nomem;
		}
		c.count = i + 1; /* so wf_slot_free releases what exists */
	}
	*dst = c;
	return WF_OK;
nomem:
	wf_slot_free(&c);
	return WF_E_NOMEM;
}

WfStatus wf_xfer_capture(const WfProbeOps *ops,
        WfTransferMode mode,
        const wchar_t *dir,
        const wchar_t *const *names,
        size_t n,
        size_t hidden_excluded,
        WfTransferSlot *out)
{
	WfTransferSlot s;
	WfNodeInfo dn;
	size_t i;
	WfStatus st;

	if (!out) {
		return WF_E_INVAL;
	}
	wf_slot_init(out);
	if (!ops || !ops->node || !dir || !names || WF_XFER_NONE == mode ||
	        0 == n) {
		return WF_E_INVAL;
	}
	for (i = 0; i < n; i++) {
		if (!names[i]) {
			return WF_E_INVAL;
		}
	}

	wf_slot_init(&s);
	s.mode = mode;
	s.hidden_excluded = hidden_excluded;
	if (!(s.dir = dup_w(dir)) || !(s.src = calloc(n, sizeof *s.src))) {
		st = WF_E_NOMEM;
		goto fail;
	}
	if (WF_OK != (st = ops->node(ops->ctx, dir, &dn))) {
		goto fail;
	}
	s.dir_id = dn.id;

	for (i = 0; i < n; i++) {
		wchar_t *full;

		if (!(s.src[i].name = dup_w(names[i]))) {
			st = WF_E_NOMEM;
			goto fail;
		}
		s.count = i + 1;
		if (WF_OK != (st = wf_path_join(dir, names[i], &full))) {
			goto fail;
		}
		st = ops->node(ops->ctx, full, &s.src[i].snap);
		free(full);
		if (WF_OK != st) {
			goto fail;
		}
	}
	*out = s;
	return WF_OK;
fail:
	wf_slot_free(&s);
	return st;
}

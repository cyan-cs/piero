#include "wf_copyjob.h"

#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"

bool wf_copyjob_allowed(WfCopyKind kind, WfCollide collide)
{
	/*
	 * Enabled on the user's decision (2026-10-31), with the residual risk
	 * recorded in DR-DRAFT-015: the Shell's recycle works on a name, and a
	 * name cannot be bound to the verified source atomically. The engine
	 * narrows the window (the File ID is checked again immediately before,
	 * the result is checked after, and a swap that is seen is reported per
	 * item) but cannot close it.
	 */
	switch (kind) {
	case WF_CJ_COPY:
	case WF_CJ_MOVE_ACROSS:
		return WF_COLLIDE_STOP == collide ||
		        WF_COLLIDE_SKIP == collide ||
		        WF_COLLIDE_OVERWRITE == collide ||
		        WF_COLLIDE_RENAME == collide;
	case WF_CJ_RECYCLE: return WF_COLLIDE_STOP == collide;
	case WF_CJ_EXTRACT:
		return WF_COLLIDE_STOP == collide ||
		        WF_COLLIDE_SKIP == collide ||
		        WF_COLLIDE_OVERWRITE == collide ||
		        WF_COLLIDE_RENAME == collide;
	}
	return false;
}

void wf_copyspec_free(WfCopySpec *s)
{
	if (!s) {
		return;
	}
	wf_slot_free(&s->slot);
	free(s->dest);
	free(s->archive);
	for (size_t i = 0; i < s->n_inner; i++) {
		free(s->inner[i]);
	}
	free(s->inner);
	memset(s, 0, sizeof *s);
}

WfStatus wf_copyspec_clone(WfCopySpec *dst, const WfCopySpec *src)
{
	WfStatus st;

	memset(dst, 0, sizeof *dst);
	wf_slot_init(&dst->slot);
	*dst = *src;
	wf_slot_init(&dst->slot);
	dst->dest = NULL;
	dst->archive = NULL;
	dst->inner = NULL;
	dst->n_inner = 0;
	st = wf_slot_clone(&dst->slot, &src->slot);
	if (WF_OK != st) {
		return st;
	}
	dst->dest = src->dest ? wf_path_dup(src->dest) : NULL;
	dst->archive = src->archive ? wf_path_dup(src->archive) : NULL;
	if (src->n_inner) {
		dst->inner = calloc(src->n_inner, sizeof *dst->inner);
	}
	if ((src->dest && !dst->dest) || (src->archive && !dst->archive) ||
	        (src->n_inner && !dst->inner)) {
		wf_copyspec_free(dst);
		return WF_E_NOMEM;
	}
	for (size_t i = 0; i < src->n_inner; i++) {
		dst->inner[i] = wf_path_dup(src->inner[i]);
		if (!dst->inner[i]) {
			wf_copyspec_free(dst);
			return WF_E_NOMEM;
		}
		dst->n_inner = i + 1;
	}
	return WF_OK;
}

void wf_copyresult_free(WfCopyResult *r)
{
	size_t i;

	if (!r) {
		return;
	}
	for (i = 0; i < r->created_n; i++) {
		free(r->created[i].path);
	}
	free(r->created);
	free(r->item_done);
	for (i = 0; i < r->fail_all_n; i++) {
		free(r->fail_all[i].path);
	}
	free(r->fail_all);
	memset(r, 0, sizeof *r);
}

const char *wf_copyfail_text(WfCopyFailKind k)
{
	switch (k) {
	case WF_CF_SOURCE_CHANGED:
		return "the source changed while it was copied";
	case WF_CF_SOURCE_UNREADABLE:
		return "the source cannot be read (in use or no access)";
	case WF_CF_EXISTS:
		return "the name already exists (a copy enters a folder that "
		       "is "
		       "there; a file and a folder never replace each other)";
	case WF_CF_DEST_CHANGED: return "the file to replace changed meanwhile";
	case WF_CF_DEST_UNSAFE:
		return "replacing it would lose something (read-only, linked "
		       "or in use)";
	case WF_CF_NO_SPACE:      return "not enough free space";
	case WF_CF_WRITE_FAILED:  return "writing failed";
	case WF_CF_VERIFY_FAILED: return "the copy did not read back the same";
	case WF_CF_METADATA_FAILED:
		return "attributes or times could not be preserved; copy not "
		       "committed";
	case WF_CF_COMMIT_FAILED: return "the copy could not be given its name";
	case WF_CF_CLOUD_SKIPPED: return "online-only file (not downloaded)";
	case WF_CF_ENCRYPTED_SKIPPED:
		return "encrypted file (it would be copied unencrypted)";
	case WF_CF_LINK_SKIPPED: return "a link (not followed)";
	case WF_CF_TOO_DEEP:     return "folders nested too deeply";
	case WF_CF_NOT_REMOVED:  return "recycling was not confirmed";
	case WF_CF_STAGING_LEFT:
		return "unfinished work left as a hidden .wfcopy-*.dir or "
		       ".wfextract-*.dir folder";
	case WF_CF_REPLACED_IN_BIN:
		return "another file took its place and went to the Recycle "
		       "Bin";
	case WF_CF_ARCHIVE_NAME:
		return "the name in the archive is not safe to write";
	case WF_CF_ARCHIVE_COLLISION:
		return "same name as another entry once letter case is ignored";
	case WF_CF_ARCHIVE_DATA:
		return "the data in the archive is damaged or not the size it "
		       "says";
	case WF_CF_ARCHIVE_LIMIT: return "over a limit of extraction";
	case WF_CF_ARCHIVE_UNSUPPORTED:
		return "not extracted: a link, a password-protected entry, or "
		       "not a ZIP";
	case WF_CF_ARCHIVE_CHANGED:
		return "the archive changed while it was read";
	}
	return "failed";
}

#include "wf_dupehash_win.h"

#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"
#include "../xfer/wf_sha256_win.h"

WfStatus wf_dupehash_file(const wchar_t *dir,
        const wchar_t *name,
        const WfSearchSeen *seen,
        uint64_t limit,
        bool (*cancel)(void *),
        void *cctx,
        uint8_t digest[32])
{
	wchar_t *path = NULL;
	WfShaResult r;
	WfShaSeen expect;
	WfStatus st;

	if (!seen) {
		return WF_E_INVAL;
	}
	st = wf_path_join(dir, name, &path);
	if (WF_OK != st) {
		return st;
	}
	expect.size = seen->size;
	expect.last_write = seen->mtime;
	expect.id = seen->id;
	memset(&r, 0, sizeof r);
	/* a limit as large as the file is the whole file: the same read either way */
	wf_sha256_file_seen(path, limit < seen->size ? limit : 0, &expect,
	        cancel, cctx, &r);
	free(path);
	if (WF_OK != r.st) {
		return r.st;
	}
	memcpy(digest, r.digest, 32);
	return WF_OK;
}

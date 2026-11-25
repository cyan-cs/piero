#ifndef WF_DUPEHASH_WIN_H
#define WF_DUPEHASH_WIN_H

#include "wf_search.h"

/*
 * The hash of one file for the duplicate search (DR-DRAFT-032): the SHA-256 of its
 * first `limit` bytes (0: all of it) through the same read-only, never-follow-a-link
 * reader as `#`. A file whose identity, size or last write time is not what the
 * listing saw when it is opened, or that changed while it was read, is
 * WF_E_CHANGED: it is not the file that was listed, so it takes no part. Matches
 * WfSearchHashFn.
 */
WfStatus wf_dupehash_file(const wchar_t *dir,
        const wchar_t *name,
        const WfSearchSeen *seen,
        uint64_t limit,
        bool (*cancel)(void *),
        void *cctx,
        uint8_t digest[32]);

#endif

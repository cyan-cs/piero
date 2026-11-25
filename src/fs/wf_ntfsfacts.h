#ifndef WF_NTFSFACTS_H
#define WF_NTFSFACTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What the file system says about one file or folder beyond what the listing
 * holds (DR-DRAFT-026): the link target, the number of names, the alternate data
 * streams, the volume, how the data is stored. Read on demand for the entry under
 * the cursor, from one handle opened with the weakest access. Texts are sanitized
 * UTF-8, cut to fit.
 */
#define WF_NF_TEXT 192
#define WF_NF_STREAMS 6
#define WF_NF_STREAM_NAME 96
#define WF_NF_FS_NAME 32

typedef enum
{
	WF_NF_NOT_ASKED, /* nothing was read: the builder prints no facts */
	WF_NF_REMOTE,    /* not read: a network, WSL, archive or SFTP path */
	WF_NF_FAILED,    /* the handle could not be opened: see `error` */
	WF_NF_OK
} WfNtfsState;

typedef struct
{
	char name[WF_NF_STREAM_NAME]; /* without the leading ':' and ":$DATA" */
	uint64_t size;
} WfNtfsStream;

typedef struct
{
	WfNtfsState state;
	uint32_t error; /* GetLastError() with WF_NF_FAILED */

	uint32_t attributes; /* from the handle, not from the listing */

	bool target_known; /* a symbolic link or junction whose target was read */
	char target[WF_NF_TEXT];

	bool links_known;
	uint32_t links; /* names of the file (1 for a file with one name) */

	bool volume_known;
	char root[WF_NF_TEXT];
	char fs_name[WF_NF_FS_NAME];
	uint32_t serial;

	bool streams_known; /* false: more than the read could hold, or no answer */
	uint32_t n_streams; /* alternate streams (not the main one) */
	WfNtfsStream stream[WF_NF_STREAMS]; /* the first ones */
} WfNtfsFacts;

/* True for a path that must not be opened to read facts: a share (\\server\x), a
 * WSL distribution, a mapped drive. Pure except the drive-type query. */
bool wf_ntfsfacts_remote(const wchar_t *path);

/* Fills `f` for `path` (an absolute path of a real file or folder). Never
 * writes, never reads data, never follows a link. */
void wf_ntfsfacts_read(const wchar_t *path, WfNtfsFacts *f);

#endif

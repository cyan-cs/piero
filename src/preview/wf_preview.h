#ifndef WF_PREVIEW_H
#define WF_PREVIEW_H

#include "../base/wf_buf.h"
#include "../fs/wf_dir.h"

#define WF_PREVIEW_BYTES (256u * 1024u)
#define WF_PREVIEW_HARD_BYTES (1024u * 1024u)
#define WF_PREVIEW_LINE_BYTES 4096u
#define WF_PREVIEW_LINES 4096u

typedef enum
{
	WF_PREVIEW_EMPTY,
	WF_PREVIEW_TEXT,
	WF_PREVIEW_BINARY,
	WF_PREVIEW_INVALID,
	WF_PREVIEW_REFUSED,
	WF_PREVIEW_IMAGE_BLOCKED
} WfPreviewKind;

typedef struct
{
	WfPreviewKind kind;
	WfBuf text; /* sanitized UTF-8, NUL-separated lines */
	size_t lines[WF_PREVIEW_LINES], nlines;
	size_t max_cells; /* widest line in display cells (horizontal scroll) */
	bool truncated;
	uint32_t system_error; /* captured Win32 error; zero for policy/decoding
	                          verdicts */
	char detail[96];
} WfPreview;

/* Takes at most HARD_BYTES; BOM wins, otherwise strict UTF-8. Invalid input
 * is a visible INVALID result rather than silently repaired text. */
WfStatus wf_preview_decode(
        const void *bytes, size_t n, bool truncated, WfPreview **out);
/* The same; with `keep_tabs` a tab stays a tab instead of four spaces (a TSV's
 * delimiter: wf_preview_tableize needs it). */
WfStatus wf_preview_decode_opt(const void *bytes,
        size_t n,
        bool truncated,
        bool keep_tabs,
        WfPreview **out);
WfPreview *wf_preview_notice(WfPreviewKind kind, const char *detail);
void wf_preview_free(WfPreview *p);
/* Preliminary policy: path syntax and entry flags only, no filesystem access.
 * A drive-letter path does not establish that the volume is local or that
 * ancestors are safe; the reader must check those before opening data. */
bool wf_preview_auto_allowed(const wchar_t *path, uint32_t flags);
/* Directory variant: permits WF_ENT_DIR, still rejects links and cloud entries.
 * Explicit preview requests bypass this preliminary check in the caller. */
bool wf_preview_dir_auto_allowed(const wchar_t *path, uint32_t flags);
bool wf_preview_is_image(const wchar_t *name);
/* A PDF by its name; the content is the decoder's to confirm. */
bool wf_preview_is_pdf(const wchar_t *name);

/* Nonblocking, latest-only text preview service. submit copies path before
 * returning; cancel abandons queued/in-flight work without waiting. */
typedef struct
{
	void *ctx;
	WfStatus (*submit)(void *ctx,
	        uint32_t gen,
	        const wchar_t *path,
	        bool explicit_load);
	void (*cancel)(void *ctx);
} WfPreviewPort;

#endif

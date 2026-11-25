#ifndef WF_ESESSION_H
#define WF_ESESSION_H

#include "../git/wf_gitport.h"
#include "../input/wf_keymap.h"
#include "../render/wf_frame.h"
#include "../syntax/wf_synport.h"
#include "../ui/wf_event.h"
#include "wf_editor.h"
#include "wf_efile_win.h"
#include "wf_ekeymap.h"
#include "wf_esyntax.h"

/*
 * The editor as the user meets it: the open buffers, the key handling, the
 * prompts (find, replace, go to line, save as, open, the questions about
 * unsaved text and about files that changed), and the drawing of all of it.
 *
 * It sits between the keys and WfEditor. A key becomes a command through the
 * preset's table; the command is carried out by the one editing model, or by
 * a prompt that asks for what the command needs. Files and the system
 * clipboard are reached only through WfEditIo, so the whole of it can be run
 * against a fake in tests, with the real thing behind the same seam in the
 * product.
 *
 * Session commands run on the UI thread and call WfEditIo synchronously.
 * The system adapter can run file I/O on a worker and pump the busy callback
 * while the caller waits (see wf_edit_io_set_busy).
 */
typedef struct
{
	void *ctx;
	WfStatus (*load)(void *ctx,
	        const wchar_t *path,
	        size_t max_bytes,
	        unsigned fallback_codepage,
	        const WfEncoding *force,
	        WfLoaded *out);
	WfStatus (*save_over)(void *ctx,
	        const wchar_t *path,
	        const WfFileStamp *was,
	        const char *utf8,
	        size_t n,
	        WfEncoding enc,
	        const WfSaveOptions *opt,
	        WfSaveResult *out);
	WfStatus (*save_new)(void *ctx,
	        const wchar_t *path,
	        const char *utf8,
	        size_t n,
	        WfEncoding enc,
	        WfSaveResult *out);
	/* System clipboard as UTF-8; the text is malloc'd for the caller. */
	WfStatus (*clip_get)(void *ctx, char **utf8, size_t *n);
	WfStatus (*clip_set)(void *ctx, const char *utf8, size_t n);
	/* Drafts of unsaved text (optional: NULL means none are kept). The text
	 * is UTF-8 as the editor holds it. draft_allowed says whether the text
	 * of this path may be kept at all. */
	bool (*draft_allowed)(void *ctx, const wchar_t *path, size_t bytes);
	/* The system adapter queues a copy. Startup failure returns an error
	 * without writing on the caller's thread. */
	WfStatus (*draft_write)(void *ctx,
	        const wchar_t *path,
	        WfEncoding enc,
	        const char *utf8,
	        size_t n);
	WfStatus (*draft_read)(void *ctx,
	        const wchar_t *path,
	        WfEncoding *enc,
	        char **utf8,
	        size_t *n);
	void (*draft_remove)(void *ctx, const wchar_t *path);
	/* Optional completion reporting for queued writes. Returns true once for
	 * an unreported failure of path. NULL means draft_write finishes the write
	 * before returning; an asynchronous adapter must supply this callback. */
	bool (*draft_failed)(void *ctx, const wchar_t *path);
} WfEditIo;

/* The real thing: wf_efile_win and the Win32 clipboard. A load or a save runs
 * on a worker thread while the caller waits; the wait calls the busy function
 * (if one is set) about every 50 ms once the job has taken 120 ms, so the
 * screen can show what is going on and the user can ask to stop. It returns
 * false to ask the job to stop (it does so at its next safe point: while
 * reading, or before a save begins to replace the file; after that the file
 * is replaced whatever is asked). */
typedef bool (*WfEditBusy)(void *ctx, bool saving, unsigned elapsed_ms);
void wf_edit_io_set_busy(WfEditBusy fn, void *ctx);
WfEditIo wf_edit_io_system(void);
/* The drafts that wait for the disk are written, and the worker that does it
 * ends (up to `wait_ms`); for the end of the run. Safe when nothing was started. */
void wf_edit_io_drafts_stop(unsigned wait_ms);

typedef struct
{
	WfEditorOptions ed;
	WfKeymapId keymap;
	bool line_numbers;
	bool syntax;
	bool soft_wrap;
	bool show_whitespace;
	bool backup;
	bool drafts; /* keep unsaved text as drafts (see wf_edraft_win.h) */
	unsigned fallback_codepage; /* 0 = none */
	int md_split; /* Markdown preview pane: 0 auto (wide), 1 on, 2 off */
	/* The keys of the chosen profile (borrowed; it must outlive the
	 * session). NULL: the preset in `keymap`, as before there were
	 * profiles. */
	const WfKeymap *keys;
} WfEditConfig;

void wf_es_config_default(WfEditConfig *c);

typedef struct WfEditSession WfEditSession;

/* Git (DR-DRAFT-038): the base of an opened file at HEAD and the diff of the text
 * against it, beside the text. Zeroed: no Git, the editor as it is. */
void wf_es_set_git(WfEditSession *s, WfGitPort port);
/* Takes an answer of the Git worker (the caller frees what is left of it). True
 * if the screen changed. */
bool wf_es_on_git(WfEditSession *s, WfGitAnswer *answer);

/* The syntax worker (DR-DRAFT-033); zeroed: the hand lexer only. */
void wf_es_set_syntax(WfEditSession *s, WfSynPort port);
/* Takes an answer of the editor slot (the caller frees what is left of it). */
void wf_es_on_syntax(WfEditSession *s, WfSynAnswer *a);

WfEditSession *wf_es_new(const WfEditConfig *c, WfEditIo io);
void wf_es_free(WfEditSession *s);

typedef enum
{
	WF_ES_NONE,   /* nothing to redraw */
	WF_ES_REDRAW, /* redraw */
	WF_ES_CLOSED  /* the last buffer was closed: leave the editor */
} WfEsResult;

typedef enum
{
	WF_OPEN_OK,     /* a buffer is open */
	WF_OPEN_ASKING, /* a question is on screen (encoding) */
	WF_OPEN_REFUSED /* `why` says why */
} WfOpenResult;

/* Opens `path` in a buffer (or switches to it if it is already open). */
WfOpenResult wf_es_open(
        WfEditSession *s, const wchar_t *path, char *why, size_t why_cap);

WfEsResult wf_es_event(WfEditSession *s, const WfEvent *ev, int rows, int cols);
/* Monotonic time used for sequence deadlines. */
void wf_es_set_clock(WfEditSession *s, uint64_t now_ms);
/* How often the unsaved text of a buffer is written down. */
#define WF_ES_DRAFT_EVERY_MS 5000u

/* Writes the drafts of the buffers with unsaved changes that are due, and
 * removes those of buffers that no longer have any. wf_es_timeout() says when
 * the next one is due. A no-op when drafts are off. */
void wf_es_save_drafts(WfEditSession *s, uint64_t now_ms);
/* One deadline wake, then infinite wait again; no periodic polling. */
uint32_t wf_es_timeout(const WfEditSession *s, uint64_t now_ms);
bool wf_es_tick(WfEditSession *s, uint64_t now_ms);
void wf_es_draw(WfEditSession *s, WfFrame *f);

size_t wf_es_buffer_count(const WfEditSession *s);
/* Whether anything was written to disk since the last call; the file manager
 * then refreshes its listing and preview. */
bool wf_es_take_saved(WfEditSession *s);
/* Who has the file open: the path whose last save found it in use (malloc'd
 * for the caller; NULL when the last save did not fail that way); whether the
 * person has just chosen to look (once); and the editor's own save once more,
 * which checks everything again (DR-DRAFT-024). */
wchar_t *wf_es_lock_path(const WfEditSession *s);
bool wf_es_take_lock_look(WfEditSession *s);
WfEsResult wf_es_save_again(WfEditSession *s);
const char *wf_es_status(const WfEditSession *s);

/* Where the cursor and the view are in the current buffer, for the saved
 * session (DR-DRAFT-018). `path` is borrowed from the buffer. `modified` is
 * true only when there are unsaved changes AND a draft of them has been
 * written (so a saved "modified" always has a draft behind it). False when
 * there is no buffer, or none with a path. */
typedef struct
{
	const wchar_t *path;
	size_t line, col; /* the cursor: line, byte offset in it */
	size_t top, left; /* the first line and column shown */
	bool modified;
} WfEsPlace;
bool wf_es_place(const WfEditSession *s, WfEsPlace *out);
/* Puts the cursor and the view back, clamped to the text as it is now (a
 * place from a session may be out of range). While a question about a draft
 * is open it waits for the answer. Changes no text. */
void wf_es_goto(
        WfEditSession *s, size_t line, size_t col, size_t top, size_t left);

/* The file of the current buffer as the editor last read or wrote it, for the
 * program to compare with the disk (wf_efile_probably_changed). False when
 * there is none to compare (a new file, an uncertain save). */
bool wf_es_disk_stamp(
        const WfEditSession *s, const wchar_t **path, WfFileStamp *out);
/* Says, once, that another program changed the file under this buffer. A
 * hint for the person: nothing is reloaded, no text is touched, and a save asks
 * about the change based on its own byte comparison. Said again only
 * after a save or a reload. */
void wf_es_note_external_change(WfEditSession *s);

/* Test access. */
WfEditor *wf_es_current(WfEditSession *s);
bool wf_es_prompt_active(const WfEditSession *s);

#endif

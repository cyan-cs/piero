/* Main-loop state shared by main.c and the main_* modules. */
#ifndef WF_MAIN_LOOP_H
#define WF_MAIN_LOOP_H

#include <windows.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/wf_buf.h"
#include "session/wf_session_dedupe.h"
#include "config/wf_cli.h"
#include "config/wf_config_win.h"
#include "diagnostics/wf_diag.h"
#include "fs/wf_candworker.h"
#include "clipboard/wf_clipworker_win.h"
#include "process/wf_lockworker_win.h"
#include "fs/wf_sizeworker.h"
#include "search/wf_fuzzyworker.h"
#include "xfer/wf_hashworker.h"
#include "search/wf_dupehash_win.h"
#include "search/wf_searchworker.h"
#include "syntax/wf_synworker.h"
#include "syntax/wf_synhost_win.h"
#include "fs/wf_enum_win.h"
#ifdef WF_PRODUCT_EDIT
#	include "xfer/wf_batchworker.h"
#endif
#include "session/wf_session_writer.h"
#include "fs/wf_debounce.h"
#include "i18n/wf_i18n.h"
#include "path/wf_smartgo_env_win.h"
#ifdef WF_PRODUCT_EDIT
#	include "fs/wf_editworker.h"
#endif
#include "archive/wf_archive.h"
#include "config/wf_config_save_win.h"
#include "edit/wf_efile_win.h"
#include "edit/wf_esession.h"
#include "fs/wf_fsworker.h"
#include "fs/wf_launchworker.h"
#include "git/wf_gitworker.h"
#include "fs/wf_terminal_win.h"
#include "fs/wf_watch_win.h"
#include "ui/wf_dircache.h"
#include "path/cwd_neutralize.h"
#include "path/wf_path.h"
#include "preview/wf_dechost_win.h"
#include "preview/wf_imageworker.h"
#include "preview/wf_previewworker.h"
#include "recovery/wf_recovery_store.h"
#include "render/wf_present.h"
#include "render/wf_graphics.h"
#include "render/wf_theme.h"
#include "term/wf_term.h"
#include "text/wf_text.h"
#include "ui/wf_app.h"
#include "xfer/wf_copyjournal_win.h"
#include "xfer/wf_copyworker.h"
#include "xfer/wf_probe_win.h"
#include "xfer/wf_xferworker.h"

#include "wf_version.h"
#include "update/wf_update_win.h"
#include "base/wf_mem.h"
#define EVENTS_PER_TICK 64
/* Entries the preview pane lists for a folder; the rest is "partial". */
#define WF_DIRPREV_MAX_ENTRIES 2000u

enum
{
	W_INPUT,
	W_SHUTDOWN,
	W_FS,
	W_XFER,
	W_PREVIEW,
	W_EDIT,
	W_LAUNCH,
	W_DIRPREV,
	W_CAND,
	W_COPY,
	W_IMAGE,
	W_UPDATE,
	W_SIZE,
	W_CLIP,
	W_LOCK,
	W_FUZZY,
	W_BATCH,
	W_HASH,
	W_SEARCH,
	W_SYNTAX,
	W_GIT,
	W_WATCH,
	W_COUNT
};

/* Everything the loop owns, so cleanup is one place. */
typedef struct
{
	WfTerm *term;
	WfFsWorker *fs;
	WfXferWorker *xfer;
	WfPreviewWorker *preview;
	WfImageWorker
	        *image;  /* NULL: not started (or released), or pictures off */
	int image_style; /* WfImageStyle, resolved */
	uint64_t
	        image_needed_ms; /* last turn a picture was on screen or asked */
	uint32_t idle_image_ms,
	        idle_archive_ms; /* idle release (DR-DRAFT-037) */
	HANDLE preview_disabled;
	HANDLE idle; /* never signalled: stands in for absent workers */
	WfLaunchWorker *launch;
	WfFsWorker *dirprev; /* listings for the preview pane, bounded */
	WfCandWorker *cand;  /* Smart Go candidate batches */
	WfSizeWorker *size;  /* folder sizes (D); NULL: not started (main.c) */
	WfFuzzyWorker
	        *fuzzy; /* fuzzy search (s); NULL: searched in the foreground */
	WfHashWorker *hash; /* the SHA-256 of a file (#); NULL: not started */
	WfSearchWorker *
	        search; /* the search below a folder (Tab in /, F); NULL: not started */
	WfSynWorker *
	        syntax; /* colours (Tree-sitter grammars); NULL: not available */
	WfClipWorker *clip; /* the clipboard's files (V); NULL: not started */
	WfGitWorker *
	        git; /* Git (DR-DRAFT-038); NULL: not started, made by the first repository */
	WfLockWorker *lock; /* who has a file open (L); NULL: not started */
	WfEditSession *editor;
	WfCopyWorker *copy; /* NULL: copying is unavailable */
	bool copy_off_set;  /* the copy service did not start: */
	WfMsg copy_off;     /* why, as the message the user is shown */
	WfCopyJournalInfo copy_recovery; /* what the journal said at startup */
	bool term_editing;               /* the terminal is in editing mode */
	bool term_modified; /* ... or reports modified keys in the file manager */
	const WfKeymap *editor_keys; /* the keymap the editor was made with */
	bool launch_enabled;
	const wchar_t *launch_directory; /* neutral lpDirectory; NULL: file's
	                                    own folder */
#ifdef WF_PRODUCT_EDIT
	WfEditWorker *edit;
	WfBatchWorker *batch; /* bulk rename (M); NULL: not available */
#endif
	WfConfig *
	        config; /* the live settings; the settings screen changes them */
	WfUpdateWorker *updates;
	bool update_auto_attempted;
	const wchar_t *config_path; /* where they are saved; NULL: nowhere */
	WfConfigFile cfgfile; /* the file as it was last read or written */
	bool first_run;       /* show the theme screen before the files */
	WfColorDepth depth;   /* what the presenter is set to */
	WfDiagnostics
	        *diagnostics;   /* borrowed; all writes are from this thread */
	DWORD diagnostic_error; /* reported after restoring the terminal */
	WfWatch *watch;
	uint32_t watch_version; /* app.cwd_version the watch was opened for */
	bool watch_listing_dirty, watch_preview_dirty;
	uint64_t rendered_frames, output_bytes, sixel_bytes;
	WfDebounce debounce;
	/* `--continue` and [session] persist (main_session.c); NULL: off */
	WfSessionStore *session_store;
	WfSessionWriter *session_writer; /* NULL: this process does not write */
	uint64_t
	        session_due; /* when the changes gathered are handed over; 0: none */
	WfSessionDedupe
	        session_last; /* what was handed over last, to skip a repeat */
	bool session_told; /* a failed save was said once */
	WfApp app;
	bool app_ready;
	WfFrame *prev; /* what the terminal shows; NULL: repaint every cell */
	WfFrame *
	        spare; /* the frame before it, kept to be drawn over (no allocation) */
	WfBuf out;
	bool graphics_retry;
} Loop;

/* main_editor.c */
void wf_main_start_editor(Loop *lp);
void wf_main_restart_editor(Loop *lp);
void wf_main_sync_editing_keys(Loop *lp);
/* The watch says the edited file changed: the editor says so, once. */
bool wf_main_editor_disk_check(Loop *lp);

/* main_session.c */
void wf_main_session_start(
        Loop *lp, const wchar_t *state_dir, bool continue_session);
/* Something on screen may have changed: write it, soon. */
void wf_main_session_note(Loop *lp);
/* Milliseconds until the next hand-over, WF_WAIT_FOREVER when none waits. */
uint32_t wf_main_session_timeout(const Loop *lp, uint64_t now_ms);
void wf_main_session_tick(Loop *lp);
/* The last state, written; the writer and the store closed. */
void wf_main_session_stop(Loop *lp);
int wf_main_forget_session(void);

/* main_pump.c */
WfStatus wf_main_request_update(void *context, bool manual);
WfStatus wf_main_clip_get(void *ctx, char **utf8, size_t *n);
WfFileTime wf_main_local_time(WfFileTime utc);
void wf_main_diagnose(Loop *lp,
        WfDiagStage stage,
        WfDiagResult result,
        uint64_t operation_id,
        DWORD error);
wchar_t *wf_main_current_directory(void);
WfStatus wf_main_preview_list_submit(
        void *ctx, uint32_t gen, const wchar_t *path, const wchar_t *parent);
int wf_main_run_loop(Loop *lp);
void wf_main_release_idle_workers(Loop *lp);
/* The screen's and the editor's way to ask Git (DR-DRAFT-038): nothing is run, and
 * no worker is made, until a request is. */
WfGitPort wf_main_git_port(Loop *lp);
/* Before the turn's input is handled: what is still on screen counts as use. */
void wf_main_note_modules_in_use(Loop *lp);
/* Milliseconds until a module is released for being idle; WF_WAIT_FOREVER: none. */
uint32_t wf_main_idle_timeout(const Loop *lp, uint64_t now_ms);

/* main_recovery.c */
int wf_main_recovery_command(const WfCli *cli);
void wf_main_refresh_recovery(void *ctx);
/* Both state folders exist and the older one holds unresolved or unreadable
 * records: copying and replacing saves must stay off (DR-DRAFT-017). */
bool wf_main_old_state_blocks(void);

#endif

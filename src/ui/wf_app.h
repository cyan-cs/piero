#ifndef WF_APP_H
#define WF_APP_H

#include <stdbool.h>
#include <stdint.h>
#include "../update/wf_update.h"

#include "../config/wf_config.h"
#include "../clipboard/wf_clipboard.h"
#include "../fs/wf_cand.h"
#include "../fs/wf_size.h"
#include "../session/wf_session.h"
#include "wf_dirsize.h"
#include "wf_batch_ui.h"
#include "wf_fuzzy_ui.h"
#include "wf_hits_ui.h"
#include "../xfer/wf_hash.h"
#include "../syntax/wf_synoverlay.h"
#include "../syntax/wf_synport.h"
#include "../fs/wf_debounce.h" /* WF_WAIT_FOREVER */
#include "../fs/wf_dir.h"
#include "../fs/wf_fsport.h"
#include "../fs/wf_edit.h"
#include "../input/wf_choice.h"
#include "../fs/wf_launch.h"
#include "../i18n/wf_i18n.h"
#include "../input/wf_keymap.h"
#include "../path/wf_smartgo_path.h"
#include "../preview/wf_decode_wire.h"
#include "../preview/wf_pixmap.h"
#include "../preview/wf_preview.h"
#include "../git/wf_gitport.h"
#include "../preview/wf_image.h"
#include "../process/wf_lock.h"
#include "../render/wf_frame.h"
#include "../render/wf_theme.h"
#include "../xfer/wf_copyjob.h"
#include "../xfer/wf_xfer.h"
#include "wf_event.h"
#include "wf_info.h"
#include "wf_layout.h"
#include "wf_sel.h"
#include "wf_view.h"

typedef enum
{
	WF_ACT_NONE,
	WF_ACT_REDRAW,
	WF_ACT_QUIT
} WfAction;

/*
 * Legacy preflight executor seam for embedders and test fakes. The product
 * copy queue uses WfCopyPort; same-volume moves use WfEditPort. An absent
 * executor never authorizes a mutation or bypasses a release gate.
 */
typedef struct
{
	void *ctx;
	WfStatus (*start)(void *ctx,
	        const WfTransferSlot *slot,
	        const wchar_t *dest,
	        const WfPreflightReport *report,
	        bool hydrate_cloud);
} WfExecPort;

typedef enum
{
	WF_IMG_NONE,  /* the entry is not a picture, or pictures are off */
	WF_IMG_BUSY,  /* asked; the answer is pending */
	WF_IMG_READY, /* image.pm is what the pane shows */
	WF_IMG_FAILED /* image.status says why */
} WfImageState;

#define WF_IMAGE_ZOOM_STEPS 5u

/* Lists the directory under the cursor for the preview pane. Same shape as
 * WfFsPort (latest request wins) but the listings are bounded and nothing
 * else about the app changes. Zeroed: directories show metadata only. */
typedef WfFsPort WfDirPreviewPort;

/* What the preview pane shows for a directory. */
typedef enum
{
	WF_DIRPREV_NONE,    /* the pane is not showing a directory listing */
	WF_DIRPREV_WAIT,    /* debounce: the request has not gone out yet */
	WF_DIRPREV_LOADING, /* requested; the answer is pending */
	WF_DIRPREV_READY,   /* dirprev holds the listing */
	WF_DIRPREV_FAILED   /* dirprev_status says why */
} WfDirPreviewState;

/* The pause between the cursor settling on a directory and its listing being
 * requested, so that holding j/k does not enumerate every folder passed. */
#define WF_DIRPREV_DEBOUNCE_MS 60u

/*
 * The built-in editor, as the file manager sees it. The editor itself lives
 * in the edit library; this is the seam. open() takes an absolute path and
 * returns 0 when the editor is showing (a buffer, or a question about the
 * file's encoding) and 1 with `why` when the file cannot be edited. event()
 * returns 0 for nothing to redraw, 1 for redraw, 2 when the editor has no
 * buffer left. A zeroed port means "no editor": `e` says so.
 */
typedef struct
{
	void *ctx;
	int (*open)(void *ctx, const wchar_t *path, char *why, size_t why_cap);
	int (*event)(void *ctx, const WfEvent *ev, int rows, int cols);
	void (*draw)(void *ctx, WfFrame *f);
	bool (*take_saved)(void *ctx);
	/* For the saved session (both optional): where the cursor and the view are
	 * in the current buffer (`path` malloc'd for the caller), and putting them
	 * back, clamped to the text there is now. */
	bool (*place)(void *ctx, WfSessionEdit *out);
	void (*goto_place)(void *ctx, const WfSessionEdit *in);
} WfEditorPort;

/* What `u` can take back. Every entry carries the identities of the objects
 * it will touch; the action is refused if one is no longer that object. */
typedef enum
{
	WF_UNDO_RENAME, /* rename back */
	WF_UNDO_MOVE,   /* move the items back to where they came from */
	WF_UNDO_RECYCLE_CREATED, /* recycle what a copy created */
	WF_UNDO_BATCH /* a bulk rename: every name back, as one batch */
} WfUndoKind;

/* One object an Undo entry puts back. All of it is owned. */
typedef struct
{
	wchar_t *name;     /* name now */
	wchar_t *old_name; /* RENAME, BATCH: name before; else NULL */
	WfFileId id;
	bool is_dir;
} WfUndoItem;

#define WF_UNDO_MAX 16
typedef struct
{
	WfUndoKind kind;
	uint32_t
	        uid; /* names this entry for as long as it lives; never reused */
	uint32_t
	        group; /* a copy that made items in several folders has one entry
		          for each: the uid of the first; 0 when there is one */
	bool group_lost; /* an entry of the group was pushed out: the row never
	                    says "undone" */
	wchar_t *folder; /* where the objects are now */
	WfFileId folder_id;
	wchar_t *origin; /* MOVE: where they came from */
	WfFileId origin_id;
	WfUndoItem *item; /* n of them */
	size_t n;
} WfUndoEntry;

/* The history (J): what was done in this session, newest last, kept after the
 * report window is closed. A row is a job of the copy engine (with its
 * failures) or a change made another way (a rename, a move on one volume, a new
 * file or folder, a bulk rename). Each row says whether Undo can still take it
 * back, and when it cannot, why (DR-DRAFT-029). Nothing of it is written to disk. */
#define WF_JOBLOG_MAX 32

typedef enum
{
	WF_HU_NONE,   /* never could be undone: `why` says why */
	WF_HU_KEPT,   /* an Undo entry (undo_uid) holds what it needs */
	WF_HU_DONE,   /* it was undone */
	WF_HU_EXPIRED /* its Undo entry was pushed out by newer ones */
} WfHistUndo;

typedef struct
{
	char *label; /* what failed and the system code (owned) */
	char *path;  /* the full path, as text (owned) */
} WfJobLogFail;

typedef struct
{
	char head[128];    /* the one-line result */
	uint32_t job;      /* the copy engine's job; 0: not one of its jobs */
	uint32_t undo_uid; /* with WF_HU_KEPT: the Undo entry of this row */
	WfHistUndo undo;
	int why;            /* with WF_HU_NONE: the WfMsg that says why */
	size_t total;       /* failures there were */
	WfJobLogFail *fail; /* the ones kept, with their full paths */
	size_t fail_n;
	/* A copy or move that left items undone: just those sources, and the
	 * folder they were going to, so they can be offered again. */
	WfTransferSlot retry; /* count 0: nothing to offer */
	wchar_t *retry_dest;  /* owned */
	WfCopyKind retry_kind;
} WfJobLogEntry;

#define WF_REPORT_LINES 14
#define WF_PREVIEW_LOADING_MS 180u
#define WF_REPORT_BYTES 80

/*
 * Settings and the first-run theme screen. The app edits a draft of the
 * configuration; saving goes through the port, which writes the changed
 * settings to the file (and says why when it cannot). apply() is told when
 * something the app cannot do itself has to change: `committed` false while
 * only previewing (colour depth), true after a successful save (directory
 * preview, editor options, ...). reload() re-reads the file into *fresh after
 * a conflict. A zeroed port means "settings cannot be saved": the screens
 * still work for this session and say so.
 */
typedef struct
{
	void *ctx;
	WfStatus (*save)(void *ctx,
	        const WfConfigChange *changes,
	        size_t n,
	        char *why,
	        size_t why_cap);
	void (*apply)(void *ctx, const WfConfig *config, bool committed);
	WfStatus (*reload)(void *ctx, WfConfig *fresh);
} WfSettingsPort;

enum
{
	WF_PICKER_LANGUAGE,
	WF_PICKER_KEYS, /* Nano or Vim, between the language and the theme */
	WF_PICKER_THEME
};

typedef enum
{
	WF_SS_BROWSE,  /* the three columns */
	WF_SS_TEXT,    /* typing: a colour, a name, a number, a search, keys */
	WF_SS_PICK,    /* a list of the values of one choice setting */
	WF_SS_CAPTURE, /* waiting for the key a command is to get */
	WF_SS_WARN,    /* a colour with poor contrast: keep it? */
	WF_SS_DELETE,  /* delete a custom theme? */
	WF_SS_RESET_ALL, /* put every key back to the profile's? */
	WF_SS_DISCARD    /* leaving with unsaved changes */
} WfSettingsSub;

enum
{
	WF_FOCUS_SECTION,
	WF_FOCUS_CATEGORY,
	WF_FOCUS_DETAILS
};

enum
{
	WF_TEXT_HEX,    /* one colour of a custom theme */
	WF_TEXT_NAME,   /* the name of a custom theme */
	WF_TEXT_SEARCH, /* filter of the key list */
	WF_TEXT_KEYS,   /* the keys of a command, typed */
	WF_TEXT_NUMBER  /* the value of a number setting */
};

#define WF_LICENSE_FILES 16

/* The system clipboard as UTF-8 (malloc'd for the caller). Zeroed: Ctrl+V is
 * unavailable in Smart Go. */
typedef struct
{
	void *ctx;
	WfStatus (*get)(void *ctx, char **utf8, size_t *n);
} WfClipPort;

/* One line of Smart Go's list. */
typedef struct
{
	wchar_t *name;  /* what is shown and what Tab inserts */
	wchar_t *path;  /* recent folders: the whole path; else NULL */
	uint32_t flags; /* WF_ENT_DIR ... */
} WfGoCand;

typedef enum
{
	WF_GO_NONE,
	WF_GO_FS,   /* entries of a folder */
	WF_GO_ALIAS /* @home, @desktop ... */
} WfGoSource;

/* Maximum candidates retained; visible rows depend on the terminal size. */
#define WF_GO_CAND_MAX 1000u
#define WF_GO_ROWS 24

/* Moves of this many items or more ask for confirmation (spec 12; the
 * threshold is the spec's provisional value). */
#define WF_LARGE_MOVE_ITEMS 100

typedef enum
{
	WF_XB_IDLE,
	WF_XB_CAPTURE,
	WF_XB_PREFLIGHT
} WfXferBusy;

#define WF_CONFIRM_MAX_LINES 8
#define WF_CONFIRM_LINE_BYTES 64
#define WF_CONFIRM_GUARD_MS 300
#define WF_HISTORY_LIMIT 128u
#define WF_RECEIPT_KEEP_MS 4000u

typedef struct
{
	wchar_t *path,
	        *select; /* owned; recorded only after successful navigation */
} WfHistoryEntry;

typedef struct WfApp WfApp;

/* Smart Go request state: initial candidate loading or an additional batch
 * requested after reaching the end of the loaded candidates. */
typedef enum
{
	WF_GR_IDLE,
	WF_GR_FIRST,
	WF_GR_MORE
} WfGoRead;

/* Tabs (wf_app_tabs.c): at most this many. */
#define WF_TABS_MAX 8

/* One tab's record: the folder and the name that was under the cursor (NULL:
 * none). Both are owned by the tab. */
typedef struct
{
	wchar_t *path;
	wchar_t *name;
} WfTab;

#define WF_PLACE_ITEMS 8 /* places listed in the window of `'` and `m` */

/* The SHA-256 of a file asked for with # (wf_app_hash.c): the request that is
 * out, and the digest of the file it was made from while that file is as it was. */
typedef struct
{
	uint32_t gen; /* the newest request; 0: none */
	bool running;
	wchar_t *path; /* owned: the file asked about */
	char name[96]; /* its name as shown */
	uint64_t done, total;
	uint64_t size, last_write; /* of the file the digest is of */
	bool has;
	uint8_t digest[32];
} WfHash;

typedef struct
{
	bool valid;
	int kind;             /* WfEditKind: CREATE_*, RENAME or MOVE */
	wchar_t *dir, *dst;   /* where it was / where it is now */
	wchar_t **from, **to; /* name before / after, per item */
	size_t n;
} WfWslUndo;
typedef struct WfNavigationMemory WfNavigationMemory;

/*
 * A mode owns how the screen looks and what keys mean. Adding a mode is a
 * new WfMode plus one pointer assignment in an existing handler; the event
 * loop and renderer do not change.
 *
 * draw() is called after the base screen is painted and must not retain the
 * frame. on_event() may switch app->mode.
 */
typedef struct WfMode
{
	const char *name;
	void (*draw)(const WfApp *app, WfFrame *f);
	WfAction (*on_event)(WfApp *app, const WfEvent *ev);
} WfMode;

/* Type-ahead protection (spec 5.2): PENDING until the modal has been
 * presented, GUARD for 300 ms after that, then ARMED. Only ARMED accepts
 * Y/N; Esc is honoured in every state. */
typedef enum
{
	WF_CONF_PENDING,
	WF_CONF_GUARD,
	WF_CONF_ARMED
} WfConfirmState;

typedef struct
{
	char lines[WF_CONFIRM_MAX_LINES][WF_CONFIRM_LINE_BYTES];
	int n;
	void (*on_yes)(WfApp *app, void *ctx);
	void *ctx;
	uint32_t yes_ch; /* lowercase ASCII accepted as "go" ('y', 'h', 'c') */
	char armed_text[96]; /* button line once armed */
	WfConfirmState state;
	uint64_t deadline;
	bool ime_hint;
	bool held; /* trigger key still down: ignore its repeats */
	uint32_t held_ch;
	uint16_t held_vk;
	uint32_t alt_ch; /* an optional second answer ("skip") */
	void (*on_alt)(WfApp *app, void *ctx);
	bool pick_yes; /* arrows chose "yes" (only with plain Y/N; the default is no) */
	uint32_t alt2_ch; /* an optional third answer */
	void (*on_alt2)(WfApp *app, void *ctx);
} WfConfirm;

struct WfApp
{
	const WfMode *mode; /* never NULL; points at static storage */
	int cols, rows;
	WfRatios ratios;
	WfFsPort fs;
	wchar_t *cwd;         /* owned; directory being shown */
	uint32_t cwd_version; /* bumped when cwd changes (watchers retarget) */
	/* What the status line says about the list, counted from it when it
	 * changes and not at every frame (wf_app_sync_counts): valid while the view,
	 * the selection and the folder are the ones it was counted for. */
	struct
	{
		bool valid;
		uint32_t view, sel, cwd;
		size_t sel_visible; /* selected names on screen */
		size_t attr_hidden; /* entries the Hidden setting hides */
	} counts;
	uint32_t nav_gen;     /* id of the newest listing request */
	int create_pick;      /* the create menu: 0 file, 1 folder, 2 cancel */
	wchar_t *want_path;   /* owned; target of the in-flight request */
	wchar_t *want_select; /* owned or NULL; cursor name for it */
	wchar_t *want_history_path;
	size_t want_history_index;
	WfHistoryEntry history[WF_HISTORY_LIMIT];
	size_t history_n, history_index;
	WfNavigationMemory *navigation_memory; /* owned, bounded session
	                                          positions; not selection */
	wchar_t *path_input; /* owned growable UTF-16 path prompt */
	size_t path_input_len, path_input_cap, path_input_cursor;
	bool path_input_selected; /* the start of the text is selected: the next
	                             edit replaces it */
	size_t path_input_sel_end; /* where that selection ends (a rename
	                              selects the name, not the extension) */
	WfCandPort cand_port;
	WfSizePort size_port;
	WfClipFilesPort clip_files; /* the Windows clipboard's files (V) */
	uint32_t clip_gen;
	bool clip_busy;           /* a read of the clipboard is out */
	bool paste_after_capture; /* the capture that is out ends in a paste */
	WfDirSizes dirsize;       /* what D has counted for the listing */
	uint32_t dirsize_gen;
	WfFuzzyPort
	        fuzzy_port; /* NULL start: always searched in the foreground */
	WfFuzzy fz;         /* the fuzzy search (s) */
	WfBatch batch;      /* the bulk rename (M) */
	WfHits hits; /* the results of a search below the folder (Tab in /) */
	WfHashPort hash_port; /* the SHA-256 of a file (#) */
	WfHash hash;
	/* Tabs: n 0 is one tab and no record. With more, `cur` is the tab on screen
	 * (its record is only brought up to date when it is left); the others hold
	 * the folder and the name under the cursor they had. */
	struct
	{
		WfTab t[WF_TABS_MAX];
		size_t n, cur;
		/* a walk to tab `to` has been asked for: `cur` becomes `to` when its
		 * folder is on screen, and stays where it is when the walk fails */
		bool going;
		size_t to;
		/* the walk is a close: when the folder of tab `to` is on screen, tab
		 * `close_at` (the one that was on screen) is removed; if it never is,
		 * nothing is removed. `walk_gen` is the listing request the walk is
		 * waiting for (0: not made yet): a newer one ends the walk. */
		bool closing;
		size_t close_at;
		uint32_t walk_gen;
	} tabs;
	/* `piero --continue`: a saved session being put back, until its folder's
	 * listing has arrived (wf_app_session_restore). */
	struct
	{
		bool active;
		bool fell_back; /* the saved folder is gone: a parent is shown */
		wchar_t *dir;   /* owned: the folder being gone to */
		wchar_t *
		        entry; /* owned: the entry the cursor goes on, or NULL */
		WfSessionPane pane; /* who has the keys afterwards */
		size_t cursor, top; /* the row it had and the first row shown */
		size_t pv_top, pv_left;
		bool has_edit; /* the editor is to be opened on `edit` */
		WfSessionEdit edit;
	} session_restore;
	WfClipPort clip;
	WfSmartEnv go_env;
	wchar_t *prev_dir; /* the folder shown before this one (session only) */
	struct
	{
		bool open;
		wchar_t *text; /* owned; always NUL-terminated */
		size_t len, cap, cursor;
		bool all_selected; /* Ctrl+L: typing replaces the whole text */
		WfGoCand *cand;    /* owned */
		size_t cand_n, cand_cap;
		WfGoSource source;
		size_t sel, top;
		bool has_sel;  /* the user chose a line: Enter goes there */
		uint32_t gen;  /* the newest request */
		WfGoRead read; /* which read of the folder is out, if any */
		bool has_more, advance_after_more;
		WfStatus list_status;
		wchar_t *q_dir, *q_prefix; /* the search in force */
		bool q_hidden, dirs_only;
		char note[96];
		bool note_is_error;
		bool committing; /* Enter pressed; waiting for the listing */
		bool fallback_tried;
		uint32_t commit_gen;
		wchar_t *
		        leaf; /* committing: the last element, if it is a file */
	} go;
	WfLaunchPort launch;
	WfLaunchPort
	        terminal; /* Open Terminal Here: the same worker; NULL: off */
	bool launch_terminal;      /* the request in flight is a terminal's */
	uint64_t terminal_ms;      /* when `!` was last pressed (0: never) */
	wchar_t *launch_directory; /* owned: neutral lpDirectory; NULL means the
	                              file's own folder */
	uint32_t launch_gen;
	bool launch_busy;
	struct
	{
		wchar_t *path; /* owned: the file a confirmed open is for */
		WfFileId id;
	} open_pending;
	WfEditPort edit;
	uint32_t edit_gen; /* id of the newest create/rename request */
	bool quit_now;  /* the user confirmed leaving while work was running */
	bool paste_cut; /* a pasted line break ended what a field takes */
	uint32_t undo_inflight; /* uid of the Undo entry being undone, or 0 */
	uint32_t undo_ask_uid; /* uid of the entry the Undo question is about */
	bool keys_stale; /* saved keys that could not be put in force yet */
	uint32_t undo_uid_next;
	/* Recycle jobs that remove the items of an Undo entry: the job and the
	 * entry's uid. Only that job's result touches that entry. */
	struct
	{
		uint32_t job, uid;
	} recycling[32]; /* queued, running and finished-but-unread jobs */
	size_t recycling_n;
	uint32_t last_job; /* id the worker gave the newest submit */
	bool edit_busy;    /* a request is in flight: no second one is sent */
	/* A same-volume move in progress: the slot's items are sent one at a
	 * time through the edit port, each with its own proven identities. */
	struct
	{
		bool active;
		WfTransferSlot slot; /* a copy: Esc may empty the app's slot */
		wchar_t *dest;       /* owned: the folder pasted into */
		WfFileId dest_id;    /* identity the listing proved for it */
		size_t next, moved;
		bool is_undo; /* the items are being put back, not moved */
	} move;
	/* What c / x last picked (folder + names), kept as plain text so a
	 * paste into or out of a WSL distribution does not need file identity,
	 * and the steps of a WSL copy, move or delete in progress. */
	struct
	{
		wchar_t *dir;
		wchar_t **names;
		size_t n;
		bool move;
	} wclip;
	struct
	{
		bool active;
		int kind; /* WfEditKind: WSL_COPY, MOVE or WSL_DELETE */
		wchar_t *src_dir, *dst_dir;
		wchar_t **names;
		wchar_t **names2; /* RENAME: the new names; else NULL */
		bool is_undo;
		bool cancelled;   /* the user asked to stop: after this item */
		size_t warned;    /* items that went through with a warning */
		const char *warn; /* what the last such warning said */
		size_t n, next, done;
		unsigned long long shown; /* progress last drawn */
	} wseq;
	/* The last create, rename or move in a WSL folder, and the one being
	 * made (kept until the worker answers), so `u` can put it back. A copy
	 * and a delete are not undone. */
	WfWslUndo wundo, wundo_pending;
	uint64_t
	        msg_keep_until; /* a fresh result stays on the status line until
		                   now_ms reaches this, even if a reload commits */
	WfInfo info;            /* snapshot shown by the INFO mode */
	WfLocalTime local_time; /* NULL: details are shown in UTC */
	/* Immutable values captured when an operation name form opens. No
	 * borrowed listing pointers survive asynchronous replacement.
	 * cwd_version binds the committed path; nav_gen also invalidates
	 * reloads and pending navigation. */
	struct
	{
		WfFileId parent, source;
		uint32_t nav_gen, cwd_version, source_flags;
		wchar_t source_name[256];
		bool bound, rename;
	} name_form;
	bool confirm_unc_navigation;
	wchar_t *unc_approved_root; /* owned; explicit approval of this share */
	wchar_t *pending_path, *pending_select, *pending_unc_root;
	size_t pending_history_index;
	WfView cur, parent;
	WfSelStore sel;
	WfConfirm confirm;
	WfXferPort xfer;
	WfExecPort exec;
	WfTransferSlot slot; /* y/x contents; mode NONE = empty */
	/* Items picked inside a ZIP by y there: p in a real folder extracts them.
	 * Kept as text (the archive, the paths inside it, the names they take). */
	struct
	{
		wchar_t *archive;
		wchar_t **inner;
		wchar_t **leaf;
		size_t n;
	} arc_pick;
	uint32_t xfer_gen; /* id of the newest capture/preflight request */
	WfXferBusy xfer_busy;
	WfTransferMode capture_mode; /* mode the in-flight capture is for */
	WfPreflightReport last_report;
	char msg[96]; /* last error for the status bar; "" = none */
	struct
	{
		size_t unresolved; /* read-only startup local-state count */
		unsigned causes;   /* WF_RECOVERY_NOTICE_*, observation only */
		unsigned long error; /* last inspection error, never operation
		                        authority */
		bool unknown; /* incomplete/unreadable local-state inspection */
		void *context;
		void (*refresh)(
		        void *context); /* read-only, on explicit Info */
	} recovery;
	uint64_t now_ms; /* set by the event loop before each dispatch */
	uint64_t
	        enter_guard_until; /* Enter does nothing until then (see wf_app_theme.c) */
	bool flush_input; /* loop must drop queued input before presenting */
	WfPreviewPort preview_port;
	uint32_t preview_gen;
	uint32_t preview_flags;
	WfFileId preview_id; /* observation only, never operation authority */
	uint64_t preview_size, preview_mtime;
	uint64_t preview_loading_due; /* one-shot delayed wait indication */
	bool want_preview_refresh; /* explicit refresh on the pending listing */
	struct
	{
		uint64_t text_requests, image_requests, directory_requests;
		uint64_t cleared_snapshots, retained_snapshots;
	} preview_stats; /* diagnostics only; never rendered in the status bar */
	wchar_t *preview_path; /* owned identity of the currently previewed
	                          entry */
	WfPreview *preview;    /* owned, immutable result */
	WfSynPort syn_port;    /* the syntax worker (zeroed: no colouring) */
	uint32_t syn_gen;      /* generation of the newest syntax request */
	uint32_t
	        prev_syn_gen; /* the request the preview's colours wait for; 0: none */
	bool prev_syn_open; /* the worker holds a session for the preview */
	uint32_t prev_syn_status; /* WfSynStatus of the last answer */
	WfSynOverlay prev_ov;     /* the colours of the preview text */
	WfImagePort image_port;
	/* Git (DR-DRAFT-038): the repository whose folder is on screen. Nothing here
	 * exists, and no process or worker is made, until a folder with a `.git`
	 * above it is shown (wf_app_git.c). */
	struct
	{
		WfGitPort port; /* zeroed: Git is off */
		wchar_t *
		        root; /* the folder that holds .git (owned); NULL: none */
		WfGitEnv env;
		uint32_t gen;  /* of the newest request */
		bool resolved; /* `root` holds the .git (else it is the folder shown, to be resolved by the worker) */
		bool deferred; /* the .git is found by the worker (WSL, removable): each folder is checked for a nearer one */
		wchar_t *
		        vcwd; /* the folder the worker found `root` for, or confirmed it for (owned) */
		wchar_t *
		        lcwd; /* the folder a check for a nearer .git is out for (owned) */
		bool stale; /* the list was read again while the answer was awaited */
		bool answered;     /* the answer for `root` has come */
		WfGitState state;  /* what it said */
		uint64_t asked_ms; /* when the newest request was made */
		uint64_t
		        due_ms; /* a reload was held back: ask again then; 0: none */
		WfGitStatus
		        table; /* sorted records; empty unless state is OK */
		char *prefix; /* the shown folder from the root: "src/sub/" or "" (owned) */
	} git;
	uint64_t image_session;
	unsigned image_cell_w,
	        image_cell_h; /* verified Sixel coordinates; zero = text */
	int image_quality;
	unsigned image_colors;
	int image_style; /* WfImageStyle, already resolved: HALFBLOCK or ASCII
	                  * draws pictures, anything else does not */
	struct
	{
		WfImageState state;
		uint32_t gen;
		WfDecodeKind kind;
		WfBuf sixel;
		uint32_t sixel_w, sixel_h;
		unsigned cell_w, cell_h;
		uint32_t bg_rgb;
		int quality;
		unsigned max_colors;
		WfPixmap *pm; /* owned: the page as decoded for `box` */
		WfDecodeInfo info;
		WfDecodeStatus status;
		bool refused, unavailable;
		uint32_t os_error;
		uint32_t page; /* 0-based */
		uint32_t box_w,
		        box_h;  /* what the request or the pixmap was for */
		uint32_t zoom;  /* index into the zoom table; 0 = fit */
		bool fit_width; /* else fit the whole page */
		bool sixel_failed;
		unsigned failed_cw, failed_ch, failed_colors;
		int failed_cols, failed_rows, failed_quality;
		uint32_t failed_bg;
		bool failed_full;
		unsigned pm_cell_w, pm_cell_h;
		int pm_cols, pm_rows;
		uint32_t pan_x, pan_y; /* pixels of the pixmap scrolled off */
		uint32_t focus_x,
		        focus_y; /* 0..65535: the point to keep centred
			            across a zoom or a resize */
		bool refocus;    /* apply focus_* when the answer arrives */
		bool pan_to_end; /* arrive at the bottom (back a page) */
		uint64_t due;    /* when to ask again after a resize */
		int view_cols,
		        view_rows; /* the area the request was made for */
		bool view_full; /* the request was for the full-screen area */
		bool pw_fullscreen; /* the password line was opened from it */
		const WfMode *pw_from; /* where Esc and Enter return to */
		wchar_t password[WF_IMAGE_PASSWORD_MAX + 1];
		wchar_t typing[WF_IMAGE_PASSWORD_MAX + 1];
		size_t typing_n;
	} image;
	size_t preview_top;
	size_t preview_left; /* display columns scrolled off the left */
	/* Where the preview was read when the editor saved and closed: the
	 * reloaded preview of the same file opens there, not at the top. */
	struct
	{
		wchar_t *path; /* owned; NULL: nothing to restore */
		size_t top, left;
	} preview_keep;
	struct WfAppMd *md; /* Markdown layout of the text preview (DR-025) */
	bool preview_busy;
	WfEditorPort editor;
	WfCopyPort copy;
	WfConfig *config; /* the live settings, owned by the program */
	WfSettingsPort settings_port;
	void *update_context;
	WfStatus (*update_request)(void *context, bool manual);
	WfUpdateResult update_result;
	bool update_prompted, update_choice;
	struct
	{
		bool onboarding;  /* the first-run screen, not a chooser */
		size_t index;     /* 0..7 into the presets, row-major 2 x 4 */
		WfThemeId before; /* what to put back on Esc (chooser) */
		int stage;        /* WF_PICKER_*: language first, then theme */
		int lang; /* WfLangSetting chosen on the first-run screen */
		int lang_before;
		int keys_sel; /* 0 Nano, 1 Vim: the row under the cursor */
		int keys; /* chosen: 0 / 1, or -1 to leave the profile as it is */
	} picker;
	WfUiLang lang_detected; /* Windows' UI language, read once */
	struct
	{
		WfSettingsSub sub;
		bool reload_pending;
		WfChoice
		        choice; /* the answers of the unsaved-changes question */
		WfConfig draft; /* start_directory stays NULL: not owned */
		int section;    /* Settings, Licenses, About */
		int focus;      /* the column the cursor is in */
		int category;   /* the entry of the middle column */
		size_t item;    /* the row of the details */
		size_t top;     /* first row (or line of text) shown */
		int pick;       /* WF_SS_PICK: the value under the cursor */
		int text_kind;
		char input[128];
		size_t input_n;
		size_t input_cur; /* byte offset of the cursor in input */
		bool input_fresh; /* typing replaces the suggested HEX value */
		char search[32];  /* filter of the key list */
		int64_t previous; /* the colour a warned change replaced */
		int edit_theme; /* custom theme whose colours are listed, or -1 */
		int rename_index; /* custom theme being renamed from the list, or -1 */
		WfKeymap *kmap; /* the draft's keys, for the key list */
		uint8_t kb_ctx; /* the command being given keys */
		uint16_t kb_cmd;
		bool kb_add;    /* ... as a second alternative */
		char note[112]; /* last result, shown on the bottom line */
		bool note_is_error;
		char *doc; /* text of the licence / about entry shown */
		size_t doc_n;
		char lic_name[WF_LICENSE_FILES][32];
		int lic_n;
	} settings;
	const char *version; /* product version metadata */
	wchar_t *
	        license_dir; /* where LICENSES\*.txt are; NULL: next to the exe */
	WfCopyVerify copy_verify;
	/* When the copy service is not running: why, as a message (0 is a valid
	 * message id, so the flag says whether there is a reason). */
	bool copy_off_set;
	WfMsg copy_off;
	/* The same for Open. */
	bool open_off_set;
	WfMsg open_off;
	size_t copy_jobs; /* jobs submitted and not yet answered */
	bool copy_paused;
	WfFileId
	        cross_first; /* first source of the move across volumes in flight */
	bool pending_cross; /* the slot (mode COPY) stands for a move across
	                       volumes: originals go to the Recycle Bin after */
	WfCollide pending_collide;
	WfTransferSlot recycle_slot; /* what a confirmed Delete will recycle */
	uint64_t copy_refresh_at;    /* ms: next redraw while a job runs */
	char report[WF_REPORT_LINES][WF_REPORT_BYTES];
	int report_n;
	int report_top; /* the first line shown when the report does not fit */
	WfUndoEntry undo[WF_UNDO_MAX];
	size_t undo_n;
	WfUndoEntry undo_pending; /* a rename sent, not yet answered */
	bool undo_pending_set;
	/* The form that was sent (rename, new file, new folder), for its row in
	 * the history when the answer comes. */
	struct
	{
		bool set, rename, dir;
		char old_name[64]; /* rename: the name before, as shown */
	} form_note;
	bool edit_is_undo; /* the edit in flight is an undo */
	/* A directory under the cursor, listed for the preview pane. The view is
	 * the pane's own; nothing here touches history, navigation memory,
	 * selection or the cursor of the current pane. */
	struct WfDirCache
	        *dircache; /* owned; optional, memory-only preview snapshots */
	WfDirPreviewPort dirprev_port;
	WfDirPreviewPort
	        dirprev_full; /* the real port while it is switched off */
	bool dirprev_off;
	WfView dirprev;
	WfDirPreviewState dirprev_state;
	WfStatus dirprev_status; /* FAILED: why */
	uint32_t dirprev_gen;
	uint64_t dirprev_due; /* WAIT: when the request goes out (ms) */
	/* Keys: what the keyboard means. keymap is never NULL once the app is
	 * initialised; `keys` holds the part of a sequence or count typed so far
	 * in context `keys_ctx`; keys_issue says why the configured keys were
	 * replaced by the profile's own (kind NONE: they were not). */
	WfKeymap *keymap; /* owned */
	WfKeymap *
	        keymap_prev; /* the one before: a borrower (the editor) that has
		                not been told yet never holds freed memory */
	WfKeyState keys;
	WfKeyContext keys_ctx;
	WfKeyIssue keys_issue;
	/* The command line (Vim's ':'). */
	struct
	{
		char text[256];
		size_t n, cur;
		char hist[16][256];
		char draft
		        [256]; /* the line being typed, kept while history is browsed */
		size_t hist_n, hist_at;
	} cmdline;
	struct
	{
		size_t top; /* the first line of the help shown */
		int cat; /* the category shown (file manager, preview, editor) */
		size_t sel; /* the action the explanation below is about */
		char q[48]; /* what the help is narrowed to (a part of a key or an
		               action) */
		const WfMode
		        *back; /* the screen the help was opened from and is
			          closed back to; NULL: the list */
	} help;
	int sort_pick; /* the sort menu: the row the arrows are on */
	int info_cols; /* the width the details window was last laid out for */
	const WfMode *
	        edit_from; /* where the editor returns to (the preview, or the lists) */
	/* Where Settings was left, for the next time it is opened this session. */
	struct
	{
		bool valid;
		int section, category, focus;
		size_t item;
		size_t top;
	} settings_mem;
	/* What each submitted copy or move was asked to do, until its result
	 * says whether something has to be offered again. */
	struct
	{
		uint32_t job;
		WfCopyKind kind;
		WfTransferSlot slot;
		wchar_t *dest;
	} retry_src[8];
	size_t retry_src_n;
	/* Who has a file open (L, DR-DRAFT-024): the failed operation with the
	 * identities it had, the answer of the lock worker, and what is asked. */
	struct
	{
		WfLockPort port;
		/* The editor's side (both optional, with the editor port's ctx): the
		 * path whose last save failed because another program had it open
		 * (malloc'd for the caller; NULL when it did not fail that way), and a
		 * new try of the editor's own save (it checks everything again), whose
		 * return is the editor port's event(). */
		wchar_t *(*editor_path)(void *ctx);
		int (*editor_save)(void *ctx);
		uint32_t gen;  /* the newest request */
		int busy;      /* a WfLockReq + 1 is out; 0: none */
		int op;        /* WfLockOp: what failed; 0: asked by hand */
		wchar_t *path; /* the file in use (owned) */
		wchar_t *dir;  /* the folder a rename worked in (owned) */
		WfTransferSlot
		        slot;  /* failed sources, as first captured (owned) */
		wchar_t *dest; /* where a copy or move was going (owned) */
		wchar_t *old_name, *new_name; /* a rename (owned) */
		WfFileId source_id; /* a rename: the item that failed */
		WfLockList list;
		bool searching, shown;
		WfStatus qst; /* why the query could not look (WF_OK: it did) */
		unsigned long qerr;
		size_t pick;
		const WfMode *back; /* the mode the window returns to */
		struct
		{
			uint32_t pid;
			uint64_t start;
			WfLockAppType type;
			char name[64];
		} act;          /* the program a question is about */
		char note[160]; /* the last action's outcome, in the window */
		struct
		{
			uint32_t job;
			WfCopyKind kind;
			WfTransferSlot slot;
			wchar_t *dest;
		} jobs[8]; /* what each running job was told, for a retry */
		size_t jobs_n;
	} lock;
	struct
	{
		WfJobLogEntry log[WF_JOBLOG_MAX]; /* oldest first */
		size_t n;
		size_t top; /* the first line shown in the job log */
		/* The lines each entry takes at `lines_cols` columns, counted when a
		 * key is read (and not at every frame): valid until the log changes. */
		bool lines_ok;
		int lines_cols;
		size_t lines_of[WF_JOBLOG_MAX], lines_total;
	} jobs;
	struct
	{
		wchar_t *base; /* the file the backups are of (owned) */
		wchar_t *names
		        [9]; /* the backups found, newest name first (owned) */
		size_t at
		        [9]; /* where each was in the list: a hint, checked by name */
		size_t n, pick;
	} backups;
	WfSortKey sort_key; /* the order in force when there is no config */
	bool sort_desc;
};

extern const WfMode WF_MODE_NORMAL;
extern const WfMode WF_MODE_PREVIEW_PANE;
extern const WfMode WF_MODE_HELP;
extern const WfMode WF_MODE_INFO;
extern const WfMode WF_MODE_FILTER;
extern const WfMode WF_MODE_CONFIRM;
extern const WfMode WF_MODE_PREVIEW;
extern const WfMode WF_MODE_REPORT;
extern const WfMode WF_MODE_JOBLOG;
extern const WfMode WF_MODE_HITS;
extern const WfMode WF_MODE_PLACE_SET;
extern const WfMode WF_MODE_PLACE_GO;
extern const WfMode WF_MODE_BACKUPS;
extern const WfMode WF_MODE_SETUP;
extern const WfMode WF_MODE_GO;
extern const WfMode WF_MODE_SETTINGS;
extern const WfMode WF_MODE_EDITOR;
extern const WfMode WF_MODE_CMDLINE;
extern const WfMode WF_MODE_SORT;
extern const WfMode WF_MODE_LOCK;
extern const WfMode WF_MODE_FUZZY;
extern const WfMode WF_MODE_BATCH;

/* The styles of the active theme (render/wf_theme.h); the names stay for the
 * many places that draw with them. */
#define WF_STYLE_BASE (wf_theme_styles()->base)

/* Starts loading `start_path` (asynchronously). Fails only if the request
 * cannot be submitted; a bad path shows up later as a status-bar error. On
 * failure nothing is allocated and the app must not be destroyed. */
WfStatus wf_app_init(
        WfApp *app, int cols, int rows, WfFsPort fs, const wchar_t *start_path);
void wf_app_destroy(WfApp *app);

/* Delivers a listing. Takes ownership of `dir` and `parent` in every case,
 * including stale generations. `parent` may be NULL. */
void wf_app_on_listing(
        WfApp *app, uint32_t gen, WfStatus st, WfDir *dir, WfDir *parent);

/* Re-reads the current directory, keeping the cursor on its entry. */
void wf_app_reload(WfApp *app);
/* Notification refresh is independent of explicit user reload. */
void wf_app_refresh(WfApp *app, bool refresh_preview);
void wf_app_preview_sync(WfApp *app, bool explicit_load);
void wf_app_dircache_forget_neighbours(WfApp *app);

/* Call after every successful present, with app->now_ms current. */
void wf_app_on_presented(WfApp *app);

/* Time-driven transitions. Timeout is ms until the next one (or
 * WF_WAIT_FOREVER); tick() applies it and reports whether to redraw. */
uint32_t wf_app_timeout(const WfApp *app);
WfAction wf_app_tick(WfApp *app);
/* False: this event is pasted text the current mode must not take (a
 * question, a list command, the lines after a pasted line break). */
bool wf_app_paste_filter(WfApp *app, const WfEvent *ev);
/* The wheel in a window that scrolls: handled here as presses of Up/Down;
 * true when the event was such a wheel (*act: whether to redraw). */
bool wf_app_window_wheel(WfApp *app, const WfEvent *ev, WfAction *act);

/* Wiring for the optional services. */
void wf_app_set_xfer(WfApp *app, WfXferPort port);
void wf_app_set_exec(WfApp *app, WfExecPort port);
void wf_app_set_preview(WfApp *app, WfPreviewPort port);
/* The picture service and how pictures are drawn (WfImageStyle; DEFAULT must
 * have been resolved by the caller). A zeroed port or anything but HALFBLOCK
 * and ASCII means pictures are not previewed. */
void wf_app_set_image(WfApp *app, WfImagePort port, int style);

/* True when a folder inside an archive is on screen, in this tab or in another:
 * the archive's index is then in use, and idle release leaves it (DR-DRAFT-037). */
bool wf_app_archive_in_view(const WfApp *app);

/* Git (wf_app_git.c). */
void wf_app_set_git(WfApp *app, WfGitPort port);
/* The folder on screen has just been listed (`reload`: the same folder again).
 * Decides whether the repository is the same, new or gone, and asks for its
 * status when that is needed. Nothing is run here. */
void wf_app_git_note_listing(WfApp *app, bool reload);
/* An answer of the worker for the repository (takes the table). True if what the
 * list shows changed. Answers that are not for the newest request are dropped. */
bool wf_app_on_git(WfApp *app, WfGitAnswer *answer);
/* The one letter for a row of the shown folder (M U A D R !); 0 for none. */
char wf_app_git_letter(const WfApp *app, const WfEntry *e);
/* True for a folder with a changed file somewhere below it (Git's answer for
 * the folder on screen, as for the letter). */
bool wf_app_git_dir_changed(const WfApp *app, const WfEntry *e);
/* What Git said about the file `rel` (UTF-8, '/') of the repository whose `.git`
 * is in `root`, if that is the repository on screen and its status is known:
 * the letter (0 if the file is not in the table) and the path it was renamed
 * from (NUL when not). False when there is nothing known for that repository. */
bool wf_app_git_file(const WfApp *app,
        const wchar_t *root,
        const char *rel,
        char *letter,
        char *orig,
        size_t orig_cap);
/* What the screen knows for the base of the file at `path` (absolute): true only
 * when the repository on screen is the file's AND the folder has been checked for
 * a nearer .git (a nested repository or a submodule is then not mistaken for the
 * parent). Then `root` (owned by the app), `rel` (from the root), whether Git
 * calls the file new, and the path it was renamed from (NUL when not). False:
 * nothing is known; the caller lets the worker find the repository. */
bool wf_app_git_base_hint(const WfApp *app,
        const WfGitEnv *env,
        const wchar_t *path,
        const wchar_t **root,
        char *rel,
        size_t rel_cap,
        bool *expect_new,
        char *orig,
        size_t orig_cap);
/* Delivers the service's answer; takes answer->pm. */
void wf_app_on_image(WfApp *app, const WfImageAnswer *answer);
void wf_app_set_editor(WfApp *app, WfEditorPort port);
void wf_app_set_copy(WfApp *app, WfCopyPort port, WfCopyVerify verify);
/* Why there is no copy service: shown instead of a generic refusal. */
void wf_app_set_copy_off(WfApp *app, WfMsg why);
/* Why Open is not available: shown when Enter is pressed on a file. */
void wf_app_set_open_off(WfApp *app, WfMsg why);
/* `config` is the program's live configuration; the app reads it, and writes
 * it only after a successful save. */
void wf_app_set_settings(WfApp *app, WfConfig *config, WfSettingsPort port);
/* Shows the theme screen: the first-run one (Esc keeps the default and still
 * records that setup was done) or, with onboarding false, the chooser the
 * settings screen opens. */
void wf_app_setup_begin(WfApp *app, bool onboarding);
/* What Windows asks for, so that "Auto" can say which language it means. */
void wf_app_set_detected_language(WfApp *app, WfUiLang lang);
/* Product version metadata and (for tests) the folder the third-party licence
 * files are read from; both are copied. */
void wf_app_set_version(WfApp *app, const char *version);
void wf_app_update_consent_begin(WfApp *app);
void wf_app_update_complete(WfApp *app, const WfUpdateResult *result);
void wf_app_set_license_dir(WfApp *app, const wchar_t *dir);

/* `,`: the settings screen. */
void wf_app_settings_begin(WfApp *app);
/* The keys of the profile in `cfg` (Custom included) become the app's. If
 * they cannot be used (two commands on one key...) the profile's own keys
 * are used and keys_issue says what was wrong. `editor_legacy_keymap` is the
 * editor preset (WfKeymapId) used when no profile was chosen. */
WfStatus wf_app_set_keys(
        WfApp *app, const WfKeyConfig *cfg, int editor_legacy_keymap);
/* The keymap in force (borrowed; valid until the next wf_app_set_keys()
 * after the next one), and a one-line warning when the configured keys could
 * not be used. */
const WfKeymap *wf_app_keymap(const WfApp *app);
void wf_app_keys_warn(WfApp *app);
/* The text of the keys of `cmd` in the file manager (for hints). */
size_t wf_app_key_text(const WfApp *app,
        WfKeyContext ctx,
        uint16_t cmd,
        char *out,
        size_t cap);
/* Delivers a finished job. The app copies what it needs (for undo); the caller
 * still frees the result with wf_copyresult_free. */
void wf_app_on_copy_result(WfApp *app, WfCopyResult *r);
/* True while the editor owns the screen and keys: the terminal then reports
 * Ctrl/Alt chords and modified cursor keys. */
bool wf_app_editing(const WfApp *app);
void wf_app_set_dirpreview(WfApp *app, WfDirPreviewPort port);
/* The right-column directory listing on or off (a setting). */
void wf_app_dirpreview_enable(WfApp *app, bool on);
/* Takes ownership of `dir` even for stale generations. */
void wf_app_on_dirpreview(WfApp *app, uint32_t gen, WfStatus st, WfDir *dir);
void wf_app_set_candidates(WfApp *app, WfCandPort port);
void wf_app_set_sizes(WfApp *app, WfSizePort port);
void wf_app_set_fuzzy(WfApp *app, WfFuzzyPort port);
void wf_app_set_hash(WfApp *app, WfHashPort port);
void wf_app_set_search(WfApp *app, WfSearchPort port);
/* The syntax worker (DR-DRAFT-033). */
void wf_app_set_syntax(WfApp *app, WfSynPort port);
/* Takes an answer of the preview slot (and frees what it does not keep); false
 * when it is not for the preview (the editor's), nothing was done with it. */
bool wf_app_on_syntax(WfApp *app, WfSynAnswer *a);
/* Takes a batch of hits of the search below a folder (and frees what it does not
 * keep). One that is not the newest request's is dropped. */
void wf_app_on_search(WfApp *app, WfSearchBatch *batch);
/* Takes progress or the end of the newest hash request; others are dropped. */
void wf_app_on_hash(WfApp *app, const WfHashUpdate *update);
/* Takes the answer of a fuzzy search (and frees it). One that is not the newest
 * request's is refused. */
void wf_app_on_fuzzy(WfApp *app, WfFzAnswer *answer);
/* Asks a fuzzy search again after the listing changed under it (the loop calls
 * it every turn); true when the screen changed. */
bool wf_app_fuzzy_tick(WfApp *app);
/* The bulk rename (M, DR-DRAFT-022). A port with no functions means "not
 * available". The answer of the worker is taken here; one that is not the
 * request in flight is dropped. */
void wf_app_set_batch(WfApp *app, WfBatchPort port);
void wf_app_on_batch(WfApp *app, const WfBxDone *done);
/* Counts the journals an earlier run left; the question about them is asked
 * when the screen is free. Never recovers by itself. */
void wf_app_batch_startup(WfApp *app);
/* True while a batch (or a recovery) runs on the worker. */
bool wf_app_batch_busy(const WfApp *app);
void wf_app_set_clip_files(WfApp *app, WfClipFilesPort port);
/* Takes the answer of a clipboard read (and frees it). A stale one is dropped. */
void wf_app_on_clip_files(WfApp *app, WfClipFiles *files);
/* Who has a file open (L, DR-DRAFT-024). The port may be zeroed ("not
 * available"). An answer is taken (and freed) here; a stale one is dropped. */
void wf_app_set_lock(WfApp *app, WfLockPort port);
void wf_app_set_editor_lock(
        WfApp *app, wchar_t *(*path)(void *ctx), int (*save)(void *ctx));
void wf_app_on_lock(WfApp *app, WfLockAnswer *answer);
/* What `--continue` saves: where the person is, as names (DR-DRAFT-018).
 * False when there is nothing to save now (a listing is being replaced, an
 * archive or an ssh place that a session does not hold). */
bool wf_app_session_capture(const WfApp *app, WfSession *out);
/* A line for the status bar that stays long enough to be read, past the first
 * listing (the program's startup notices). */
void wf_app_notice(WfApp *app, const char *text);
/* Puts a saved session back: the folder through the ordinary navigation (its
 * cursor and scroll read by the listing when it arrives, its selection through the
 * selection, its preview through the preview's own restore). Nothing is
 * opened for writing. A listing that fails goes to the nearest parent. */
WfStatus wf_app_session_restore(WfApp *app, const WfSession *session);
/* Takes one update of the folder count. Stale generations are ignored. */
void wf_app_on_sizes(WfApp *app, const WfSizeUpdate *update);
void wf_app_set_clipboard(WfApp *app, WfClipPort port);
void wf_app_set_smartgo_env(WfApp *app, WfSmartEnv env);
/* Delivers up to WF_CAND_BATCH candidates. Stale generations are ignored. */
void wf_app_on_candidates(WfApp *app, const WfCandBatch *batch);
void wf_app_set_edit(WfApp *app, WfEditPort port);
/* Enables Enter on files. `neutral_directory` is the lpDirectory given to the
 * shell ([launch] working_directory = neutral); NULL selects the file's own
 * folder (file_dir). Copies the string. Without this call Enter on a file
 * stays blocked. */
WfStatus wf_app_set_launch(
        WfApp *app, WfLaunchPort port, const wchar_t *neutral_directory);
/* Enables Open Terminal Here (the `!` key). Without it the key says so. */
void wf_app_set_terminal(WfApp *app, WfLaunchPort port);
/* Delivers the answer to a launch request. Stale generations are ignored. */
void wf_app_on_launch(WfApp *app,
        uint32_t gen,
        const WfLaunchOutcome *outcome,
        const wchar_t *name);
/* Delivers the receipt of a create/rename request. `new_name` is the name
 * that was requested. Stale generations are ignored. */
void wf_app_on_edit(WfApp *app,
        uint32_t gen,
        const WfEditOutcome *outcome,
        const wchar_t *new_name);
void wf_app_set_local_time(WfApp *app, WfLocalTime local);
/* Takes ownership even for stale generations. */
void wf_app_on_preview(WfApp *app, uint32_t gen, WfPreview *preview);

/* Answers to WfXferPort requests. Stale generations are dropped. */
void wf_app_on_capture(WfApp *app,
        uint32_t gen,
        WfStatus st,
        WfTransferSlot *slot); /* takes the contents */
void wf_app_on_preflight(
        WfApp *app, uint32_t gen, WfStatus st, const WfPreflightReport *report);

/*
 * Opens a Y/N modal. Texts are sanitized and truncated. `trigger` is the
 * event that opened it (may be NULL); if it is a key still held down, its
 * auto-repeats are ignored until it is released. Sets flush_input.
 */
WfStatus wf_app_confirm_begin(WfApp *app,
        const char *const *lines,
        int n,
        void (*on_yes)(WfApp *, void *),
        void *ctx,
        const WfEvent *trigger);

/* Same, but the "go" key is `yes_ch` (lowercase ASCII, either case accepted)
 * and the button line shown once armed is `armed_text`. N/Esc/Enter still
 * cancel. Used for [H] Hydrate & Copy and [C] Copy instead. */
WfStatus wf_app_confirm_begin_ex(WfApp *app,
        const char *const *lines,
        int n,
        uint32_t yes_ch,
        const char *armed_text,
        void (*on_yes)(WfApp *, void *),
        void *ctx,
        const WfEvent *trigger);

/* Two ways forward: `yes_ch` runs on_yes, `alt_ch` runs on_alt (both lowercase
 * ASCII, either case accepted); N/Esc/Enter cancel. */
WfStatus wf_app_confirm_begin_alt(WfApp *app,
        const char *const *lines,
        int n,
        uint32_t yes_ch,
        uint32_t alt_ch,
        const char *armed_text,
        void (*on_yes)(WfApp *, void *),
        void (*on_alt)(WfApp *, void *),
        void *ctx);

/* Three ways forward: yes_ch, alt_ch and alt2_ch (all different, lowercase
 * ASCII, not 'n'); N/Esc/Enter cancel. */
WfStatus wf_app_confirm_begin_alt2(WfApp *app,
        const char *const *lines,
        int n,
        uint32_t yes_ch,
        uint32_t alt_ch,
        uint32_t alt2_ch,
        const char *armed_text,
        void (*on_yes)(WfApp *, void *),
        void (*on_alt)(WfApp *, void *),
        void (*on_alt2)(WfApp *, void *),
        void *ctx);

/* Paints the whole screen for the current mode into `f` (f->cols x f->rows). */
/* Counts what the status line shows once for the list on screen, so that a frame
 * does not walk it (wf_app_render.c). The status line is right without this call,
 * only slower. */
void wf_app_sync_counts(WfApp *app);
/* A toggle of one entry that is on screen (the cursor entry) changes the count of
 * the selected names on screen by one: when the counts were of the selection
 * before it (`had`: wf_app_counts_current, asked before the toggle), they are
 * made the counts of the selection after it without a walk of the list. */
bool wf_app_counts_current(const WfApp *app);
void wf_app_counts_toggled(WfApp *app, bool had, bool was_selected);

void wf_app_draw(const WfApp *app, WfFrame *f);

/* Centered alert with a 2-cell halo. Returns 0, drawing nothing, if the box
 * does not fit the frame; modal callers must then provide a fallback. */
bool wf_ui_alert(WfFrame *f, const char *const *lines, int n);
/* The box when it fits; on a small screen the same lines as plain text, the
 * first ones and the last (the one that says how to close) when they do not
 * all fit, so a window is never just blank. */
void wf_ui_alert_or_text(WfFrame *f, const char *const *lines, int n);

#endif

#ifndef WF_ESESSION_INTERNAL_H
#define WF_ESESSION_INTERNAL_H

#include "wf_egitpane.h"
#include "wf_emdpane.h"
#include "wf_esession.h"
#include "wf_evim.h"
#include "../input/wf_choice.h"

/* Shared by wf_esession.c (behaviour) and wf_eview.c (drawing). */

#define WF_ES_INPUT_MAX 512
#define WF_ES_HISTORY 8

/*
 * The soft wrap of the lines of one text: the rows of a line and where each
 * starts. wf_et_wrap allocates and walks the line, so the answer is kept for as
 * long as nothing it depends on changes: the text (its `gen`), the width the
 * rows are made for and the tab width. Moving the cursor or scrolling asks for
 * the same rows again and gets them. When the cache is full the entry that was
 * made first is replaced (`next`), so the lines being looked at are the ones kept:
 * callers are done with a line's rows before they ask for the next.
 */
#define WRAP_CACHE_LINES 128

typedef struct
{
	size_t line, rows;
	size_t *starts; /* owned by the cache; NULL with rows 1 when the line fits */
} WrapEntry;

typedef struct
{
	WrapEntry e[WRAP_CACHE_LINES];
	size_t n;
	size_t next; /* the entry to replace when all are in use */
	bool keyed;  /* gen, width and tab say what the entries were made for */
	uint64_t gen;
	size_t width;
	unsigned tab;
} WrapCache;

typedef struct
{
	WfEditor ed;
	WfSyntax syn;
	WfLang lang;
	wchar_t *path; /* absolute; the file this buffer is, or will become */
	bool on_disk;  /* `stamp` describes the file as last read or written */
	bool save_uncertain; /* committed bytes, invalid baseline: reload or Save As */
	WfFileStamp stamp;
	size_t top;      /* first line shown */
	size_t top_wrap; /* wrapped display rows skipped within top */
	size_t left;     /* cells scrolled off the left (no wrapping) */
	WrapCache wrap;  /* owned: wf_es_wrap_free */
	uint64_t eol_gen;
	char eol_label[16];
	bool has_draft;     /* this session wrote a draft for it */
	uint64_t draft_gen; /* the text gen that draft holds (with has_draft) */
	bool external_noted; /* "changed by another program" was said since the
	                        last save or reload; said once, not at every event */
	uint64_t draft_ms;   /* when it was last written */
	WfMdPane md;         /* the Markdown preview pane's layout (DR-025) */
	WfGitPane git;       /* what the buffer changes against HEAD (DR-038) */
	/* Where the matches of the last search are, for "match 3 of 120": made once
	 * for a text (its `gen`) and a pattern, so that moving from match to match
	 * does not read the whole text each time. */
	struct
	{
		WfPos *at; /* WF_ES_COUNT_CAP of them once made */
		size_t n;
		uint64_t gen;
		char *pat;
		size_t pat_n;
		bool match_case, whole_word, valid;
	} found;
} WfBuffer;

typedef enum
{
	PR_NONE,
	PR_FIND,
	PR_FIND_BACK,
	PR_REPL_FIND,
	PR_REPL_WITH,
	PR_REPL_ASK,
	PR_GOTO,
	PR_SAVE_AS,
	PR_OPEN,
	PR_INSERT,
	PR_QUIT,
	PR_CONFLICT,
	PR_GONE,
	PR_RECOVER, /* a save stopped half way: which of the files to open */
	PR_LOCKED, /* a save failed because another program has the file open */
	PR_DRAFT,  /* a draft of unsaved text exists for the file just opened */
	PR_ENCODING,
	PR_EX, /* the ':' command line of the Vim keys */
	PR_HELP
} PromptKind;

struct WfEditSession
{
	bool paste_cut; /* a pasted line break ended the line a prompt takes */
	int md_force; /* the preview pane asked for with the key: 1 on, -1 off,
	                 0 as configured */
	WfEditConfig cfg;
	WfEditIo io;
	WfBuffer **buf;
	size_t n, cap, cur;
	wchar_t *
	        base_dir; /* where a bare file name goes when the buffer has none */

	PromptKind pk;
	char label[96];
	WfChoice
	        choice; /* the answers of a PR_QUIT/CONFLICT/GONE/ENCODING question */
	char in[WF_ES_INPUT_MAX];
	size_t in_n, in_cur;
	bool in_fresh; /* the text is a suggestion: typing replaces it */

	char find[WF_ED_FIND_MAX];
	size_t find_n;
	bool f_case, f_word;
	bool hl; /* matches of the last search are marked on screen */
	char repl[WF_ED_FIND_MAX];
	size_t repl_n, repl_count;
	char hist[WF_ES_HISTORY][WF_ED_FIND_MAX];
	size_t hist_n;
	size_t hist_pos;

	wchar_t *
	        pending_path; /* an ambiguous file waiting for the encoding answer */
	WfEncoding pending_enc;
	/* A save that stopped half way left the new text in one file and (if the
	 * replacement had begun) the previous version in another; the question
	 * PR_RECOVER offers to open them. Either may be NULL. */
	wchar_t *recover_new, *recover_old;
	/* The draft found when a file was opened, waiting for the answer. */
	char *draft_text;
	size_t draft_n;
	/* a place asked for (wf_es_goto) while the question about a draft is open:
	 * the answer replaces the text and moves the cursor, so it is applied after */
	bool goto_pending;
	size_t goto_line, goto_col, goto_top, goto_left;
	WfEncoding draft_enc;
	bool exit_after_save; /* the quit question chose Save */
	wchar_t *lock_path;   /* the file the last save found in use (owned) */
	bool lock_look;       /* the person chose to see who has it */

	WfGitPort
	        git_port; /* the base and the diff against HEAD; zeroed: none */
	uint32_t git_gen; /* of the newest Git request */
	uint64_t git_next_id; /* the next buffer's id for the answers */
	uint64_t
	        git_base_owner; /* the buffer the newest base request was for */
	const WfBuffer *git_last; /* the buffer the pane was last drawn for */
	WfSynPort syn_port;       /* Tree-sitter colours; zeroed: none */
	uint32_t syn_gen;         /* the newest request */
	uint32_t syn_req_gen; /* the request the colours wait for; 0: none */
	WfBuffer *syn_buf;    /* the buffer that request was for */

	char msg[160];
	bool msg_error;
	bool saved_since;
	bool clip_local_only; /* the system refused our last copy or cut */
	struct
	{
		char *text;
		size_t n;
		bool chain;
		bool action;
		const WfBuffer *buffer;
	} nano_cut;
	int rows, cols;
	size_t help_top;

	/* keys of a profile */
	WfKeyState ks;
	WfKeyContext ks_ctx;
	bool vim_insert;    /* modal profile: typing, not commands */
	WfVim vim;          /* operators, Visual mode, registers, macros */
	unsigned vim_count; /* the count typed before the command being run */
	WfEvent input
	        [64]; /* ordered type-ahead; a full queue refuses ownership */
	size_t input_n;
	bool input_draining;
	WfStatus input_notice;
	uint64_t now_ms;
	/* A long word motion runs in slices on the event loop. The text and the
	 * visible cursor stay unchanged until the result is complete. */
	struct
	{
		WfWordScan scan;
		WfBuffer *buffer;
		WfPos from;
		uint64_t gen;
		bool active, extend, deleting;
	} motion;
	char exhist[WF_ES_HISTORY][WF_ES_INPUT_MAX];
	size_t exhist_n, exhist_pos;
};

WfBuffer *wf_es_buffer(WfEditSession *s);
/* Tree-sitter colours (wf_estsyntax.c). */
void wf_es_syntax_assign(WfEditSession *s, WfBuffer *b, const wchar_t *path);
void wf_es_syntax_sync(WfEditSession *s, WfBuffer *b);
void wf_es_syntax_close(WfEditSession *s);
/* Runs one editor command as if its key had been pressed (wf_evim.c). */
WfEsResult wf_es_run(WfEditSession *s, WfEdCmd cmd, bool extend);
/* Sends the editor's clip to the system clipboard (so a line clip survives). */
void wf_es_clip_out(WfEditSession *s);
/* Says that the action stopped, and why (as the editor's own failures are said). */
void wf_es_fail_status(WfEditSession *s, WfStatus st);
/* Cells the text area is wide in the current layout (wf_eview.c). */
int wf_es_text_width(const WfEditSession *s, const WfBuffer *b);
void wf_es_wrap_free(WfBuffer *b); /* the cache of soft wrap rows */

/* The Markdown preview pane (wf_emdpane.c). cols: the columns the pane and
 * its separator take at the right of the screen, 0 when it is not shown. */
int wf_es_md_cols(const WfEditSession *s, const WfBuffer *b);
uint32_t wf_es_md_wait(const WfEditSession *s, uint64_t now_ms);
bool wf_es_md_tick(WfEditSession *s, uint64_t now_ms);
WfEsResult wf_es_md_toggle(WfEditSession *s);
void wf_es_md_draw(WfEditSession *s, WfBuffer *b, WfFrame *f, int th);

/* The diff pane against HEAD (wf_egitpane.c). cols: as for the Markdown pane. */
void wf_es_git_assign(WfEditSession *s, WfBuffer *b, const wchar_t *path);
int wf_es_git_cols(const WfEditSession *s, const WfBuffer *b);
uint32_t wf_es_git_wait(const WfEditSession *s, uint64_t now_ms);
bool wf_es_git_tick(WfEditSession *s, uint64_t now_ms);
void wf_es_git_draw(WfEditSession *s, WfBuffer *b, WfFrame *f, int th);

#endif

#ifndef WF_EUNDO_H
#define WF_EUNDO_H

#include "wf_etext.h"

/*
 * Undo/redo as a stack of replacements. Each record says: at `at`, the text
 * `del` was replaced by `ins` (either may be empty). Applying a record
 * backwards deletes `ins` and puts `del` back, so the structure of the text,
 * terminators included, is restored exactly.
 *
 * Records of one user action share a `group` and are undone together (a
 * paste, a replace-all). Adjacent typing, backspace and forward-delete edits
 * may merge within the merge-size limit. Typing also checks whitespace
 * boundaries. Merging never crosses the saved state:
 * the record current when the file was saved is closed, so "modified" stays exact.
 *
 * The history is bounded in bytes. When it is full the oldest records are
 * dropped; if that loses the saved state, the buffer stays "modified" until
 * the next save instead of pretending to be clean.
 */
typedef enum
{
	WF_UK_OTHER,
	WF_UK_TYPE, /* typed characters: may merge */
	WF_UK_BACK, /* backspace: may merge */
	WF_UK_DEL   /* delete forward: may merge */
} WfUKind;

typedef struct
{
	WfPos at;
	char *del;
	size_t del_n;
	char *ins;
	size_t ins_n;
	WfPos before, after; /* cursor before the change and after it */
	uint32_t group;
	WfUKind kind;
} WfURec;

typedef struct
{
	WfURec *rec;
	size_t n, cap;
	size_t pos; /* records [0, pos) are applied; the rest can be redone */
	size_t bytes, max_bytes;
	uint32_t next_group;
	uint32_t open_group; /* 0 = none */
	size_t open_bytes;
	size_t clean_pos;
	bool clean_lost;
	bool truncated;        /* the oldest history was dropped */
	bool capacity_refused; /* last push could not keep the open group whole */
} WfUndo;

void wf_un_init(WfUndo *u, size_t max_bytes);
void wf_un_free(WfUndo *u);

/* Starts/ends a group: records pushed in between undo as one step. */
void wf_un_group_begin(WfUndo *u);
void wf_un_group_end(WfUndo *u);

/* Takes ownership of rec->del and rec->ins (also on failure). Drops the redo
 * tail. May merge into the previous record (kind TYPE/BACK/DEL). */
WfStatus wf_un_push(WfUndo *u, WfURec *rec);

bool wf_un_can_undo(const WfUndo *u);
bool wf_un_can_redo(const WfUndo *u);
/* The index range [first, last] of the group to undo / redo next. */
bool wf_un_undo_span(const WfUndo *u, size_t *first, size_t *last);
bool wf_un_redo_span(const WfUndo *u, size_t *first, size_t *last);

void wf_un_mark_clean(WfUndo *u);
bool wf_un_modified(const WfUndo *u);

/* Where `s` (raw bytes) ends when inserted at `at`. */
WfPos wf_et_advance(WfPos at, const char *s, size_t n);

#endif

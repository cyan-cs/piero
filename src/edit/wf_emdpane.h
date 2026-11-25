#ifndef WF_EMDPANE_H
#define WF_EMDPANE_H

#include <stdbool.h>
#include <stdint.h>

#include "../base/wf_status.h"
#include "../text/wf_markdown.h"
#include "wf_etext.h"

/*
 * The right-hand pane of the two-pane editor (DR-DRAFT-025): the Markdown
 * layout of the buffer's current text. It only ever reads a copy of the text;
 * saving, undo and the buffer itself never see it.
 *
 * Automatic refresh after text edits is debounced by WF_MDP_DEBOUNCE_MS
 * and runs from the idle wake-up. Explicit wf_mdp_refresh() calls rebuild
 * immediately, including when the pane is opened or resized.
 */
#define WF_MDP_DEBOUNCE_MS 80u

typedef struct
{
	WfMdDoc *doc;  /* the last layout; may be older than the text */
	int width;     /* the width it was laid out for */
	uint64_t gen;  /* the text generation it shows */
	uint64_t seen; /* the newest generation noticed */
	bool seen_valid;
	uint64_t due_ms; /* when to lay the new text out; 0: nothing waits */
} WfMdPane;

void wf_mdp_free(WfMdPane *p);
/* Lays the text of `t` out at `width` now. The old layout is kept when this
 * fails. */
WfStatus wf_mdp_refresh(WfMdPane *p, const WfEText *t, int width);
/* Each new text generation moves the deadline; redraws without changes leave
 * it alone. Call whenever the pane is drawn. */
void wf_mdp_note(WfMdPane *p, const WfEText *t, uint64_t now_ms);

#endif

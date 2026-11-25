#ifndef WF_MDDRAW_H
#define WF_MDDRAW_H

#include "../text/wf_markdown.h"
#include "wf_frame.h"

/*
 * Paints a laid-out Markdown document (DR-DRAFT-025) with the active theme.
 * Shared by the file manager's preview and the editor's right pane.
 * Every colour comes from the theme; the attributes
 * (bold, underline, italic, dim) tell headings and emphasis apart when the
 * terminal shows no colour at all.
 */

/* The style of a run with these WF_MDS_* bits. */
WfStyle wf_md_style(unsigned bits);

/* Draws document rows [top, top + h) in the rectangle. Code rows fill the
 * full width with the code background; other uncovered cells are unchanged. */
void wf_md_draw(
        WfFrame *f, int x, int y, int w, int h, const WfMdDoc *d, size_t top);

#endif

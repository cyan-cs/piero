#ifndef WF_TABLE_H
#define WF_TABLE_H

#include <stdbool.h>
#include <stddef.h>

#include "../base/wf_buf.h"
#include "../base/wf_status.h"
#include "wf_preview.h"

/*
 * CSV and TSV as aligned columns in the preview (DR-DRAFT-027). Pure: text in,
 * text out; nothing is read from disk and nothing is run.
 *
 * CSV-style quoting: a field may start with double quotes; doubled quotes
 * become one quote, and quoted delimiters belong to the field. Quoted LF
 * becomes a space; an unclosed quote is accepted through the end of input.
 * Unquoted LF or CRLF ends a record. Empty records are skipped. Only WF_TABLE_ROWS
 * records and WF_TABLE_COLS columns are drawn; a cell is cut to WF_TABLE_CELL_CELLS
 * display cells with "...". The first record is the header: a rule follows it.
 */
#define WF_TABLE_ROWS 200u
#define WF_TABLE_COLS 64u
#define WF_TABLE_CELL_CELLS 32u

/* ',' for .csv, '\t' for .tsv (also .tab), 0 for any other name. */
char wf_preview_table_delim(const wchar_t *name);

/* Appends aligned lines (separated by '\n', no trailing one) for `utf8[0..n)`.
 * `*rows` counts records drawn; `*cut` is set when records or columns were left out. WF_OK with
 * `*rows` == 0 means there was nothing to draw. */
WfStatus wf_table_render(const char *utf8,
        size_t n,
        char delim,
        WfBuf *out,
        size_t *rows,
        bool *cut);

/* Replaces the lines of a TEXT preview by the table (a TSV preview must have been
 * decoded with its tabs kept). For comma-delimited input, uses semicolons if
 * the first record has semicolons and no commas outside quotes.
 * Leaves the preview unchanged when no table is produced; marks it truncated
 * when rows or columns were left out. */
WfStatus wf_preview_tableize(WfPreview *p, char delim);

#endif

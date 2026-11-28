#ifndef WF_TERM_H
#define WF_TERM_H

#include <windows.h>

#include <stddef.h>

#include "../base/wf_status.h"
#include "../ui/wf_event.h"
#include "../render/wf_term_caps.h"

/* Windows console modes, input records and VT output. */
typedef struct WfTerm WfTerm;

/* Enters the alternate screen with VT output and raw-ish input. The
 * previous console modes are restored by wf_term_close(). */
WfStatus wf_term_open(WfTerm **out);
void wf_term_close(WfTerm *t);

/*
 * Editing mode for the built-in editor: Ctrl+C and the other control keys
 * arrive as keys instead of signals, and modified keys (Ctrl, Shift, Alt with
 * letters and navigation keys) are reported as the WF_EV_CTRL / WF_EV_ALT
 * events and the extra WfKey values. Modified-key reporting can also be enabled
 * separately for the file manager.
 */
WfStatus wf_term_set_editing(WfTerm *t, bool on);

/*
 * Modified keys in the file manager: Ctrl, Alt and Shift chords arrive as the
 * same events as in the editor, so keys can be bound to them. Unlike editing
 * mode, Ctrl+C stays a signal (the console keeps PROCESSED_INPUT). Off by
 * default: the Legacy profile does not need it.
 */
WfStatus wf_term_set_modified_keys(WfTerm *t, bool on);

HANDLE wf_term_input_handle(const WfTerm *t);
/* Manual-reset event signalled by Ctrl+C / Ctrl+Break / window close. */
HANDLE wf_term_shutdown_event(const WfTerm *t);

const WfTermCaps *wf_term_capabilities(const WfTerm *t);
void wf_term_probe(WfTerm *t);

WfStatus wf_term_size(const WfTerm *t, int *cols, int *rows);

/* Writes UTF-8 VT text (chunked, never splitting a surrogate pair or, when
 * avoidable, an escape sequence). */
WfStatus wf_term_write(WfTerm *t, const char *utf8, size_t n);

/* Drains at most `max` pending input records into `ev`; returns the count. */
size_t wf_term_read_events(WfTerm *t, WfEvent *ev, size_t max);
/* Buffered records/repeats need delivery even if the console is not signalled. */
bool wf_term_has_pending_input(const WfTerm *t);

/*
 * Drops every queued input record except key-ups, which are kept and
 * delivered by the next wf_term_read_events(). Used when a modal opens so
 * keys typed ahead are discarded without losing the release of the key that
 * opened it.
 */
void wf_term_drain_input(WfTerm *t);

/*
 * Why the shutdown event was raised.
 *   INTERRUPT  Ctrl+C / Ctrl+Break: the process lives on; run the full cleanup.
 *   FORCED     console close / logoff / shutdown: the process is ended as soon
 *              as wf_term_close() returns (or after a fixed bound). Terminal
 *              restoration is best effort; treat it as a crash and skip
 *              any slow or optional cleanup.
 * Valid between wf_term_open() and the next wf_term_open().
 */
typedef enum
{
	WF_CLOSE_NONE = 0,
	WF_CLOSE_INTERRUPT = 1,
	WF_CLOSE_FORCED = 2
} WfCloseReason;

WfCloseReason wf_term_close_reason(void);

/* Fixed VT reset for `piero --reset-terminal`. */
const char *wf_term_reset_sequence(void);

#endif

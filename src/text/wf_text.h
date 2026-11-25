#ifndef WF_TEXT_H
#define WF_TEXT_H

#include <stddef.h>
#include <stdint.h>

#include "../base/wf_buf.h"

/*
 * Platform assumption (Windows): wchar_t is a UTF-16 code unit. The text
 * layer is otherwise plain C17 and has no Win32 dependency.
 */

#define WF_CP_REPLACEMENT 0xFFFDu

/* Strict UTF-8 decode of one scalar; cp is required. For non-NULL s and n > 0
 * always consumes >= 1 byte;
 * malformed input yields U+FFFD and consumes exactly one byte. Returns 0
 * (with *cp = U+FFFD) only for an empty or NULL input. */
size_t wf_utf8_decode(const char *s, size_t n, uint32_t *cp);

/* Encodes one scalar into out[0..3]. Non-scalars (surrogates, > 10FFFF)
 * are encoded as U+FFFD. Returns the byte count, 1..4. */
size_t wf_utf8_encode(uint32_t cp, char out[4]);

/* Byte prefix ending on a scalar boundary; n may include terminal controls. */
size_t wf_utf8_prefix(const char *s, size_t n, size_t budget);
/* Sanitized text shortened by both cell width and destination byte capacity.
 * Adds "..." when shortened and capacity permits. No partial scalar is copied. */
void wf_utf8_elide(char *out, size_t cap, const char *s, size_t cells);

/* Next scalar from UTF-16 at *i, advancing *i. Lone surrogates -> U+FFFD.
 * *i >= n yields U+FFFD without advancing. */
uint32_t wf_utf16_next(const wchar_t *s, size_t n, size_t *i);

/* UTF-16 caret movement, in code units, not grapheme clusters. The caller
 * supplies a boundary within readable s[0..len); lone surrogates are one unit.
 * Previous needs pos readable units; next clamps pos >= len to len. */
size_t wf_utf16_prev_boundary(const wchar_t *s, size_t pos);
size_t wf_utf16_next_boundary(const wchar_t *s, size_t len, size_t pos);

/* Converts UTF-8 to UTF-16 into out[0..out_cap). n code units always suffice.
 * WF_E_RANGE if out_cap is too small; *used is 0 on any failure. */
WfStatus wf_utf8_to_utf16(
        const char *s, size_t n, wchar_t *out, size_t out_cap, size_t *used);

/*
 * Length of the next piece to write when output must be split into chunks of
 * at most `want` UTF-16 units (want >= 2). The cut never separates a
 * surrogate pair. It prefers the last ESC in offsets 1..want as the cut;
 * escape sequences longer than want can still straddle chunks. When n <= want
 * the whole remainder is returned. Always >= 1 for n > 0; a want < 2 degrades
 * to 1 unit per chunk and can split a surrogate pair.
 */
size_t wf_utf16_chunk_end(const wchar_t *w, size_t n, size_t want);

/* Display columns of a scalar: 0 (combining), 1, or 2 (CJK, fullwidth and
 * emoji with East_Asian_Width=W). Ambiguous-width characters (East_Asian_Width=A:
 * accented Latin, Greek, Cyrillic, many symbols and box drawing) are width 1
 * (D-025) unless wf_set_ambiguous_width(2) was called. */
int wf_cp_width(uint32_t cp);

/* What the terminal draws ambiguous-width characters as: 1 (the default) or 2
 * (the common setting for Japanese, Chinese and Korean environments), from
 * `[terminal] ambiguous_width`. Any other value is 1. Call it once at start,
 * before any other thread measures text: it is a plain setting, not a lock. */
void wf_set_ambiguous_width(int cells);

/* Cursor steps in UTF-8 text, over what the user sees as one character: a
 * letter with its combining marks and variation selectors, an emoji with its
 * skin tone. Both stay inside [0, n] and always move when they can, on
 * invalid bytes too. The editor steps the same way (wf_et_col_*). */
size_t wf_utf8_cluster_next(const char *s, size_t n, size_t at);
size_t wf_utf8_cluster_prev(const char *s, size_t n, size_t at);

/* Display columns of sanitized UTF-8 (sum of wf_cp_width over scalars). */
int wf_utf8_cells(const char *s, size_t n);

/*
 * The single entry point for external text (file names, OS strings, INI
 * errors, logs). Appends to `out` text that is safe to put in a cell:
 * C0/DEL -> ^X, C1 -> <U+00XX>, bidi and zero-width controls -> <LABEL>,
 * Tab -> 4 spaces, malformed input -> U+FFFD. Everything else is unchanged.
 * On failure `out` may contain a partial result.
 */
WfStatus wf_sanitize_utf8(const char *s, size_t n, WfBuf *out);
/* The display columns that wf_sanitize_utf8 gives one scalar: the length of the
 * text that replaces it, or its own width when it is kept. */
size_t wf_sanitized_cells(uint32_t cp);
WfStatus wf_sanitize_utf16(const wchar_t *s, size_t n, WfBuf *out);

#endif

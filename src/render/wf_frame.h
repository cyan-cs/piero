#ifndef WF_FRAME_H
#define WF_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"

#define WF_CELL_TEXT_MAX 20

enum
{
	WF_ATTR_BOLD = 1u << 0,
	WF_ATTR_DIM = 1u << 1,
	WF_ATTR_UNDERLINE = 1u << 2,
	WF_ATTR_REVERSE = 1u << 3,
	WF_ATTR_ITALIC = 1u << 4,
};

typedef struct
{
	uint32_t fg_rgb, bg_rgb;
	uint16_t attributes;
} WfStyle;

/*
 * width: 0 = continuation of the wide cell to the left, 1 or 2 = head.
 * The text is inline, sanitized, NUL-terminated; the cell never points
 * into another frame's storage.
 */
typedef struct
{
	char utf8[WF_CELL_TEXT_MAX];
	uint32_t fg_rgb, bg_rgb;
	uint16_t attributes;
	uint8_t width;
	uint8_t flags;
} WfCell;
/* Keep cells at 32 bytes for frame storage and copying. The inline text must
 * hold at least one 4-byte UTF-8 scalar and its NUL; combining marks use the
 * remaining capacity. */
_Static_assert(sizeof(WfCell) == 32, "WfCell no longer fits half a cache line");
_Static_assert(
        WF_CELL_TEXT_MAX >= 5, "a cell must hold one UTF-8 scalar and a NUL");

typedef struct
{
	int cols, rows;
	struct
	{
		const char *data; /* borrowed until present returns */
		size_t len;
		uint32_t gen;
		int x, y, w, h;
	} graphic;
	/* Where the terminal's own cursor belongs when this frame is shown. A
	 * text field being edited sets it: the terminal puts an IME composition
	 * at its cursor, so the cursor must sit on the insertion cell. Not
	 * visible means hidden (every frame starts that way). */
	struct
	{
		bool visible;
		int x, y;
	} cursor;
	WfCell *cells; /* owned, rows * cols, row-major */
} WfFrame;

/* A bogus console report must not become a GB allocation. A side is bounded,
 * and so is the whole: the program draws into two frames in turn, so the cells
 * it can ask for are twice this, 64 MiB at 32 bytes a cell. (A terminal of a
 * thousand columns and a thousand rows is 1000000 cells.) */
#define WF_FRAME_MAX_DIM 4096
#define WF_FRAME_MAX_CELLS ((size_t)1 << 20)

/* Returns NULL on invalid size or allocation failure. All cells are blank. */
WfFrame *wf_frame_new(int cols, int rows, const WfStyle *blank);

/* Makes *f a frame of this size to be drawn over. A frame of that size is kept as
 * it is: its cells hold what was drawn in it before, so the one who draws must
 * overwrite every cell first (wf_app_draw begins by filling the whole frame);
 * only the graphic and the cursor are reset. A frame of another size, or none, is
 * replaced by a new one, all blank. False on a bad size or no memory, and *f is
 * then left as it was. */
bool wf_frame_reuse(WfFrame **f, int cols, int rows, const WfStyle *blank);
void wf_frame_free(WfFrame *f);

/* Field-wise comparison; padding bytes are never compared. */
bool wf_cell_equal(const WfCell *a, const WfCell *b);

/*
 * Writes UTF-8 at (x, y), replacing controls with '?' and malformed input
 * with U+FFFD. Uses at most max_cells columns without crossing the right edge.
 * A wide character that does
 * not fit becomes '?'. Returns the columns consumed.
 */
int wf_frame_put(WfFrame *f,
        int x,
        int y,
        const char *utf8,
        size_t len,
        const WfStyle *st,
        int max_cells);

/* Puts the terminal cursor on cell (x, y); outside the frame hides it. */
void wf_frame_set_cursor(WfFrame *f, int x, int y);

/* Fills a clipped rectangle with one ASCII character. */
void wf_frame_fill(
        WfFrame *f, int x, int y, int w, int h, char ch, const WfStyle *st);

#endif

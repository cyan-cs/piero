/*
 * Where the terminal's own cursor is after a frame, for every text field of
 * the file manager (the editor is covered in test_esession.c).
 *
 * A terminal puts an IME composition at its cursor. Drawing leaves the cursor
 * at the last cell written, the status line, so a focused text field has to
 * end the frame with the cursor on its insertion cell, and every other screen
 * with the cursor hidden. These tests read the frame; they say where the
 * cursor ends up, not what an IME does with it.
 */
#include <string.h>

#include "base/wf_macro.h"
#include "text/wf_text.h"
#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

#define COLS 100
#define ROWS 30

static void ch(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};
	a->mode->on_event(a, &e);
}

static void key(WfApp *a, WfKey k)
{
	WfEvent e = {WF_EV_KEY, 0, k, 0};
	a->mode->on_event(a, &e);
}

/* Draws and returns the frame (caller frees). */
static WfFrame *draw(const WfApp *a)
{
	WfFrame *f = wf_frame_new(a->cols, a->rows, &(WfStyle){0, 0, 0});

	CHECK(NULL != f);
	if (f) {
		wf_app_draw(a, f);
	}
	return f;
}

/* The cell under the cursor says "_" (the insertion mark of the field). */
static bool cursor_on_mark(const WfApp *a, int *x, int *y)
{
	WfFrame *f = draw(a);
	bool ok = false;

	if (f) {
		ok = f->cursor.visible && f->cursor.x < f->cols &&
		        f->cursor.y < f->rows &&
		        !strcmp("_",
		                f->cells[f->cursor.y * f->cols + f->cursor.x]
		                        .utf8);
		if (!ok) {
			printf("  cursor visible=%d at %d,%d, cell [%s]\n",
			        f->cursor.visible, f->cursor.x, f->cursor.y,
			        f->cursor.visible
			                ? f->cells[f->cursor.y * f->cols +
			                           f->cursor.x]
			                          .utf8
			                : "-");
		}
		*x = f->cursor.x;
		*y = f->cursor.y;
		wf_frame_free(f);
	}
	return ok;
}

static bool cursor_hidden(const WfApp *a)
{
	WfFrame *f = draw(a);
	bool hidden = f && !f->cursor.visible;

	wf_frame_free(f);
	return hidden;
}

/* Japanese, an emoji and a character outside the BMP, as an IME commits them */
static const uint32_t TEXT[] = {0x6708, 0x304C, 0x304D, 0x308C, 0x3044, 0x3067,
        0x3059, 0x306D, 0x1F600, 0x20BB7};

static int typed_cells(size_t count)
{
	int w = 0;
	size_t i;

	for (i = 0; i < count; i++) {
		w += (int)wf_cp_width(TEXT[i]);
	}
	return w;
}

/* The mark moves right by exactly the width of each committed character. */
static void field_follows_committed_text(WfApp *a, int expect_y_from_bottom)
{
	int x0, y0, x, y;
	size_t i;

	CHECK(cursor_on_mark(a, &x0, &y0));
	CHECK(expect_y_from_bottom <= 0 || y0 == a->rows - expect_y_from_bottom);
	for (i = 0; i < WF_ARRAY_LEN(TEXT); i++) {
		ch(a, TEXT[i]);
		CHECK(cursor_on_mark(a, &x, &y));
		CHECK(y == y0 && x == x0 + typed_cells(i + 1));
	}
	/* Backspace removes one whole character, the pair included */
	key(a, WF_KEY_BACKSPACE);
	CHECK(cursor_on_mark(a, &x, &y));
	CHECK(y == y0 && x == x0 + typed_cells(WF_ARRAY_LEN(TEXT) - 1));
}

static void a_name_form_puts_the_cursor_after_the_text(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	int x, y;

	CHECK(WF_OK ==
	        wf_app_init(&a, COLS, ROWS, fake_port(&fp), L"C:\\Users"));
	CHECK(cursor_hidden(&a)); /* the file list: no cursor */
	ch(&a, 'a');
	ch(&a, '%');
	CHECK(!strcmp(a.mode->name, "NAME"));
	field_follows_committed_text(&a, 0); /* on the list row, in place */
	key(&a, WF_KEY_LEFT);
	CHECK(cursor_on_mark(&a, &x, &y));
	key(&a, WF_KEY_ESC);
	CHECK(cursor_hidden(&a)); /* back to the list */
	wf_app_destroy(&a);
}

static void the_filter_puts_the_cursor_after_the_text(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	CHECK(WF_OK ==
	        wf_app_init(&a, COLS, ROWS, fake_port(&fp), L"C:\\Users"));
	ch(&a, '/');
	CHECK(&WF_MODE_FILTER == a.mode);
	field_follows_committed_text(&a, 2);
	key(&a, WF_KEY_ESC);
	CHECK(cursor_hidden(&a));
	wf_app_destroy(&a);
}

static void the_go_prompt_puts_the_cursor_after_the_text(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	int x, y;

	CHECK(WF_OK ==
	        wf_app_init(&a, COLS, ROWS, fake_port(&fp), L"C:\\Users"));
	ch(&a, 'g');
	CHECK(!strcmp(a.mode->name, "PATH") || !strcmp(a.mode->name, "GO"));
	/* a first character replaces a selected path; after it the mark follows */
	ch(&a, 'a');
	CHECK(cursor_on_mark(&a, &x, &y));
	{
		int base = x, y0 = y;
		size_t i;

		for (i = 0; i < WF_ARRAY_LEN(TEXT); i++) {
			ch(&a, TEXT[i]);
			CHECK(cursor_on_mark(&a, &x, &y));
			CHECK(y == y0 && x == base + typed_cells(i + 1));
		}
	}
	key(&a, WF_KEY_ESC);
	CHECK(cursor_hidden(&a));
	wf_app_destroy(&a);
}

/* The same field in a smaller window: the cursor stays inside the frame and
 * on the mark, however narrow it gets. */
static void the_cursor_stays_in_a_resized_window(void)
{
	static const int sizes[][2] = {{20, 5}, {40, 12}, {80, 24}, {100, 30}};
	size_t i, k;

	for (i = 0; i < WF_ARRAY_LEN(sizes); i++) {
		WfApp a;
		FakePort fp = {.app = &a};
		int x, y;

		CHECK(WF_OK ==
		        wf_app_init(&a, sizes[i][0], sizes[i][1],
		                fake_port(&fp), L"C:\\Users"));
		ch(&a, 'a');
		ch(&a, '%');
		for (k = 0; k < WF_ARRAY_LEN(TEXT); k++) {
			ch(&a, TEXT[k]);
			ch(&a, 'x');
		}
		CHECK(cursor_on_mark(&a, &x, &y));
		wf_app_destroy(&a);
	}
}

/* The ':' command line shows its insertion point as a reversed cell. */
static void the_command_line_puts_the_cursor_on_its_reversed_cell(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	size_t i;

	CHECK(WF_OK ==
	        wf_app_init(&a, COLS, ROWS, fake_port(&fp), L"C:\\Users"));
	wf_app_cmdline_begin(&a);
	for (i = 0; i <= WF_ARRAY_LEN(TEXT); i++) {
		WfFrame *f = draw(&a);

		if (f) {
			const WfCell *at =
			        &f->cells[f->cursor.y * f->cols + f->cursor.x];

			CHECK(f->cursor.visible && ROWS - 1 == f->cursor.y);
			CHECK(f->cursor.x == 1 + typed_cells(i));
			CHECK(at->attributes & WF_ATTR_REVERSE);
			wf_frame_free(f);
		}
		if (i < WF_ARRAY_LEN(TEXT)) {
			ch(&a, TEXT[i]);
		}
	}
	key(&a, WF_KEY_ESC);
	CHECK(cursor_hidden(&a));
	wf_app_destroy(&a);
}

int main(void)
{
	a_name_form_puts_the_cursor_after_the_text();
	the_filter_puts_the_cursor_after_the_text();
	the_go_prompt_puts_the_cursor_after_the_text();
	the_cursor_stays_in_a_resized_window();
	the_command_line_puts_the_cursor_on_its_reversed_cell();
	return TEST_RESULT();
}

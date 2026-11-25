/*
 * Pictures and PDF that cannot be shown, through the real product.
 *
 * product_pictures() proves the pages that work. This is the other half: a
 * damaged, cut off, password-protected or non-picture file must say what is
 * wrong, in words, and leave the program usable, the file untouched and the
 * decoder isolated. The samples are the ones tests/pdf_samples.h already
 * holds. Included by p0_conpty_host.c after product_pictures().
 *
 * Folder "pics-bad" holds one file per case, named so that the list order is
 * the order below; each `j` moves the cursor to the next case.
 */

typedef struct
{
	const char *file;           /* name in the folder (ASCII) */
	const unsigned char *bytes; /* its content; NULL with size 0: empty */
	size_t size;
	const char *
	        shows; /* what the preview says, as the English catalog has it */
	const char *name; /* evidence name */
} PictureCase;

static const unsigned char picture_damaged_png[] = {0x89, 'P', 'N', 'G', 0x0D,
        0x0A, 0x1A, 0x0A, 'n', 'o', 't', ' ', 'a', ' ', 'p', 'i', 'c', 't', 'u',
        'r', 'e'};

/* True when the file in the lab still holds exactly `bytes`. */
static bool picture_unchanged(
        Product *pr, const wchar_t *rel, const unsigned char *bytes, size_t n)
{
	wchar_t path[1100];
	unsigned char *got = NULL;
	HANDLE h;
	LARGE_INTEGER size;
	DWORD read = 0;
	bool same = false;

	if (!p0_lab_path(&pr->lab, rel, path, 1100)) {
		return false;
	}
	h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	        NULL, OPEN_EXISTING, 0, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	if (GetFileSizeEx(h, &size) && (LONGLONG)n == size.QuadPart) {
		got = malloc(n ? n : 1);
		same = got &&
		        (!n ||
		                (ReadFile(h, got, (DWORD)n, &read, NULL) &&
		                        read == n && !memcmp(got, bytes, n)));
	}
	free(got);
	CloseHandle(h);
	return same;
}

/* Waits for `text` (and shows it as evidence under `name`). */
static bool picture_expect(
        Product *pr, const char *name, const char *text, DWORD ms)
{
	int ok = product_wait_ms(
	        &pr->p, text, pr->screen, sizeof pr->screen, ms);

	product_snapshot(pr, name);
	ev_check(pr->e, name, 0, text, ok ? "observed" : "missing", ok);
	return ok;
}

/* The words of a refusal are shown, and nothing internal comes with them. */
static void picture_no_internal_names(Product *pr, const char *name)
{
	static const char *const internal[] = {
	        "P0-", "G-IMG", "G-OPEN", "DR-DRAFT", "HRESULT", "0x8"};
	size_t i;
	const char *seen = "none";

	for (i = 0; i < sizeof internal / sizeof *internal; i++) {
		if (strstr(pr->screen, internal[i])) {
			seen = internal[i];
			break;
		}
	}
	ev_check(pr->e, name, 0, "no gate or error-code names on screen", seen,
	        !strcmp(seen, "none"));
}

/* The encrypted PDF: the password is asked for, a wrong one is refused with
 * the way back, the right one shows the page. */
static bool picture_password(Product *pr)
{
	int ok;

	PRODUCT_STEP("\x1b[C", "PREVIEW_PANE", "password_focus_pane");
	PRODUCT_STEP("\r", "Password:", "password_is_asked_for");
	pty_send(&pr->p, "wrong\r", 6);
	if (!picture_expect(
	            pr, "password_wrong_is_refused", "Wrong password", 20000)) {
		return false;
	}
	picture_no_internal_names(pr, "password_refusal_has_no_internal_names");
	PRODUCT_STEP("\r", "Password:", "password_is_asked_again");
	pty_send(&pr->p, "secret\r", 7);
	if (!picture_expect(
	            pr, "password_right_shows_the_page", "page 1/", 20000)) {
		return false;
	}
	ev_check(pr->e, "password_page_is_drawn", 0,
	        "more than 20 half-block cells",
	        count_text(pr->screen, "\xE2\x96\x80") > 20 ? "observed"
	                                                    : "missing",
	        count_text(pr->screen, "\xE2\x96\x80") > 20);
	PRODUCT_STEP("\x1b[D", "NORMAL", "password_back_to_the_list");
	return true;
}

/* A refusal and the one before it say the same words, so a refusal is only
 * believed when a file that is shown comes between: stale text on screen can
 * never satisfy the next check. */
#define PICTURE_CASES 8

static bool product_picture_failures(Product *pr)
{
	unsigned char bmp[54 + 8 * 24];
	PictureCase cases[PICTURE_CASES] = {
	        {"1-truncated.pdf", pdf_truncated, sizeof pdf_truncated,
	                "Cannot show this file", "truncated_pdf_is_refused"},
	        {"2-encrypted.pdf", pdf_encrypted, sizeof pdf_encrypted,
	                "Password-protected PDF",
	                "encrypted_pdf_asks_for_password"},
	        {"3-japanese.pdf", pdf_japanese, sizeof pdf_japanese, "page 1/",
	                "japanese_pdf_is_shown"},
	        {"4-garbage.pdf", pdf_garbage, sizeof pdf_garbage,
	                "Cannot show this file", "garbage_pdf_is_refused"},
	        {"5-valid.pdf", pdf_two_page, sizeof pdf_two_page, "page 1/2",
	                "valid_pdf_is_shown_again"},
	        {"6-empty.png", NULL, 0, "Cannot show this file",
	                "empty_png_is_refused"},
	        {"7-valid.bmp", bmp, 0, "8 x 8", "valid_bitmap_is_shown_again"},
	        {"8-damaged.png", picture_damaged_png,
	                sizeof picture_damaged_png, "Cannot show this file",
	                "damaged_png_is_refused"}};
	wchar_t dir[1024], pics[1024], rel[128];
	size_t i;
	int ok;

	cases[6].size = product_bmp(bmp);
	PRODUCT_REQUIRE(p0_area_mkdir(pr->lab.area, L"pics-bad"));
	for (i = 0; i < PICTURE_CASES; i++) {
		swprintf(rel, 128, L"pics-bad\\%hs", cases[i].file);
		PRODUCT_REQUIRE(product_file_bytes(
		        &pr->lab, rel, cases[i].bytes, cases[i].size));
	}
	PRODUCT_REQUIRE(p0_lab_path(&pr->lab, L"pics-bad", dir, 1024));
	PRODUCT_REQUIRE(go_by_path_bar(pr, dir, "pictures_bad_folder"));
	for (i = 0; i < PICTURE_CASES; i++) {
		if (i) {
			PRODUCT_STEP("j", cases[i].file, "pictures_bad_next");
		}
		if (!picture_expect(pr, cases[i].name, cases[i].shows, 30000)) {
			return false;
		}
		picture_no_internal_names(
		        pr, "pictures_have_no_internal_names");
		if (!strcmp(cases[i].file, "2-encrypted.pdf") &&
		        !picture_password(pr)) {
			return false;
		}
	}
	/* every file is as it was written: previewing reads, never writes */
	for (i = 0; i < PICTURE_CASES; i++) {
		swprintf(rel, 128, L"pics-bad\\%hs", cases[i].file);
		ok = picture_unchanged(pr, rel, cases[i].bytes, cases[i].size);
		ev_check(pr->e, "picture_files_are_unchanged", 0, cases[i].file,
		        ok ? "unchanged" : "CHANGED", ok);
	}
	/* the program is still usable after the last refusal: the file before
	 * it is shown again */
	PRODUCT_STEP("k", "8 x 8", "pictures_bad_still_responds");
	/* back where product_pictures() left the cursor, for the steps after */
	PRODUCT_REQUIRE(p0_lab_path(&pr->lab, L"pics", pics, 1024));
	PRODUCT_REQUIRE(go_by_path_bar(pr, pics, "pictures_folder_again"));
	PRODUCT_STEP("j", "8 x 8", "pictures_bitmap_again");
	return true;
}

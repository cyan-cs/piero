/*
 * Files from the Windows clipboard (V) through the real product: a test
 * process puts files on the clipboard the way Explorer's Copy does (CF_HDROP and
 * "Preferred DropEffect"), V is pressed in piero.exe, and the files arrive in the
 * folder on screen by the ordinary copy; what the clipboard may not do is
 * refused and changes nothing. The source files are checked by SHA-256 and file
 * id, the copies by SHA-256.
 *
 * This is a Shell compatible test process, not Explorer: it proves piero reads
 * what the Shell format carries, not that a real Explorer is satisfied.
 * Clipboard data belongs to a newly created private window station, also
 * assigned explicitly to each product child.
 * Included by p0_conpty_host.c after p0_watch.c, whose helpers it uses.
 */

#define COBJMACROS
#include <ole2.h>
#include <shlobj.h>

#include "clipboard/wf_clipboard.h"
#include "wf_clipboard_fixture.h"

#define CLIP_CONFIG                                                            \
	"[meta]\r\nschema_version = 1\r\nonboarding_completed = true\r\n"      \
	"[updates]\r\nenabled = false\r\nconsent_recorded = true\r\n"

static HWND g_clip_owner;

static bool clip_open(void)
{
	for (int i = 0; i < 100; i++) {
		if (OpenClipboard(g_clip_owner)) {
			return true;
		}
		Sleep(20);
	}
	return false;
}

static bool clip_put(UINT format, const void *data, size_t bytes)
{
	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes ? bytes : 1);

	if (!h) {
		return false;
	}
	memcpy(GlobalLock(h), data, bytes);
	GlobalUnlock(h);
	if (!SetClipboardData(format, h)) {
		GlobalFree(h);
		return false;
	}
	return true;
}

/* Explorer's Copy: the files as a DROPFILES block, and the effect. */
static bool clip_set_files(const wchar_t *const *path, size_t n, DWORD effect)
{
	unsigned char block[16384];
	DROPFILES h;
	size_t at = sizeof h;
	bool ok;

	memset(&h, 0, sizeof h);
	h.pFiles = (DWORD)sizeof h;
	h.fWide = TRUE;
	memcpy(block, &h, sizeof h);
	for (size_t i = 0; i < n; i++) {
		size_t bytes = (wcslen(path[i]) + 1) * sizeof(wchar_t);

		if (at + bytes + sizeof(wchar_t) > sizeof block) {
			return false;
		}
		memcpy(block + at, path[i], bytes);
		at += bytes;
	}
	memset(block + at, 0, sizeof(wchar_t));
	at += sizeof(wchar_t);
	if (!clip_open()) {
		return false;
	}
	EmptyClipboard();
	ok = clip_put(CF_HDROP, block, at) &&
	        clip_put(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT),
	                &effect, sizeof effect);
	CloseClipboard();
	return ok;
}

static bool clip_set_text(const wchar_t *text)
{
	bool ok;

	if (!clip_open()) {
		return false;
	}
	EmptyClipboard();
	ok = clip_put(
	        CF_UNICODETEXT, text, (wcslen(text) + 1) * sizeof(wchar_t));
	CloseClipboard();
	return ok;
}

/* SHA-256 of two files equal. */
static bool clip_same_bytes(const wchar_t *a, const wchar_t *b)
{
	char x[P0_HASH_HEX], y[P0_HASH_HEX];

	return p0_sha256_file(a, x) && p0_sha256_file(b, y) && !strcmp(x, y);
}

static bool clipboard_copy_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	bool ok = false;
	int fails = e->fails;
	wchar_t d1[1100], d2[1100];
	const wchar_t *two[2];
	char *dest_before = NULL;
	GateSnap doc, note;

	if (!g || !gate_wanted("clipboard_copy")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "clipboard_copy") ||
	        !gate_write(g->cfg, CLIP_CONFIG) || !gate_launch(g)) {
		goto done;
	}
	pr = &g->pr;
	swprintf(d1, 1100, L"%ls\\doc.txt", g->dest);
	swprintf(d2, 1100, L"%ls\\note.txt", g->dest);
	two[0] = g->doc;
	two[1] = g->note;
	/* Explorer's Copy of two files, then piero is taken to the folder to paste into */
	ok = clip_set_files(two, 2, 5) &&
	        gate_goto(g, g->dest, "clip_goto_dest");
	if (!ok) {
		goto done;
	}
	pty_send(&pr->p, "V", 1);
	ok = product_wait_ms(
	        &pr->p, "doc.txt", pr->screen, sizeof pr->screen, 20000);
	/* the copy is a job: wait until both files are on the disk */
	for (int i = 0; ok && i < 200 &&
	        !(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(d1) &&
	                INVALID_FILE_ATTRIBUTES != GetFileAttributesW(d2));
	        i++) {
		Sleep(50);
	}
	Sleep(300);
	ok = ok && clip_same_bytes(g->doc, d1) && clip_same_bytes(g->note, d2);
	ev_check(e, "clipboard_files_arrive_byte_for_byte", 0,
	        "both files copied, SHA-256 equal to the sources",
	        ok ? "equal" : "missing or different", ok);
	if (!ok) {
		goto done;
	}
	/* the sources: untouched, by SHA-256 and file id */
	ok = gate_snap(g->doc, &doc) && gate_same(&doc, &g->doc0) &&
	        gate_snap(g->note, &note) && gate_same(&note, &g->note0);
	ev_check(e, "clipboard_sources_are_untouched", 0,
	        "SHA-256 and file id as before", ok ? "unchanged" : "CHANGED",
	        ok);
	if (!ok) {
		goto done;
	}
	/* the same again: the names exist; the ordinary question is asked, and
	 * answering no leaves what is there */
	dest_before = gate_tree_text(g->dest);
	pty_send(&pr->p, "V", 1);
	ok = product_wait_ms(
	        &pr->p, "exist", pr->screen, sizeof pr->screen, 20000);
	product_snapshot(pr, "clipboard_collision_question");
	ev_check(e, "clipboard_existing_names_are_asked_about", 0,
	        "the ordinary question about names that exist",
	        ok ? "asked" : "missing", ok);
	if (ok) {
		pty_send(&pr->p, "\x1b", 1);
		Sleep(500);
		{
			char *now = gate_tree_text(g->dest);

			ok = dest_before && now && !strcmp(dest_before, now) &&
			        clip_same_bytes(g->doc, d1);
			free(now);
		}
		ev_check(e, "clipboard_answer_no_leaves_the_files", 0,
		        "the folder as it was", ok ? "unchanged" : "CHANGED",
		        ok);
	}
	ok = ok && gate_quit(g);
done:
	free(dest_before);
	return gate_first_run_end(g, e, ok, fails);
}

static bool clip_refusal(Gate *g, const char *name, const char *message)
{
	Product *pr = &g->pr;
	bool ok;
	char *before = gate_tree_text(g->dest), *after;

	pty_send(&pr->p, "V", 1);
	ok = product_wait_ms(
	        &pr->p, message, pr->screen, sizeof pr->screen, 20000);
	Sleep(300);
	after = gate_tree_text(g->dest);
	ok = ok && before && after && !strcmp(before, after);
	ev_check(pr->e, name, 0, message,
	        ok ? "refused, nothing changed" : "NOT refused", ok);
	free(before);
	free(after);
	return ok;
}

static bool clipboard_refusal_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	bool ok = false;
	int fails = e->fails;
	wchar_t gone[1100], other[1100];
	const wchar_t *one[1];
	const wchar_t *mixed[2];
	const wchar_t *share[1] = {L"\\\\server\\share\\x.txt"};

	if (!g || !gate_wanted("clipboard_refusal")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "clipboard_refusal") ||
	        !gate_write(g->cfg, CLIP_CONFIG) || !gate_launch(g)) {
		goto done;
	}
	swprintf(gone, 1100, L"%ls\\no-such-file.txt", g->browse);
	swprintf(other, 1100, L"%ls\\other.txt", g->dest);
	ok = gate_goto(g, g->dest, "clip_refusal_goto");
	one[0] = g->doc;
	mixed[0] = g->doc;
	mixed[1] = other;
	/* a cut: not supported yet, so it is refused, not half done */
	ok = ok && clip_set_files(one, 1, 2) &&
	        clip_refusal(
	                g, "clipboard_a_cut_is_refused", "not supported yet");
	ok = ok && clip_set_text(L"only text") &&
	        clip_refusal(
	                g, "clipboard_text_only_is_refused", "holds no files");
	ok = ok && clip_set_files(mixed, 2, 1) &&
	        clip_refusal(g, "clipboard_several_folders_are_refused",
	                "several folders");
	ok = ok && clip_set_files(share, 1, 1) &&
	        clip_refusal(
	                g, "clipboard_a_share_is_refused", "network share");
	/* a name that is not there: the capture fails, nothing is pasted */
	one[0] = gone;
	ok = ok && clip_set_files(one, 1, 1) &&
	        clip_refusal(g, "clipboard_a_missing_file_pastes_nothing",
	                "cannot read the selected");
	ok = ok && gate_quit(g);
done:
	return gate_first_run_end(g, e, ok, fails);
}

/* A receiver is an application: it serves its messages (how OLE learns that the
 * clipboard changed), then asks the clipboard for its formats. */
static void clip_serve_messages(DWORD ms)
{
	ULONGLONG until = GetTickCount64() + ms;

	do {
		MSG m;

		while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
		Sleep(10);
	} while (GetTickCount64() < until);
}

static bool clip_receiver_read(WfClipFiles *out, DWORD *effect)
{
	IDataObject *pdo = NULL;
	FORMATETC f = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
	FORMATETC e = f;
	STGMEDIUM m;
	bool ok = false;

	e.cfFormat =
	        (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
	*effect = 0;
	memset(out, 0, sizeof *out);
	clip_serve_messages(150);
	if (S_OK != OleGetClipboard(&pdo) || !pdo) {
		return false;
	}
	if (S_OK == IDataObject_GetData(pdo, &f, &m)) {
		void *p = GlobalLock(m.hGlobal);

		ok = p &&
		        WF_CLIP_OK ==
		                wf_clip_parse_hdrop(
		                        p, GlobalSize(m.hGlobal), out);
		if (p) {
			GlobalUnlock(m.hGlobal);
		}
		ReleaseStgMedium(&m);
	}
	if (ok && S_OK == IDataObject_GetData(pdo, &e, &m)) {
		DWORD *p = GlobalLock(m.hGlobal);

		*effect = p && GlobalSize(m.hGlobal) >= sizeof *p ? *p : 0;
		if (p) {
			GlobalUnlock(m.hGlobal);
		}
		ReleaseStgMedium(&m);
	}
	IDataObject_Release(pdo);
	return ok;
}

static bool clip_is(const WfClipFiles *f, const wchar_t *a, const wchar_t *b)
{
	return 2 == f->n && !_wcsicmp(f->path[0], a) &&
	        !_wcsicmp(f->path[1], b);
}

static bool clipboard_publish_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	bool ok = false;
	int fails = e->fails;
	WfClipFiles f;
	DWORD effect = 0;
	wchar_t d1[1100], d2[1100];
	GateSnap doc, note;

	if (!g || !gate_wanted("clipboard_publish")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "clipboard_publish") ||
	        !gate_write(g->cfg, CLIP_CONFIG) || !gate_launch(g)) {
		goto done;
	}
	pr = &g->pr;
	/* mark the two files (one key at a time), then Y */
	pty_send(&pr->p, " ", 1);
	ok = dirsize_row(g, "*doc.txt", "", 8000);
	pty_send(&pr->p, " ", 1);
	ok = ok && dirsize_row(g, "*note.txt", "", 8000);
	pty_send(&pr->p, "Y", 1);
	ok = ok &&
	        product_wait_ms(&pr->p, "Copied to the Windows clipboard: 2",
	                pr->screen, sizeof pr->screen, 20000);
	product_snapshot(pr, "clipboard_publish_done");
	ev_check(e, "clipboard_y_says_it_copied", 0,
	        "Copied to the Windows clipboard: 2",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	/* the receiver (Explorer's part): reads the clipboard as the Shell does */
	ok = clip_receiver_read(&f, &effect) && clip_is(&f, g->doc, g->note) &&
	        DROPEFFECT_COPY == effect;
	ev_check(e, "clipboard_receiver_finds_the_files_and_a_copy", 0,
	        "doc.txt and note.txt, Preferred DropEffect = copy",
	        ok ? "observed" : "missing or different", ok);
	wf_clipfiles_free(&f);
	if (!ok) {
		goto done;
	}
	/* the receiver pastes: it copies, byte for byte, into a folder of its own */
	swprintf(d1, 1100, L"%ls\\doc.txt", g->dest);
	swprintf(d2, 1100, L"%ls\\note.txt", g->dest);
	ok = CopyFileW(g->doc, d1, TRUE) && CopyFileW(g->note, d2, TRUE) &&
	        clip_same_bytes(g->doc, d1) && clip_same_bytes(g->note, d2);
	ev_check(e, "clipboard_receiver_paste_is_byte_for_byte", 0,
	        "both copies equal to the sources", ok ? "equal" : "different",
	        ok);
	/* piero changed nothing: the sources are as they were */
	ok = ok && gate_snap(g->doc, &doc) && gate_same(&doc, &g->doc0) &&
	        gate_snap(g->note, &note) && gate_same(&note, &g->note0);
	ev_check(e, "clipboard_y_changes_no_file", 0,
	        "SHA-256 and file id as before", ok ? "unchanged" : "CHANGED",
	        ok);
	ok = ok && gate_quit(g);
	/* piero has ended: the Shell was given the data, so a paste still works */
	ok = ok && clip_receiver_read(&f, &effect) &&
	        clip_is(&f, g->doc, g->note) && DROPEFFECT_COPY == effect;
	ev_check(e, "clipboard_files_survive_the_end_of_piero", 0,
	        "the same files on the clipboard after piero exited",
	        ok ? "observed" : "gone", ok);
	wf_clipfiles_free(&f);
done:
	return gate_first_run_end(g, e, ok, fails);
}

static int scn_clipboard(Ev *e)
{
	bool good;
	DWORD error = 0;
	if (!p0_clipboard_fixture(g_clipboard_desktop,
	            WF_ARRAY_LEN(g_clipboard_desktop), &error)) {
		printf("SKIP clipboard: private station unavailable, "
		       "error=%lu\n",
		        error);
		return 77;
	}

	g_clip_owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
	        HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
	if (!g_clip_owner) {
		return 77; /* skipped: no window to own the clipboard */
	}
	HRESULT initialized = OleInitialize(NULL);
	if (FAILED(initialized)) {
		DestroyWindow(g_clip_owner);
		printf("FAIL clipboard: OleInitialize failed, HRESULT=%08lx\n",
		        (unsigned long)initialized);
		return 1;
	}
	good = clipboard_copy_case(e);
	good = clipboard_refusal_case(e) && good;
	good = clipboard_publish_case(e) && good;
	OleUninitialize();
	DestroyWindow(g_clip_owner);
	if (!good || e->fails) {
		return 1;
	}
	return g_gate_skips ? 77 : 0;
}

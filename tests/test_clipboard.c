/*
 * The files on the Windows clipboard (DR-DRAFT-021): the parsing of the blocks
 * another program supplies is strict and refuses whole (a hostile or damaged
 * DROPFILES gives no path at all); the effect decides copy or cut; and the
 * clipboard worker reads real clipboard data on its own apartment.
 *
 * Native cases require a newly created private window station. --pure checks
 * parsing and data objects without reading or writing clipboard contents.
 */
#define COBJMACROS
#include <windows.h>

#include <ole2.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clipboard/wf_clipboard.h"
#include "clipboard/wf_clipworker_win.h"
#include "clipboard/wf_dataobj_files_win.h"
#include "../tools/p0/wf_clipboard_fixture.h"
#include "wf_test.h"

/* A DROPFILES block: header, the names, the final empty name. */
static size_t build(unsigned char *buf,
        size_t cap,
        bool wide,
        const wchar_t *const *path,
        size_t n)
{
	DROPFILES h;
	size_t at = sizeof h;

	memset(&h, 0, sizeof h);
	h.pFiles = (DWORD)sizeof h;
	h.fWide = wide;
	memcpy(buf, &h, sizeof h);
	for (size_t i = 0; i < n; i++) {
		if (wide) {
			size_t bytes = (wcslen(path[i]) + 1) * sizeof(wchar_t);

			memcpy(buf + at, path[i], bytes);
			at += bytes;
		} else {
			size_t len = wcslen(path[i]);

			for (size_t k = 0; k <= len; k++) {
				buf[at++] = (unsigned char)path[i][k];
			}
		}
	}
	/* the empty name that ends the list */
	if (wide) {
		memset(buf + at, 0, sizeof(wchar_t));
		at += sizeof(wchar_t);
	} else {
		buf[at++] = 0;
	}
	CHECK(at <= cap);
	return at;
}

static unsigned char block[1 << 17];

static WfClipReason parse(const void *data, size_t n, WfClipFiles *out)
{
	return wf_clip_parse_hdrop(data, n, out);
}

static void the_effect_decides_copy_or_cut(void)
{
	CHECK(wf_clip_effect_is_cut(2));  /* move */
	CHECK(!wf_clip_effect_is_cut(1)); /* copy */
	CHECK(!wf_clip_effect_is_cut(5)); /* copy | link: Explorer's copy */
	CHECK(!wf_clip_effect_is_cut(3)); /* the copy bit wins */
	CHECK(!wf_clip_effect_is_cut(4)); /* link */
	CHECK(!wf_clip_effect_is_cut(0)); /* nothing said: a copy */
	{
		DWORD two = 2;

		CHECK(2 == wf_clip_parse_effect(&two, sizeof two));
		CHECK(0 == wf_clip_parse_effect(&two, 3)); /* too short */
		CHECK(0 == wf_clip_parse_effect(NULL, 4));
	}
}

static void names_are_read_wide_and_narrow(void)
{
	const wchar_t *two[] = {L"C:\\a b\\one.txt", L"C:\\a b\\two.txt"};
	const wchar_t *japanese[] = {
	        L"C:\\\x30c4\x30fc\x30eb\\\x65e5\x672c\x8a9e.txt"};
	WfClipFiles f;
	size_t n;

	n = build(block, sizeof block, true, two, 2);
	CHECK(WF_CLIP_OK == parse(block, n, &f) && 2 == f.n);
	CHECK(!wcscmp(f.path[0], two[0]) && !wcscmp(f.path[1], two[1]));
	wf_clipfiles_free(&f);
	CHECK(!f.path && 0 == f.n);
	n = build(block, sizeof block, false, two, 2);
	CHECK(WF_CLIP_OK == parse(block, n, &f) && 2 == f.n &&
	        !wcscmp(f.path[1], two[1]));
	wf_clipfiles_free(&f);
	n = build(block, sizeof block, true, japanese, 1);
	CHECK(WF_CLIP_OK == parse(block, n, &f) && 1 == f.n &&
	        !wcscmp(f.path[0], japanese[0]));
	wf_clipfiles_free(&f);
	/* a share is a path too (the application decides whether to take it) */
	{
		const wchar_t *share[] = {L"\\\\server\\share\\x.txt"};

		n = build(block, sizeof block, true, share, 1);
		CHECK(WF_CLIP_OK == parse(block, n, &f) && 1 == f.n);
		wf_clipfiles_free(&f);
	}
}

static void a_damaged_or_hostile_block_gives_no_path_at_all(void)
{
	const wchar_t *one[] = {L"C:\\a\\one.txt"};
	WfClipFiles f;
	size_t n = build(block, sizeof block, true, one, 1);
	DROPFILES h;

	CHECK(WF_CLIP_BAD == parse(NULL, 0, &f) && !f.path);
	CHECK(WF_CLIP_BAD == parse(block, 0, &f));
	CHECK(WF_CLIP_BAD ==
	        parse(block, 19, &f)); /* shorter than the header */
	/* the offset of the names: before the end of the header, past the block */
	memcpy(&h, block, sizeof h);
	h.pFiles = 8;
	memcpy(block, &h, sizeof h);
	CHECK(WF_CLIP_BAD == parse(block, n, &f) && !f.path);
	h.pFiles = (DWORD)n + 1;
	memcpy(block, &h, sizeof h);
	CHECK(WF_CLIP_BAD == parse(block, n, &f));
	h.pFiles = 0xFFFFFFF0u;
	memcpy(block, &h, sizeof h);
	CHECK(WF_CLIP_BAD == parse(block, n, &f));
	h.pFiles = (DWORD)n; /* names start at the very end: no terminator */
	memcpy(block, &h, sizeof h);
	CHECK(WF_CLIP_BAD == parse(block, n, &f));
	/* the list does not end with an empty name */
	n = build(block, sizeof block, true, one, 1);
	CHECK(WF_CLIP_BAD == parse(block, n - sizeof(wchar_t), &f) && !f.path);
	CHECK(WF_CLIP_BAD == parse(block, n - 2 * sizeof(wchar_t), &f));
	/* a name cut in the middle (an odd number of bytes) */
	CHECK(WF_CLIP_BAD == parse(block, n - 1, &f));
}

static void a_list_with_nothing_or_nothing_acceptable_is_refused(void)
{
	WfClipFiles f;
	size_t n;
	static const wchar_t *const bad[] = {L"relative\\a.txt", L"a.txt",
	        L"C:", L"C:a.txt", L"C:\\a\\..\\b.txt", L"C:\\a\\.\\b.txt",
	        L"\\\\?\\C:\\a.txt", L"\\\\.\\COM1", L"\\\\",
	        L"C:\\a\x01\x62.txt", /* a control character in a name */
	        L"C:/a/b.txt", L""};

	n = build(block, sizeof block, true, NULL, 0);
	CHECK(WF_CLIP_NONE == parse(block, n, &f)); /* a list with no name */
	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
		const wchar_t *one[1];

		one[0] = bad[i];
		if (!*bad[i]) {
			continue; /* an empty name is the terminator, not a name */
		}
		n = build(block, sizeof block, true, one, 1);
		CHECK(WF_CLIP_BAD == parse(block, n, &f) && !f.path);
	}
	/* one bad name among good ones refuses the lot */
	{
		const wchar_t *mixed[] = {
		        L"C:\\a.txt", L"..\\b.txt", L"C:\\c.txt"};

		n = build(block, sizeof block, true, mixed, 3);
		CHECK(WF_CLIP_BAD == parse(block, n, &f) && !f.path &&
		        0 == f.n);
	}
	/* a lone surrogate is not text */
	{
		wchar_t odd[] = L"C:\\ok\xD800.txt";
		const wchar_t *one[] = {odd};

		n = build(block, sizeof block, true, one, 1);
		CHECK(WF_CLIP_BAD == parse(block, n, &f));
	}
}

static void the_limits_are_exact(void)
{
	WfClipFiles f;
	size_t n;
	static wchar_t longest[WF_CLIPFILES_PATH + 8];
	const wchar_t *one[1];
	static const wchar_t *many[WF_CLIPFILES_MAX + 1];
	static wchar_t names[WF_CLIPFILES_MAX + 1][16];

	/* 4096 units is the longest path; one more is refused */
	wcscpy_s(longest, WF_CLIPFILES_PATH + 8, L"C:\\");
	for (size_t i = 3; i < WF_CLIPFILES_PATH; i++) {
		longest[i] = L'a';
	}
	longest[WF_CLIPFILES_PATH] = L'\0';
	one[0] = longest;
	n = build(block, sizeof block, true, one, 1);
	CHECK(WF_CLIP_OK == parse(block, n, &f) && 1 == f.n);
	wf_clipfiles_free(&f);
	longest[WF_CLIPFILES_PATH] = L'a';
	longest[WF_CLIPFILES_PATH + 1] = L'\0';
	n = build(block, sizeof block, true, one, 1);
	CHECK(WF_CLIP_BAD == parse(block, n, &f) && !f.path);
	/* 1000 files is the most; the 1001st is refused whole */
	for (size_t i = 0; i <= WF_CLIPFILES_MAX; i++) {
		swprintf(names[i], 16, L"C:\\f%zu", i);
		many[i] = names[i];
	}
	n = build(block, sizeof block, true, many, WF_CLIPFILES_MAX);
	CHECK(WF_CLIP_OK == parse(block, n, &f) && WF_CLIPFILES_MAX == f.n);
	wf_clipfiles_free(&f);
	n = build(block, sizeof block, true, many, WF_CLIPFILES_MAX + 1);
	CHECK(WF_CLIP_TOO_MANY == parse(block, n, &f) && !f.path && 0 == f.n);
}

static HWND g_owner;
static bool open_clipboard(HWND owner);

/* The clipboard can be open in another program (or still be closed by the one that
 * just read it): opening it is tried for a short while, as any program must. */
static bool open_clipboard(HWND owner)
{
	for (int i = 0; i < 100; i++) {
		if (OpenClipboard(owner)) {
			return true;
		}
		Sleep(20);
	}
	return false;
}

static bool put_block(UINT format, const void *data, size_t bytes)
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

/* The clipboard now holds `data` as CF_HDROP and, when given, `effect`. */
static bool set_files(const void *data, size_t bytes, const DWORD *effect)
{
	bool ok = false;

	if (!open_clipboard(g_owner)) {
		return false;
	}
	EmptyClipboard();
	ok = put_block(CF_HDROP, data, bytes);
	if (ok && effect) {
		ok = put_block(
		        RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT),
		        effect, sizeof *effect);
	}
	CloseClipboard();
	return ok;
}

static bool set_text_only(const wchar_t *text)
{
	bool ok;

	if (!open_clipboard(g_owner)) {
		return false;
	}
	EmptyClipboard();
	ok = put_block(
	        CF_UNICODETEXT, text, (wcslen(text) + 1) * sizeof(wchar_t));
	CloseClipboard();
	return ok;
}

static bool set_empty(void)
{
	if (!open_clipboard(g_owner)) {
		return false;
	}
	EmptyClipboard();
	CloseClipboard();
	return true;
}

/* Asks the worker and waits for its answer (up to 15 s). */
static bool ask(WfClipWorker *w, uint32_t gen, WfClipFiles *out)
{
	WfClipFilesPort port = wf_clipworker_port(w);
	ULONGLONG until = GetTickCount64() + 15000;

	CHECK(WF_OK == port.read(port.ctx, gen));
	while (GetTickCount64() < until) {
		if (wf_clipworker_poll(w, out)) {
			return true;
		}
		WaitForSingleObject(wf_clipworker_event(w), 100);
	}
	return false;
}

static void the_worker_reads_files_the_way_explorer_puts_them(void)
{
	WfClipWorker *w;
	WfClipFiles f;
	const wchar_t *two[] = {
	        L"C:\\Pics\\a.png", L"C:\\Pics\\\x65e5\x672c.png"};
	size_t n = build(block, sizeof block, true, two, 2);
	DWORD copy_link = 5, move = 2;

	CHECK(WF_OK == wf_clipworker_start(&w));
	/* an Explorer copy: the files, the effect copy | link */
	CHECK(set_files(block, n, &copy_link));
	CHECK(ask(w, 1, &f));
	CHECK(1 == f.gen && WF_CLIP_OK == f.why && 2 == f.n && 5 == f.effect);
	CHECK(!wcscmp(f.path[0], two[0]) && !wcscmp(f.path[1], two[1]));
	CHECK(!wf_clip_effect_is_cut(f.effect));
	wf_clipfiles_free(&f);
	/* a cut says so */
	CHECK(set_files(block, n, &move));
	CHECK(ask(w, 2, &f) && 2 == f.gen && WF_CLIP_OK == f.why &&
	        wf_clip_effect_is_cut(f.effect));
	wf_clipfiles_free(&f);
	/* no effect given */
	CHECK(set_files(block, n, NULL));
	CHECK(ask(w, 3, &f) && WF_CLIP_OK == f.why && 0 == f.effect);
	wf_clipfiles_free(&f);
	/* narrow names (an older program) */
	n = build(block, sizeof block, false, two, 1);
	CHECK(set_files(block, n, NULL));
	CHECK(ask(w, 4, &f) && WF_CLIP_OK == f.why && 1 == f.n);
	wf_clipfiles_free(&f);
	wf_clipworker_stop(w);
}

static void what_is_not_files_or_is_damaged_is_refused_whole(void)
{
	WfClipWorker *w;
	WfClipFiles f;
	const wchar_t *one[] = {L"C:\\a.txt"};
	size_t n = build(block, sizeof block, true, one, 1);

	CHECK(WF_OK == wf_clipworker_start(&w));
	CHECK(set_text_only(L"just some text"));
	CHECK(ask(w, 1, &f) && WF_CLIP_NONE == f.why && !f.path);
	CHECK(set_empty());
	CHECK(ask(w, 2, &f) && WF_CLIP_NONE == f.why && !f.path);
	/* a block that claims to be a file list and is not one */
	{
		unsigned char junk[64];

		memset(junk, 0xFF, sizeof junk);
		CHECK(set_files(junk, sizeof junk, NULL));
		CHECK(ask(w, 3, &f) && WF_CLIP_BAD == f.why && !f.path);
		CHECK(set_files(junk, 3, NULL));
		CHECK(ask(w, 4, &f) && WF_CLIP_BAD == f.why && !f.path);
	}
	/* a list cut short */
	CHECK(set_files(block, n - 4, NULL));
	CHECK(ask(w, 5, &f) && WF_CLIP_BAD == f.why && !f.path);
	/* a block larger than any list the limits allow is refused before it is copied
	 * (its first names are right: it is the size that is wrong) */
	{
		size_t big_n = WF_CLIP_HDROP_MAX_BYTES + 4096;
		unsigned char *big = calloc(1, big_n);

		CHECK(NULL != big);
		if (big) {
			memcpy(big, block,
			        n - sizeof(wchar_t)); /* header and the name */
			CHECK(set_files(big, big_n, NULL));
			CHECK(ask(w, 6, &f) && WF_CLIP_BAD == f.why && !f.path);
			memset(big, 0,
			        big_n); /* and as a list of nothing at the limit */
			memcpy(big, block, n);
			CHECK(set_files(big, WF_CLIP_HDROP_MAX_BYTES, NULL));
			CHECK(ask(w, 7, &f) && WF_CLIP_OK == f.why && 1 == f.n);
			wf_clipfiles_free(&f);
			free(big);
		}
	}
	/* an effect block of any size gives its first four bytes */
	{
		size_t eff_n = (size_t)1 << 20;
		DWORD *eff = calloc(1, eff_n);

		CHECK(NULL != eff);
		if (eff && open_clipboard(g_owner)) {
			eff[0] = 2;
			EmptyClipboard();
			CHECK(put_block(CF_HDROP, block, n));
			CHECK(put_block(RegisterClipboardFormatW(
			                        CFSTR_PREFERREDDROPEFFECT),
			        eff, eff_n));
			CloseClipboard();
			CHECK(ask(w, 8, &f) && WF_CLIP_OK == f.why &&
			        2 == f.effect);
			wf_clipfiles_free(&f);
		}
		free(eff);
	}
	wf_clipworker_stop(w);
}

static void a_newer_request_wins_and_stopping_never_hangs(void)
{
	WfClipWorker *w;
	WfClipFiles f;
	WfClipFilesPort port;
	const wchar_t *one[] = {L"C:\\a.txt"};
	size_t n = build(block, sizeof block, true, one, 1);
	ULONGLONG until;
	bool got = false;
	WfWorkerStopReport r;

	CHECK(set_files(block, n, NULL));
	CHECK(WF_OK == wf_clipworker_start(&w));
	port = wf_clipworker_port(w);
	CHECK(WF_E_INVAL == port.read(port.ctx, 0));
	for (uint32_t g = 1; g <= 20; g++) {
		CHECK(WF_OK == port.read(port.ctx, g));
	}
	until = GetTickCount64() + 15000;
	while (GetTickCount64() < until && !got) {
		WaitForSingleObject(wf_clipworker_event(w), 100);
		/* answers may come for any request that was already running; the
		 * last one asked for is always among them, and nothing is lost to
		 * a stale one being taken as current */
		while (wf_clipworker_poll(w, &f)) {
			CHECK(f.gen >= 1 && f.gen <= 20);
			got = got || 20 == f.gen;
			wf_clipfiles_free(&f);
		}
	}
	CHECK(got);
	r = wf_clipworker_stop(w);
	CHECK(WF_OK == r.status);
	/* stopping at once after asking returns */
	CHECK(WF_OK == wf_clipworker_start(&w));
	port = wf_clipworker_port(w);
	CHECK(WF_OK == port.read(port.ctx, 1));
	r = wf_clipworker_stop(w);
	CHECK(WF_OK == r.status);
	if (r.completion) {
		WaitForSingleObject(r.completion, 5000);
		CloseHandle(r.completion);
	}
}

static void the_block_made_is_the_block_read(void)
{
	const wchar_t *path[] = {L"C:\\a b\\one.txt",
	        L"C:\\\x30c4\x30fc\x30eb\\\x65e5\x672c\x8a9e.txt",
	        L"\\\\server\\share\\x.txt"};
	void *blk;
	size_t bytes;
	WfClipFiles f;

	CHECK(WF_OK == wf_clip_build_hdrop(path, 3, &blk, &bytes));
	CHECK(WF_CLIP_OK == wf_clip_parse_hdrop(blk, bytes, &f) && 3 == f.n);
	for (size_t i = 0; i < 3; i++) {
		CHECK(!wcscmp(f.path[i], path[i]));
	}
	wf_clipfiles_free(&f);
	free(blk);
	/* what this program would refuse to read back it does not publish */
	{
		const wchar_t *bad[] = {L"relative.txt"},
		              *dots[] = {L"C:\\a\\..\\b"},
		              *surrogate[] = {L"C:\\x\xD800"};

		CHECK(WF_E_RANGE == wf_clip_build_hdrop(bad, 1, &blk, &bytes) &&
		        !blk);
		CHECK(WF_E_RANGE == wf_clip_build_hdrop(dots, 1, &blk, &bytes));
		CHECK(WF_E_RANGE ==
		        wf_clip_build_hdrop(surrogate, 1, &blk, &bytes));
		CHECK(WF_E_RANGE == wf_clip_build_hdrop(path, 0, &blk, &bytes));
	}
	{
		static const wchar_t *many[WF_CLIPFILES_MAX + 1];

		for (size_t i = 0; i <= WF_CLIPFILES_MAX; i++) {
			many[i] = path[0];
		}
		CHECK(WF_OK ==
		        wf_clip_build_hdrop(
		                many, WF_CLIPFILES_MAX, &blk, &bytes));
		free(blk);
		CHECK(WF_E_RANGE ==
		        wf_clip_build_hdrop(
		                many, WF_CLIPFILES_MAX + 1, &blk, &bytes));
	}
}

/* The data object as a receiver uses it. */
static void the_data_object_answers_as_a_receiver_expects(void)
{
	const wchar_t *path[] = {
	        L"C:\\Pics\\a.png", L"C:\\Pics\\\x65e5\x672c.png"};
	IDataObject *pdo = NULL, *other = NULL;
	FORMATETC hdrop = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
	FORMATETC effect = {0, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
	FORMATETC text = {
	        CF_UNICODETEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
	STGMEDIUM m;
	FORMATETC canon;
	IEnumFORMATETC *en = NULL;
	FORMATETC got[3];
	ULONG fetched = 0;
	WfClipFiles f;
	DWORD *eff;
	void *p;

	effect.cfFormat =
	        (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
	CHECK(0 == wf_dataobj_files_live());
	CHECK(WF_OK == wf_dataobj_files_create(path, 2, &pdo) && pdo);
	CHECK(1 == wf_dataobj_files_live());
	/* what is on offer, and what is not */
	CHECK(S_OK == IDataObject_QueryGetData(pdo, &hdrop));
	CHECK(S_OK == IDataObject_QueryGetData(pdo, &effect));
	CHECK(DV_E_FORMATETC == IDataObject_QueryGetData(pdo, &text));
	{
		FORMATETC bad = hdrop;

		bad.tymed = TYMED_ISTREAM;
		CHECK(DV_E_TYMED == IDataObject_QueryGetData(pdo, &bad));
		bad = hdrop;
		bad.lindex = 0;
		CHECK(DV_E_LINDEX == IDataObject_QueryGetData(pdo, &bad));
		bad = hdrop;
		bad.dwAspect = DVASPECT_ICON;
		CHECK(DV_E_DVASPECT == IDataObject_QueryGetData(pdo, &bad));
		CHECK(E_INVALIDARG == IDataObject_QueryGetData(pdo, NULL));
	}
	/* the files, as a receiver reads them */
	CHECK(S_OK == IDataObject_GetData(pdo, &hdrop, &m));
	CHECK(TYMED_HGLOBAL == m.tymed && NULL == m.pUnkForRelease);
	p = GlobalLock(m.hGlobal);
	CHECK(p &&
	        WF_CLIP_OK ==
	                wf_clip_parse_hdrop(p, GlobalSize(m.hGlobal), &f) &&
	        2 == f.n && !wcscmp(f.path[0], path[0]) &&
	        !wcscmp(f.path[1], path[1]));
	wf_clipfiles_free(&f);
	/* the receiver may scribble on its copy: the next one is fresh */
	memset(p, 0, 8);
	GlobalUnlock(m.hGlobal);
	ReleaseStgMedium(&m);
	CHECK(S_OK == IDataObject_GetData(pdo, &hdrop, &m));
	p = GlobalLock(m.hGlobal);
	CHECK(WF_CLIP_OK == wf_clip_parse_hdrop(p, GlobalSize(m.hGlobal), &f));
	wf_clipfiles_free(&f);
	GlobalUnlock(m.hGlobal);
	ReleaseStgMedium(&m);
	/* the effect: a copy, and only a copy */
	CHECK(S_OK == IDataObject_GetData(pdo, &effect, &m));
	eff = GlobalLock(m.hGlobal);
	CHECK(eff && DROPEFFECT_COPY == *eff && !wf_clip_effect_is_cut(*eff));
	GlobalUnlock(m.hGlobal);
	ReleaseStgMedium(&m);
	CHECK(DV_E_FORMATETC == IDataObject_GetData(pdo, &text, &m) &&
	        0 == m.tymed);
	CHECK(E_NOTIMPL == IDataObject_GetDataHere(pdo, &hdrop, &m));
	/* a receiver writes back "Performed DropEffect": nothing is accepted or done */
	{
		STGMEDIUM w;
		FORMATETC performed = effect;
		DWORD two = DROPEFFECT_MOVE;
		HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof two);

		performed.cfFormat = (CLIPFORMAT)RegisterClipboardFormatW(
		        CFSTR_PERFORMEDDROPEFFECT);
		memcpy(GlobalLock(g), &two, sizeof two);
		GlobalUnlock(g);
		memset(&w, 0, sizeof w);
		w.tymed = TYMED_HGLOBAL;
		w.hGlobal = g;
		CHECK(E_NOTIMPL ==
		        IDataObject_SetData(pdo, &performed, &w, TRUE));
		GlobalFree(
		        g); /* failed SetData leaves ownership with the caller */
		CHECK(DV_E_FORMATETC ==
		        IDataObject_QueryGetData(pdo, &performed));
	}
	/* the formats can be listed, in two steps, and the list can be cloned */
	CHECK(S_OK == IDataObject_EnumFormatEtc(pdo, DATADIR_GET, &en) && en);
	CHECK(S_FALSE == IEnumFORMATETC_Next(en, 3, got, &fetched));
	CHECK(2 == fetched && CF_HDROP == got[0].cfFormat &&
	        effect.cfFormat == got[1].cfFormat);
	IEnumFORMATETC_Release(en);
	CHECK(E_NOTIMPL == IDataObject_EnumFormatEtc(pdo, DATADIR_SET, &en) &&
	        !en);
	CHECK(DATA_S_SAMEFORMATETC ==
	        IDataObject_GetCanonicalFormatEtc(pdo, &hdrop, &canon));
	{
		DWORD conn = 5;

		CHECK(OLE_E_ADVISENOTSUPPORTED ==
		                IDataObject_DAdvise(
		                        pdo, &hdrop, 0, NULL, &conn) &&
		        0 == conn);
		CHECK(OLE_E_ADVISENOTSUPPORTED ==
		        IDataObject_DUnadvise(pdo, 1));
		CHECK(OLE_E_ADVISENOTSUPPORTED ==
		        IDataObject_EnumDAdvise(pdo, NULL));
	}
	/* the interface and the count */
	CHECK(S_OK ==
	        IDataObject_QueryInterface(
	                pdo, &IID_IUnknown, (void **)&other));
	CHECK(other == pdo);
	IDataObject_Release(other);
	CHECK(E_NOINTERFACE ==
	        IDataObject_QueryInterface(
	                pdo, &IID_IDropTarget, (void **)&other));
	CHECK(!other);
	CHECK(2 == IDataObject_AddRef(pdo));
	CHECK(1 == IDataObject_Release(pdo));
	CHECK(1 == wf_dataobj_files_live());
	CHECK(0 == IDataObject_Release(pdo));
	CHECK(0 == wf_dataobj_files_live()); /* nothing is kept */
	{
		const wchar_t *bad[] = {L"relative"};

		CHECK(WF_E_RANGE == wf_dataobj_files_create(bad, 1, &pdo) &&
		        !pdo);
		CHECK(0 == wf_dataobj_files_live());
	}
}

/* Asks the worker to publish and waits for the answer. */
static bool publish_and_wait(WfClipWorker *w,
        uint32_t gen,
        const wchar_t *const *path,
        size_t n,
        WfClipFiles *out)
{
	WfClipFilesPort port = wf_clipworker_port(w);
	ULONGLONG until = GetTickCount64() + 15000;

	CHECK(WF_OK == port.publish(port.ctx, gen, path, n));
	while (GetTickCount64() < until) {
		if (wf_clipworker_poll(w, out)) {
			return true;
		}
		WaitForSingleObject(wf_clipworker_event(w), 100);
	}
	return false;
}

/* A receiver is an application: it serves its messages, which is how OLE learns
 * that the clipboard changed. */
static void serve_messages(DWORD ms)
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

/* What a Shell compatible receiver does: OleGetClipboard, then the object's formats. */
static bool receiver_reads(WfClipFiles *out, DWORD *effect)
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
	serve_messages(150);
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

		*effect = p ? *p : 0;
		if (p) {
			GlobalUnlock(m.hGlobal);
		}
		ReleaseStgMedium(&m);
	}
	IDataObject_Release(pdo);
	return ok;
}

static void piero_puts_files_where_a_receiver_finds_them(void)
{
	WfClipWorker *w;
	WfClipFiles f;
	const wchar_t *path[] = {
	        L"C:\\Pics\\a.png", L"C:\\Pics\\\x65e5\x672c.png"};
	DWORD effect = 0;
	LONG settled;

	CHECK(WF_OK == wf_clipworker_start(&w));
	CHECK(wf_clipworker_idle(w)); /* just made: nothing to lose */
	CHECK(publish_and_wait(w, 7, path, 2, &f));
	CHECK(7 == f.gen && f.published && WF_CLIP_OK == f.why && 2 == f.n);
	wf_clipfiles_free(&f);
	/* the answer is taken, and the worker is still not idle: what it put on
	 * the clipboard is its own until the end, so the loop does not stop it */
	CHECK(!wf_clipworker_idle(w));
	/* the receiver, through OLE, as Explorer does */
	CHECK(receiver_reads(&f, &effect));
	CHECK(2 == f.n && !wcscmp(f.path[0], path[0]) &&
	        !wcscmp(f.path[1], path[1]));
	CHECK(DROPEFFECT_COPY == effect);
	wf_clipfiles_free(&f);
	/* and any program that reads the clipboard the classic way (the worker's own read) */
	{
		WfClipFilesPort port = wf_clipworker_port(w);
		ULONGLONG until = GetTickCount64() + 15000;
		bool got = false;

		CHECK(WF_OK == port.read(port.ctx, 8));
		while (!got && GetTickCount64() < until) {
			got = wf_clipworker_poll(w, &f);
			if (!got) {
				WaitForSingleObject(
				        wf_clipworker_event(w), 100);
			}
		}
		CHECK(got && 8 == f.gen && !f.published &&
		        WF_CLIP_OK == f.why && 2 == f.n);
		wf_clipfiles_free(&f);
	}
	/* publishing again replaces it, and the first object is let go */
	{
		const wchar_t *one[] = {L"C:\\Pics\\b.png"};

		CHECK(publish_and_wait(w, 9, one, 1, &f) &&
		        WF_CLIP_OK == f.why);
		wf_clipfiles_free(&f);
		CHECK(receiver_reads(&f, &effect) && 1 == f.n &&
		        !wcscmp(f.path[0], one[0]));
		wf_clipfiles_free(&f);
	}
	/* a name this program would not read back is refused, and the clipboard is as it was */
	{
		const wchar_t *bad[] = {L"relative.txt"};

		CHECK(publish_and_wait(w, 10, bad, 1, &f) && f.published &&
		        WF_CLIP_BAD == f.why);
		wf_clipfiles_free(&f);
		CHECK(receiver_reads(&f, &effect) && 1 == f.n);
		wf_clipfiles_free(&f);
	}
	/* when piero ends, the Shell is given the data: a paste after that still works */
	{
		WfWorkerStopReport r = wf_clipworker_stop(w);

		CHECK(WF_OK == r.status);
		if (r.completion) {
			WaitForSingleObject(r.completion, 5000);
			CloseHandle(r.completion);
		}
	}
	CHECK(receiver_reads(&f, &effect) && 1 == f.n &&
	        DROPEFFECT_COPY == effect);
	wf_clipfiles_free(&f);
	/* nothing of piero's objects is left alive once the data has been handed over */
	for (int i = 0; i < 100 && (settled = wf_dataobj_files_live()) != 0;
	        i++) {
		Sleep(20);
	}
	CHECK(0 == wf_dataobj_files_live());
	(void)settled;
}

/* If another program has put something else on the clipboard by then, piero ends
 * without touching it. */
static void another_programs_clipboard_is_never_overwritten_at_the_end(void)
{
	WfClipWorker *w;
	WfClipFiles f;
	const wchar_t *path[] = {L"C:\\Pics\\a.png"};

	CHECK(WF_OK == wf_clipworker_start(&w));
	CHECK(publish_and_wait(w, 1, path, 1, &f) && WF_CLIP_OK == f.why);
	wf_clipfiles_free(&f);
	CHECK(set_text_only(L"the other program's text"));
	{
		WfWorkerStopReport r = wf_clipworker_stop(w);

		if (r.completion) {
			WaitForSingleObject(r.completion, 5000);
			CloseHandle(r.completion);
		}
	}
	/* still the other program's text, and no files */
	{
		WfClipWorker *reader;
		bool got = false;
		ULONGLONG until = GetTickCount64() + 15000;
		WfClipFilesPort port;

		CHECK(WF_OK == wf_clipworker_start(&reader));
		port = wf_clipworker_port(reader);
		CHECK(WF_OK == port.read(port.ctx, 1));
		while (!got && GetTickCount64() < until) {
			got = wf_clipworker_poll(reader, &f);
			if (!got) {
				WaitForSingleObject(
				        wf_clipworker_event(reader), 100);
			}
		}
		CHECK(got && WF_CLIP_NONE == f.why);
		wf_clipworker_stop(reader);
	}
	CHECK(0 == wf_dataobj_files_live());
}

int main(int argc, char **argv)
{
	bool pure = argc == 2 && !strcmp(argv[1], "--pure");
	wchar_t desktop[64];
	DWORD error = 0;
#ifdef WF_CLIPBOARD_PURE_TEST
	pure = true;
#endif
	if (argc > 2 || (argc == 2 && !pure)) {
		return 2;
	}
	the_effect_decides_copy_or_cut();
	names_are_read_wide_and_narrow();
	a_damaged_or_hostile_block_gives_no_path_at_all();
	a_list_with_nothing_or_nothing_acceptable_is_refused();
	the_limits_are_exact();
	the_block_made_is_the_block_read();
	if (pure) {
		the_data_object_answers_as_a_receiver_expects();
		return TEST_RESULT();
	}
	if (wf_test_failures) {
		return TEST_RESULT();
	}
	if (!p0_clipboard_fixture(desktop, 64, &error)) {
		printf("SKIP clipboard: private station unavailable, "
		       "error=%lu\n",
		        error);
		return 77;
	}
	the_data_object_answers_as_a_receiver_expects();
	g_owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
	        HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
	if (!g_owner) {
		printf("SKIP clipboard: no window could be made\n");
		return wf_test_failures ? 1 : 77;
	}
	HRESULT initialized = OleInitialize(NULL);
	CHECK(SUCCEEDED(initialized));
	if (FAILED(initialized)) {
		DestroyWindow(g_owner);
		return TEST_RESULT();
	}
	the_worker_reads_files_the_way_explorer_puts_them();
	what_is_not_files_or_is_damaged_is_refused_whole();
	a_newer_request_wins_and_stopping_never_hangs();
	piero_puts_files_where_a_receiver_finds_them();
	another_programs_clipboard_is_never_overwritten_at_the_end();
	OleUninitialize();
	DestroyWindow(g_owner);
	return TEST_RESULT();
}

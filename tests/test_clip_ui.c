/* Files from the Windows clipboard (V) in the application (DR-DRAFT-021): what is
 * asked of the clipboard worker, every way an answer is refused (and that a
 * refusal touches nothing), and that an accepted one goes through the ordinary
 * capture and the ordinary paste (preflight, then the copy job as `p` starts it). The worker, the capture and the file system are
 * fakes, so no disk and no real clipboard is touched. */
#include <stdlib.h>
#include <string.h>

#include "wf_fake_fs.h"
#include "wf_fake_xfer.h"
#include "wf_test.h"

static FakeProbe P;

typedef struct
{
	unsigned reads, published;
	uint32_t gen;
	WfStatus answer;
	size_t pub_n;
	wchar_t pub_path[8][260];
} FakeClip;

static WfStatus fc_read(void *ctx, uint32_t gen)
{
	FakeClip *c = ctx;

	c->reads++;
	c->gen = gen;
	return c->answer;
}

static WfStatus fc_publish(
        void *ctx, uint32_t gen, const wchar_t *const *path, size_t n)
{
	FakeClip *c = ctx;

	c->published++;
	c->gen = gen;
	c->pub_n = n;
	for (size_t i = 0; i < n && i < 8; i++) {
		wcsncpy_s(c->pub_path[i], 260, path[i], _TRUNCATE);
	}
	return c->answer;
}

typedef struct
{
	WfApp app;
	FakePort fs;
	FakeXfer xf;
	FakeExec ex;
	FakeClip clip;
} Rig;

static void ch(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	a->mode->on_event(a, &e);
}

/* The app browses C:\Users\dev (the fake listing); the sources are in C:\Pics. */
static void world(void)
{
	fp_init(&P);
	fp_dir(&P, L"C:\\Users", 2);
	fp_dir(&P, L"C:\\Users\\dev", 3);
	fp_dir(&P, L"C:\\Pics", 4);
	fp_file(&P, L"C:\\Pics\\a.png", 20, 5);
	fp_file(&P, L"C:\\Pics\\b.png", 30, 6);
	fp_dir(&P, L"C:\\Other", 7);
	fp_file(&P, L"C:\\Other\\c.txt", 10, 8);
}

static void rig_open(Rig *r)
{
	memset(r, 0, sizeof *r);
	r->fs.app = &r->app;
	r->xf.app = &r->app;
	r->xf.ops = fp_ops(&P);
	CHECK(WF_OK ==
	        wf_app_init(&r->app, 120, 30, fake_port(&r->fs),
	                L"C:\\Users\\dev"));
	wf_app_set_xfer(&r->app, fx_port(&r->xf));
	wf_app_set_exec(&r->app, fe_port(&r->ex));
	wf_app_set_clip_files(
	        &r->app, (WfClipFilesPort){&r->clip, fc_read, fc_publish});
}

static WfClipFiles answer(const Rig *r,
        WfClipReason why,
        DWORD effect,
        const wchar_t *const *paths,
        size_t n)
{
	WfClipFiles f;

	memset(&f, 0, sizeof f);
	f.gen = r->clip.gen;
	f.why = why;
	f.effect = effect;
	if (n) {
		f.path = calloc(n, sizeof *f.path);
		for (size_t i = 0; i < n; i++) {
			f.path[i] = _wcsdup(paths[i]);
		}
		f.n = n;
	}
	return f;
}

/* V, then the worker's answer (the answer's memory is taken by the app). */
static void paste_with(Rig *r,
        WfClipReason why,
        DWORD effect,
        const wchar_t *const *paths,
        size_t n)
{
	WfClipFiles f;

	ch(&r->app, 'V');
	f = answer(r, why, effect, paths, n);
	wf_app_on_clip_files(&r->app, &f);
}

static bool said(const Rig *r, const char *text)
{
	return NULL != strstr(r->app.msg, text);
}

static void files_of_one_folder_go_through_the_ordinary_paste(void)
{
	static Rig r;
	const wchar_t *two[] = {L"C:\\Pics\\a.png", L"C:\\Pics\\b.png"};

	world();
	rig_open(&r);
	ch(&r.app, 'V');
	CHECK(1 == r.clip.reads && said(&r, "Reading"));
	CHECK(r.app.clip_busy && 0 == r.xf.n_capture);
	{
		/* Explorer's copy is "copy | link" (5), not a cut */
		WfClipFiles f = answer(&r, WF_CLIP_OK, 5, two, 2);

		wf_app_on_clip_files(&r.app, &f);
	}
	CHECK(!r.app.clip_busy);
	/* captured as `y` captures, then the paste goes on as `p` does */
	CHECK(1 == r.xf.n_capture && WF_XFER_COPY == r.app.slot.mode &&
	        2 == r.app.slot.count);
	CHECK(1 == r.xf.n_preflight);
	/* an ordinary copy with no name in the way starts its job (as `p` does):
	 * the two sources, into the folder on screen */
	CHECK(1 == r.ex.calls && 2 == r.ex.count);
	CHECK(!wcscmp(r.ex.dest, L"C:\\Users\\dev"));
	CHECK(!r.app.paste_after_capture);
	wf_app_destroy(&r.app);
}

static void a_cut_is_refused_and_a_clipboard_without_an_effect_is_a_copy(void)
{
	static Rig r;
	const wchar_t *one[] = {L"C:\\Pics\\a.png"};

	world();
	rig_open(&r);
	paste_with(&r, WF_CLIP_OK, 2, one, 1); /* move only: a cut */
	CHECK(said(&r, "cut") && 0 == r.xf.n_capture &&
	        WF_XFER_NONE == r.app.slot.mode);
	CHECK(&WF_MODE_NORMAL == r.app.mode && 0 == r.ex.calls);
	paste_with(&r, WF_CLIP_OK, 0, one, 1); /* no effect given: a copy */
	CHECK(1 == r.xf.n_capture && 1 == r.xf.n_preflight);
	wf_app_destroy(&r.app);
}

static void every_refusal_says_why_and_does_nothing(void)
{
	static const struct
	{
		WfClipReason why;
		const char *word;
	} cases[] = {{WF_CLIP_NONE, "no files"},
	        {WF_CLIP_UNSUPPORTED, "not real files"},
	        {WF_CLIP_BAD, "could not be read safely"},
	        {WF_CLIP_BUSY, "is using the clipboard"},
	        {WF_CLIP_FAILED, "could not be read"},
	        {WF_CLIP_TOO_MANY, "could not be read"}};
	static Rig r;

	for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
		world();
		rig_open(&r);
		paste_with(&r, cases[i].why, 0, NULL, 0);
		CHECK(said(&r, cases[i].word));
		CHECK(0 == r.xf.n_capture && 0 == r.xf.n_preflight &&
		        WF_XFER_NONE == r.app.slot.mode && 0 == r.ex.calls);
		CHECK(!r.app.clip_busy && &WF_MODE_NORMAL == r.app.mode);
		wf_app_destroy(&r.app);
	}
}

static void files_of_several_folders_or_a_share_are_refused(void)
{
	static Rig r;
	const wchar_t *mixed[] = {L"C:\\Pics\\a.png", L"C:\\Other\\c.txt"};
	const wchar_t *share[] = {L"\\\\server\\share\\a.txt"};
	const wchar_t *root[] = {L"C:\\"};

	world();
	rig_open(&r);
	paste_with(&r, WF_CLIP_OK, 1, mixed, 2);
	CHECK(said(&r, "several folders") && 0 == r.xf.n_capture);
	paste_with(&r, WF_CLIP_OK, 1, share, 1);
	CHECK(said(&r, "network share") && 0 == r.xf.n_capture);
	paste_with(
	        &r, WF_CLIP_OK, 1, root, 1); /* a drive is not a file to copy */
	CHECK(0 == r.xf.n_capture && WF_XFER_NONE == r.app.slot.mode);
	CHECK(0 == r.ex.calls && &WF_MODE_NORMAL == r.app.mode);
	wf_app_destroy(&r.app);
}

static void a_name_listed_twice_is_asked_for_once(void)
{
	static Rig r;
	const wchar_t *twice[] = {L"C:\\Pics\\a.png", L"C:\\Pics\\a.png"};
	/* the folder in another case is not taken for the same folder: in a folder
	 * that tells case apart it is another one, and its file is not the first
	 * folder's */
	const wchar_t *folder_case[] = {L"C:\\Pics\\a.png", L"c:\\pics\\a.png"};

	world();
	rig_open(&r);
	paste_with(&r, WF_CLIP_OK, 1, twice, 2);
	CHECK(1 == r.xf.n_capture && 1 == r.app.slot.count);
	wf_app_destroy(&r.app);
	world();
	rig_open(&r);
	paste_with(&r, WF_CLIP_OK, 1, folder_case, 2);
	CHECK(0 == r.xf.n_capture && said(&r, "several folders"));
	wf_app_destroy(&r.app);
}

/* foo and Foo are two names (in a folder that tells case apart, two files):
 * both are captured. The fake probe knows a.png only, so the capture of the
 * second stops: what matters is that it was asked for. */
static void names_that_differ_only_by_case_are_two_names(void)
{
	static Rig r;
	const wchar_t *two[] = {L"C:\\Pics\\a.png", L"C:\\Pics\\A.png"};

	world();
	rig_open(&r);
	paste_with(&r, WF_CLIP_OK, 1, two, 2);
	CHECK(1 == r.xf.n_capture);
	CHECK(2 == r.xf.last_n);
	wf_app_destroy(&r.app);
}

static void a_source_that_cannot_be_captured_stops_everything(void)
{
	static Rig r;
	const wchar_t *gone[] = {L"C:\\Pics\\gone.png"}; /* not in the probe */

	world();
	rig_open(&r);
	paste_with(&r, WF_CLIP_OK, 1, gone, 1);
	CHECK(1 == r.xf.n_capture && 0 == r.xf.n_preflight);
	CHECK(WF_XFER_NONE == r.app.slot.mode && !r.app.paste_after_capture);
	CHECK(said(&r, "capture") || said(&r, "read") || r.app.msg[0]);
	CHECK(0 == r.ex.calls && &WF_MODE_NORMAL == r.app.mode);
	wf_app_destroy(&r.app);
}

static void a_stale_answer_is_dropped_and_a_second_request_waits(void)
{
	static Rig r;
	const wchar_t *one[] = {L"C:\\Pics\\a.png"};
	WfClipFiles f;

	world();
	rig_open(&r);
	ch(&r.app, 'V');
	ch(&r.app, 'V'); /* while the first is out */
	CHECK(1 == r.clip.reads &&
	        said(&r, "Checking") + said(&r, "busy") + said(&r, "wait") >=
	                0);
	f = answer(&r, WF_CLIP_OK, 1, one, 1);
	f.gen = r.clip.gen - 1; /* an answer of an older request */
	wf_app_on_clip_files(&r.app, &f);
	CHECK(0 == r.xf.n_capture &&
	        r.app.clip_busy); /* still waiting for its own */
	f = answer(&r, WF_CLIP_OK, 1, one, 1);
	wf_app_on_clip_files(&r.app, &f);
	CHECK(1 == r.xf.n_capture);
	/* an answer that nobody asked for */
	f = answer(&r, WF_CLIP_OK, 1, one, 1);
	wf_app_on_clip_files(&r.app, &f);
	CHECK(1 == r.xf.n_capture);
	wf_app_destroy(&r.app);
}

static void without_a_clipboard_service_it_says_so(void)
{
	static Rig r;

	world();
	rig_open(&r);
	wf_app_set_clip_files(&r.app, (WfClipFilesPort){0});
	ch(&r.app, 'V');
	CHECK(said(&r, "cannot be read in this session") && !r.app.clip_busy);
	wf_app_set_clip_files(
	        &r.app, (WfClipFilesPort){&r.clip, fc_read, fc_publish});
	r.clip.answer = WF_E_OS;
	ch(&r.app, 'V');
	CHECK(said(&r, "could not be read") && !r.app.clip_busy);
	wf_app_destroy(&r.app);
}

static void y_publishes_the_selection_as_absolute_names(void)
{
	static Rig r;
	WfClipFiles ok;

	world();
	rig_open(&r);
	/* the listing is Docs/ file2.txt file10.txt; mark the two files */
	ch(&r.app, 'j');
	ch(&r.app, ' '); /* file2.txt, the cursor goes on */
	ch(&r.app, ' '); /* file10.txt */
	ch(&r.app, 'Y');
	CHECK(1 == r.clip.published && 2 == r.clip.pub_n && r.app.clip_busy);
	CHECK(!wcscmp(r.clip.pub_path[0], L"C:\\Users\\dev\\file2.txt"));
	CHECK(!wcscmp(r.clip.pub_path[1], L"C:\\Users\\dev\\file10.txt"));
	/* nothing is read, copied or captured by it */
	CHECK(0 == r.clip.reads && 0 == r.xf.n_capture && 0 == r.ex.calls);
	memset(&ok, 0, sizeof ok);
	ok.gen = r.clip.gen;
	ok.published = true;
	ok.why = WF_CLIP_OK;
	ok.n = 2;
	wf_app_on_clip_files(&r.app, &ok);
	CHECK(!r.app.clip_busy &&
	        said(&r, "Copied to the Windows clipboard: 2"));
	wf_app_destroy(&r.app);
}

static void y_without_a_selection_takes_the_entry_under_the_cursor(void)
{
	static Rig r;

	world();
	rig_open(&r);
	ch(&r.app,
	        'Y'); /* the cursor is on Docs, a folder: a folder is copied too */
	CHECK(1 == r.clip.published && 1 == r.clip.pub_n);
	CHECK(!wcscmp(r.clip.pub_path[0], L"C:\\Users\\dev\\Docs"));
	wf_app_destroy(&r.app);
}

static void y_says_when_the_clipboard_could_not_be_written(void)
{
	static const struct
	{
		WfClipReason why;
		const char *word;
	} cases[] = {{WF_CLIP_BUSY, "is using the clipboard"},
	        {WF_CLIP_FAILED, "could not be put"},
	        {WF_CLIP_BAD, "could not be put"}};
	static Rig r;

	for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
		WfClipFiles f;

		world();
		rig_open(&r);
		ch(&r.app, 'Y');
		memset(&f, 0, sizeof f);
		f.gen = r.clip.gen;
		f.published = true;
		f.why = cases[i].why;
		wf_app_on_clip_files(&r.app, &f);
		CHECK(said(&r, cases[i].word) && !r.app.clip_busy);
		wf_app_destroy(&r.app);
	}
}

static void y_waits_while_a_request_is_out_and_refuses_what_it_cannot_do(void)
{
	static Rig r;
	WfClipFiles f;

	world();
	rig_open(&r);
	ch(&r.app, 'Y');
	ch(&r.app, 'Y'); /* one request is out */
	ch(&r.app, 'V');
	CHECK(1 == r.clip.published && 0 == r.clip.reads);
	/* an answer of an older request is dropped */
	memset(&f, 0, sizeof f);
	f.gen = r.clip.gen - 1;
	f.published = true;
	f.why = WF_CLIP_OK;
	wf_app_on_clip_files(&r.app, &f);
	CHECK(r.app.clip_busy);
	wf_app_destroy(&r.app);
	/* no service, or a service that says no */
	world();
	rig_open(&r);
	wf_app_set_clip_files(&r.app, (WfClipFilesPort){0});
	ch(&r.app, 'Y');
	CHECK(said(&r, "cannot be read in this session") && !r.app.clip_busy);
	wf_app_set_clip_files(
	        &r.app, (WfClipFilesPort){&r.clip, fc_read, fc_publish});
	r.clip.answer = WF_E_OS;
	ch(&r.app, 'Y');
	CHECK(said(&r, "could not be put") && !r.app.clip_busy);
	wf_app_destroy(&r.app);
}

int main(void)
{
	files_of_one_folder_go_through_the_ordinary_paste();
	a_cut_is_refused_and_a_clipboard_without_an_effect_is_a_copy();
	every_refusal_says_why_and_does_nothing();
	files_of_several_folders_or_a_share_are_refused();
	a_name_listed_twice_is_asked_for_once();
	names_that_differ_only_by_case_are_two_names();
	a_source_that_cannot_be_captured_stops_everything();
	a_stale_answer_is_dropped_and_a_second_request_waits();
	without_a_clipboard_service_it_says_so();
	y_publishes_the_selection_as_absolute_names();
	y_without_a_selection_takes_the_entry_under_the_cursor();
	y_says_when_the_clipboard_could_not_be_written();
	y_waits_while_a_request_is_out_and_refuses_what_it_cannot_do();
	return TEST_RESULT();
}

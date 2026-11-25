/* Open Terminal Here (`!`): the request handed to the launch worker names the
 * folder on screen, in the tab you are in, never the entry under the cursor; a
 * folder that is not an ordinary one of a local drive is not asked for; the
 * answer of the worker is told plainly; a held key asks once however long it is
 * held. The port is a spy: no terminal is started, and nothing here asks the
 * disk (the worker does that). */
#include <stdlib.h>
#include <string.h>

#include "wf_fake_fs.h"
#include "wf_test.h"

typedef struct
{
	unsigned calls;
	uint32_t gen;
	bool terminal;
	wchar_t dir[260];
	WfStatus answer; /* what submit() says */
} Spy;

static WfStatus spy_submit(void *ctx, uint32_t gen, const WfLaunchRequest *r)
{
	Spy *s = ctx;

	s->calls++;
	s->gen = gen;
	s->terminal = r->terminal;
	wcscpy_s(s->dir, 260, r->directory);
	return s->answer;
}

static void ch(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	(void)a->mode->on_event(a, &e);
}

typedef struct
{
	WfApp app;
	FakePort fs;
	Spy spy;
} Rig;

/* Browsing C:\Users\dev: Docs/ (the cursor), file2.txt, file10.txt. */
static void rig(Rig *r, bool with_port)
{
	memset(r, 0, sizeof *r);
	r->fs.app = &r->app;
	CHECK(wf_app_init(&r->app, 120, 30, fake_port(&r->fs),
	              L"C:\\Users\\dev") == WF_OK);
	if (with_port) {
		wf_app_set_terminal(
		        &r->app, (WfLaunchPort){&r->spy, spy_submit});
	}
	r->app.now_ms = 10000;
}

static const wchar_t *under(const WfApp *a)
{
	const WfEntry *e = wf_view_current(&a->cur);

	return e ? e->name : L"";
}

/* The worker's answer for the request in flight. */
static void answer(Rig *r, WfStatus st, unsigned long error)
{
	WfLaunchOutcome o = {st, error};

	wf_app_on_launch(&r->app, r->spy.gen, &o, r->spy.dir);
}

static void the_folder_on_screen_is_asked_for_not_the_entry_under_the_cursor(
        void)
{
	static Rig r;

	rig(&r, true);
	ch(&r.app, 'j'); /* the cursor is on a file now */
	CHECK(!wcscmp(under(&r.app), L"file2.txt"));
	ch(&r.app, '!');
	CHECK(1 == r.spy.calls && r.spy.terminal &&
	        !wcscmp(r.spy.dir, L"C:\\Users\\dev"));
	CHECK(r.app.launch_busy && strstr(r.app.msg, "Opening a terminal"));
	answer(&r, WF_OK, 0);
	CHECK(!r.app.launch_busy && strstr(r.app.msg, "Terminal opened in"));
	/* the cursor and the folder are what they were */
	CHECK(!wcscmp(under(&r.app), L"file2.txt") &&
	        !wcscmp(r.app.cwd, L"C:\\Users\\dev"));
	r.app.now_ms += 2000;
	ch(&r.app, 'k'); /* onto the folder Docs: still the folder on screen */
	CHECK(!wcscmp(under(&r.app), L"Docs"));
	ch(&r.app, '!');
	CHECK(2 == r.spy.calls && !wcscmp(r.spy.dir, L"C:\\Users\\dev"));
	wf_app_destroy(&r.app);
}

static void each_tab_asks_for_its_own_folder(void)
{
	static Rig r;

	rig(&r, true);
	ch(&r.app, 't'); /* a second tab, at this folder */
	ch(&r.app, 'h'); /* up, in this tab only */
	CHECK(!wcscmp(r.app.cwd, L"C:\\Users"));
	ch(&r.app, '!');
	CHECK(1 == r.spy.calls && !wcscmp(r.spy.dir, L"C:\\Users"));
	answer(&r, WF_OK, 0);
	r.app.now_ms += 2000;
	ch(&r.app, '['); /* the first tab: its own folder */
	CHECK(!wcscmp(r.app.cwd, L"C:\\Users\\dev"));
	ch(&r.app, '!');
	CHECK(2 == r.spy.calls && !wcscmp(r.spy.dir, L"C:\\Users\\dev"));
	wf_app_destroy(&r.app);
}

static void places_that_are_not_ordinary_local_folders_are_not_asked_for(void)
{
	static Rig r;
	static const wchar_t *const places[] = {
	        L"\\\\wsl.localhost\\Ubuntu\\home\\me", L"sftp://host/dir"};

	for (size_t i = 0; i < sizeof places / sizeof *places; i++) {
		rig(&r, true);
		free(r.app.cwd);
		r.app.cwd = _wcsdup(places[i]);
		ch(&r.app, '!');
		CHECK(0 == r.spy.calls && strstr(r.app.msg, "local drive"));
		wf_app_destroy(&r.app);
	}
}

static void every_answer_is_told_plainly(void)
{
	static Rig r;
	static const struct
	{
		WfStatus st;
		const char *words;
	} cases[] = {{WF_E_INVAL, "local drive"},
	        {WF_E_NOENT, "no longer exists"}, {WF_E_ACCESS, "no access"},
	        {WF_E_OS, "no terminal program"},
	        {WF_E_NOMEM, "no terminal program"}};

	for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
		rig(&r, true);
		ch(&r.app, '!');
		answer(&r, cases[i].st, 5);
		CHECK(1 == r.spy.calls && !r.app.launch_busy &&
		        strstr(r.app.msg, cases[i].words));
		/* the app goes on, and a press after a pause asks again */
		ch(&r.app, 'j');
		CHECK(!wcscmp(under(&r.app), L"file2.txt"));
		r.app.now_ms += 2000;
		ch(&r.app, '!');
		CHECK(2 == r.spy.calls);
		wf_app_destroy(&r.app);
	}
}

/* Pressed for 5 seconds at the rate of a held key: one request, and not one
 * more when the second after the first is over. A press after the keys were let
 * go, and a pause, asks again. */
static void a_held_key_asks_once_however_long_it_is_held(void)
{
	static Rig r;

	rig(&r, true);
	for (int i = 0; i < 170; i++) {
		r.app.now_ms += 30;
		ch(&r.app, '!');
		if (0 == i) {
			answer(&r, WF_OK,
			        0); /* answered at once: not what holds it */
		}
	}
	CHECK(1 == r.spy.calls);
	r.app.now_ms += 1000;
	ch(&r.app, '!');
	CHECK(2 == r.spy.calls);
	wf_app_destroy(&r.app);
}

static void a_request_still_starting_and_a_refused_one_are_told(void)
{
	static Rig r;

	rig(&r, true);
	ch(&r.app, '!');
	r.app.now_ms += 2000;
	ch(&r.app, '!'); /* the first is not answered yet */
	CHECK(1 == r.spy.calls && strstr(r.app.msg, "busy"));
	wf_app_destroy(&r.app);
	rig(&r, true);
	r.spy.answer = WF_E_INVAL; /* the worker would not take it */
	ch(&r.app, '!');
	CHECK(1 == r.spy.calls && !r.app.launch_busy &&
	        strstr(r.app.msg, "could not be started"));
	wf_app_destroy(&r.app);
	rig(&r, false);
	ch(&r.app, '!');
	CHECK(0 == r.spy.calls && strstr(r.app.msg, "not available"));
	wf_app_destroy(&r.app);
}

int main(void)
{
	the_folder_on_screen_is_asked_for_not_the_entry_under_the_cursor();
	each_tab_asks_for_its_own_folder();
	places_that_are_not_ordinary_local_folders_are_not_asked_for();
	every_answer_is_told_plainly();
	a_held_key_asks_once_however_long_it_is_held();
	a_request_still_starting_and_a_refused_one_are_told();
	return TEST_RESULT();
}

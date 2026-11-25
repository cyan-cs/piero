/* Enter on a file: what is asked of the launch port, what is refused before
 * asking, and how the answer is shown. The port is a spy. */
#include <stdlib.h>
#include <string.h>

#include "wf_fake_fs.h"
#include "wf_test.h"
#include "base/wf_macro.h"

typedef struct
{
	unsigned calls;
	uint32_t gen;
	wchar_t path[128], dir[128];
	WfFileId id;
	WfStatus answer;
} Spy;

static WfStatus spy_submit(void *ctx, uint32_t gen, const WfLaunchRequest *r)
{
	Spy *s = ctx;

	s->calls++;
	s->gen = gen;
	wcscpy_s(s->path, 128, r->path);
	wcscpy_s(s->dir, 128, r->directory);
	s->id = r->id;
	return s->answer;
}

static void ch(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	a->mode->on_event(a, &e);
}

static void enter(WfApp *a)
{
	WfEvent e = {WF_EV_KEY, 0, WF_KEY_ENTER, 0};

	a->mode->on_event(a, &e);
}

static void esc(WfApp *a)
{
	WfEvent e = {WF_EV_KEY, 0, WF_KEY_ESC, 0};

	a->mode->on_event(a, &e);
}

typedef struct
{
	WfApp app;
	FakePort fs;
	Spy spy;
} Rig;

/* Browsing C:\Users\dev with the cursor on file2.txt, whose identity the
 * listing proved. `neutral` is the configured lpDirectory (NULL: file_dir). */
static void rig(Rig *r, const wchar_t *neutral, bool with_port)
{
	WfEntry *e;

	memset(r, 0, sizeof *r);
	r->fs.app = &r->app;
	CHECK(wf_app_init(&r->app, 120, 30, fake_port(&r->fs),
	              L"C:\\Users\\dev") == WF_OK);
	if (with_port) {
		CHECK(wf_app_set_launch(&r->app,
		              (WfLaunchPort){&r->spy, spy_submit},
		              neutral) == WF_OK);
	}
	ch(&r->app, 'j'); /* file2.txt */
	e = &r->app.cur.dir
	             ->e[wf_view_current(&r->app.cur) - r->app.cur.dir->e];
	CHECK(!wcscmp(e->name, L"file2.txt"));
	e->identity.kind = WF_ID_STRONG;
	e->identity.volume = 3;
	e->identity.id[0] = 42;
}

static WfEntry *cursor_entry(Rig *r)
{
	return &r->app.cur.dir
	                ->e[wf_view_current(&r->app.cur) - r->app.cur.dir->e];
}

static void a_plain_file_is_sent_with_its_identity_and_the_neutral_directory(
        void)
{
	static Rig r;
	WfLaunchOutcome ok = {WF_OK, 0};

	rig(&r, L"C:\\neutral", true);
	enter(&r.app);
	CHECK(1 == r.spy.calls && r.app.launch_busy);
	CHECK(!wcscmp(r.spy.path, L"C:\\Users\\dev\\file2.txt"));
	CHECK(!wcscmp(r.spy.dir, L"C:\\neutral"));
	CHECK((r.spy.id.kind == WF_ID_STRONG) && 42 == r.spy.id.id[0] &&
	        3 == r.spy.id.volume);
	CHECK(strstr(r.app.msg, "Opening"));
	/* a second request waits for the first answer */
	enter(&r.app);
	CHECK(1 == r.spy.calls && strstr(r.app.msg, "busy"));
	wf_app_on_launch(&r.app, r.spy.gen, &ok, L"file2.txt");
	CHECK(!r.app.launch_busy && strstr(r.app.msg, "Opened: file2.txt"));
	/* stale or unexpected answers change nothing */
	r.app.msg[0] = 0;
	wf_app_on_launch(&r.app, r.spy.gen, &ok, L"file2.txt");
	CHECK(0 == r.app.msg[0]);
	enter(&r.app);
	CHECK(2 == r.spy.calls);
	wf_app_destroy(&r.app);
}

static void file_dir_uses_the_folder_of_the_file(void)
{
	static Rig r;

	rig(&r, NULL, true);
	enter(&r.app);
	CHECK(1 == r.spy.calls && !wcscmp(r.spy.dir, L"C:\\Users\\dev"));
	wf_app_destroy(&r.app);
}

static void answers_are_reported_plainly(void)
{
	static Rig r;
	static const struct
	{
		WfStatus st;
		unsigned long error;
		const char *needle;
	} cases[] = {
	        {WF_E_INVAL, 0, "changed since it was listed"},
	        {WF_E_NOENT, 2, "gone or has no program"},
	        {WF_E_ACCESS, 5, "denied"},
	        {WF_E_OS, 1155, "failed"},
	};
	size_t i;

	for (i = 0; i < WF_ARRAY_LEN(cases); i++) {
		WfLaunchOutcome o = {cases[i].st, cases[i].error};

		rig(&r, L"C:\\neutral", true);
		enter(&r.app);
		wf_app_on_launch(&r.app, r.spy.gen, &o, L"file2.txt");
		CHECK(!r.app.launch_busy && strstr(r.app.msg, cases[i].needle));
		CHECK(!strstr(r.app.msg, "Opened"));
		wf_app_destroy(&r.app);
	}
}

static void refusals_happen_before_the_port_is_asked(void)
{
	static Rig r;

	/* identity not proven */
	rig(&r, L"C:\\neutral", true);
	cursor_entry(&r)->identity.kind = WF_ID_NONE;
	enter(&r.app);
	CHECK(0 == r.spy.calls && strstr(r.app.msg, "unproven"));
	wf_app_destroy(&r.app);

	/* a link */
	rig(&r, L"C:\\neutral", true);
	cursor_entry(&r)->flags |= WF_ENT_LINK;
	enter(&r.app);
	CHECK(0 == r.spy.calls && strstr(r.app.msg, "link"));
	wf_app_destroy(&r.app);

	/* a file that is not local would be downloaded */
	rig(&r, L"C:\\neutral", true);
	cursor_entry(&r)->flags |= WF_ENT_CLOUD;
	enter(&r.app);
	CHECK(0 == r.spy.calls && strstr(r.app.msg, "cloud"));
	wf_app_destroy(&r.app);

	/* no port: the gate wording stays and nothing is opened */
	rig(&r, L"C:\\neutral", false);
	enter(&r.app);
	CHECK(0 == r.spy.calls &&
	        strstr(r.app.msg, "opening files is not set up"));
	wf_app_destroy(&r.app);

	/* no port, and the program knows why: it says that, not the gate */
	rig(&r, L"C:\\neutral", false);
	wf_app_set_open_off(&r.app, MSG_OPEN_OFF_PRIVATE);
	enter(&r.app);
	CHECK(0 == r.spy.calls &&
	        !strcmp(r.app.msg, wf_msg(MSG_OPEN_OFF_PRIVATE)) &&
	        !strstr(r.app.msg, "opening files is not set up"));
	wf_app_destroy(&r.app);

	/* the port refusing the request leaves nothing pending */
	rig(&r, L"C:\\neutral", true);
	r.spy.answer = WF_E_INVAL;
	enter(&r.app);
	CHECK(1 == r.spy.calls && !r.app.launch_busy &&
	        strstr(r.app.msg, "could not start"));
	wf_app_destroy(&r.app);
}

/* A program or script asks first; the answer is a letter, never Enter, and
 * Esc leaves everything as it was. */
static void a_program_needs_the_confirmation(void)
{
	static Rig r;
	WfEntry *e;

	rig(&r, L"C:\\neutral", true);
	e = cursor_entry(&r);
	memcpy(e->name, L"tool.exe", sizeof L"tool.exe");
	memcpy(e->display, "tool.exe", sizeof "tool.exe");
	enter(&r.app);
	CHECK(0 == r.spy.calls && &WF_MODE_CONFIRM == r.app.mode);
	CHECK(strstr(r.app.confirm.lines[1], "tool.exe"));

	/* Esc: no request, still nothing pending */
	wf_app_on_presented(&r.app);
	r.app.now_ms += WF_CONFIRM_GUARD_MS;
	CHECK(WF_ACT_REDRAW == wf_app_tick(&r.app));
	esc(&r.app);
	CHECK(0 == r.spy.calls && &WF_MODE_NORMAL == r.app.mode);

	/* during the guard the letter does nothing */
	enter(&r.app);
	wf_app_on_presented(&r.app);
	ch(&r.app, 'o');
	CHECK(0 == r.spy.calls && &WF_MODE_CONFIRM == r.app.mode);
	/* armed: the letter opens it */
	r.app.now_ms += WF_CONFIRM_GUARD_MS;
	(void)wf_app_tick(&r.app);
	ch(&r.app, 'o');
	CHECK(1 == r.spy.calls && &WF_MODE_NORMAL == r.app.mode &&
	        r.app.launch_busy);
	CHECK(!wcscmp(r.spy.path, L"C:\\Users\\dev\\tool.exe"));
	wf_app_destroy(&r.app);
}

int main(void)
{
	a_plain_file_is_sent_with_its_identity_and_the_neutral_directory();
	file_dir_uses_the_folder_of_the_file();
	answers_are_reported_plainly();
	refusals_happen_before_the_port_is_asked();
	a_program_needs_the_confirmation();
	return TEST_RESULT();
}

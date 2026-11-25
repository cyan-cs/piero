/* Create/rename forms with an execution port installed (a fake): what the app
 * sends, what it refuses to send, and how it reports the receipt. Without a
 * port the forms still stop at the release gate; that is covered by
 * test_operations_ui.c. */
#include <string.h>

#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"
#include "base/wf_macro.h"

typedef struct
{
	unsigned calls;
	uint32_t gen;
	WfEditKind kind;
	wchar_t parent[128], old_name[128], new_name[128];
	WfFileId parent_id, source_id;
	WfStatus answer;
} Spy;

static WfStatus spy_submit(void *ctx, uint32_t gen, const WfEditRequest *req)
{
	Spy *s = ctx;

	s->calls++;
	s->gen = gen;
	s->kind = req->kind;
	wcscpy_s(s->parent, 128, req->parent);
	wcscpy_s(s->old_name, 128, req->old_name ? req->old_name : L"");
	wcscpy_s(s->new_name, 128, req->new_name);
	s->parent_id = req->parent_id;
	s->source_id = req->source_id;
	return s->answer;
}

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

static void type(WfApp *a, const wchar_t *s)
{
	for (; *s; s++) {
		ch(a, (uint32_t)*s);
	}
}

/* Listing identities as a guarded native listing would supply them. */
static void prove_listing(WfApp *a)
{
	WfDir *d = a->cur.dir;

	d->identity.kind = WF_ID_STRONG;
	d->identity.volume = 7;
	d->identity.id[0] = 11;
	for (size_t i = 0; i < d->n; i++) {
		d->e[i].identity.kind = WF_ID_STRONG;
		d->e[i].identity.volume = 7;
		d->e[i].identity.id[0] = (uint8_t)(i + 20);
	}
}

static void setup(WfApp *a, FakePort *fp, Spy *spy)
{
	CHECK(WF_OK ==
	        wf_app_init(a, 120, 30, fake_port(fp), L"C:\\Users\\dev"));
	prove_listing(a);
	spy->answer = WF_OK;
	wf_app_set_edit(a, (WfEditPort){spy, spy_submit});
}

static WfEditOutcome outcome(WfStatus st, unsigned long error, bool renamed)
{
	WfEditOutcome o = {st, error, renamed, NULL, NULL, 0};

	return o;
}

static void create_file_sends_the_captured_identities(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Spy spy = {0};
	WfEditOutcome done = outcome(WF_OK, 0, true);

	setup(&a, &fp, &spy);
	ch(&a, '%');
	type(&a, L"notes.txt");
	key(&a, WF_KEY_ENTER);
	CHECK(1 == spy.calls && WF_EDIT_CREATE_FILE == spy.kind &&
	        !wcscmp(spy.parent, L"C:\\Users\\dev") &&
	        !wcscmp(spy.new_name, L"notes.txt"));
	CHECK((spy.parent_id.kind == WF_ID_STRONG) &&
	        7 == spy.parent_id.volume && 11 == spy.parent_id.id[0]);
	CHECK(&WF_MODE_NORMAL == a.mode && a.edit_busy &&
	        strstr(a.msg, "Creating"));
	/* a stale receipt is ignored */
	wf_app_on_edit(&a, spy.gen + 1, &done, L"notes.txt");
	CHECK(a.edit_busy);
	wf_app_on_edit(&a, spy.gen, &done, L"notes.txt");
	CHECK(!a.edit_busy && strstr(a.msg, "Done: notes.txt"));
	CHECK(!wcscmp(a.cwd, L"C:\\Users\\dev")); /* reloaded in place */
	wf_app_destroy(&a);
}

static void create_folder_and_rename(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Spy spy = {0};
	const WfEntry *entry;

	setup(&a, &fp, &spy);
	ch(&a, 'd');
	type(&a, L"newdir");
	key(&a, WF_KEY_ENTER);
	CHECK(1 == spy.calls && WF_EDIT_CREATE_DIR == spy.kind);
	wf_app_destroy(&a);

	setup(&a, &fp, &spy);
	spy.calls = 0;
	entry = wf_view_current(&a.cur);
	CHECK(NULL != entry);
	ch(&a, 'R');
	CHECK(!wcscmp(a.path_input, entry->name));
	/* the name (not the extension) is selected: typing replaces it */
	CHECK(a.path_input_selected && a.path_input_sel_end <= wcslen(entry->name));
	key(&a, WF_KEY_END); /* leaves the selection; the text is kept */
	CHECK(!a.path_input_selected && !wcscmp(a.path_input, entry->name));
	type(&a, L"2");
	key(&a, WF_KEY_ENTER);
	CHECK(1 == spy.calls && WF_EDIT_RENAME == spy.kind &&
	        !wcscmp(spy.old_name, entry->name) &&
	        (spy.source_id.kind == WF_ID_STRONG) &&
	        7 == spy.source_id.volume);
	{
		wchar_t expected[128];

		swprintf(expected, 128, L"%ls2", entry->name);
		CHECK(!wcscmp(spy.new_name, expected));
	}
	wf_app_destroy(&a);
}

static void bad_names_keep_the_form_open_and_send_nothing(void)
{
	static const wchar_t *const bad[] = {
	        L"", L"CON", L"a:b", L"trailing.", L"a<b", L"x\x202Ey"};
	WfApp a;
	FakePort fp = {.app = &a};
	Spy spy = {0};

	for (size_t i = 0; i < WF_ARRAY_LEN(bad); i++) {
		setup(&a, &fp, &spy);
		ch(&a, '%');
		type(&a, bad[i]);
		key(&a, WF_KEY_ENTER);
		CHECK(0 == spy.calls && !a.edit_busy);
		CHECK(&WF_MODE_NORMAL != a.mode); /* still editing */
		CHECK(strstr(a.msg, "ERROR NAME"));
		wf_app_destroy(&a);
	}
	/* renaming to the same name is a no-op, not a request */
	setup(&a, &fp, &spy);
	ch(&a, 'R');
	key(&a, WF_KEY_ENTER);
	CHECK(0 == spy.calls && &WF_MODE_NORMAL == a.mode &&
	        strstr(a.msg, "unchanged"));
	wf_app_destroy(&a);
}

static void unproven_listing_still_refuses(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Spy spy = {0};

	setup(&a, &fp, &spy);
	a.cur.dir->identity.kind = WF_ID_NONE; /* parent never proven */
	ch(&a, '%');
	type(&a, L"x.txt");
	key(&a, WF_KEY_ENTER);
	CHECK(0 == spy.calls && !a.edit_busy && strstr(a.msg, "identity"));
	wf_app_destroy(&a);

	setup(&a, &fp, &spy);
	ch(&a, '%');
	type(&a, L"x.txt");
	a.nav_gen++; /* the listing was replaced while typing */
	key(&a, WF_KEY_ENTER);
	CHECK(0 == spy.calls && !a.edit_busy && strstr(a.msg, "changed"));
	wf_app_destroy(&a);
}

static void busy_and_refused_submissions(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Spy spy = {0};
	WfEditOutcome done = outcome(WF_OK, 0, true);

	setup(&a, &fp, &spy);
	ch(&a, '%');
	type(&a, L"one.txt");
	key(&a, WF_KEY_ENTER);
	CHECK(1 == spy.calls && a.edit_busy);
	/* a second form cannot be submitted until the receipt arrives */
	prove_listing(&a);
	ch(&a, '%');
	type(&a, L"two.txt");
	key(&a, WF_KEY_ENTER);
	CHECK(1 == spy.calls && strstr(a.msg, "BUSY") &&
	        &WF_MODE_NORMAL != a.mode);
	key(&a, WF_KEY_ESC);
	wf_app_on_edit(&a, spy.gen, &done, L"one.txt");
	CHECK(!a.edit_busy);

	/* the port refusing (busy/out of memory) leaves nothing pending */
	spy.answer = WF_E_INVAL;
	prove_listing(&a);
	ch(&a, '%');
	type(&a, L"three.txt");
	key(&a, WF_KEY_ENTER);
	CHECK(2 == spy.calls && !a.edit_busy &&
	        strstr(a.msg, "nothing changed"));
	wf_app_destroy(&a);
}

static void receipts_are_reported_faithfully(void)
{
	struct
	{
		WfEditOutcome o;
		const char *expect;
	} cases[] = {
	        {{WF_E_ACCESS, 5, false, NULL, NULL, 0}, "access denied"},
	        {{WF_E_OS, 183, false, NULL, NULL, 0}, "already exists"},
	        {{WF_E_OS, 80, false, NULL, NULL, 0}, "already exists"},
	        {{WF_E_INVAL, 0, false, NULL, NULL, 0}, "changed since it was listed"},
	        {{WF_E_INVAL, 0, false, "reserved device name", NULL, 0},
	                "ERROR NAME: reserved device name"},
	        {{WF_E_OS, 1117, false, NULL, NULL, 0}, "Win32 1117"},
	        /* the object moved but the post-check failed: say so */
	        {{WF_E_OS, 6, true, NULL, NULL, 0}, "WARNING: done"},
	};

	for (size_t i = 0; i < WF_ARRAY_LEN(cases); i++) {
		WfApp a;
		FakePort fp = {.app = &a};
		Spy spy = {0};

		setup(&a, &fp, &spy);
		ch(&a, '%');
		type(&a, L"f.txt");
		key(&a, WF_KEY_ENTER);
		wf_app_on_edit(&a, spy.gen, &cases[i].o, L"f.txt");
		CHECK(!a.edit_busy && strstr(a.msg, cases[i].expect));
		if (cases[i].o.renamed) {
			CHECK(strstr(a.msg, "f.txt"));
		} else {
			CHECK(strstr(a.msg, "nothing") ||
			        strstr(a.msg, "overwritten"));
		}
		wf_app_destroy(&a);
	}
}

/* The reload that follows a create/rename clears the status line when it
 * commits (it may arrive later); the receipt must survive that. */
static void receipt_survives_the_reload(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Spy spy = {0};
	WfEditOutcome done = outcome(WF_OK, 0, true);

	setup(&a, &fp, &spy);
	ch(&a, '%');
	type(&a, L"notes.txt");
	key(&a, WF_KEY_ENTER);
	fp.defer = 1; /* the answer to the reload comes later */
	wf_app_on_edit(&a, spy.gen, &done, L"notes.txt");
	CHECK(1 == fp.nq && NULL != a.want_path);
	CHECK(strstr(a.msg, "Done: notes.txt"));
	fake_deliver(&fp, 0); /* the listing commits */
	CHECK(NULL == a.want_path);
	CHECK(strstr(a.msg, "Done: notes.txt"));
	/* a second reload inside the hold (the directory watcher does this
	 * right after the change) keeps it too */
	wf_app_reload(&a);
	fake_deliver(&fp, 1);
	CHECK(strstr(a.msg, "Done: notes.txt"));
	/* once the hold is over an ordinary reload clears it */
	a.now_ms += WF_RECEIPT_KEEP_MS + 1;
	wf_app_reload(&a);
	fake_deliver(&fp, 2);
	CHECK(0 == a.msg[0]);
	wf_app_destroy(&a);
}

static void without_a_port_the_gate_stays_closed(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	prove_listing(&a);
	ch(&a, '%');
	type(&a, L"x.txt");
	key(&a, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == a.mode && !a.edit_busy &&
	        strstr(a.msg, "BLOCKED create"));
	wf_app_destroy(&a);
}

int main(void)
{
	create_file_sends_the_captured_identities();
	create_folder_and_rename();
	bad_names_keep_the_form_open_and_send_nothing();
	unproven_listing_still_refuses();
	busy_and_refused_submissions();
	receipts_are_reported_faithfully();
	receipt_survives_the_reload();
	without_a_port_the_gate_stays_closed();
	return TEST_RESULT();
}

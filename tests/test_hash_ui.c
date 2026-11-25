/*
 * SHA-256 on request in the application (DR-DRAFT-027): # asks the worker for the
 * file under the cursor and for nothing else, shows progress and the end, refuses
 * a folder, a link and a build without the worker, drops answers that are not the
 * newest or that came after Esc, shows the digest in the details window only for
 * the file it was made from (and only while that file is as it was), and never
 * shows a digest for a file that changed while it was read. The worker is a fake.
 */
#include <string.h>

#include "ui/wf_app.h"
#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

typedef struct
{
	int starts, cancels;
	uint32_t gen;
	wchar_t path[260];
} Fake;

static WfStatus fake_start(void *ctx, uint32_t gen, const wchar_t *path)
{
	Fake *f = ctx;

	f->starts++;
	f->gen = gen;
	wcsncpy_s(f->path, 260, path, _TRUNCATE);
	return WF_OK;
}

static void fake_cancel(void *ctx)
{
	((Fake *)ctx)->cancels++;
}

static const char *HEX_ABC =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

static void press(WfApp *a, uint32_t ch)
{
	WfEvent e = {WF_EV_CHAR, ch, WF_KEY_NONE, 0};

	(void)a->mode->on_event(a, &e);
}

static void esc(WfApp *a)
{
	WfEvent e = {WF_EV_KEY, 0, WF_KEY_ESC, 0};

	(void)a->mode->on_event(a, &e);
}

/* The cursor on the entry called `name`. */
static bool go_to(WfApp *a, const wchar_t *name)
{
	for (int k = 0; k < 20; k++) {
		const WfEntry *e = wf_view_current(&a->cur);

		if (e && !wcscmp(e->name, name)) {
			return true;
		}
		press(a, 'j');
	}
	return false;
}

static bool has_line(const WfInfo *in, const char *part)
{
	for (int i = 0; i < in->n; i++) {
		if (strstr(in->text[i], part)) {
			return true;
		}
	}
	return false;
}

static void finish(
        WfApp *a, uint32_t gen, WfStatus st, uint64_t size, uint64_t when)
{
	WfHashUpdate u = {0};

	u.gen = gen;
	u.finished = true;
	u.res.st = st;
	u.res.size = size;
	u.res.last_write = when;
	if (WF_OK == st) {
		static const uint8_t abc[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f,
		        0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae,
		        0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a,
		        0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};

		memcpy(u.res.digest, abc, 32);
	}
	wf_app_on_hash(a, &u);
}

static WfApp *open_app(FakePort *fp, Fake *f, WfApp *a)
{
	WfHashPort port = {f, fake_start, fake_cancel};

	fp->app = a;
	CHECK(WF_OK ==
	        wf_app_init(a, 100, 30, fake_port(fp), L"C:\\Users\\dev"));
	wf_app_set_hash(a, port);
	return a;
}

static void happy_path(void)
{
	WfApp a;
	FakePort fp = {0};
	Fake f = {0};
	WfHashUpdate p = {0};

	open_app(&fp, &f, &a);
	CHECK(go_to(&a, L"file2.txt"));
	press(&a, '#');
	CHECK(1 == f.starts && 0 == f.cancels);
	CHECK(!wcscmp(f.path, L"C:\\Users\\dev\\file2.txt"));
	CHECK(NULL != strstr(a.msg, "file2.txt"));

	/* progress is shown; an answer for another request is not */
	p.gen = f.gen;
	p.done = 50;
	p.total = 200;
	wf_app_on_hash(&a, &p);
	CHECK(NULL != strstr(a.msg, "25%"));
	p.gen = f.gen + 7;
	p.done = 100;
	wf_app_on_hash(&a, &p);
	CHECK(NULL != strstr(a.msg, "25%"));

	/* the end: the digest and the name */
	finish(&a, f.gen, WF_OK, 0, 0);
	CHECK(NULL != strstr(a.msg, "SHA-256 ba7816bf"));
	CHECK(f.starts == 1);

	/* asked again for the same file as it is: shown, not read again */
	a.msg[0] = 0;
	press(&a, '#');
	CHECK(1 == f.starts);
	CHECK(NULL != strstr(a.msg, "SHA-256 ba7816bf"));

	/* the details window has the digest, for this file only */
	press(&a, 'i');
	CHECK(&WF_MODE_INFO == a.mode);
	CHECK(has_line(&a.info, HEX_ABC));
	esc(&a);
	CHECK(go_to(&a, L"file10.txt"));
	press(&a, 'i');
	CHECK(!has_line(&a.info, "SHA-256:"));
	esc(&a);
	wf_app_destroy(&a);
}

static void refusals(void)
{
	WfApp a;
	FakePort fp = {0};
	Fake f = {0};

	open_app(&fp, &f, &a);
	CHECK(go_to(&a, L"Docs"));
	press(&a, '#');
	CHECK(0 == f.starts);
	CHECK(NULL != strstr(a.msg, "folder"));

	/* a link is not followed: the entry is marked as one */
	CHECK(go_to(&a, L"file2.txt"));
	a.cur.dir->e[a.cur.vis[a.cur.cursor]].flags |= WF_ENT_LINK;
	press(&a, '#');
	CHECK(0 == f.starts);
	CHECK(NULL != strstr(a.msg, "link"));
	wf_app_destroy(&a);

	/* a build without the worker says so */
	{
		WfApp b;
		FakePort fb = {0};

		fb.app = &b;
		CHECK(WF_OK ==
		        wf_app_init(&b, 100, 30, fake_port(&fb),
		                L"C:\\Users\\dev"));
		CHECK(go_to(&b, L"file2.txt"));
		press(&b, '#');
		CHECK(NULL != strstr(b.msg, "not available"));
		wf_app_destroy(&b);
	}
}

static void failures_show_no_digest(void)
{
	static const struct
	{
		WfStatus st;
		const char *text;
	} cases[] = {{WF_E_CHANGED, "changed while it was read"},
	        {WF_E_ACCESS, "cannot be read"}, {WF_E_NOENT, "was not found"},
	        {WF_E_OS, "could not be read"}};

	for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		WfApp a;
		FakePort fp = {0};
		Fake f = {0};

		open_app(&fp, &f, &a);
		CHECK(go_to(&a, L"file2.txt"));
		press(&a, '#');
		finish(&a, f.gen, cases[k].st, 0, 0);
		CHECK(NULL != strstr(a.msg, cases[k].text));
		CHECK(NULL == strstr(a.msg, "ba7816bf"));
		press(&a, 'i');
		CHECK(!has_line(&a.info, "SHA-256:"));
		esc(&a);
		/* and the key reads again: nothing was kept */
		press(&a, '#');
		CHECK(2 == f.starts);
		wf_app_destroy(&a);
	}
}

static void stopping(void)
{
	WfApp a;
	FakePort fp = {0};
	Fake f = {0};

	open_app(&fp, &f, &a);
	CHECK(go_to(&a, L"file2.txt"));
	press(&a, '#');
	press(&a, 'i');
	CHECK(has_line(&a.info, "computing"));
	esc(&a);
	esc(&a); /* Esc in the list stops the running hash */
	CHECK(1 == f.cancels);
	CHECK(NULL != strstr(a.msg, "stopped"));
	/* what the worker still delivers is dropped */
	finish(&a, f.gen, WF_OK, 0, 0);
	CHECK(NULL == strstr(a.msg, "ba7816bf"));
	press(&a, 'i');
	CHECK(!has_line(&a.info, "SHA-256:"));
	esc(&a);
	/* a second Esc has nothing to stop */
	esc(&a);
	CHECK(1 == f.cancels);

	/* a newer request replaces a running one */
	press(&a, '#');
	CHECK(go_to(&a, L"file10.txt"));
	press(&a, '#');
	CHECK(3 == f.starts);
	CHECK(2 == f.cancels);
	CHECK(!wcscmp(f.path, L"C:\\Users\\dev\\file10.txt"));
	wf_app_destroy(&a);
}

/* A digest is for the file as it was read: when the listing shows another size or
 * time, it is not shown as that file's. */
static void only_for_the_same_file(void)
{
	WfApp a;
	FakePort fp = {0};
	Fake f = {0};

	open_app(&fp, &f, &a);
	CHECK(go_to(&a, L"file2.txt"));
	press(&a, '#');
	finish(&a, f.gen, WF_OK, 5, 0); /* read when it was 5 bytes long */
	press(&a, 'i');
	CHECK(!has_line(&a.info, "SHA-256:")); /* the listing says 0 */
	esc(&a);
	press(&a, '#'); /* so it is read again, not shown from memory */
	CHECK(2 == f.starts);

	press(&a, '#');
	finish(&a, f.gen, WF_OK, 0, 99); /* another last write time */
	press(&a, 'i');
	CHECK(!has_line(&a.info, "SHA-256:"));
	esc(&a);
	wf_app_destroy(&a);
}

int main(void)
{
	happy_path();
	refusals();
	failures_show_no_digest();
	stopping();
	only_for_the_same_file();
	return TEST_RESULT();
}

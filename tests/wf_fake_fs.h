#ifndef WF_FAKE_FS_H
#define WF_FAKE_FS_H

#include <string.h>

#include "fs/wf_dir.h"
#include "fs/wf_pcplace_win.h"
#include "path/wf_path.h"
#include "ui/wf_app.h"

/* A tiny in-memory tree:
 *   C:\            Users/ secret/ a.txt
 *   C:\Users       dev/ .hidden
 *   C:\Users\dev   file10.txt file2.txt Docs/
 *   C:\Pics        Sub/ a.png b.png doc.pdf note.txt
 *   C:\secret      -> WF_E_ACCESS
 */
static WfStatus fake_list(const wchar_t *path, WfDir **out)
{
	WfDir *d;
	WfStatus st = WF_OK;

#define ADD(n, fl)                                                             \
	do {                                                                   \
		if (st == WF_OK)                                               \
			st = wf_dir_add(d, n, wcslen(n), fl, 0, 0);            \
	} while (0)
	if (0 == wcscmp(path, L"C:\\secret")) {
		return WF_E_ACCESS;
	}
	if (wf_pcroot_is(path)) {
		return wf_pcplace_list(
		        0, out); /* the real list; tests fix its source */
	}
	d = wf_dir_new();
	if (!d) {
		return WF_E_NOMEM;
	}
	if (0 == wcscmp(path, L"C:\\")) {
		ADD(L"a.txt", 0);
		ADD(L"Users", WF_ENT_DIR);
		ADD(L"secret", WF_ENT_DIR);
	} else if (0 == wcscmp(path, L"C:\\Users")) {
		ADD(L"dev", WF_ENT_DIR);
		ADD(L".hidden", WF_ENT_HIDDEN);
	} else if (0 == wcscmp(path, L"C:\\Users\\dev")) {
		ADD(L"file10.txt", 0);
		ADD(L"file2.txt", 0);
		ADD(L"Docs", WF_ENT_DIR);
	} else if (0 == wcscmp(path, L"\\\\wsl.localhost\\Ubuntu")) {
		ADD(L"home", WF_ENT_DIR);
	} else if (0 == wcscmp(path, L"\\\\wsl.localhost\\Ubuntu\\home\\me")) {
		ADD(L"Docs", WF_ENT_DIR);
		ADD(L"a.txt", 0);
		ADD(L"b.txt", 0);
	} else if (0 ==
	        wcscmp(path, L"\\\\wsl.localhost\\Ubuntu\\home\\me\\Docs")) {
		ADD(L"in.txt", 0);
	} else if (0 == wcscmp(path, L"C:\\Pics")) {
		ADD(L"Sub", WF_ENT_DIR);
		ADD(L"a.png", 0);
		ADD(L"b.png", 0);
		ADD(L"doc.pdf", 0);
		ADD(L"note.txt", 0);
	} else {
		st = WF_E_NOENT;
	}
#undef ADD
	if (WF_OK == st) {
		st = wf_dir_sort(d, WF_SORT_NAME, 0);
	}
	if (WF_OK != st) {
		wf_dir_free(d);
		return st;
	}
	*out = d;
	return WF_OK;
}

/*
 * The app's fs port backed by fake_list. By default requests are answered
 * synchronously from inside submit(). With defer = 1 they are queued and the
 * test delivers them (in any order) with fake_deliver().
 */
typedef struct
{
	WfApp *app; /* set before wf_app_init */
	int defer;
	int nq;
	struct
	{
		uint32_t gen;
		wchar_t path[64], parent[64];
		int has_parent;
	} q[8];
} FakePort;

static void fake_answer(FakePort *p,
        uint32_t gen,
        const wchar_t *path,
        const wchar_t *parent,
        int has_parent)
{
	WfDir *d = NULL, *pd = NULL;
	WfStatus st = fake_list(path, &d);

	if (WF_OK == st && has_parent && WF_OK != fake_list(parent, &pd)) {
		pd = NULL;
	}
	wf_app_on_listing(p->app, gen, st, d, pd);
}

static WfStatus fake_submit(
        void *ctx, uint32_t gen, const wchar_t *path, const wchar_t *parent)
{
	FakePort *p = ctx;

	if (p->defer) {
		int i = p->nq++;

		p->q[i].gen = gen;
		wcsncpy_s(p->q[i].path, 64, path, _TRUNCATE);
		p->q[i].has_parent = NULL != parent;
		wcsncpy_s(p->q[i].parent, 64, parent ? parent : L"", _TRUNCATE);
		return WF_OK;
	}
	fake_answer(p, gen, path, parent, NULL != parent);
	return WF_OK;
}

static WfFsPort fake_port(FakePort *p)
{
	WfFsPort port = {p, fake_submit};

	return port;
}

/* Not every test includes this header for deferred delivery. */
#if defined(__clang__)
__attribute__((unused))
#endif
static void fake_deliver(FakePort *p, int i)
{
	fake_answer(p, p->q[i].gen, p->q[i].path, p->q[i].parent,
	        p->q[i].has_parent);
}

#endif

/*
 * "This PC": the place above every drive. Going left from C:\ reaches it with
 * C: selected, it lists the drives and the WSL distributions, entering one goes
 * into it, and nothing that changes files can be done there.
 */
#include <string.h>

#include "base/wf_macro.h"
#include "fs/wf_pcplace_win.h"
#include "path/wf_path.h"
#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

static void key(WfApp *a, WfKey k)
{
	WfEvent e = {WF_EV_KEY, 0, k, 0};

	a->mode->on_event(a, &e);
}

static void ch(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	a->mode->on_event(a, &e);
}

static bool joins_to(const wchar_t *name, const wchar_t *want)
{
	wchar_t *r = NULL;
	bool ok = WF_OK == wf_path_join(WF_PCROOT, name, &r) && r &&
	        !wcscmp(r, want);

	free(r);
	return ok;
}

static bool leaf_is(const wchar_t *path, const wchar_t *want)
{
	wchar_t *l = wf_pcroot_leaf(path);
	bool ok = want ? l && !wcscmp(l, want) : !l;

	free(l);
	return ok;
}

/* The path layer: what is "This PC", what its entries open, and what is above
 * a drive. */
static void paths(void)
{
	wchar_t *r = NULL;

	CHECK(wf_pcroot_is(WF_PCROOT));
	CHECK(wf_pcroot_is(WF_PCROOT L"\\"));
	CHECK(wf_pcroot_is(L"\\\\PC.INVALID\\Computer"));
	CHECK(!wf_pcroot_is(WF_PCROOT L"\\x"));
	CHECK(!wf_pcroot_is(L"C:\\") && !wf_pcroot_is(L"") &&
	        !wf_pcroot_is(NULL));
	CHECK(0 == wf_path_parent_len(WF_PCROOT)); /* nothing above it */

	CHECK(joins_to(L"C:", L"C:\\") && joins_to(L"d:", L"d:\\"));
	CHECK(joins_to(L"wsl:Ubuntu", L"\\\\wsl.localhost\\Ubuntu"));
	CHECK(joins_to(
	        L"wsl:docker-desktop", L"\\\\wsl.localhost\\docker-desktop"));
	/* nothing else is made from a name here */
	{
		const wchar_t *bad[] = {L"", L"x", L"C", L"CD:", L"C:x", L"1:",
		        L"C:\\", L"C:\\Windows", L"wsl:", L"wsl:.", L"wsl:..",
		        L"wsl:a\\b", L"wsl:a/b", L"wsl:a:b", L"wsl:a.",
		        L"wsl:a ", L"wsl:a*", L"WSL:Ubuntu",
		        L"\\\\evil\\share"};

		for (size_t i = 0; i < WF_ARRAY_LEN(bad); i++) {
			CHECK(WF_OK != wf_path_join(WF_PCROOT, bad[i], &r) &&
			        !r);
		}
	}
	/* a long name is refused, the longest is not */
	{
		wchar_t name[200] = L"wsl:";

		for (int i = 0; i < 129; i++) {
			name[4 + i] = L'a';
		}
		name[4 + 129] = L'\0';
		CHECK(WF_OK != wf_path_join(WF_PCROOT, name, &r));
		name[4 + 128] = L'\0';
		CHECK(WF_OK == wf_path_join(WF_PCROOT, name, &r));
		free(r);
	}

	CHECK(leaf_is(L"C:\\", L"C:") && leaf_is(L"d:\\", L"D:"));
	CHECK(leaf_is(L"\\\\wsl.localhost\\Ubuntu", L"wsl:Ubuntu"));
	CHECK(leaf_is(L"\\\\wsl.localhost\\Ubuntu\\", L"wsl:Ubuntu"));
	CHECK(leaf_is(L"\\\\WSL.LOCALHOST\\Ubuntu", L"wsl:Ubuntu"));
	CHECK(leaf_is(L"C:\\Users", NULL) && leaf_is(WF_PCROOT, NULL) &&
	        leaf_is(L"\\\\wsl.localhost\\Ubuntu\\home", NULL) &&
	        leaf_is(L"\\\\wsl.localhost\\", NULL) &&
	        leaf_is(L"\\\\srv\\share", NULL) && leaf_is(L"", NULL) &&
	        leaf_is(NULL, NULL));
}

static const wchar_t *const DISTROS[] = {L"Ubuntu", L"docker-desktop", NULL};

/* The listing: one folder for each drive letter, one for each distribution,
 * and no file identity (so no operation that needs one). */
static void listing(void)
{
	WfDir *d = NULL;
	const wchar_t *want[] = {
	        L"C:", L"D:", L"wsl:Ubuntu", L"wsl:docker-desktop"};

	wf_pcplace_test_source(true, (1ul << 2) | (1ul << 3), DISTROS);
	CHECK(WF_OK == wf_pcplace_list(0, &d) && d && 4 == d->n);
	for (size_t i = 0; d && i < d->n && i < WF_ARRAY_LEN(want); i++) {
		CHECK(!wcscmp(d->e[i].name, want[i]));
		CHECK(d->e[i].flags & WF_ENT_DIR);
		CHECK(!(d->e[i].identity.kind == WF_ID_STRONG));
	}
	wf_dir_free(d);
	wf_pcplace_test_source(
	        true, 0, NULL); /* nothing to show is not an error */
	d = NULL;
	CHECK(WF_OK == wf_pcplace_list(0, &d) && d && 0 == d->n);
	wf_dir_free(d);
	wf_pcplace_test_source(true, 0x3ffffff, NULL); /* every letter */
	d = NULL;
	CHECK(WF_OK == wf_pcplace_list(0, &d) && d && 26 == d->n);
	wf_dir_free(d);
	d = NULL;
	CHECK(WF_OK == wf_pcplace_list(5, &d) && d && 5 == d->n &&
	        d->truncated);
	wf_dir_free(d);
	CHECK(WF_E_INVAL == wf_pcplace_list(0, NULL));
	wf_pcplace_test_source(false, 0, NULL);
}

static int submits;

static WfStatus count_submit(void *ctx, uint32_t gen, const WfEditRequest *r)
{
	(void)ctx;
	(void)gen;
	(void)r;
	submits++;
	return WF_OK;
}

static const wchar_t *current_name(const WfApp *a)
{
	const WfEntry *e = wf_view_current(&a->cur);

	return e ? e->name : L"";
}

/* Left from C:\ is "This PC" with C: selected; left again stays; Enter goes
 * back into the drive. Nothing can be created there. */
static void navigation(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	wf_pcplace_test_source(true, (1ul << 2) | (1ul << 3), DISTROS);
	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\"));
	CHECK(!wcscmp(a.cwd, L"C:\\"));
	key(&a, WF_KEY_LEFT);
	CHECK(!wcscmp(a.cwd, WF_PCROOT));
	CHECK(!wcscmp(current_name(&a), L"C:"));
	CHECK(4 == a.cur.dir->n);
	key(&a, WF_KEY_LEFT);
	CHECK(!wcscmp(a.cwd, WF_PCROOT));
	key(&a, WF_KEY_ENTER);
	CHECK(!wcscmp(a.cwd, L"C:\\"));

	/* a distribution is above too: its root goes up to its own entry */
	a.confirm_unc_navigation = false; /* the question is about networks */
	wf_app_go(&a, L"\\\\wsl.localhost\\Ubuntu", NULL);
	key(&a, WF_KEY_LEFT);
	if (!wcscmp(a.cwd, WF_PCROOT)) {
		CHECK(!wcscmp(current_name(&a), L"wsl:Ubuntu"));
	}

	/* a name made there reaches no execution port */
	wf_app_go(&a, WF_PCROOT, NULL);
	CHECK(!wcscmp(a.cwd, WF_PCROOT));
	a.edit = (WfEditPort){NULL, count_submit};
	submits = 0;
	ch(&a, 'a');
	ch(&a, '%');
	ch(&a, 'x');
	key(&a, WF_KEY_ENTER);
	CHECK(0 == submits);
	wf_app_destroy(&a);
	wf_pcplace_test_source(false, 0, NULL);
}

/* The title bar names the place: the stand-in path is never shown. Only its left
 * part is read: the mode label at the right says "This PC" too. */
static bool title_has(WfApp *a, const char *needle)
{
	WfFrame *f = wf_frame_new(a->cols, a->rows, &WF_STYLE_BASE);
	char row[512] = "";
	bool has;

	wf_app_draw(a, f);
	for (int x = 0; x < 30 && x < f->cols;
	        x++) { /* the left part: the mode label is at the right */
		strcat_s(row, sizeof row, f->cells[x].utf8);
	}
	has = NULL != strstr(row, needle);
	wf_frame_free(f);
	return has;
}

static void title(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	wf_pcplace_test_source(true, 1ul << 2, DISTROS);
	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
	CHECK(!title_has(&a, "This PC") && title_has(&a, "C:\\Users"));
	wf_app_go(&a, WF_PCROOT, NULL);
	CHECK(title_has(&a, "This PC") && !title_has(&a, "pc.invalid"));
	wf_app_destroy(&a);
	wf_pcplace_test_source(false, 0, NULL);
}

int main(void)
{
	paths();
	listing();
	navigation();
	title();
	return TEST_RESULT();
}

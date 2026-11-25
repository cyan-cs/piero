#include <string.h>

#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"
#include "base/wf_macro.h"

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

/* Synthetic fixture identities are explicit; ordinary fake listings stay
 * unproven. They model the identities supplied by a guarded native listing. */
static void prove_listing(WfApp *a)
{
	WfDir *d = a->cur.dir;
	CHECK(NULL != d);
	if (!d) {
		return;
	}
	d->identity.kind = WF_ID_STRONG;
	d->identity.volume = 7;
	d->identity.id[0] = 11;
	for (size_t i = 0; i < d->n; i++) {
		d->e[i].identity.kind = WF_ID_STRONG;
		d->e[i].identity.volume = 7;
		d->e[i].identity.id[0] = (uint8_t)(i + 20);
	}
}

/* Missing proof must never reach even the release-gate submission path. */
static void missing_operation_identities_are_refused(void)
{
	for (unsigned kind = 0; kind < 4; kind++) {
		WfApp a;
		FakePort fp = {.app = &a};
		CHECK(WF_OK ==
		        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
		if (kind >= 2) {
			prove_listing(&a);
			a.cur.dir->e[a.cur.vis[a.cur.cursor]].identity.kind =
			        WF_ID_NONE;
		}
		ch(&a, 0 == kind ? '%' : 1 == kind ? 'd' : 'R');
		key(&a, WF_KEY_ENTER);
		CHECK(&WF_MODE_NORMAL == a.mode &&
		        strstr(a.msg, "identity unavailable"));
		CHECK(1 == a.cur.nvis && 1 == a.history_n);
		wf_app_destroy(&a);
	}
}

/* Losing a generation or either identity must reject the captured form,
 * including a reload of the same path with matching synthetic IDs. */
static void superseded_operation_forms_are_refused(void)
{
	for (unsigned kind = 0; kind < 7; kind++) {
		WfApp a;
		FakePort fp = {.app = &a};
		WfEntry *entry;
		CHECK(WF_OK ==
		        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
		prove_listing(&a);
		ch(&a, 0 == kind ? '%' : 'R');
		entry = &a.cur.dir->e[a.cur.vis[a.cur.cursor]];
		if (0 == kind) {
			a.nav_gen++;
		} else if (1 == kind) {
			a.cwd_version++;
		} else if (2 == kind) {
			a.cur.dir->identity.id[0] ^= 1;
		} else if (3 == kind) {
			entry->identity.id[0] ^= 1;
		} else if (4 == kind) {
			entry->name[0] = L'D';
		} else if (5 == kind) {
			wf_app_reload(&a);
			prove_listing(&a);
		} else {
			fp.defer = 1;
			wf_app_reload(&a);
		}
		key(&a, WF_KEY_ENTER);
		CHECK(&WF_MODE_NORMAL == a.mode &&
		        strstr(a.msg, "listing changed"));
		CHECK(1 == a.cur.nvis && 1 == a.history_n);
		if (6 == kind) {
			fake_deliver(&fp, 0);
		}
		wf_app_destroy(&a);
	}
}

/* Direct shortcuts must open forms, and every character inside stays text. */
static void direct_create_shortcuts_and_name_boundaries(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
	prove_listing(&a);
	ch(&a, '%');
	CHECK(!strcmp(a.mode->name, "NAME") && strstr(a.msg, "file"));
	if (!strcmp(a.mode->name, "NAME")) {
		ch(&a, 0xD800);
		ch(&a, 0x110000);
		ch(&a, 0);
		CHECK(0 == a.path_input_len && strstr(a.msg, "ERROR NAME"));
		ch(&a, 0x1F600);
		CHECK(2 == a.path_input_len && 2 == a.path_input_cursor);
		key(&a, WF_KEY_LEFT);
		CHECK(0 == a.path_input_cursor);
		key(&a, WF_KEY_RIGHT);
		key(&a, WF_KEY_BACKSPACE);
		CHECK(0 == a.path_input_len);
		for (size_t i = 0; i < 2000; i++) {
			ch(&a, 'n');
		}
		ch(&a, '%');
		ch(&a, 'd');
		CHECK(2002 == a.path_input_len && L'%' == a.path_input[2000] &&
		        L'd' == a.path_input[2001] && 0 == a.path_input[2002]);
		key(&a, WF_KEY_ENTER);
		CHECK(&WF_MODE_NORMAL == a.mode && strstr(a.msg, "BLOCKED"));
	}
	ch(&a, 'd');
	CHECK(!strcmp(a.mode->name, "NAME") && strstr(a.msg, "folder"));
	key(&a, WF_KEY_ESC);
	ch(&a, 'g');
	ch(&a, '%');
	ch(&a, 'd');
	CHECK(!strcmp(a.mode->name, "PATH") && a.go.text &&
	        !wcscmp(a.go.text, L"%d"));
	key(&a, WF_KEY_ESC);
	ch(&a, '/');
	ch(&a, '%');
	ch(&a, 'd');
	CHECK(&WF_MODE_FILTER == a.mode && !wcscmp(a.cur.filter, L"%d"));
	key(&a, WF_KEY_ESC);
	CHECK(!wcscmp(a.cwd, L"C:\\Users") && 1 == a.history_n &&
	        1 == a.cur.nvis);
	wf_app_destroy(&a);
}

/* Removing the modal dispatch must fail: command letters are literal names. */
static void create_and_rename_remain_blocked(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
	prove_listing(&a);
	ch(&a, 'a');
	CHECK(!strcmp(a.mode->name, "CREATE"));
	ch(&a, '%');
	CHECK(!strcmp(a.mode->name, "NAME"));
	if (!strcmp(a.mode->name, "NAME")) {
		ch(&a, 'g');
		ch(&a, 'q');
		ch(&a, 'y');
		ch(&a, 'R');
		CHECK(a.path_input && !wcscmp(a.path_input, L"gqyR"));
		key(&a, WF_KEY_ENTER);
		CHECK(&WF_MODE_NORMAL == a.mode && strstr(a.msg, "BLOCKED"));
	}
	ch(&a, 'a');
	ch(&a, 'd');
	CHECK(!strcmp(a.mode->name, "NAME"));
	key(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode);
	key(&a, WF_KEY_F2);
	CHECK(!strcmp(a.mode->name, "NAME"));
	CHECK(a.path_input && !wcscmp(a.path_input, L"dev"));
	key(&a, WF_KEY_ENTER);
	CHECK(strstr(a.msg, "read-only build") && 1 == a.cur.nvis);
	ch(&a, 'R');
	CHECK(!strcmp(a.mode->name, "NAME"));
	key(&a, WF_KEY_ESC);
	CHECK(!wcscmp(a.cwd, L"C:\\Users") && 1 == a.history_n);
	wf_app_destroy(&a);
}

static int submits;
static WfEditKind submitted_kind;
static wchar_t submitted_name[300];

static WfStatus capture_submit(void *ctx, uint32_t gen, const WfEditRequest *r)
{
	(void)ctx;
	(void)gen;
	submits++;
	submitted_kind = r->kind;
	wcscpy_s(submitted_name, 300, r->new_name);
	return WF_OK;
}

static void type_name(WfApp *a, const wchar_t *text)
{
	for (; *text; text++) {
		ch(a, (uint32_t)*text);
	}
}

/* "test/hello" reaches the execution port whole (the backend makes the folder),
 * as a file or as a folder; a name with an empty part is refused with the form
 * left open; a rename still takes a single name only. */
static void a_new_name_may_name_folders_on_the_way(void)
{
	const wchar_t *refused[] = {L"a//b", L"/a", L"a/", L"a/../b", L"a/NUL"};
	WfApp a;
	FakePort fp = {.app = &a};

	for (int dir = 0; dir < 2; dir++) {
		CHECK(WF_OK ==
		        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
		prove_listing(&a);
		a.edit = (WfEditPort){NULL, capture_submit};
		submits = 0;
		ch(&a, 'a');
		ch(&a, dir ? 'd' : '%');
		CHECK(!strcmp(a.mode->name, "NAME"));
		type_name(&a, L"test/hello");
		key(&a, WF_KEY_ENTER);
		CHECK(1 == submits && !wcscmp(submitted_name, L"test/hello") &&
		        (dir ? WF_EDIT_CREATE_DIR : WF_EDIT_CREATE_FILE) ==
		                submitted_kind);
		wf_app_destroy(&a);
	}
	for (size_t i = 0; i < WF_ARRAY_LEN(refused); i++) {
		CHECK(WF_OK ==
		        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
		prove_listing(&a);
		a.edit = (WfEditPort){NULL, capture_submit};
		submits = 0;
		ch(&a, 'a');
		ch(&a, '%');
		type_name(&a, refused[i]);
		key(&a, WF_KEY_ENTER);
		CHECK(0 == submits && !strcmp(a.mode->name, "NAME"));
		wf_app_destroy(&a);
	}
	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
	prove_listing(&a);
	a.edit = (WfEditPort){NULL, capture_submit};
	submits = 0;
	key(&a, WF_KEY_F2);
	CHECK(!strcmp(a.mode->name, "NAME"));
	type_name(&a, L"/x");
	key(&a, WF_KEY_ENTER);
	CHECK(0 == submits && !strcmp(a.mode->name, "NAME"));
	wf_app_destroy(&a);
}

/* With no execution engine wired, Delete changes nothing, does not consume
 * the selected names, and asks nothing it could not carry out (so there is
 * nothing to dismiss: Esc would clear the selection). */
static void recycle_without_an_engine_changes_nothing(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
	ch(&a, ' ');
	key(&a, WF_KEY_DELETE);
	CHECK(a.copy_jobs == 0 && &WF_MODE_NORMAL == a.mode && a.msg[0]);
	a.now_ms = 700;
	wf_app_tick(&a);
	key(&a, WF_KEY_DELETE);
	CHECK(&WF_MODE_NORMAL == a.mode && a.copy_jobs == 0 && a.msg[0]);
	CHECK(1 == wf_sel_count(&a.sel, a.cwd) && 1 == a.cur.nvis);
	wf_app_destroy(&a);
}

/* A loading listing must not supply operation targets or open a form. */
static void loading_operations_are_refused(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	CHECK(WF_OK == wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users"));
	fp.defer = 1;
	ch(&a, 'r');
	ch(&a, 'a');
	CHECK(&WF_MODE_NORMAL == a.mode && strstr(a.msg, "BLOCKED"));
	ch(&a, '%');
	CHECK(&WF_MODE_NORMAL == a.mode && strstr(a.msg, "BLOCKED"));
	ch(&a, 'd');
	CHECK(&WF_MODE_NORMAL == a.mode && strstr(a.msg, "BLOCKED"));
	ch(&a, 'R');
	CHECK(&WF_MODE_NORMAL == a.mode && strstr(a.msg, "BLOCKED"));
	key(&a, WF_KEY_DELETE);
	CHECK(&WF_MODE_NORMAL == a.mode && strstr(a.msg, "BLOCKED"));
	fake_deliver(&fp, 0);
	wf_app_destroy(&a);
}

int main(void)
{
	missing_operation_identities_are_refused();
	superseded_operation_forms_are_refused();
	direct_create_shortcuts_and_name_boundaries();
	create_and_rename_remain_blocked();
	a_new_name_may_name_folders_on_the_way();
	recycle_without_an_engine_changes_nothing();
	loading_operations_are_refused();
	return TEST_RESULT();
}

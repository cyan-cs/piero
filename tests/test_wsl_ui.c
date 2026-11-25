/*
 * WSL places in the file manager: "wsl:<distribution>" reaches a distribution;
 * creating, renaming, copying, moving and deleting there go through the edit
 * port (no file identity is needed). The distribution list comes from the
 * registry and starts nothing.
 */
#include <stdlib.h>
#include <string.h>

#include "fs/wf_dir.h"
#include "fs/wf_loc.h"
#include "fs/wf_wsl_win.h"
#include "path/wf_smartgo_path.h"
#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

#define CWD L"C:\\Users\\user"

static wchar_t *no_home(void *ctx)
{
	(void)ctx;
	return NULL;
}

static wchar_t *no_known(void *ctx, const wchar_t *alias)
{
	(void)ctx;
	(void)alias;
	return NULL;
}

static wchar_t *no_env(void *ctx, const wchar_t *name)
{
	(void)ctx;
	(void)name;
	return NULL;
}

static const WfSmartEnv env = {NULL, no_home, no_known, no_env};

static WfSmartResult resolve(const wchar_t *text)
{
	WfSmartResult r;

	CHECK(WF_OK ==
	        wf_smart_resolve(&env, CWD, NULL, text, wcslen(text), &r));
	return r;
}

static bool goes_to(const wchar_t *text, const wchar_t *target)
{
	WfSmartResult r = resolve(text);
	bool ok = WF_SG_OK == r.error && r.target && !wcscmp(r.target, target);

	if (!ok) {
		printf("  %ls -> error %d target %ls (wanted %ls)\n", text,
		        r.error, r.target ? r.target : L"-", target);
	}
	wf_smart_result_free(&r);
	return ok;
}

static bool refused(const wchar_t *text, WfSmartError why)
{
	WfSmartResult r = resolve(text);
	bool ok = r.error == why && !r.target;

	wf_smart_result_free(&r);
	return ok;
}

static void wsl_scheme_reaches_a_distribution(void)
{
	CHECK(goes_to(L"wsl:Ubuntu", L"\\\\wsl.localhost\\Ubuntu"));
	CHECK(goes_to(L"WSL:Ubuntu", L"\\\\wsl.localhost\\Ubuntu"));
	CHECK(goes_to(
	        L"wsl:Ubuntu/home/me", L"\\\\wsl.localhost\\Ubuntu\\home\\me"));
	/* Linux case is kept exactly */
	CHECK(goes_to(
	        L"wsl:Ubuntu/Home/ME", L"\\\\wsl.localhost\\Ubuntu\\Home\\ME"));
	CHECK(goes_to(
	        L"\"wsl:Ubuntu/tmp\"", L"\\\\wsl.localhost\\Ubuntu\\tmp"));
	CHECK(goes_to(
	        L"wsl:docker-desktop", L"\\\\wsl.localhost\\docker-desktop"));

	/* no usable distribution name */
	CHECK(refused(L"wsl:", WF_SG_WSL_NAME));
	CHECK(refused(L"wsl:/home", WF_SG_WSL_NAME));
	CHECK(refused(L"wsl:-o", WF_SG_WSL_NAME));
	CHECK(refused(L"wsl:a b", WF_SG_WSL_NAME));
	CHECK(refused(L"wsl:a:b", WF_SG_WSL_NAME));
	CHECK(refused(L"wsl:\u30e6", WF_SG_WSL_NAME));
	/* a drive letter is not mistaken for the scheme */
	CHECK(goes_to(L"C:\\Users", L"C:\\Users"));
}

static void the_completion_position_is_in_what_was_typed(void)
{
	WfSmartResult r = resolve(L"wsl:Ubuntu/ho");

	CHECK(WF_SG_OK == r.error && r.prefix && !wcscmp(r.prefix, L"ho"));
	CHECK(11 == r.prefix_start); /* "wsl:Ubuntu/" is 11 characters */
	CHECK(r.list_dir && !wcscmp(r.list_dir, L"\\\\wsl.localhost\\Ubuntu"));
	wf_smart_result_free(&r);
}

static void the_registry_list_is_usable_in_a_path(void)
{
	WfWslDistro d[16];
	size_t n = wf_wsl_distros(d, 16);
	wchar_t line[64];
	WfLocId id;

	for (size_t i = 0; i < n; i++) {
		char narrow[WF_WSL_NAME_MAX];
		size_t k = 0;

		while (d[i].name[k] && k + 1 < sizeof narrow) {
			CHECK(d[i].name[k] < 128);
			narrow[k] = (char)d[i].name[k];
			k++;
		}
		narrow[k] = 0;
		CHECK(WF_OK == wf_locid_wsl(&id, narrow));
		CHECK(d[i].version <= 2);
	}
	wf_wsl_names_line(line, 64);
	CHECK(n ? line[0] != 0 : 0 == line[0]);
	wf_wsl_names_line(
	        line, 1); /* too small for anything: empty, no overrun */
	CHECK(0 == line[0]);
	CHECK(0 == wf_wsl_distros(d, 0));
	CHECK(0 == wf_wsl_distros(NULL, 4));
}

/* Linux can hold File, file and FILE side by side: all three are listed, in
 * one fixed order, and none is taken for another. */
static void names_that_differ_only_by_case_are_all_kept(void)
{
	static const wchar_t *const names[] = {
	        L"file", L"FILE", L"File", L"fILE"};
	WfDir *d = wf_dir_new();
	size_t seen[4] = {0};

	CHECK(NULL != d);
	if (!d) {
		return;
	}
	for (size_t i = 0; i < 4; i++) {
		CHECK(WF_OK ==
		        wf_dir_add(d, names[i], wcslen(names[i]), 0, 1, 1));
	}
	CHECK(WF_OK == wf_dir_sort(d, WF_SORT_NAME, false));
	CHECK(4 == d->n);
	for (size_t i = 0; i < d->n; i++) {
		for (size_t k = 0; k < 4; k++) {
			seen[k] += !wcscmp(d->e[i].name, names[k]);
		}
	}
	for (size_t k = 0; k < 4; k++) {
		CHECK(1 == seen[k]);
	}
	/* the order does not depend on the order they arrived in */
	{
		WfDir *e = wf_dir_new();

		CHECK(NULL != e);
		for (size_t i = 4; e && i-- > 0;) {
			CHECK(WF_OK ==
			        wf_dir_add(e, names[i], wcslen(names[i]), 0, 1,
			                1));
		}
		if (e) {
			CHECK(WF_OK == wf_dir_sort(e, WF_SORT_NAME, false));
			for (size_t i = 0; i < 4; i++) {
				CHECK(!wcscmp(d->e[i].name, e->e[i].name));
			}
		}
		wf_dir_free(e);
	}
	wf_dir_free(d);
}

typedef struct
{
	unsigned calls;
	uint32_t gen;
	WfEditKind kind;
	wchar_t parent[128], old_name[64], dest[128], new_name[64];
} Spy;

static WfStatus spy_submit(void *ctx, uint32_t gen, const WfEditRequest *req)
{
	Spy *s = ctx;

	s->calls++;
	s->gen = gen;
	s->kind = req->kind;
	wcscpy_s(s->parent, 128, req->parent);
	wcscpy_s(s->old_name, 64, req->old_name ? req->old_name : L"");
	wcscpy_s(s->dest, 128, req->dest_parent ? req->dest_parent : L"");
	wcscpy_s(s->new_name, 64, req->new_name ? req->new_name : L"");
	return WF_OK;
}

static void ch(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	a->mode->on_event(a, &e);
}

static void cwd_is(WfApp *a, const wchar_t *path)
{
	free(a->cwd);
	a->cwd = _wcsdup(path);
	CHECK(NULL != a->cwd);
}

#define ME L"\\\\wsl.localhost\\Ubuntu\\home\\me"

/* The app asks before it enters a network share; the tests answer no
 * question and go straight to the folder. */
static bool start_in(WfApp *a, FakePort *fp, const wchar_t *dir)
{
	bool ok = WF_OK ==
	        wf_app_init(a, 100, 30, fake_port(fp), L"C:\\Users\\dev");

	CHECK(ok);
	a->confirm_unc_navigation = false;
	CHECK(WF_OK == wf_app_navigate(a, dir, NULL, SIZE_MAX));
	return ok;
}


/* A WSL folder can be changed: create and rename are not refused, and copy,
 * move and delete go through the edit port one item at a time. */
static void copy_in_a_distribution_goes_through_the_edit_port(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Spy spy = {0};
	WfEditOutcome ok = {WF_OK, 0, true, NULL, NULL, 0};
	WfEditOutcome taken = {WF_E_INVAL, 0, false, NULL, "that name is taken", 0};

	CHECK(start_in(&a, &fp, ME));
	wf_app_set_edit(&a, (WfEditPort){&spy, spy_submit});
	ch(&a, 'j'); /* a.txt: the first entry is the folder Docs */
	wf_app_cmd_stage(&a, false);
	CHECK(1 == a.wclip.n && !wcscmp(a.wclip.names[0], L"a.txt") &&
	        !a.wclip.move && strstr(a.msg, "COPY: 1 item"));

	wf_app_cmd_paste(&a);
	CHECK(1 == spy.calls && a.edit_busy && a.wseq.active);
	CHECK(WF_EDIT_WSL_COPY == spy.kind && !wcscmp(spy.parent, ME) &&
	        !wcscmp(spy.dest, ME) && !wcscmp(spy.old_name, L"a.txt") &&
	        !wcscmp(spy.new_name, L"a.txt"));
	wf_app_on_edit(&a, spy.gen, &ok, L"a.txt");
	CHECK(!a.edit_busy && !a.wseq.active && strstr(a.msg, "COPY done: 1"));
	CHECK(1 == a.wclip.n); /* a copy can be pasted again */

	wf_app_cmd_paste(&a);
	CHECK(2 == spy.calls);
	wf_app_on_edit(&a, spy.gen, &taken, L"a.txt");
	CHECK(!a.wseq.active && strstr(a.msg, "STOPPED after 0 of 1") &&
	        strstr(a.msg, "that name is taken"));
	wf_app_destroy(&a);
}

static void a_move_stays_inside_one_distribution(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Spy spy = {0};
	WfEditOutcome ok = {WF_OK, 0, true, NULL, NULL, 0};

	CHECK(start_in(&a, &fp, ME));
	wf_app_set_edit(&a, (WfEditPort){&spy, spy_submit});
	ch(&a, 'j');
	wf_app_cmd_stage(&a, true);
	CHECK(a.wclip.move);

	cwd_is(&a, L"\\\\wsl.localhost\\Debian\\home"); /* another distribution */
	wf_app_cmd_paste(&a);
	CHECK(0 == spy.calls && strstr(a.msg, "between places"));
	cwd_is(&a, L"C:\\Users\\dev"); /* a local folder */
	wf_app_cmd_paste(&a);
	CHECK(0 == spy.calls && strstr(a.msg, "between places"));

	cwd_is(&a, L"\\\\wsl.localhost\\Ubuntu\\home\\me\\Docs");
	wf_app_cmd_paste(&a);
	CHECK(1 == spy.calls && WF_EDIT_MOVE == spy.kind &&
	        !wcscmp(spy.parent, ME) &&
	        !wcscmp(spy.dest, L"\\\\wsl.localhost\\Ubuntu\\home\\me\\Docs"));
	wf_app_on_edit(&a, spy.gen, &ok, L"a.txt");
	CHECK(strstr(a.msg, "MOVE done: 1") && 0 == a.wclip.n);
	wf_app_destroy(&a);
}

static void copies_cross_between_a_distribution_and_a_local_folder(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Spy spy = {0};
	WfEditOutcome ok = {WF_OK, 0, true, NULL, NULL, 0};

	/* out of a distribution into a local folder */
	CHECK(start_in(&a, &fp, ME));
	wf_app_set_edit(&a, (WfEditPort){&spy, spy_submit});
	ch(&a, 'j');
	wf_app_cmd_stage(&a, false);
	cwd_is(&a, L"C:\\Users\\dev");
	wf_app_cmd_paste(&a);
	CHECK(1 == spy.calls && WF_EDIT_WSL_COPY == spy.kind &&
	        !wcscmp(spy.parent, ME) && !wcscmp(spy.dest, L"C:\\Users\\dev"));
	wf_app_on_edit(&a, spy.gen, &ok, L"a.txt");
	wf_app_destroy(&a);

	/* from a local folder into a distribution */
	memset(&spy, 0, sizeof spy);
	CHECK(WF_OK ==
	        wf_app_init(&a, 100, 30, fake_port(&fp), L"C:\\Users\\dev"));
	wf_app_set_edit(&a, (WfEditPort){&spy, spy_submit});
	ch(&a, 'j'); /* file2.txt: the first entry is file10.txt */
	wf_app_cmd_stage(&a, false);
	CHECK(1 == a.wclip.n && !wcscmp(a.wclip.dir, L"C:\\Users\\dev"));
	cwd_is(&a, ME);
	wf_app_cmd_paste(&a);
	CHECK(1 == spy.calls && WF_EDIT_WSL_COPY == spy.kind &&
	        !wcscmp(spy.parent, L"C:\\Users\\dev") && !wcscmp(spy.dest, ME));
	wf_app_on_edit(&a, spy.gen, &ok, L"file2.txt");
	CHECK(strstr(a.msg, "COPY done: 1"));

	/* a copy picked locally is not pasted into a distribution once dropped */
	wf_app_xfer_drop_slot(&a);
	wf_app_cmd_paste(&a);
	CHECK(1 == spy.calls);
	wf_app_destroy(&a);
}

static void delete_asks_first_and_is_permanent(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Spy spy = {0};
	WfEditOutcome ok = {WF_OK, 0, true, NULL, NULL, 0};
	WfEvent del = {WF_EV_KEY, 0, WF_KEY_DELETE, 0};

	CHECK(start_in(&a, &fp, ME));
	wf_app_set_edit(&a, (WfEditPort){&spy, spy_submit});
	ch(&a, 'j');
	wf_app_cmd_recycle(&a, &del);
	CHECK(0 == spy.calls); /* nothing before the answer */
	CHECK(a.confirm.on_yes && 1 == a.wseq.n);
	a.confirm.state = WF_CONF_ARMED; /* the guard time has passed */
	ch(&a, 'n');
	CHECK(0 == spy.calls);
	wf_app_cmd_recycle(&a, &del);
	a.confirm.state = WF_CONF_ARMED;
	ch(&a, 'y');
	CHECK(1 == spy.calls && WF_EDIT_WSL_DELETE == spy.kind &&
	        !wcscmp(spy.parent, ME) && !wcscmp(spy.old_name, L"a.txt"));
	wf_app_on_edit(&a, spy.gen, &ok, L"a.txt");
	CHECK(strstr(a.msg, "DELETE done: 1"));
	wf_app_destroy(&a);
}

static void create_and_rename_are_open_in_a_distribution(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Spy spy = {0};

	CHECK(start_in(&a, &fp, ME));
	wf_app_set_edit(&a, (WfEditPort){&spy, spy_submit});
	wf_app_cmd_create_kind(&a, true);
	CHECK(&WF_MODE_NORMAL != a.mode && a.name_form.bound);
	wf_app_destroy(&a);

	CHECK(start_in(&a, &fp, ME));
	wf_app_set_edit(&a, (WfEditPort){&spy, spy_submit});
	ch(&a, 'j');
	wf_app_cmd_rename(&a);
	CHECK(&WF_MODE_NORMAL != a.mode && a.name_form.bound);
	wf_app_destroy(&a);
}

static void enter(WfApp *a)
{
	WfEvent e = {WF_EV_KEY, 0, WF_KEY_ENTER, 0};

	a->mode->on_event(a, &e);
}

/* `u` puts back the last create, rename or move in a distribution. */
static void undo_puts_back_a_rename_a_create_and_a_move(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Spy spy = {0};
	WfEditOutcome ok = {WF_OK, 0, true, NULL, NULL, 0};

	CHECK(start_in(&a, &fp, ME));
	wf_app_set_edit(&a, (WfEditPort){&spy, spy_submit});

	/* rename a.txt -> a.txtX, then undo */
	ch(&a, 'j');
	wf_app_cmd_rename(&a);
	{
		WfEvent end = {WF_EV_KEY, 0, WF_KEY_END, 0};

		a.mode->on_event(&a, &end); /* leave the selected name: append */
	}
	ch(&a, 'X');
	enter(&a);
	CHECK(1 == spy.calls && WF_EDIT_RENAME == spy.kind &&
	        !wcscmp(spy.old_name, L"a.txt") && !wcscmp(spy.new_name, L"a.txtX"));
	wf_app_on_edit(&a, spy.gen, &ok, L"a.txtX");
	CHECK(a.wundo.valid);
	wf_app_cmd_undo(&a);
	CHECK(2 == spy.calls && WF_EDIT_RENAME == spy.kind &&
	        !wcscmp(spy.parent, ME) && !wcscmp(spy.old_name, L"a.txtX") &&
	        !wcscmp(spy.new_name, L"a.txt"));
	wf_app_on_edit(&a, spy.gen, &ok, L"a.txt");
	CHECK(!a.wundo.valid && strstr(a.msg, "UNDO done: 1"));

	/* a new file, then undo takes it away only while it is empty */
	wf_app_cmd_create_kind(&a, true);
	ch(&a, 'n');
	enter(&a);
	CHECK(3 == spy.calls && WF_EDIT_CREATE_FILE == spy.kind);
	wf_app_on_edit(&a, spy.gen, &ok, L"n");
	CHECK(a.wundo.valid);
	wf_app_cmd_undo(&a);
	CHECK(4 == spy.calls && WF_EDIT_WSL_REMOVE_EMPTY == spy.kind &&
	        !wcscmp(spy.old_name, L"n"));
	wf_app_on_edit(&a, spy.gen, &ok, L"n");
	CHECK(!a.wundo.valid);

	/* a move into a sub folder, then back */
	cwd_is(&a, ME);
	ch(&a, 'j');
	wf_app_cmd_stage(&a, true);
	cwd_is(&a, L"\\\\wsl.localhost\\Ubuntu\\home\\me\\Docs");
	wf_app_cmd_paste(&a);
	CHECK(5 == spy.calls && WF_EDIT_MOVE == spy.kind);
	wf_app_on_edit(&a, spy.gen, &ok, L"b.txt");
	CHECK(a.wundo.valid && 1 == a.wundo.n);
	wf_app_cmd_undo(&a);
	CHECK(6 == spy.calls && WF_EDIT_MOVE == spy.kind &&
	        !wcscmp(spy.parent, L"\\\\wsl.localhost\\Ubuntu\\home\\me\\Docs") &&
	        !wcscmp(spy.dest, ME) && !wcscmp(spy.old_name, L"b.txt"));
	wf_app_on_edit(&a, spy.gen, &ok, L"b.txt");
	CHECK(!a.wundo.valid);

	/* a copy and a delete leave nothing to undo */
	wf_app_destroy(&a);
}

/* An Undo of several moved items that stops half way has put the first ones
 * back: the record keeps only the rest, so the next Undo goes on from there
 * instead of asking for an item that is no longer in the place it is sought. */
static void a_stopped_undo_keeps_only_what_is_left(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Spy spy = {0};
	WfEditOutcome ok = {WF_OK, 0, true, NULL, NULL, 0};
	WfEditOutcome taken = {
	        WF_E_INVAL, 0, false, NULL, "that name is taken", 0};

	CHECK(start_in(&a, &fp, ME));
	wf_app_set_edit(&a, (WfEditPort){&spy, spy_submit});
	ch(&a, 'j'); /* a.txt */
	ch(&a, ' '); /* selected; the cursor goes on to b.txt */
	ch(&a, ' ');
	wf_app_cmd_stage(&a, true);
	CHECK(2 == a.wclip.n);
	cwd_is(&a, L"\\\\wsl.localhost\\Ubuntu\\home\\me\\Docs");
	wf_app_cmd_paste(&a);
	CHECK(1 == spy.calls && !wcscmp(spy.old_name, L"a.txt"));
	wf_app_on_edit(&a, spy.gen, &ok, L"a.txt");
	CHECK(2 == spy.calls && !wcscmp(spy.old_name, L"b.txt"));
	wf_app_on_edit(&a, spy.gen, &ok, L"b.txt");
	CHECK(a.wundo.valid && 2 == a.wundo.n);

	wf_app_cmd_undo(&a);
	CHECK(3 == spy.calls && !wcscmp(spy.old_name, L"a.txt"));
	wf_app_on_edit(&a, spy.gen, &ok, L"a.txt");
	CHECK(4 == spy.calls && !wcscmp(spy.old_name, L"b.txt"));
	wf_app_on_edit(&a, spy.gen, &taken, L"b.txt");
	CHECK(!a.wseq.active && strstr(a.msg, "STOPPED after 1 of 2"));
	CHECK(a.wundo.valid && 1 == a.wundo.n &&
	        !wcscmp(a.wundo.to[0], L"b.txt") &&
	        !wcscmp(a.wundo.from[0], L"b.txt"));

	wf_app_cmd_undo(&a); /* goes on with b.txt, not with a.txt again */
	CHECK(5 == spy.calls && !wcscmp(spy.old_name, L"b.txt"));
	wf_app_on_edit(&a, spy.gen, &ok, L"b.txt");
	CHECK(!a.wundo.valid && strstr(a.msg, "UNDO done: 1"));
	wf_app_destroy(&a);
}

static void a_running_copy_shows_how_far_it_is(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Spy spy = {0};
	WfBuf s;

	CHECK(start_in(&a, &fp, ME));
	wf_app_set_edit(&a, (WfEditPort){&spy, spy_submit});
	ch(&a, 'j');
	wf_app_cmd_stage(&a, false);
	wf_app_cmd_paste(&a);
	CHECK(a.wseq.active);
	wf_buf_init(&s);
	wf_app_xfer_status(&a, &s);
	CHECK(strstr(wf_buf_cstr(&s), "COPY 1/1"));
	wf_buf_free(&s);
	CHECK(wf_app_wsl_next_ms(&a) < 1000);
	wf_app_destroy(&a);
}

/* Opening a WSL file with another program stays off. */
static void other_programs_do_not_open_distribution_files(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	CHECK(start_in(&a, &fp, ME));
	a.msg[0] = 0;
	ch(&a, 'j');
	wf_app_open_selected(&a);
	CHECK(!strcmp(a.msg, wf_msg(MSG_WSL_READONLY)) ||
	        strstr(a.msg, "another program"));
	wf_app_destroy(&a);
}

/* The status line says where the user is. */
static bool screen_says(const WfApp *a, const char *needle)
{
	WfFrame *f = wf_frame_new(a->cols, a->rows, &(WfStyle){0, 0, 0});
	bool found = false;

	CHECK(NULL != f);
	if (f) {
		wf_app_draw(a, f);
	}
	for (int y = 0; f && y < f->rows && !found; y++) {
		char row[1024] = "";

		for (int x = 0; x < f->cols; x++) {
			strcat_s(row, sizeof row,
			        f->cells[y * f->cols + x].utf8);
		}
		found = NULL != strstr(row, needle);
	}
	wf_frame_free(f);
	return found;
}

static void the_status_line_says_wsl_in_a_distribution(void)
{
	WfApp a;
	FakePort fp = {.app = &a};

	CHECK(WF_OK == wf_app_init(&a, 100, 30, fake_port(&fp), CWD));
	CHECK(!screen_says(&a, "WSL"));
	cwd_is(&a, L"\\\\wsl.localhost\\Ubuntu");
	CHECK(screen_says(&a, "WSL"));
	CHECK(!screen_says(&a, "read-only"));
	wf_app_destroy(&a);
}

int main(void)
{
	wsl_scheme_reaches_a_distribution();
	the_completion_position_is_in_what_was_typed();
	the_registry_list_is_usable_in_a_path();
	names_that_differ_only_by_case_are_all_kept();
	the_status_line_says_wsl_in_a_distribution();
	copy_in_a_distribution_goes_through_the_edit_port();
	a_move_stays_inside_one_distribution();
	copies_cross_between_a_distribution_and_a_local_folder();
	delete_asks_first_and_is_permanent();
	create_and_rename_are_open_in_a_distribution();
	undo_puts_back_a_rename_a_create_and_a_move();
	a_stopped_undo_keeps_only_what_is_left();
	a_running_copy_shows_how_far_it_is();
	other_programs_do_not_open_distribution_files();
	return TEST_RESULT();
}

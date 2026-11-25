/* `--continue` in the application (DR-DRAFT-018): what is captured of the
 * screen, and how a saved session is put back through the ordinary navigation
 * (the listing, the generation that drops an old answer, the Navigation
 * Memory, the selection, the preview's own restore, the confirmation for a
 * network share). The file system is a fake, so no disk is touched. */
#include <stdlib.h>
#include <string.h>

#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

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

/* Delivers every queued listing, in order, including ones the delivery
 * queues. */
static void settle(FakePort *fp)
{
	for (int guard = 0; fp->nq && guard < 16; guard++) {
		int n = fp->nq;

		for (int i = 0; i < n; i++) {
			fake_deliver(fp, i);
		}
		memmove(fp->q, fp->q + n,
		        (size_t)(fp->nq - n) * sizeof fp->q[0]);
		fp->nq -= n;
	}
}

static void start(WfApp *a, FakePort *fp, const wchar_t *at)
{
	fp->defer = 1;
	CHECK(WF_OK == wf_app_init(a, 120, 30, fake_port(fp), at));
	settle(fp);
}

static const wchar_t *cur_name(WfApp *a)
{
	const WfEntry *e = wf_view_current(&a->cur);

	return e ? e->name : L"(none)";
}

static WfSession saved(const wchar_t *dir, const wchar_t *entry)
{
	WfSession s;

	memset(&s, 0, sizeof s);
	s.dir = _wcsdup(dir);
	s.entry = entry ? _wcsdup(entry) : NULL;
	return s;
}

static void pick(WfSession *s, const wchar_t *name)
{
	s->selected =
	        realloc(s->selected, (s->n_selected + 1) * sizeof *s->selected);
	s->selected[s->n_selected++] = _wcsdup(name);
}

static void what_is_on_screen_is_captured_as_names(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfSession s;

	start(&a, &fp, L"C:\\Users\\dev");
	ch(&a, 'j'); /* file2.txt */
	ch(&a, ' '); /* marks it, the cursor goes on */
	CHECK(wf_app_session_capture(&a, &s));
	CHECK(!wcscmp(s.dir, L"C:\\Users\\dev"));
	CHECK(s.entry && !wcscmp(s.entry, cur_name(&a)));
	CHECK(s.cursor == a.cur.cursor);
	CHECK(1 == s.n_selected && !wcscmp(s.selected[0], L"file2.txt"));
	CHECK(WF_SESSION_PANE_LIST == s.pane);
	wf_session_free(&s);
	wf_app_destroy(&a);
}

static void nothing_is_captured_while_a_listing_is_being_replaced(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfSession s;

	start(&a, &fp, L"C:\\Users\\dev");
	wf_app_go(&a, L"C:\\Pics", NULL); /* queued, not answered yet */
	CHECK(!wf_app_session_capture(&a, &s));
	CHECK(!s.dir && !s.entry && !s.selected);
	settle(&fp);
	CHECK(wf_app_session_capture(&a, &s));
	CHECK(!wcscmp(s.dir, L"C:\\Pics"));
	wf_session_free(&s);
	wf_app_destroy(&a);
}

static void a_saved_session_is_put_back_through_the_listing(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfSession s = saved(L"C:\\Pics", L"b.png");

	start(&a, &fp, L"C:\\Users\\dev");
	s.cursor = 2;
	pick(&s, L"a.png");
	pick(&s, L"gone.txt"); /* not in the folder any more */
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	CHECK(0 == wcscmp(a.cwd, L"C:\\Users\\dev")); /* until the listing */
	settle(&fp);
	CHECK(!wcscmp(a.cwd, L"C:\\Pics"));
	CHECK(!wcscmp(cur_name(&a), L"b.png"));
	/* the selection is the selection: what is gone was pruned by the listing */
	CHECK(wf_nameset_has(wf_sel_folder(&a.sel, L"C:\\Pics"), L"a.png"));
	CHECK(!wf_nameset_has(wf_sel_folder(&a.sel, L"C:\\Pics"), L"gone.txt"));
	CHECK(NULL != strstr(a.msg, "Session restored"));
	CHECK(!a.session_restore.active);
	wf_session_free(&s);
	wf_app_destroy(&a);
}

static void the_cursor_follows_the_name_not_the_row(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	/* the row it had was 0 and the entry is now at row 3: the name wins */
	WfSession s = saved(L"C:\\Pics", L"note.txt");

	start(&a, &fp, L"C:\\Users\\dev");
	s.cursor = 0;
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	settle(&fp);
	CHECK(!wcscmp(cur_name(&a), L"note.txt"));
	wf_session_free(&s);
	wf_app_destroy(&a);
}

static void a_vanished_entry_leaves_the_cursor_near_where_it_was(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfSession s = saved(L"C:\\Pics", L"deleted-since.png");

	start(&a, &fp, L"C:\\Users\\dev");
	s.cursor = 2; /* the row it had: still a row of the folder */
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	settle(&fp);
	CHECK(!wcscmp(a.cwd, L"C:\\Pics") && 2 == a.cur.cursor);
	wf_session_free(&s);
	wf_app_destroy(&a);
}

static void a_folder_that_is_gone_goes_to_the_nearest_one_above(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfSession s = saved(L"C:\\Users\\dev\\Gone\\deep", L"x.txt");

	start(&a, &fp, L"C:\\Pics");
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	settle(&fp); /* deep: no; Gone: no; dev: yes */
	CHECK(!wcscmp(a.cwd, L"C:\\Users\\dev"));
	CHECK(NULL != strstr(a.msg, "nearest folder"));
	CHECK(!a.session_restore.active);
	/* a folder that cannot be read goes the same way */
	{
		WfSession t = saved(L"C:\\secret", NULL);

		CHECK(WF_OK == wf_app_session_restore(&a, &t));
		settle(&fp);
		CHECK(!wcscmp(a.cwd, L"C:\\"));
		wf_session_free(&t);
	}
	wf_session_free(&s);
	wf_app_destroy(&a);
}

static void when_even_the_root_is_gone_the_screen_is_left_alone(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfSession s = saved(L"D:\\nowhere", NULL);

	start(&a, &fp, L"C:\\Pics");
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	settle(&fp); /* D:\nowhere: no; D:\: no; nothing above */
	CHECK(!wcscmp(a.cwd, L"C:\\Pics"));
	CHECK(!a.session_restore.active && !a.want_path);
	wf_session_free(&s);
	wf_app_destroy(&a);
}

static void an_old_answer_cannot_overwrite_the_restored_state(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfSession s = saved(L"C:\\Pics", L"b.png");

	fp.defer = 1;
	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	/* the start folder's listing has not arrived when the session is asked
	 * for; the session's arrives first, the old one after it */
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	CHECK(2 == fp.nq);
	fake_deliver(&fp, 1);
	fake_deliver(&fp, 0);
	CHECK(!wcscmp(a.cwd, L"C:\\Pics"));
	CHECK(!wcscmp(cur_name(&a), L"b.png"));
	wf_session_free(&s);
	wf_app_destroy(&a);
}

static void going_elsewhere_first_ends_the_restore(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfSession s = saved(L"C:\\Pics", L"note.txt");

	start(&a, &fp, L"C:\\Users\\dev");
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	wf_app_go(&a, L"C:\\Users", NULL); /* the person's own move wins */
	fake_deliver(&fp, fp.nq - 1);
	CHECK(!wcscmp(a.cwd, L"C:\\Users"));
	CHECK(!a.session_restore.active);
	/* ... and the Pics listing, now old, changes nothing */
	fake_deliver(&fp, fp.nq - 2);
	CHECK(!wcscmp(a.cwd, L"C:\\Users"));
	wf_session_free(&s);
	wf_app_destroy(&a);
}

static void the_preview_comes_back_only_for_the_file_it_was_about(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfSession s = saved(L"C:\\Pics", L"b.png");

	start(&a, &fp, L"C:\\Users\\dev");
	s.pane = WF_SESSION_PANE_PREVIEW;
	s.pv_top = 5;
	s.pv_left = 2;
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	settle(&fp);
	CHECK(&WF_MODE_PREVIEW_PANE == a.mode);
	/* the scroll goes through the preview's own restore */
	CHECK(a.preview_keep.path && 5 == a.preview_keep.top &&
	        2 == a.preview_keep.left);
	CHECK(NULL != wcsstr(a.preview_keep.path, L"b.png"));
	key(&a, WF_KEY_ESC);
	/* the cursor is on another entry than the one saved: the list has the
	 * keys, nothing is kept for a file that is not under the cursor */
	wf_session_free(&s);
	s = saved(L"C:\\Pics", L"vanished.png");
	s.cursor =
	        1; /* a file (a.png) lands under the cursor, not the saved name */
	s.pane = WF_SESSION_PANE_FULL;
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	settle(&fp);
	CHECK(!wcscmp(cur_name(&a), L"a.png") && &WF_MODE_NORMAL == a.mode);
	/* a folder under the cursor never opens a pane */
	wf_session_free(&s);
	s = saved(L"C:\\Pics", L"Sub");
	s.pane = WF_SESSION_PANE_PREVIEW;
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	settle(&fp);
	CHECK(&WF_MODE_NORMAL == a.mode);
	/* the full preview */
	wf_session_free(&s);
	s = saved(L"C:\\Pics", L"a.png");
	s.pane = WF_SESSION_PANE_FULL;
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	settle(&fp);
	CHECK(&WF_MODE_PREVIEW == a.mode);
	wf_session_free(&s);
	wf_app_destroy(&a);
}

static void a_network_share_is_still_asked_about(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfSession s = saved(L"\\\\server\\share\\x", NULL);

	start(&a, &fp, L"C:\\Pics");
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	/* the question of the ordinary navigation, not a silent connection */
	CHECK(&WF_MODE_CONFIRM == a.mode);
	CHECK(0 == fp.nq);
	key(&a, WF_KEY_ESC); /* the person says no: nothing is contacted */
	CHECK(!wcscmp(a.cwd, L"C:\\Pics") && 0 == fp.nq);
	wf_session_free(&s);
	wf_app_destroy(&a);
}

static void a_session_that_is_not_a_place_is_refused_before_anything_moves(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	WfSession s = saved(L"relative\\path", NULL);

	start(&a, &fp, L"C:\\Pics");
	CHECK(WF_E_INVAL == wf_app_session_restore(&a, &s));
	CHECK(!a.want_path && !a.session_restore.active && 0 == fp.nq);
	wf_session_free(&s);
	s = saved(L"C:\\a\\..\\b", NULL);
	CHECK(WF_E_INVAL == wf_app_session_restore(&a, &s));
	CHECK(!a.want_path && 0 == fp.nq);
	wf_session_free(&s);
	wf_app_destroy(&a);
}

typedef struct
{
	unsigned opens, gotos;
	wchar_t opened[260];
	int refuse;
	WfSessionEdit placed, went;
} FakeEditor;

static int fe_open(void *ctx, const wchar_t *path, char *why, size_t cap)
{
	FakeEditor *f = ctx;

	f->opens++;
	wcsncpy_s(f->opened, 260, path, _TRUNCATE);
	if (f->refuse) {
		snprintf(why, cap, "refused");
	}
	return f->refuse;
}

static bool fe_place(void *ctx, WfSessionEdit *out)
{
	FakeEditor *f = ctx;

	*out = f->placed;
	out->path = _wcsdup(f->placed.path);
	return true;
}

static void fe_goto(void *ctx, const WfSessionEdit *in)
{
	FakeEditor *f = ctx;

	f->gotos++;
	f->went = *in;
}

static void use_fake_editor(WfApp *a, FakeEditor *f)
{
	memset(f, 0, sizeof *f);
	a->editor =
	        (WfEditorPort){f, fe_open, NULL, NULL, NULL, fe_place, fe_goto};
}

static void the_editor_is_part_of_what_is_captured(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	FakeEditor fe;
	WfSession s;

	start(&a, &fp, L"C:\\Pics");
	use_fake_editor(&a, &fe);
	fe.placed = (WfSessionEdit){L"C:\\Pics\\note.txt", 4, 2, 1, 0, true};
	CHECK(wf_app_session_capture(&a, &s) && !s.has_edit); /* not showing */
	wf_session_free(&s);
	a.mode = &WF_MODE_EDITOR;
	CHECK(wf_app_session_capture(&a, &s) && s.has_edit);
	CHECK(!wcscmp(s.edit.path, L"C:\\Pics\\note.txt") && 4 == s.edit.line &&
	        2 == s.edit.col && 1 == s.edit.top && s.edit.modified);
	CHECK(!wcscmp(
	        s.dir, L"C:\\Pics")); /* the folder behind it is kept too */
	wf_session_free(&s);
	a.mode = &WF_MODE_NORMAL;
	wf_app_destroy(&a);
}

static void the_editor_is_opened_by_the_ordinary_call_and_the_place_put_back(
        void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	FakeEditor fe;
	WfSession s = saved(L"C:\\Pics", L"note.txt");

	start(&a, &fp, L"C:\\Users\\dev");
	use_fake_editor(&a, &fe);
	s.has_edit = true;
	s.edit = (WfSessionEdit){
	        _wcsdup(L"C:\\Pics\\note.txt"), 9, 3, 2, 1, true};
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	CHECK(0 == fe.opens); /* not before the folder is there */
	settle(&fp);
	CHECK(1 == fe.opens && !wcscmp(fe.opened, L"C:\\Pics\\note.txt"));
	CHECK(&WF_MODE_EDITOR == a.mode);
	CHECK(1 == fe.gotos && 9 == fe.went.line && 3 == fe.went.col &&
	        2 == fe.went.top && 1 == fe.went.left);
	CHECK(!a.session_restore.active);
	wf_session_free(&s);
	a.mode = &WF_MODE_NORMAL;
	wf_app_destroy(&a);
}

static void a_file_that_is_not_in_the_folder_is_not_opened(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	FakeEditor fe;
	WfSession s = saved(L"C:\\Pics", NULL);

	start(&a, &fp, L"C:\\Users\\dev");
	use_fake_editor(&a, &fe);
	s.has_edit = true; /* the editor had a file of another folder open */
	s.edit = (WfSessionEdit){
	        _wcsdup(L"C:\\Users\\dev\\file2.txt"), 1, 1, 0, 0, false};
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	settle(&fp);
	CHECK(0 == fe.opens && &WF_MODE_NORMAL == a.mode);
	CHECK(!wcscmp(a.cwd, L"C:\\Pics"));
	wf_session_free(&s);
	wf_app_destroy(&a);
}

static void the_editor_refuses_and_so_does_the_restore(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	FakeEditor fe;
	WfSession s = saved(L"C:\\Pics", NULL);

	start(&a, &fp, L"C:\\Users\\dev");
	use_fake_editor(&a, &fe);
	fe.refuse = 1;
	s.has_edit = true;
	s.edit = (WfSessionEdit){
	        _wcsdup(L"C:\\Pics\\note.txt"), 1, 1, 0, 0, false};
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	settle(&fp);
	CHECK(1 == fe.opens && 0 == fe.gotos && &WF_MODE_NORMAL == a.mode);
	CHECK(NULL != strstr(a.msg, "refused"));
	wf_session_free(&s);
	wf_app_destroy(&a);
}

static void a_link_or_a_folder_is_never_opened_in_the_editor(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	FakeEditor fe;
	WfSession s = saved(L"C:\\Pics", NULL);

	start(&a, &fp, L"C:\\Users\\dev");
	use_fake_editor(&a, &fe);
	s.has_edit = true; /* "Sub" is a folder of the fake file system */
	s.edit = (WfSessionEdit){_wcsdup(L"C:\\Pics\\Sub"), 1, 1, 0, 0, false};
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	settle(&fp);
	CHECK(0 == fe.opens && &WF_MODE_NORMAL == a.mode);
	wf_session_free(&s);
	/* the file is not in the listing any more: nothing is opened either */
	s = saved(L"C:\\Pics", NULL);
	s.has_edit = true;
	s.edit = (WfSessionEdit){
	        _wcsdup(L"C:\\Pics\\vanished.txt"), 1, 1, 0, 0, false};
	CHECK(WF_OK == wf_app_session_restore(&a, &s));
	settle(&fp);
	CHECK(0 == fe.opens && &WF_MODE_NORMAL == a.mode);
	wf_session_free(&s);
	wf_app_destroy(&a);
}

int main(void)
{
	what_is_on_screen_is_captured_as_names();
	nothing_is_captured_while_a_listing_is_being_replaced();
	a_saved_session_is_put_back_through_the_listing();
	the_cursor_follows_the_name_not_the_row();
	a_vanished_entry_leaves_the_cursor_near_where_it_was();
	a_folder_that_is_gone_goes_to_the_nearest_one_above();
	when_even_the_root_is_gone_the_screen_is_left_alone();
	an_old_answer_cannot_overwrite_the_restored_state();
	going_elsewhere_first_ends_the_restore();
	the_preview_comes_back_only_for_the_file_it_was_about();
	a_network_share_is_still_asked_about();
	a_session_that_is_not_a_place_is_refused_before_anything_moves();
	the_editor_is_part_of_what_is_captured();
	the_editor_is_opened_by_the_ordinary_call_and_the_place_put_back();
	a_file_that_is_not_in_the_folder_is_not_opened();
	the_editor_refuses_and_so_does_the_restore();
	a_link_or_a_folder_is_never_opened_in_the_editor();
	return TEST_RESULT();
}

/* Deterministic clock/port tests: no sleeps, filesystem writes or decoder bypass. */
#include <stdlib.h>
#include <string.h>

#include "ui/wf_app_priv.h"
#include "ui/wf_app_settings_priv.h"
#include "render/wf_graphics.h"
#include "text/wf_text.h"
#include "edit/wf_eenc.h"
#include "wf_test.h"

static WfApp app;
static unsigned text_requests;
static uint64_t file_time;
static unsigned file_identity = 2;
static unsigned root_identity = 1;
static bool defer_listing;

static WfFileId identity(unsigned n)
{
	WfFileId id = {0};
	id.kind = WF_ID_STRONG;
	id.volume = 1;
	memcpy(id.id, &n, sizeof n);
	return id;
}

static WfDir *listing(void)
{
	WfDir *d = wf_dir_new();
	CHECK(d != NULL);
	if (!d) {
		return NULL;
	}
	d->identity = identity(root_identity);
	CHECK(wf_dir_add(d, L"read.txt", 8, 0, 20, file_time) == WF_OK);
	d->e[0].identity = identity(file_identity);
	CHECK(wf_dir_add(d, L"other.txt", 9, 0, 20, 0) == WF_OK);
	d->e[1].identity = identity(3);
	return d;
}

static WfStatus list_submit(
        void *ctx, uint32_t gen, const wchar_t *path, const wchar_t *parent)
{
	(void)ctx;
	(void)path;
	(void)parent;
	if (defer_listing) {
		return WF_OK;
	}
	wf_app_on_listing(&app, gen, WF_OK, listing(), NULL);
	return WF_OK;
}

static WfStatus text_submit(
        void *ctx, uint32_t gen, const wchar_t *path, bool explicit_load)
{
	(void)ctx;
	(void)gen;
	(void)path;
	(void)explicit_load;
	text_requests++;
	return WF_OK;
}

static WfPreview *text(const char *s)
{
	WfPreview *p = NULL;
	CHECK(wf_preview_decode(s, strlen(s), false, &p) == WF_OK);
	return p;
}

static void open_app(void)
{
	memset(&app, 0, sizeof app);
	file_time = 0;
	file_identity = 2;
	root_identity = 1;
	defer_listing = false;
	text_requests = 0;
	CHECK(wf_app_init(&app, 120, 30, (WfFsPort){NULL, list_submit},
	              L"C:\\fixture") == WF_OK);
	wf_app_set_preview(&app, (WfPreviewPort){NULL, text_submit, NULL});
	wf_app_on_preview(&app, app.preview_gen, text("first\nsecond\n"));
	CHECK(text_requests == 1 && app.preview && !app.preview_busy);
}

static void unchanged_listing(void)
{
	open_app();
	WfPreview *shown = app.preview;
	for (unsigned i = 0; i < 30; i++) {
		app.now_ms = (uint64_t)i * 1000;
		CHECK(wf_app_navigate(&app, app.cwd, NULL, SIZE_MAX) == WF_OK);
	}
	CHECK(text_requests == 1);
	CHECK(app.preview == shown && !app.preview_busy);
	printf("unchanged_listing_requests,%u\n", text_requests - 1);
	wf_app_destroy(&app);
}

#ifndef WF_STABILITY_BASELINE
static bool frame_has(const WfFrame *f, const char *needle)
{
	WfBuf row = {0};
	bool found = false;
	for (int y = 0; y < f->rows; y++) {
		wf_buf_clear(&row);
		for (int x = 0; x < f->cols; x++) {
			const WfCell *c =
			        &f->cells[(size_t)y * (size_t)f->cols +
			                (size_t)x];
			wf_buf_puts(&row, c->utf8);
		}
		found |= strstr(wf_buf_cstr(&row), needle) != NULL;
	}
	wf_buf_free(&row);
	return found;
}

static void idle_and_revalidation(void)
{
	open_app();
	WfFrame *before = wf_frame_new(120, 30, &WF_STYLE_BASE);
	WfFrame *after = wf_frame_new(120, 30, &WF_STYLE_BASE);
	WfBuf output = {0};
	CHECK(before && after);
	wf_app_draw(&app, before);
	uint64_t clears = app.preview_stats.cleared_snapshots;
	for (uint64_t time = 0; time <= 30000; time += 10) {
		app.now_ms = time;
		CHECK(wf_app_tick(&app) == WF_ACT_NONE);
		CHECK(wf_app_timeout(&app) == WF_WAIT_FOREVER);
	}
	wf_app_draw(&app, after);
	CHECK(wf_graphics_present(before, after, &output) == WF_OK &&
	        !output.len);
	CHECK(text_requests == 1 &&
	        app.preview_stats.cleared_snapshots == clears);
	CHECK(!frame_has(after, "Loading"));
	WfPreview *shown = app.preview;
	defer_listing = true;
	wf_app_refresh(&app, true);
	wf_app_draw(&app, after);
	/* the folder is read again where it is: nothing on the screen says so,
	 * and not one cell of it changes (no "updating", no LOADING) */
	{
		WfBuf quiet = {0};

		CHECK(app.want_path && !frame_has(after, "previous view") &&
		        !frame_has(after, "Updating") &&
		        !frame_has(after, "LOADING") &&
		        frame_has(after, "first"));
		CHECK(wf_graphics_present(before, after, &quiet) == WF_OK &&
		        !quiet.len);
		wf_buf_free(&quiet);
	}
	defer_listing = false;
	wf_app_on_listing(&app, app.nav_gen, WF_OK, listing(), NULL);
	CHECK(app.preview_busy && app.preview == shown && text_requests == 2);
	wf_app_draw(&app, after);
	/* the preview is read again too: the view that was there stays, unlabelled */
	{
		WfBuf quiet = {0};

		CHECK(!frame_has(after, "previous view") &&
		        !frame_has(after, "Updating") &&
		        frame_has(after, "first"));
		CHECK(wf_graphics_present(before, after, &quiet) == WF_OK &&
		        !quiet.len);
		wf_buf_free(&quiet);
	}
	wf_app_on_preview(&app, app.preview_gen, text("first\nsecond\n"));
	CHECK(app.preview == shown && !app.preview_busy);
	file_time++;
	wf_app_refresh(&app, false);
	CHECK(app.preview_busy && app.preview == shown && text_requests == 3);
	wf_app_on_preview(&app, app.preview_gen, text("changed\n"));
	CHECK(app.preview && !strcmp(app.preview->text.data, "changed"));
	file_identity++;
	wf_app_refresh(&app, false);
	CHECK(app.preview_busy && !app.preview && text_requests == 4);
	uint32_t old = app.preview_gen;
	app.cur.cursor = 1;
	wf_app_preview_sync(&app, false);
	wf_app_on_preview(&app, old, text("wrong file\n"));
	CHECK(!app.preview && app.preview_busy);
	uint32_t version = app.cwd_version;
	root_identity++;
	wf_app_refresh(&app, false);
	CHECK(app.cwd_version == version + 1 && app.preview_busy &&
	        !app.preview);
	printf("idle_30s_extra_requests,0\nidle_30s_clears,0\nidle_30s_output_"
	       "bytes,%zu\n",
	        output.len);
	wf_frame_free(before);
	wf_frame_free(after);
	wf_buf_free(&output);
	wf_app_destroy(&app);
}

static void delayed_loading_and_fast_navigation(void)
{
	open_app();
	WfFrame *frame = wf_frame_new(120, 30, &WF_STYLE_BASE);
	for (unsigned i = 0; i < 100; i++) {
		app.cur.cursor = (size_t)(i & 1u);
		app.now_ms += 1;
		wf_app_preview_sync(&app, false);
		wf_app_draw(&app, frame);
		CHECK(!frame_has(frame, "Loading"));
	}
	app.now_ms += WF_PREVIEW_LOADING_MS - 1;
	CHECK(wf_app_tick(&app) == WF_ACT_NONE);
	wf_app_draw(&app, frame);
	CHECK(!frame_has(frame, "Loading"));
	app.now_ms++;
	CHECK(wf_app_tick(&app) == WF_ACT_REDRAW);
	wf_app_draw(&app, frame);
	CHECK(frame_has(frame, "Loading text"));
	CHECK(wf_app_timeout(&app) == WF_WAIT_FOREVER);
	CHECK(wf_app_tick(&app) == WF_ACT_NONE);
	wf_app_on_preview(&app, app.preview_gen, text("ready\n"));
	wf_app_draw(&app, frame);
	CHECK(!frame_has(frame, "Loading"));
	wf_frame_free(frame);
	wf_app_destroy(&app);
}

static void inspect_recovery(void *ctx)
{
	bool *resolved = ctx;
	app.recovery.unknown = !*resolved;
	app.recovery.unresolved =
	        0; /* A legacy log is not a proven pending operation. */
	app.recovery.causes = *resolved ? 0u : WF_RECOVERY_NOTICE_LEGACY;
}

static void press(WfKey key)
{
	WfEvent ev = {.type = WF_EV_KEY, .key = key};
	(void)app.mode->on_event(&app, &ev);
}

static void open_safety(void)
{
	WfEvent settings = {.type = WF_EV_CHAR, .ch = ','};
	(void)app.mode->on_event(&app, &settings);
	CHECK(app.mode == &WF_MODE_SETTINGS);
	press(WF_KEY_LEFT);
	press(WF_KEY_DOWN);
	press(WF_KEY_DOWN);
	press(WF_KEY_RIGHT);
	press(WF_KEY_DOWN);
	press(WF_KEY_DOWN);
	press(WF_KEY_ENTER);
	CHECK(app.settings.section == WF_SEC_ABOUT);
	CHECK(app.settings.focus == WF_FOCUS_DETAILS);
	CHECK(app.settings.doc &&
	        strstr(app.settings.doc, wf_msg(MSG_RECOVERY_TITLE)));
}

static void recovery_attention(void)
{
	open_app();
	WfConfig config;
	wf_config_init(&config);
	wf_app_set_settings(&app, &config, (WfSettingsPort){0});
	bool resolved = false;
	WfFrame *before = wf_frame_new(120, 30, &WF_STYLE_BASE);
	WfFrame *after = wf_frame_new(120, 30, &WF_STYLE_BASE);
	const WfUiLang languages[] = {WF_UILANG_EN, WF_UILANG_JA};
	for (size_t language = 0; language < 2; language++) {
		wf_i18n_set_language(languages[language]);
		app.recovery.unknown = false;
		app.recovery.unresolved = 0;
		wf_app_draw(&app, before);
		CHECK(!frame_has(before, wf_msg(MSG_SAFETY_REVIEW)));
		app.recovery.refresh = inspect_recovery;
		app.recovery.context = &resolved;
		resolved = false;
		inspect_recovery(&resolved);
		wf_app_draw(&app, after);
		CHECK(frame_has(after, wf_msg(MSG_SAFETY_REVIEW)));
		CHECK(!frame_has(after, wf_msg(MSG_STATUS_RECOVERY_HISTORY)));
		CHECK(0 ==
		        memcmp(before->cells + 29 * 120,
		                after->cells + 29 * 120,
		                120 * sizeof *before->cells));
		WfEvent info = {.type = WF_EV_CHAR, .ch = 'i'};
		(void)app.mode->on_event(&app, &info);
		CHECK(app.mode == &WF_MODE_INFO);
		for (int row = 0; row < app.info.n; row++) {
			CHECK(!strstr(app.info.text[row],
			        wf_msg(MSG_RECOVERY_TITLE)));
			CHECK(!strstr(app.info.text[row], "copy-journal.txt"));
		}
		press(WF_KEY_ESC);
		open_safety();
		CHECK(strstr(app.settings.doc, wf_msg(MSG_SAFETY_REVIEW)));
		CHECK(strstr(
		        app.settings.doc, wf_msg(MSG_RECOVERY_LEGACY_DETAIL)));
		CHECK(strstr(
		        app.settings.doc, wf_msg(MSG_RECOVERY_LEGACY_STATE)));
		CHECK(strstr(
		        app.settings.doc, wf_msg(MSG_RECOVERY_ACTION_DETAIL)));
		CHECK(strstr(app.settings.doc, wf_msg(MSG_RECOVERY_NO_REPAIR)));
		press(WF_KEY_END);
		resolved = true;
		press(WF_KEY_ENTER);
		CHECK(app.settings.top == 0 && !app.recovery.unknown);
		CHECK(strstr(
		        app.settings.doc, wf_msg(MSG_SAFETY_NORMAL_DETAIL)));
		resolved = false;
		press(WF_KEY_ENTER);
		CHECK(app.recovery.unknown &&
		        strstr(app.settings.doc, wf_msg(MSG_SAFETY_REVIEW)));
		press(WF_KEY_ESC);
		CHECK(app.mode == &WF_MODE_NORMAL && app.recovery.unknown);
		CHECK(wf_app_copy_start(&app, false));
		CHECK(!strcmp(app.msg, wf_msg(MSG_COPY_SAFETY_BLOCKED)));
		app.slot.mode = WF_XFER_COPY;
		app.msg[0] = '\0';
		WfEvent paste = {.type = WF_EV_CHAR, .ch = 'p'};
		(void)app.mode->on_event(&app, &paste);
		CHECK(!strcmp(app.msg, wf_msg(MSG_COPY_SAFETY_BLOCKED)));
		CHECK(app.xfer_busy == WF_XB_IDLE);
		app.slot.mode = WF_XFER_NONE;
		app.now_ms = app.msg_keep_until;
		(void)wf_app_tick(&app);
		resolved = true;
		open_safety();
		CHECK(!app.recovery.unknown && !app.recovery.unresolved);
		CHECK(strstr(app.settings.doc, wf_msg(MSG_SAFETY_NORMAL)));
		CHECK(strstr(
		        app.settings.doc, wf_msg(MSG_SAFETY_NORMAL_DETAIL)));
		CHECK(!strstr(
		        app.settings.doc, wf_msg(MSG_RECOVERY_LEGACY_DETAIL)));
		press(WF_KEY_ESC);
		wf_app_draw(&app, after);
		CHECK(!frame_has(after, wf_msg(MSG_SAFETY_REVIEW)));
		/* Unknown inspection errors and known pending operations never say Normal. */
		app.recovery.refresh = NULL;
		app.recovery.unknown = true;
		app.recovery.causes = WF_RECOVERY_NOTICE_STORE;
		open_safety();
		CHECK(strstr(app.settings.doc, wf_msg(MSG_SAFETY_REVIEW)));
		CHECK(strstr(
		        app.settings.doc, wf_msg(MSG_RECOVERY_STORE_DETAIL)));
		CHECK(!strstr(
		        app.settings.doc, wf_msg(MSG_SAFETY_NORMAL_DETAIL)));
		press(WF_KEY_ESC);
		app.recovery.unknown = false;
		app.recovery.unresolved = 2;
		app.recovery.causes = WF_RECOVERY_NOTICE_PENDING;
		open_safety();
		CHECK(strstr(app.settings.doc, wf_msg(MSG_SAFETY_REVIEW)));
		CHECK(strstr(app.settings.doc,
		        wf_msg(MSG_RECOVERY_OPERATION_DETAIL)));
		press(WF_KEY_ESC);
		CHECK(!strcmp(
		        wf_msg_reason("Settings > About: recovery recording "
		                      "unavailable; original unchanged"),
		        wf_msg(MSG_REASON_SAVE_RECORD)));
	}
	wf_i18n_set_language(WF_UILANG_EN);
	wf_frame_free(before);
	wf_frame_free(after);
	wf_app_destroy(&app);
	wf_config_dispose(&config);
}

static void information_small_terminal(void)
{
	const WfUiLang languages[] = {WF_UILANG_EN, WF_UILANG_JA};
	const int widths[] = {32, 64, 120};
	for (size_t language = 0; language < 2; language++) {
		for (size_t width = 0; width < 3; width++) {
			open_app();
			WfConfig config;
			wf_config_init(&config);
			wf_app_set_settings(&app, &config, (WfSettingsPort){0});
			wf_i18n_set_language(languages[language]);
			app.cols = widths[width];
			app.rows = 12;
			app.recovery.unknown = true;
			app.recovery.causes = WF_RECOVERY_NOTICE_STORE |
			        WF_RECOVERY_NOTICE_PENDING |
			        WF_RECOVERY_NOTICE_LEGACY |
			        WF_RECOVERY_NOTICE_COPY_INVALID |
			        WF_RECOVERY_NOTICE_LIMIT;
			app.recovery.error = 5;
			WfFrame *frame = wf_frame_new(
			        app.cols, app.rows, &WF_STYLE_BASE);
			WfPreview *shown = app.preview;
			unsigned requests_before = text_requests;
			open_safety();
			CHECK(wf_utf8_strict(
			        app.settings.doc, app.settings.doc_n, NULL));
			wf_app_draw(&app, frame);
			CHECK(frame_has(frame, wf_msg(MSG_RECOVERY_TITLE)));
			press(WF_KEY_END);
			CHECK(app.settings.top > 0);
			size_t last = app.settings.top;
			press(WF_KEY_END);
			CHECK(app.settings.top == last);
			press(WF_KEY_HOME);
			CHECK(app.settings.top == 0);
			press(WF_KEY_PGDN);
			CHECK(app.settings.top > 0);
			wf_app_draw(&app, frame);
			press(WF_KEY_ESC);
			CHECK(app.mode == &WF_MODE_NORMAL);
			CHECK(app.preview == shown &&
			        text_requests == requests_before);
			CHECK(app.recovery.unknown && app.recovery.error == 5);
			wf_app_draw(&app, frame);
			CHECK(frame_has(frame, wf_msg(MSG_SAFETY_REVIEW)));
			CHECK(!frame_has(
			        frame, wf_msg(MSG_STATUS_RECOVERY_HISTORY)));
			wf_frame_free(frame);
			wf_app_destroy(&app);
			wf_config_dispose(&config);
		}
	}
	wf_i18n_set_language(WF_UILANG_EN);
}

static void replacement_notice_has_its_own_lifetime(void)
{
	open_app();
	app.now_ms = 1000;
	wf_app_set_msg(&app, "Earlier operation");
	uint64_t old_deadline = app.msg_keep_until;
	CHECK(old_deadline == app.now_ms + WF_RECEIPT_KEEP_MS);
	app.now_ms = old_deadline - 1;
	wf_app_set_msg(&app, "Copy was refused");
	CHECK(app.msg_keep_until > old_deadline);
	uint64_t own_deadline = app.msg_keep_until;
	app.now_ms = old_deadline;
	(void)wf_app_tick(&app);
	CHECK(!strcmp(app.msg, "Copy was refused"));
	app.now_ms++;
	wf_app_set_msg(&app, "Copy was refused");
	CHECK(app.msg_keep_until == own_deadline);
	app.now_ms = own_deadline;
	(void)wf_app_tick(&app);
	CHECK(!app.msg[0]);
	wf_app_destroy(&app);
}
#endif

int main(void)
{
	unchanged_listing();
#ifndef WF_STABILITY_BASELINE
	idle_and_revalidation();
	delayed_loading_and_fast_navigation();
	recovery_attention();
	information_small_terminal();
	replacement_notice_has_its_own_lifetime();
#endif
	return TEST_RESULT();
}

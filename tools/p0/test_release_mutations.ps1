param([Parameter(Mandatory)][string]$Compiler,[Parameter(Mandatory)][string]$Build,
  [Parameter(Mandatory)][string]$Configuration,[string]$Only='',[string]$Test='',
  [int]$MutantTimeoutMs=15000,[int]$ControlTimeoutMs=0)
# $MutantTimeoutMs is the HANG detector for ONE mutant's test process (the supervisor kills it and the
# mutant is then reported as not completed, never as killed). It is not a budget for the whole table:
# CTest runs one group per test file (-Test), each with its own limit. A mutant dies on its first FAIL
# (WF_TEST_FAIL_FAST), so a healthy mutant takes about as long as the passing test does.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../wf_toolchain.ps1')
. (Join-Path $PSScriptRoot 'wf_mutation_text.ps1')
. (Join-Path $PSScriptRoot 'wf_sanitizer.ps1')
Initialize-WfToolchain
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildRoot=(Resolve-Path -LiteralPath $Build).Path
$root=Join-Path $buildRoot ('release-mutants-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root|Out-Null
$san=Get-WfSanitizer $Compiler $buildRoot
if($ControlTimeoutMs -le 0){$ControlTimeoutMs=$MutantTimeoutMs}
$ControlTimeoutMs=$ControlTimeoutMs*$san.HangFactor
$MutantTimeoutMs=$MutantTimeoutMs*$san.HangFactor
$mutants=@(
 @{name='choice_processkey_guard';file='src/input/wf_choice.c';test='tests/test_choice.c';old='WF_VK_PROCESSKEY == ev->vk || (i = index_of(c, ev->ch)) < 0';new='(i = index_of(c, ev->ch)) < 0'},
 @{name='choice_starts_on_cancel';file='src/input/wf_choice.c';test='tests/test_choice.c';old='c->cancel = c->sel = (uint8_t)cancel;';new='c->cancel = (uint8_t)cancel; c->sel = 0;'},
 @{name='editor_question_starts_on_cancel';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='wf_choice_begin(&s->choice, "sdc", 2);';new='wf_choice_begin(&s->choice, "sdc", 0);'},
 @{name='settings_question_no_fullwidth_fold';file='src/ui/wf_app_settings.c';test='tests/test_theme_ui.c';old=': WF_SS_CAPTURE != app->settings.sub && !question;';new=': WF_SS_CAPTURE != app->settings.sub && !(question && 0);'},
 @{name='journal_open_error_preserved';file='src/xfer/wf_copyjournal_win.c';test='tests/test_copyjournal_error.c';old='return open_error == ERROR_FILE_NOT_FOUND;';new='(void)open_error; return ERROR_FILE_NOT_FOUND == GetLastError();'},
 @{name='reserved_device';file='src/path/wf_name.c';test='tests/test_name.c';old='ascii_equal(name,stem,L"NUL")';new='0'},
 @{name='unicode_invisible';file='src/path/wf_name.c';test='tests/test_name.c';old='invisible(c) ||';new='(invisible(c),0) ||'},
 @{name='component_bound';file='src/path/wf_name.c';test='tests/test_name.c';old='units > max_component';new='0'},
 @{name='create_no_clobber';file='src/fs/wf_edit_win.c';test='tests/test_edit_win.c';old='CREATE_NEW,FILE_ATTRIBUTE_NORMAL';new='OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL'},
 @{name='rename_identity';file='src/fs/wf_edit_win.c';test='tests/test_edit_win.c';old='expected && !wf_fileid_equal_or_weak(expected,out)';new='expected && (wf_fileid_equal_or_weak(expected,out) && 0)'},
 @{name='edit_parent_identity';file='src/fs/wf_edit_win.c';test='tests/test_edit_win.c';old='expected_parent && !wf_fileid_equal_or_weak(expected_parent,&pinned.id)';new='expected_parent && (wf_fileid_equal_or_weak(expected_parent,&pinned.id) && 0)'},
 @{name='edit_parent_identity_rename';file='src/fs/wf_edit_win.c';test='tests/test_edit_win.c';old='expected_parent && !wf_fileid_equal(expected_parent,&pinned.id)';new='expected_parent && (wf_fileid_equal(expected_parent,&pinned.id) && 0)'},
 @{name='edit_parent_required';file='src/fs/wf_edit_win.c';test='tests/test_edit_win.c';old='!wf_fileid_usable_to_read(expected_parent)';new='0'},
 @{name='private_directory_acl';file='src/path/cwd_neutralize.c';test='tests/test_startup.c';old='!private_acl(h,user->User.Sid)';new='(private_acl(h,user->User.Sid),0)'},
 @{name='neutral_directory_empty';file='src/path/cwd_neutralize.c';test='tests/test_startup.c';old='empty&&!directory_empty(canonical.path)';new='empty&&(directory_empty(canonical.path),0)'},
 @{name='ui_filter_scalar';file='src/ui/wf_app_input.c';test='tests/test_app.c';old='!input_scalar_valid(cp) || n + need >= WF_FILTER_MAX';new='n + need >= WF_FILTER_MAX'},
 @{name='ui_path_scalar';file='src/ui/wf_app_input.c';test='tests/test_operations_ui.c';old='if (!input_scalar_valid(cp))';new='if (input_scalar_valid(cp) && 0)'},
 @{name='ui_path_pair_right';file='src/text/wf_utf.c';test='tests/test_operations_ui.c';old='(void)wf_utf16_next(s, len, &pos);';new='(void)s; pos++;'},
 @{name='ui_path_pair_left';file='src/text/wf_utf.c';test='tests/test_operations_ui.c';old='(uint16_t)s[pos - 1] <= 0xDBFF';new='((uint16_t)s[pos - 1] <= 0xDBFF && 0)'},
 @{name='go_scalar';file='src/ui/wf_app_go.c';test='tests/test_path_bar.c';old='return cp >= 32 && 127 != cp && cp <= 0x10FFFF &&';new='return 127 != cp && cp <= 0x10FFFF &&'},
 @{name='go_pair_right';file='src/text/wf_utf.c';test='tests/test_path_bar.c';old='(void)wf_utf16_next(s, len, &pos);';new='(void)s; pos++;'},
 @{name='go_pair_left';file='src/text/wf_utf.c';test='tests/test_path_bar.c';old='(uint16_t)s[pos - 1] <= 0xDBFF';new='((uint16_t)s[pos - 1] <= 0xDBFF && 0)'},
 @{name='go_stale_batch';file='src/ui/wf_app_go.c';test='tests/test_path_bar.c';old='!b || b->gen != app->go.gen ||';new='!b ||'},
 @{name='go_no_implicit_candidate';file='src/ui/wf_app_go.c';test='tests/test_path_bar.c';old='if (app->go.has_sel && app->go.sel < app->go.cand_n) {';new='if (app->go.sel < app->go.cand_n && app->go.cand_n) {'},
 @{name='go_more_once';file='src/ui/wf_app_go.c';test='tests/test_path_bar.c';old='} else if (app->go.has_more && WF_GR_MORE != app->go.read &&';new='} else if (app->go.has_more &&'},
 @{name='go_missing_file_vetoed';file='src/ui/wf_app_go.c';test='tests/test_path_bar.c';old='if (dir_has_entry(dir, app->go.leaf))';new='if (!dir_has_entry(dir, app->go.leaf) || 1)'},
 @{name='go_paste_multiline';file='src/ui/wf_app_go.c';test='tests/test_path_bar.c';old='if (WF_E_INVAL == st) {';new='if (WF_E_INVAL == st && 0) {'},
 @{name='i18n_tag_separator';file='src/i18n/wf_i18n.c';test='tests/test_i18n.c';old="L'-' == tag[n] ||";new=""},
 @{name='i18n_setting_wins';file='src/i18n/wf_i18n.c';test='tests/test_i18n.c';old='return (WfUiLang)(setting - 1);';new='return detected;'},
 @{name='i18n_control_scrub';file='src/i18n/wf_i18n.c';test='tests/test_i18n.c';old="put(o, (unsigned char)*s < 0x20 || 0x7f == *s ? '?' : *s);";new="put(o, 0x7f == *s ? '?' : *s);"},
 @{name='i18n_plural_rule';file='src/i18n/wf_language.c';test='tests/test_i18n.c';old='return !none && n == 1 ? one : other;';new='return !none && n == 2 ? one : other;'},
 @{name='i18n_cut_on_boundary';file='src/i18n/wf_i18n.c';test='tests/test_i18n.c';old='if (o->len - (k - 1) < need)';new='if (o->len - (k - 1) < need - 1)'},
 @{name='i18n_reason_prefix';file='src/i18n/wf_language.c';test='tests/test_i18n.c';old='!strncmp(catalog[id].name, "MSG_REASON_", 11) &&';new=''},
 @{name='efile_content_always_compared';file='src/edit/wf_efile_win.c';test='tests/test_efile_win.c';old='!memcmp(digest, was->sha256, sizeof digest);';new='now->mtime == was->mtime;'},
 @{name='clip_refusal_remembered';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='s->clip_local_only = true;';new='s->clip_local_only = false;'},
 @{name='edit_rollback_without_memory';file='src/edit/wf_editor.c';test='tests/test_editor.c';old='if (rb->copied) {
		wf_et_snap_restore(&e->text, &rb->snap, cur_last);
		return;
	}';new='if (rb->copied && 0) {
		wf_et_snap_restore(&e->text, &rb->snap, cur_last);
		return;
	}'},
 @{name='copy_dest_id_checked';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='!wf_info_matches(want, &st.id)) {';new='0) {'},
 @{name='copy_dest_pinned';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='if (ok && WF_CJ_RECYCLE != spec->kind)';new='if (0)'},
 @{name='recycle_checks_the_object_taken';file='src/xfer/wf_recycle_win.c';test='tests/test_recycle_faults.c';old='if (1 != receipt) {';new='if (0) {'},
 @{name='save_temp_permissions_protected';file='src/edit/wf_efile_win.c';test='tests/test_efile_win.c';old='!SetSecurityDescriptorDacl(&abs, TRUE, dacl, FALSE) ||';new='!SetSecurityDescriptorDacl(&abs, FALSE, dacl, FALSE) ||'},
 @{name='ui_current_generation';file='src/ui/wf_app_nav.c';test='tests/test_app.c';old='gen != app->nav_gen || !app->want_path';new='(gen != app->nav_gen && 0) || !app->want_path'},
 @{name='ui_stale_transfer';file='src/ui/wf_app_xfer.c';test='tests/test_app.c';old='NULL == app->want_path &&';new=''},
 @{name='ui_name_identity_required';file='src/ui/wf_app_input.c';test='tests/test_operations_ui.c';old='if (!app->name_form.bound)';new='if (app->name_form.bound && 0)'},
 @{name='ui_name_generation';file='src/ui/wf_app_input.c';test='tests/test_operations_ui.c';old='app->name_form.nav_gen != app->nav_gen ||';new='(app->name_form.nav_gen != app->nav_gen && 0) ||'},
 @{name='ui_empty_destination';file='src/ui/wf_app_xfer.c';test='tests/test_app.c';old='return NULL == app->want_path && NULL != app->cur.dir;';new='return NULL == app->want_path && NULL != app->cur.dir && NULL != wf_view_current(&app->cur);'},
 @{name='pane_pending_guard';file='src/ui/wf_app_preview.c';test='tests/test_preview_app.c';old='if (app->want_path) {';new='if (app->want_path && 0) {'},
 @{name='pane_right_focus';file='src/ui/wf_app_keys.c';test='tests/test_preview_app.c';old='wf_app_preview_focus(app);';new='wf_app_enter_selected(app);'},
 @{name='pane_scroll';file='src/ui/wf_app_preview.c';test='tests/test_preview_app.c';old='app->preview_top++;';new='app->preview_top += 0;'},
 @{name='pane_sha_row_not_reserved';file='src/ui/wf_app_render.c';test='tests/test_preview_app.c';old='h--;';new='h -= 0;'},
 @{name='pane_sha_scroll_limit_ignores_row';file='src/ui/wf_app_preview.c';test='tests/test_preview_app.c';old='body = layout.body_h - sha_row_taken(app, layout.body_h);';new='body = layout.body_h - 0 * sha_row_taken(app, layout.body_h);'},
 @{name='preview_bottom_limit';file='src/ui/wf_app_preview.c';test='tests/test_preview_app.c';old='return preview->nlines - (size_t)rows;';new='return preview->nlines - 1;'},
 @{name='preview_truncated_row';file='src/ui/wf_app_preview.c';test='tests/test_preview_app.c';old='height - 1 - (0 != preview->truncated)';new='height - 1'},
 @{name='preview_line_number_origin';file='src/ui/wf_app_render.c';test='tests/test_preview_app.c';old='digits, line + 1);';new='digits, line);'},
 @{name='navigation_name_priority';file='src/ui/wf_app_nav.c';test='tests/test_navigation_memory.c';old='if (!wcscmp(view->dir->e[view->vis[i]].name, name))';new='if (!wcscmp(view->dir->e[view->vis[i]].name, name) && 0)'},
 @{name='navigation_scroll_memory';file='src/ui/wf_app_nav.c';test='tests/test_navigation_memory.c';old=': position   ? position->top';new=': position   ? 0'},
 @{name='navigation_missing_fallback';file='src/ui/wf_app_nav.c';test='tests/test_navigation_memory.c';old='view->cursor = cursor < view->nvis ? cursor : view->nvis - 1;';new='view->cursor = cursor < view->nvis ? 0 : view->nvis - 1;'},
 @{name='navigation_target_lru';file='src/ui/wf_app_nav.c';test='tests/test_navigation_memory.c';old='(void)memory_find_and_touch(app, app->want_path);';new='(void)app->want_path;'},
 @{name='navigation_ctrl_edge';file='src/ui/wf_view.c';test='tests/test_navigation_memory.c';old='v->cursor = last && v->nvis ? v->nvis - 1 : 0;';new='v->cursor = 0;'},
 @{name='copyjob_recycle_takes_any_collision';file='src/xfer/wf_copyjob.c';test='tests/test_cross_move_gate.c';old='case WF_CJ_RECYCLE: return WF_COLLIDE_STOP == collide;';new='case WF_CJ_RECYCLE: return collide <= WF_COLLIDE_RENAME;'},
 @{name='copyjob_unknown_collision_allowed';file='src/xfer/wf_copyjob.c';test='tests/test_cross_move_gate.c';old='case WF_CJ_MOVE_ACROSS: return WF_COLLIDE_STOP == collide || WF_COLLIDE_SKIP == collide || WF_COLLIDE_OVERWRITE == collide ||';new='case WF_CJ_MOVE_ACROSS: return WF_COLLIDE_STOP == collide || WF_COLLIDE_SKIP == collide || WF_COLLIDE_OVERWRITE <= collide ||'},
 @{name='engine_stop_overwrites';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='if (WF_COLLIDE_OVERWRITE != r->spec->collide) {';new='if (WF_COLLIDE_STOP != r->spec->collide) {'},
 @{name='keep_both_starts_at_one';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='for (unsigned k = 2; k <= 999; k++) {';new='for (unsigned k = 1; k <= 999; k++) {'},
 @{name='keep_both_splits_a_dotfile';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='for (size_t i = n; i > 1; i--) {';new='for (size_t i = n; i > 0; i--) {'},
 @{name='confirm_approves_unseen';file='src/ui/wf_app_confirm.c';test='tests/test_confirm.c';old='if (app->cols < 20 || app->rows < 8) {';new='if (app->cols < 0 || app->rows < 0) {'},
 @{name='undo_removed_before_it_happened';file='src/ui/wf_app_undo.c';test='tests/test_copy_ui.c';old='app->undo_inflight = u->uid; /* removed when done */';new='undo_remove(app, (size_t)(u - app->undo));'},
 @{name='recycle_result_of_another_job_edits_the_entry';file='src/ui/wf_app_undo.c';test='tests/test_copy_ui.c';old='if (app->recycling[k].job == r->job) {';new='if (app->recycling[k].job != r->job) {'},
 @{name='overwritten_file_offered_for_undo';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='if (OUT_OK == o && out->overwritten == replaced_before && !merge_top) {';new='if (OUT_OK == o && (out->overwritten == replaced_before || out->overwritten != replaced_before) && !merge_top) {'},
 @{name='terminal_two_keys_are_a_paste';file='src/term/wf_term.c';test='tests/test_term_input.c';old='(text >= WF_PASTE_MIN_KEYS || (enter && text >= 2))';new='(text >= 2 || (enter && text >= 2))'},
 @{name='terminal_held_key_is_a_paste';file='src/term/wf_term.c';test='tests/test_term_input.c';old='} else if (key_identity(&ev[i]) != first) {';new='} else if (key_identity(&ev[i]) == first) {'},
 @{name='terminal_paste_ends_with_the_read';file='src/term/wf_term.c';test='tests/test_term_input.c';old='now >= t->paste_until) {';new='now >= t->paste_until + 100000) {'},
 @{name='paste_enter_confirms_a_field';file='src/ui/wf_app_mouse.c';test='tests/test_mouse_ui.c';old='if (WF_EV_KEY == ev->type) { /* Enter or Tab: never confirms */';new='if (false && WF_EV_KEY == ev->type) { /* Enter or Tab: never confirms */'},
 @{name='paste_answers_a_question';file='src/ui/wf_app_mouse.c';test='tests/test_mouse_ui.c';old='if (!takes_text(app)) {';new='if (false && !takes_text(app)) {'},
 @{name='parent_click_ignores_the_row';file='src/ui/wf_app_mouse.c';test='tests/test_mouse_ui.c';old='(size_t)(y - lay->body_y);';new='(size_t)(y - y + 0 * lay->body_y);'},
 @{name='vim_normal_runs_pasted_keys';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='(WF_CTX_VIM_NORMAL != ctx || (ev->vk & WF_VK_PASTED))) {';new='(WF_CTX_VIM_NORMAL != ctx)) {'},
 @{name='prompt_takes_the_next_pasted_line';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='s->paste_cut = true;';new='s->paste_cut = false;'},
 @{name='terminal_pasted_crlf_is_two_enters';file='src/term/wf_term.c';test='tests/test_term_input.c';old='now - t->cr_tick <= CRLF_MS) {';new='now - t->cr_tick > CRLF_MS) {'},
 @{name='terminal_mouse_release_is_a_press';file='src/term/wf_term.c';test='tests/test_term_input.c';old='DWORD fresh = down & ~t->mouse_buttons;';new='DWORD fresh = down;'},
 @{name='pasted_text_runs_as_commands';file='src/ui/wf_app.c';test='tests/test_mouse_ui.c';old='if (ev->vk & WF_VK_PASTED) {';new='if (ev->vk & WF_VK_PASTED && false) {'},
 @{name='mouse_click_picks_the_row_below';file='src/ui/wf_app_mouse.c';test='tests/test_mouse_ui.c';old='(size_t)(y - lay.body_y);';new='(size_t)(y - lay.body_y + 1);'},
 @{name='terminal_ctrl_up';file='src/term/wf_term.c';test='tests/test_term_input.c';old='return WF_KEY_CTRL_UP;';new='return WF_KEY_UP;'},
 @{name='terminal_ctrl_down';file='src/term/wf_term.c';test='tests/test_term_input.c';old='return WF_KEY_CTRL_DOWN;';new='return WF_KEY_DOWN;'},
 @{name='hmac_digest_equality';file='src/xfer/wf_integrity_win.c';test='tests/test_integrity_win.c';old='result = difference ? WF_INTEGRITY_MISMATCH : WF_INTEGRITY_MATCH;';new='result = difference ? WF_INTEGRITY_MATCH : WF_INTEGRITY_MATCH;'},
 @{name='hmac_expected_identity';file='src/xfer/wf_integrity_win.c';test='tests/test_integrity_win.c';old='identity_equal(&s->id, id) &&';new='(identity_equal(&s->id, id) || 1) &&'},
 @{name='hmac_volume_identity';file='src/xfer/wf_integrity_win.c';test='tests/test_integrity_win.c';old='a->VolumeSerialNumber == b->VolumeSerialNumber &&';new='(a->VolumeSerialNumber == b->VolumeSerialNumber || 1) &&'},
 @{name='hmac_rng_failure';file='src/xfer/wf_integrity_win.c';test='tests/test_integrity_faults.c';old='BCRYPT_USE_SYSTEM_PREFERRED_RNG), report)) { goto fail; }';new='BCRYPT_USE_SYSTEM_PREFERRED_RNG), report) && 0) { goto fail; }'},
 @{name='hmac_cancel';file='src/xfer/wf_integrity_win.c';test='tests/test_integrity_win.c';old='cancelled && cancelled(context)';new='cancelled && (cancelled(context) && 0)'},
 @{name='hmac_hardlink';file='src/xfer/wf_integrity_win.c';test='tests/test_integrity_win.c';old='1 == s->standard.NumberOfLinks &&';new='(1 == s->standard.NumberOfLinks || 1) &&'},
 @{name='hmac_job_erase';file='src/xfer/wf_integrity_win.c';test='tests/test_integrity_faults.c';old='SecureZeroMemory(j, sizeof *j);';new='/* job erasure removed by mutation */'},
 @{name='copy_actual_source';file='src/xfer/wf_copy_source_win.c';test='tests/test_copy_source_win.c';old='!matches(o, actual_source, 0, &error)';new='!matches(o, o->guard, 0, &error)'},
 @{name='copy_no_callback';file='src/xfer/wf_copy_source_win.c';test='tests/test_copy_source_win.c';old='!o->report.callback_checks || INVALID_HANDLE_VALUE == o->observed';new='(o->report.callback_checks && 0)'},
 @{name='copy_sticky_rejection';file='src/xfer/wf_copy_source_win.c';test='tests/test_copy_source_win.c';old='WF_COPY_SOURCE_REJECTED == o->report.state || o->terminal';new='o->terminal'},
 @{name='copy_source_path';file='src/xfer/wf_copy_source_win.c';test='tests/test_copy_source_faults.c';old='ok = same_path &&';new='ok = (same_path || 1) &&'},
 @{name='copy_failed_api';file='src/xfer/wf_copy_source_win.c';test='tests/test_copy_source_win.c';old='if (!copied)';new='if (copied && 0)'},
 @{name='copy_snapshot_id';file='src/xfer/wf_copy_source_win.c';test='tests/test_copy_source_win.c';old='!wf_info_matches(&o->snapshot.id, &id) ||';new='0 ||'},
 # Exercise inventory-only recovery contracts.
 @{name='journal_legacy_append';file='src/xfer/wf_copyjournal_win.c';test='tests/test_copyjournal_inventory.c';old='} else if (info.format != WF_CJ_EMPTY &&';new='} else if (0 && info.format != WF_CJ_EMPTY &&'},
 @{name='journal_hardlink_accepted';file='src/xfer/wf_copyjournal_win.c';test='tests/test_copyjournal_inventory.c';old='standard.NumberOfLinks != 1 ||';new='(standard.NumberOfLinks != 1 && 0) ||'},
 @{name='journal_partial_accepted';file='src/xfer/wf_copyjournal_win.c';test='tests/test_copyjournal_inventory.c';old='if (length) {';new='if (length && 0) {'},
 @{name='copy_listing_error_ignored';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_faults.c';old='if (ERROR_NO_MORE_FILES != error) {';new='if (0) {'},
 @{name='move_source_listing_unchecked';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='return same && proofs_valid(r);';new='return same || true;'},
 @{name='save_old_version_recorded_as_nothing';file='src/edit/wf_efile_win.c';test='tests/test_efile_win.c';old='if (jr && !wf_cj_aside(jr, ext, aside, &again.id)) {';new='if (jr && 0) {'},
 @{name='save_without_journal_proceeds';file='src/edit/wf_efile_win.c';test='tests/test_efile_win.c';old='if (JOURNAL_MISSING == journal_mode ||';new='if (0 ||'},
 @{name='prompt_text_raw';file='src/edit/wf_eview.c';test='tests/test_esession.c';old='before_s = wf_buf_cstr(&before_b);';new='before_s = s->in;'},
 @{name='load_failure_ignored';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='st = apply_loaded(s, b, path, &l, false);';new='(void)apply_loaded(s, b, path, &l, false);'},
 @{name='markdown_rescan';file='src/edit/wf_esyntax.c';test='tests/test_esyntax.c';old='if (c->set && from >= c->from && from <= c->at)';new='if (0 && c->set && from >= c->from && from <= c->at)'},
 @{name='long_line_lexed_for_state';file='src/edit/wf_esyntax.c';test='tests/test_esyntax.c';old='if (l->len > WF_SYN_LINE_MAX) {';new='if (0 && l->len > WF_SYN_LINE_MAX) {'},
 @{name='archive_dotdot_climbs';file='src/archive/wf_archive.c';test='tests/test_archive.c';old="L'.' == p[from + 1])";new="L'X' == p[from + 1])"},
 @{name='archive_raw_believed';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='!a->info.filters[0]) {';new='0) {'},
 @{name='archive_stale_index_served';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='if (hit && same_stamp(&hit->stamp, &stamp)) {';new='if (hit) {'},
 @{name='archive_scan_unbounded';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='uint64_t limit = (uint64_t)g_max_scan;';new='uint64_t limit = UINT64_MAX;'},
 @{name='archive_controls_kept';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='return c < 0x20 || 0x7F == c ||';new='return 0x7F == c ||'},
 @{name='archive_link_read';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='if (node.flags & N_LINK) {';new='if (0) {'},
 @{name='archive_ui_guard_open';file='src/ui/wf_app_nav.c';test='tests/test_archive_ui.c';old='if (wf_vpath_is(app->cwd)) {';new='if (0) {'},
 @{name='vim_archive_guard';file='src/ui/wf_app_nav.c';test='tests/test_vimkeys.c';old='if (wf_vpath_is(app->cwd)) {';new='if (0) {'},
 @{name='keys_sequence_timeout';file='src/input/wf_keymap.c';test='tests/test_keymap.c';old='if (wf_keystate_active(st) && now_ms >= st->deadline)';new='if (0)'},
 @{name='keys_count_cap';file='src/input/wf_keymap.c';test='tests/test_keymap.c';old='st->count = WF_COUNT_MAX;';new='(void)st;'},
 @{name='keys_esc_cancels_partial';file='src/input/wf_keymap.c';test='tests/test_keymap.c';old='WF_NK_ESC == stroke.code && !stroke.mods) {';new='WF_NK_ESC == stroke.code && 0) {'},
 @{name='keys_count_refused';file='src/input/wf_keymap.c';test='tests/test_keymap.c';old='if (!ci || !(ci->flags & (WF_CF_COUNT | WF_CF_LINE))) {';new='if (ci && 0) {'},
 @{name='keys_stray_prefix_retry';file='src/input/wf_keymap.c';test='tests/test_keymap.c';old='if (st->pending.n > 1 && !retried) {';new='if (st->pending.n > 1 && !retried && 0) {'},
 @{name='keys_prefix_clash';file='src/input/wf_keymap.c';test='tests/test_keymap.c';old='wf_seq_equal(a, b) || wf_seq_is_prefix(a, b) ||';new='wf_seq_equal(a, b) ||'},
 @{name='keys_dd_is_a_move_not_a_delete';file='src/input/wf_keymap.c';test='tests/test_keymap.c';old='{WF_CTX_FM, WF_FM_MOVE_STAGE, "d d"}';new='{WF_CTX_FM, WF_FM_RECYCLE, "d d"}'},
 @{name='vim_dd_is_a_move_not_a_delete';file='src/input/wf_keymap.c';test='tests/test_vimkeys.c';old='{WF_CTX_FM, WF_FM_MOVE_STAGE, "d d"}';new='{WF_CTX_FM, WF_FM_RECYCLE, "d d"}'},
 @{name='ed_wq_closes_only_after_a_save';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='SaveOutcome o; s->exit_after_save = true; o = do_save(s); if (SAVE_DONE == o) {';new='SaveOutcome o; s->exit_after_save = true; o = do_save(s); if (1) {'},
 @{name='ed_q_asks_before_discarding';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='case WF_EX_QUIT: return try_close(s);';new='case WF_EX_QUIT: return close_current(s);'},
 @{name='ed_normal_mode_types_nothing';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='(WF_CTX_VIM_NORMAL != ctx || (ev->vk & WF_VK_PASTED))) {';new='(true)) {'},
 @{name='settings_esc_asks_when_dirty';file='src/ui/wf_app_settings.c';test='tests/test_theme_ui.c';old='app->settings.reload_pending = false; if (wf_set_is_dirty(app)) {';new='app->settings.reload_pending = false; if (0) {'},
 @{name='settings_capture_esc_cancels';file='src/ui/wf_app_settings_keys.c';test='tests/test_theme_ui.c';old='WF_EV_KEY == ev->type && WF_KEY_ESC == ev->key) {';new='0) {'},
 @{name='settings_preset_edit_is_explained';file='src/ui/wf_app_settings_theme.c';test='tests/test_theme_ui.c';old='if (is_custom) { edit_enter(app, custom_index); } else { wf_set_note_msg(app, true, MSG_SET_THEME_PRESET_FIXED); }';new='if (is_custom) { edit_enter(app, custom_index); }'},
 @{name='settings_low_contrast_warns';file='src/ui/wf_app_settings_theme.c';test='tests/test_theme_ui.c';old='if (wf_theme_check_contrast(&p, issue, 8) > 0) {';new='if (0) {'},
 # Locations (WSL / SFTP): the read-only default, the name rules, the wire limits.
 @{name='loc_replace_needs_identity_proof';file='src/fs/wf_loc.c';test='tests/test_loc.c';old='if (p->atomic_replace && p->identity_checked_at_commit) {';new='if (p->atomic_replace) {'},
 @{name='loc_wsl_prefix_boundary';file='src/fs/wf_loc.c';test='tests/test_loc.c';old='(L''\\'' == path[len] || L''/'' == path[len] || !path[len])';new='1'},
 @{name='loc_display_hides_c1_controls';file='src/fs/wf_loc.c';test='tests/test_loc.c';old='(c >= 0x7F && c <= 0x9F)';new='(c == 0x7F)'},
 @{name='sftp_entry_name_unchecked';file='src/fs/wf_sftp.c';test='tests/test_sftp.c';old='if (!wf_posix_name_ok(e->name, e->name_n)) {';new='if (0) {'},
 @{name='sftp_packet_limit_ignored';file='src/fs/wf_sftp.c';test='tests/test_sftp.c';old='if (0 == len || len > WF_SFTP_MAX_PACKET) {';new='if (0 == len) {'},
 @{name='wsl_move_across_places';file='src/ui/wf_app_wsl.c';test='tests/test_wsl_ui.c';old='!wf_wsl_same_distro(app->wclip.dir, app->cwd)) {';new='0 && !wf_wsl_same_distro(app->wclip.dir, app->cwd)) {'},
 @{name='wsl_delete_without_asking';file='src/ui/wf_app_wsl.c';test='tests/test_wsl_ui.c';old='st = wf_app_confirm_begin(app, lines, 3, delete_yes, NULL, ev);';new='(void)ev; delete_yes(app, NULL); st = WF_OK;'},
 @{name='wslops_name_allows_separators';file='src/fs/wf_wslops_win.c';test='tests/test_wslops.c';old='wcschr(L"\\/:*?\"<>|", name[i])';new='wcschr(L":*?\"<>|", name[i])'},
 @{name='wslops_tree_delete_unasked';file='src/fs/wf_wslops_win.c';test='tests/test_wslops.c';old='if (tree) {';new='if (1) {'},
 @{name='wslops_copy_overwrites';file='src/fs/wf_wslops_win.c';test='tests/test_wslops.c';old='MoveFileExW(etmp, efinal, 0)';new='MoveFileExW(etmp, efinal, MOVEFILE_REPLACE_EXISTING)'},
 @{name='sftp_place_host_may_start_with_dash';file='src/fs/wf_sftpplace_win.c';test='tests/test_sftpplace_proc.c';old='(L''-'' == *p && hn > 0);';new='L''-'' == *p;'},
 @{name='sftp_place_guard_removed';file='src/ui/wf_app_nav.c';test='tests/test_sftpplace.c';old='if (wf_sftp_place_is(app->cwd)) {';new='if (0) {'},
 @{name='sftp_place_read_ignores_the_size_limit';file='src/fs/wf_sftpplace_win.c';test='tests/test_sftpplace_proc.c';old='((max_bytes && at.size > max_bytes) ||';new='((max_bytes && at.size > (uint64_t)max_bytes + SIZE_MAX / 2) ||'},
 @{name='wslops_undo_removes_full_file';file='src/fs/wf_wslops_win.c';test='tests/test_wslops.c';old='0 == std.EndOfFile.QuadPart;';new='true;'},
 @{name='wslops_copy_loses_time';file='src/fs/wf_wslops_win.c';test='tests/test_wslops.c';old='keep_time(src, final, r);';new='(void)0;'},
 @{name='wslops_progress_not_counted';file='src/fs/wf_wslops_win.c';test='tests/test_wslops.c';old='InterlockedAdd64(&progress_bytes, (LONG64)got);';new='(void)got;'},
 @{name='wslsave_ignores_changed_bytes';file='src/edit/wf_efile_win.c';test='tests/test_efile_win.c';old='memcmp(seen, was->sha256, sizeof seen)) {';new='(0 && memcmp(seen, was->sha256, sizeof seen))) {'},
 @{name='wsl_scheme_dash_name';file='src/path/wf_smartgo_path.c';test='tests/test_wsl_ui.c';old='if (scheme_n == d || L''-'' == text[scheme_n]) {';new='if (scheme_n == d) {'},
 @{name='wsl_scheme_lowercases_nothing_but_maps_slash';file='src/path/wf_smartgo_path.c';test='tests/test_wsl_ui.c';old='full[head_n + i - scheme_n] = L''/'' == text[i] ? L''\\'' : text[i];';new='full[head_n + i - scheme_n] = L''/'' == text[i] ? L''\\'' : towlower(text[i]);'},
 @{name='wsl_files_equal_ignoring_case';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='b->path && path && !wcscmp(b->path, path)';new='b->path && path && !_wcsicmp(b->path, path)'},
 @{name='sftpc_reply_id_unchecked';file='src/fs/wf_sftpc.c';test='tests/test_sftpc.c';old='if (m->id != id) {';new='if (m->id != id && 0) {'},
 @{name='sftpc_reads_non_regular';file='src/fs/wf_sftpc.c';test='tests/test_sftpc.c';old='if (!a.has_perm || WF_SFTP_S_IFREG != (a.perm & WF_SFTP_S_IFMT)) {';new='if (0) {'},
 @{name='sftpc_data_longer_than_asked';file='src/fs/wf_sftpc.c';test='tests/test_sftpc.c';old='m.data_n > want || 0 == m.data_n';new='0 == m.data_n'},
 @{name='sftpc_handle_left_open';file='src/fs/wf_sftpc.c';test='tests/test_sftpc.c';old='if (!c->broken) {';new='if (!c->broken && 0) {'},
 @{name='sftpc_realpath_relative_ok';file='src/fs/wf_sftpc.c';test='tests/test_sftpc.c';old='!wf_posix_path_ok(name, n)) {';new='0) {'},
 @{name='save_hash_overruled_by_clock';file='src/fs/wf_savemodel.c';test='tests/test_savemodel.c';old='return memcmp(b->sha256, n->sha256, sizeof b->sha256)';new='return 0 && memcmp(b->sha256, n->sha256, sizeof b->sha256)'},
 @{name='save_coarse_clock_trusted';file='src/fs/wf_savemodel.c';test='tests/test_savemodel.c';old='if (weak_time_only || !(b->have_mtime && n->have_mtime)) {';new='if ((weak_time_only && 0) || !(b->have_mtime && n->have_mtime)) {'},
 @{name='save_replaced_object_ignored';file='src/fs/wf_savemodel.c';test='tests/test_savemodel.c';old='(b->dev != n->dev || b->ino != n->ino)) {';new='(b->dev != n->dev && 0)) {'},
 @{name='save_over_unopened_file';file='src/fs/wf_savemodel.c';test='tests/test_savemodel.c';old='} else if (!buffer_from_file) {';new='} else if (0) {'},
 @{name='save_unknown_counts_as_same';file='src/fs/wf_savemodel.c';test='tests/test_savemodel.c';old='default: w = WF_WHY_UNVERIFIED; break;';new='default: p = WF_PLAN_REPLACE; break;'},
 @{name='ssh_hostkey_not_strict';file='src/fs/wf_sshcmd.c';test='tests/test_sshcmd.c';old='StrictHostKeyChecking=yes"';new='StrictHostKeyChecking=no"'},
 @{name='ssh_updates_known_hosts';file='src/fs/wf_sshcmd.c';test='tests/test_sshcmd.c';old='UpdateHostKeys=no';new='UpdateHostKeys=yes'},
 @{name='ssh_accepts_dns_host_keys';file='src/fs/wf_sshcmd.c';test='tests/test_sshcmd.c';old='VerifyHostKeyDNS=no';new='VerifyHostKeyDNS=yes'},
 @{name='ssh_other_authentication';file='src/fs/wf_sshcmd.c';test='tests/test_sshcmd.c';old='PreferredAuthentications=publickey';new='PreferredAuthentications=hostbased'},
 @{name='ssh_forwards_x11';file='src/fs/wf_sshcmd.c';test='tests/test_sshcmd.c';old='ForwardX11=no';new='ForwardX11=yes'},
 @{name='sftp_realpath_ignores_record_tail';file='src/fs/wf_sftp.c';test='tests/test_sftp.c';old='if (!r.ok || !parse_attrs(&r, &attrs) || r.left) {';new='(void)attrs; if (!r.ok) {'},
 # State folders (DR-DRAFT-017): which of piero / wfile is used, and that records left in the older one are never overlooked.
 @{name='state_both_uses_older';file='src/base/wf_state_dir.c';test='tests/test_state_dir.c';old='*kind = PROBE_FOLDER == old ? WF_STATE_BOTH : WF_STATE_PIERO;';new='*kind = PROBE_FOLDER == old ? WF_STATE_LEGACY : WF_STATE_PIERO;'},
 @{name='state_older_not_seen';file='src/base/wf_state_dir.c';test='tests/test_state_dir.c';old='*kind = PROBE_FOLDER == old ? WF_STATE_LEGACY : WF_STATE_FRESH;';new='*kind = WF_STATE_FRESH;'},
 @{name='state_file_in_place_accepted';file='src/base/wf_state_dir.c';test='tests/test_state_dir.c';old='if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {';new='if (0 && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {'},
 @{name='old_unresolved_does_not_block';file='src/recovery/wf_old_state.c';test='tests/test_state_dir.c';old='return r->unresolved || r->store_unknown || r->limit || r->copy_legacy ||';new='return r->store_unknown || r->limit || r->copy_legacy ||'},
 @{name='old_unreadable_does_not_block';file='src/recovery/wf_old_state.c';test='tests/test_state_dir.c';old='return r->unresolved || r->store_unknown || r->limit || r->copy_legacy ||';new='return r->unresolved || r->limit || r->copy_legacy ||'},
 @{name='old_bad_inventory_does_not_block';file='src/recovery/wf_old_state.c';test='tests/test_state_dir.c';old='r->copy_invalid;
}

bool wf_state_report_any';new='0;
}

bool wf_state_report_any'},
 @{name='old_drafts_not_noticed';file='src/recovery/wf_old_state.c';test='tests/test_state_dir.c';old='if (any) {
		r->drafts = true;
	}';new='if (any && 0) {
		r->drafts = true;
	}'},
 @{name='stage_ignores_state_choice';file='src/preview/wf_dechost_client_win.c';test='tests/test_state_dir.c';old='wf_state_name(kind));';new='L"piero");'},
 @{name='stage_name_is_old';file='src/preview/wf_dechost_client_win.c';test='tests/test_state_dir.c';old='swprintf(out, cap, L"%ls\\piero.exe", dir);';new='swprintf(out, cap, L"%ls\\wfile.exe", dir);'},
 # What the user is told when the program cannot use its private state folder, or cannot copy (DR-DRAFT-017 follow-up).
 @{name='not_private_reads_as_access_denied';file='src/path/cwd_neutralize.c';test='tests/test_startup.c';old='DWORD error = WF_PRIVATE_E_NOT_PRIVATE, query;';new='DWORD error = ERROR_ACCESS_DENIED, query;'},
 @{name='link_reads_as_access_denied';file='src/path/cwd_neutralize.c';test='tests/test_startup.c';old='*error = ERROR_REPARSE_POINT_ENCOUNTERED;';new='*error = ERROR_ACCESS_DENIED;'},
 @{name='file_in_place_reads_as_access_denied';file='src/path/cwd_neutralize.c';test='tests/test_startup.c';old='*error = ERROR_DIRECTORY;';new='*error = ERROR_ACCESS_DENIED;'},
 @{name='state_link_reads_as_not_a_folder';file='src/base/wf_state_dir.c';test='tests/test_state_dir.c';old='*error = ERROR_REPARSE_POINT_ENCOUNTERED;';new='*error = ERROR_DIRECTORY;'},
 @{name='copy_refusal_ignores_the_reason';file='src/ui/wf_app_xfer.c';test='tests/test_copy_ui.c';old='wf_msg(app->copy_off_set ? app->copy_off : MSG_COPY_SAFETY_BLOCKED)';new='wf_msg(MSG_COPY_SAFETY_BLOCKED)'},
 @{name='open_refusal_ignores_the_reason';file='src/ui/wf_app_open.c';test='tests/test_open_ui.c';old='wf_msg(app->open_off_set ? app->open_off : MSG_OPEN_GATES)';new='wf_msg(MSG_OPEN_GATES)'},
 # `[terminal] ambiguous_width` and `[files] directories_first` do what they say.
 @{name='ambiguous_width_never_wide';file='src/text/wf_width.c';test='tests/test_text.c';old='ambiguous_cells = 2 == cells ? 2 : 1;';new='(void)cells; ambiguous_cells = 1;'},
 @{name='ambiguous_any_value_turns_it_on';file='src/text/wf_width.c';test='tests/test_text.c';old='ambiguous_cells = 2 == cells ? 2 : 1;';new='ambiguous_cells = cells ? 2 : 1;'},
 @{name='ambiguous_search_misses_single_scalars';file='src/text/wf_width.c';test='tests/test_text.c';old='} else if (cp > t[mid].hi) {';new='} else if (cp >= t[mid].hi) {'},
 @{name='mixed_order_still_sets_directories_apart';file='src/fs/wf_dir.c';test='tests/test_dir.c';old='if (s->dirs_first && ad != bd) {';new='if (ad != bd) {'},
 @{name='sort_menu_ignores_directories_first';file='src/ui/wf_app_sort.c';test='tests/test_app.c';old='return !app->config || 0 != app->config->directories_first;';new='(void)app; return true;'},
 @{name='view_resort_ignores_directories_first';file='src/ui/wf_view.c';test='tests/test_app.c';old='st = wf_dir_sort_with(v->dir, key, descending, dirs_first);';new='(void)dirs_first; st = wf_dir_sort_with(v->dir, key, descending, true);'},
 @{name='recycle_refusal_ignores_the_reason';file='src/ui/wf_app_input.c';test='tests/test_copy_ui.c';old='wf_app_set_msg(app, wf_msg(recycle_off_reason(app)));';new='(void)recycle_off_reason(app); wf_app_set_msg(app, wf_msg(MSG_RECYCLE_OFF_SAFETY));'},
 # Folder sizes (D): links are not asked about, totals stay with their folder and their request,
 # a running or cut-short count says "at least", Esc stops, r forgets, the details window shows it.
 @{name='dirsize_asks_about_links';file='src/ui/wf_app_dirsize.c';test='tests/test_dirsize_ui.c';old='if ((e->flags & WF_ENT_DIR) && !(e->flags & WF_ENT_LINK)) {';new='if (e->flags & WF_ENT_DIR) {'},
 @{name='dirsize_totals_shown_for_another_folder';file='src/ui/wf_dirsize.c';test='tests/test_dirsize_ui.c';old='if (!s->dir || !dir || !name || wcscmp(s->dir, dir)) {';new='if (!s->dir || !dir || !name) {'},
 @{name='dirsize_stale_request_accepted';file='src/ui/wf_dirsize.c';test='tests/test_dirsize_ui.c';old='if (!s->gen || u->gen != s->gen || u->index >= s->n) {';new='if (!s->gen || u->index >= s->n) {'},
 @{name='dirsize_running_total_has_no_mark';file='src/ui/wf_dirsize.c';test='tests/test_dirsize_ui.c';old='bool at_least = !u->finished || u->partial;';new='bool at_least = u->partial;'},
 @{name='dirsize_cut_short_total_has_no_mark';file='src/ui/wf_dirsize.c';test='tests/test_dirsize_ui.c';old='bool at_least = !u->finished || u->partial;';new='bool at_least = !u->finished;'},
 @{name='dirsize_late_update_after_esc_accepted';file='src/ui/wf_app_dirsize.c';test='tests/test_dirsize_ui.c';old='if (!app->dirsize.running || !wf_dirsizes_take(&app->dirsize, u)) {';new='if (!wf_dirsizes_take(&app->dirsize, u)) {'},
 @{name='dirsize_esc_does_not_cancel_the_worker';file='src/ui/wf_app_dirsize.c';test='tests/test_dirsize_ui.c';old='	if (app->size_port.cancel) {
		app->size_port.cancel(app->size_port.ctx);
	}
	/* what the worker still delivers is dropped: nothing is running */';new='	/* what the worker still delivers is dropped: nothing is running */'},
 @{name='dirsize_r_keeps_the_totals';file='src/ui/wf_app_keys.c';test='tests/test_dirsize_ui.c';old='wf_app_dirsize_forget(app);';new='(void)0;'},
 @{name='dirsize_details_ignore_the_total';file='src/ui/wf_info.c';test='tests/test_dirsize_ui.c';old='folder_total ? folder_total';new='(folder_total && 0) ? folder_total'},
 # `piero --continue`: the text form refuses what it does not know, once-only records, odd paths; the
 # store keeps one writer and leaves a newer version alone; the restore takes the nearest parent, asks
 # nothing of the screen while a listing is replaced, keeps a pane only for the file it was about.
 @{name='session_unknown_record_accepted';file='src/session/wf_session.c';test='tests/test_session.c';old='return WF_E_CORRUPT; /* a record this reader does not know */';new='return WF_OK;'},
 @{name='session_duplicate_record_accepted';file='src/session/wf_session.c';test='tests/test_session.c';old='if (*seen & bit) {';new='if (0 && (*seen & bit)) {'},
 @{name='session_dotdot_path_accepted';file='src/session/wf_session.c';test='tests/test_session.c';old='(2 == j - i && L''.'' == p[i] && L''.'' == p[i + 1])) {';new='0) {'},
 @{name='session_second_writer_allowed';file='src/session/wf_session_store.c';test='tests/test_session.c';old='h = CreateFileW(s->lock_file, GENERIC_WRITE, FILE_SHARE_READ, NULL,';new='h = CreateFileW(s->lock_file, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,'},
 @{name='session_newer_version_written_over';file='src/session/wf_session_store.c';test='tests/test_session.c';old='if (WF_OK != s->write_error) {';new='if (WF_OK != s->write_error && WF_E_FORMAT != s->write_error) {'},
 @{name='session_newer_version_not_noticed';file='src/session/wf_session_store.c';test='tests/test_session.c';old='if (PEEK_NEWER == p[0] || PEEK_NEWER == p[1]) {';new='if (0) {'},
 @{name='session_restore_has_no_parent_fallback';file='src/ui/wf_app_session.c';test='tests/test_session_ui.c';old='parent = wf_path_parent_len(failed);';new='parent = 0;'},
 @{name='session_pane_for_another_file';file='src/ui/wf_app_session.c';test='tests/test_session_ui.c';old='!wcscmp(entry->name, app->session_restore.entry) &&';new='1 &&'},
 @{name='session_captured_while_navigating';file='src/ui/wf_app_session.c';test='tests/test_session_ui.c';old='if (!app->cwd || !app->cur.dir || app->want_path) {';new='if (!app->cwd || !app->cur.dir) {'},
 @{name='session_restore_accepts_any_path';file='src/ui/wf_app_session.c';test='tests/test_session_ui.c';old='if (!s || !s->dir || !wf_session_path_ok(s->dir)) {';new='if (!s || !s->dir) {'},
 @{name='session_position_not_read_by_the_listing';file='src/ui/wf_app_nav.c';test='tests/test_session_ui.c';old='bool session = app->session_restore.active && app->cwd &&';new='bool session = 0 && app->cwd &&'},
 # Session restore waits for the draft, keeps the cursor on character boundaries, and "modified"
 # always has a draft; the record's flag is checked; the writer needs the right to write; the editor is
 # opened only for a file of the folder and its place is put back.
 @{name='session_place_lost_to_the_draft_answer';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='if (s->goto_pending) { /* the place asked for while it was open */';new='if (0 && s->goto_pending) { /* the place asked for while it was open */'},
 @{name='session_modified_without_a_draft';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='out->modified = wf_ed_modified(&b->ed) && b->has_draft;';new='out->modified = wf_ed_modified(&b->ed);'},
 @{name='session_place_inside_a_character';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='if (next > p.col) {';new='if (0) {'},
 @{name='session_modified_flag_unchecked';file='src/session/wf_session.c';test='tests/test_session.c';old='if (n[4] > 1) {';new='if (0) {'},
 @{name='session_writer_without_the_right';file='src/session/wf_session_writer.c';test='tests/test_session.c';old='if (!wf_session_store_lock(store)) {';new='if (0) {'},
 @{name='session_editor_of_another_folder_opened';file='src/ui/wf_app_session.c';test='tests/test_session_ui.c';old='CSTR_EQUAL != CompareStringOrdinal(path, (int)parent, app->cwd,';new='CSTR_EQUAL == CompareStringOrdinal(path, (int)parent, app->cwd,'},
 @{name='session_editor_place_not_put_back';file='src/ui/wf_app_session.c';test='tests/test_session_ui.c';old='app->editor.goto_place(app->editor.ctx, &app->session_restore.edit);';new='(void)app->editor.goto_place;'},
 # The editor is told once that another program changed its file; the hint compares size and write time.
 @{name='editor_outside_change_said_every_time';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='if (!b || !b->path || b->external_noted) {';new='if (!b || !b->path) {'},
 @{name='editor_outside_change_ignores_size';file='src/edit/wf_efile_win.c';test='tests/test_efile_win.c';old='return !same_object(&now, was) || now.size != was->size ||';new='return !same_object(&now, was) || 0 ||'},
 @{name='editor_outside_change_ignores_time';file='src/edit/wf_efile_win.c';test='tests/test_efile_win.c';old='now.mtime != was->mtime;';new='0;'},
 # Files from the Windows clipboard (V): the parsing refuses what another program may send, a cut
 # is a cut, the application refuses what it must, and a capture ends in the paste that was asked.
 @{name='clipboard_unterminated_list_accepted';file='src/clipboard/wf_clipboard.c';test='tests/test_clipboard.c';old='if (at + len >= units) {';new='if (0) {'},
 @{name='clipboard_relative_path_accepted';file='src/clipboard/wf_clipboard.c';test='tests/test_clipboard.c';old='if (!wf_session_path_ok(copy)) { /* absolute, no . or .. component */';new='if (0) { /* absolute, no . or .. component */'},
 @{name='clipboard_thousand_and_one_accepted';file='src/clipboard/wf_clipboard.c';test='tests/test_clipboard.c';old='if (out->n >= WF_CLIPFILES_MAX) {';new='if (0) {'},
 @{name='clipboard_copy_and_move_is_a_cut';file='src/clipboard/wf_clipboard.c';test='tests/test_clipboard.c';old='return (effect & 2u) && !(effect & 1u); /* DROPEFFECT_MOVE, _COPY */';new='return (effect & 2u); /* DROPEFFECT_MOVE, _COPY */'},
 @{name='clipboard_lone_surrogate_accepted';file='src/clipboard/wf_clipboard.c';test='tests/test_clipboard.c';old='if (n > WF_CLIPFILES_PATH || !utf16_ok(s, n)) {';new='if (n > WF_CLIPFILES_PATH) {'},
 @{name='clipboard_cut_is_pasted';file='src/ui/wf_app_clip.c';test='tests/test_clip_ui.c';old='if (wf_clip_effect_is_cut(files->effect)) {';new='if (0) {'},
 @{name='clipboard_share_is_taken';file='src/ui/wf_app_clip.c';test='tests/test_clip_ui.c';old='if (wf_app_unc_root_length(dir)) {';new='if (0) {'},
 @{name='clipboard_stale_answer_taken';file='src/ui/wf_app_clip.c';test='tests/test_clip_ui.c';old='if (files->gen != app->clip_gen || !app->clip_busy) {';new='if (!app->clip_busy) {'},
 @{name='clipboard_capture_does_not_paste';file='src/ui/wf_app_clip.c';test='tests/test_clip_ui.c';old='app->paste_after_capture = true; /* the capture, then the same as `p` */';new='app->paste_after_capture = false; /* the capture, then the same as `p` */'},
 @{name='clipboard_repeated_name_asked_twice';file='src/ui/wf_app_clip.c';test='tests/test_clip_ui.c';old='if (!repeated) {';new='if (1 || !repeated) {'},
 # Y, to the clipboard: only names a reader would accept are published, the object offers a copy and nothing
 # else, answers each format as the Shell expects, is let go, and the data is handed over at the end.
 @{name='clipboard_publishes_a_relative_name';file='src/clipboard/wf_clipboard.c';test='tests/test_clipboard.c';old='if (len > WF_CLIPFILES_PATH || !wf_session_path_ok(path[i]) ||';new='if (len > WF_CLIPFILES_PATH ||'},
 @{name='clipboard_object_offers_a_move';file='src/clipboard/wf_dataobj_files_win.c';test='tests/test_clipboard.c';old='DWORD copy = DROPEFFECT_COPY; /* the one thing this object offers */';new='DWORD copy = DROPEFFECT_MOVE; /* the one thing this object offers */'},
 @{name='clipboard_object_ignores_the_index';file='src/clipboard/wf_dataobj_files_win.c';test='tests/test_clipboard.c';old='if (want->lindex != -1) {';new='if (0) {'},
 @{name='clipboard_object_lists_for_setting';file='src/clipboard/wf_dataobj_files_win.c';test='tests/test_clipboard.c';old='if (DATADIR_GET != direction) {';new='if (direction > 1000) {'},
 @{name='clipboard_object_is_never_freed';file='src/clipboard/wf_dataobj_files_win.c';test='tests/test_clipboard.c';old='if (0 == n) {';new='if (n < 0) {'},
 @{name='clipboard_data_is_not_handed_over_at_the_end';file='src/clipboard/wf_clipworker_win.c';test='tests/test_clipboard.c';old='(void)OleFlushClipboard();';new='(void)0;'},
 @{name='clipboard_y_ignores_a_request_in_flight';file='src/ui/wf_app_clip.c';test='tests/test_clip_ui.c';old='if (app->clip_busy) {';new='if (0) {'},
 @{name='clipboard_y_failure_said_as_success';file='src/ui/wf_app_clip.c';test='tests/test_clip_ui.c';old='if (WF_CLIP_OK == files->why) {';new='if (1) {'},
 # Batch rename: the plan and the pattern (DR-DRAFT-022).
 @{name='batchplan_ignores_existing_names';file='src/xfer/wf_batchplan.c';test='tests/test_batchplan.c';old='} else if (existing) {';new='} else if (existing && 0) {'},
 @{name='batchplan_ignores_duplicate_targets';file='src/xfer/wf_batchplan.c';test='tests/test_batchplan.c';old='if (targets > 1) {';new='if (targets > 1 && 0) {'},
 @{name='batchplan_compares_case_exactly';file='src/xfer/wf_batchplan.c';test='tests/test_batchplan.c';old='if (!case_sensitive && n) {';new='(void)case_sensitive; if (0) {'},
 @{name='batchplan_unchanged_owner_vacates';file='src/xfer/wf_batchplan.c';test='tests/test_batchplan.c';old='if (source->stays) {';new='if (0) {'},
 @{name='batchplan_nested_not_checked';file='src/xfer/wf_batchplan.c';test='tests/test_batchplan.c';old='if (!path_inside(parent[q].path, full)) {';new='if (!path_inside(parent[q].path, full) || 1) {'},
 @{name='batchplan_same_file_twice_allowed';file='src/xfer/wf_batchplan.c';test='tests/test_batchplan.c';old='if ((ref[i - 1].id->kind == WF_ID_STRONG) && !idref_cmp(&ref[i - 1], &ref[i])) {';new='if (0) {'},
 @{name='batchplan_name_not_validated';file='src/xfer/wf_batchplan.c';test='tests/test_batchplan.c';old='if (WF_NAME_OK != row[i].name) {';new='if (0) {'},
 @{name='batchplan_path_length_not_checked';file='src/xfer/wf_batchplan.c';test='tests/test_batchplan.c';old='if (wcslen(to->path) + 1 + len > WF_BATCH_PATH_MAX) {';new='if (0) {'},
 @{name='batchplan_cycle_not_broken';file='src/xfer/wf_batchplan.c';test='tests/test_batchplan.c';old='if (NONE != at && 1 == state[at]) {';new='if (0) {'},
 @{name='batchplan_other_volume_allowed';file='src/xfer/wf_batchplan.c';test='tests/test_batchplan.c';old='if (it->id.volume != volume ||';new='if (0 ||'},
 @{name='batchplan_link_folder_allowed';file='src/xfer/wf_batchplan.c';test='tests/test_batchplan.c';old='if (parent[it->parent].link || to->link) {';new='if (0) {'},
 @{name='batchplan_missing_id_allowed';file='src/xfer/wf_batchplan.c';test='tests/test_batchplan.c';old='if (!(it->id.kind == WF_ID_STRONG) || !(parent[it->parent].id.kind == WF_ID_STRONG) || !(to->id.kind == WF_ID_STRONG)) {';new='if (0) {'},
 # Accept an overflowing number without actually executing undefined signed
 # arithmetic: the assertion must reject acceptance, also under UBSan.
 @{name='bulkname_number_wraps';file='src/xfer/wf_bulkname.c';test='tests/test_bulkname.c';old='if (mag && (int64_t)index > INT64_MAX / mag) { return false; }';new='if (mag && (int64_t)index > INT64_MAX / mag) { *value = 0; return true; }'},
 @{name='bulkname_start_wraps';file='src/xfer/wf_bulkname.c';test='tests/test_bulkname.c';old='if ((v > 0 && p->start > INT64_MAX - v) || (v < 0 && p->start < INT64_MIN - v)) { return false; }';new='if ((v > 0 && p->start > INT64_MAX - v) || (v < 0 && p->start < INT64_MIN - v)) { *value = 0; return true; }'},
 @{name='bulkname_extension_dropped';file='src/xfer/wf_bulkname.c';test='tests/test_bulkname.c';old='if (dot) { /* the extension goes straight into the name, then takes its case */';new='if (0) {'},
 @{name='bulkname_find_never_replaces';file='src/xfer/wf_bulkname.c';test='tests/test_bulkname.c';old='if (fl && i + fl <= n && !wcsncmp(stem + i, p->find, fl)) {';new='if (fl && i + fl <= n && !wcsncmp(stem + i, p->find, fl) && 0) {'},
 @{name='bulkname_folder_has_extension';file='src/xfer/wf_bulkname.c';test='tests/test_bulkname.c';old='if (!dir) {';new='(void)dir; if (1) {'},
 @{name='bulkname_upper_case_ignored';file='src/xfer/wf_bulkname.c';test='tests/test_bulkname.c';old='if (n && WF_BULK_CASE_UPPER == c) {';new='if (0 && n && WF_BULK_CASE_UPPER == c) {'},
 @{name='bulkname_long_name_cut_silently';file='src/xfer/wf_bulkname.c';test='tests/test_bulkname.c';old='if (o->n + n > WF_BULK_NAME_MAX) {';new='if (0) {'},
 @{name='bulkname_leading_dot_is_extension';file='src/xfer/wf_bulkname.c';test='tests/test_bulkname.c';old='for (size_t i = 1; i < len; i++) {';new='for (size_t i = 0; i < len; i++) {'},
 @{name='bulkname_lone_brace_accepted';file='src/xfer/wf_bulkname.c';test='tests/test_bulkname.c';old='if (L''}'' != t[i + 1]) { return 0; }';new='if (L''}'' != t[i + 1]) { *kind = ''}''; return 1; }'},
 @{name='batchexec_replaces_an_existing_name';file='src/xfer/wf_batchexec_win.c';test='tests/test_batchexec.c';old='info->ReplaceIfExists = FALSE;';new='info->ReplaceIfExists = TRUE;'},
 @{name='batchexec_cancel_ignored';file='src/xfer/wf_batchexec_win.c';test='tests/test_batchexec.c';old='if (cancel && *cancel) {';new='(void)cancel; if (0) {'},
 @{name='batchexec_step_not_journaled_before_rename';file='src/xfer/wf_batchexec_win.c';test='tests/test_batchexec.c';old='if (WF_OK != wf_batchlog_mark(log, ''B'', k)) {';new='if (0) {'},
 @{name='batchexec_recovery_trusts_the_wrong_name';file='src/xfer/wf_batchexec_win.c';test='tests/test_batchexec.c';old='if (is_the_one(run.step[k].to, &id, rep)) {';new='if (1) {'},
 @{name='batchexec_preflight_error_ignored';file='src/xfer/wf_batchexec_win.c';test='tests/test_batchexec.c';old='rep->pre = check_source(job, i, &src[i], dest_dir, dest_index, rep);';new='check_source(job, i, &src[i], dest_dir, dest_index, rep); rep->pre = WF_BATCH_READY;'},
 @{name='batchexec_listed_identity_not_compared';file='src/xfer/wf_batchexec_win.c';test='tests/test_batchexec.c';old='if (!id_equal(&seen, &src->expected)) {';new='if (0) {'},
 # Who has the file open (L): the policy rule by rule, the identity check, the verified exit, the
 # question before every action, a stale answer, and a retry that must ask again (DR-DRAFT-024).
 @{name='lock_identity_not_checked';file='src/process/wf_process_control_win.c';test='tests/test_lock.c';old='wf_proc_ft64(created) == start;';new='(wf_proc_ft64(created) == start || 1);'},
 @{name='lock_system_rule_off';file='src/process/wf_lock.c';test='tests/test_lock.c';old='if (f->pid <= 4 || 0 == f->session) {';new='if (0 && (f->pid <= 4 || 0 == f->session)) {'},
 @{name='lock_self_may_be_ended';file='src/process/wf_lock.c';test='tests/test_lock.c';old='if (f->self_pid && f->pid == f->self_pid) {';new='if (0 && f->self_pid && f->pid == f->self_pid) {'},
 @{name='lock_critical_may_be_ended';file='src/process/wf_lock.c';test='tests/test_lock.c';old='if (f->critical) {';new='if (f->critical && 0) {'},
 @{name='lock_protected_may_be_ended';file='src/process/wf_lock.c';test='tests/test_lock.c';old='if (f->is_protected) {';new='if (f->is_protected && 0) {'},
 @{name='lock_other_user_may_be_ended';file='src/process/wf_lock.c';test='tests/test_lock.c';old='if (!f->same_user) {';new='if (!f->same_user && 0) {'},
 @{name='lock_service_may_be_ended';file='src/process/wf_lock.c';test='tests/test_lock.c';old='WF_LOCKAPP_SERVICE == f->type ||';new='(0 && WF_LOCKAPP_SERVICE == f->type) ||'},
 @{name='lock_host_of_piero_may_be_ended';file='src/process/wf_lock.c';test='tests/test_lock.c';old='if (f->ancestor) {';new='if (f->ancestor && 0) {'},
 @{name='lock_unverified_identity_may_be_ended';file='src/process/wf_lock.c';test='tests/test_lock.c';old='if (!f->identity_ok) {';new='if (!f->identity_ok && 0) {'},
 @{name='lock_unreadable_facts_may_be_ended';file='src/process/wf_lock.c';test='tests/test_lock.c';old='if (!f->facts_ok) {';new='if (!f->facts_ok && 0) {'},
 @{name='lock_access_denied_is_called_in_use';file='src/process/wf_lock.c';test='tests/test_lock.c';old='return WF_LOCK_MAYBE;';new='return WF_LOCK_IN_USE;'},
 @{name='lock_lock_violation_not_in_use';file='src/process/wf_lock.c';test='tests/test_lock.c';old='ERR_LOCK_VIOLATION == e ||';new='0 ||'},
 @{name='lock_close_success_without_exit';file='src/process/wf_process_control_win.c';test='tests/test_lock.c';old='if (WAIT_OBJECT_0 == r) {';new='if (r || 1) {'},
 @{name='lock_rm_session_not_ended';file='src/process/wf_restart_manager_win.c';test='tests/test_lock.c';old='(void)RmEndSession(session);';new='(void)session; InterlockedIncrement(&g_open_sessions);'},
 @{name='lock_stale_query_applied';file='src/ui/wf_app_lock.c';test='tests/test_lock_ui.c';old='if (a->gen == app->lock.gen) { uint32_t pid';new='if (1) { uint32_t pid'},
 @{name='lock_refused_close_escalates_to_end';file='src/ui/wf_app_lock.c';test='tests/test_lock_ui.c';old='case WF_LOCKRES_STILL:';new='case WF_LOCKRES_STILL: if (WF_LOCKREQ_CLOSE == a->req && app->lock.port.end) { (void)app->lock.port.end(app->lock.port.ctx, ++app->lock.gen, app->lock.act.pid, app->lock.act.start, app->lock.act.type); }'},
 @{name='lock_end_asks_no_question';file='src/ui/wf_app_lock.c';test='tests/test_lock_ui.c';old="wf_app_confirm_begin_ex(app, lines, 4, 'y', wf_msg(MSG_LOCK_EQ1_ARMED), end_yes1, NULL, ev)";new="((void)ev, (void)end_yes1, (end_yes2(app, NULL), WF_OK))"},
 @{name='lock_end_second_question_skipped';file='src/ui/wf_app_lock.c';test='tests/test_lock_ui.c';old="st = wf_app_confirm_begin_ex(app, lines, 3, 'k', wf_msg(MSG_LOCK_EQ2_ARMED), end_yes2, NULL, NULL);";new="(void)lines; (void)who; st = (end_yes2(app, NULL), WF_OK);"},
 @{name='lock_escape_ends_the_program';file='src/ui/wf_app_lock.c';test='tests/test_lock_ui.c';old='case WF_KEY_ESC: close_window(app);';new='case WF_KEY_ESC: end_yes2(app, NULL); close_window(app);'},
 @{name='lock_refused_row_is_asked_about';file='src/ui/wf_app_lock.c';test='tests/test_lock_ui.c';old='if (WF_DENY_NONE != h->end_deny) {';new='if (0 && WF_DENY_NONE != h->end_deny) {'},
 @{name='lock_small_window_may_end';file='src/ui/wf_app_lock.c';test='tests/test_lock_ui.c';old='return app->cols >= END_MIN_COLS && app->rows >= END_MIN_ROWS;';new='return app->cols >= 0 && app->rows >= 0 && END_MIN_COLS > 0;'},
 @{name='lock_retry_keeps_the_old_approval';file='src/ui/wf_app_retry.c';test='tests/test_lock_ui.c';old='app->slot.collisions_ok = false;';new='app->slot.collisions_ok = true;'},
 @{name='lock_rename_file_id_not_checked';file='src/ui/wf_app_input.c';test='tests/test_lock_ui.c';old='if (!(e->identity.kind == WF_ID_STRONG) || !wf_fileid_equal(&e->identity, id)) {';new='if (!(e->identity.kind == WF_ID_STRONG) || (wf_fileid_equal(&e->identity, id) && 0)) {'},
 @{name='lock_delete_retry_asks_no_question';file='src/ui/wf_app_lock.c';test='tests/test_lock_ui.c';old='ok = wf_app_recycle_retry(app, &slot);';new='ok = wf_app_recycle_retry(app, &slot); if (ok) { wf_app_recycle_confirmed(app, NULL); }'},
 # Markdown preview and the two-pane editor (DR-DRAFT-025): the grammar's decisions, the bounds, the wiring.
 @{name='md_setext_level_two';file='src/text/wf_markdown.c';test='tests/test_markdown.c';old="*level = '=' == ch ? 1 : 2;";new='*level = 1;'},
 @{name='md_snake_case_is_emphasis';file='src/text/wf_mdinline.c';test='tests/test_markdown.c';old="if ('_' == ch) {";new='if (0) {'},
 @{name='md_code_span_keeps_padding';file='src/text/wf_mdinline.c';test='tests/test_markdown.c';old="if (len >= 2 && ' ' == tmp[0] && ' ' == tmp[len - 1] && !all_space) {";new="if (len >= 2 && ' ' == tmp[0] && ' ' == tmp[len - 1] && !all_space && 0) {"},
 @{name='md_link_address_hidden';file='src/text/wf_mdinline.c';test='tests/test_markdown.c';old='} else if (plen != ul || memcmp(plain, ubuf, ul)) {';new='} else if (0) {'},
 @{name='md_table_right_align';file='src/text/wf_markdown.c';test='tests/test_markdown.c';old=': 3 == align[j] ? pad';new=': 3 == align[j] ? 0'},
 @{name='md_wide_char_is_one_cell';file='src/text/wf_markdown.c';test='tests/test_markdown.c';old='width += u[i].w;';new='width += 1;'},
 @{name='md_ordered_list_counts_on';file='src/text/wf_markdown.c';test='tests/test_markdown.c';old='(unsigned)(first.num + (uint32_t)t), first.ch);';new='(unsigned)first.num, first.ch);'},
 @{name='md_output_unbounded';file='src/text/wf_markdown.c';test='tests/test_markdown.c';old='if (d->nlines >= WF_MD_OUT_LINES || d->text_len >= WF_MD_OUT_BYTES) {';new='if (d->text_len >= WF_MD_OUT_BYTES) {'},
 @{name='md_name_case_matters';file='src/text/wf_markdown.c';test='tests/test_markdown.c';old="if (ch >= L'A' && ch <= L'Z') {";new='if (0) {'},
 @{name='md_preview_not_drawn_as_markdown';file='src/ui/wf_app_render.c';test='tests/test_markdown_ui.c';old='const WfMdDoc *md = wf_app_md_doc(app, w);';new='const WfMdDoc *md = (wf_app_md_doc(app, w), (const WfMdDoc *)0);'},
 @{name='md_pane_wide_threshold';file='src/edit/wf_emdpane.c';test='tests/test_markdown_ui.c';old=': WF_MD_WIDE_MIN);';new=': 10);'},
 @{name='md_pane_rebuilt_without_waiting';file='src/edit/wf_emdpane.c';test='tests/test_markdown_ui.c';old='p->due_ms = now_ms + WF_MDP_DEBOUNCE_MS;';new='p->due_ms = now_ms;'},
 @{name='md_left_pane_ignores_the_right';file='src/edit/wf_eview.c';test='tests/test_markdown_ui.c';old='int side = wf_es_md_cols(s, b);';new='int side = 0;'},
 @{name='gitpane_left_pane_ignores_the_right';file='src/edit/wf_eview.c';test='tests/test_git_edit.c';old='side = wf_es_git_cols(s, b); /* one pane at the right, never two */';new='side = 0; /* one pane at the right, never two */'},
 @{name='md_alt_m_unbound';file='src/edit/wf_ekeymap.c';test='tests/test_markdown_ui.c';old="ALT('m', WF_CMD_MD_PREVIEW, ""M-M""),";new="ALT('M', WF_CMD_MD_PREVIEW, ""M-M""),"},
 @{name='fuzzy_prefilter_disabled';file='src/search/wf_fuzzy.c';test='tests/test_fuzzy.c';old='if (!wf_fz_prefilter(q, name, len)) {';new='if (0 && !wf_fz_prefilter(q, name, len)) {'},
 @{name='fuzzy_run_bonus_removed';file='src/search/wf_fuzzy.c';test='tests/test_fuzzy.c';old='b = max32(b, max32(B_CONSEC, first));';new='b = max32(b, first);'},
 @{name='fuzzy_word_bonus_removed';file='src/search/wf_fuzzy.c';test='tests/test_fuzzy.c';old='case K_NONWORD: return B_NONWORD;';new='case K_NONWORD: return 0;'},
 @{name='fuzzy_topk_order_reversed';file='src/search/wf_fuzzy.c';test='tests/test_fuzzy.c';old='return a.score > b.score ||';new='return a.score < b.score ||'},
 @{name='fuzzy_surrogate_pair_unchecked';file='src/search/wf_fuzzy.c';test='tests/test_fuzzy.c';old='if (d >= 0xDC00 && d <= 0xDFFF) {';new='if (d >= 0x20) {'},
 @{name='fuzzy_name_limit_off_by_one';file='src/search/wf_fuzzy.c';test='tests/test_fuzzy.c';old='while (i < len && n < WF_FZ_NAME_MAX) {';new='while (i < len && n + 1 < WF_FZ_NAME_MAX) {'},
 @{name='fuzzy_cancel_ignored';file='src/search/wf_fuzzy.c';test='tests/test_fuzzy.c';old='if (cancel && 0 == i % CANCEL_EVERY && cancel(cancel_ctx)) {';new='if (cancel && 0 == i % CANCEL_EVERY && cancel(cancel_ctx) && st.scanned > nvis) {'},
 @{name='fuzzy_katakana_not_folded';file='src/search/wf_fuzzy.c';test='tests/test_fuzzy.c';old='return c - 0x60;';new='return c;'},
 @{name='fuzzy_ui_stale_answer_taken';file='src/ui/wf_app_fuzzy.c';test='tests/test_fuzzy_ui.c';old='if (!z->active || a->gen != z->gen) {';new='if (!z->active) {'},
 @{name='fuzzy_ui_end_does_not_release_the_worker';file='src/ui/wf_app_fuzzy.c';test='tests/test_fuzzy_ui.c';old='static void finish(WfApp *app, bool restore) { WfFuzzy *z = &app->fz; release_worker(app);';new='static void finish(WfApp *app, bool restore) { WfFuzzy *z = &app->fz;'},
 @{name='fuzzy_ui_guard_does_not_release_the_worker';file='src/ui/wf_app_fuzzy.c';test='tests/test_fuzzy_ui.c';old='if (!z->active) { return; } release_worker(app);';new='if (!z->active) { return; }'},
 @{name='fuzzy_ui_esc_keeps_the_cursor_on_the_result';file='src/ui/wf_app_fuzzy.c';test='tests/test_fuzzy_ui.c';old='case WF_KEY_ESC: finish(app, true);';new='case WF_KEY_ESC: finish(app, false);'},
 @{name='fuzzy_ui_folder_change_unnoticed';file='src/ui/wf_app_fuzzy.c';test='tests/test_fuzzy_ui.c';old='if (z->cwd_version != app->cwd_version || !app->cur.dir) {';new='if (!app->cur.dir) {'},
 @{name='fuzzy_ui_more_results_never_asked';file='src/ui/wf_app_fuzzy.c';test='tests/test_fuzzy_ui.c';old='} else if (z->nhit < z->matched && !z->pending) {';new='} else if (0) {'},
 @{name='fuzzy_ui_selection_not_kept_across_a_refresh';file='src/ui/wf_app_fuzzy.c';test='tests/test_fuzzy_ui.c';old='z->sel = i; break;';new='z->sel = 0; break;'},
 @{name='fuzzyworker_release_does_not_cancel';file='src/search/wf_fuzzyworker.c';test='tests/test_fuzzyworker_win.c';old='InterlockedExchange(&w->live_gen, 0);';new='(void)w->live_gen;'},
 @{name='batch_ui_run_without_the_question';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='ask_to_run(app, ev);';new='if (ev->vk == 0xFFFF) { ask_to_run(app, ev); } batch_yes(app, NULL);'},
 @{name='batch_ui_refused_rows_still_run';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='return pl && 0 == pl->n_refused && pl->n_steps > 0;';new='return pl && (0 == pl->n_refused || pl->n_steps > 0) && pl->n_steps > 0;'},
 @{name='batch_ui_nothing_to_do_can_run';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='return pl && 0 == pl->n_refused && pl->n_steps > 0;';new='return pl && 0 == pl->n_refused;'},
 @{name='batch_ui_cancel_ignored';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='if (b->port.cancel) {';new='if (b->port.cancel && b->recover) {'},
 @{name='batch_ui_done_for_a_rollback';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='switch (r->result) {';new='switch (r->result == WF_BX_ROLLED_BACK ? WF_BX_SUCCESS : r->result) {'},
 @{name='batch_ui_gate_always_open';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='return NULL != app->edit.submit;';new='return NULL != app || NULL != app->edit.submit;'},
 @{name='batch_ui_gate_not_checked_at_the_answer';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='if (!gate_open(app)) { close_screen(app);';new='if (0 && !gate_open(app)) { close_screen(app);'},
 @{name='batch_ui_recovery_not_gated_at_the_answer';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='if (!gate_open(app) || !b->port.recover) {';new='if (!b->port.recover) {'},
 @{name='batch_ui_stale_nav_gen_ignored';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='b->nav_gen != app->nav_gen ||';new='(b->nav_gen != app->nav_gen && app->want_path) ||'},
 @{name='batch_ui_stale_cwd_version_ignored';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='b->cwd_version != app->cwd_version ||';new='(b->cwd_version != app->cwd_version && app->want_path) ||'},
 @{name='batch_ui_stale_folder_identity_ignored';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='!wf_fileid_equal(&b->dir_id, &app->cur.dir->identity);';new='!wf_fileid_equal(&b->dir_id, &b->dir_id);'},
 @{name='batch_ui_replan_keeps_the_old_binding';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='b->nav_gen = app->nav_gen; b->cwd_version = app->cwd_version; if (app->cur.dir) {';new='b->cwd_version = app->cwd_version; if (app->cur.dir) {'},
 @{name='batch_ui_recovery_runs_unasked';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='b->ask_pending = b->unfinished > 0;';new='b->ask_pending = b->unfinished > 0; if (b->ask_pending) { recover_yes(app, NULL); }'},
 @{name='batch_ui_cursor_on_the_old_name';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='select = first_renamed(b);';new='(void)first_renamed(b); select = b->item[0].name;'},
 @{name='batch_ui_preview_esc_keeps_the_plan';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='drop_job(app); memset(&b->pre, 0, sizeof b->pre); b->stage = WF_BS_PATTERN;';new='memset(&b->pre, 0, sizeof b->pre); b->stage = WF_BS_PATTERN;'},
 @{name='batchworker_cancel_ignored';file='src/xfer/wf_batchworker.c';test='tests/test_batchworker_win.c';old='if (w) { InterlockedExchange(&w->cancel, 1); }';new='if (w) { (void)w->cancel; }'},
 @{name='batchworker_stop_does_not_cancel';file='src/xfer/wf_batchworker.c';test='tests/test_batchworker_win.c';old='InterlockedExchange(&w->cancel, 1); return wf_wc_stop(&w->core, STOP_WAIT_MS, false, worker_free, w);';new='return wf_wc_stop(&w->core, STOP_WAIT_MS, false, worker_free, w);'},
 @{name='batchworker_runs_a_job_that_is_not_ready';file='src/xfer/wf_batchworker.c';test='tests/test_batchworker_win.c';old='if (!job || !wf_batch_job_ready(job)) {';new='if (!job) {'},
 @{name='batchworker_second_request_accepted';file='src/xfer/wf_batchworker.c';test='tests/test_batchworker_win.c';old='if (w->core.stop || REQ_NONE != w->req || w->core.busy || w->has_result) {';new='if (w->core.stop) {'},
 @{name='batchworker_recovers_receipts_again';file='src/xfer/wf_batchworker.c';test='tests/test_batchworker_win.c';old='todo = !j.ended || WF_BJ_RECOVERY_REQUIRED == j.end;';new='todo = true;'},
 @{name='ntfsfacts_remote_path_is_opened';file='src/fs/wf_ntfsfacts_win.c';test='tests/test_ntfsfacts.c';old='if (!path || wf_ntfsfacts_remote(path)) {';new='if (!path) {'},
 @{name='ntfsfacts_asks_for_data_access';file='src/fs/wf_ntfsfacts_win.c';test='tests/test_ntfsfacts.c';old='h = CreateFileW(path, FILE_READ_ATTRIBUTES,';new='h = CreateFileW(path, FILE_READ_ATTRIBUTES | GENERIC_READ,'},
 @{name='ntfsfacts_follows_the_link';file='src/fs/wf_ntfsfacts_win.c';test='tests/test_ntfsfacts.c';old='FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);';new='FILE_FLAG_BACKUP_SEMANTICS, NULL);'},
 @{name='ntfsfacts_main_stream_counted';file='src/fs/wf_ntfsfacts_win.c';test='tests/test_ntfsfacts.c';old='if (!(7 == n && 0 == wcsncmp(name, L"::$DATA", 7))) {';new='if (1) {'},
 @{name='ntfsfacts_links_off_by_one';file='src/fs/wf_ntfsfacts_win.c';test='tests/test_ntfsfacts.c';old='f->links = bi.nNumberOfLinks;';new='f->links = bi.nNumberOfLinks + 1;'},
 @{name='ntfsfacts_target_from_wrong_name';file='src/fs/wf_ntfsfacts_win.c';test='tests/test_ntfsfacts.c';old='buf + head + prn_off, prn_len / 2, false, f->target);';new='buf + head + sub_off, prn_len / 2, false, f->target);'},
 @{name='ntfsfacts_stream_size_is_allocation';file='src/fs/wf_ntfsfacts_win.c';test='tests/test_ntfsfacts.c';old='o->size = (uint64_t)r.size.QuadPart;';new='o->size = (uint64_t)r.allocated.QuadPart;'},
 @{name='ntfsfacts_offline_ignored';file='src/ui/wf_info.c';test='tests/test_ntfsfacts.c';old='{ATTR_OFFLINE, MSG_NTFS_OFFLINE}};';new='{0, MSG_NTFS_OFFLINE}};'},
 @{name='ntfsfacts_one_name_has_others';file='src/ui/wf_info.c';test='tests/test_ntfsfacts.c';old='} else if (f->links <= 1) {';new='} else if (f->links <= 0) {'},
 @{name='ntfsfacts_more_streams_not_said';file='src/ui/wf_info.c';test='tests/test_ntfsfacts.c';old='if (f->n_streams > shown) {';new='if (0) {'},
 @{name='ntfsfacts_ui_never_reads';file='src/ui/wf_app_input.c';test='tests/test_ntfsfacts.c';old='wf_ntfsfacts_read(full, &facts);';new='(void)full;'},
 @{name='table_break_in_quotes_kept';file='src/preview/wf_table.c';test='tests/test_table.c';old='st = wf_buf_putc(&field, ''\n'' == c ? '' '' : c);';new='st = wf_buf_putc(&field, c);'},
 @{name='table_doubled_quote_dropped';file='src/preview/wf_table.c';test='tests/test_table.c';old='if (i + 1 < n && ''"'' == s[i + 1]) {';new='if (0) {'},
 @{name='table_row_cut_not_said';file='src/preview/wf_table.c';test='tests/test_table.c';old='if (k < n) {';new='if (k < n && 0) {'},
 @{name='table_last_column_dropped';file='src/preview/wf_table.c';test='tests/test_table.c';old='if (col >= WF_TABLE_COLS) {';new='if (col >= WF_TABLE_COLS - 1) {'},
 @{name='table_header_rule_missing';file='src/preview/wf_table.c';test='tests/test_table.c';old='if (0 == r && WF_OK == st) {';new='if (0 && WF_OK == st) {'},
 @{name='table_pad_counts_bytes';file='src/preview/wf_table.c';test='tests/test_table.c';old='st = pad_to(out, w, width[c]);';new='(void)w; st = pad_to(out, cell ? (int)strlen(cell) : 0, width[c]);'},
 @{name='table_trailing_blank_cells';file='src/preview/wf_table.c';test='tests/test_table.c';old='if (cell && *cell) {';new='if (cell) {'},
 @{name='table_cut_not_marked_truncated';file='src/preview/wf_table.c';test='tests/test_table.c';old='p->truncated = p->truncated || cut;';new='p->truncated = p->truncated && cut;'},
 @{name='table_semicolon_not_detected';file='src/preview/wf_table.c';test='tests/test_table.c';old='return 0 == commas && semis ? '';'' : delim;';new='return 0 == commas && semis && 0 ? '';'' : delim;'},
 @{name='table_blank_lines_kept';file='src/preview/wf_table.c';test='tests/test_table.c';old='if (any || field.len) {';new='if (any || field.len || 1) {'},
 @{name='table_quote_in_the_middle_opens';file='src/preview/wf_table.c';test='tests/test_table.c';old='if (''"'' == c && 0 == field.len && !was_quoted) {';new='if (''"'' == c && !was_quoted) {'},
 @{name='table_tabs_become_spaces';file='src/preview/wf_preview.c';test='tests/test_table.c';old='if (keep_tabs && ''\t'' == cp) {';new='if (keep_tabs && ''\t'' == cp && 0) {'},
 @{name='hash_ui_folder_is_hashed';file='src/ui/wf_app_hash.c';test='tests/test_hash_ui.c';old='if (e->flags & WF_ENT_DIR) {';new='if (e->flags & WF_ENT_DIR && 0) {'},
 @{name='hash_ui_link_is_followed';file='src/ui/wf_app_hash.c';test='tests/test_hash_ui.c';old='if (e->flags & WF_ENT_LINK) {';new='if (e->flags & WF_ENT_LINK && 0) {'},
 @{name='hash_ui_stale_answer_taken';file='src/ui/wf_app_hash.c';test='tests/test_hash_ui.c';old='if (!app->hash.running || u->gen != app->hash.gen) {';new='if (!app->hash.running) {'},
 @{name='hash_ui_answer_after_stop_taken';file='src/ui/wf_app_hash.c';test='tests/test_hash_ui.c';old='app->hash.running = false; /* what the worker still delivers is dropped */';new='/* kept running */'},
 @{name='hash_ui_digest_for_another_size';file='src/ui/wf_app_hash.c';test='tests/test_hash_ui.c';old='e->size == app->hash.size &&';new='1 &&'},
 @{name='hash_ui_digest_for_another_time';file='src/ui/wf_app_hash.c';test='tests/test_hash_ui.c';old='e->mtime == app->hash.last_write;';new='1;'},
 @{name='hash_ui_digest_for_another_file';file='src/ui/wf_app_hash.c';test='tests/test_hash_ui.c';old='same = !wcscmp(full, app->hash.path) &&';new='same = 1 &&'},
 @{name='hash_ui_changed_file_not_said';file='src/ui/wf_app_hash.c';test='tests/test_hash_ui.c';old='case WF_E_CHANGED:';new='case WF_E_NOMEM:'},
 @{name='hash_ui_older_request_not_ended';file='src/ui/wf_app_hash.c';test='tests/test_hash_ui.c';old='if (app->hash.running && app->hash_port.cancel) {';new='if (0) {'},
 @{name='hash_ui_running_not_shown_in_details';file='src/ui/wf_app_hash.c';test='tests/test_hash_ui.c';old='if (full && app->hash.running && app->hash.path &&';new='if (0 && full && app->hash.running && app->hash.path &&'},
 @{name='hash_ui_progress_percent_wrong';file='src/ui/wf_app_hash.c';test='tests/test_hash_ui.c';old='h->total ? (unsigned)(h->done * 100u / h->total) : 0u);';new='h->total ? (unsigned)(h->done * 10u / h->total) : 0u);'},
 @{name='sha256_cancel_ignored';file='src/xfer/wf_sha256_win.c';test='tests/test_sha256.c';old='if (cancel && cancel(cancel_ctx)) {';new='if (cancel && cancel(cancel_ctx) && 0) {'},
 @{name='sha256_rewrite_in_place_not_seen';file='src/xfer/wf_sha256_win.c';test='tests/test_sha256.c';old='after.last_write != before.last_write || done != expected) {';new='done != expected) {'},
 @{name='sha256_prefix_reads_to_the_end';file='src/xfer/wf_sha256_win.c';test='tests/test_sha256.c';old='expected = limit && limit < before.size ? limit : before.size;';new='expected = before.size; (void)limit;'},
 @{name='dupes_empty_files_are_candidates';file='src/search/wf_dupes.c';test='tests/test_dupes.c';old='if (b - a >= 2 && idx[a]->size > 0) {';new='if (b - a >= 2) {'},
 @{name='dupes_prefix_step_skipped';file='src/search/wf_dupes.c';test='tests/test_dupes.c';old='if (size <= WF_DUPE_PREFIX) {';new='if (size <= WF_DUPE_PREFIX || 1) {'},
 @{name='dupes_whole_file_read_for_every_prefix';file='src/search/wf_dupes.c';test='tests/test_dupes.c';old='if (j - i >= 2) {';new='if (1) {'},
 @{name='dupes_a_single_file_is_a_group';file='src/search/wf_dupes.c';test='tests/test_dupes.c';old='if (k >= 2) {';new='if (k >= 1) {'},
 @{name='dupes_unreadable_file_kept';file='src/search/wf_dupes.c';test='tests/test_dupes.c';old='if (WF_OK != st) {';new='if (WF_OK != st && 0) {'},
 @{name='dupes_cancel_ignored';file='src/search/wf_dupes.c';test='tests/test_dupes.c';old='if (r->ops->cancelled && r->ops->cancelled(r->ctx)) {';new='if (r->ops->cancelled && r->ops->cancelled(r->ctx) && 0) {'},
 @{name='dupes_stop_of_the_screen_ignored';file='src/search/wf_dupes.c';test='tests/test_dupes.c';old='if (!r->ops->group(r->ctx, out, k)) {';new='if (!r->ops->group(r->ctx, out, k) && 0) {'},
 @{name='dupes_last_copy_can_be_marked';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='if (!another_is_free(h, i)) {';new='if (!another_is_free(h, i) && 0) {'},
 @{name='dupes_whole_group_can_be_recycled';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='if (h->dupes && !each_group_keeps_one(h, idx, n)) {';new='if (0 && h->dupes && !each_group_keeps_one(h, idx, n)) {'},
 @{name='dupes_ctrl_a_marks_all';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='if (h->dupes && ((WF_EV_CTRL == ev->type && ''a'' == ev->ch) ||';new='if (0 && h->dupes && ((WF_EV_CTRL == ev->type && ''a'' == ev->ch) ||'},
 @{name='dupes_group_ends_at_its_first_file';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='while (*b < h->n && h->hit[*b].group == h->hit[i].group) {';new='while (0 && *b < h->n && h->hit[*b].group == h->hit[i].group) {'},
 @{name='dupes_progress_never_shown';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='h->read_total = b->read_total;';new='h->read_total = 0;'},
 @{name='dupes_batch_of_a_search_taken';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='b->gen != h->gen || b->dupes != h->dupes) {';new='b->gen != h->gen) {'},
 @{name='dupes_group_number_never_set';file='src/search/wf_searchworker.c';test='tests/test_searchworker_win.c';old='h->group = r->group;';new='h->group = 1;'},
 @{name='dupes_asked_without_a_hasher';file='src/search/wf_searchworker.c';test='tests/test_searchworker_win.c';old='if (w->core.stop || (dupes && !w->hash)) {';new='if (w->core.stop) {'},
 @{name='dupes_progress_total_lost';file='src/search/wf_searchworker.c';test='tests/test_searchworker_win.c';old='r->ptotal = total;';new='r->ptotal = total * 0;'},
 @{name='dupes_changed_size_not_seen';file='src/xfer/wf_sha256_win.c';test='tests/test_searchworker_win.c';old='bool same = before.size == seen->size &&';new='bool same = (before.size == seen->size || 1) &&'},
 @{name='dupes_prefix_hashed_whole';file='src/search/wf_dupehash_win.c';test='tests/test_searchworker_win.c';old='limit < seen->size ? limit : 0,';new='0 * limit,'},
 @{name='sha256_folder_not_refused';file='src/xfer/wf_sha256_win.c';test='tests/test_sha256.c';old='if (attributes & FILE_ATTRIBUTE_DIRECTORY) {';new='if (0) {'},
 @{name='sha256_opened_without_read_access';file='src/xfer/wf_sha256_win.c';test='tests/test_sha256.c';old='CreateFileW(path, GENERIC_READ,';new='CreateFileW(path, FILE_READ_ATTRIBUTES,'},
 @{name='sha256_hex_high_nibble_wrong';file='src/xfer/wf_hash.c';test='tests/test_sha256.c';old='out[2 * i] = hex[digest[i] >> 4];';new='out[2 * i] = hex[digest[i] & 15];'},
 @{name='hashworker_cancel_does_not_cancel';file='src/xfer/wf_hashworker.c';test='tests/test_sha256.c';old='InterlockedExchange(&w->live_gen, 0);';new='(void)w;'},
 @{name='hashworker_newer_request_not_live';file='src/xfer/wf_hashworker.c';test='tests/test_sha256.c';old='InterlockedExchange(&w->live_gen, (LONG)gen);';new='(void)gen;'},
 @{name='verify_wording_for_an_unchecked_copy';file='src/ui/wf_app_copy.c';test='tests/test_copy_ui.c';old='if (WF_VERIFY_NONE == r->verify) {';new='if (WF_VERIFY_NONE == r->verify && 0) {'},
 @{name='verify_wording_for_any_failure';file='src/ui/wf_app_copy.c';test='tests/test_copy_ui.c';old='if (WF_CF_VERIFY_FAILED == r->failures[i].kind) {';new='if (1) {'},
 @{name='verify_wording_never_used';file='src/ui/wf_app_copy.c';test='tests/test_copy_ui.c';old='} else if (problems && verify_mismatch(r)) {';new='} else if (problems && verify_mismatch(r) && 0) {'},
 @{name='history_undo_success_keeps_the_entry';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='wf_app_undo_retire(app, b->undo_uid);';new='(void)0;'},
 @{name='history_undo_retires_on_any_result';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='if (!d->recover && b->undo_uid) {';new='if (!d->recover && b->undo_uid) { wf_app_undo_retire(app, b->undo_uid);'},
 @{name='history_undo_ignores_the_gate';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='if (!gate_open(app) || !b->port.prepare || !b->port.run) {';new='if (!b->port.prepare || !b->port.run) {'},
 @{name='history_undo_runs_a_refused_plan';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='if (!executable(app) || !pl || pl->n_skipped) {';new='if (!pl || pl->n_skipped) {'},
 @{name='history_batch_row_has_no_undo';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='wf_app_hist_add(app, head, keep_for_undo(app),';new='(void)keep_for_undo; wf_app_hist_add(app, head, 0,'},
 @{name='history_undo_says_done_not_undone';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='line(b, b->undo_uid ? MSG_BATCH_UNDO_DONE : MSG_BATCH_R_DONE,';new='line(b, MSG_BATCH_R_DONE,'},
 @{name='history_undo_goes_back_to_the_new_name';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='wf_path_dup(b->item[i].name);';new='wf_path_dup(b->item[i].to);'},
 @{name='history_expired_row_not_marked';file='src/ui/wf_app_undo.c';test='tests/test_batch_ui.c';old='undo_row_left(app, 0, false);';new='(void)0;'},
 @{name='history_undone_row_not_marked';file='src/ui/wf_app_undo.c';test='tests/test_copy_ui.c';old='undo_row_left(app, i, true);';new='(void)0;'},
 @{name='history_newest_row_is_the_oldest';file='src/ui/wf_app_joblog.c';test='tests/test_batch_ui.c';old='app->undo_n ? &app->undo[app->undo_n - 1] : NULL;';new='app->undo_n ? &app->undo[0] : NULL;'},
 @{name='history_every_row_marked_gone';file='src/ui/wf_app_joblog.c';test='tests/test_copy_ui.c';old='if (WF_HU_KEPT == e->undo && e->undo_uid == uid) {';new='if (WF_HU_KEPT == e->undo) {'},
 @{name='history_rename_row_has_no_undo';file='src/ui/wf_app_input.c';test='tests/test_copy_ui.c';old='wf_app_hist_add(app, head, wsl ? 0 : undo_uid,';new='(void)undo_uid; wf_app_hist_add(app, head, 0,'},
 @{name='history_move_has_no_undo_entry';file='src/ui/wf_app_xfer.c';test='tests/test_copy_ui.c';old='move_history_row(app, moved, record_move_undo(app));';new='(void)record_move_undo; move_history_row(app, moved, 0);'},
 @{name='history_copy_row_has_no_undo';file='src/ui/wf_app_copy.c';test='tests/test_copy_ui.c';old='undo_uid = wf_app_undo_keep_created(app, r);';new='(void)wf_app_undo_keep_created(app, r);'},
 @{name='tabs_new_tab_goes_to_the_end';file='src/ui/wf_app_tabs.c';test='tests/test_tabs.c';old='at = app->tabs.cur + 1;';new='at = app->tabs.n;'},
 @{name='tabs_close_hides_a_failed_walk';file='src/ui/wf_app_tabs.c';test='tests/test_tabs.c';old='if (WF_OK != st) { app->tabs.going = false; app->tabs.closing = false;';new='if (0) { app->tabs.going = false; app->tabs.closing = false;'},
 @{name='replace_all_cursor_inside_a_character';file='src/edit/wf_editor.c';test='tests/test_editor.c';old='while (keep.col > 0 && keep.col < l->len &&';new='while ((void)l, 0 && keep.col > 0 && keep.col < l->len &&'},
 @{name='undo_group_repeats_done_records';file='src/edit/wf_eundo.c';test='tests/test_editor.c';old='*last = u->pos - 1;';new='*last = *last;'},
 @{name='redo_group_repeats_done_records';file='src/edit/wf_eundo.c';test='tests/test_editor.c';old='*first = u->pos; /* likewise for a group redone in part */';new='*first = *first;'},
 @{name='undo_apply_without_a_copy';file='src/edit/wf_editor.c';test='tests/test_editor.c';old='st = rollback_begin(&rb, e, r->at, end, put, put_n);';new='memset(&rb, 0, sizeof rb); st = WF_OK; (void)put_n;'},
 @{name='vertical_move_changes_line_first';file='src/edit/wf_editor.c';test='tests/test_editor.c';old='to.line--;';new='e->cur.line--;'},
 @{name='session_save_only_run_overwrites_newer';file='src/session/wf_session_store.c';test='tests/test_session.c';old='(void)wf_session_store_read(s, &saved, NULL);';new='memset(&saved, 0, sizeof saved);'},
 @{name='wsl_undo_stopped_keeps_done_items';file='src/ui/wf_app_wsl.c';test='tests/test_wsl_ui.c';old='undo_drop_done(&app->wundo, app->wseq.done);';new='(void)undo_drop_done;'},
 @{name='tabs_cur_moves_before_the_folder_arrives';file='src/ui/wf_app_tabs.c';test='tests/test_tabs.c';old='app->tabs.to = to; app->tabs.closing = true;';new='app->tabs.cur = to; app->tabs.to = to; app->tabs.closing = true;'},
 @{name='new_file_loses_the_configured_break';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='b->ed.text.default_eol = b->ed.opt.new_eol;';new='(void)b;'},
 @{name='editor_cells_ignore_the_drawn_label';file='src/edit/wf_etext.c';test='tests/test_editor.c';old='	if (wf_et_special_scalar(cp)) {';new='	if (0 && wf_et_special_scalar(cp)) {'},
 @{name='copy_source_rewritten_same_size';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='snap->size == s->size && snap->mtime == s->mtime;';new='snap->size == s->size;'},
 @{name='copy_failure_path_loses_the_sub_folder';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='dir, L''\\'' == dir[wcslen(dir) - 1] ? L"" : L"\\", rel);';new='dir, L"", L"x");'},
 @{name='copy_paths_limited_to_a_buffer';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='if (na > SIZE_MAX / sizeof *out - nb - nc - 1) {';new='if (na > 1000) {'},
 @{name='go_requests_restart_at_zero';file='src/ui/wf_app_go.c';test='tests/test_path_bar.c';old='app->go.gen = gen;';new='(void)gen;'},
 @{name='fuzzy_enter_takes_the_old_entries';file='src/ui/wf_app_fuzzy.c';test='tests/test_fuzzy_ui.c';old='return z->pending || z->dirty;';new='(void)z; return false;'},
 @{name='vim_counted_e_steps_every_time';file='src/edit/wf_evim.c';test='tests/test_esession.c';old='if (0 == i && p.col < l->len) {';new='if (p.col < l->len) {'},
 @{name='draft_names_fold_case_always';file='src/edit/wf_edraft_win.c';test='tests/test_edraft.c';old='return known && !(info.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR);';new='return known || 1;'},
 @{name='draft_header_path_not_checked';file='src/edit/wf_edraft_win.c';test='tests/test_edraft.c';old='!header_names(sl[pick].header, path, fold)) {';new='(0 && !header_names(sl[pick].header, path, fold))) {'},
 @{name='draft_restore_goes_through_the_paste';file='src/edit/wf_editor.c';test='tests/test_esession.c';old='return replace_selection(e, utf8, n, false, WF_UK_OTHER);';new='return wf_ed_insert_text(e, utf8, n);'},
 @{name='completion_skips_only_the_typed_prefix';file='src/edit/wf_editor.c';test='tests/test_editor.c';old='s == e->comp_start && s + j == e->cur.col) {';new='s + j == e->cur.col && s + plen <= e->cur.col && j == plen) {'},
 @{name='eol_label_ignores_added_breaks';file='src/edit/wf_etext.c';test='tests/test_esession.c';old='if (sh.breaks) {';new='if (0 && sh.breaks) {'},
 @{name='filter_narrows_after_any_change';file='src/ui/wf_view.c';test='tests/test_dir.c';old='0 == wcsncmp(filter, before, wcslen(before));';new='1;'},
 @{name='wrap_cache_ignores_the_text';file='src/edit/wf_eview.c';test='tests/test_esession.c';old='c->keyed && c->gen == gen && c->width == width && c->tab == tab) {';new='c->keyed && c->width == width && c->tab == tab) {'},
 @{name='wrap_cache_ignores_the_width';file='src/edit/wf_eview.c';test='tests/test_esession.c';old='c->keyed && c->gen == gen && c->width == width && c->tab == tab) {';new='c->keyed && c->gen == gen && c->tab == tab) {'},
 @{name='tabs_limit_is_one_short';file='src/ui/wf_app_tabs.c';test='tests/test_tabs.c';old='if (wf_app_tab_count(app) >= WF_TABS_MAX) {';new='if (wf_app_tab_count(app) >= WF_TABS_MAX - 1) {'},
 @{name='tabs_previous_goes_forward';file='src/ui/wf_app_tabs.c';test='tests/test_tabs.c';old='to = by < 0 ? (app->tabs.cur + n - 1) % n : (app->tabs.cur + 1) % n;';new='to = (app->tabs.cur + 1) % n; (void)by;'},
 @{name='tabs_cursor_name_not_restored';file='src/ui/wf_app_tabs.c';test='tests/test_tabs.c';old='app, app->tabs.t[to].path, app->tabs.t[to].name, SIZE_MAX);';new='app, app->tabs.t[to].path, NULL, SIZE_MAX);'},
 @{name='tabs_current_tab_not_remembered';file='src/ui/wf_app_tabs.c';test='tests/test_tabs.c';old='if (!store_current(app)) {';new='if (0 && !store_current(app)) {'},
 @{name='tabs_one_tab_left_keeps_the_strip';file='src/ui/wf_app_tabs.c';test='tests/test_tabs.c';old='wf_app_tabs_destroy(app); /* back to a single tab: no strip */';new='(void)0;'},
 @{name='places_failed_save_is_kept';file='src/ui/wf_app_tabs.c';test='tests/test_tabs.c';old='why, sizeof why)) {';new='why, sizeof why) && 0) {'},
 @{name='places_archive_folder_is_kept';file='src/ui/wf_app_tabs.c';test='tests/test_tabs.c';old='if (wf_vpath_is(app->cwd) || wf_sftp_place_is(app->cwd)) {';new='if (wf_sftp_place_is(app->cwd)) {'},
 @{name='places_newer_configuration_is_written';file='src/ui/wf_app_tabs.c';test='tests/test_tabs.c';old='if (app->config->schema_version > WF_CONFIG_SCHEMA_VERSION) {';new='if (0) {'},
 @{name='places_go_takes_the_first';file='src/ui/wf_app_tabs.c';test='tests/test_tabs.c';old='utf8 = app->config ? app->config->places[letter - ''a''] : "";';new='utf8 = app->config ? app->config->places[0] : "";'},
 @{name='tabs_path_drawn_over_the_strip';file='src/ui/wf_app_render.c';test='tests/test_tabs.c';old='wf_app_put_fit(f, strip ? strip + 1 : 0, 0,';new='wf_app_put_fit(f, 0, 0,'},
 @{name='places_repeated_letter_replaces';file='src/config/wf_config.c';test='tests/test_config.c';old='dup = c->places[slot][0] ? DYN_DUPLICATE : DYN_OK;';new='dup = DYN_OK;'},
 @{name='places_control_character_accepted';file='src/config/wf_config.c';test='tests/test_config.c';old='if ((unsigned char)value[i] < 0x20 || 0x7F == (unsigned char)value[i]) {';new='if ((unsigned char)value[i] < 0x01 || 0x7F == (unsigned char)value[i]) {'},
 @{name='query_only_the_first_word_counts';file='src/search/wf_query.c';test='tests/test_query.c';old='if (!wcsstr(folded, q->word[i])) {';new='if (!wcsstr(folded, q->word[0])) {'},
 @{name='query_size_greater_is_inclusive';file='src/search/wf_query.c';test='tests/test_query.c';old='if (q->has_gt && size <= q->gt) {';new='if (q->has_gt && size < q->gt) {'},
 @{name='query_size_less_is_inclusive';file='src/search/wf_query.c';test='tests/test_query.c';old='if (q->has_lt && size >= q->lt) {';new='if (q->has_lt && size > q->lt) {'},
 @{name='query_newer_is_exclusive';file='src/search/wf_query.c';test='tests/test_query.c';old='if (q->has_newer && mtime < q->newer) {';new='if (q->has_newer && mtime <= q->newer) {'},
 @{name='query_older_is_inclusive';file='src/search/wf_query.c';test='tests/test_query.c';old='if (q->has_older && mtime >= q->older) {';new='if (q->has_older && mtime > q->older) {'},
 @{name='query_a_folder_matches_a_size';file='src/search/wf_query.c';test='tests/test_query.c';old='if (dir && (q->n_ext || q->has_gt || q->has_lt)) {';new='if (dir && (q->n_ext)) {'},
 @{name='query_a_dotfile_has_an_extension';file='src/search/wf_query.c';test='tests/test_query.c';old='if (!dot || dot == folded) {';new='if (!dot) {'},
 @{name='query_kilo_is_a_mega';file='src/search/wf_query.c';test='tests/test_query.c';old='case L''k'': shift = 10; break;';new='case L''k'': shift = 20; break;'},
 @{name='query_size_overflow_accepted';file='src/search/wf_query.c';test='tests/test_query.c';old='if (shift && v > (UINT64_MAX >> shift)) {';new='if (shift && v > (UINT64_MAX >> shift) && 0) {'},
 @{name='query_only_the_first_extension_counts';file='src/search/wf_query.c';test='tests/test_query.c';old='any = 0 == wcscmp(dot + 1, q->ext[i]);';new='any = 0 == wcscmp(dot + 1, q->ext[0]);'},
 @{name='retry_notes_lose_their_order';file='src/ui/wf_app_retry.c';test='tests/test_retry.c';old='(app->retry_src_n - i) * sizeof app->retry_src[0]);';new='0);'},
 @{name='search_count_cache_survives_edits';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='if (!b->found.valid || b->found.gen != b->ed.text.gen ||';new='if (!b->found.valid ||'},
 @{name='md_bare_url_close_count_not_updated';file='src/text/wf_mdinline.c';test='tests/test_markdown.c';old='close--;';new='(void)close;'},
 @{name='md_image_nesting_one_too_deep';file='src/text/wf_mdinline.c';test='tests/test_markdown.c';old='if (image && images >= IMAGE_NEST_MAX) {';new='if (image && images >= IMAGE_NEST_MAX + 1u) {'},
  @{name='treewalk_byte_budget_ignored';file='src/search/wf_treewalk.c';test='tests/test_treewalk.c';old='q.bytes + (len + 1) * sizeof *child > budget';new='(0 && q.bytes + (len + 1) * sizeof *child > budget)'},
  @{name='draft_rewritten_for_the_same_text';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='!(b->has_draft && b->draft_gen == b->ed.text.gen)';new='!(0 && b->has_draft && b->draft_gen == b->ed.text.gen)'},
  @{name='draft_wait_for_a_text_the_draft_holds';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='if (!draft_wanted(b)) { continue; } due = b->draft_ms';new='if (0) { continue; } due = b->draft_ms'},
  @{name='draft_failed_afterwards_not_told';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='s->io.draft_failed(s->io.ctx, b->path)';new='0'},
  @{name='clip_crlf_taken_for_another_text';file='src/edit/wf_esession.c';test='tests/test_esession.c';old='if (!same_clip_text(b->ed.clip, b->ed.clip_n, os, os_n)) {';new='if (!same_clip_text(b->ed.clip, b->ed.clip_n, os, os_n) || 1) {'},
  @{name='draftw_remove_does_not_wait_for_a_write_under_way';file='src/edit/wf_edraftw_win.c';test='tests/test_draftwriter.c';old='if (w->busy && same_path(w->busy, path)) {';new='if (0) {'},
  @{name='draftw_paths_differing_by_case_are_one';file='src/edit/wf_edraftw_win.c';test='tests/test_draftwriter.c';old='return 0 == wcscmp(a, b);';new='return 0 == _wcsicmp(a, b);'},
  @{name='status_counts_only_with_a_filter';file='src/ui/wf_app_render.c';test='tests/test_app.c';old='bool fit = status_append_if_fit(s, &part, max_cells);';new='bool fit = app->cur.filter[0] && status_append_if_fit(s, &part, max_cells);'},
  @{name='draftw_every_text_is_written';file='src/edit/wf_edraftw_win.c';test='tests/test_draftwriter.c';old='if (i < w->nq) {';new='if (0) {'},
  @{name='draftw_failure_not_noted';file='src/edit/wf_edraftw_win.c';test='tests/test_draftwriter.c';old='note_failure(w, &job.path);';new='if (0) note_failure(w, &job.path);'},
  @{name='draftw_remove_keeps_queued_text';file='src/edit/wf_edraftw_win.c';test='tests/test_draftwriter.c';old='if (drop) {';new='if (drop && 0) {'},
  @{name='draftw_submit_waits_for_disk';file='src/edit/wf_edraftw_win.c';test='tests/test_draftwriter.c';old='WakeConditionVariable(&w->core.wake); return WF_OK;';new='WakeConditionVariable(&w->core.wake); wf_draftw_settle(w, path, false); return WF_OK;'},
  @{name='settings_directories_first_not_reloaded';file='src/ui/wf_app_settings_save.c';test='tests/test_theme_ui.c';old='|| before->directories_first != app->config->directories_first';new=''},
  @{name='tabs_strip_starts_at_the_first_tab';file='src/ui/wf_app_tabs.c';test='tests/test_tabs.c';old='while (first < app->tabs.cur) {';new='while (0) {'},
  @{name='clip_parent_in_another_case_is_the_same';file='src/ui/wf_app_clip.c';test='tests/test_clip_ui.c';old='f->path[0], (int)parent, FALSE)) {';new='f->path[0], (int)parent, TRUE)) {'},
  @{name='gitstatus_records_not_bounded';file='src/git/wf_gitstatus.c';test='tests/test_git.c';old='if (records > WF_GITSTATUS_MAX_ENTRIES) {';new='if (0) {'},
  @{name='filter_mark_not_told';file='src/ui/wf_app_render.c';test='tests/test_app.c';old='app->cur.filter[0] && strcmp(app->mode->name, "FILTER")';new='app->cur.filter[0] && strcmp(app->mode->name, "FILTER") && app->cur.nvis > 100000000'},
  @{name='input_tail_never_cut';file='src/ui/wf_app_render.c';test='tests/test_app.c';old='if (cells + 1 <= width) {';new='if (1) {'},
  @{name='help_closes_to_the_list';file='src/ui/wf_app_help.c';test='tests/test_layout.c';old='app->mode = app->help.back ? app->help.back : &WF_MODE_NORMAL;';new='app->mode = &WF_MODE_NORMAL;'},
  @{name='empty_list_says_nothing';file='src/ui/wf_app_render.c';test='tests/test_app.c';old='if (!app->cur.dir || app->cur.nvis || navigating(app) || w < 1) {';new='if (1) {'},
  @{name='copy_status_name_not_cut';file='src/ui/wf_app_copy.c';test='tests/test_copy_ui.c';old='(size_t)(left - 1));';new='(size_t)1000);'},
  @{name='status_counts_ignore_the_selection_gen';file='src/ui/wf_app_render.c';test='tests/test_app.c';old='return list_counts_current(app) && app->counts.sel == app->sel.gen;';new='return list_counts_current(app);'},
  @{name='size_poll_walks_all_slots_when_idle';file='src/fs/wf_sizeworker.c';test='tests/test_sizeworker_win.c';old='w->pending && k < w->n';new='k < w->n'},
  @{name='batch_row_temp_not_shown';file='src/ui/wf_app_batch.c';test='tests/test_batch_ui.c';old='return r->temp ? wf_msg(MSG_BATCH_ROW_TEMP) : "";';new='return "";'},
  @{name='hits_whole_group_allowed_by_one';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='if (chosen_here >= b - a) {';new='if (chosen_here > b - a) {'},
  @{name='preflight_same_name_twice_not_found';file='src/xfer/wf_preflight_paths.c';test='tests/test_xfer.c';old='if (i == twin) {';new='if (0) {'},
  @{name='batch_step_always_from_the_original_place';file='src/xfer/wf_batchexec_win.c';test='tests/test_batchexec.c';old='r->from[k] = at[item];';new='r->from[k] = r->item[item].path;'},
 @{name='retry_component_taken_whole';file='src/ui/wf_app_retry.c';test='tests/test_retry.c';old='c.n = wcscspn(rest, L"\\");';new='c.n = wcslen(rest);'},
  @{name='cand_worker_sends_files_for_folders_only';file='src/fs/wf_candworker.c';test='tests/test_candworker_win.c';old='if (s->dirs_only && !(fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {';new='if (0 && !(fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {'},
  @{name='whole_word_replace_all_ignores_the_replacement';file='src/edit/wf_editor.c';test='tests/test_editor.c';old='return (unsigned char)repl[n - 1];';new='(void)repl; return -1;'},
 @{name='search_table_restarts_after_a_refused_match';file='src/edit/wf_esearch.c';test='tests/test_editor.c';old='return true; } k = f[k - 1]; } } return false;';new='return true; } k = 0; } } return false;'},
 @{name='search_table_restarts_after_a_mismatch';file='src/edit/wf_esearch.c';test='tests/test_editor.c';old='pat[k], s)) { k = f[k - 1]; } if (same((unsigned char)l->s[i]';new='pat[k], s)) { k = 0; } if (same((unsigned char)l->s[i]'},
 @{name='counts_toggle_sign_ignored';file='src/ui/wf_app_render.c';test='tests/test_app.c';old='was_selected ? (size_t)-1 : 1;';new='1;'},
 @{name='counts_hidden_recounted_by_a_selection';file='src/ui/wf_app_render.c';test='tests/test_app.c';old='if (!list_counts_current(app)) {';new='if (1) {'},
 @{name='dupes_last_copy_looks_past_the_group_below';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='down = down && i + d < h->n && h->hit[i + d].group == h->hit[i].group;';new='down = down && i + d < h->n;'},
 @{name='dupes_last_copy_looks_past_the_group_above';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='up = up && i >= d && h->hit[i - d].group == h->hit[i].group;';new='up = up && i >= d;'},
 @{name='md_dead_openers_kept_after_a_pop';file='src/text/wf_mdinline.c';test='tests/test_markdown.c';old='if (p->nb_dead > p->nb) {';new='if (0) {'},
 @{name='md_link_leaves_the_openers_below_active';file='src/text/wf_mdinline.c';test='tests/test_markdown.c';old='for (size_t k = p->nb_dead; k < p->nb; k++) {';new='for (size_t k = p->nb; k < p->nb; k++) {'},
 @{name='table_delimiter_counts_inside_quotes';file='src/preview/wf_table.c';test='tests/test_table.c';old='quoted = !quoted;';new='(void)0;'},
 @{name='wslops_inside_ignores_linux_case';file='src/fs/wf_wslops_win.c';test='tests/test_wslops.c';old='0 == wcsncmp(dst + windows_part, src + windows_part,';new='0 == _wcsnicmp(dst + windows_part, src + windows_part,'},
 @{name='blocking_runs_inline_without_a_thread';file='src/async/wf_blocking.c';test='tests/test_blocking.c';old='return WF_E_NOMEM; /* not started: on this thread it would hold the screen */';new='return fn(arg);'},
 @{name='lock_snapshot_read_for_every_holder';file='src/process/wf_process_control_win.c';test='tests/test_lock.c';old='if (s->read) {';new='s->n = 0; s->ok = false; if (0) {'},
 @{name='backups_row_trusts_the_old_place';file='src/ui/wf_app_backups.c';test='tests/test_copy_ui.c';old='if (d && at < d->n && !wcscmp(d->e[at].name, name)) {';new='if (d && at < d->n) {'},
 @{name='clipboard_block_size_not_limited';file='src/clipboard/wf_clipworker_win.c';test='tests/test_clipboard.c';old='if (size > limit) {';new='if (size > limit && 0) {'},
 @{name='joblog_piece_cut_inside_a_character';file='src/ui/wf_app_joblog.c';test='tests/test_copy_ui.c';old='while (take > 0 && 0x80 == ((unsigned char)s[at + take] & 0xC0)) {';new='while (0 && take > 0 && 0x80 == ((unsigned char)s[at + take] & 0xC0)) {'},
 @{name='joblog_counts_kept_after_a_new_job';file='src/ui/wf_app_joblog.c';test='tests/test_copy_ui.c';old='app->jobs.log[app->jobs.n++] = *e; 	app->jobs.lines_ok = false;';new='app->jobs.log[app->jobs.n++] = *e;'},
 @{name='slot_header_accepts_a_sign';file='src/base/wf_slotfile.c';test='tests/test_slotfile.c';old='if (*at < ''0'' || *at > ''9'') {';new='if (0) {'},
 @{name='slot_header_accepts_overflow';file='src/base/wf_slotfile.c';test='tests/test_slotfile.c';old='ERANGE == errno || ULLONG_MAX == v ||';new='0 ||'},
 @{name='slot_header_accepts_a_tail';file='src/base/wf_slotfile.c';test='tests/test_slotfile.c';old='(*end && ''\n'' != *end)';new='0'},
 @{name='slot_seq_wraps_to_zero';file='src/base/wf_slotfile.c';test='tests/test_slotfile.c';old='if (seq >= ULLONG_MAX - 1) {';new='if (0) {'},
 @{name='exline_sort_empty_word_ends_the_line';file='src/input/wf_exline.c';test='tests/test_exline.c';old='if (!found) { return true; }';new='if (!found || !dir[0]) { return true; }'},
 @{name='exline_sort_trailing_by_length';file='src/input/wf_exline.c';test='tests/test_exline.c';old='return found ? fail(out, WF_EXE_TRAILING) : true; } static bool set_args';new='return dir[0] ? fail(out, WF_EXE_TRAILING) : true; } static bool set_args'},
 @{name='exline_set_trailing_by_length';file='src/input/wf_exline.c';test='tests/test_exline.c';old='return found ? fail(out, WF_EXE_TRAILING) : true; } bool wf_ex_parse';new='return value[0] ? fail(out, WF_EXE_TRAILING) : true; } bool wf_ex_parse'},
 @{name='batch_listing_cut_short_is_trusted';file='src/xfer/wf_batchexec_win.c';test='tests/test_batchexec.c';old='if (WF_BATCH_READY == pre && ERROR_NO_MORE_FILES != GetLastError()) {';new='if (0) {'},
 @{name='preview_cp932_cut_character_not_dropped';file='src/preview/wf_previewworker.c';test='tests/test_preview_win.c';old='units <= 0 && truncated && cut < 3 && n > 1;';new='0 && units <= 0 && truncated && cut < 3 && n > 1;'},
 @{name='clip_names_compared_ignoring_case';file='src/ui/wf_app_clip.c';test='tests/test_clip_ui.c';old='names[k], -1, leaf, -1, FALSE);';new='names[k], -1, leaf, -1, TRUE);'},
 @{name='term_repeat_count_ignored';file='src/term/wf_term.c';test='tests/test_term_input.c';old='return rec->Event.KeyEvent.wRepeatCount;';new='return 1;'},
 @{name='term_repeat_rest_dropped';file='src/term/wf_term.c';test='tests/test_term_input.c';old='t->npend = n - i;';new='t->npend = 0;'},
 @{name='term_repeat_remainder_dropped';file='src/term/wf_term.c';test='tests/test_term_input.c';old='t->rep_left = times_of(&rec[i], &e);';new='t->rep_left = 0; (void)times_of(&rec[i], &e);'},
 @{name='dechost_time_left_wraps';file='src/preview/wf_dechost_client_win.c';test='tests/test_dechost_win.c';old='ULONGLONG left = deadline > now ? deadline - now : 0;';new='ULONGLONG left = deadline - now;'},
 @{name='dechost_time_left_capped_to_infinity';file='src/preview/wf_dechost_client_win.c';test='tests/test_dechost_win.c';old='return left < INFINITE ? (DWORD)left : INFINITE - 1;';new='return (DWORD)left;'},
 @{name='terminal_windows_terminal_never_tried';file='src/fs/wf_terminal_win.c';test='tests/test_terminal.c';old='started = start_wt(dir, &e);';new='started = (start_wt(dir, &e), START_SKIPPED);'},
 @{name='terminal_dir_not_checked';file='src/fs/wf_terminal_win.c';test='tests/test_terminal.c';old='if (!wf_terminal_dir_ok(dir)) {';new='if (0) {'},
 @{name='terminal_window_attached_to_this_console';file='src/fs/wf_terminal_win.c';test='tests/test_terminal.c';old='spawn(exe, command, dir, DETACHED_PROCESS, error)';new='spawn(exe, command, dir, 0, error)'},
 @{name='terminal_fallback_without_a_console';file='src/fs/wf_terminal_win.c';test='tests/test_terminal.c';old='spawn(exe, command, dir, CREATE_NEW_CONSOLE, error)';new='spawn(exe, command, dir, 0, error)'},
 @{name='copy_tmp_without_id_taken_for_made';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='identified = false; /* a test says the identity cannot be read */';new='identified = true;'},
 @{name='terminal_press_in_a_row_not_refreshed';file='src/ui/wf_app_open.c';test='tests/test_terminal_ui.c';old='app->terminal_ms = app->now_ms ? app->now_ms : 1; /* every press counts */';new='if (!repeat) { app->terminal_ms = app->now_ms ? app->now_ms : 1; }'},
 @{name='terminal_opens_where_the_cursor_is';file='src/ui/wf_app_open.c';test='tests/test_terminal_ui.c';old='req.directory = app->cwd;';new='req.directory = wf_view_current(&app->cur) ? wf_view_current(&app->cur)->name : app->cwd;'},
 @{name='terminal_search_reaches_the_current_directory';file='src/fs/wf_terminal_win.c';test='tests/test_terminal.c';old='!find_on_path(L"wt.exe", exe, MAX_PATH)) {';new='!(find_on_path(L"wt.exe", exe, MAX_PATH) || SearchPathW(NULL, L"wt.exe", NULL, MAX_PATH, exe, NULL))) {'},
 @{name='terminal_left_out_candidate_is_the_reason';file='src/fs/wf_terminal_win.c';test='tests/test_terminal.c';old='return START_SKIPPED;';new='*error = ERROR_BAD_PATHNAME; return START_FAILED;'},
 @{name='terminal_memory_failure_hidden';file='src/fs/wf_terminal_win.c';test='tests/test_terminal.c';old='*error = memory ? memory : first;';new='*error = first;'},
 @{name='merge_enters_a_folder_under_stop';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='WF_COLLIDE_STOP != r->spec->collide &&';new='1 &&'},
 @{name='merge_registers_nothing_for_undo';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='add_created(r, dst, &here, leaf_use, child_dir);';new='(void)r; (void)dst; (void)here; (void)leaf_use; (void)child_dir;'},
 @{name='merge_keep_both_never_renames';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='if (merge && WF_COLLIDE_RENAME == r->spec->collide) {';new='if (0 && merge && WF_COLLIDE_RENAME == r->spec->collide) {'},
 @{name='merge_new_folder_is_not_staged';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='if (r->stage || (depth > 0 && !merging(r)) ||';new='if (r->stage || depth > 0 ||'},
 @{name='merge_created_forgets_its_parent';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='grown[res->created_n].parent = *parent;';new='grown[res->created_n].parent.kind = WF_ID_NONE; (void)parent;'},
 @{name='undo_entry_uses_destination_identity_for_every_folder';file='src/ui/wf_app_undo.c';test='tests/test_copy_ui.c';old='u.folder_id = (first->parent.kind == WF_ID_STRONG) ? first->parent : r->dest_id;';new='u.folder_id = r->dest_id;'},
 @{name='undo_row_done_after_a_partial_undo';file='src/ui/wf_app_undo.c';test='tests/test_copy_ui.c';old='if (k != i && undo_row_key(&app->undo[k]) == row) {';new='if (0) {'},
 @{name='undo_row_ignores_a_pushed_out_entry';file='src/ui/wf_app_undo.c';test='tests/test_copy_ui.c';old='bool lost = app->undo[i].group_lost || !undone;';new='bool lost = false;'},
 @{name='clip_idle_ignores_an_unread_answer';file='src/clipboard/wf_clipworker_win.c';test='tests/test_worker_lifecycle.c';old='idle = !w->core.busy && !w->has_req && !w->has_result && !w->owned;';new='idle = !w->core.busy && !w->has_req && !w->owned;'},
 @{name='clip_idle_ignores_what_it_put_on_the_clipboard';file='src/clipboard/wf_clipworker_win.c';test='tests/test_clipboard.c';old='idle = !w->core.busy && !w->has_req && !w->has_result && !w->owned;';new='idle = !w->core.busy && !w->has_req && !w->has_result;'},
 @{name='launch_idle_ignores_an_unread_answer';file='src/fs/wf_launchworker.c';test='tests/test_worker_lifecycle.c';old='idle = !w->core.busy && !w->job.present && !w->has_result;';new='idle = !w->core.busy && !w->job.present;'},
 @{name='image_idle_ignores_an_unread_answer';file='src/preview/wf_imageworker.c';test='tests/test_worker_lifecycle.c';old='idle = !w->core.busy && !w->has_fg && !w->has_pf && !(w->has_result && w->result_serial == w->serial);';new='idle = !w->core.busy && !w->has_fg && !w->has_pf;'},
 @{name='image_idle_ignores_a_waiting_request';file='src/preview/wf_imageworker.c';test='tests/test_worker_lifecycle.c';old='idle = !w->core.busy && !w->has_fg && !w->has_pf && !(w->has_result && w->result_serial == w->serial);';new='idle = !w->core.busy && !w->has_pf && !(w->has_result && w->result_serial == w->serial);'},
 @{name='archive_idle_wait_says_something_for_an_empty_cache';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='if (!kept) { return UINT64_MAX; }';new='if (!kept && 0) { return UINT64_MAX; }'},
 @{name='archive_idle_wait_ignores_a_build_in_progress';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='return gone >= idle_ms ? (building ? idle_ms : 0) : idle_ms - gone;';new='(void)building; return gone >= idle_ms ? 0 : idle_ms - gone;'},
 @{name='archive_touch_is_not_use';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='g_cache.used_ms = now_ms;';new='(void)now_ms;'},
 @{name='archive_idle_wait_wraps_when_the_clock_is_behind';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='gone = now_ms > g_cache.used_ms ? now_ms - g_cache.used_ms : 0;';new='gone = now_ms - g_cache.used_ms;'},
 @{name='archive_hit_is_not_use';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='hit->used = ++g_cache.tick; g_cache.used_ms = GetTickCount64();';new='hit->used = ++g_cache.tick;'},
 @{name='archive_build_is_not_use';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='mine->building = false; g_cache.used_ms = GetTickCount64();';new='mine->building = false;'},
 @{name='archive_in_view_ignores_this_tab';file='src/ui/wf_app_tabs.c';test='tests/test_tabs.c';old='if (wf_vpath_is(app->cwd)) { return true; }';new='if (0 && wf_vpath_is(app->cwd)) { return true; }'},
 @{name='archive_in_view_reads_the_stale_record_of_this_tab';file='src/ui/wf_app_tabs.c';test='tests/test_tabs.c';old='if (i != app->tabs.cur && app->tabs.t[i].path &&';new='if (app->tabs.t[i].path &&'},
 @{name='gitenv_a_bad_distribution_name_is_taken';file='src/git/wf_gitenv.c';test='tests/test_git.c';old='if (WF_OK != wf_locid_wsl(&id, name)) {';new='if (WF_OK != wf_locid_wsl(&id, name) && 0) {'},
 @{name='gitenv_linux_path_is_not_checked';file='src/git/wf_gitenv.c';test='tests/test_git.c';old='return wf_posix_path_ok((const uint8_t *)out, (size_t)bytes) ? WF_OK : WF_E_INVAL;';new='return WF_OK;'},
 @{name='gitenv_a_lone_surrogate_is_taken';file='src/git/wf_gitenv.c';test='tests/test_git.c';old='bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, tmp, (int)len,';new='bytes = WideCharToMultiByte(CP_UTF8, 0, tmp, (int)len,'},
 @{name='gitstatus_untracked_is_shown_as_modified';file='src/git/wf_gitstatus.c';test='tests/test_git.c';old='return ''U'';';new='return ''M'';'},
 @{name='gitstatus_search_finds_a_prefix';file='src/git/wf_gitstatus.c';test='tests/test_git.c';old='c = e->path_n < n ? -1 : e->path_n > n;';new='c = 0;'},
 @{name='gitstatus_rename_forgets_the_old_path';file='src/git/wf_gitstatus.c';test='tests/test_git.c';old='e.orig = p;';new='e.orig = NULL;'},
 @{name='gitstatus_unknown_letters_are_accepted';file='src/git/wf_gitstatus.c';test='tests/test_git.c';old='!letter_ok(rec[2]) || !letter_ok(rec[3])) {';new='(!letter_ok(rec[2]) && 0)) {'},
 @{name='gitstatus_conflict_is_not_noted';file='src/git/wf_gitstatus.c';test='tests/test_git.c';old='e.conflict = ''u'' == rec[0];';new='e.conflict = false;'},
 @{name='linediff_cr_is_part_of_the_line';file='src/text/wf_linediff.c';test='tests/test_git.c';old='return len && ''\r'' == s[len - 1] ? len - 1 : len;';new='(void)s; return len;'},
 @{name='linediff_removed_count_taken_from_the_new_side';file='src/text/wf_linediff.c';test='tests/test_git.c';old='h.an = i - h.a;';new='h.an = j - h.b;'},
 @{name='git_wsl_falls_back_to_windows_git';file='src/git/wf_gitbackend_win.c';test='tests/test_git.c';old='return distro_ok(env->distro) ? wsl_capability(env->distro, cancelled, cancel_ctx) : WF_GIT_UNAVAILABLE;';new='return wsl_capability(env->distro, cancelled, cancel_ctx) == WF_GIT_AVAILABLE || windows_exe(exe, ARRAYSIZE(exe)) ? WF_GIT_AVAILABLE : WF_GIT_UNAVAILABLE;'},
 @{name='git_windows_falls_back_to_wsl_git';file='src/git/wf_gitbackend_win.c';test='tests/test_git.c';old='return windows_exe(exe, ARRAYSIZE(exe)) ? WF_GIT_AVAILABLE : WF_GIT_UNAVAILABLE;';new='return windows_exe(exe, ARRAYSIZE(exe)) ? WF_GIT_AVAILABLE : wsl_capability("Ubuntu", cancelled, cancel_ctx);'},
 @{name='git_missing_wsl_git_is_remembered_for_ever';file='src/git/wf_gitbackend_win.c';test='tests/test_git.c';old='g.wsl[slot].slot.until = WF_GIT_AVAILABLE == cap ? 0 : now + WF_GIT_RETRY_MS;';new='g.wsl[slot].slot.until = UINT64_MAX;'},
 @{name='git_available_wsl_git_is_asked_again';file='src/git/wf_gitbackend_win.c';test='tests/test_git.c';old='if (WF_GIT_AVAILABLE == s->cap || now < s->until) {';new='if (now < s->until) {'},
 @{name='git_read_only_options_are_dropped';file='src/git/wf_gitbackend_win.c';test='tests/test_git.c';old='L"-c", L"core.fsmonitor=false", NULL};';new='NULL};'},
 @{name='git_the_prompt_is_not_switched_off';file='src/git/wf_gitbackend_win.c';test='tests/test_git.c';old='L"GIT_TERMINAL_PROMPT=0", L"GIT_OPTIONAL_LOCKS=0", NULL};';new='L"GIT_OPTIONAL_LOCKS=0", NULL};'},
 @{name='gitproc_keeps_git_dir';file='src/git/wf_gitproc_win.c';test='tests/test_git.c';old='static const wchar_t *const drop_env[] = {L"GIT_DIR", L"GIT_WORK_TREE",';new='static const wchar_t *const drop_env[] = {L"GIT_DIRX", L"GIT_WORK_TREE",'},
 @{name='gitproc_extra_environment_does_not_win';file='src/git/wf_gitproc_win.c';test='tests/test_git.c';old='skip = n && CSTR_EQUAL == CompareStringOrdinal(p, (int)n,';new='skip = ((void)n, 0) && n && CSTR_EQUAL == CompareStringOrdinal(p, (int)n,'},
 @{name='gitproc_output_is_not_limited';file='src/git/wf_gitproc_win.c';test='tests/test_git.c';old='run->max_out ? run->max_out : (size_t)-1 - 1, true,';new='(size_t)-1 - 1, true,'},
 @{name='gitproc_quotes_every_argument';file='src/git/wf_gitproc_win.c';test='tests/test_git.c';old='if (needs_quotes(args[i])';new='if (1 || needs_quotes(args[i])'},
 @{name='gitworker_stale_answer_is_published';file='src/git/wf_gitworker.c';test='tests/test_git_worker.c';old='if (!w->core.stop && (LONG)a->gen == w->newest[j->kind]) {';new='if (!w->core.stop) {'},
 @{name='gitworker_poll_delivers_an_unwanted_answer';file='src/git/wf_gitworker.c';test='tests/test_git_worker.c';old='if ((LONG)w->ans[k].gen == w->newest[k]) { /* still the wanted one */';new='if (1) { /* still the wanted one */'},
 @{name='gitworker_a_newer_request_does_not_end_the_process';file='src/git/wf_gitworker.c';test='tests/test_git_worker.c';old='return j->w->core.stop || (LONG)j->gen != j->w->newest[j->kind];';new='return j->w->core.stop;'},
 @{name='gitworker_diff_waits_behind_a_base';file='src/git/wf_gitworker.c';test='tests/test_git_worker.c';old='if (w->diff.has) { kind = K_DIFF;';new='if (w->diff.has && !w->base.has) { kind = K_DIFF;'},
 @{name='gitworker_a_new_file_runs_git';file='src/git/wf_gitworker.c';test='tests/test_git_worker.c';old='if (rq->expect_new && !rq->orig) {';new='if (0 && rq->expect_new && !rq->orig) {'},
 @{name='gitworker_a_rename_reads_the_new_path';file='src/git/wf_gitworker.c';test='tests/test_git_worker.c';old='path = rq->orig ? rq->orig : rel;';new='path = rel;'},
 @{name='gitworker_a_non_repository_reads_its_status';file='src/git/wf_gitworker.c';test='tests/test_git_worker.c';old='if (0 != r.exit_code || !r.out || 0 != strncmp((const char *)r.out, "true", 4)) {';new='if (0 && (0 != r.exit_code || !r.out || 0 != strncmp((const char *)r.out, "true", 4))) {'},
 @{name='gitworker_idle_with_an_unread_answer';file='src/git/wf_gitworker.c';test='tests/test_git_worker.c';old='idle = idle && !w->has_ans[k];';new='idle = idle && k >= 0;'},
 @{name='gitui_a_stale_answer_is_shown';file='src/ui/wf_app_git.c';test='tests/test_git_ui.c';old='a->gen != app->git.gen || !a->asked ||';new='!a->asked ||'},
 @{name='gitui_a_folder_is_looked_up_without_its_slash';file='src/ui/wf_app_git.c';test='tests/test_git_ui.c';old='key[n++] = ''/''; /* Git reports an untracked folder as one record */';new='(void)n; /* Git reports an untracked folder as one record */'},
 @{name='gitui_a_reload_asks_without_a_gap';file='src/ui/wf_app_git.c';test='tests/test_git_ui.c';old='(reload ? age >= GIT_REFRESH_GAP_MS : age >= GIT_STALE_MS)';new='(reload ? 1 : age >= GIT_STALE_MS)'},
 @{name='gitui_a_held_back_reload_is_lost';file='src/ui/wf_app_git.c';test='tests/test_git_ui.c';old='app->git.due_ms = app->git.asked_ms + GIT_REFRESH_GAP_MS;';new='app->git.due_ms = 0;'},
 @{name='gitui_leaving_the_work_tree_keeps_the_table';file='src/ui/wf_app_git.c';test='tests/test_git_ui.c';old='drop(app); /* not a place with a work tree: nothing of Git is kept */';new='(void)0;'},
 @{name='gitui_a_refused_request_keeps_the_state';file='src/ui/wf_app_git.c';test='tests/test_git_ui.c';old='drop(app); /* no letters; the list is as it was */';new='(void)0;'},
 @{name='gitpane_a_stale_diff_is_shown';file='src/edit/wf_egitpane.c';test='tests/test_git_edit.c';old='if (a->gen != p->diff_gen || WF_GP_READY != p->state) {';new='if (WF_GP_READY != p->state) {'},
 @{name='gitpane_shown_on_a_narrow_terminal';file='src/edit/wf_egitpane.c';test='tests/test_git_edit.c';old='s->cols < WF_GP_MIN_COLS ||';new='0 ||'},
 @{name='gitpane_no_debounce';file='src/edit/wf_egitpane.c';test='tests/test_git_edit.c';old='p->due_ms = s->now_ms + (p->has_diff || p->too_many ? WF_GP_DEBOUNCE_MS : 0);';new='p->due_ms = s->now_ms + 0;'},
 @{name='gitpane_the_wrong_base_is_taken';file='src/edit/wf_egitpane.c';test='tests/test_git_edit.c';old='if (a->gen != p->base_gen || WF_GP_ASKED != p->state) {';new='if (WF_GP_ASKED != p->state) {'},
 @{name='gitpane_another_buffer_is_not_asked_again';file='src/edit/wf_egitpane.c';test='tests/test_git_edit.c';old='if (s->git_last != b) {';new='if (0) {'},
 @{name='gitpane_text_with_an_extra_break';file='src/edit/wf_egitpane.c';test='tests/test_git_edit.c';old='if (i + 1 < t->n) { /* the empty last line is the final break */';new='if (1) { /* the empty last line is the final break */'},
 @{name='gitpane_close_keeps_the_requests';file='src/edit/wf_esession.c';test='tests/test_git_edit.c';old='s->git_port.cancel(s->git_port.ctx); /* what waits and runs is not wanted */';new='(void)0; /* what waits and runs is not wanted */'},
 @{name='gitpane_close_hunks_are_not_joined';file='src/edit/wf_egitpane.c';test='tests/test_git_edit.c';old='d->h[j + 1].b - (d->h[j].b + d->h[j].bn) <= 2 * context) {';new='d->h[j + 1].b - (d->h[j].b + d->h[j].bn) < 1) {'},
 @{name='gitui_the_generation_restarts_with_each_repository';file='src/ui/wf_app_git.c';test='tests/test_git_ui.c';old='app->git.gen = gen;';new='(void)gen;'},
 @{name='gitui_an_answer_for_another_repository_is_taken';file='src/ui/wf_app_git.c';test='tests/test_git_ui.c';old='!same_root(&app->git.env, a->asked, app->git.root)) {';new='0) {'},
 @{name='gitstatus_an_untracked_folder_does_not_cover_what_is_inside';file='src/git/wf_gitstatus.c';test='tests/test_git.c';old='e = dir && ''?'' == dir->work ? dir : NULL;';new='e = dir && 0 ? dir : NULL;'},
 @{name='gitworker_a_path_not_in_head_is_an_error';file='src/git/wf_gitworker.c';test='tests/test_git_worker.c';old='} else if (WF_OK == st && is_new_at_head(&rq->env, root, spec, &job)) {';new='} else if (0 && WF_OK == st && is_new_at_head(&rq->env, root, spec, &job)) {'},
 @{name='gitworker_a_folder_is_taken_as_the_root_without_looking';file='src/git/wf_gitworker.c';test='tests/test_git_worker.c';old='if (rq->mode != WF_GIT_REPO_PLAIN) {';new='if (0 && rq->mode != WF_GIT_REPO_PLAIN) {'},
 @{name='gitworker_no_dotgit_still_runs_git';file='src/git/wf_gitworker.c';test='tests/test_git_worker.c';old='a.state = WF_GITR_NOT_REPO; /* no .git above: no process, no wsl.exe probe */';new='(void)0;'},
 @{name='gitworker_the_answer_does_not_name_what_was_asked';file='src/git/wf_gitworker.c';test='tests/test_git_worker.c';old='a.asked = wf_path_dup(rq->root);';new='a.asked = wf_path_dup(L"");'},
 @{name='gitworker_a_base_is_read_in_the_folder_of_the_file';file='src/git/wf_gitworker.c';test='tests/test_git_worker.c';old='root = root_buf;';new='root = rq->path;'},
 @{name='gitui_a_found_repository_is_not_adopted';file='src/ui/wf_app_git.c';test='tests/test_git_ui.c';old='app->git.root = root; app->git.prefix = prefix; } was_resolve = !app->git.resolved;';new='free(root); free(prefix); app->git.root = NULL; app->git.prefix = NULL; } was_resolve = !app->git.resolved;'},
 @{name='imgcheck_png_header_values_are_not_checked';file='src/preview/wf_image_check.c';test='tests/test_image_hostile.c';old='if (first && !png_ihdr_ok(data + at + 8)) {';new='if (0 && first && !png_ihdr_ok(data + at + 8)) {'},
 @{name='imgcheck_png_compression_and_filter_methods_ignored';file='src/preview/wf_image_check.c';test='tests/test_image_hostile.c';old='return depth_ok && 0 == h[10] && 0 == h[11] && h[12] <= 1;';new='return depth_ok && h[12] <= 1;'},
 @{name='imgcheck_png_crc_table_is_wrong';file='src/preview/wf_image_check.c';test='tests/test_image_hostile.c';old='c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;';new='c = c & 1 ? (c >> 1) ^ 0xEDB88321u : c >> 1;'},
 @{name='imgdecode_pixel_limit_is_gone';file='src/preview/wf_decode_win.c';test='tests/test_image_hostile.c';old='if ((unsigned long long)w * h > WF_DECODE_SOURCE_PIXELS) {';new='if (0) {'},
 @{name='imgdecode_a_damaged_bitmap_is_called_unsupported';file='src/preview/wf_decode_win.c';test='tests/test_decode_win.c';old='if (WINCODEC_ERR_UNKNOWNIMAGEFORMAT == hr && has_picture_signature(data, n)) {';new='if ((void)data, (void)n, 0 && WINCODEC_ERR_UNKNOWNIMAGEFORMAT == hr && has_picture_signature(data, n)) {'},
 @{name='imgdecode_any_open_failure_on_a_picture_is_damage';file='src/preview/wf_decode_win.c';test='tests/test_decode_win.c';old='return status_of_wic(hr); } /* `checked` is 0';new='return has_picture_signature(data, n) ? WF_DSTAT_CORRUPT : status_of_wic(hr); } /* `checked` is 0'},
 @{name='gitui_unverified_letters_are_shown';file='src/ui/wf_app_git.c';test='tests/test_git_ui.c';old='0 == app->git.table.n || !git_verified(app)) {';new='0 == app->git.table.n) {'},
 @{name='gitui_a_nested_repository_is_not_looked_for';file='src/ui/wf_app_git.c';test='tests/test_git_ui.c';old='locate(app); /* a nearer .git may be below the root */';new='(void)0;'},
 @{name='gitui_a_reload_during_the_wait_is_lost';file='src/ui/wf_app_git.c';test='tests/test_git_ui.c';old='app->git.stale = app->git.stale || reload;';new='app->git.stale = false;'},
 @{name='gitui_a_nested_repository_is_not_asked_about';file='src/ui/wf_app_git.c';test='tests/test_git_ui.c';old='ask(app); /* the nested repository''s own status */';new='(void)0;'},
 @{name='gitworker_locating_runs_git';file='src/git/wf_gitworker.c';test='tests/test_git_worker.c';old='if (WF_GIT_REPO_LOCATE == rq->mode) {';new='if (0 && WF_GIT_REPO_LOCATE == rq->mode) {'},
 @{name='gitui_a_status_overtakes_the_folder_check';file='src/ui/wf_app_git.c';test='tests/test_git_ui.c';old='if (app->git.lcwd) {';new='if (0 && app->git.lcwd) {'},
 @{name='gitui_a_status_due_during_the_check_is_forgotten';file='src/ui/wf_app_git.c';test='tests/test_git_ui.c';old='if (!app->git.answered || app->git.stale) {';new='if (!app->git.answered) {'},
 @{name='gitui_the_editor_uses_an_unchecked_parent_root';file='src/ui/wf_app_git.c';test='tests/test_git_ui.c';old='!wf_app_git_file( app, app->git.root, rel, &letter, orig, orig_cap)) {';new='(wf_app_git_file(app, app->git.root, rel, &letter, orig, orig_cap), 0)) {'},
 @{name='gitworker_any_failure_is_taken_for_a_new_file';file='src/git/wf_gitworker.c';test='tests/test_git_worker.c';old='1 == r.exit_code &&';new='0 != r.exit_code &&'},
 @{name='gitpane_a_replaced_base_is_never_asked_again';file='src/edit/wf_egitpane.c';test='tests/test_git_edit.c';old='s->git_base_owner != b->git.id) {';new='0) {'},
 @{name='gitpane_the_base_is_asked_at_every_tick';file='src/edit/wf_egitpane.c';test='tests/test_git_edit.c';old='s->git_base_owner = p->id;';new='(void)0;'},
 @{name='linediff_all_new_or_all_gone_is_not_bounded';file='src/text/wf_linediff.c';test='tests/test_git.c';old='if (edits > WF_LD_EDITS_MAX) {';new='if (0 && edits > WF_LD_EDITS_MAX) {'},
 @{name='hash_idle_ignores_an_unread_answer';file='src/xfer/wf_hashworker.c';test='tests/test_worker_lifecycle.c';old='idle = !w->core.busy && !w->has_req && !w->has_upd;';new='idle = !w->core.busy && !w->has_req;'},
 @{name='size_idle_ignores_an_unread_answer';file='src/fs/wf_sizeworker.c';test='tests/test_worker_lifecycle.c';old='idle = !w->core.busy && !w->has_req && !w->pending;';new='idle = !w->core.busy && !w->has_req;'},
 @{name='lock_idle_ignores_an_unread_answer';file='src/process/wf_lockworker_win.c';test='tests/test_worker_lifecycle.c';old='idle = !w->core.busy && !w->has_req && !w->has_result;';new='idle = !w->core.busy && !w->has_req;'},
 @{name='search_idle_ignores_an_unread_batch';file='src/search/wf_searchworker.c';test='tests/test_worker_lifecycle.c';old='idle = !w->core.busy && !w->has_req && !w->qn;';new='idle = !w->core.busy && !w->has_req;'},
 @{name='worker_start_failure_keeps_the_event';file='src/async/wf_workcore.c';test='tests/test_worker_lifecycle.c';old='CloseHandle(c->ready); c->ready = NULL; return WF_E_OS;';new='c->ready = NULL; return WF_E_OS;'},
 @{name='extract_publishes_from_an_unclean_staging';file='src/xfer/wf_extract_win.c';test='tests/test_extract_win.c';old='if (x.unclean) {';new='if (0 && x.unclean) {'},
 @{name='extract_destination_identity_not_compared';file='src/xfer/wf_extract_win.c';test='tests/test_extract_win.c';old='memcmp(want->id, id.FileId.Identifier, sizeof want->id)) {';new='(0 && memcmp(want->id, id.FileId.Identifier, sizeof want->id))) {'},
 @{name='extract_staged_folder_not_held';file='src/xfer/wf_extract_win.c';test='tests/test_extract_win.c';old='h = CreateFileW(p, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,';new='h = CreateFileW(p, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,'},
 @{name='extract_sorted_lookup_finds_nothing';file='src/ui/wf_app_extract.c';test='tests/test_copy_ui.c';old='if (bsearch(&leaf, sorted, dir->n, sizeof *sorted, name_cmp)) {';new='if (0 && bsearch(&leaf, sorted, dir->n, sizeof *sorted, name_cmp)) {'},
 @{name='merge_enters_a_junction';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='!(a & FILE_ATTRIBUTE_REPARSE_POINT)';new='(a | 0)'},
 @{name='merge_enters_its_own_source_folder';file='src/xfer/wf_copyengine_win.c';test='tests/test_copyengine_win.c';old='same_id(&there.id, &ss.id)) {';new='(same_id(&there.id, &ss.id) && 0)) {'},
 @{name='extract_leftover_staging_not_named';file='src/xfer/wf_extract_win.c';test='tests/test_extract_win.c';old='(void)issue_add(x, fd.cFileName, WF_CF_STAGING_LEFT, 0);';new='(void)x;'},
 @{name='extract_full_disk_reported_as_write_failure';file='src/xfer/wf_extract_win.c';test='tests/test_extract_win.c';old='k = ERROR_DISK_FULL == error ? WF_CF_NO_SPACE';new='k = 0 ? WF_CF_NO_SPACE'},
 @{name='extract_absolute_name_taken';file='src/archive/wf_archive.c';test='tests/test_archive.c';old="L'/' == p[0] || L'\\' == p[0]) {";new='0) {'},
 @{name='extract_reserved_device_taken';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='reserved_device(p + from, i - from)) {';new='(0 && reserved_device(p + from, i - from))) {'},
 @{name='extract_trailing_dot_taken';file='src/archive/wf_archive.c';test='tests/test_archive.c';old="L'.' == p[i - 1] || L' ' == p[i - 1] ||";new='0 ||'},
 @{name='extract_bad_chars_taken';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='if (bad_char(p[k]) || 0xFFFD == p[k]) {';new='if (0) {'},
 @{name='extract_case_collision_missed';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='return same ? 1 : 0;';new='return same | 1;'},
 @{name='extract_total_not_refused_early';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='out->declared_bytes > lim.max_total_bytes) {';new='0) {'},
 @{name='extract_file_limit_ignored';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='if (!(nd->flags & N_NOSIZE) && nd->size > lim.max_file_bytes) {';new='if (0) {'},
 @{name='extract_checksum_taken_for_read_error';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='*kind = message_has(m, "CRC") ? WF_XI_CHECKSUM';new='*kind = 0 ? WF_XI_CHECKSUM'},
 @{name='extract_nested_item_taken_twice';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='if (at != node && chosen[at]) {';new='if ((void)chosen, 0 && at != node && chosen[at]) {'},
 @{name='extract_bytes_not_counted';file='src/archive/wf_archive.c';test='tests/test_archive.c';old='st->bytes += (uint64_t)k;';new='st->bytes += 0u * (uint64_t)k;'},
 @{name='extract_job_never_merges';file='src/xfer/wf_extract_win.c';test='tests/test_extract_win.c';old='es.merge_folders = true;';new='es.merge_folders = false;'},
 @{name='extract_thrown_away_file_stays_staged';file='src/xfer/wf_extract_win.c';test='tests/test_extract_win.c';old='if (WF_OWNED_DELETED == r || WF_OWNED_ABSENT == r) {';new='if (0 && (WF_OWNED_DELETED == r || WF_OWNED_ABSENT == r)) {'},
 @{name='extract_name_issue_reported_as_data';file='src/xfer/wf_extract_win.c';test='tests/test_extract_win.c';old='case WF_XI_NAME:      k = WF_CF_ARCHIVE_NAME; break;';new='case WF_XI_NAME:      k = WF_CF_ARCHIVE_DATA; break;'},
 @{name='bracket_forward_empty_lines_are_free';file='src/edit/wf_editor.c';test='tests/test_editor.c';old='e->text.n || ++scanned > BRACKET_SCAN_MAX) {';new='e->text.n) {'},
 @{name='bracket_backward_empty_lines_are_free';file='src/edit/wf_editor.c';test='tests/test_editor.c';old='0 == line || ++scanned > BRACKET_SCAN_MAX) {';new='0 == line) {'},
  @{name='slot_probe_never_whole';file='src/base/wf_slotfile.c';test='tests/test_slotfile.c';old='(unsigned long long)(marker - head) + 4 + want';new='(unsigned long long)(marker - head) + want'},
 @{name='treewalk_enters_a_link';file='src/search/wf_treewalk.c';test='tests/test_treewalk.c';old='descend = (e->flags & WF_ENT_DIR) && !(e->flags & WF_ENT_LINK) &&';new='descend = (e->flags & WF_ENT_DIR) &&'},
 @{name='treewalk_enters_a_cloud_folder';file='src/search/wf_treewalk.c';test='tests/test_treewalk.c';old='!(e->flags & WF_ENT_CLOUD);';new='1;'},
 @{name='treewalk_hidden_always_visited';file='src/search/wf_treewalk.c';test='tests/test_treewalk.c';old='if (!ops->hidden && (e->flags & WF_ENT_HIDDEN)) {';new='if (!ops->hidden && (e->flags & WF_ENT_HIDDEN) && 0) {'},
 @{name='treewalk_depth_is_always_zero';file='src/search/wf_treewalk.c';test='tests/test_treewalk.c';old='item.depth = cur.depth;';new='item.depth = 0;'},
 @{name='treewalk_every_folder_has_the_roots_identity';file='src/search/wf_treewalk.c';test='tests/test_treewalk.c';old='item.dir_id = cur.id;';new='item.dir_id = root_id;'},
 @{name='treewalk_unreadable_not_counted';file='src/search/wf_treewalk.c';test='tests/test_treewalk.c';old='stats->unreadable++;';new='(void)0;'},
 @{name='treewalk_visitor_stop_not_kept';file='src/search/wf_treewalk.c';test='tests/test_treewalk.c';old='stats->stopped = true;';new='(void)0;'},
 @{name='treewalk_pending_limit_ignored';file='src/search/wf_treewalk.c';test='tests/test_treewalk.c';old='queue_live(&q) >= WF_WALK_MAX_PENDING ||';new='queue_live(&q) >= WF_WALK_MAX_PENDING * 10 ||'},
 @{name='treewalk_last_folder_first';file='src/search/wf_treewalk.c';test='tests/test_treewalk.c';old='Folder cur = q.f[q.head++];';new='Folder cur = q.f[--q.n];'},
 @{name='searchworker_cancel_does_not_cancel';file='src/search/wf_searchworker.c';test='tests/test_searchworker_win.c';old='InterlockedExchange(&w->live_gen, 0);';new='(void)w;'},
 @{name='searchworker_newer_request_not_live';file='src/search/wf_searchworker.c';test='tests/test_searchworker_win.c';old='InterlockedExchange(&w->live_gen, (LONG)gen);';new='(void)gen;'},
 @{name='searchworker_limit_ignored';file='src/search/wf_searchworker.c';test='tests/test_searchworker_win.c';old='if (r->total >= WF_SEARCH_MAX_HITS) {';new='if (r->total >= WF_SEARCH_MAX_HITS * 2) {'},
 @{name='searchworker_hidden_always_searched';file='src/search/wf_searchworker.c';test='tests/test_searchworker_win.c';old='WfWalkOps ops = {list_cb, visit_cb, run_cancelled, hidden, 0};';new='WfWalkOps ops = {list_cb, visit_cb, run_cancelled, true, 0}; (void)hidden;'},
 @{name='searchworker_everything_is_a_hit';file='src/search/wf_searchworker.c';test='tests/test_searchworker_win.c';old='if (!wf_query_match(r->query, e->name, 0 != (e->flags & WF_ENT_DIR), e->size, e->mtime)) {';new='if (0 && !wf_query_match(r->query, e->name, 0 != (e->flags & WF_ENT_DIR), e->size, e->mtime)) {'},
 @{name='searchworker_dupes_collect_follows_links';file='src/search/wf_searchworker.c';test='tests/test_searchworker_win.c';old='(e->flags & (WF_ENT_DIR | WF_ENT_LINK | WF_ENT_CLOUD)) ||';new='(e->flags & (WF_ENT_DIR | WF_ENT_CLOUD)) ||'},
 @{name='searchworker_dupes_collect_reads_cloud_files';file='src/search/wf_searchworker.c';test='tests/test_searchworker_win.c';old='(e->flags & (WF_ENT_DIR | WF_ENT_LINK | WF_ENT_CLOUD)) ||';new='(e->flags & (WF_ENT_DIR | WF_ENT_LINK)) ||'},
 @{name='hits_stale_batch_taken';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='if (!h->open || !h->running || b->gen != h->gen || b->dupes != h->dupes) {';new='if (!h->open || !h->running || b->dupes != h->dupes) {'},
 @{name='hits_esc_closes_at_once';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='if (h->running) {';new='if (h->running && 0) {'},
 @{name='hits_copy_of_several_folders_taken';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='if (1 != plan.nrun) {';new='if (0 != plan.nrun) {'},
 @{name='hits_one_job_for_all_folders';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='return a->dir == b->dir || !wcscmp(a->dir->path, b->dir->path);';new='(void)a; (void)b; return false;'},
 @{name='hits_unproven_results_sent';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='if (!(x->id.kind == WF_ID_STRONG) || !(x->dir->id.kind == WF_ID_STRONG)) {';new='if ((!(x->id.kind == WF_ID_STRONG) || !(x->dir->id.kind == WF_ID_STRONG)) && 0) {'},
 @{name='hits_folder_limit_ignored';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='if (folders > WF_HITS_MAX_FOLDERS) {';new='if (folders > WF_HITS_MAX_FOLDERS * 100) {'},
 @{name='hits_enter_forgets_the_name';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='st = wf_app_navigate(app, dir, name, SIZE_MAX);';new='st = wf_app_navigate(app, dir, NULL, SIZE_MAX);'},
 @{name='hits_marks_not_counted';file='src/ui/wf_app_hits.c';test='tests/test_hits_ui.c';old='h->nsel += h->sel[i] ? 1u : 0u;';new='h->nsel += 0u;'},
 @{name='syntax_trust_hash_not_compared';file='src/syntax/wf_dllload_win.c';test='tests/test_syntax_loader.c';old='if (expect && memcmp(expect, hold->sha256, 32)) {';new='(void)expect; if (0 && expect && memcmp(expect, hold->sha256, 32)) {'},
 @{name='syntax_held_file_can_be_changed';file='src/syntax/wf_dllload_win.c';test='tests/test_syntax_loader.c';old='h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,';new='h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,'},
 @{name='syntax_junction_in_the_path_followed';file='src/syntax/wf_dllload_win.c';test='tests/test_syntax_loader.c';old='} else if (a & FILE_ATTRIBUTE_REPARSE_POINT) {';new='} else if (0 && (a & FILE_ATTRIBUTE_REPARSE_POINT)) {'},
 @{name='syntax_hidden_name_accepted';file='src/syntax/wf_dllload_win.c';test='tests/test_syntax_loader.c';old='if (!n || n > 64 || L''.'' == s[0]) {';new='if (!n || n > 64) {'},
 @{name='syntax_relative_path_loaded';file='src/syntax/wf_dllload_win.c';test='tests/test_syntax_loader.c';old='L'':'' != path[1]) {';new='(0 && L'':'' != path[1])) {'},
 @{name='syntax_file_size_limit_ignored';file='src/syntax/wf_dllload_win.c';test='tests/test_syntax_loader.c';old='(uint64_t)size.QuadPart > max_bytes) {';new='(uint64_t)size.QuadPart > (uint64_t)max_bytes * 1000u) {'},
 @{name='syntax_manifest_abi_not_compared';file='src/syntax/wf_syncore.c';test='tests/test_syntax_loader.c';old='g->abi > TREE_SITTER_LANGUAGE_VERSION || g->abi != man.abi) {';new='g->abi > TREE_SITTER_LANGUAGE_VERSION) {'},
 @{name='syntax_manifest_not_compared_with_the_table';file='src/syntax/wf_syncore.c';test='tests/test_syntax_loader.c';old='WF_MF_OK != wf_manifest_agrees(&man, lang)) {';new='0) {'},
 @{name='syntax_refusal_forgotten';file='src/syntax/wf_syncore.c';test='tests/test_syntax_loader.c';old='g->state = 2; /* a refusal is kept: it is not hashed again */';new='g->state = 0;'},
 @{name='syntax_not_installed_remembered';file='src/syntax/wf_syncore.c';test='tests/test_syntax_loader.c';old='} else if (WF_SYN_E_NOGRAMMAR != st) {';new='} else {'},
 @{name='syntax_grammar_loaded_twice';file='src/syntax/wf_syncore.c';test='tests/test_syntax_loader.c';old='if (1 == g->state) {';new='if (0 && 1 == g->state) {'},
 @{name='syntax_same_text_parsed_again';file='src/syntax/wf_syncore.c';test='tests/test_syntax_loader.c';old='if (pre == len && pre == s->prev_len) {';new='if (0 && pre == len && pre == s->prev_len) {'},
 @{name='syntax_text_limit_ignored';file='src/syntax/wf_syncore.c';test='tests/test_syntax_loader.c';old='if (len > s->lim.max_text || len > 0xfffffffeu ||';new='if (len > 0xfffffffeu ||'},
 @{name='syntax_line_limit_ignored';file='src/syntax/wf_syncore.c';test='tests/test_syntax_loader.c';old='!lines_ok(text, len, s->lim.max_line)) {';new='!lines_ok(text, len, s->lim.max_line * 1000u)) {'},
 @{name='syntax_pin_not_checked';file='src/syntax/wf_synhost_win.c';test='tests/test_syntax_loader.c';old='st = wf_dll_hold(p, MAX_DLL_BYTES, H.pin, &hold);';new='st = wf_dll_hold(p, MAX_DLL_BYTES, NULL, &hold);'},
 @{name='syntax_host_refusal_forgotten';file='src/syntax/wf_synhost_win.c';test='tests/test_syntax_loader.c';old='H.refused = true;';new='H.refused = false;'},
 @{name='syntax_host_open_keeps_a_stale_session';file='src/syntax/wf_synhost_win.c';test='tests/test_syntax_loader.c';old='*out = NULL;';new='(void)out;'},
 @{name='syntax_manifest_duplicate_key_accepted';file='src/syntax/wf_manifest.c';test='tests/test_syntax_core.c';old='if (seen & (1u << k)) {';new='if (0 && (seen & (1u << k))) {'},
 @{name='syntax_manifest_dotdot_in_a_name';file='src/syntax/wf_manifest.c';test='tests/test_syntax_core.c';old='if (''.'' == c && i + 1 < n && ''.'' == s[i + 1]) {';new='if (0 && ''.'' == c) {'},
 @{name='syntax_manifest_hidden_name';file='src/syntax/wf_manifest.c';test='tests/test_syntax_core.c';old='if (!n || ''.'' == s[0]) {';new='if (!n) {'},
 @{name='syntax_manifest_size_unbounded';file='src/syntax/wf_manifest.c';test='tests/test_syntax_core.c';old='if (!n || n > WF_MANIFEST_MAX_BYTES) {';new='if (!n) {'},
 @{name='syntax_manifest_extension_count_ignored';file='src/syntax/wf_manifest.c';test='tests/test_syntax_core.c';old='if (m->nexts != count_list(lang->exts)) {';new='if (0 && m->nexts != count_list(lang->exts)) {'},
 @{name='syntax_overlay_suffix_not_shifted';file='src/syntax/wf_synoverlay.c';test='tests/test_syntax_core.c';old='if (a < to && ov->suf) {';new='if (0 && a < to && ov->suf) {'},
 @{name='syntax_overlay_runs_not_checked';file='src/syntax/wf_synoverlay.c';test='tests/test_syntax_core.c';old='if (!runs_are_sane(runs, n, text_len)) {';new='(void)text_len; if (0 && !runs_are_sane(runs, n, text_len)) {'},
 @{name='syntax_dotenv_comment_after_a_value';file='src/syntax/wf_dotenv.c';test='tests/test_syntax_core.c';old='if (''#'' == t[e] && e > v && ('' '' == t[e - 1] || ''\t'' == t[e - 1])) {';new='if (0) {'},
 @{name='syntax_whole_file_name_does_not_win';file='src/syntax/wf_synlang.c';test='tests/test_syntax_core.c';old='if (in_list(langs[i].names, low, n)) {';new='if (0 && in_list(langs[i].names, low, n)) {'},
 @{name='syntax_theme_class_not_readable';file='src/render/wf_theme.c';test='tests/test_syntax_core.c';old='if (wf_color_contrast(m, bg) >= 4.5) {';new='if (wf_color_contrast(m, bg) >= 0.0) {'},
 @{name='syntax_worker_answers_after_a_close';file='src/syntax/wf_synworker.c';test='tests/test_syntax_worker.c';old='if (!close && (LONG)gen == sl->live && !w->stopping) {';new='if (!close && !w->stopping) {'},
 @{name='syntax_worker_env_needs_a_dll';file='src/syntax/wf_synworker.c';test='tests/test_syntax_worker.c';old='if (l->builtin) {';new='if (0 && l->builtin) {'},
 @{name='syntax_ui_stale_answer_applied';file='src/ui/wf_app_syntax.c';test='tests/test_syntax_ui.c';old='if (a->gen != app->prev_syn_gen || !app->preview) {';new='if (!app->preview) {'},
 @{name='syntax_ui_markdown_is_coloured_raw';file='src/ui/wf_app_syntax.c';test='tests/test_syntax_ui.c';old='lang->companion || !strcmp(lang->id, "markdown")) {';new='lang->companion) {'},
 @{name='syntax_ui_not_installed_is_asked_for';file='src/ui/wf_app_syntax.c';test='tests/test_syntax_ui.c';old='if (app->syn_port.installed &&';new='if (0 && app->syn_port.installed &&'},
 @{name='syntax_editor_stale_answer_applied';file='src/edit/wf_estsyntax.c';test='tests/test_esession.c';old='a->gen != s->syn_req_gen || !b) {';new='!b) {'},
 @{name='syntax_editor_asks_at_every_drawing';file='src/edit/wf_estsyntax.c';test='tests/test_esession.c';old='if (ts->flat && ts->flat_gen == t->gen && s->syn_buf == b) {';new='if (0 && ts->flat && ts->flat_gen == t->gen && s->syn_buf == b) {'},
 @{name='syntax_editor_text_over_the_limit_sent';file='src/edit/wf_estsyntax.c';test='tests/test_esession.c';old='if (total > WF_SYN_DEFAULT_MAX_TEXT) {';new='if (total > WF_SYN_DEFAULT_MAX_TEXT * 100u) {'},
 @{name='syntax_editor_colours_not_kept_in_place';file='src/edit/wf_estsyntax.c';test='tests/test_esession.c';old='wf_synov_retarget(&ts->ov, ts->flat, ts->flat_len);';new='(void)0;'}
)
$results=@()
$controls=@{}
foreach($m in $mutants){
 if($Only -and $m.name -notmatch $Only){continue}
 if($Test -and $m.test -ne $Test){continue}
 if(!$controls.ContainsKey($m.test)) {
  $controlName=[IO.Path]::GetFileNameWithoutExtension($m.test)
  $controlExe=Join-Path $buildRoot ($controlName+'.exe')
  $controlDir=Join-Path $root ('control-'+$controlName)
  New-Item -ItemType Directory -Path $controlDir|Out-Null
  $runner=Join-Path $buildRoot 'p0_run.exe'
  $controlReceipt=Join-Path $controlDir 'process.json'
  $controlClock=[Diagnostics.Stopwatch]::StartNew()
  & $runner $ControlTimeoutMs $controlExe (Join-Path $controlDir 'test.log') (Join-Path $controlDir 'test-error.log') $controlReceipt
  $controlClock.Stop()
  if($LASTEXITCODE -ne 0){throw "Control supervisor failed: $controlName"}
  $control=Get-Content -LiteralPath $controlReceipt -Raw | ConvertFrom-Json
  $controlVerdict=Get-WfControlVerdict $control
  if($controlVerdict -eq 'SKIP'){
   Write-Host "SKIP: unmodified control prerequisites unavailable: $controlName; see $controlDir"
   Get-Content -LiteralPath (Join-Path $controlDir 'test.log') | Write-Host
   exit 77
  }
  if($controlVerdict -ne 'PASS'){throw "Unmodified control failed; no mutant verdict is valid: $controlName, state=$($control.state), Win32=$($control.error), exit=$($control.exit_code), ran $([int]$controlClock.Elapsed.TotalMilliseconds) ms of a $ControlTimeoutMs ms hang limit; see $controlDir"}
  $controls[$m.test]=$true
 }
 $dir=Join-Path $root $m.name;New-Item -ItemType Directory -Path $dir|Out-Null
 $source=[IO.File]::ReadAllText((Join-Path $repo $m.file))
 $count=Measure-WfAnchor $source $m.old
 $expectedCount=if($m.name -in @('hmac_cancel','edit_parent_required')){2}else{1}
 if($count -ne $expectedCount){throw "Mutation edit count $count for $($m.name)"}
 # Preserve the original relative include base, using absolute include paths.
 $source=Edit-WfAnchor $source $m.old $m.new
 $base=Split-Path -Parent (Join-Path $repo $m.file)
 $source=[regex]::Replace($source,'#include "([^"]+)"',{param($match)
  $header=[IO.Path]::GetFullPath((Join-Path $base $match.Groups[1].Value))
  if(Test-Path -LiteralPath $header){'#include "'+$header.Replace('\','/')+'"'}
  else{$match.Value} # Generated headers resolve through the build include path.
 })
 $file=Join-Path $dir 'mutant.c';[IO.File]::WriteAllText($file,$source)
 $runtime=Get-WfRuntime $Configuration $san
 $exe=Join-Path $dir 'test.exe'
 $driver=Join-Path $repo $m.test
 $compileFiles=@($file,$driver)
 if($m.test -eq 'tests/test_term_input.c'){
  $driverSource=[IO.File]::ReadAllText($driver).Replace('#include "../src/term/wf_term.c"','#include "'+$file.Replace('\','/')+'"')
  $driver=Join-Path $dir 'driver.c';[IO.File]::WriteAllText($driver,$driverSource)
  $compileFiles=@($driver)
 }
 if($m.test -eq 'tests/test_integrity_faults.c'){
  $driverSource=[IO.File]::ReadAllText($driver).Replace('#include "../src/xfer/wf_integrity_win.c"','#include "'+$file.Replace('\','/')+'"')
  $driver=Join-Path $dir 'driver.c';[IO.File]::WriteAllText($driver,$driverSource)
  $compileFiles=@($driver)
 }
 if($m.test -eq 'tests/test_recycle_faults.c'){
  $driverSource=[IO.File]::ReadAllText($driver).Replace('#include "../src/xfer/wf_recycle_win.c"','#include "'+$file.Replace('\','/')+'"')
  $driver=Join-Path $dir 'driver.c';[IO.File]::WriteAllText($driver,$driverSource)
  $compileFiles=@($driver)
 }
 if($m.test -eq 'tests/test_copyjournal_error.c'){
  $driverSource=[IO.File]::ReadAllText($driver).Replace('#include "../src/xfer/wf_copyjournal_win.c"','#include "'+$file.Replace('\','/')+'"')
  $driver=Join-Path $dir 'driver.c';[IO.File]::WriteAllText($driver,$driverSource)
  $compileFiles=@($driver)
 }
 if($m.test -eq 'tests/test_copyengine_faults.c'){
  $driverSource=[IO.File]::ReadAllText($driver).Replace('#include "../src/xfer/wf_copyengine_win.c"','#include "'+$file.Replace('\','/')+'"')
  $driver=Join-Path $dir 'driver.c';[IO.File]::WriteAllText($driver,$driverSource)
  $compileFiles=@($driver)+@('src/xfer/wf_recycle_win.c','src/xfer/wf_probe_win.c','src/fs/wf_enum_win.c' | ForEach-Object { Join-Path $repo $_ })
 }
 if($m.test -eq 'tests/test_copy_source_faults.c'){
  $driverSource=[IO.File]::ReadAllText($driver).Replace('#include "../src/xfer/wf_copy_source_win.c"','#include "'+$file.Replace('\','/')+'"')
  $driver=Join-Path $dir 'driver.c';[IO.File]::WriteAllText($driver,$driverSource)
  $compileFiles=@($driver)
 }
 if($m.test -eq 'tests/test_copyengine_win.c' -and $m.file -ne 'src/xfer/wf_copyengine_win.c'){
  $compileFiles+=@(Join-Path $repo 'tests/wf_copyengine_quarantine.c')
 }
 $archiveOpts=@()
 if($m.test -eq 'tests/test_archive.c'){
  $archiveHeader=Join-Path $dir 'archive-config.h'
  [IO.File]::WriteAllText($archiveHeader, '#define WF_TEST_ARCHIVE_DIR "'+(Join-Path $repo 'tests/archive').Replace('\','/')+'"')
  $archiveOpts=@('/DLIBARCHIVE_STATIC',"/I$repo/src/third_party/libarchive-3.8.10/libarchive","/FI$archiveHeader")
 }
 $arguments=@('/nologo','/std:c17','/W4','/WX','/utf-8',$runtime,'/DUNICODE','/D_UNICODE','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN',
  "/I$repo/src","/I$repo/tests","/I$buildRoot/generated","/Fo$dir/","/Fe$exe") + $archiveOpts + $compileFiles
 Copy-Item -LiteralPath (Join-Path $repo 'languages') -Destination (Join-Path $dir 'languages') -Recurse
 if($m.test -in @('tests/test_edit_win.c','tests/test_copy_source_win.c','tests/test_copy_source_faults.c','tests/test_copyengine_win.c','tests/test_extract_win.c')){
  $arguments+=@((Join-Path $repo 'src/xfer/wf_probe_win.c'),(Join-Path $repo 'src/fs/wf_enum_win.c'))
 }
 if($m.test -eq 'tests/test_searchworker_win.c'){
  # the program's own enumerator, as a source file, beside the mutated worker
  $arguments+=(Join-Path $repo 'src/fs/wf_enum_win.c')
 }
 $synTests=@('tests/test_syntax_loader.c','tests/test_syntax_host.c','tests/test_syntax_worker.c')
 if($m.test -eq 'tests/test_syntax_loader.c'){
  # the loader is linked straight in, with the runtime it parses with
  if($m.file -ne 'src/syntax/wf_syncore.c'){$arguments+=(Join-Path $repo 'src/syntax/wf_syncore.c')}
  $arguments+=@("/I$repo/src/third_party/tree-sitter-0.27.2/include")
 }
 if($m.test -in $synTests){
  $arguments+=@("/I$repo/src/syntax")
  # the real piero-syntax.dll and the grammars beside the program, as the loader looks for them
  Copy-Item -LiteralPath (Join-Path $buildRoot 'syntax') -Destination (Join-Path $dir 'syntax') -Recurse
  Remove-Item -LiteralPath (Join-Path $dir 'syntax/lib') -Recurse -Force -ErrorAction SilentlyContinue
  Copy-Item -LiteralPath (Join-Path $buildRoot 'syntax_fixtures') -Destination (Join-Path $dir 'syntax_fixtures') -Recurse
  Remove-Item -LiteralPath (Join-Path $dir 'syntax_fixtures/lib') -Recurse -Force -ErrorAction SilentlyContinue
 }
 if($m.test -in @('tests/test_efile_win.c','tests/test_efile_faults.c')){
  # the editor's file code asks the job's cancel token, which lives here
  $arguments+=@((Join-Path $repo 'src/async/wf_blocking.c'))
 }
 if($m.test -eq 'tests/test_state_dir.c'){
  # the config path helpers are part of what is under test; built beside the mutant
  $arguments+=@((Join-Path $repo 'src/config/wf_config_win.c'))
 }
 if($m.test -eq 'tests/test_copyengine_win.c' -and $m.file -eq 'src/xfer/wf_copyengine_win.c'){$arguments+=@('/DWF_COPYENGINE_QUARANTINE_TEST')}
 if($m.test -eq 'tests/test_copyengine_win.c'){
  # Keep the quarantined engine distinct if a CRT helper pulls the product TU.
  $arguments+=@('/Dwf_copyengine_run=wf_quarantine_run','/Dwf_copyengine_test_after_aside=wf_quarantine_after_aside')
 }
 $arguments+=@('/link',(Join-Path $buildRoot 'wfcore.lib'),(Join-Path $buildRoot 'wfstorage.lib'),
  (Join-Path $buildRoot 'p0_lab.lib'),(Join-Path $buildRoot 'p0_common.lib'),(Join-Path $buildRoot 'wfjournal.lib'),'bcrypt.lib','advapi32.lib','shell32.lib','user32.lib')
 if($m.test -eq 'tests/test_copyengine_win.c'){
  $arguments+=@((Join-Path $buildRoot 'wfcopyengine.lib'),(Join-Path $buildRoot 'wfverify.lib'),(Join-Path $buildRoot 'wfintegrity.lib'),'ole32.lib','shell32.lib','uuid.lib')
 }
 if($m.test -eq 'tests/test_image_hostile.c'){$arguments+='psapi.lib'}
 if($m.test -in @('tests/test_state_dir.c','tests/test_dechost_win.c','tests/test_image_hostile.c','tests/test_decode_win.c')){
  foreach($l in 'wfoldstate','wfefile','wfdecode','wfarchive','wfstate'){$arguments+=(Join-Path $buildRoot "$l.lib")}
  foreach($l in 'wf_libarchive','wf_zlib','wf_lzma','wf_zstd'){$arguments+=(Join-Path $buildRoot "src/third_party/$l.lib")}
  $arguments+=@('user32.lib','ole32.lib','uuid.lib','shlwapi.lib','shcore.lib','userenv.lib','windowscodecs.lib','runtimeobject.lib')
 }
 if($m.test -eq 'tests/test_worker_lifecycle.c'){
  # the four workers sit beside the mutated one (it replaces its own object in the library)
  $arguments+=@((Join-Path $buildRoot 'wfhash.lib'),(Join-Path $buildRoot 'wfsmartgo.lib'),(Join-Path $buildRoot 'wflock.lib'),(Join-Path $buildRoot 'wfclip.lib'),(Join-Path $buildRoot 'wflaunch.lib'),(Join-Path $buildRoot 'wfdecode.lib'),(Join-Path $buildRoot 'wfstate.lib'),(Join-Path $buildRoot 'wfoldstate.lib'),(Join-Path $buildRoot 'wfefile.lib'),(Join-Path $buildRoot 'wfarchive.lib'),'rstrtmgr.lib','ole32.lib','uuid.lib','psapi.lib','shlwapi.lib','shcore.lib','userenv.lib','windowscodecs.lib','runtimeobject.lib')
 foreach($l in 'wf_libarchive','wf_zlib','wf_lzma','wf_zstd'){$arguments+=(Join-Path $buildRoot "src/third_party/$l.lib")}
 }
 if($m.test -eq 'tests/test_extract_win.c'){
  # the copy engine puts the staged items in place, and the archive reader fills the staging
  $arguments+=@((Join-Path $buildRoot 'wfcopyengine.lib'),(Join-Path $buildRoot 'wfverify.lib'),(Join-Path $buildRoot 'wfintegrity.lib'),(Join-Path $buildRoot 'wfarchive.lib'),'ole32.lib','shell32.lib','uuid.lib')
  foreach($l in 'wf_libarchive','wf_zlib','wf_lzma','wf_zstd'){$arguments+=(Join-Path $buildRoot "src/third_party/$l.lib")}
 }
 if($m.test -eq 'tests/test_copyengine_faults.c'){
  $arguments+=@((Join-Path $buildRoot 'wfverify.lib'),(Join-Path $buildRoot 'wfintegrity.lib'),'ole32.lib','shell32.lib','uuid.lib')
 }
 if($m.test -eq 'tests/test_recycle_faults.c'){
  $arguments+=@('ole32.lib','shell32.lib','uuid.lib')
 }
 if($m.test -eq 'tests/test_archive.c'){
  foreach($l in 'wf_libarchive','wf_zlib','wf_lzma','wf_zstd'){$arguments+=(Join-Path $buildRoot "src/third_party/$l.lib")}
 }
 if($m.test -in @('tests/test_esession.c','tests/test_sftpplace.c','tests/test_sftpplace_proc.c','tests/test_markdown_ui.c','tests/test_git_edit.c')){
  $arguments+=(Join-Path $buildRoot 'wfefile.lib')
 }
 if($m.test -in @('tests/test_git.c','tests/test_git_worker.c')){
  # the Git worker and backend, and the program that quotes their arguments
  $arguments+=@((Join-Path $buildRoot 'wfgit.lib'),(Join-Path $buildRoot 'wflaunch.lib'),'shell32.lib','ole32.lib')
 }
 if($m.test -eq 'tests/test_session.c'){
  $arguments+=(Join-Path $buildRoot 'wfsession.lib')
 }
 if($m.test -eq 'tests/test_searchworker_win.c'){
  $arguments+=(Join-Path $buildRoot 'wfsmartgo.lib')
  $arguments+=@((Join-Path $buildRoot 'wfhash.lib'),'bcrypt.lib')
  $arguments+=@((Join-Path $buildRoot 'p0_common.lib'),'advapi32.lib')
 }
 if($m.test -eq 'tests/test_sha256.c'){
  $arguments+=@((Join-Path $buildRoot 'wfhash.lib'),'bcrypt.lib')
 }
 if($m.test -in @('tests/test_syntax_core.c','tests/test_syntax_host.c','tests/test_syntax_worker.c')){
  $arguments+=(Join-Path $buildRoot 'wfsyntax.lib')
 }
 if($m.test -eq 'tests/test_syntax_host.c'){$arguments+='psapi.lib'}
 if($m.test -eq 'tests/test_syntax_loader.c'){
  $arguments+=@((Join-Path $buildRoot 'wfsyntax.lib'),(Join-Path $buildRoot 'src/third_party/wf_treesitter_dyn.lib'))
 }
 if($m.test -eq 'tests/test_batchworker_win.c'){
  # the engine, its journal and the single rename sit beside the mutated worker
  $arguments+=@((Join-Path $buildRoot 'wfbatch.lib'),(Join-Path $buildRoot 'wfedit.lib'),'bcrypt.lib','user32.lib')
 }
 if($m.test -eq 'tests/test_batchexec.c'){
  $arguments+=@((Join-Path $buildRoot 'wfbatch.lib'),(Join-Path $buildRoot 'wfedit.lib'),'bcrypt.lib','user32.lib')
 }
 if($m.test -in @('tests/test_batchplan.c','tests/test_bulkname.c')){
  $arguments+='user32.lib'
 }
 if($m.test -in @('tests/test_edraft.c','tests/test_draftwriter.c')){
  # the slot files, the owned delete and the state folder sit beside the mutated draft code
  $arguments+=@((Join-Path $buildRoot 'wfefile.lib'),(Join-Path $buildRoot 'wfstate.lib'),(Join-Path $buildRoot 'wfjournal.lib'),(Join-Path $buildRoot 'wfcore.lib'),(Join-Path $buildRoot 'wfstorage.lib'),'bcrypt.lib','user32.lib','advapi32.lib','shell32.lib')
 }
 if($m.test -eq 'tests/test_edraft.c'){
  $arguments=@("/FI$repo/tests/wf_draft_case_probe.h")+$arguments
 }
 if($m.test -eq 'tests/test_draftwriter.c'){
  $arguments=@("/FI$repo/tests/wf_draft_write_probe.h")+$arguments
 }
 if($m.test -eq 'tests/test_preview_win.c'){
  # the archive reader that the worker's archive preview calls
  $arguments+=@((Join-Path $buildRoot 'wfarchive.lib'),(Join-Path $buildRoot 'src/third_party/wf_libarchive.lib'),(Join-Path $buildRoot 'src/third_party/wf_zlib.lib'),(Join-Path $buildRoot 'src/third_party/wf_lzma.lib'),(Join-Path $buildRoot 'src/third_party/wf_zstd.lib'),'bcrypt.lib','advapi32.lib')
 }
 if($m.test -eq 'tests/test_terminal.c'){
  # the program that stands for wt.exe, copied by the test from beside itself
  $arguments+=@((Join-Path $buildRoot 'wflaunch.lib'),(Join-Path $buildRoot 'wfcore.lib'),(Join-Path $buildRoot 'wfstorage.lib'),'shell32.lib','user32.lib','ole32.lib','bcrypt.lib','advapi32.lib')
  Copy-Item -LiteralPath (Join-Path $buildRoot 'terminal_probe.exe') -Destination $dir -Force
 }
 if($m.test -eq 'tests/test_fuzzyworker_win.c'){
  $arguments+=(Join-Path $buildRoot 'wfsmartgo.lib')
 }
 if($m.test -eq 'tests/test_clipboard.c'){
  $arguments+=@((Join-Path $buildRoot 'wfclip.lib'),'user32.lib','ole32.lib','bcrypt.lib','advapi32.lib')
  if($m.file -in @('src/clipboard/wf_clipboard.c','src/clipboard/wf_dataobj_files_win.c')){
   $arguments=@('/DWF_CLIPBOARD_PURE_TEST')+$arguments
  }
 }
 if($m.test -eq 'tests/test_lock.c'){
  # the lock library, Restart Manager, and the helper program the test starts (never a user's)
  $arguments+=@((Join-Path $buildRoot 'wflock.lib'),(Join-Path $buildRoot 'wfclip.lib'),(Join-Path $buildRoot 'wflaunch.lib'),'rstrtmgr.lib','user32.lib')
  Copy-Item -LiteralPath (Join-Path $buildRoot 'lock_helper.exe') -Destination $dir -Force
 }
 if($m.test -eq 'tests/test_sftpplace_proc.c'){
  # the stand-in for ssh.exe sits next to the test, where the test looks
  Copy-Item -LiteralPath (Join-Path $buildRoot 'fake_sftpd.exe') -Destination $dir -Force
 }
 if($m.test -in @('tests/test_copy_source_win.c','tests/test_copy_source_faults.c')){
  $arguments+=(Join-Path $buildRoot 'p0_source_guard.lib')
 }
 $arguments=Add-WfSanitizerArgs $arguments $san
 $saved=$ErrorActionPreference
 $compileClock=[Diagnostics.Stopwatch]::StartNew()
 try{$ErrorActionPreference='Continue';& $Compiler @arguments *> (Join-Path $dir 'build.log');$compileExit=$LASTEXITCODE}
 finally{$ErrorActionPreference=$saved}
 $compileClock.Stop()
 if($compileExit -ne 0){throw "Mutant compile failed, never a kill: $($m.name); see $(Join-Path $dir 'build.log')"}
 $runner=Join-Path $buildRoot 'p0_run.exe'
 $receiptPath=Join-Path $dir 'process.json'
 $env:WF_TEST_FAIL_FAST='1'
 $clock=[Diagnostics.Stopwatch]::StartNew()
 try{& $runner $MutantTimeoutMs $exe (Join-Path $dir 'test.log') (Join-Path $dir 'test-error.log') $receiptPath}
 finally{Remove-Item Env:\WF_TEST_FAIL_FAST -ErrorAction SilentlyContinue}
 $elapsedMs=[int]$clock.Elapsed.TotalMilliseconds
 if($LASTEXITCODE -ne 0){throw "Process supervisor failed: $($m.name)"}
 $receipt=Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
 if($receipt.state -ne 0){throw "Mutant not completed (never a kill): $($m.name), state=$($receipt.state), Win32=$($receipt.error), ran ${elapsedMs} ms of a ${MutantTimeoutMs} ms hang limit; see $receiptPath"}
 $exitCode=[int]$receipt.exit_code
 $verdict=Test-WfMutant $m.name $dir $exitCode
 $verdict | Add-Member -NotePropertyName run_ms -NotePropertyValue $elapsedMs
 $verdict | Add-Member -NotePropertyName compile_ms -NotePropertyValue ([int]$compileClock.Elapsed.TotalMilliseconds)
 Write-Host "  $($m.name): ${elapsedMs} ms"
 $results+=$verdict
}
exit (Complete-WfMutations $results $root $null)

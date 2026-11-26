param([Parameter(Mandatory)][string]$Compiler,[Parameter(Mandatory)][string]$Build,
  [Parameter(Mandatory)][string]$Configuration,[string]$Only='',[int]$RunTimeoutMs=240000)
# Mutation check of the recovery gate on the REAL program (DR-DRAFT-017). Each mutant is a piero.exe
# built by a CMake target with ONE source file replaced by a copy in
# which the gate (or the state-folder choice, or the recovery command's lookup) is disabled; the
# `state-gate` (or `first-run`, or `pictures`) ConPTY scenario is then run against it. A mutant is KILLED only when the scenario
# passes on the unmodified program, then runs to its end and reports failed checks (exit 1) on
# the mutant. A scenario that hangs, cannot start, or a
# mutant that does not compile is an error, never a kill.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../wf_toolchain.ps1')
. (Join-Path $PSScriptRoot '../wf_vs.ps1')
. (Join-Path $PSScriptRoot 'wf_mutation_text.ps1')
Initialize-WfToolchain
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildRoot=(Resolve-Path -LiteralPath $Build).Path
$root=Join-Path $buildRoot ('gate-mutants-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root|Out-Null
$host_exe=Join-Path $buildRoot 'p0_conpty_host.exe'
if(!(Test-Path -LiteralPath $host_exe)){throw 'p0_conpty_host.exe is not built'}
$mutants=@(
 @{name='gate_removed';file='src/main.c';only='gate_unresolved_record';old='if (WF_STATE_BOTH == kind && wf_main_old_state_blocks()) {';new='if (0 && WF_STATE_BOTH == kind && wf_main_old_state_blocks()) {'},
 @{name='gate_does_not_stop';file='src/main.c';only='gate_unresolved_record';old='lp->copy_off = MSG_COPY_SAFETY_BLOCKED; goto done; } if (!wf_cj_inspect(journal, &lp->copy_recovery) ||';new='lp->copy_off = MSG_COPY_SAFETY_BLOCKED; } if (!wf_cj_inspect(journal, &lp->copy_recovery) ||'},
 @{name='gate_judges_nothing';file='src/main_recovery.c';only='gate_unresolved_record';old='return !look_old_state(&r) || wf_state_report_blocks(&r);';new='return !look_old_state(&r) && wf_state_report_blocks(&r);'},
 @{name='unresolved_record_not_blocking';file='src/recovery/wf_old_state.c';only='gate_unresolved_record';old='return r->unresolved || r->store_unknown || r->limit || r->copy_legacy ||';new='return r->store_unknown || r->limit || r->copy_legacy ||'},
 @{name='unreadable_record_not_blocking';file='src/recovery/wf_old_state.c';only='gate_bad_record_name';old='return r->unresolved || r->store_unknown || r->limit || r->copy_legacy ||';new='return r->unresolved || r->limit || r->copy_legacy ||'},
 @{name='damaged_receipts_not_blocking';file='src/recovery/wf_old_state.c';only='gate_damaged_receipts';old='r->limit || r->copy_legacy ||';new='r->limit ||'},
 @{name='both_folders_use_the_older';file='src/base/wf_state_dir.c';only='select_both';old='*kind = PROBE_FOLDER == old ? WF_STATE_BOTH : WF_STATE_PIERO;';new='*kind = PROBE_FOLDER == old ? WF_STATE_LEGACY : WF_STATE_PIERO;'},
 @{name='recovery_command_ignores_older_store';file='src/main_recovery.c';only='resolve';old='if (WF_RECOVERY_LOOKUP_NONE == got && WF_STATE_BOTH == kind) {';new='if (0 && WF_RECOVERY_LOOKUP_NONE == got && WF_STATE_BOTH == kind) {'},
 @{name='copy_reason_not_given';file='src/main.c';only='select_access_denied';old='wf_app_set_copy_off(&lp->app, lp->copy_off);';new='(void)lp->copy_off;'},
 @{name='open_reason_not_given';file='src/main.c';scenario='first-run';only='first_run_folder_made_by_another_program';old='wf_app_set_open_off(&lp->app, MSG_OPEN_OFF_PRIVATE);';new='(void)0;'},
 @{name='foreign_folder_accepted';file='src/path/cwd_neutralize.c';scenario='first-run';only='first_run_folder_made_by_another_program';old='!private_acl(h, user->User.Sid)';new='!private_acl(h, user->User.Sid) && 0'},
 @{name='used_working_directory_accepted';file='src/path/cwd_neutralize.c';scenario='first-run';only='first_run_working_directory_not_empty';old='if (empty && !directory_empty(canonical.path)) {';new='if (empty && !directory_empty(canonical.path) && 0) {'},
 @{name='state_link_followed';file='src/base/wf_state_dir.c';scenario='first-run';only='first_run_state_folder_is_a_link';old='if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {';new='if (0 && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {'},
 @{name='damaged_picture_has_no_words';file='src/ui/wf_app_image.c';scenario='pictures';old='return wf_msg(MSG_IMG_ERR_CORRUPT);';new='return wf_msg(MSG_IMG_ERR_INTERNAL);'},
 @{name='password_is_not_asked_for';file='src/ui/wf_app_image.c';scenario='pictures';old='case WF_DSTAT_PASSWORD_REQUIRED: return wf_msg(MSG_IMG_ERR_PASSWORD);';new='case WF_DSTAT_PASSWORD_REQUIRED: return wf_msg(MSG_IMG_ERR_CORRUPT);'},
 @{name='wrong_password_not_told';file='src/ui/wf_app_image.c';scenario='pictures';old='case WF_DSTAT_PASSWORD_WRONG: return wf_msg(MSG_IMG_ERR_PASSWORD_WRONG);';new='case WF_DSTAT_PASSWORD_WRONG: return wf_msg(MSG_IMG_ERR_PASSWORD);'},
 @{name='wrong_password_read_as_missing';file='src/preview/wf_pdf_win.c';scenario='pictures';old='return had_password ? WF_DSTAT_PASSWORD_WRONG : WF_DSTAT_PASSWORD_REQUIRED;';new='return (void)had_password, WF_DSTAT_PASSWORD_REQUIRED;'},
 @{name='listing_ignores_directories_first';file='src/main_pump.c';scenario='listing';old='lp->config->sort_descending, 0 != lp->config->directories_first) : WF_OK;';new='lp->config->sort_descending, 1) : WF_OK;'},
 @{name='sort_menu_ignores_directories_first_in_the_program';file='src/ui/wf_app_sort.c';scenario='listing';old='return !app->config || 0 != app->config->directories_first;';new='(void)app; return true;'},
 @{name='recycle_reason_not_given';file='src/ui/wf_app_input.c';scenario='first-run';only='first_run_state_folder_denied';old='wf_app_set_msg(app, wf_msg(recycle_off_reason(app)));';new='(void)recycle_off_reason(app); wf_app_set_msg(app, wf_msg(MSG_RECYCLE_OFF_SAFETY));'},
 @{name='dirsize_totals_not_drawn';file='src/ui/wf_app_render.c';scenario='dirsize';old='if (w - (cx - x) - gw < total_w + 10) {';new='if (1) {'},
 @{name='dirsize_r_keeps_totals_in_the_program';file='src/ui/wf_app_keys.c';scenario='dirsize';old='wf_app_dirsize_forget(app);';new='(void)0;'},
 @{name='dirsize_never_started_in_the_program';file='src/main.c';scenario='dirsize';old='wf_app_set_sizes(&lp->app, (WfSizePort){lp, lazy_size_start, lazy_size_cancel});';new='(void)lp; (void)lazy_size_start; (void)lazy_size_cancel;'},
 @{name='continue_never_reads_the_session';file='src/main_session.c';scenario='continue';only='session_round_trip';old='		continue_from_store(lp);';new='		(void)continue_from_store;'},
 @{name='continue_never_saves_the_session';file='src/main_session.c';scenario='continue';only='session_round_trip';old='if (WF_OK != wf_session_writer_submit(lp->session_writer, text, n)) {';new='free(text); if (0) {'},
 @{name='continue_cursor_lost_to_the_listing';file='src/ui/wf_app_nav.c';scenario='continue';only='session_same_folder';old='bool session = app->session_restore.active && app->cwd &&';new='bool session = 0 && app->cwd &&'},
 @{name='continue_editor_never_reopened';file='src/ui/wf_app_session.c';scenario='continue';only='session_editor';old='wf_app_edit_begin_named(app, leaf);';new='(void)leaf;'},
 @{name='continue_cursor_lost_to_the_draft_answer';file='src/edit/wf_esession.c';scenario='continue';only='session_editor';old='if (s->goto_pending) { /* the place asked for while it was open */';new='if (0 && s->goto_pending) { /* the place asked for while it was open */'},
 @{name='continue_writer_without_the_right';file='src/session/wf_session_writer.c';scenario='continue';only='session_other';old='if (!wf_session_store_lock(store)) {';new='if (0) {'},
 @{name='watch_listing_never_refreshed';file='src/main_pump.c';scenario='watch';only='watch';old='lp->watch_listing_dirty |= change.listing;';new='lp->watch_listing_dirty |= 0;'},
 @{name='watch_preview_never_refreshed';file='src/main_pump.c';scenario='watch';only='watch';old='lp->watch_preview_dirty |= change.preview;';new='lp->watch_preview_dirty |= 0;'},
 @{name='watch_editor_never_told';file='src/main_pump.c';scenario='watch';only='watch_editor';old='wf_main_editor_disk_check(lp)) {';new='0 && wf_main_editor_disk_check(lp)) {'},
 @{name='clipboard_service_never_connected';file='src/main.c';scenario='clipboard';only='clipboard_copy';old='wf_app_set_clip_files(&lp->app, (WfClipFilesPort){lp, lazy_clip_read, lazy_clip_publish});';new='(void)lp; (void)lazy_clip_read; (void)lazy_clip_publish;'},
 @{name='clipboard_cut_is_pasted_in_the_program';file='src/ui/wf_app_clip.c';scenario='clipboard';only='clipboard_refusal';old='if (wf_clip_effect_is_cut(files->effect)) {';new='if (0) {'},
 @{name='clipboard_paste_never_follows_the_capture';file='src/ui/wf_app_clip.c';scenario='clipboard';only='clipboard_copy';old='app->paste_after_capture = true; /* the capture, then the same as `p` */';new='app->paste_after_capture = false; /* the capture, then the same as `p` */'},
 @{name='clipboard_y_publishes_nothing';file='src/clipboard/wf_clipworker_win.c';scenario='clipboard';only='clipboard_publish';old='hr = OleSetClipboard(pdo);';new='hr = S_OK;'},
 @{name='clipboard_y_data_gone_after_piero_ends';file='src/clipboard/wf_clipworker_win.c';scenario='clipboard';only='clipboard_publish';old='(void)OleFlushClipboard();';new='(void)0;'},
 @{name='lock_service_never_connected';file='src/main.c';scenario='lock';only='lock_rename';old='wf_app_set_lock(&lp->app, (WfLockPort){lp, lazy_lock_query, lazy_lock_close, lazy_lock_end});';new='(void)lp; (void)lazy_lock_query; (void)lazy_lock_close; (void)lazy_lock_end;'},
 @{name='lock_close_success_without_exit';file='src/process/wf_process_control_win.c';scenario='lock';only='lock_copy';old='if (WAIT_OBJECT_0 == r) {';new='if (r || 1) {'},
 @{name='lock_refused_close_escalates_to_end';file='src/ui/wf_app_lock.c';scenario='lock';only='lock_copy';old='case WF_LOCKRES_STILL:';new='case WF_LOCKRES_STILL: if (WF_LOCKREQ_CLOSE == a->req && app->lock.port.end) { (void)app->lock.port.end(app->lock.port.ctx, ++app->lock.gen, app->lock.act.pid, app->lock.act.start, app->lock.act.type); }'},
 @{name='lock_end_asks_no_question';file='src/ui/wf_app_lock.c';scenario='lock';only='lock_copy';old="wf_app_confirm_begin_ex(app, lines, 4, 'y', wf_msg(MSG_LOCK_EQ1_ARMED), end_yes1, NULL, ev)";new="((void)ev, (void)end_yes1, (end_yes2(app, NULL), WF_OK))"},
 @{name='lock_end_second_question_skipped';file='src/ui/wf_app_lock.c';scenario='lock';only='lock_copy';old="st = wf_app_confirm_begin_ex(app, lines, 3, 'k', wf_msg(MSG_LOCK_EQ2_ARMED), end_yes2, NULL, NULL);";new="(void)lines; (void)who; st = (end_yes2(app, NULL), WF_OK);"},
 @{name='fuzzy_answers_dropped_in_the_program';file='src/main_pump.c';scenario='fuzzy';only='fuzzybig';old='wf_app_on_fuzzy(&lp->app, &a);';new='wf_fz_answer_free(&a);'},
 @{name='fuzzy_matched_letters_not_marked_in_the_program';file='src/ui/wf_app_fuzzy.c';scenario='fuzzy';only='fuzzy';old='st.attributes |= WF_ATTR_BOLD | WF_ATTR_UNDERLINE;';new='st.attributes |= WF_ATTR_BOLD;'},
 @{name='fuzzy_list_change_not_searched_again_in_the_program';file='src/ui/wf_app_fuzzy.c';scenario='fuzzy';only='fuzzy';old='drop_hits(z); z->pending = false; z->dirty = true;';new='drop_hits(z); z->pending = false;'},
 @{name='batch_confirm_skipped_in_the_program';file='src/ui/wf_app_batch.c';scenario='batch';only='batch_rename';old='ask_to_run(app, ev);';new='if (ev->vk == 0xFFFF) { ask_to_run(app, ev); } batch_yes(app, NULL);'},
 @{name='batch_refused_rows_run_in_the_program';file='src/ui/wf_app_batch.c';scenario='batch';only='batch_refused';old='if (!b->job || !b->port.ready || !b->port.ready(b->port.ctx, b->job) || WF_BX_PRE_READY != b->pre.pre || 0 != b->n_bad) { return false; } pl = plan_of(app); return pl && 0 == pl->n_refused && pl->n_steps > 0;';new='pl = plan_of(app); return b->job && pl;'},
 @{name='batch_cancel_ignored_in_the_program';file='src/ui/wf_app_batch.c';scenario='batch';only='batch_cancel';old='if (b->port.cancel) {';new='if (b->port.cancel && b->recover) {'},
 @{name='batch_done_for_a_rollback_in_the_program';file='src/ui/wf_app_batch.c';scenario='batch';only='batch_cancel';old='switch (r->result) {';new='switch (r->result == WF_BX_ROLLED_BACK ? WF_BX_SUCCESS : r->result) {'},
 @{name='batch_recovery_runs_unasked_in_the_program';file='src/ui/wf_app_batch.c';scenario='batch';only='batch_crash';old='b->ask_pending = b->unfinished > 0;';new='b->ask_pending = b->unfinished > 0; if (b->ask_pending) { recover_yes(app, NULL); }'},
 @{name='batch_cursor_on_the_old_name_in_the_program';file='src/ui/wf_app_batch.c';scenario='batch';only='batch_rename';old='select = first_renamed(b);';new='(void)first_renamed(b); select = b->item[0].name;'},
 @{name='ntfsfacts_never_read_in_the_program';file='src/ui/wf_app_input.c';scenario='ntfsfacts';only='ntfsfacts';old='wf_ntfsfacts_read(full, &facts);';new='(void)full;'},
 @{name='ntfsfacts_streams_not_listed_in_the_program';file='src/fs/wf_ntfsfacts_win.c';scenario='ntfsfacts';only='ntfsfacts';old='f->n_streams++;';new='(void)0;'},
 @{name='hash_answer_dropped_in_the_program';file='src/main_pump.c';scenario='hash';only='hash';old='wf_app_on_hash(&lp->app, &u);';new='(void)u;'},
 @{name='hash_key_does_nothing_in_the_program';file='src/ui/wf_app_keys.c';scenario='hash';only='hash';old='case WF_FM_HASH:            wf_app_cmd_hash(app); break;';new='case WF_FM_HASH: break;'},
 @{name='history_undo_entry_kept_after_success_in_the_program';file='src/ui/wf_app_batch.c';scenario='batch';only='batch_rename';old='wf_app_undo_retire(app, b->undo_uid);';new='(void)0;'},
 @{name='history_undo_goes_to_the_new_name_in_the_program';file='src/ui/wf_app_batch.c';scenario='batch';only='batch_rename';old='wf_path_dup(b->item[i].name);';new='wf_path_dup(b->item[i].to);'},
 @{name='tabs_place_written_under_the_wrong_letter_in_the_program';file='src/ui/wf_app_tabs.c';scenario='tabs';only='tabs';old='ch.key = key;';new='ch.key = "z";'},
 @{name='tabs_place_never_goes_in_the_program';file='src/ui/wf_app_tabs.c';scenario='tabs';only='tabs';old='st = wf_app_navigate(app, path, NULL, SIZE_MAX);';new='st = WF_OK; (void)path;'},
 @{name='tabs_strip_never_drawn_in_the_program';file='src/ui/wf_app_render.c';scenario='tabs';only='tabs';old='int strip = wf_app_tabs_strip(app, f, 0, room > 24 ? room / 2 : room >= 12 ? 7 : 0);';new='int strip = wf_app_tabs_strip(app, f, 0, 0);'},
 @{name='search_never_started_in_the_program';file='src/main.c';scenario='search';only='search';old='wf_app_set_search(&lp->app, (WfSearchPort){lp, lazy_search_start, lazy_search_cancel, lazy_search_dupes});';new='(void)lp; (void)lazy_search_start; (void)lazy_search_cancel; (void)lazy_search_dupes;'},
 @{name='search_tab_does_not_search_in_the_program';file='src/ui/wf_app_input.c';scenario='search';only='search';old='wf_app_search_begin(app);';new='(void)app;'},
 @{name='search_several_folders_copied_in_the_program';file='src/ui/wf_app_hits.c';scenario='search';only='search';old='if (1 != plan.nrun) {';new='if (0) {'},
 @{name='dupes_never_started_in_the_program';file='src/main.c';scenario='dupes';only='dupes';old='wf_searchworker_set_hash(lp->search, wf_dupehash_file);';new='(void)lp;'},
 @{name='dupes_key_does_nothing_in_the_program';file='src/ui/wf_app_keys.c';scenario='dupes';only='dupes';old='case WF_FM_DUPES:           wf_app_dupes_begin(app); break;';new='case WF_FM_DUPES: break;'},
 @{name='dupes_last_copy_marked_in_the_program';file='src/ui/wf_app_hits.c';scenario='dupes';only='dupes';old='if (!another_is_free(h, i)) {';new='if (!another_is_free(h, i) && 0) {'},
 @{name='hits_ctrl_a_never_delivered_in_the_program';file='src/main_editor.c';scenario='search';only='search';old='&WF_MODE_HITS == lp->app.mode; /* Ctrl+A marks all results */';new='0; /* Ctrl+A marks all results */'},
 @{name='syntax_preview_never_wired_in_the_program';file='src/main.c';scenario='syntax';only='syntax_json_only';old='wf_app_set_syntax(&lp->app, wf_synworker_port(lp->syntax));';new='(void)lp;'},
 @{name='syntax_answers_never_delivered_in_the_program';file='src/main_pump.c';scenario='syntax';only='syntax_json_only';old='changed |= pump_syntax(lp);';new='changed |= (0 && pump_syntax(lp));'},
 @{name='syntax_pin_missing_in_the_program';file='src/main.c';scenario='syntax';only='syntax_json_only';old='wf_synhost_configure(NULL, WF_SYNTAX_DLL_SHA256);';new='wf_synhost_configure(NULL, NULL);'},
 @{name='syntax_editor_never_wired_in_the_program';file='src/main_editor.c';scenario='syntax';only='syntax_editor';old='wf_es_set_syntax(lp->editor, wf_synworker_port(lp->syntax));';new='(void)lp;'}
)
function Invoke-GateScenario([string]$Scenario,[string]$OnlyCase,[string]$Child,
  [string]$Directory,[string]$Label) {
 $evidence=Join-Path $Directory 'evidence'
 $log=Join-Path $Directory 'scenario.log'
 $previousOnly=$env:PIERO_GATE_ONLY
 $env:PIERO_GATE_ONLY=$OnlyCase
 $clock=[Diagnostics.Stopwatch]::StartNew()
 try{
  $arguments=@('--scenario',$Scenario,'--child',('"'+$Child+'"'),
    '--results',('"'+$evidence+'"'),'--label',$Label)
  if($Scenario -eq 'lock'){$arguments+=@('--helper',('"'+(Join-Path $buildRoot 'lock_helper.exe')+'"'))}
  $p=Start-Process -FilePath $host_exe -ArgumentList $arguments -PassThru -NoNewWindow -RedirectStandardOutput $log -RedirectStandardError (Join-Path $Directory 'scenario-error.log')
  $null=$p.Handle
  if(!$p.WaitForExit($RunTimeoutMs)){
   Stop-Process -Id $p.Id -Force
   throw "The $Label scenario did not finish (never a kill): $Directory"
  }
 } finally {$env:PIERO_GATE_ONLY=$previousOnly}
 $out=Get-Content -LiteralPath $log -Raw
 if($out -notmatch 'PRODUCT-TUI: (\d+) checks passed, (\d+) failed'){
  throw "The $Label scenario did not report its checks (never a kill): $log"
 }
 return [pscustomobject]@{ExitCode=$p.ExitCode;Passed=[int]$Matches[1];
   Failed=[int]$Matches[2];Seconds=[int]$clock.Elapsed.TotalSeconds;Evidence=$evidence}
}
$controls=@{}
$results=@()
try {
$controlSaved=$ErrorActionPreference
try {
 $ErrorActionPreference='Continue'
 & cmake --build $buildRoot --target piero --parallel 2 *> (Join-Path $root 'control-build.log')
 $controlBuildExit=$LASTEXITCODE
} finally {$ErrorActionPreference=$controlSaved}
if($controlBuildExit -ne 0){throw "Unmodified product build failed; no mutant verdict is valid; see $root\control-build.log"}
foreach($m in $mutants){
 if($Only -and $m.name -notmatch $Only){continue}
 $scenario=if($m.scenario){$m.scenario}else{'state-gate'}
 $controlKey=$scenario+'|'+$m.only
 if(!$controls.ContainsKey($controlKey)){
  # Match the mutant's installation layout as well as its scenario filter.
  $controlDir=Join-Path $root ('control-'+$controls.Count)
  New-Item -ItemType Directory -Path $controlDir|Out-Null
  $controlExe=Join-Path $controlDir 'piero.exe'
  Copy-Item -LiteralPath (Join-Path $buildRoot 'piero.exe') -Destination $controlExe
  Copy-Item -LiteralPath (Join-Path $buildRoot 'languages') -Destination (Join-Path $controlDir 'languages') -Recurse
  Copy-Item -LiteralPath (Join-Path $buildRoot 'LICENSE') -Destination $controlDir -Force -ErrorAction SilentlyContinue
  Copy-Item -LiteralPath (Join-Path $buildRoot 'LICENSES') -Destination $controlDir -Recurse
  $syntax=Join-Path $buildRoot 'syntax'
  if(Test-Path -LiteralPath $syntax){
   Copy-Item -LiteralPath $syntax -Destination $controlDir -Recurse
  }
  $runtime=Join-Path $buildRoot 'piero.asan.runtime'
  if(Test-Path -LiteralPath $runtime){
   Copy-Item -LiteralPath $runtime -Destination $controlDir -Recurse
  }
  $control=Invoke-GateScenario $scenario $m.only $controlExe $controlDir 'control'
  if($control.ExitCode -eq 77 -and $control.Failed -eq 0){
   Write-Host "SKIP: control prerequisites unavailable: $controlKey; see $controlDir"
   $results+=@{name=$m.name;verdict='SKIP';reason='control prerequisite unavailable';directory=$controlDir}
   $results | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $root 'mutations.json')
   exit 77
  }
  if($control.ExitCode -ne 0 -or $control.Failed -ne 0 -or $control.Passed -le 0){
   throw "Control failed (never a kill): $controlKey, $($control.Failed) failed checks, exit $($control.ExitCode); see $controlDir"
  }
  $controls[$controlKey]=$control
  Write-Host "Control passed: $controlKey ($($control.Passed) checks)"
 }
 $dir=Join-Path $root $m.name;New-Item -ItemType Directory -Path $dir|Out-Null
 $src=Join-Path $repo $m.file
 $text=[IO.File]::ReadAllText($src)
 $count=Measure-WfAnchor $text $m.old
 if($count -ne 1){throw "Mutation edit count $count for $($m.name), expected 1; the anchor moved with the code"}
 $text=Edit-WfAnchor $text $m.old $m.new
 $base=Split-Path -Parent $src
 $text=[regex]::Replace($text,'#include "([^"]+)"',{param($match)
  $header=[IO.Path]::GetFullPath((Join-Path $base $match.Groups[1].Value))
  if(Test-Path -LiteralPath $header){'#include "'+$header.Replace('\','/')+'"'}else{$match.Value}
 })
 $mutantSrc=Join-Path $dir 'mutant.c';[IO.File]::WriteAllText($mutantSrc,$text)
 $exe=Join-Path $dir 'piero.exe'
 $saved=$ErrorActionPreference
 try{
  $ErrorActionPreference='Continue'
  & cmake -S $repo -B $buildRoot "-DWF_GATE_MUTANT_SOURCE=$mutantSrc" "-DWF_GATE_MUTANT_ORIGINAL=$($m.file)" "-DWF_GATE_MUTANT_OUTPUT=$dir" *> (Join-Path $dir 'configure.log')
  if($LASTEXITCODE -ne 0){throw 'Mutant configure failed, never a kill'}
  & cmake --build $buildRoot --target wf_gate_mutant --parallel 2 *> (Join-Path $dir 'build.log')
  $buildExit=$LASTEXITCODE
 }
 finally{$ErrorActionPreference=$saved}
 if($buildExit -ne 0 -or !(Test-Path -LiteralPath $exe)){throw "Mutant build failed, never a kill: $($m.name); see $dir\build.log"}
 $run=Invoke-GateScenario $scenario $m.only $exe $dir 'mutant'
 $verdict=if($run.ExitCode -eq 1 -and $run.Failed -gt 0){'KILLED'}else{'SURVIVED'}
 Write-Host "$($m.name): $verdict ($($run.Failed) failed checks, exit $($run.ExitCode), $($run.Seconds) s)"
 $csv=Get-ChildItem -LiteralPath $run.Evidence -Recurse -Filter results.csv -ErrorAction SilentlyContinue | Select-Object -First 1
 if($csv){Get-Content -LiteralPath $csv.FullName | Where-Object { $_ -match ',FAIL$' } | Select-Object -First 3 | ForEach-Object { Write-Host "    $($_.Split(',')[0])" }}
 $results+=@{name=$m.name;verdict=$verdict}
}
} finally {
 # A normal build must not depend on retained scratch source paths.
 $restoreSaved=$ErrorActionPreference
 try {
  $ErrorActionPreference='Continue'
  & cmake -S $repo -B $buildRoot '-DWF_GATE_MUTANT_SOURCE:FILEPATH=' '-DWF_GATE_MUTANT_ORIGINAL:STRING=' '-DWF_GATE_MUTANT_OUTPUT:PATH=' *> (Join-Path $root 'restore-configure.log')
  $restoreExit=$LASTEXITCODE
 } finally {$ErrorActionPreference=$restoreSaved}
 if($restoreExit -ne 0){throw "Cannot restore normal CMake configuration; see $root\restore-configure.log"}
}
if(!$results.Count){throw 'No gate mutants were evaluated'}
$survived=@($results | Where-Object { $_.verdict -ne 'KILLED' })
if($survived.Count){Write-Host ("SURVIVED: "+(($survived|ForEach-Object{$_.name}) -join ', '));Write-Host "kept: $root";exit 1}
Write-Host "Evidence retained: $root"
Write-Host "PASS: $($results.Count) gate mutants killed"
exit 0

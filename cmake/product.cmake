# Product and reusable C17 libraries. Windows API boundaries remain explicit.
# Embed the canonical project license, so the legal text never drifts or needs
# an external file at runtime. Keep the normal distribution copy below too.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/LICENSE")
file(READ "${PROJECT_SOURCE_DIR}/LICENSE" WF_PROJECT_LICENSE_C)
if(NOT WF_PROJECT_LICENSE_C MATCHES "^MIT License" OR
    NOT WF_PROJECT_LICENSE_C MATCHES "SOFTWARE\\.")
  message(FATAL_ERROR "The canonical MIT LICENSE is missing or incomplete")
endif()
string(REPLACE "\\" "\\\\" WF_PROJECT_LICENSE_C "${WF_PROJECT_LICENSE_C}")
string(REPLACE "\"" "\\\"" WF_PROJECT_LICENSE_C "${WF_PROJECT_LICENSE_C}")
string(REPLACE "\r" "\\r" WF_PROJECT_LICENSE_C "${WF_PROJECT_LICENSE_C}")
string(REPLACE "\n" "\\n\"\n\"" WF_PROJECT_LICENSE_C "${WF_PROJECT_LICENSE_C}")
set(WF_PROJECT_URL "https://github.com/cyan-cs/piero")
set(PIERO_VERSION "0.0.3-dev" CACHE STRING "piero version (SemVer)")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/tools/build_version.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/build_version.py"
  "${PIERO_VERSION}" RESULT_VARIABLE WF_BUILD_VERSION_RESULT)
if(NOT WF_BUILD_VERSION_RESULT STREQUAL "0")
  message(FATAL_ERROR "PIERO_VERSION must be a supported semantic version")
endif()
configure_file("${PROJECT_SOURCE_DIR}/cmake/version.h.in"
  "${PROJECT_BINARY_DIR}/generated/wf_version.h" @ONLY)
configure_file("${PROJECT_SOURCE_DIR}/cmake/project_info.h.in"
  "${PROJECT_BINARY_DIR}/generated/wf_project_info.h" @ONLY)

# Translation editing has one source of truth: UTF-8 INI.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/languages/en.ini" "${PROJECT_SOURCE_DIR}/languages/ja.ini"
  "${PROJECT_SOURCE_DIR}/tools/generate_i18n.py" "${PROJECT_SOURCE_DIR}/tools/language_catalog.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/generate_i18n.py"
  "${PROJECT_SOURCE_DIR}/languages" "${PROJECT_BINARY_DIR}/generated"
  RESULT_VARIABLE WF_I18N_GENERATED)
if(NOT WF_I18N_GENERATED EQUAL 0)
  message(FATAL_ERROR "Language INI validation/generation failed")
endif()
add_custom_target(wf_languages
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/languages"
    "${PROJECT_BINARY_DIR}/languages")

add_library(wfcore STATIC
  src/base/wf_status.c
  src/async/wf_workcore.c
  src/update/wf_update.c
  src/ui/wf_app_updates.c
  src/base/wf_buf.c
  src/base/wf_slotfile.c
  src/session/wf_session.c
  src/clipboard/wf_clipboard.c
  src/xfer/wf_batchplan.c
  src/xfer/wf_hash.c
  src/xfer/wf_bulkname.c
  src/process/wf_lock.c
  src/ui/wf_app_lock.c
  src/text/wf_utf.c
  src/text/wf_width.c
  src/text/wf_sanitize.c
  src/text/wf_markdown.c
  src/text/wf_mdinline.c
  src/render/wf_mddraw.c
  src/render/wf_frame.c
  src/render/wf_present.c
  src/render/wf_theme.c
  src/path/wf_path.c
  src/path/wf_name.c
  src/path/wf_smartgo_path.c
  src/config/wf_config.c
  src/config/wf_ini_edit.c
  src/config/wf_cli.c
  src/preview/wf_decode_wire.c
  src/preview/wf_pixmap.c
  src/render/wf_pixcell.c
  src/render/wf_term_caps.c
  src/render/wf_graphics.c
  src/render/wf_sixel.c
  src/preview/wf_preview.c
  src/preview/wf_table.c
  src/preview/wf_image_wire.c
  src/preview/wf_image_check.c
  src/xfer/wf_xfer.c
  src/xfer/wf_copyjob.c
  src/xfer/wf_preflight.c
  src/xfer/wf_preflight_paths.c
  src/xfer/wf_preflight_walk.c
  src/xfer/wf_xfer_paths.c
  src/xfer/wf_verify.c
  src/fs/wf_dir.c
  src/search/wf_fuzzy.c
  src/search/wf_query.c
  src/search/wf_treewalk.c
  src/search/wf_hits.c
  src/search/wf_dupes.c
  src/git/wf_gitenv.c
  src/git/wf_gitstatus.c
  src/git/wf_gitport.c
  src/text/wf_linediff.c
  src/fs/wf_loc.c
  src/fs/wf_savemodel.c
  src/fs/wf_sftp.c
  src/fs/wf_sftpc.c
  src/fs/wf_sftpplace_win.c
  src/fs/wf_sshproc_win.c
  src/fs/wf_sshcmd.c
  src/fs/wf_wsl_win.c
  src/fs/wf_pcplace_win.c
  src/fs/wf_ntfsfacts_win.c
  src/fs/wf_wslops_win.c
  src/fs/wf_wslrm_win.c
  src/archive/wf_vpath.c
  src/input/wf_stroke.c
  src/input/wf_commands.c
  src/input/wf_keymap.c
  src/input/wf_choice.c
  src/input/wf_exline.c
  src/ui/wf_app_keys.c
  src/ui/wf_app_cmdline.c
  src/ui/wf_app_sort.c
  src/ui/wf_app_undo.c
  src/ui/wf_app_backups.c
  src/ui/wf_app_dirsize.c
  src/ui/wf_app_session.c
  src/ui/wf_app_clip.c
  src/ui/wf_app_fuzzy.c
  src/ui/wf_app_hash.c
  src/ui/wf_app_hits.c
  src/ui/wf_app_git.c
  src/ui/wf_app_tabs.c
  src/ui/wf_app_batch.c
  src/ui/wf_dirsize.c
  src/ui/wf_app_help.c
  src/fs/wf_debounce.c
  src/edit/wf_etext.c
  src/edit/wf_eenc.c
  src/edit/wf_eundo.c
  src/edit/wf_esearch.c
  src/edit/wf_editor.c
  src/edit/wf_esyntax.c src/edit/wf_estsyntax.c
  src/edit/wf_ekeymap.c
  src/ui/wf_layout.c
  src/ui/wf_view.c
  src/ui/wf_sel.c
  src/ui/wf_app.c
  src/ui/wf_info.c
  src/ui/wf_app_nav.c
  src/ui/wf_app_preview.c
  src/ui/wf_dircache.c
  src/ui/wf_app_input.c
  src/ui/wf_app_render.c
  src/ui/wf_app_confirm.c
  src/ui/wf_app_xfer.c
  src/ui/wf_app_extract.c
  src/ui/wf_app_wsl.c
  src/ui/wf_app_mouse.c
  src/ui/wf_app_joblog.c
  src/ui/wf_app_retry.c
  src/ui/wf_app_open.c
  src/ui/wf_app_copy.c
  src/ui/wf_app_theme.c
  src/ui/wf_app_image.c
  src/ui/wf_app_go.c
  src/i18n/wf_i18n.c
  src/i18n/wf_language.c
  src/ui/wf_app_settings.c
  src/ui/wf_app_settings_draw.c
  src/ui/wf_app_settings_save.c
  src/ui/wf_app_settings_input.c
  src/ui/wf_app_settings_keys.c
  src/ui/wf_app_settings_theme.c
  src/ui/wf_app_about.c
  src/ui/wf_app_editor.c src/ui/wf_app_syntax.c
  src/syntax/wf_synlang.c src/syntax/wf_synruns.c src/syntax/wf_synoverlay.c
  src/fs/wf_launch_class.c)

target_include_directories(wfcore PUBLIC src)
target_link_libraries(wfcore PUBLIC user32)
target_include_directories(wfcore PUBLIC "${PROJECT_BINARY_DIR}/generated")
add_dependencies(wfcore wf_languages)

add_executable(piero src/main.c src/main_editor.c src/main_recovery.c src/main_session.c src/main_pump.c src/term/wf_term.c src/fs/wf_enum_win.c
  src/update/wf_update_win.c
  src/config/wf_config_win.c
  src/path/cwd_neutralize.c
  src/fs/wf_fsworker.c src/fs/wf_watch_win.c
  src/preview/wf_previewworker.c
  src/xfer/wf_probe_win.c src/xfer/wf_xferworker.c src/wfile.manifest)

target_link_libraries(piero PRIVATE wfstate wfoldstate wfarchive wfcore wfstorage wfdiagnostics wflaunch wfefile wfcopyengine wfsmartgo wfsession wfclip wflock wfhash wfgit wfdecode advapi32 delayimp)
target_link_libraries(piero PRIVATE winhttp)
target_include_directories(piero PRIVATE "${PROJECT_BINARY_DIR}/generated")

target_link_options(piero PRIVATE
  $<$<NOT:$<CONFIG:Debug>>:/DEPENDENTLOADFLAG:0x800>
  # pictures are optional: a program that never shows one must not pay to load
  # the WinRT, shell-stream and AppContainer libraries at start-up
  /DELAYLOAD:runtimeobject.dll /DELAYLOAD:shcore.dll /DELAYLOAD:shlwapi.dll
  /DELAYLOAD:userenv.dll)
target_link_options(piero PRIVATE /DELAYLOAD:winhttp.dll)

set(WF_OWNED_PROOF_SOURCES
  src/fs/owned_common.c src/fs/owned_payload.c src/fs/owned_committed.c
  src/fs/owned_local.c src/fs/owned_live.c src/fs/owned_log.c)
# Which state folder is used under %APPDATA% / %LOCALAPPDATA% (piero, else an
# existing wfile): no dependencies, shared by the program, drafts and the decoder.
add_library(wfstate STATIC src/base/wf_state_dir.c)
target_include_directories(wfstate PUBLIC src)
wf_product_warnings(wfstate)

add_library(wfintegrity STATIC src/xfer/wf_integrity_win.c)
target_include_directories(wfintegrity PUBLIC src)
target_link_libraries(wfintegrity PUBLIC bcrypt)

add_library(wfstorage STATIC src/fs/owned_delete.c src/fs/owned_integrity.c ${WF_OWNED_PROOF_SOURCES} src/path/wf_canonical_win.c
  src/recovery/wf_recovery.c src/recovery/wf_recovery_store.c)

target_include_directories(wfstorage PUBLIC src)

target_link_libraries(wfstorage PUBLIC wfintegrity bcrypt)

add_library(wfcopy STATIC src/xfer/wf_copy_source_win.c)
target_include_directories(wfcopy PUBLIC src)
target_link_libraries(wfcopy PUBLIC wfcore wfstorage)

# Open: ShellExecuteExW behind an identity pin, on its own STA worker.
add_library(wflaunch STATIC src/fs/wf_launch_win.c src/fs/wf_launchworker.c src/fs/wf_terminal_win.c)
target_include_directories(wflaunch PUBLIC src)
target_link_libraries(wflaunch PUBLIC wfcore wfstorage shell32 ole32)

add_library(wfedit STATIC EXCLUDE_FROM_ALL src/fs/wf_edit_win.c)

# Batch rename and same-volume batch move: the journal and the execution
# (DR-DRAFT-022). The plan and the pattern are in wfcore.
add_library(wfbatch STATIC EXCLUDE_FROM_ALL src/xfer/wf_batchlog_win.c src/xfer/wf_batchexec_win.c)
target_include_directories(wfbatch PUBLIC src)
target_link_libraries(wfbatch PUBLIC wfedit wfcore wfstorage bcrypt)

# The thread that runs a batch and recovers old journals, and the port the screen
# talks to it through (DR-DRAFT-022, UI). Linked into the product with the edit
# worker; the screen itself is in wfcore and knows only the port.
add_library(wfbatchworker STATIC EXCLUDE_FROM_ALL src/xfer/wf_batchworker.c)
target_include_directories(wfbatchworker PUBLIC src)
target_link_libraries(wfbatchworker PUBLIC wfbatch wfcore)

target_link_libraries(wfedit PUBLIC wfcore wfstorage)

# Read-back verification stage (policy + stages in wfcore; this glues the
# existing integrity and ownership code to it). Linked through wfcopyengine.
add_library(wfverify STATIC EXCLUDE_FROM_ALL src/xfer/wf_verify_win.c)
target_include_directories(wfverify PUBLIC src)
target_link_libraries(wfverify PUBLIC wfcore wfintegrity wfstorage)

# Create/rename execution. Built for the tests always; linked into the product
# unless -DWF_PRODUCT_EDIT=OFF (the read-only variant, see CMakeLists.txt).
add_library(wfeditworker STATIC EXCLUDE_FROM_ALL src/fs/wf_editworker.c)
target_include_directories(wfeditworker PUBLIC src)
target_link_libraries(wfeditworker PUBLIC wfedit wfcore)
if(WF_PRODUCT_EDIT)
  target_compile_definitions(piero PRIVATE WF_PRODUCT_EDIT)
  target_link_libraries(piero PRIVATE wfeditworker wfbatchworker)
endif()

# Picture and PDF decoders (WIC, Windows.Data.Pdf). They run inside the sandboxed
# decoder host; the application process only links them to host them.
add_library(wfdecode STATIC src/preview/wf_decode_win.c src/preview/wf_pdf_win.c
  src/preview/wf_dechost_win.c src/preview/wf_dechost_client_win.c
  src/preview/wf_imageworker.c)
target_include_directories(wfdecode PUBLIC src)
target_link_libraries(wfdecode PUBLIC wfstate wfcore wfarchive windowscodecs runtimeobject shcore shlwapi ole32 uuid userenv advapi32 bcrypt shell32)

add_library(wfdiagnostics STATIC src/diagnostics/wf_diag.c
  src/diagnostics/wf_diag_store.c src/diagnostics/wf_diag_validate.c
  src/diagnostics/wf_diag_format.c)

target_link_libraries(wfdiagnostics PUBLIC wfcore wfstorage advapi32)

wf_product_warnings(wfcore wfdecode wflaunch piero wfintegrity wfstorage wfcopy wfedit wfeditworker wfverify wfdiagnostics wfbatch wfbatchworker)

# Receipts for what a writer moved aside or created, inventoried for manual
# recovery at the next start. Shared by the copy engine and the editor's save.
add_library(wfjournal STATIC src/xfer/wf_copyjournal_win.c)
target_include_directories(wfjournal PUBLIC src)
target_link_libraries(wfjournal PUBLIC wfcore wfstorage)
wf_product_warnings(wfjournal)

# A read-only look at a state folder's recovery information (the older wfile
# folder when piero exists too): journal store, receipt inventory, drafts.
add_library(wfoldstate STATIC src/recovery/wf_old_state.c)
target_include_directories(wfoldstate PUBLIC src)
target_link_libraries(wfoldstate PUBLIC wfjournal wfstorage)
wf_product_warnings(wfoldstate)

# Reading archives: the index of ZIP / 7z / TAR / single compressed files, and
# the first bytes of a file in one, on libarchive (src/third_party).
add_library(wfarchive STATIC src/archive/wf_archive.c)
target_include_directories(wfarchive PUBLIC src)
target_link_libraries(wfarchive PUBLIC wfcore wf_libarchive)
wf_product_warnings(wfarchive)

# The editor's file access: load with checks, save by verified replacement.
add_library(wfefile STATIC src/async/wf_blocking.c src/edit/wf_edraft_win.c
  src/edit/wf_efile_win.c
  src/edit/wf_eio_win.c src/edit/wf_edraftw_win.c
  src/edit/wf_esession.c src/edit/wf_evim.c src/edit/wf_eview.c src/edit/wf_emdpane.c
  src/edit/wf_egitpane.c src/config/wf_config_save_win.c)
target_include_directories(wfefile PUBLIC src)
target_link_libraries(wfefile PUBLIC wfstate wfjournal wfcore wfstorage bcrypt user32 advapi32 shell32)
wf_product_warnings(wfefile)

# The copy engine: verified copies by temporary + no-clobber rename, a journal
# for manual crash recovery. Cross-volume Move verifies before recycling the source.
add_library(wfcopyengine STATIC src/xfer/wf_copyengine_win.c
  src/xfer/wf_extract_win.c
  src/xfer/wf_recycle_win.c src/xfer/wf_copyworker.c)
target_include_directories(wfcopyengine PUBLIC src)
target_link_libraries(wfcopyengine PUBLIC wfjournal wfcore wfstorage wfverify wfintegrity wfarchive ole32 shell32 uuid bcrypt)
wf_product_warnings(wfcopyengine)

# Smart Go: the candidate worker (bounded batches) and the machine's
# side of path typing (profile folder, known folders, environment). The folder
# size worker (D) is the same kind of folder reader and lives beside it.
add_library(wfsmartgo STATIC src/fs/wf_candworker.c src/fs/wf_sizeworker.c
  src/search/wf_fuzzyworker.c
  src/search/wf_searchworker.c
  src/path/wf_smartgo_env_win.c)
target_include_directories(wfsmartgo PUBLIC src)
target_link_libraries(wfsmartgo PUBLIC wfcore shell32 ole32 uuid)
wf_product_warnings(wfsmartgo)

# The Windows clipboard's files (V): the worker that reads and publishes them on its own STA
# thread. The pure parsing of the clipboard's blocks is in wfcore.
add_library(wfclip STATIC src/clipboard/wf_clipworker_win.c
  src/clipboard/wf_dataobj_files_win.c)
target_include_directories(wfclip PUBLIC src)
target_link_libraries(wfclip PUBLIC wfcore ole32 shell32 uuid user32)
wf_product_warnings(wfclip)

# Who has a file open (L): Restart Manager, closing and ending one program, and
# the worker that runs them off the screen's thread (DR-DRAFT-024). The policy and
# the data are pure and live in wfcore (wf_lock.c).
add_library(wflock STATIC src/process/wf_restart_manager_win.c
  src/process/wf_process_control_win.c src/process/wf_lockworker_win.c)
target_include_directories(wflock PUBLIC src)
target_link_libraries(wflock PUBLIC wfcore rstrtmgr advapi32 user32)
wf_product_warnings(wflock)

# The SHA-256 of a file on request (`#`): the streaming hash and the worker that
# runs it off the screen's thread (DR-DRAFT-027). The types and the hex form are
# in wfcore (wf_hash.c).
add_library(wfhash STATIC src/xfer/wf_sha256_win.c src/xfer/wf_hashworker.c
  src/search/wf_dupehash_win.c)
target_include_directories(wfhash PUBLIC src)
target_link_libraries(wfhash PUBLIC wfcore bcrypt)
wf_product_warnings(wfhash)

# Git through the Git of the environment that owns the repository (DR-DRAFT-038):
# the process runner and the two backends. The status table, the line diff and the
# classification of paths are in wfcore.
add_library(wfgit STATIC src/git/wf_gitproc_win.c src/git/wf_gitbackend_win.c src/git/wf_gitworker.c)
target_include_directories(wfgit PUBLIC src)
target_link_libraries(wfgit PUBLIC wflaunch wfcore)
wf_product_warnings(wfgit)

# `piero --continue`: the saved session's store with its lock, and the writer
# thread. The text form (wf_session.c) is in wfcore, which the screen uses.
# Nothing here knows the screen.
add_library(wfsession STATIC src/session/wf_session_store.c
  src/session/wf_session_writer.c)
target_include_directories(wfsession PUBLIC src)
target_link_libraries(wfsession PUBLIC wfcore)
wf_product_warnings(wfsession)

# The licences of what is linked in travel with the executable.
add_custom_command(TARGET piero POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/LICENSES" "$<TARGET_FILE_DIR:piero>/LICENSES"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/LICENSE" "$<TARGET_FILE_DIR:piero>/LICENSE"
  VERBATIM)
# A notice-only change must refresh the files beside an existing executable,
# too; POST_BUILD alone would otherwise leave stale third-party legal text.
file(GLOB WF_DISTRIBUTION_LICENSE_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/LICENSES/*.txt")
set_property(TARGET piero APPEND PROPERTY LINK_DEPENDS
  "${CMAKE_SOURCE_DIR}/LICENSE" ${WF_DISTRIBUTION_LICENSE_FILES})
if(WF_SANITIZE)
  wf_prepare_sanitizer_runtime()
  wf_use_sanitizer_runtime(piero)
  target_sources(wfdecode PRIVATE src/preview/wf_sanitizer_stage_win.c)
  target_compile_definitions(wfdecode PRIVATE WF_SANITIZER_STAGE)
  target_include_directories(wfdecode PRIVATE "${PROJECT_BINARY_DIR}/generated")
endif()

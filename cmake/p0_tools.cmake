# Isolated measurement tools, excluded from BUILD_TESTING=OFF builds.
add_library(p0_common STATIC tools/p0/p0_common.c tools/p0/p0_area.c)

target_link_libraries(p0_common PUBLIC bcrypt advapi32)

add_library(p0_lab STATIC tools/p0/p0_lab.c)
add_library(p0_runner STATIC tools/p0/wf_run_win.c)
target_include_directories(p0_runner PUBLIC tools/p0)
add_executable(p0_run tools/p0/p0_run.c)
target_link_libraries(p0_run PRIVATE p0_runner shell32)

target_link_libraries(p0_lab PUBLIC p0_common)

add_executable(p0_integrity_bench tools/p0/p0_integrity_bench.c)
target_link_libraries(p0_integrity_bench PRIVATE wfintegrity p0_lab psapi)

add_library(p0_source_guard STATIC tools/p0/p0_source_guard.c)

target_link_libraries(p0_source_guard PUBLIC wfcore)

add_executable(p0_s08s10 tools/p0/p0_s08s10.c src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)

target_link_libraries(p0_s08s10 PRIVATE p0_source_guard p0_lab)

add_executable(p0_copy_callback tools/p0/p0_copy_callback.c)

target_link_libraries(p0_copy_callback PRIVATE p0_common)

add_executable(p0_stop_repro tools/p0/p0_stop_repro.c)

target_link_libraries(p0_stop_repro PRIVATE p0_common)

add_library(p0_journal STATIC tools/p0/p0_journal.c)

target_link_libraries(p0_journal PUBLIC p0_common wfstorage)

add_executable(p0_copy_op tools/p0/p0_copy_op.c src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)

target_link_libraries(p0_copy_op PRIVATE p0_journal p0_source_guard wfcopy)

add_executable(p0_s02s03 tools/p0/p0_s02s03.c)

target_link_libraries(p0_s02s03 PRIVATE p0_journal)

add_executable(p0_commit_rename tools/p0/p0_commit_rename.c)

target_link_libraries(p0_commit_rename PRIVATE p0_common)

add_executable(p0_area_reap tools/p0/p0_area_reap.c)

add_executable(p0_scratch_sweep tools/p0/p0_scratch_sweep.c)
target_link_libraries(p0_scratch_sweep PRIVATE p0_common)

target_link_libraries(p0_area_reap PRIVATE p0_common)

add_executable(p0_terminal tools/p0/p0_terminal.c src/term/wf_term.c)

target_link_libraries(p0_terminal PRIVATE p0_common wfcore)

add_library(p0_canary SHARED tools/p0/p0_canary_dll.c)

add_executable(p0_image_sandbox tools/p0/p0_image_sandbox.c)

add_dependencies(p0_image_sandbox p0_canary)

target_link_libraries(p0_image_sandbox PRIVATE p0_common wfcore userenv ole32 windowscodecs uuid shell32 ws2_32 wintrust)

add_executable(p0_env tools/p0/p0_env.c)

target_link_libraries(p0_env PRIVATE p0_common)

add_executable(p0_conpty_host tools/p0/p0_conpty_host.c)

target_link_libraries(p0_conpty_host PRIVATE p0_common p0_lab wfstorage wfcore wflock advapi32 ole32 shell32 user32 userenv rstrtmgr)
target_compile_definitions(p0_conpty_host PRIVATE
  "WF_TEST_ARCHIVE_DIR=\"${CMAKE_SOURCE_DIR}/tests/archive\"")
add_dependencies(p0_conpty_host open_target)
if(WF_PRODUCT_EDIT)
  target_compile_definitions(p0_conpty_host PRIVATE WF_PRODUCT_EDIT)
endif()

add_executable(p0_recovery_suite tools/p0/p0_recovery_suite.c)

target_link_libraries(p0_recovery_suite PRIVATE p0_journal p0_lab)

# Measures the cost of browsing archives (tools/make_archive_bench.py makes the
# input). A tool, not a test: it prints a table.
add_executable(bench_archive EXCLUDE_FROM_ALL tools/bench_archive.c)
target_link_libraries(bench_archive PRIVATE wfarchive psapi)

add_executable(bench_sixel EXCLUDE_FROM_ALL tools/bench_sixel.c)
target_link_libraries(bench_sixel PRIVATE wfdecode psapi)

add_executable(bench_review EXCLUDE_FROM_ALL tools/bench_review.c
  src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)
target_include_directories(bench_review PRIVATE src tools)
target_link_libraries(bench_review PRIVATE wfefile wfcopyengine p0_lab psapi)

add_executable(probe_appcontainer EXCLUDE_FROM_ALL tools/probe_appcontainer.c)
target_link_libraries(probe_appcontainer PRIVATE userenv shell32 ole32 advapi32)

add_executable(probe_copy_environment EXCLUDE_FROM_ALL tools/probe_copy_environment.c
  src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)
target_include_directories(probe_copy_environment PRIVATE src tools)
target_link_libraries(probe_copy_environment PRIVATE wfcopyengine p0_lab)
if(WF_SANITIZE)
  wf_use_sanitizer_runtime(probe_copy_environment)
endif()

add_executable(bench_fuzzy EXCLUDE_FROM_ALL tools/bench_fuzzy.c)
target_include_directories(bench_fuzzy PRIVATE src)
target_compile_definitions(bench_fuzzy PRIVATE WF_BENCH_CONFIGURATION="$<CONFIG>")
target_link_libraries(bench_fuzzy PRIVATE wfsmartgo psapi)

# The batch rename engine against the same number of single renames (DR-DRAFT-022):
# wall and CPU time, peak working set, handles, the engine's call counts. Not run by
# CTest: it creates up to 10,000 files in the temporary directory.
add_executable(bench_batch EXCLUDE_FROM_ALL tools/bench_batch.c)
target_include_directories(bench_batch PRIVATE src)
target_compile_definitions(bench_batch PRIVATE WF_BENCH_CONFIGURATION="$<CONFIG>")
target_link_libraries(bench_batch PRIVATE wfbatch psapi)

add_executable(bench_entry_layout EXCLUDE_FROM_ALL tools/bench_entry_layout.c)
target_include_directories(bench_entry_layout PRIVATE src)
target_link_libraries(bench_entry_layout PRIVATE wfcore psapi)

add_executable(bench_copy EXCLUDE_FROM_ALL tools/bench_copy.c
  src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)
target_include_directories(bench_copy PRIVATE src tools)
target_link_libraries(bench_copy PRIVATE wfcopyengine p0_lab)

add_executable(probe_wsl EXCLUDE_FROM_ALL tools/probe_wsl.c)

add_executable(probe_legacy_journal EXCLUDE_FROM_ALL tools/probe_legacy_journal.c
  src/xfer/wf_copyjournal_legacy.c)
target_include_directories(probe_legacy_journal PRIVATE src)
target_link_libraries(probe_legacy_journal PRIVATE wfcore)

# The engine itself is compiled into this probe (wrapped calls), so it is not linked.
add_executable(probe_sharing_stress EXCLUDE_FROM_ALL tools/probe_sharing_stress.c
  src/xfer/wf_copyjournal_win.c src/xfer/wf_recycle_win.c
  src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)
target_include_directories(probe_sharing_stress PRIVATE src tools)
target_link_libraries(probe_sharing_stress PRIVATE wfcore wfstorage wfverify wfintegrity
  p0_lab p0_common ole32 shell32 uuid bcrypt rstrtmgr)
if(WF_SANITIZE)
  wf_use_sanitizer_runtime(probe_sharing_stress)
endif()

add_executable(probe_decoder_start EXCLUDE_FROM_ALL tools/probe_decoder_start.c)
target_include_directories(probe_decoder_start PRIVATE src)
target_link_libraries(probe_decoder_start PRIVATE wfdecode)
add_executable(probe_safety_state EXCLUDE_FROM_ALL tools/probe_safety_state.c src/config/wf_config_win.c)
target_link_libraries(probe_safety_state PRIVATE wfstate wfjournal wfstorage wfcore)
add_executable(wic_proto EXCLUDE_FROM_ALL tools/proto/wic_proto.c)
target_link_libraries(wic_proto PRIVATE windowscodecs ole32 uuid propsys)
add_executable(bench_dircache EXCLUDE_FROM_ALL tools/bench_dircache.c)
target_compile_definitions(bench_dircache PRIVATE WF_BENCH_DIRCACHE)
target_link_libraries(bench_dircache PRIVATE wfcore psapi)
add_executable(bench_dir_enum EXCLUDE_FROM_ALL tools/bench_dir_enum.c src/fs/wf_enum_win.c)
target_link_libraries(bench_dir_enum PRIVATE wfcore psapi)

# Explicit diagnostics for access-denied environments; not product executables.
add_executable(diagnose_preview_open EXCLUDE_FROM_ALL tools/diagnose_preview_access.c)
target_link_libraries(diagnose_preview_open PRIVATE wfdecode)
add_executable(diagnose_preview_stage EXCLUDE_FROM_ALL tools/diagnose_preview_access.c)
target_compile_definitions(diagnose_preview_stage PRIVATE WF_DIAG_STAGE)
target_link_libraries(diagnose_preview_stage PRIVATE wfdecode p0_lab)

add_executable(probe_sixel_terminal EXCLUDE_FROM_ALL tools/probe_sixel_terminal.c src/term/wf_term.c)
target_link_libraries(probe_sixel_terminal PRIVATE wfcore)

# Same product sources/options, with the traced client object resolving before
# the static library. Logging requires an explicit WF_SIXEL_ACCESS_LOG path.
get_target_property(WF_DIAG_PRODUCT_SOURCES piero SOURCES)
add_executable(wfile_access_diagnostic EXCLUDE_FROM_ALL
  ${WF_DIAG_PRODUCT_SOURCES} tools/trace_preview_client.c)
add_dependencies(wfile_access_diagnostic wf_syntax_pin wf_grammar_pack)
if(WF_SANITIZE)
  # Link options are copied below; the runtime staging dependency is separate.
  add_dependencies(wfile_access_diagnostic wf_sanitizer_runtime)
endif()
foreach(property COMPILE_DEFINITIONS COMPILE_OPTIONS INCLUDE_DIRECTORIES LINK_OPTIONS)
  get_target_property(WF_DIAG_VALUE piero ${property})
  if(WF_DIAG_VALUE)
    set_property(TARGET wfile_access_diagnostic PROPERTY ${property} "${WF_DIAG_VALUE}")
  endif()
endforeach()
get_target_property(WF_DIAG_PRODUCT_LIBS piero LINK_LIBRARIES)
target_link_libraries(wfile_access_diagnostic PRIVATE ${WF_DIAG_PRODUCT_LIBS})

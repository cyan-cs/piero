# Unit, real-filesystem, fault, evidence-integrity and mutation tests.
add_executable(test_updates tests/test_updates.c)
target_include_directories(test_updates PRIVATE tests)
target_link_libraries(test_updates PRIVATE wfcore)
add_test(NAME updates COMMAND test_updates)
add_executable(test_update_worker tests/test_update_worker.c)
target_include_directories(test_update_worker PRIVATE tests src)
target_link_libraries(test_update_worker PRIVATE wfcore advapi32)
add_test(NAME update_worker COMMAND test_update_worker)
set_tests_properties(update_worker PROPERTIES LABELS non-destructive TIMEOUT 30)
set_tests_properties(updates PROPERTIES LABELS non-destructive)
add_executable(test_installer_path tests/test_installer_path.c tools/path_entry.c)
target_include_directories(test_installer_path PRIVATE tests tools)
add_test(NAME installer_path COMMAND test_installer_path)
set_tests_properties(installer_path PROPERTIES LABELS non-destructive)
add_executable(test_installer_path_fault tests/test_installer_path_fault.c)
target_include_directories(test_installer_path_fault PRIVATE tests tools)
add_test(NAME installer_path_fault COMMAND test_installer_path_fault)
set_tests_properties(installer_path_fault PROPERTIES LABELS non-destructive TIMEOUT 20)
add_executable(test_installer_directory tests/test_installer_directory.c tools/path_entry.c)
target_include_directories(test_installer_directory PRIVATE tests tools)
target_link_libraries(test_installer_directory PRIVATE advapi32 user32)
add_test(NAME installer_directory COMMAND test_installer_directory)
set_tests_properties(installer_directory PROPERTIES LABELS non-destructive TIMEOUT 20)
add_executable(test_owned_record tests/test_owned_record.c)
target_link_libraries(test_owned_record PRIVATE wfstorage)
target_include_directories(test_owned_record PRIVATE tests)
add_test(NAME owned_record COMMAND test_owned_record)

foreach(t text frame layout path dir sel debounce confirm app xfer slot preflight_batch
    name config preview preview_app image_wire navigation_memory operations_ui path_bar info edit_ui verify image_check move_ui copy_ui listing_perf theme ini_edit color_depth theme_ui smartgo_path launch_class open_ui terminal_ui dir_preview etext eenc editor esyntax ekeymap i18n i18n_ui archive_ui keymap vimkeys exline pixmap pixcell sixel term_caps graphics decode_wire image_ui native_cursor choice loc sftp sshcmd wsl_ui sftpc savemodel mouse_ui retry dirsize_ui slotfile session_ui clip_ui batchplan bulkname fuzzy fuzzy_ui batch_ui ntfsfacts table hash_ui tabs query treewalk hits_ui dupes git_ui pcplace)
  add_executable(test_${t} tests/test_${t}.c)
  target_link_libraries(test_${t} PRIVATE wfcore)
  target_include_directories(test_${t} PRIVATE tests)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()
add_executable(test_keymap_alloc tests/test_keymap_alloc.c)
target_link_libraries(test_keymap_alloc PRIVATE wfcore)
target_include_directories(test_keymap_alloc PRIVATE tests)
add_test(NAME keymap_alloc COMMAND test_keymap_alloc)
foreach(t batchlog batchexec)
  add_executable(test_${t} tests/test_${t}.c)
  target_link_libraries(test_${t} PRIVATE wfbatch)
  target_include_directories(test_${t} PRIVATE tests)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()
set_tests_properties(batchexec PROPERTIES TIMEOUT 300)
foreach(t markdown markdown_ui markdown_alloc)
  add_executable(test_${t} tests/test_${t}.c)
  target_link_libraries(test_${t} PRIVATE wfefile)
  target_include_directories(test_${t} PRIVATE tests)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()
# About tests read the canonical LICENSE and bundled notices; no temporary
# files, state mutations or fixed names in the user's temporary directory.
set_tests_properties(theme_ui PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" TIMEOUT 120)

add_executable(test_decode_win tests/test_decode_win.c)
target_link_libraries(test_decode_win PRIVATE wfdecode)
target_include_directories(test_decode_win PRIVATE tests)
add_test(NAME decode_win COMMAND test_decode_win)

# A picture is untrusted input (DR-DRAFT-039): hostile files, damage, big pictures.
add_executable(test_image_hostile tests/test_image_hostile.c)
target_link_libraries(test_image_hostile PRIVATE wfdecode psapi)
target_include_directories(test_image_hostile PRIVATE tests)
add_test(NAME image_hostile COMMAND test_image_hostile)
set_tests_properties(image_hostile PROPERTIES TIMEOUT 300)

add_executable(test_image_hostile_host tests/test_image_hostile_host.c)
target_link_libraries(test_image_hostile_host PRIVATE wfdecode)
target_include_directories(test_image_hostile_host PRIVATE tests)
add_dependencies(test_image_hostile_host wf_dechost)
add_test(NAME image_hostile_host COMMAND test_image_hostile_host)
set_tests_properties(image_hostile_host PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

add_executable(wf_dechost tests/dechost_main.c)
target_link_libraries(wf_dechost PRIVATE wfdecode)
if(WF_SANITIZE)
  wf_use_sanitizer_runtime(wf_dechost)
  add_executable(test_sanitizer_stage tests/test_sanitizer_stage.c)
  target_include_directories(test_sanitizer_stage PRIVATE tests)
  target_link_libraries(test_sanitizer_stage PRIVATE wfdecode p0_lab)
  wf_use_sanitizer_runtime(test_sanitizer_stage)
  add_test(NAME sanitizer_bundle_files COMMAND test_sanitizer_stage)
  add_test(NAME sanitizer_decoder_loader COMMAND test_sanitizer_stage $<TARGET_FILE:piero>)
  add_executable(test_sanitizer_stage_faults tests/test_sanitizer_stage_faults.c)
  target_include_directories(test_sanitizer_stage_faults PRIVATE tests "${PROJECT_BINARY_DIR}/generated")
  target_link_libraries(test_sanitizer_stage_faults PRIVATE p0_lab bcrypt)
  wf_use_sanitizer_runtime(test_sanitizer_stage_faults)
  add_test(NAME sanitizer_bundle_faults COMMAND test_sanitizer_stage_faults)
  set_tests_properties(sanitizer_bundle_files sanitizer_decoder_loader sanitizer_bundle_faults PROPERTIES TIMEOUT 45)
endif()
add_executable(terminal_probe tests/terminal_probe.c)
target_link_libraries(terminal_probe PRIVATE shell32)
add_executable(test_terminal tests/test_terminal.c)
target_link_libraries(test_terminal PRIVATE wflaunch wfcore shell32)
add_dependencies(test_terminal terminal_probe)
target_include_directories(test_terminal PRIVATE tests)
add_test(NAME terminal COMMAND test_terminal)
add_executable(test_dechost_win tests/test_dechost_win.c)
target_link_libraries(test_dechost_win PRIVATE wfdecode)
target_include_directories(test_dechost_win PRIVATE tests)
add_dependencies(test_dechost_win wf_dechost)
add_test(NAME dechost_win COMMAND test_dechost_win)
set_tests_properties(dechost_win PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_imageworker_win tests/test_imageworker_win.c)
target_link_libraries(test_imageworker_win PRIVATE wfdecode p0_lab)
target_include_directories(test_imageworker_win PRIVATE tests)
add_dependencies(test_imageworker_win wf_dechost)
add_test(NAME imageworker_win COMMAND test_imageworker_win)

add_executable(test_imageworker_async tests/test_imageworker_async.c)
target_include_directories(test_imageworker_async PRIVATE tests)
target_link_libraries(test_imageworker_async PRIVATE wfdecode)
add_test(NAME imageworker_async COMMAND test_imageworker_async)
set_tests_properties(imageworker_async PROPERTIES TIMEOUT 20)

add_executable(test_integrity_win tests/test_integrity_win.c)
target_link_libraries(test_integrity_win PRIVATE wfintegrity p0_lab)
target_include_directories(test_integrity_win PRIVATE tests)
add_test(NAME integrity_win COMMAND test_integrity_win)
add_executable(test_copy_source_win tests/test_copy_source_win.c src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)
target_link_libraries(test_copy_source_win PRIVATE wfcopy p0_source_guard p0_lab)
target_include_directories(test_copy_source_win PRIVATE tests)
add_test(NAME copy_source_win COMMAND test_copy_source_win)
set_tests_properties(copy_source_win PROPERTIES TIMEOUT 30)
add_executable(test_copy_source_faults tests/test_copy_source_faults.c src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)
target_link_libraries(test_copy_source_faults PRIVATE wfstorage wfcore p0_source_guard p0_lab)
target_include_directories(test_copy_source_faults PRIVATE tests)
add_test(NAME copy_source_faults COMMAND test_copy_source_faults)
set_tests_properties(copy_source_faults PROPERTIES TIMEOUT 30)
add_executable(test_integrity_faults tests/test_integrity_faults.c)
target_link_libraries(test_integrity_faults PRIVATE p0_lab)
target_include_directories(test_integrity_faults PRIVATE src tests)
add_test(NAME integrity_faults COMMAND test_integrity_faults)
add_executable(test_owned_integrity tests/test_owned_integrity.c)
target_link_libraries(test_owned_integrity PRIVATE wfstorage p0_lab p0_journal)
target_include_directories(test_owned_integrity PRIVATE tests)
add_test(NAME owned_integrity COMMAND test_owned_integrity)
add_test(NAME integrity_batch COMMAND p0_integrity_bench)
set_tests_properties(integrity_batch PROPERTIES TIMEOUT 120)

# Synthetic Win32 input records exercise the actual terminal translator.
add_executable(test_term_input tests/test_term_input.c)
target_link_libraries(test_term_input PRIVATE wfcore)
target_include_directories(test_term_input PRIVATE tests)
add_test(NAME term_input COMMAND test_term_input)
add_executable(test_term_edit_input tests/test_term_edit_input.c)
target_link_libraries(test_term_edit_input PRIVATE wfcore)
target_include_directories(test_term_edit_input PRIVATE tests)
add_test(NAME term_edit_input COMMAND test_term_edit_input)

# Real-filesystem test: needs the Win32 enumerator, so it is not in wfcore.
add_executable(test_enum_win tests/test_enum_win.c src/fs/wf_enum_win.c)
target_link_libraries(test_enum_win PRIVATE wfcore p0_common)
target_include_directories(test_enum_win PRIVATE tests)
add_test(NAME enum_win COMMAND test_enum_win)

add_executable(test_async_win tests/test_async_win.c src/fs/wf_enum_win.c
  src/fs/wf_fsworker.c src/fs/wf_watch_win.c)
target_link_libraries(test_async_win PRIVATE wfarchive wfcore)
target_include_directories(test_async_win PRIVATE tests)
add_test(NAME async_win COMMAND test_async_win)

add_executable(test_xfer_win tests/test_xfer_win.c src/fs/wf_enum_win.c
  src/xfer/wf_probe_win.c src/xfer/wf_xferworker.c)
target_link_libraries(test_xfer_win PRIVATE wfcore p0_lab)
target_include_directories(test_xfer_win PRIVATE tests)
add_test(NAME xfer_win COMMAND test_xfer_win)
set_tests_properties(xfer_win PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_source_guard_win tests/test_source_guard_win.c src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)
target_include_directories(test_source_guard_win PRIVATE tests)
target_link_libraries(test_source_guard_win PRIVATE p0_source_guard p0_lab)
add_test(NAME source_guard_win COMMAND test_source_guard_win)
set_tests_properties(source_guard_win PROPERTIES TIMEOUT 60)
add_executable(test_source_guard_faults tests/test_source_guard_faults.c src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)
target_link_libraries(test_source_guard_faults PRIVATE wfcore p0_lab)
target_include_directories(test_source_guard_faults PRIVATE tests)
add_test(NAME source_guard_faults COMMAND test_source_guard_faults)
set_tests_properties(source_guard_faults PROPERTIES TIMEOUT 60)

add_test(NAME p0_s08s10 COMMAND p0_s08s10)
set_tests_properties(p0_s08s10 PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)

add_executable(test_p0_area tests/test_p0_area.c)
target_link_libraries(test_p0_area PRIVATE p0_common)
target_include_directories(test_p0_area PRIVATE tests)
add_test(NAME p0_area COMMAND test_p0_area)

add_executable(test_p0_area_close tests/test_p0_area_close.c)
target_link_libraries(test_p0_area_close PRIVATE p0_common)
target_include_directories(test_p0_area_close PRIVATE tests)
add_test(NAME p0_area_close COMMAND test_p0_area_close)

add_executable(test_p0_permissions tests/test_p0_permissions.c)
target_link_libraries(test_p0_permissions PRIVATE p0_journal)
target_include_directories(test_p0_permissions PRIVATE tests)
add_test(NAME p0_permissions COMMAND test_p0_permissions)

add_executable(test_e2e_ui_bounds tests/test_e2e_ui_bounds.c)
target_include_directories(test_e2e_ui_bounds PRIVATE tests)
target_link_libraries(test_e2e_ui_bounds PRIVATE user32 gdi32 ole32 oleaut32 uuid dwmapi)
add_test(NAME e2e_ui_bounds COMMAND test_e2e_ui_bounds)

add_executable(test_storage_win tests/test_storage_win.c)
target_link_libraries(test_storage_win PRIVATE wfstorage p0_journal)
target_include_directories(test_storage_win PRIVATE tests)
add_test(NAME storage_win COMMAND test_storage_win)
set_tests_properties(storage_win PROPERTIES TIMEOUT 60)

# Test-only Win32 fault boundary; no injectable flags enter production.
add_executable(test_owned_faults tests/test_owned_faults.c)
add_library(owned_fault_proofs STATIC ${WF_OWNED_PROOF_SOURCES})
target_include_directories(owned_fault_proofs PRIVATE src)
target_compile_options(owned_fault_proofs PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/tests/wf_owned_fault_shim.h)
target_include_directories(test_owned_faults PRIVATE src tests)
target_link_libraries(test_owned_faults PRIVATE owned_fault_proofs p0_journal)
add_test(NAME owned_faults COMMAND test_owned_faults)
set_tests_properties(owned_faults PROPERTIES TIMEOUT 60)

add_executable(test_edit_win tests/test_edit_win.c src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)
target_link_libraries(test_edit_win PRIVATE wfedit p0_lab)
target_include_directories(test_edit_win PRIVATE tests)
add_test(NAME edit_win COMMAND test_edit_win)
add_executable(test_editworker_win tests/test_editworker_win.c src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)
target_link_libraries(test_editworker_win PRIVATE wfeditworker p0_lab)
target_include_directories(test_editworker_win PRIVATE tests)
add_test(NAME editworker_win COMMAND test_editworker_win)
set_tests_properties(editworker_win PROPERTIES TIMEOUT 60)
add_executable(test_verify_win tests/test_verify_win.c)
target_link_libraries(test_verify_win PRIVATE wfverify p0_lab)
target_include_directories(test_verify_win PRIVATE tests)
add_test(NAME verify_win COMMAND test_verify_win)
set_tests_properties(verify_win PROPERTIES TIMEOUT 120)
add_executable(test_preview_win tests/test_preview_win.c src/preview/wf_previewworker.c)
target_link_libraries(test_preview_win PRIVATE wfarchive wfcore p0_lab)
target_include_directories(test_preview_win PRIVATE tests)
add_test(NAME preview_win COMMAND test_preview_win)
add_executable(test_table_win tests/test_table_win.c src/preview/wf_previewworker.c)
target_link_libraries(test_table_win PRIVATE wfarchive wfcore p0_lab)
target_include_directories(test_table_win PRIVATE tests)
add_test(NAME table_win COMMAND test_table_win)
add_executable(test_sha256 tests/test_sha256.c)
target_link_libraries(test_sha256 PRIVATE wfhash)
target_include_directories(test_sha256 PRIVATE tests)
add_test(NAME sha256 COMMAND test_sha256)
add_executable(test_searchworker_win tests/test_searchworker_win.c src/fs/wf_enum_win.c)
target_link_libraries(test_searchworker_win PRIVATE wfsmartgo wfhash wfcore p0_common)
target_include_directories(test_searchworker_win PRIVATE tests)
add_test(NAME searchworker_win COMMAND test_searchworker_win)
set_tests_properties(searchworker_win PROPERTIES TIMEOUT 600)
set_tests_properties(sha256 PROPERTIES TIMEOUT 300)
add_executable(test_config_win tests/test_config_win.c src/config/wf_config_win.c)
target_link_libraries(test_config_win PRIVATE wfstate wfcore p0_lab)
target_include_directories(test_config_win PRIVATE tests)
add_test(NAME config_win COMMAND test_config_win)
add_executable(test_state_dir tests/test_state_dir.c src/config/wf_config_win.c)
target_link_libraries(test_state_dir PRIVATE wfoldstate wfefile wfdecode wfstate wfcore)
target_include_directories(test_state_dir PRIVATE tests)
add_test(NAME state_dir COMMAND test_state_dir)
add_executable(test_startup tests/test_startup.c src/path/cwd_neutralize.c)
target_link_libraries(test_startup PRIVATE wfcore wfstorage p0_lab advapi32)
target_include_directories(test_startup PRIVATE tests)
add_test(NAME startup COMMAND test_startup)
add_executable(test_private_owner tests/test_private_owner.c)
target_link_libraries(test_private_owner PRIVATE wfcore wfstorage p0_lab advapi32)
target_include_directories(test_private_owner PRIVATE tests)
add_test(NAME private_owner COMMAND test_private_owner)
set_tests_properties(private_owner PROPERTIES TIMEOUT 30 RESOURCE_LOCK wf_dos_drive)
add_executable(test_process_facts tests/test_process_facts.c)
target_link_libraries(test_process_facts PRIVATE wfcore advapi32 user32)
target_include_directories(test_process_facts PRIVATE tests)
add_test(NAME process_facts COMMAND test_process_facts)
set_tests_properties(process_facts PROPERTIES TIMEOUT 20)
foreach(name git_resolver license_checkout license_audit)
  add_test(NAME ${name} COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/test_${name}.py")
  set_tests_properties(${name} PROPERTIES TIMEOUT 45 LABELS non-destructive)
endforeach()
add_test(NAME license_notices COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tools/audit_licenses.py" "${PROJECT_BINARY_DIR}"
  --output "${PROJECT_BINARY_DIR}/license-notice-audit.json" --check-notices)
set_tests_properties(license_notices PROPERTIES TIMEOUT 60 LABELS non-destructive)
add_executable(test_diag_format tests/test_diag_format.c)
target_link_libraries(test_diag_format PRIVATE wfstorage)
target_include_directories(test_diag_format PRIVATE tests)
add_test(NAME diag_format COMMAND test_diag_format)
add_executable(test_diag_win tests/test_diag_win.c src/path/cwd_neutralize.c)
target_link_libraries(test_diag_win PRIVATE wfdiagnostics p0_lab)
target_include_directories(test_diag_win PRIVATE tests)
add_test(NAME diag_win COMMAND test_diag_win)
set_tests_properties(diag_win PROPERTIES TIMEOUT 60)
add_executable(test_recovery_log tests/test_recovery_log.c)
target_link_libraries(test_recovery_log PRIVATE wfstorage p0_lab)
target_include_directories(test_recovery_log PRIVATE tests)
add_test(NAME recovery_log COMMAND test_recovery_log)
foreach(t xfer_lifetime preview_lifetime fs_lifetime)
  add_executable(test_${t} tests/test_${t}.c)
  target_link_libraries(test_${t} PRIVATE wfarchive wfcore p0_lab)
  target_include_directories(test_${t} PRIVATE tests)
  add_test(NAME ${t} COMMAND test_${t})
  set_tests_properties(${t} PROPERTIES TIMEOUT 15)
endforeach()
foreach(t recovery_win recovery_store recovery_partial)
  add_executable(test_${t} tests/test_${t}.c)
  if(t STREQUAL "recovery_partial")
    target_link_libraries(test_${t} PRIVATE recovery_fault_proofs wfstorage p0_lab)
  else()
    target_link_libraries(test_${t} PRIVATE wfstorage p0_lab)
  endif()
  target_include_directories(test_${t} PRIVATE tests)
  add_test(NAME ${t} COMMAND test_${t})
  set_tests_properties(${t} PROPERTIES TIMEOUT 60)
endforeach()
add_library(recovery_fault_proofs STATIC ${WF_OWNED_PROOF_SOURCES})
target_include_directories(recovery_fault_proofs PRIVATE src)
target_compile_options(recovery_fault_proofs PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/tests/wf_recovery_fault_shim.h)
target_link_libraries(test_recovery_win PRIVATE p0_journal)
add_executable(test_recovery_journal tests/test_recovery_journal.c)
target_link_libraries(test_recovery_journal PRIVATE p0_journal p0_lab)
target_include_directories(test_recovery_journal PRIVATE tests)
add_test(NAME recovery_journal COMMAND test_recovery_journal)
add_executable(test_recovery_cli tests/test_recovery_cli.c)
target_link_libraries(test_recovery_cli PRIVATE wfstorage p0_lab)
target_include_directories(test_recovery_cli PRIVATE tests)
add_test(NAME recovery_cli COMMAND test_recovery_cli $<TARGET_FILE:piero>)
set_tests_properties(recovery_cli PROPERTIES TIMEOUT 30)

find_program(WF_PWSH NAMES pwsh powershell REQUIRED)
if(WF_PWSH)
  add_test(NAME copy_acceptance COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/p0/test_copy_acceptance.ps1 -Build ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(copy_acceptance PROPERTIES TIMEOUT 120)
  add_test(NAME worker_mutations COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/p0/test_worker_mutations.ps1
    -Compiler ${CMAKE_C_COMPILER} -Build ${CMAKE_CURRENT_BINARY_DIR} -Configuration $<CONFIG>)
  set_tests_properties(worker_mutations PROPERTIES TIMEOUT 120)
  add_test(NAME hash_dependency COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/p0/test_hash_dependency.ps1
    -Exe $<TARGET_FILE:test_storage_win> -Results ${CMAKE_CURRENT_BINARY_DIR}/hash-evidence)
  set_tests_properties(hash_dependency PROPERTIES TIMEOUT 60)
  add_test(NAME diag_mutations COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/p0/test_diag_mutations.ps1
    -Compiler ${CMAKE_C_COMPILER} -Build ${CMAKE_CURRENT_BINARY_DIR} -Configuration $<CONFIG>)
  set_tests_properties(diag_mutations PROPERTIES TIMEOUT 180)
  add_test(NAME log_mutations COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/p0/test_log_mutations.ps1
    -Compiler ${CMAKE_C_COMPILER} -Build ${CMAKE_CURRENT_BINARY_DIR} -Configuration $<CONFIG>)
  set_tests_properties(log_mutations PROPERTIES TIMEOUT 180)
  add_test(NAME product_tui COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/p0/test_product.ps1 -HostExe $<TARGET_FILE:p0_conpty_host>
    -Product $<TARGET_FILE:piero> -Results ${CMAKE_CURRENT_BINARY_DIR}/product-evidence)
  # The scenario now includes a real transfer of 256 MiB with pause, resume and
  # cancel (product_midtransfer); it was about 40 s before, so 60 s no longer
  # fits. This is the limit of the whole run, not of any single check.
  set_tests_properties(product_tui PROPERTIES TIMEOUT 150 SKIP_RETURN_CODE 77)
  # UI evidence is independent of shell association registration and the
  # copy-queue integration trial. product_tui remains required and unchanged.
  add_test(NAME ui_audit COMMAND $<TARGET_FILE:p0_conpty_host>
    --scenario ui-audit --child $<TARGET_FILE:piero>
    --results ${CMAKE_CURRENT_BINARY_DIR}/ui-evidence --label ctest)
  set_tests_properties(ui_audit PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
  add_test(NAME keyboard_profiles COMMAND $<TARGET_FILE:p0_conpty_host>
    --scenario keyboard --child $<TARGET_FILE:piero>
    --results ${CMAKE_CURRENT_BINARY_DIR}/keyboard-evidence --label ctest)
  set_tests_properties(keyboard_profiles PROPERTIES TIMEOUT 90 RESOURCE_LOCK wf_dos_drive)
  add_test(NAME recovery_mutations COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/p0/test_recovery_mutations.ps1
    -Compiler ${CMAKE_C_COMPILER} -Build ${CMAKE_CURRENT_BINARY_DIR} -Configuration $<CONFIG>)
  set_tests_properties(recovery_mutations PROPERTIES TIMEOUT 180)
  add_test(NAME recovery_crash COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/p0/test_recovery_suite.ps1 -Exe $<TARGET_FILE:p0_recovery_suite>
    -Results ${CMAKE_CURRENT_BINARY_DIR}/recovery-evidence)
  set_tests_properties(recovery_crash PROPERTIES TIMEOUT 180)
  # One CTest per test file in the mutation table, read from the script itself so
  # the table and the registration cannot drift apart. Each group has its own
  # budget; inside it every mutant still has the script's own hang limit.
  file(READ ${CMAKE_CURRENT_SOURCE_DIR}/tools/p0/test_release_mutations.ps1 WF_RELEASE_MUTANTS)
  string(REGEX MATCHALL "test='tests/[a-z0-9_]+\\.c'" WF_RELEASE_MUTANT_TESTS "${WF_RELEASE_MUTANTS}")
  list(REMOVE_DUPLICATES WF_RELEASE_MUTANT_TESTS)
  set(WF_RELEASE_MUTATION_GROUPS "")
  foreach(entry IN LISTS WF_RELEASE_MUTANT_TESTS)
    string(REGEX REPLACE "test='(tests/([a-z0-9_]+)\\.c)'" "\\1;\\2" parts "${entry}")
    list(GET parts 0 mutant_test_file)
    list(GET parts 1 mutant_test_name)
    set(mutant_hang_limit 15000)
    if(mutant_test_name STREQUAL "test_extract_win" OR
        mutant_test_name STREQUAL "test_batchworker_win")
      # Extraction creates 4000 flushed entries. Batch cancellation/stop also
      # exercises thousands of durable operations (19.4 s in the CI control)
      # with a bounded 180 s test wait. Allow that control to finish before
      # judging mutants; only an assertion and exit 1 count as a kill.
      set(mutant_hang_limit 240000)
    elseif(mutant_test_name STREQUAL "test_searchworker_win")
      # The stress cases have bounded waits of 30, 60 and 120 seconds.
      set(mutant_hang_limit 240000)
    endif()
    set(control_hang_limit ${mutant_hang_limit})
    if(mutant_test_name STREQUAL "test_batchexec" OR
        mutant_test_name STREQUAL "test_theme_ui" OR
        mutant_test_name STREQUAL "test_image_hostile")
      # Full controls are not fail-fast mutants. Hosted MSVC Debug takes
      # 26.05 s for theme_ui and 16.22 s for image_hostile. Use the ordinary
      # test budget, retaining the 15 s mutant hang detector.
      string(REGEX REPLACE "^test_" "" control_test "${mutant_test_name}")
      get_test_property(${control_test} TIMEOUT control_seconds)
      math(EXPR control_hang_limit "${control_seconds} * 1000")
    elseif(mutant_test_name STREQUAL "test_fuzzyworker_win" OR
        mutant_test_name STREQUAL "test_archive")
      # Both ordinary tests are registered later with a 120 s timeout.
      set(control_hang_limit 120000)
    endif()
    add_test(NAME release_mutations_${mutant_test_name} COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/p0/test_release_mutations.ps1
      -Compiler ${CMAKE_C_COMPILER} -Build ${CMAKE_CURRENT_BINARY_DIR} -Configuration $<CONFIG>
      -Test ${mutant_test_file} -MutantTimeoutMs ${mutant_hang_limit}
      -ControlTimeoutMs ${control_hang_limit})
    string(REGEX MATCHALL "test='${mutant_test_file}'" group_entries "${WF_RELEASE_MUTANTS}")
    list(LENGTH group_entries group_count)
    # Each case has its own hang deadline. Budget the group for every case,
    # one unmodified control, and up to 30 seconds of compilation per case;
    # a 180-second group could interrupt healthy 10-second editor cases.
    math(EXPR group_budget "${group_count} * (${mutant_hang_limit} / 1000 + 30) + ${control_hang_limit} / 1000 + 30")
    if(WF_SANITIZE)
      math(EXPR group_budget "${group_budget} * 4")
    endif()
    set_tests_properties(release_mutations_${mutant_test_name} PROPERTIES
      TIMEOUT ${group_budget} SKIP_RETURN_CODE 77)
    list(APPEND WF_RELEASE_MUTATION_GROUPS release_mutations_${mutant_test_name})
  endforeach()
  list(LENGTH WF_RELEASE_MUTATION_GROUPS WF_RELEASE_MUTATION_GROUP_COUNT)
  if(WF_RELEASE_MUTATION_GROUP_COUNT LESS 20)
    message(FATAL_ERROR "release mutation table parsed into ${WF_RELEASE_MUTATION_GROUP_COUNT} groups")
  endif()
  # the groups of the syntax tests copy the DLL and the grammars for every mutant
  if(TEST release_mutations_test_extract_win)
    set_tests_properties(release_mutations_test_extract_win PROPERTIES TIMEOUT 2400)
  endif()
  foreach(g release_mutations_test_syntax_loader release_mutations_test_syntax_host
      release_mutations_test_syntax_worker release_mutations_test_syntax_core)
    if(TEST ${g})
      set_tests_properties(${g} PROPERTIES TIMEOUT 1200)
    endif()
  endforeach()
  add_test(NAME cli COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_cli.ps1 -Exe $<TARGET_FILE:piero>)
  add_test(NAME config_mutations COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config/test_config_mutations.ps1
    -Compiler ${CMAKE_C_COMPILER} -Build ${CMAKE_CURRENT_BINARY_DIR} -Configuration $<CONFIG>)
  set_tests_properties(config_mutations PROPERTIES TIMEOUT 180)
  add_test(NAME verify_mutations COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/p0/test_verify_mutations.ps1
    -Compiler ${CMAKE_C_COMPILER} -Build ${CMAKE_CURRENT_BINARY_DIR} -Configuration $<CONFIG>)
  set_tests_properties(verify_mutations PROPERTIES TIMEOUT 180)
  add_test(NAME preflight_mutations
    COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/p0/test_preflight_mutations.ps1
      -Compiler ${CMAKE_C_COMPILER} -Build ${CMAKE_CURRENT_BINARY_DIR}
      -Configuration $<CONFIG>)
  set_tests_properties(preflight_mutations PROPERTIES TIMEOUT 180)
  add_test(NAME storage_evidence
    COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/p0/test_storage_evidence.ps1
      -Exe $<TARGET_FILE:test_storage_win>
      -Results ${CMAKE_CURRENT_BINARY_DIR}/evidence-tests)
  set_tests_properties(storage_evidence PROPERTIES TIMEOUT 60)
  add_test(NAME storage_mutations
    COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/p0/test_storage_mutations.ps1
      -Compiler ${CMAKE_C_COMPILER} -Build ${CMAKE_CURRENT_BINARY_DIR}
      -Configuration $<CONFIG>)
  set_tests_properties(storage_mutations PROPERTIES TIMEOUT 180)
  add_test(NAME ci_banned_apis
    COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci_check_banned_apis.ps1
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

# Open against the real shell, through a private HKCU file-type registration.
add_executable(open_target tests/open_target.c)
add_executable(test_launchworker_win tests/test_launchworker_win.c src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)
target_link_libraries(test_launchworker_win PRIVATE wflaunch p0_lab)
target_include_directories(test_launchworker_win PRIVATE tests)
add_dependencies(test_launchworker_win open_target)
add_test(NAME launchworker_win COMMAND test_launchworker_win)
set_tests_properties(launchworker_win PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(launchworker_win PROPERTIES TIMEOUT 120)

# The weak identity of a FAT volume: never taken for a proven one.
add_executable(test_weakid tests/test_weakid.c)
target_link_libraries(test_weakid PRIVATE wfcore)
target_include_directories(test_weakid PRIVATE tests)
add_test(NAME weakid COMMAND test_weakid)

# The size worker's queue of folders to list: bounded, and no leak when it cannot grow.
add_executable(test_sizeworker_bounds tests/test_sizeworker_bounds.c)
target_link_libraries(test_sizeworker_bounds PRIVATE wfcore)
target_include_directories(test_sizeworker_bounds PRIVATE tests)
add_test(NAME sizeworker_bounds COMMAND test_sizeworker_bounds)

# What an Undo entry keeps after a job took some of its items back.
add_executable(test_undo_partial tests/test_undo_partial.c)
target_link_libraries(test_undo_partial PRIVATE wfcore)
target_include_directories(test_undo_partial PRIVATE tests)
add_test(NAME undo_partial COMMAND test_undo_partial)

# Closing a watch whose request does not finish: a short wait, memory kept.
add_executable(test_watch_close tests/test_watch_close.c)
target_link_libraries(test_watch_close PRIVATE wfcore)
target_include_directories(test_watch_close PRIVATE tests)
add_test(NAME watch_close COMMAND test_watch_close)

# A program's output that cannot be held is out of memory, not too big.
add_executable(test_gitproc_memory tests/test_gitproc_memory.c)
target_link_libraries(test_gitproc_memory PRIVATE wflaunch wfcore)
target_include_directories(test_gitproc_memory PRIVATE tests)
add_test(NAME gitproc_memory COMMAND test_gitproc_memory)

foreach(kind ssh git decoder)
  add_executable(test_proc_attributes_${kind} tests/test_proc_attributes.c)
  string(TOUPPER "${kind}" kind_define)
  target_compile_definitions(test_proc_attributes_${kind} PRIVATE WF_TEST_PROC_${kind_define})
  target_include_directories(test_proc_attributes_${kind} PRIVATE tests)
  target_link_libraries(test_proc_attributes_${kind} PRIVATE wfcore)
  add_test(NAME proc_attributes_${kind} COMMAND test_proc_attributes_${kind})
  set_tests_properties(proc_attributes_${kind} PROPERTIES TIMEOUT 30)
endforeach()
target_link_libraries(test_proc_attributes_git PRIVATE wflaunch)
target_link_libraries(test_proc_attributes_decoder PRIVATE wfdecode)

# The line diff's bounds, before anything is allocated.
add_executable(test_linediff_bound tests/test_linediff_bound.c)
target_include_directories(test_linediff_bound PRIVATE tests)
add_test(NAME linediff_bound COMMAND test_linediff_bound)

# A FAT volume's listing and open: unsupported FileId/attribute answers all the way.
add_executable(test_launch_fat tests/test_launch_fat.c)
target_link_libraries(test_launch_fat PRIVATE wfcore wfstorage shell32 ole32)
target_include_directories(test_launch_fat PRIVATE tests)
add_test(NAME launch_fat COMMAND test_launch_fat)

# The preview pane's bounded directory listing, on the real filesystem.
add_executable(test_dirpreview_win tests/test_dirpreview_win.c src/fs/wf_enum_win.c src/fs/wf_fsworker.c src/fs/wf_watch_win.c)
target_link_libraries(test_dirpreview_win PRIVATE wfarchive wfcore p0_lab)
target_include_directories(test_dirpreview_win PRIVATE tests)
add_test(NAME dirpreview_win COMMAND test_dirpreview_win)
set_tests_properties(dirpreview_win PROPERTIES TIMEOUT 60)

# The editor's file access on the real filesystem.
add_executable(test_efile_win tests/test_efile_win.c)
target_link_libraries(test_efile_win PRIVATE wfefile p0_lab)
target_include_directories(test_efile_win PRIVATE tests)
add_test(NAME efile_win COMMAND test_efile_win)
set_tests_properties(efile_win PROPERTIES TIMEOUT 60)

# Injected failures during a save: the original must survive every one.
add_executable(test_efile_faults tests/test_efile_faults.c
  src/async/wf_blocking.c)
target_link_libraries(test_efile_faults PRIVATE wfjournal wfcore wfstorage bcrypt p0_lab)
target_include_directories(test_efile_faults PRIVATE tests)
add_test(NAME efile_faults COMMAND test_efile_faults)
set_tests_properties(efile_faults PROPERTIES TIMEOUT 60)

# The editor's key handling, prompts and screen, against a fake file system.
add_executable(test_esession tests/test_esession.c)
target_link_libraries(test_esession PRIVATE wfefile)
target_include_directories(test_esession PRIVATE tests)
add_test(NAME esession COMMAND test_esession)

add_executable(test_editor_compat tests/test_editor_compat.c)
target_link_libraries(test_editor_compat PRIVATE wfefile)
target_include_directories(test_editor_compat PRIVATE tests)
add_test(NAME editor_compat COMMAND test_editor_compat)

# Big-text behaviour of the editing model; prints its figures.
add_executable(test_editor_perf tests/test_editor_perf.c)
target_link_libraries(test_editor_perf PRIVATE wfcore)
target_include_directories(test_editor_perf PRIVATE tests)
add_test(NAME editor_perf COMMAND test_editor_perf)
set_tests_properties(editor_perf PROPERTIES TIMEOUT 120)

# Measurement only: reports CPU work, not terminal presentation latency.
add_executable(bench_ui tools/bench_ui.c)
target_link_libraries(bench_ui PRIVATE wfcore)
target_include_directories(bench_ui PRIVATE src)
add_executable(test_run_win tests/test_run_win.c)
target_link_libraries(test_run_win PRIVATE p0_lab)
target_include_directories(test_run_win PRIVATE tests)
add_test(NAME run_win COMMAND test_run_win)
set_tests_properties(run_win PROPERTIES TIMEOUT 30)
add_executable(test_assoc_faults tests/test_assoc_faults.c)
target_link_libraries(test_assoc_faults PRIVATE bcrypt)
target_include_directories(test_assoc_faults PRIVATE tests)
add_test(NAME assoc_faults COMMAND test_assoc_faults)
add_executable(test_recycle_faults tests/test_recycle_faults.c)
target_link_libraries(test_recycle_faults PRIVATE wfcore p0_lab ole32 shell32 uuid)
target_include_directories(test_recycle_faults PRIVATE tests src)
add_test(NAME recycle_faults COMMAND test_recycle_faults)
set_tests_properties(recycle_faults PROPERTIES TIMEOUT 30)
add_executable(test_decoder_startup tests/test_decoder_startup.c)
add_executable(test_language_files tests/test_language_files.c)
target_include_directories(test_language_files PRIVATE tests)
target_link_libraries(test_language_files PRIVATE wfefile p0_lab)
add_test(NAME language_files COMMAND test_language_files)
add_test(NAME language_missing COMMAND test_language_files --missing)
set_tests_properties(language_files language_missing PROPERTIES TIMEOUT 120)
add_test(NAME language_catalog COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tests/test_language_catalog.py")
add_test(NAME pe_imports COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tests/test_pe_imports.py")
set_tests_properties(pe_imports PROPERTIES LABELS non-destructive TIMEOUT 30)
add_test(NAME package_windows COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tests/test_package_windows.py")
set_tests_properties(package_windows PROPERTIES LABELS non-destructive TIMEOUT 30)
add_test(NAME check_package COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tests/test_check_package.py")
set_tests_properties(check_package PROPERTIES LABELS non-destructive TIMEOUT 30)
add_test(NAME prepare_baseline COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tests/test_prepare_baseline.py")
set_tests_properties(prepare_baseline PROPERTIES LABELS non-destructive TIMEOUT 60)
add_test(NAME syntax_codegen COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tests/test_syntax_codegen.py")
set_tests_properties(syntax_codegen PROPERTIES LABELS non-destructive TIMEOUT 30)
add_test(NAME mutation_tools COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tests/test_mutation_tools.py")
set_tests_properties(mutation_tools PROPERTIES LABELS non-destructive TIMEOUT 30)
add_test(NAME ci_cache COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_ci_cache.py")
set_tests_properties(ci_cache PROPERTIES LABELS non-destructive TIMEOUT 30)
add_test(NAME build_version COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tests/test_build_version.py")
set_tests_properties(build_version PROPERTIES LABELS non-destructive TIMEOUT 30)
add_test(NAME preflight_results COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tests/test_preflight_results.py")
set_tests_properties(preflight_results PROPERTIES LABELS non-destructive TIMEOUT 30)
add_test(NAME publication_paths COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tests/test_commit_publication.py")
set_tests_properties(publication_paths PROPERTIES LABELS non-destructive TIMEOUT 30)
add_test(NAME publication_audit COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tests/test_audit_publication.py")
set_tests_properties(publication_audit PROPERTIES LABELS non-destructive TIMEOUT 30)
add_test(NAME language_relocation COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tests/test_language_catalog.py" --relocate $<TARGET_FILE:test_language_files>)
set_tests_properties(language_files language_missing language_catalog language_relocation
  PROPERTIES LABELS non-destructive TIMEOUT 120)
target_include_directories(test_decoder_startup PRIVATE tests src)
target_link_libraries(test_decoder_startup PRIVATE wfstate wfcore userenv shell32 ole32
  advapi32 bcrypt uuid)
add_test(NAME decoder_startup COMMAND test_decoder_startup)
set_tests_properties(decoder_startup PROPERTIES TIMEOUT 15)

if(WF_SANITIZE)
  add_executable(test_asan_pool tests/test_asan_pool.c)
  add_test(NAME asan_pool COMMAND test_asan_pool)
  add_library(asan_pool_dll SHARED tests/asan_pool_dll.c)
  get_target_property(WF_SYN_TEST_LINK_OPTIONS piero-syntax LINK_OPTIONS)
  set_property(TARGET asan_pool_dll PROPERTY LINK_OPTIONS "${WF_SYN_TEST_LINK_OPTIONS}")
  target_compile_options(asan_pool_dll PRIVATE /O2 /GF)
  add_executable(test_asan_pool_dll tests/test_asan_pool_dll.c)
  target_include_directories(test_asan_pool_dll PRIVATE tests)
  add_dependencies(test_asan_pool_dll asan_pool_dll)
  add_test(NAME asan_pool_dll COMMAND test_asan_pool_dll)
  add_test(NAME sanitizer_detect_dll_global COMMAND ${CMAKE_COMMAND}
    -DPROGRAM=$<TARGET_FILE:test_asan_pool_dll> -DMODE=asan_global
    -P ${PROJECT_SOURCE_DIR}/tests/check_sanitizer.cmake)
  set_tests_properties(asan_pool_dll sanitizer_detect_dll_global
    PROPERTIES LABELS non-destructive TIMEOUT 30)
  add_executable(test_sanitizer_probe tests/test_sanitizer_probe.c)
  wf_use_sanitizer_runtime(test_sanitizer_probe)
  target_link_options(test_sanitizer_probe PRIVATE /DEPENDENTLOADFLAG:0x800)
  add_executable(test_sanitizer_runtime tests/test_sanitizer_runtime.c)
  target_include_directories(test_sanitizer_runtime PRIVATE tests)
  wf_use_sanitizer_runtime(test_sanitizer_runtime)
  target_link_options(test_sanitizer_runtime PRIVATE /DEPENDENTLOADFLAG:0x800)
  get_filename_component(WF_SANITIZER_DLL_NAME "${WF_ASAN_DLL}" NAME)
  add_test(NAME sanitizer_runtime_identity COMMAND test_sanitizer_runtime
    "$<TARGET_FILE_DIR:piero>/piero.asan.runtime/${WF_SANITIZER_DLL_NAME}")
  foreach(mode IN ITEMS asan ubsan)
    add_test(NAME sanitizer_detect_${mode} COMMAND ${CMAKE_COMMAND}
      -DPROGRAM=$<TARGET_FILE:test_sanitizer_probe> -DMODE=${mode}
      -P ${PROJECT_SOURCE_DIR}/tests/check_sanitizer.cmake)
  endforeach()
  add_test(NAME sanitizer_product_version COMMAND $<TARGET_FILE:piero> --version)
  set_tests_properties(sanitizer_product_version PROPERTIES TIMEOUT 15)
endif()

# Extracting from a ZIP: the chosen items, hostile names, merge and policies.
add_executable(test_extract_win tests/test_extract_win.c src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)
target_link_libraries(test_extract_win PRIVATE wfcopyengine p0_lab p0_common shell32)
target_include_directories(test_extract_win PRIVATE tests)
add_test(NAME extract_win COMMAND test_extract_win)
set_tests_properties(extract_win PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)

# The copy engine on the real filesystem, including a killed process.
add_executable(test_copyengine_win tests/test_copyengine_win.c tests/wf_copyengine_quarantine.c src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)
target_link_libraries(test_copyengine_win PRIVATE wfcopyengine p0_lab p0_common shell32)
target_include_directories(test_copyengine_win PRIVATE tests)
add_test(NAME copyengine_win COMMAND test_copyengine_win)
set_tests_properties(copyengine_win PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)

# Injected failures during a copy.
add_executable(test_copyengine_faults tests/test_copyengine_faults.c src/xfer/wf_copyjournal_win.c
  src/xfer/wf_recycle_win.c src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)
target_link_libraries(test_copyengine_faults PRIVATE wfcore wfstorage wfverify wfintegrity p0_lab p0_common ole32 shell32 uuid bcrypt)
target_include_directories(test_copyengine_faults PRIVATE tests)
add_test(NAME copyengine_faults COMMAND test_copyengine_faults)
set_tests_properties(copyengine_faults PROPERTIES TIMEOUT 120)

# The copy worker: order, pause/resume, cancel, bounded queue, clean stop.
add_executable(test_copyworker_win tests/test_copyworker_win.c src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)
target_link_libraries(test_copyworker_win PRIVATE wfcopyengine p0_lab)
target_include_directories(test_copyworker_win PRIVATE tests)
add_test(NAME copyworker_win COMMAND test_copyworker_win)
add_executable(test_copyjournal_legacy tests/test_copyjournal_legacy.c src/xfer/wf_copyjournal_legacy.c)
target_include_directories(test_copyjournal_legacy PRIVATE tests src)
add_test(NAME copyjournal_legacy COMMAND test_copyjournal_legacy)
add_executable(test_copyworker_midtransfer tests/test_copyworker_midtransfer.c src/xfer/wf_probe_win.c src/fs/wf_enum_win.c)
target_link_libraries(test_copyworker_midtransfer PRIVATE wfcopyengine p0_lab p0_common)
target_include_directories(test_copyworker_midtransfer PRIVATE tests)
add_test(NAME copyworker_midtransfer COMMAND test_copyworker_midtransfer)
set_tests_properties(copyworker_midtransfer PROPERTIES TIMEOUT 180)
add_executable(test_copyworker_startup tests/test_copyworker_startup.c)
target_include_directories(test_copyworker_startup PRIVATE tests)
target_link_libraries(test_copyworker_startup PRIVATE wfcopyengine p0_lab)
add_test(NAME copyworker_startup COMMAND test_copyworker_startup)
set_tests_properties(copyworker_startup PROPERTIES TIMEOUT 30)
set_tests_properties(copyworker_win PROPERTIES TIMEOUT 120)

# Saving settings to the real configuration file.
add_executable(test_config_save_win tests/test_config_save_win.c)
target_link_libraries(test_config_save_win PRIVATE wfefile p0_lab)
target_include_directories(test_config_save_win PRIVATE tests)
add_test(NAME config_save_win COMMAND test_config_save_win)
set_tests_properties(config_save_win PROPERTIES TIMEOUT 60)

# Smart Go's candidate worker on the real file system.
add_executable(test_candworker_win tests/test_candworker_win.c)
target_link_libraries(test_candworker_win PRIVATE wfsmartgo p0_lab)
target_include_directories(test_candworker_win PRIVATE tests)
add_test(NAME candworker_win COMMAND test_candworker_win)
set_tests_properties(candworker_win PROPERTIES TIMEOUT 120)

# Reading archives (ZIP, 7z, TAR, single compressed files) on libarchive.
add_executable(test_archive_unicode tests/test_archive_unicode.c)
target_link_libraries(test_archive_unicode PRIVATE wf_libarchive)
target_include_directories(test_archive_unicode PRIVATE tests)
add_test(NAME archive_unicode COMMAND test_archive_unicode)
set_tests_properties(archive_unicode PROPERTIES TIMEOUT 15 LABELS "non-destructive")
add_executable(test_archive tests/test_archive.c)
add_executable(test_archive_metadata tests/test_archive_metadata.c)
target_link_libraries(test_archive_metadata PRIVATE wfarchive)
target_include_directories(test_archive_metadata PRIVATE tests)
target_compile_definitions(test_archive_metadata PRIVATE
  "WF_TEST_ARCHIVE_FILE=L\"${CMAKE_SOURCE_DIR}/tests/archive/basic.zip\"")
add_test(NAME archive_metadata COMMAND test_archive_metadata)
target_link_libraries(test_archive PRIVATE wfarchive p0_lab)
target_include_directories(test_archive PRIVATE tests)
target_compile_definitions(test_archive PRIVATE
  "WF_TEST_ARCHIVE_DIR=\"${CMAKE_SOURCE_DIR}/tests/archive\"")
add_test(NAME archive COMMAND test_archive)
set_tests_properties(archive PROPERTIES TIMEOUT 120)

# Archives through the listing worker.
add_executable(test_archive_worker tests/test_archive_worker.c src/fs/wf_enum_win.c src/fs/wf_fsworker.c)
target_link_libraries(test_archive_worker PRIVATE wfarchive)
target_include_directories(test_archive_worker PRIVATE tests)
target_compile_definitions(test_archive_worker PRIVATE
  "WF_TEST_ARCHIVE_DIR=\"${CMAKE_SOURCE_DIR}/tests/archive\"")
add_test(NAME archive_worker COMMAND test_archive_worker)
set_tests_properties(archive_worker PROPERTIES TIMEOUT 60)

# Prints built-in profile keys for the keybindings guides' reference workflow;
# not a test.
add_executable(dump_keys tests/dump_keys.c)
target_link_libraries(dump_keys PRIVATE wfcore)

# These tests (including children compiled by mutation suites) hold the same
# named DOS-drive mutex. Scheduling a second holder while a mutant's 15-second
# deadline runs only measures time waiting for another test's fixture. Keep
# that deadline, and serialize the shared resource before starting either test.
# Separate CTest invocations must also run one at a time on the same session.
set_tests_properties(copy_source_win copy_source_faults owned_integrity xfer_win
  source_guard_win source_guard_faults p0_s08s10 edit_win preview_win config_win
  startup preview_lifetime recovery_cli table_win PROPERTIES RESOURCE_LOCK wf_dos_drive)
if(WF_PWSH)
  set_tests_properties(product_tui ui_audit worker_mutations preflight_mutations
    PROPERTIES RESOURCE_LOCK wf_dos_drive)
endif()

add_executable(test_sixel_conpty tests/test_sixel_conpty.c src/term/wf_term.c)
target_link_libraries(test_sixel_conpty PRIVATE wfcore)
target_include_directories(test_sixel_conpty PRIVATE tests)
add_test(NAME sixel_conpty COMMAND test_sixel_conpty)

add_executable(test_sixel_worker tests/test_sixel_worker.c)
target_link_libraries(test_sixel_worker PRIVATE wfdecode)
target_include_directories(test_sixel_worker PRIVATE tests)
add_test(NAME sixel_worker COMMAND test_sixel_worker)

add_executable(test_editor_alloc tests/test_editor_alloc.c)
target_link_libraries(test_editor_alloc PRIVATE wfcore)
target_include_directories(test_editor_alloc PRIVATE tests)
add_test(NAME editor_alloc COMMAND test_editor_alloc)
add_test(NAME imageworker_precondition COMMAND ${CMAKE_COMMAND}
  -DPROGRAM=$<TARGET_FILE:test_imageworker_win> -P ${PROJECT_SOURCE_DIR}/tests/check_worker_failure.cmake)

add_executable(test_dircache tests/test_dircache.c)
target_link_libraries(test_dircache PRIVATE wfcore)
target_include_directories(test_dircache PRIVATE tests)
add_test(NAME dircache COMMAND test_dircache)
add_executable(test_cross_move_gate tests/test_cross_move_gate.c)
target_link_libraries(test_cross_move_gate PRIVATE wfcopyengine)
target_include_directories(test_cross_move_gate PRIVATE tests)
add_test(NAME cross_move_gate COMMAND test_cross_move_gate)
add_executable(test_dir_alloc tests/test_dir_alloc.c)
target_link_libraries(test_dir_alloc PRIVATE wfcore)
target_include_directories(test_dir_alloc PRIVATE tests)
add_test(NAME dir_alloc COMMAND test_dir_alloc)

add_executable(test_copyjournal_error tests/test_copyjournal_error.c)
target_link_libraries(test_copyjournal_error PRIVATE wfjournal wfcore wfstorage)
target_include_directories(test_copyjournal_error PRIVATE tests)
add_test(NAME copyjournal_error COMMAND test_copyjournal_error)

add_test(NAME shutdown_go COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario shutdown-go --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/shutdown-go-evidence --label ctest)
set_tests_properties(shutdown_go PROPERTIES TIMEOUT 30)
add_test(NAME basic_file_operations COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario file-ops --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/basic-file-operations-evidence --label ctest)
set_tests_properties(basic_file_operations PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# DR-DRAFT-017 through the real program: the recovery gate (copy and replacing
# save refused while the older state folder holds unresolved or unreadable
# records), its release by --abandon-recovery, and the state-folder choice. One
# scenario of the existing ConPTY harness; exit 77 (a case that could not be
# set up) is reported as Skipped.
add_test(NAME state_gate COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario state-gate --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/state-gate-evidence --label ctest)
# Every user of the isolated drive declares the lock (see wf_dos_drive below):
# these hold it case after case for minutes.
set_tests_properties(state_gate PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# The private state folder through the real program: a new profile, a second
# start, a folder made by someone else, a link, a denied folder, a working
# directory that is not empty.
add_test(NAME first_run COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario first-run --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/first-run-evidence --label ctest)
set_tests_properties(first_run PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# The order of the listing through the real program: `directories_first` on
# and off, at the first listing and after the sort menu orders it again.
add_test(NAME listing COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario listing --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/listing-evidence --label ctest)
set_tests_properties(listing PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# Folder sizes through the real program: `D` writes each folder's total at the
# end of its row, changes no file, and `r` forgets the totals.
add_test(NAME dirsize COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario dirsize --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/dirsize-evidence --label ctest)
set_tests_properties(dirsize PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# `piero --continue` through the real program: what a run saves is what the next
# run puts back, with the files untouched; no session, a damaged one, a folder
# that is gone, a newer version's file, and a folder given with --continue.
add_test(NAME continue_session COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario continue --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/continue-evidence --label ctest)
set_tests_properties(continue_session PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# Change notification through the real program: another program creates, renames,
# rewrites and removes files in the folder on screen and the screen follows with no
# key; the cursor stays; the preview follows a rewrite; a burst ends as one settled
# listing. Measures a quiet program's CPU and the handles over navigation.
add_test(NAME watch_e2e COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario watch --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/watch-evidence --label ctest)
set_tests_properties(watch_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# Files from the Windows clipboard (V) through the real program: a Shell compatible
# test process puts files on a private clipboard the way Explorer's Copy does; they arrive
# by the ordinary copy, and what may not be done is refused. Replaces the clipboard's
# contents only in its own station. Not a proof about a real Explorer.
add_test(NAME clipboard_e2e COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario clipboard --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/clipboard-evidence --label ctest)
set_tests_properties(clipboard_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  RESOURCE_LOCK "wf_dos_drive;wf_clipboard")
# Markdown through the real program (DR-DRAFT-025): the preview of a .md file by
# its grammar, the editor's two panes following the typing, Alt+M, no file changed.
add_test(NAME markdown_e2e COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario markdown --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/markdown-evidence --label ctest)
set_tests_properties(markdown_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# Fuzzy search (s) through the real program: the ten situations of the request on a
# small folder (foreground) and on 4,500 names (the worker); a list change while the
# search is open; Japanese and non-BMP names; idle CPU, handles and memory measured.
add_test(NAME fuzzy_e2e COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario fuzzy --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/fuzzy-evidence --label ctest)
set_tests_properties(fuzzy_e2e PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
if(WF_SANITIZE)
  set_tests_properties(fuzzy_e2e PROPERTIES ENVIRONMENT PIERO_E2E_SANITIZED=1)
endif()
# Bulk rename (M) through the real program, on real files of an owned folder with
# the program's own state folder in the owned area: the pattern, the preview, the
# one question, the names and contents on disk, the refused batches, Esc and n, a
# cancel in the middle of 2,500 renames, and a program ended in the middle whose
# journal is asked about at the next start (DR-DRAFT-022, UI).
add_test(NAME batch_e2e COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario batch --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/batch-evidence --label ctest)
set_tests_properties(batch_e2e PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
if(WF_SANITIZE)
  set_tests_properties(batch_e2e PROPERTIES ENVIRONMENT PIERO_E2E_SANITIZED=1)
endif()
# NTFS facts (i) through the real program: a file with an alternate stream and a
# second name shows both, nothing is changed, Esc closes the window (DR-DRAFT-026).
add_test(NAME ntfsfacts_e2e COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario ntfsfacts --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/ntfsfacts-evidence --label ctest)
set_tests_properties(ntfsfacts_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# SHA-256 (#) through the real program: a folder is refused, a file's digest is
# on the status line and in the details window, nothing is changed (DR-DRAFT-027).
add_test(NAME hash_e2e COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario hash --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/hash-evidence --label ctest)
set_tests_properties(hash_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# Tabs and places through the real program: t, [ ], w and the strip in the title
# row; m then a letter writes [places] and nothing else, ' then the letter goes
# there, also in the next run (DR-DRAFT-030).
add_test(NAME tabs_e2e COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario tabs --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/tabs-evidence --label ctest)
set_tests_properties(tabs_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# Open Terminal Here through the real program: ! starts wt.exe (here a stand-in on
# a PATH made for the test, so no window opens) in the folder on screen, not the
# entry under the cursor, and the program goes on.
add_dependencies(p0_conpty_host terminal_probe)
add_test(NAME terminal_here_e2e COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario terminal-here --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/terminal-here-evidence --label ctest)
set_tests_properties(terminal_here_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# What the program holds at rest and after # D L F (DR-DRAFT-035): their workers
# start with the first request and every answer comes in at once.
add_test(NAME resources_e2e COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario resources --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/resources-evidence --label ctest)
set_tests_properties(resources_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# Git through the real product and the Windows Git of the machine (DR-DRAFT-038).
add_test(NAME git_e2e COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario git --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/git-evidence --label ctest)
set_tests_properties(git_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# Pictures through the real product, under stress and with hostile files (DR-DRAFT-039).
add_test(NAME images_e2e COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario images --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/images-evidence --label ctest)
set_tests_properties(images_e2e PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# Git through the Git of the environment that owns the repository (DR-DRAFT-038):
# paths, the status table, the line diff, the contract between environments, the
# process runner.
add_executable(test_git tests/test_git.c)
target_link_libraries(test_git PRIVATE wfgit wfcore)
target_include_directories(test_git PRIVATE tests)
add_test(NAME git COMMAND test_git)
set_tests_properties(git PROPERTIES TIMEOUT 120)
add_executable(test_git_worker tests/test_git_worker.c)
target_link_libraries(test_git_worker PRIVATE wfgit wfcore)
target_include_directories(test_git_worker PRIVATE tests)
add_test(NAME git_worker COMMAND test_git_worker)
set_tests_properties(git_worker PROPERTIES TIMEOUT 120)
add_executable(test_git_edit tests/test_git_edit.c)
target_link_libraries(test_git_edit PRIVATE wfefile)
target_include_directories(test_git_edit PRIVATE tests)
add_test(NAME git_edit COMMAND test_git_edit)
add_executable(test_git_real tests/test_git_real_win.c)
target_link_libraries(test_git_real PRIVATE wfgit wfcore shell32 ole32)
target_include_directories(test_git_real PRIVATE tests)
add_test(NAME git_real COMMAND test_git_real)
set_tests_properties(git_real PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
# Starting and stopping the workers of # D L F over and over, with a request in
# flight: no thread and no handle stays behind (DR-DRAFT-035).
add_executable(test_worker_lifecycle tests/test_worker_lifecycle.c)
target_link_libraries(test_worker_lifecycle PRIVATE wfhash wfsmartgo wflock wfclip wflaunch wfdecode wfcore p0_lab p0_common)
target_include_directories(test_worker_lifecycle PRIVATE tests)
add_test(NAME worker_lifecycle COMMAND test_worker_lifecycle)
set_tests_properties(worker_lifecycle PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# The search below a folder through the real program: Tab in the filter prompt, the
# results with their folders, Enter shows one, Delete asks once for all (DR-DRAFT-031).
add_test(NAME search_e2e COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario search --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/search-evidence --label ctest)
set_tests_properties(search_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# The duplicate files below a folder through the real program (DR-DRAFT-032).
add_test(NAME dupes_e2e COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario dupes --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/dupes-evidence --label ctest)
set_tests_properties(dupes_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# Pictures and PDF through the real program: pages that are shown, and files
# that are damaged, cut off, password-protected or not pictures at all. The
# decoder is an isolated process; it must start in a normal user session.
add_test(NAME pictures COMMAND $<TARGET_FILE:p0_conpty_host>
  --scenario pictures --child $<TARGET_FILE:piero>
  --results ${CMAKE_CURRENT_BINARY_DIR}/pictures-evidence --label ctest)
set_tests_properties(pictures PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  RESOURCE_LOCK wf_dos_drive)
# The same scenario against programs built with the gate (or the folder choice)
# taken out, one source file at a time: every such program must fail it.
add_test(NAME state_gate_mutations COMMAND ${WF_PWSH} -NoProfile -ExecutionPolicy Bypass -File
  ${CMAKE_CURRENT_SOURCE_DIR}/tools/p0/test_gate_mutations.ps1
  -Compiler ${CMAKE_C_COMPILER} -Build ${CMAKE_CURRENT_BINARY_DIR} -Configuration $<CONFIG>)
set_tests_properties(state_gate_mutations PROPERTIES TIMEOUT 4800
  RESOURCE_LOCK wf_dos_drive SKIP_RETURN_CODE 77)

add_executable(test_stage_faults tests/test_stage_faults.c)
target_link_libraries(test_stage_faults PRIVATE wfdecode p0_lab)
target_include_directories(test_stage_faults PRIVATE tests)
add_test(NAME stage_faults COMMAND test_stage_faults)

add_executable(test_copyjournal_inventory tests/test_copyjournal_inventory.c)
target_link_libraries(test_copyjournal_inventory PRIVATE wfjournal p0_lab)
target_include_directories(test_copyjournal_inventory PRIVATE tests)
add_test(NAME copyjournal_inventory COMMAND test_copyjournal_inventory)

# A test that leaves through exit() must not leave its drive letter mapped.
add_executable(test_mem tests/test_mem.c)
target_include_directories(test_mem PRIVATE tests src)
add_test(NAME mem COMMAND test_mem)

add_executable(test_lab_exit_unmaps tests/test_lab_exit_unmaps.c)
target_link_libraries(test_lab_exit_unmaps PRIVATE p0_lab)
target_include_directories(test_lab_exit_unmaps PRIVATE tests)
add_test(NAME lab_exit_unmaps COMMAND test_lab_exit_unmaps)

# The sweep of stale scratch areas: only what a killed run left, only what is
# provably a test's own (see tools/p0/p0_scratch_sweep.c).
add_executable(test_scratch_sweep tests/test_scratch_sweep.c)
target_link_libraries(test_scratch_sweep PRIVATE p0_common)
target_include_directories(test_scratch_sweep PRIVATE tests tools/p0)
add_dependencies(test_scratch_sweep p0_scratch_sweep)
add_test(NAME scratch_sweep COMMAND test_scratch_sweep
  $<TARGET_FILE:p0_scratch_sweep> ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(lab_exit_unmaps PROPERTIES TIMEOUT 90)

# Every test whose executable opens a lab with a drive letter (p0_lab_open(..., 1))
# shares the machine-wide mutex "Local\wfile-p0-isolated-drive", which a holder
# keeps for its whole run (product_tui and ui_audit: about 30 s). A test that
# does not declare the lock waits on that mutex for up to 30 s; the 20 s limit of
# the imageworker_precondition check then ends it first. Declare the resource
# so CTest never starts two users at once.
set_tests_properties(imageworker_win imageworker_precondition config_save_win
  shutdown_go basic_file_operations stage_faults copyjournal_inventory
  lab_exit_unmaps PROPERTIES RESOURCE_LOCK wf_dos_drive)

add_executable(test_preview_stability tests/test_preview_stability.c)
target_link_libraries(test_preview_stability PRIVATE wfcore)
target_include_directories(test_preview_stability PRIVATE tests)
add_test(NAME preview_stability COMMAND test_preview_stability)

add_executable(test_watch_notifications tests/test_watch_notifications.c src/fs/wf_watch_win.c)
target_link_libraries(test_watch_notifications PRIVATE wfcore)
target_include_directories(test_watch_notifications PRIVATE tests)
add_test(NAME watch_notifications COMMAND test_watch_notifications)

# These models do not execute destructive operations on user files. Some write
# bounded diagnostic output inside their build tree. This label is a selection,
# never a replacement for the full VM-only filesystem qualification suite.
set_tests_properties(text frame layout path dir sel debounce confirm app config
  preview preview_app image_wire navigation_memory operations_ui copy_ui ini_edit
  dir_preview keymap vimkeys pixmap pixcell sixel term_caps graphics decode_wire
  image_ui dircache preview_stability watch_notifications decoder_startup
  cross_move_gate i18n i18n_ui PROPERTIES LABELS non-destructive)
if(WF_SANITIZE)
  set_tests_properties(asan_pool sanitizer_runtime_identity sanitizer_detect_asan
    sanitizer_detect_ubsan sanitizer_product_version PROPERTIES LABELS non-destructive)
endif()

# The process side of an SFTP session, with a stand-in for ssh.exe that serves
# an in-memory tree on its stdio (no network, no real ssh).
add_executable(fake_sftpd tests/fake_sftpd.c)
target_link_libraries(fake_sftpd PRIVATE wfcore)
target_include_directories(fake_sftpd PRIVATE tests)
add_executable(test_sshproc tests/test_sshproc.c)
target_link_libraries(test_sshproc PRIVATE wfcore)
target_include_directories(test_sshproc PRIVATE tests)
add_dependencies(test_sshproc fake_sftpd)
add_test(NAME sshproc COMMAND test_sshproc $<TARGET_FILE:fake_sftpd>)
set_tests_properties(sshproc PROPERTIES TIMEOUT 60)

add_executable(test_wslops tests/test_wslops.c)
target_link_libraries(test_wslops PRIVATE wfefile p0_lab)
target_include_directories(test_wslops PRIVATE tests)
add_test(NAME wslops COMMAND test_wslops)
set_tests_properties(wslops PROPERTIES TIMEOUT 120)

add_executable(test_wsl_real tests/test_wsl_real.c)
target_link_libraries(test_wsl_real PRIVATE wfefile wfeditworker)
target_include_directories(test_wsl_real PRIVATE tests)
add_test(NAME wsl_real COMMAND test_wsl_real)
set_tests_properties(wsl_real PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)

add_executable(test_edraft tests/test_edraft.c src/edit/wf_edraft_win.c)
target_compile_options(test_edraft PRIVATE
  "/FI${PROJECT_SOURCE_DIR}/tests/wf_draft_case_probe.h")
target_link_libraries(test_edraft PRIVATE wfefile)
target_include_directories(test_edraft PRIVATE tests)
add_test(NAME edraft COMMAND test_edraft)

add_executable(test_eenc_alloc tests/test_eenc_alloc.c)
target_include_directories(test_eenc_alloc PRIVATE src tests)
add_test(NAME eenc_alloc COMMAND test_eenc_alloc)

add_executable(test_draftwriter tests/test_draftwriter.c src/edit/wf_edraftw_win.c)
target_compile_options(test_draftwriter PRIVATE
  "/FI${PROJECT_SOURCE_DIR}/tests/wf_draft_write_probe.h")
target_link_libraries(test_draftwriter PRIVATE wfefile)
target_include_directories(test_draftwriter PRIVATE tests)
add_test(NAME draftwriter COMMAND test_draftwriter)

add_executable(test_draftwriter_alloc tests/test_draftwriter_alloc.c)
target_link_libraries(test_draftwriter_alloc PRIVATE wfcore)
target_include_directories(test_draftwriter_alloc PRIVATE tests)
add_test(NAME draftwriter_alloc COMMAND test_draftwriter_alloc)

add_executable(test_edit_io_drafts tests/test_edit_io_drafts.c)
target_link_libraries(test_edit_io_drafts PRIVATE wfefile)
target_include_directories(test_edit_io_drafts PRIVATE tests)
add_test(NAME edit_io_drafts COMMAND test_edit_io_drafts)

add_executable(test_edit_io_clipboard tests/test_edit_io_clipboard.c)
target_link_libraries(test_edit_io_clipboard PRIVATE wfefile)
target_include_directories(test_edit_io_clipboard PRIVATE tests)
add_test(NAME edit_io_clipboard COMMAND test_edit_io_clipboard)

add_executable(test_about_read tests/test_about_read.c)
target_link_libraries(test_about_read PRIVATE wfcore)
target_include_directories(test_about_read PRIVATE tests)
add_test(NAME about_read COMMAND test_about_read)

add_executable(test_p0_sgr tests/test_p0_sgr.c)
target_include_directories(test_p0_sgr PRIVATE tests tools/p0)
add_test(NAME p0_sgr COMMAND test_p0_sgr)

add_executable(test_conpty_sync tests/test_conpty_sync.c)
target_link_libraries(test_conpty_sync PRIVATE p0_common p0_lab wfstorage wfcore
  wflock advapi32 ole32 shell32 user32 userenv rstrtmgr)
target_include_directories(test_conpty_sync PRIVATE tests tools/p0)
target_compile_definitions(test_conpty_sync PRIVATE WF_PRODUCT_EDIT)
add_test(NAME conpty_sync COMMAND test_conpty_sync)

add_executable(test_process_snapshot tests/test_process_snapshot.c)
target_link_libraries(test_process_snapshot PRIVATE wflock)
target_include_directories(test_process_snapshot PRIVATE tests)
add_test(NAME process_snapshot COMMAND test_process_snapshot)

add_executable(test_blocking tests/test_blocking.c)
target_link_libraries(test_blocking PRIVATE wfefile)
target_include_directories(test_blocking PRIVATE tests)
add_test(NAME blocking COMMAND test_blocking)

add_executable(test_sftpplace tests/test_sftpplace.c)
target_link_libraries(test_sftpplace PRIVATE wfcore wfefile)
target_include_directories(test_sftpplace PRIVATE tests)
add_test(NAME sftpplace COMMAND test_sftpplace)
set_tests_properties(sftpplace PROPERTIES TIMEOUT 60)

add_executable(test_sftpplace_proc tests/test_sftpplace_proc.c)
target_link_libraries(test_sftpplace_proc PRIVATE wfcore wfefile)
target_include_directories(test_sftpplace_proc PRIVATE tests)
add_dependencies(test_sftpplace_proc fake_sftpd)
add_test(NAME sftpplace_proc COMMAND test_sftpplace_proc $<TARGET_FILE:fake_sftpd>)
set_tests_properties(sftpplace_proc PROPERTIES TIMEOUT 90)

# Folder sizes (D): the worker's totals, links never followed, missing folders
# told, newer requests ending older ones, no handle left open.
add_executable(test_sizeworker_win tests/test_sizeworker_win.c)
target_link_libraries(test_sizeworker_win PRIVATE wfsmartgo p0_lab)
target_include_directories(test_sizeworker_win PRIVATE tests)
add_test(NAME sizeworker_win COMMAND test_sizeworker_win)
set_tests_properties(sizeworker_win PROPERTIES TIMEOUT 120)

# Fuzzy search (s): the worker on a real thread: the foreground's answer, only the
# newest request delivered, release() before the listing is freed.
add_executable(test_fuzzyworker_win tests/test_fuzzyworker_win.c)
target_link_libraries(test_fuzzyworker_win PRIVATE wfsmartgo)
target_include_directories(test_fuzzyworker_win PRIVATE tests)
add_test(NAME fuzzyworker_win COMMAND test_fuzzyworker_win)
set_tests_properties(fuzzyworker_win PROPERTIES TIMEOUT 120)

# The bulk rename's worker on real folders: 10 and 1,000 files through the screen's
# port, Esc and stop in the middle of a run, journals left behind by a process that
# ended (DR-DRAFT-022, UI).
add_executable(test_batchworker_win tests/test_batchworker_win.c)
target_link_libraries(test_batchworker_win PRIVATE wfbatchworker)
target_include_directories(test_batchworker_win PRIVATE tests)
add_test(NAME batchworker_win COMMAND test_batchworker_win)
set_tests_properties(batchworker_win PROPERTIES TIMEOUT 600)

# The saved session (`piero --continue`): the text form and its refusals, the
# two-slot store with its single writer, the writer thread.
add_executable(test_session tests/test_session.c)
target_link_libraries(test_session PRIVATE wfsession)
target_include_directories(test_session PRIVATE tests)
add_test(NAME session COMMAND test_session)
set_tests_properties(session PROPERTIES TIMEOUT 120)

add_executable(test_main_session tests/test_main_session.c src/main_session.c
  src/config/wf_config_win.c src/path/cwd_neutralize.c)
target_link_libraries(test_main_session PRIVATE wfsession wfstate wfstorage advapi32)
target_include_directories(test_main_session PRIVATE tests)
add_test(NAME main_session COMMAND test_main_session)
set_tests_properties(main_session PROPERTIES TIMEOUT 60)

add_executable(test_session_alloc tests/test_session_alloc.c)
target_link_libraries(test_session_alloc PRIVATE wfcore)
target_include_directories(test_session_alloc PRIVATE tests)
add_test(NAME session_alloc COMMAND test_session_alloc)
set_tests_properties(session_alloc PROPERTIES TIMEOUT 20 LABELS non-destructive)

# The files on the Windows clipboard: the strict parsing of what another program
# supplies, and the worker that reads the real clipboard on its own apartment.
# Native cases require a private window station; parser/data-object cases are separate.
add_executable(test_clipboard tests/test_clipboard.c)
target_link_libraries(test_clipboard PRIVATE wfclip bcrypt advapi32)
target_include_directories(test_clipboard PRIVATE tests)
add_test(NAME clipboard COMMAND test_clipboard)
add_test(NAME clipboard_parse COMMAND test_clipboard --pure)
add_executable(test_clipboard_fixture tests/test_clipboard_fixture.c)
target_include_directories(test_clipboard_fixture PRIVATE tests)
target_link_libraries(test_clipboard_fixture PRIVATE user32 advapi32 bcrypt)
add_test(NAME clipboard_fixture COMMAND test_clipboard_fixture)
set_tests_properties(clipboard_fixture PROPERTIES LABELS non-destructive TIMEOUT 15)
set_tests_properties(clipboard_parse PROPERTIES TIMEOUT 30 LABELS non-destructive)
set_tests_properties(clipboard PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)

# Who has a file open (L; DR-DRAFT-024): Restart Manager, asking to close and ending a
# program, against harmless helper programs the tests start themselves (never a
# program of the user's). The system processes are only described.
add_executable(lock_helper tests/lock_helper.c)
add_executable(test_lock tests/test_lock.c)
target_link_libraries(test_lock PRIVATE wflock)
target_include_directories(test_lock PRIVATE tests)
add_dependencies(test_lock lock_helper)
add_test(NAME lock COMMAND test_lock $<TARGET_FILE:lock_helper>)
add_test(NAME lock_policy COMMAND test_lock --policy)
set_tests_properties(lock_policy PROPERTIES TIMEOUT 30 LABELS non-destructive)
set_tests_properties(lock PROPERTIES SKIP_RETURN_CODE 77 LABELS interactive)
set_tests_properties(lock PROPERTIES TIMEOUT 180)
add_executable(test_lock_ui tests/test_lock_ui.c)
target_link_libraries(test_lock_ui PRIVATE wfcore)
target_include_directories(test_lock_ui PRIVATE tests)
add_test(NAME lock_ui COMMAND test_lock_ui)
set_tests_properties(lock_ui PROPERTIES LABELS non-destructive)
# Who has the file open (L) through the real program: a helper program started by the
# host holds a file; the window names it by PID, a close is requested (or, for a helper
# that refuses, ending it behind two questions), and the failed rename and copy are
# put back and succeed. Never touches a program of the user's.
# One case per process: the host's owned drive is released a moment after a case ends, and
# a second case in the same run sometimes cannot open it (the same happens to the other
# multi-case scenarios on a loaded machine). PIERO_GATE_ONLY picks the case.
foreach(c rename copy)
  add_test(NAME lock_${c}_e2e COMMAND $<TARGET_FILE:p0_conpty_host>
    --scenario lock --child $<TARGET_FILE:piero> --helper $<TARGET_FILE:lock_helper>
    --results ${CMAKE_CURRENT_BINARY_DIR}/lock-${c}-evidence --label ctest)
  set_tests_properties(lock_${c}_e2e PROPERTIES TIMEOUT 400 SKIP_RETURN_CODE 77
    RESOURCE_LOCK wf_dos_drive ENVIRONMENT "PIERO_GATE_ONLY=lock_${c}")
endforeach()
add_dependencies(p0_conpty_host lock_helper)

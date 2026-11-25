# Syntax colouring with Tree-sitter, grammars as separate DLLs (DR-DRAFT-033).
#
# Layout of the build (and of a release), beside piero.exe:
#   syntax/piero-syntax.dll
#   syntax/grammars/<id>/{parser.dll, highlights.scm, manifest.toml}
# piero.exe links none of the Tree-sitter code: the runtime is inside
# piero-syntax.dll, the grammars are their own DLLs (src/third_party).

set(WF_SYN_IDS json jsonc yaml toml ini xml markdown markdown_inline powershell bash)
set(WF_SYN_BIN "${PROJECT_BINARY_DIR}/syntax")
set(WF_SYN_PACK "${CMAKE_CURRENT_SOURCE_DIR}/grammar-pack")

# the queries and manifests, copied beside the grammar DLLs
set(WF_SYN_PACK_FILES)
set(WF_SYN_PACK_COPIES)
foreach(id IN LISTS WF_SYN_IDS)
  foreach(f highlights.scm manifest.toml)
    list(APPEND WF_SYN_PACK_FILES "${WF_SYN_PACK}/${id}/${f}")
    list(APPEND WF_SYN_PACK_COPIES
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WF_SYN_PACK}/${id}/${f}" "${WF_SYN_BIN}/grammars/${id}/${f}")
  endforeach()
endforeach()
add_custom_target(wf_grammar_pack
  ${WF_SYN_PACK_COPIES}
  DEPENDS ${WF_SYN_PACK_FILES}
  COMMENT "Copying the grammar queries and manifests")
add_dependencies(wf_grammar_pack wf_grammars)

# the trust table built into piero-syntax.dll
set(WF_SYN_GEN "${PROJECT_BINARY_DIR}/generated")
set(WF_SYN_GRAMMAR_TARGETS)
foreach(id IN LISTS WF_SYN_IDS)
  list(APPEND WF_SYN_GRAMMAR_TARGETS wf_grammar_${id})
endforeach()
add_custom_command(
  OUTPUT "${WF_SYN_GEN}/wf_syntax_trust.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${WF_SYN_GEN}"
  COMMAND ${CMAKE_COMMAND} "-DOUT=${WF_SYN_GEN}/wf_syntax_trust.h"
    "-DBIN=${WF_SYN_BIN}" "-DPACK=${WF_SYN_PACK}" "-DIDS=${WF_SYN_IDS}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_syntax_trust.cmake"
  DEPENDS ${WF_SYN_GRAMMAR_TARGETS} ${WF_SYN_PACK_FILES}
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_syntax_trust.cmake"
  COMMENT "Writing the table of official grammars")
add_custom_target(wf_syntax_trust DEPENDS "${WF_SYN_GEN}/wf_syntax_trust.h")

add_library(piero-syntax SHARED
  src/syntax/wf_syndll.c src/syntax/wf_syncore.c src/syntax/wf_dllload_win.c
  src/syntax/wf_manifest.c src/syntax/wf_synlang.c src/syntax/wf_synruns.c
  "${WF_SYN_GEN}/wf_syntax_trust.h")
set_target_properties(piero-syntax PROPERTIES
  PREFIX "" OUTPUT_NAME piero-syntax
  RUNTIME_OUTPUT_DIRECTORY "${WF_SYN_BIN}"
  PDB_OUTPUT_DIRECTORY "${WF_SYN_BIN}"
  ARCHIVE_OUTPUT_DIRECTORY "${WF_SYN_BIN}/lib")
target_include_directories(piero-syntax PRIVATE src/syntax "${WF_SYN_GEN}")
target_link_libraries(piero-syntax PRIVATE wf_treesitter bcrypt)
target_link_options(piero-syntax PRIVATE /DEPENDENTLOADFLAG:0x800 /OPT:REF)
add_dependencies(piero-syntax wf_syntax_trust)
wf_product_warnings(piero-syntax)
if(NOT WF_SANITIZE)
  target_link_options(piero-syntax PRIVATE /OPT:ICF)
  # no C runtime DLL to find: the DLL imports kernel32 and bcrypt only
  set_target_properties(piero-syntax PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# the pin of piero-syntax.dll that piero.exe checks before it loads it
add_custom_command(
  OUTPUT "${WF_SYN_GEN}/wf_syntax_pin.h"
  COMMAND ${CMAKE_COMMAND} "-DOUT=${WF_SYN_GEN}/wf_syntax_pin.h"
    "-DDLL=$<TARGET_FILE:piero-syntax>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_syntax_pin.cmake"
  DEPENDS piero-syntax "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_syntax_pin.cmake"
  COMMENT "Writing the pin of piero-syntax.dll")
add_custom_target(wf_syntax_pin DEPENDS "${WF_SYN_GEN}/wf_syntax_pin.h")

# piero.exe's side: finding out what is installed, loading piero-syntax.dll when
# something has to be coloured, the worker thread and the .env lexer. No
# Tree-sitter code, no grammar.
add_library(wfsyntax STATIC
  src/syntax/wf_dllload_win.c src/syntax/wf_synhost_win.c src/syntax/wf_manifest.c
  src/syntax/wf_dotenv.c src/syntax/wf_synworker.c)
target_include_directories(wfsyntax PUBLIC src src/syntax)
target_link_libraries(wfsyntax PUBLIC wfcore bcrypt)
wf_product_warnings(wfsyntax)

if(BUILD_TESTING)
  # a lab tool: colours a file with the grammars next to it and prints the runs
  add_executable(syntax_probe tools/syntax_probe.c)
  target_link_libraries(syntax_probe PRIVATE wfsyntax)
  target_include_directories(syntax_probe PRIVATE "${WF_SYN_GEN}")
  add_dependencies(syntax_probe wf_syntax_pin wf_grammars wf_grammar_pack)

  add_executable(test_syntax_core tests/test_syntax_core.c)
  target_link_libraries(test_syntax_core PRIVATE wfsyntax wfcore)
  target_include_directories(test_syntax_core PRIVATE tests)
  add_test(NAME syntax_core COMMAND test_syntax_core)
  set_tests_properties(syntax_core PROPERTIES LABELS non-destructive)
endif()

# piero.exe: the thin host and the worker; it builds beside syntax/ so that what is
# pinned in it is what sits next to it
target_link_libraries(piero PRIVATE wfsyntax)
add_dependencies(piero wf_syntax_pin wf_grammar_pack)

if(BUILD_TESTING)
  # The real program with different sets of grammars beside it (DR-DRAFT-033).
  add_test(NAME syntax_e2e COMMAND $<TARGET_FILE:p0_conpty_host>
    --scenario syntax --child $<TARGET_FILE:piero>
    --results ${CMAKE_CURRENT_BINARY_DIR}/syntax-evidence --label ctest)
  set_tests_properties(syntax_e2e PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77
    RESOURCE_LOCK wf_dos_drive)

  # the real DLL and grammars next to the test program
  add_executable(test_syntax_host tests/test_syntax_host.c)
  target_link_libraries(test_syntax_host PRIVATE wfsyntax wfcore psapi)
  target_include_directories(test_syntax_host PRIVATE tests "${WF_SYN_GEN}")
  add_dependencies(test_syntax_host wf_syntax_pin wf_grammars wf_grammar_pack)
  add_test(NAME syntax_host COMMAND test_syntax_host)
  set_tests_properties(syntax_host PROPERTIES LABELS non-destructive TIMEOUT 120)

  # stand-ins for grammar DLLs: the loader must refuse each of them
  set(WF_SYN_FIX "${PROJECT_BINARY_DIR}/syntax_fixtures")
  add_library(fixture_dependency SHARED tests/syntax_fixtures/fixture_dependency.c)
  set_target_properties(fixture_dependency PROPERTIES PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${WF_SYN_FIX}" PDB_OUTPUT_DIRECTORY "${WF_SYN_FIX}"
    ARCHIVE_OUTPUT_DIRECTORY "${WF_SYN_FIX}/lib")
  foreach(variant abi_new abi_old null dep_abi_new)
    add_library(fixture_${variant} SHARED tests/syntax_fixtures/fixture_grammar.c)
    set_target_properties(fixture_${variant} PROPERTIES PREFIX ""
      RUNTIME_OUTPUT_DIRECTORY "${WF_SYN_FIX}" PDB_OUTPUT_DIRECTORY "${WF_SYN_FIX}"
      ARCHIVE_OUTPUT_DIRECTORY "${WF_SYN_FIX}/lib")
  endforeach()
  target_compile_definitions(fixture_abi_new PRIVATE FIXTURE_ABI_NEW)
  target_compile_definitions(fixture_abi_old PRIVATE FIXTURE_ABI_OLD)
  target_compile_definitions(fixture_null PRIVATE FIXTURE_NULL)
  target_compile_definitions(fixture_dep_abi_new PRIVATE FIXTURE_DEP FIXTURE_ABI_NEW)
  target_link_libraries(fixture_dep_abi_new PRIVATE fixture_dependency)
  if(NOT WF_SANITIZE)
    foreach(t fixture_dependency fixture_abi_new fixture_abi_old fixture_null fixture_dep_abi_new)
      set_target_properties(${t} PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
  endif()

  # the loader linked into the test with a trust table of its own
  add_executable(test_syntax_loader tests/test_syntax_loader.c src/syntax/wf_syncore.c)
  target_link_libraries(test_syntax_loader PRIVATE wfsyntax wfcore wf_treesitter_dyn)
  target_include_directories(test_syntax_loader PRIVATE tests src/syntax)
  add_dependencies(test_syntax_loader wf_grammars wf_grammar_pack piero-syntax
    fixture_abi_new fixture_abi_old fixture_null fixture_dep_abi_new fixture_dependency)
  add_test(NAME syntax_loader COMMAND test_syntax_loader)
  set_tests_properties(syntax_loader PROPERTIES LABELS non-destructive TIMEOUT 300)

  # the preview pane with a fake worker
  add_executable(test_syntax_ui tests/test_syntax_ui.c)
  target_link_libraries(test_syntax_ui PRIVATE wfcore)
  target_include_directories(test_syntax_ui PRIVATE tests)
  add_test(NAME syntax_ui COMMAND test_syntax_ui)
  set_tests_properties(syntax_ui PROPERTIES LABELS non-destructive)

  # the worker thread with the real DLL
  add_executable(test_syntax_worker tests/test_syntax_worker.c)
  target_link_libraries(test_syntax_worker PRIVATE wfsyntax wfcore)
  target_include_directories(test_syntax_worker PRIVATE tests "${WF_SYN_GEN}")
  add_dependencies(test_syntax_worker wf_syntax_pin wf_grammars wf_grammar_pack)
  add_test(NAME syntax_worker COMMAND test_syntax_worker)
  set_tests_properties(syntax_worker PROPERTIES LABELS non-destructive TIMEOUT 180)
endif()

if(WF_SANITIZE)
  # Under ASan the same string literal is registered twice from piero-syntax.dll
  # (the host and DLL compile some of the same source files), which the
  # ODR check reports as a violation. Nothing is defined twice; the check is off
  # for the tests that load the DLL.
  foreach(t syntax_host syntax_loader syntax_worker syntax_ui syntax_e2e)
    if(TEST ${t})
      set_property(TEST ${t} APPEND PROPERTY ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0")
    endif()
  endforeach()
endif()

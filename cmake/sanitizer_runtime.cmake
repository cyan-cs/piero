# SxS binds exactly one compiler-rt DLL before main(), without broadening
# /DEPENDENTLOADFLAG:0x800 or the product's runtime DLL search policy.
function(wf_prepare_sanitizer_runtime)
  get_filename_component(WF_ASAN_DLL_NAME "${WF_ASAN_DLL}" NAME)
  configure_file("${PROJECT_SOURCE_DIR}/cmake/asan-application.manifest.in"
    "${PROJECT_BINARY_DIR}/asan-application.manifest" @ONLY)
  configure_file("${PROJECT_SOURCE_DIR}/cmake/asan-assembly.manifest.in"
    "${PROJECT_BINARY_DIR}/asan-assembly.manifest" @ONLY)
  file(SHA256 "${WF_ASAN_DLL}" WF_SAN_RUNTIME_SHA)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WF_ASAN_DLL}")
  file(READ "${PROJECT_BINARY_DIR}/asan-assembly.manifest" WF_SAN_MANIFEST_C)
  string(REPLACE "\\" "\\\\" WF_SAN_MANIFEST_C "${WF_SAN_MANIFEST_C}")
  string(REPLACE "\"" "\\\"" WF_SAN_MANIFEST_C "${WF_SAN_MANIFEST_C}")
  string(REPLACE "\n" "\\n" WF_SAN_MANIFEST_C "${WF_SAN_MANIFEST_C}")
  configure_file("${PROJECT_SOURCE_DIR}/cmake/sanitizer-stage.h.in"
    "${PROJECT_BINARY_DIR}/generated/wf_sanitizer_stage_config.h" @ONLY)
  add_custom_target(wf_sanitizer_runtime
    COMMAND "${CMAKE_COMMAND}" "-DRUNTIME=${WF_ASAN_DLL}"
      "-DASSEMBLY=${PROJECT_BINARY_DIR}/asan-assembly.manifest"
      "-DOUTPUT=$<TARGET_FILE_DIR:piero>"
      -P "${PROJECT_SOURCE_DIR}/cmake/stage_sanitizer_runtime.cmake"
    VERBATIM)
endfunction()

function(wf_use_sanitizer_runtime target)
  add_dependencies(${target} wf_sanitizer_runtime)
  if(ARGV1 STREQUAL "NON_DEBUG")
    # Debug's existing PATH-based worker setup remains unchanged. The copied
    # AppContainer decoder bundle needs its own isolated-environment proof.
    target_link_options(${target} PRIVATE
      "$<$<NOT:$<CONFIG:Debug>>:/MANIFEST:EMBED>"
      "$<$<NOT:$<CONFIG:Debug>>:/MANIFESTINPUT:${PROJECT_BINARY_DIR}/asan-application.manifest>")
  else()
    target_link_options(${target} PRIVATE /MANIFEST:EMBED
      "/MANIFESTINPUT:${PROJECT_BINARY_DIR}/asan-application.manifest")
  endif()
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
    "${PROJECT_BINARY_DIR}/asan-application.manifest" "${WF_ASAN_DLL}")
endfunction()

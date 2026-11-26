# Opt-in executable assembled by CMake, not by parsing Ninja shell commands.
set(WF_GATE_MUTANT_SOURCE "" CACHE FILEPATH "Reviewed mutated translation unit")
set(WF_GATE_MUTANT_ORIGINAL "" CACHE STRING "Original project-relative translation unit")
set(WF_GATE_MUTANT_OUTPUT "" CACHE PATH "Isolated mutant output directory")
if(WF_GATE_MUTANT_SOURCE)
  if(NOT EXISTS "${WF_GATE_MUTANT_SOURCE}" OR NOT WF_GATE_MUTANT_OUTPUT)
    message(FATAL_ERROR "Gate mutant source/output is missing")
  endif()
  get_target_property(gate_sources piero SOURCES)
  list(REMOVE_ITEM gate_sources "${WF_GATE_MUTANT_ORIGINAL}")
  # A replacement for a library member is linked as an explicit object before
  # the unchanged static libraries, so the original member is not extracted.
  add_executable(wf_gate_mutant EXCLUDE_FROM_ALL ${gate_sources} "${WF_GATE_MUTANT_SOURCE}")
  foreach(prop COMPILE_DEFINITIONS COMPILE_OPTIONS INCLUDE_DIRECTORIES
      LINK_LIBRARIES LINK_OPTIONS LINK_DEPENDS MSVC_RUNTIME_LIBRARY)
    get_target_property(value piero ${prop})
    if(NOT value STREQUAL "value-NOTFOUND")
      set_property(TARGET wf_gate_mutant PROPERTY ${prop} "${value}")
    endif()
  endforeach()
  get_target_property(gate_dependencies piero MANUALLY_ADDED_DEPENDENCIES)
  if(gate_dependencies)
    add_dependencies(wf_gate_mutant ${gate_dependencies})
  endif()
  set_target_properties(wf_gate_mutant PROPERTIES OUTPUT_NAME piero
    RUNTIME_OUTPUT_DIRECTORY "${WF_GATE_MUTANT_OUTPUT}"
    PDB_OUTPUT_DIRECTORY "${WF_GATE_MUTANT_OUTPUT}")
  add_custom_command(TARGET wf_gate_mutant POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/languages"
      "$<TARGET_FILE_DIR:wf_gate_mutant>/languages"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/LICENSES"
      "$<TARGET_FILE_DIR:wf_gate_mutant>/LICENSES"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/LICENSE"
      "$<TARGET_FILE_DIR:wf_gate_mutant>/LICENSE"
    VERBATIM)
  if(TARGET piero-syntax)
    add_custom_command(TARGET wf_gate_mutant POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_BINARY_DIR}/syntax"
        "$<TARGET_FILE_DIR:wf_gate_mutant>/syntax"
      VERBATIM)
  endif()
  if(WF_SANITIZE)
    add_custom_command(TARGET wf_gate_mutant POST_BUILD
      COMMAND ${CMAKE_COMMAND} "-DRUNTIME=${WF_ASAN_DLL}"
        "-DASSEMBLY=${PROJECT_BINARY_DIR}/asan-assembly.manifest"
        "-DOUTPUT=$<TARGET_FILE_DIR:wf_gate_mutant>"
        -P "${PROJECT_SOURCE_DIR}/cmake/stage_sanitizer_runtime.cmake"
      VERBATIM)
  endif()
endif()

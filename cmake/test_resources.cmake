# Mutation outputs have unique directories. Only drivers that actually request
# a DOS-drive lab need the session-wide drive lock; a variable/unknown request
# stays locked. The current lab calls are direct (none live in test headers).
# Match each group's ordinary driver too, so its fixtures cannot overlap with
# its unmodified control. Existing resource locks are always retained.
foreach(group IN LISTS WF_RELEASE_MUTATION_GROUPS)
  string(REPLACE "release_mutations_" "" driver "${group}")
  string(REGEX REPLACE "^test_" "" ordinary "${driver}")
  set_property(TEST ${group} APPEND PROPERTY RESOURCE_LOCK "wf_driver_${driver}")
  if(TEST ${ordinary})
    set_property(TEST ${ordinary} APPEND PROPERTY RESOURCE_LOCK "wf_driver_${driver}")
  endif()
  file(READ "${PROJECT_SOURCE_DIR}/tests/${driver}.c" driver_source)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/tests/${driver}.c")
  string(REGEX MATCHALL "p0_lab_open\\([^)]*\\)" lab_calls "${driver_source}")
  foreach(call IN LISTS lab_calls)
    if(NOT call MATCHES ",[ \t\r\n]*0[ \t\r\n]*\\)$")
      set_property(TEST ${group} APPEND PROPERTY RESOURCE_LOCK wf_dos_drive)
      break()
    endif()
  endforeach()
endforeach()
if(TEST clipboard_parse AND TEST release_mutations_test_clipboard)
  set_property(TEST clipboard_parse APPEND PROPERTY RESOURCE_LOCK wf_driver_test_clipboard)
endif()

# These tests enforce elapsed-time limits. Run them without other CTest work;
# concurrency must not change either the measured workload or its thresholds.
foreach(test listing_perf editor_perf decode_win release_mutations_test_decode_win)
  if(TEST ${test})
    set_tests_properties(${test} PROPERTIES RUN_SERIAL TRUE)
  endif()
endforeach()

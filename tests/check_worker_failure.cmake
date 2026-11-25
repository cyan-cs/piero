# A failed prerequisite must fail cleanly, without crashing or hanging.
execute_process(COMMAND "${PROGRAM}" --fault-first RESULT_VARIABLE result
  OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 20)
if(NOT "${result}" STREQUAL "1" OR NOT "${output}${errors}" MATCHES "decode precondition failed")
  message(FATAL_ERROR "Unexpected child result: ${result}\n${output}\n${errors}")
endif()

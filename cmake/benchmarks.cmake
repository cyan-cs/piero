# Explicit developer tool; no performance thresholds on shared CI machines.
add_executable(bench_core tools/bench_core.c src/fs/wf_enum_win.c)
target_include_directories(bench_core PRIVATE src)
target_compile_definitions(bench_core PRIVATE
  WF_BENCH_COMPILER="${CMAKE_C_COMPILER_ID}-${CMAKE_C_COMPILER_VERSION}"
  WF_BENCH_CONFIGURATION="$<CONFIG>")
target_link_libraries(bench_core PRIVATE wfcore wfdecode psapi)
add_test(NAME benchmark_schema COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tests/test_benchmark.py" $<TARGET_FILE:bench_core>)
set_tests_properties(benchmark_schema PROPERTIES TIMEOUT 120 LABELS "non-destructive")

# What starting an unused worker costs: run by hand, prints median and spread.
add_executable(bench_workers tools/bench_workers.c)
target_include_directories(bench_workers PRIVATE src)
target_link_libraries(bench_workers PRIVATE wfhash wfsmartgo wflock wfclip wflaunch wfdecode wfcore wfarchive psapi)

# What Git costs (DR-DRAFT-038): run by hand, prints medians and spread.
add_executable(bench_git tools/bench_git.c)
target_include_directories(bench_git PRIVATE src)
target_link_libraries(bench_git PRIVATE wfgit wfefile wfcore)

# What a big or hostile picture costs the decoder host (DR-DRAFT-039): by hand.
add_executable(bench_image_big tools/bench_image_big.c)
target_include_directories(bench_image_big PRIVATE src)
target_link_libraries(bench_image_big PRIVATE wfdecode windowscodecs ole32 uuid psapi)
add_dependencies(bench_image_big wf_dechost)

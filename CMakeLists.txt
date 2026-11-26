cmake_minimum_required(VERSION 3.20)
if(CMAKE_C_COMPILER_LAUNCHER AND POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
endif()
project(piero C)
if(CMAKE_C_COMPILER_LAUNCHER)
  foreach(config DEBUG RELWITHDEBINFO)
    if(POLICY CMP0141)
      # An existing build may retain /Zi from its earlier OLD-policy cache.
      string(REGEX REPLACE "(^|[ \t])[-/]Z[iI]([ \t]|$)" "\\1\\2" CMAKE_C_FLAGS_${config} "${CMAKE_C_FLAGS_${config}}")
    else()
      string(REGEX REPLACE "(^|[ \t])[-/]Z[iI]([ \t]|$)" "\\1/Z7\\2" CMAKE_C_FLAGS_${config} "${CMAKE_C_FLAGS_${config}}")
    endif()
  endforeach()
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "piero targets Windows only")
endif()

# MSVC and clang-cl share the same front-end flags.
add_compile_options(/W4 /WX /utf-8 /permissive- /std:c17)

# Maximum practical warning level, all errors (/WX). Two tiers:
#  - every target (product, tests, lab tools): conversions, signedness, shadowing,
#    fallthrough, implicit prototypes and the like;
#  - product libraries and executable only (wf_product_warnings): additionally
#    qualifier/alignment/function-cast checks, missing prototypes and format
#    attributes, which test and lab code deliberately does not follow.
# /Wall (cl) and -Weverything (clang) are avoided on purpose: they only add noise
# from system headers and C++-compatibility lints.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
  add_compile_options(
    -Wextra -Wconversion -Wsign-conversion -Wshadow -Wundef -Wpointer-arith
    -Wimplicit-fallthrough -Wstrict-prototypes -Wnull-dereference -Wvla
    -Wassign-enum -Wextra-semi -Wnewline-eof -Wdouble-promotion)
  set(WF_PRODUCT_WARNINGS
    -Wcast-qual -Wcast-align -Wmissing-prototypes -Wmissing-variable-declarations
    -Wformat=2 -Wfloat-equal -Wcomma -Wconditional-uninitialized -Wbad-function-cast
    -Wcast-function-type -Wjump-misses-init -Wcovered-switch-default)
elseif(MSVC)
  add_compile_options(/sdl /w14242 /w14254 /w14287 /w14296 /w14365 /w14388 /w14545
    /w14546 /w14547 /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928)
  set(WF_PRODUCT_WARNINGS)
endif()
function(wf_product_warnings)
  foreach(target IN LISTS ARGN)
    target_compile_options(${target} PRIVATE ${WF_PRODUCT_WARNINGS})
  endforeach()
endfunction()

# Opt-in checks: -DWF_SANITIZE=ON (ASan+UBSan, clang-cl) / -DWF_ANALYZE=ON (cl /analyze).
option(WF_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer (clang-cl)" OFF)
option(WF_ANALYZE "Run MSVC static analysis (/analyze) during the build" OFF)
# Create / rename execution (release gate G-RENAME/CREATE). ON by default:
# P0-N01 name semantics, the checked backend, the single-request worker and the
# end-to-end terminal run (product-edit scenario) are the evidence; see
# p0-results/STATUS.md. -DWF_PRODUCT_EDIT=OFF builds the read-only variant: its
# forms say so and change nothing.
option(WF_PRODUCT_EDIT "Link the create/rename executor into piero" ON)
if(WF_SANITIZE)
  if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "WF_SANITIZE requires clang-cl")
  endif()
  # ASan does not support the debug CRT (/MDd).
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
  # ASan pads pooled char/wchar_t literals to identical zero-filled COMDATs.
  # Folding them aliases differently sized global/redzone descriptors. Keep
  # /O2 and /GF, but preserve the instrumented objects' identities at link time.
  add_link_options(/OPT:NOICF /INCREMENTAL:NO)
  # CMake links with lld-link directly, so name the runtimes explicitly.
  execute_process(COMMAND ${CMAKE_C_COMPILER} /clang:-print-resource-dir
    OUTPUT_VARIABLE WF_CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE WF_RESOURCE_RESULT)
  if(NOT WF_RESOURCE_RESULT EQUAL 0 OR NOT IS_DIRECTORY "${WF_CLANG_RESOURCE_DIR}")
    message(FATAL_ERROR "Cannot query the selected clang-cl resource directory")
  endif()
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
      NOT CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    message(FATAL_ERROR "WF_SANITIZE currently supports the x64 toolchain only")
  endif()
  set(WF_SAN_LIB_DIR "${WF_CLANG_RESOURCE_DIR}/lib/windows")
  # Match clang-cl's sanitizer link driver. Merely naming the thunk archive
  # drops unreferenced CRT callbacks, including DLL-detach global cleanup.
  add_link_options(/INCLUDE:__asan_seh_interceptor
    "/WHOLEARCHIVE:${WF_SAN_LIB_DIR}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
  get_filename_component(WF_CLANG_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
  unset(WF_ASAN_DLL CACHE)
  find_file(WF_ASAN_DLL NAMES clang_rt.asan_dynamic-x86_64.dll
    PATHS "${WF_SAN_LIB_DIR}" "${WF_CLANG_BIN_DIR}" NO_DEFAULT_PATH)
  foreach(runtime IN ITEMS clang_rt.asan_dynamic-x86_64.lib
      clang_rt.asan_dynamic_runtime_thunk-x86_64.lib clang_rt.ubsan_standalone-x86_64.lib)
    if(NOT EXISTS "${WF_SAN_LIB_DIR}/${runtime}")
      message(FATAL_ERROR "Selected compiler is missing compiler-rt: ${runtime}")
    endif()
  endforeach()
  if(NOT WF_ASAN_DLL)
    message(FATAL_ERROR "Selected compiler has no x64 dynamic ASan runtime")
  endif()
  get_filename_component(WF_ASAN_DLL_DIR "${WF_ASAN_DLL}" DIRECTORY)
  message(STATUS "ASan runtime (development only): ${WF_ASAN_DLL}")
  include(cmake/sanitizer_runtime.cmake)
  link_directories("${WF_SAN_LIB_DIR}")
  link_libraries(clang_rt.asan_dynamic-x86_64 clang_rt.asan_dynamic_runtime_thunk-x86_64
    clang_rt.ubsan_standalone-x86_64)
  # The dynamic ASan runtime DLL must be found by the test executables.
endif()
if(WF_ANALYZE)
  # clang-cl also sets MSVC; /analyze is a cl-only option.
  if(NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR "WF_ANALYZE requires the MSVC compiler (cl), not clang-cl")
  endif()
  # Advisory: the analyzer has known false positives on count-tracked arrays, so
  # its findings are reported but do not fail the build.
  add_compile_options(/analyze /analyze:WX-)
endif()
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# wfcore contains OS-independent logic; this build targets Windows UTF-16/C ABI.
include(CTest)
# The storage/evidence tests build fixture paths without the \?\ prefix, so
# they fail (ERROR_PATH_NOT_FOUND) once a path passes MAX_PATH. A deep build
# directory is the usual cause; say so up front instead of at test time.
string(LENGTH "${CMAKE_BINARY_DIR}" WF_BINARY_DIR_LENGTH)
if(BUILD_TESTING AND WF_BINARY_DIR_LENGTH GREATER 90)
  message(WARNING "Build directory is ${WF_BINARY_DIR_LENGTH} characters long; "
    "path-length-sensitive tests (storage_evidence, hash_dependency, "
    "copy_acceptance, recovery_mutations) can fail. Use a shorter -B directory.")
endif()

# libarchive and the decompressors it needs (other people's code, own scope).
add_subdirectory(src/third_party)

include(cmake/product.cmake)
include(cmake/syntax.cmake)
include(cmake/windows_package.cmake)
if(BUILD_TESTING)
  include(cmake/benchmarks.cmake)
  include(cmake/p0_tools.cmake)
  include(cmake/tests.cmake)
  include(cmake/gate_mutant.cmake)
  include(cmake/test_resources.cmake)
  if(WF_SANITIZE)
    # Configure-time set(ENV) does not survive into a later ctest process.
    # Apply the exact selected compiler runtime directory to test children.
    get_property(WF_TEST_NAMES DIRECTORY PROPERTY TESTS)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
      # Inherit the test host's PATH, not a snapshot of the configure shell's
      # VS environment: importing VS again can exceed cmd.exe's line limit.
      set_property(TEST ${WF_TEST_NAMES} APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${WF_ASAN_DLL_DIR}")
    else()
      string(REPLACE ";" "\\;" WF_TEST_RUNTIME_PATH "${WF_ASAN_DLL_DIR};$ENV{PATH}")
      set_property(TEST ${WF_TEST_NAMES} APPEND PROPERTY ENVIRONMENT
        "PATH=${WF_TEST_RUNTIME_PATH}")
    endif()
  endif()
endif()

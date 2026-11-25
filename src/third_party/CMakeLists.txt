# Vendored archive libraries, decompressors and optional syntax parsers.
#
# These targets do not inherit piero's warning policy. Archive targets select
# readers, decompressors and shared helpers; some selected sources also contain
# encoding routines. No upstream tools or tests are built. Distribution notices
# live in LICENSES/ and are copied beside the executable.

set_directory_properties(PROPERTIES COMPILE_OPTIONS "" COMPILE_DEFINITIONS "")

set(WF_TP ${CMAKE_CURRENT_SOURCE_DIR})
set(WF_TP_FLAGS /w)
# Code that parses headers once per archive is built for size; the loops that
# decompress (zlib, liblzma, zstd) are not. Measured in tools/bench_archive.c:
# -Os on libarchive's own code costs nothing visible and saves ~90 KiB.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
  set(WF_TP_SIZE /clang:-Os)
else()
  set(WF_TP_SIZE /O1)
endif()

# ---- zlib 1.3.1: inflate only -------------------------------------------------
set(ZL ${WF_TP}/zlib-1.3.1)
add_library(wf_zlib STATIC
  ${ZL}/adler32.c ${ZL}/crc32.c ${ZL}/inffast.c ${ZL}/inflate.c
  ${ZL}/inftrees.c ${ZL}/zutil.c)
target_include_directories(wf_zlib PUBLIC ${ZL})
target_compile_definitions(wf_zlib PRIVATE _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
target_compile_options(wf_zlib PRIVATE ${WF_TP_FLAGS})

# ---- liblzma 5.8.1: the decoders (xz, lzma, lzma2, BCJ, delta) ------------------
set(XZ ${WF_TP}/xz-5.8.1/src)
set(LZ ${XZ}/liblzma)
add_library(wf_lzma STATIC
  ${LZ}/common/block_util.c ${LZ}/common/common.c ${LZ}/common/easy_preset.c
  ${LZ}/common/filter_common.c ${LZ}/common/index.c
  ${LZ}/common/stream_flags_common.c ${LZ}/common/vli_size.c
  ${LZ}/check/check.c ${LZ}/check/crc32_fast.c ${LZ}/check/crc64_fast.c
  ${LZ}/check/sha256.c
  ${LZ}/common/alone_decoder.c ${LZ}/common/auto_decoder.c
  ${LZ}/common/block_buffer_decoder.c ${LZ}/common/block_decoder.c
  ${LZ}/common/block_header_decoder.c ${LZ}/common/easy_decoder_memusage.c
  ${LZ}/common/file_info.c ${LZ}/common/filter_buffer_decoder.c
  ${LZ}/common/filter_decoder.c ${LZ}/common/filter_flags_decoder.c
  ${LZ}/common/index_decoder.c ${LZ}/common/index_hash.c
  ${LZ}/common/stream_buffer_decoder.c ${LZ}/common/stream_decoder.c
  ${LZ}/common/stream_flags_decoder.c ${LZ}/common/vli_decoder.c
  ${LZ}/common/microlzma_decoder.c ${LZ}/common/lzip_decoder.c
  ${LZ}/lz/lz_decoder.c ${LZ}/lzma/lzma_decoder.c ${LZ}/lzma/lzma2_decoder.c
  ${LZ}/lzma/lzma_encoder_presets.c
  ${LZ}/delta/delta_decoder.c ${LZ}/delta/delta_common.c
  ${LZ}/simple/simple_decoder.c ${LZ}/simple/simple_coder.c
  ${LZ}/simple/x86.c ${LZ}/simple/arm.c ${LZ}/simple/armthumb.c
  ${LZ}/simple/arm64.c ${LZ}/simple/powerpc.c ${LZ}/simple/ia64.c
  ${LZ}/simple/sparc.c ${LZ}/simple/riscv.c)
target_include_directories(wf_lzma
  PUBLIC ${LZ}/api
  PRIVATE ${LZ}/common ${LZ}/check ${LZ}/lz ${LZ}/rangecoder ${LZ}/lzma
          ${LZ}/delta ${LZ}/simple ${XZ}/common)
target_compile_definitions(wf_lzma PRIVATE
  HAVE_CHECK_CRC32 HAVE_CHECK_CRC64 HAVE_CHECK_SHA256 HAVE_DECODERS
  HAVE_DECODER_ARM HAVE_DECODER_ARM64 HAVE_DECODER_ARMTHUMB HAVE_DECODER_DELTA
  HAVE_DECODER_IA64 HAVE_DECODER_LZMA1 HAVE_DECODER_LZMA2
  HAVE_DECODER_POWERPC HAVE_DECODER_RISCV HAVE_DECODER_SPARC HAVE_DECODER_X86
  HAVE_LZIP_DECODER HAVE_INTTYPES_H HAVE_STDBOOL_H HAVE_STDINT_H
  HAVE_MF_BT2 HAVE_MF_BT3 HAVE_MF_BT4 HAVE_MF_HC3 HAVE_MF_HC4
  TUKLIB_FAST_UNALIGNED_ACCESS TUKLIB_SYMBOL_PREFIX=lzma_
  _CRT_SECURE_NO_DEPRECATE)
# lzma.h wants this when it is used from a static library on Windows.
target_compile_definitions(wf_lzma PUBLIC LZMA_API_STATIC)
target_compile_options(wf_lzma PRIVATE ${WF_TP_FLAGS})

# ---- zstd 1.5.7: the decompressor ----------------------------------------------
set(ZS ${WF_TP}/zstd-1.5.7/lib)
add_library(wf_zstd STATIC
  ${ZS}/common/debug.c ${ZS}/common/entropy_common.c ${ZS}/common/error_private.c
  ${ZS}/common/fse_decompress.c ${ZS}/common/xxhash.c ${ZS}/common/zstd_common.c
  ${ZS}/decompress/huf_decompress.c ${ZS}/decompress/zstd_ddict.c
  ${ZS}/decompress/zstd_decompress.c ${ZS}/decompress/zstd_decompress_block.c)
target_include_directories(wf_zstd PUBLIC ${ZS} PRIVATE ${ZS}/common ${ZS}/decompress)
target_compile_definitions(wf_zstd PRIVATE
  ZSTD_DISABLE_ASM ZSTD_LEGACY_SUPPORT=0
  # one Huffman decoder instead of two: ~25 KiB smaller, a few percent slower
  # on the data that is hardest to compress
  HUF_FORCE_DECOMPRESS_X1 XXH_NAMESPACE=ZSTD_ _CRT_SECURE_NO_DEPRECATE)
target_compile_options(wf_zstd PRIVATE ${WF_TP_FLAGS})

# ---- libarchive 3.8.10: reading ZIP, 7z, TAR and raw streams ---------------------
set(LA ${WF_TP}/libarchive-3.8.10/libarchive)
add_library(wf_libarchive STATIC
  ${LA}/archive_acl.c ${LA}/archive_check_magic.c ${LA}/archive_crc32.c ${LA}/archive_cryptor.c
  ${LA}/archive_digest.c ${LA}/archive_entry.c ${LA}/archive_entry_copy_stat.c
  ${LA}/archive_entry_sparse.c ${LA}/archive_entry_stat.c
  ${LA}/archive_entry_strmode.c ${LA}/archive_entry_xattr.c
  ${LA}/archive_hmac.c ${LA}/archive_options.c ${LA}/archive_pack_dev.c
  ${LA}/archive_parse_date.c ${LA}/archive_ppmd7.c ${LA}/archive_ppmd8.c
  ${LA}/archive_random.c ${LA}/archive_rb.c ${LA}/archive_read.c
  ${LA}/archive_read_add_passphrase.c ${LA}/archive_read_append_filter.c
  ${LA}/archive_read_open_memory.c ${LA}/archive_read_set_format.c
  ${LA}/archive_read_set_options.c
  ${LA}/archive_read_support_filter_gzip.c
  ${LA}/archive_read_support_filter_xz.c
  ${LA}/archive_read_support_filter_zstd.c
  ${LA}/archive_read_support_format_7zip.c
  ${LA}/archive_read_support_format_raw.c
  ${LA}/archive_read_support_format_tar.c
  ${LA}/archive_read_support_format_zip.c
  ${LA}/archive_string.c ${LA}/archive_string_sprintf.c ${LA}/archive_time.c
  ${LA}/archive_util.c ${LA}/archive_version_details.c ${LA}/archive_virtual.c
  ${LA}/archive_windows.c ${LA}/archive_entry_copy_bhfi.c ${LA}/xxhash.c)
target_include_directories(wf_libarchive
  PUBLIC ${LA}
  PRIVATE ${WF_TP}/libarchive-config)
target_compile_definitions(wf_libarchive PUBLIC LIBARCHIVE_STATIC)
target_compile_definitions(wf_libarchive PRIVATE HAVE_CONFIG_H
  _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
target_compile_options(wf_libarchive PRIVATE ${WF_TP_FLAGS}
  "$<$<NOT:$<CONFIG:Debug>>:${WF_TP_SIZE}>")
target_link_libraries(wf_libarchive PUBLIC wf_zlib wf_lzma wf_zstd bcrypt)

# ---- Tree-sitter 0.27.2 and bundled grammars (DR-DRAFT-033) ----------------------
# The runtime is compiled into syntax/piero-syntax.dll only (piero.exe never links
# it). Each grammar is its own DLL, syntax/grammars/<id>/parser.dll, with one
# export. Nothing of this is linked into piero.exe.
set(TS ${WF_TP}/tree-sitter-0.27.2)
add_library(wf_treesitter STATIC ${TS}/src/lib.c)
target_include_directories(wf_treesitter PUBLIC ${TS}/include PRIVATE ${TS}/src)
target_compile_definitions(wf_treesitter PRIVATE TREE_SITTER_HIDE_SYMBOLS
  _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
target_compile_options(wf_treesitter PRIVATE ${WF_TP_FLAGS}
  "$<$<NOT:$<CONFIG:Debug>>:${WF_TP_SIZE}>")

if(NOT WF_SANITIZE)
  # inside piero-syntax.dll, which has its own C runtime (no runtime DLL to find)
  set_target_properties(wf_treesitter PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
# the same code with the runtime the test programs use (tests link the loader directly)
add_library(wf_treesitter_dyn STATIC ${TS}/src/lib.c)
target_include_directories(wf_treesitter_dyn PUBLIC ${TS}/include PRIVATE ${TS}/src)
target_compile_definitions(wf_treesitter_dyn PRIVATE TREE_SITTER_HIDE_SYMBOLS
  _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
target_compile_options(wf_treesitter_dyn PRIVATE ${WF_TP_FLAGS}
  "$<$<NOT:$<CONFIG:Debug>>:${WF_TP_SIZE}>")

set(WF_TS_GRAMMARS json jsonc yaml toml ini xml markdown markdown_inline powershell bash)
foreach(id IN LISTS WF_TS_GRAMMARS)
  set(G ${WF_TP}/ts-grammars/${id})
  set(srcs ${G}/src/parser.c)
  if(EXISTS ${G}/src/scanner.c)
    list(APPEND srcs ${G}/src/scanner.c)
  endif()
  add_library(wf_grammar_${id} SHARED ${srcs})
  set_target_properties(wf_grammar_${id} PROPERTIES
    OUTPUT_NAME parser PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/syntax/grammars/${id}"
    PDB_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/syntax/grammars/${id}"
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/syntax/lib/${id}")
  target_include_directories(wf_grammar_${id} PRIVATE ${G}/src)
  target_compile_definitions(wf_grammar_${id} PRIVATE
    _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
  target_compile_options(wf_grammar_${id} PRIVATE ${WF_TP_FLAGS}
    "$<$<NOT:$<CONFIG:Debug>>:${WF_TP_SIZE}>")
  # one export, and System32 as the only default place for the DLLs it needs
  target_link_options(wf_grammar_${id} PRIVATE
    /EXPORT:tree_sitter_${id} /DEPENDENTLOADFLAG:0x800 /OPT:REF /OPT:ICF)
  if(NOT WF_SANITIZE)
    # the grammar imports kernel32 and nothing else: no C runtime DLL to find
    set_target_properties(wf_grammar_${id} PROPERTIES
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
endforeach()
add_custom_target(wf_grammars DEPENDS wf_grammar_json wf_grammar_jsonc wf_grammar_yaml
  wf_grammar_toml wf_grammar_ini wf_grammar_xml wf_grammar_markdown
  wf_grammar_markdown_inline wf_grammar_powershell wf_grammar_bash)

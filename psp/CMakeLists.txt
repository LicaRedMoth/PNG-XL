# Separate CMake project, not a subdirectory of the main one: it needs the
# psp-gcc cross-toolchain (CMAKE_TOOLCHAIN_FILE=$PSPDEV/psp/share/pspdev.cmake)
# active from the first configure call, and CMake does not support switching
# toolchains partway through one build tree. See psp/build.sh.
cmake_minimum_required(VERSION 3.10)
project(pxl_psp_bench C)

set(PXL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(ZSTD_LIB ${PXL_ROOT}/vendor/zstd/lib)

# Decode-only source set: the same split wasm/build.sh uses for the
# freestanding wasm32 decoder, and for the same reason -- pxl_codec_encode.c
# pulls in the zstd compressor, which a decode-only target has no use for.
add_executable(pxl_psp_bench
    main.c
    ${PXL_ROOT}/src/pxl_codec_common.c
    ${PXL_ROOT}/src/pxl_codec_decode.c
    ${PXL_ROOT}/src/pxl_io.c
    ${ZSTD_LIB}/common/debug.c
    ${ZSTD_LIB}/common/entropy_common.c
    ${ZSTD_LIB}/common/error_private.c
    ${ZSTD_LIB}/common/fse_decompress.c
    ${ZSTD_LIB}/common/xxhash.c
    ${ZSTD_LIB}/common/zstd_common.c
    ${ZSTD_LIB}/decompress/huf_decompress.c
    ${ZSTD_LIB}/decompress/zstd_ddict.c
    ${ZSTD_LIB}/decompress/zstd_decompress.c
    ${ZSTD_LIB}/decompress/zstd_decompress_block.c
)
target_include_directories(pxl_psp_bench PRIVATE
    ${PXL_ROOT}/src
    ${ZSTD_LIB}
    ${ZSTD_LIB}/common
    ${CMAKE_CURRENT_SOURCE_DIR}
)
# ZSTD_DISABLE_ASM: no x86 assembly path is relevant on MIPS anyway, but this
# also skips an asm-detection code path in zstd that assumes a host it isn't
# running on. ZSTD_LEGACY_SUPPORT/ZSTD_MULTITHREAD=0 match the wasm build:
# this is a single-threaded decode-only target.
target_compile_definitions(pxl_psp_bench PRIVATE
    ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0 ZSTD_MULTITHREAD=0)
# psp-gcc's own specs already link the base pspuser/pspkernel/libc PSPSDK
# needs; main.c uses nothing beyond sceIoWrite/sceRtcGetCurrentTick/
# sceKernelExitGame, all of which live there, so nothing extra is added here
# on a guess -- see psp/README.md if a real build turns out to need one.

create_pbp_file(
    TARGET pxl_psp_bench
    TITLE "PXL decode bench"
    VERSION 01.00
)

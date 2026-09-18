cmake_minimum_required(VERSION 3.10)
# Keep VERSION in sync with pxl_version() in src/pxl_codec.c -- the pkg-config
# files below interpolate it, and FFmpeg's configure checks it via pkg-config.
project(pxl VERSION 1.5.0 LANGUAGES C)

# PNG XL (.pxl): a lossless image format/library built on libpng + zstd.

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT MSVC)
    add_compile_options(-Wall -Wextra)
    set(CMAKE_C_FLAGS_RELEASE "-O3")
endif()

# Build everything we own with AddressSanitizer + UBSan. Used by CI to soak the
# decoders, which run on untrusted input. Applied globally (not per-target) so
# the sanitized runtime covers the library, the CLI, and both test binaries.
option(PXL_SANITIZE "Build with AddressSanitizer + UBSan" OFF)
if(PXL_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# Build against the git-submoduled vendor/libpng/ and vendor/zstd/ sources
# instead of the system libraries. Used by CI for reproducible, version-
# pinned rebuilds.
option(PXL_VENDORED "Build against bundled libpng/zstd sources" OFF)

#-----------------------------------------------------------------------------
# Dependencies
#-----------------------------------------------------------------------------
if(PXL_VENDORED)
    set(PXL_PNG_DIR  "${CMAKE_SOURCE_DIR}/vendor/libpng" CACHE PATH "libpng source dir")
    set(PXL_ZSTD_DIR "${CMAKE_SOURCE_DIR}/vendor/zstd"   CACHE PATH "zstd source dir")

    # Vendored deps are static, but libpxl.so links them in, so they must be
    # position-independent.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    # zstd static library (self-contained CMake project).
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
    # PXL never reads pre-1.x zstd frames: it only decodes frames written by
    # its own encoder. Legacy support is ~150 KB of dead weight in a decoder.
    set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
    set(ZSTD_MULTITHREAD    OFF CACHE BOOL "" FORCE)
    add_subdirectory("${PXL_ZSTD_DIR}/build/cmake" zstd_build)
    set(PXL_ZSTD_TARGET libzstd_static)
    set(PXL_ZSTD_INCLUDE "${PXL_ZSTD_DIR}/lib")

    # libpng (depends on system zlib).
    # pkg_check_modules(PNG ...) in the non-vendored branch writes an internal
    # cache entry named PNG_PREFIX (a filesystem prefix such as /usr). Vendored
    # libpng uses the same name for its API symbol prefix, so a leftover cache
    # entry from a previous non-vendored configure corrupts pnglibconf.h. Force
    # it back to the standard "no prefix" value.
    set(PNG_PREFIX "" CACHE STRING "" FORCE)
    set(PNG_SHARED OFF CACHE BOOL "" FORCE)
    set(PNG_STATIC ON  CACHE BOOL "" FORCE)
    set(PNG_TESTS  OFF CACHE BOOL "" FORCE)
    set(PNG_TOOLS  OFF CACHE BOOL "" FORCE)
    add_subdirectory("${PXL_PNG_DIR}" png_build)
    set(PXL_PNG_TARGET png_static)
    set(PXL_PNG_INCLUDE "${PXL_PNG_DIR}" "${CMAKE_BINARY_DIR}/png_build")

    # crc32 for metadata injection comes from system zlib (also needed by png).
    find_package(ZLIB REQUIRED)
    set(PXL_ZLIB_TARGET ZLIB::ZLIB)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PNG  REQUIRED IMPORTED_TARGET libpng)
    pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
    pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
    set(PXL_PNG_TARGET  PkgConfig::PNG)
    set(PXL_ZSTD_TARGET PkgConfig::ZSTD)
    set(PXL_ZLIB_TARGET PkgConfig::ZLIB)
endif()

#-----------------------------------------------------------------------------
# libpxlcore (static + shared): PNG-free codec, depends only on zstd.
# This is the library an ffmpeg codec or other front end reuses.
#-----------------------------------------------------------------------------
# The codec is three translation units, not one, so that a decode-only program
# links neither pxl_encode nor the zstd compressor behind it (~317 KB of .text).
# A linker discards unused object files, never half of one.
set(PXLCORE_SOURCES
    src/pxl_codec_common.c
    src/pxl_codec_encode.c
    src/pxl_codec_decode.c
    src/pxl_io.c
    src/apxl_codec.c
    src/gif.c)

add_library(pxlcore_obj OBJECT ${PXLCORE_SOURCES})
set_property(TARGET pxlcore_obj PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(pxlcore_obj PUBLIC src)
if(PXL_VENDORED)
    target_include_directories(pxlcore_obj PRIVATE ${PXL_ZSTD_INCLUDE})
endif()
target_link_libraries(pxlcore_obj PUBLIC ${PXL_ZSTD_TARGET})

add_library(pxlcore        SHARED $<TARGET_OBJECTS:pxlcore_obj>)
add_library(pxlcore_static STATIC $<TARGET_OBJECTS:pxlcore_obj>)
foreach(t pxlcore pxlcore_static)
    target_include_directories(${t} PUBLIC src)
    target_link_libraries(${t} PUBLIC ${PXL_ZSTD_TARGET})
endforeach()
set_target_properties(pxlcore_static PROPERTIES OUTPUT_NAME pxlcore)

#-----------------------------------------------------------------------------
# libpxl (static + shared): full library = core + PNG interop (libpng + zlib).
#-----------------------------------------------------------------------------
set(PXL_SOURCES
    src/pxl_meta.c
    src/pxl_pngio.c
    src/pxl_png.c
    src/apng.c)

add_library(pxl_obj OBJECT ${PXL_SOURCES})
set_property(TARGET pxl_obj PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(pxl_obj PUBLIC src)
if(PXL_VENDORED)
    target_include_directories(pxl_obj PRIVATE ${PXL_PNG_INCLUDE} ${PXL_ZSTD_INCLUDE})
endif()
target_link_libraries(pxl_obj PUBLIC ${PXL_PNG_TARGET} ${PXL_ZSTD_TARGET} ${PXL_ZLIB_TARGET})

add_library(pxl        SHARED $<TARGET_OBJECTS:pxl_obj> $<TARGET_OBJECTS:pxlcore_obj>)
add_library(pxl_static STATIC $<TARGET_OBJECTS:pxl_obj> $<TARGET_OBJECTS:pxlcore_obj>)
foreach(t pxl pxl_static)
    target_include_directories(${t} PUBLIC src)
    target_link_libraries(${t} PUBLIC ${PXL_PNG_TARGET} ${PXL_ZSTD_TARGET} ${PXL_ZLIB_TARGET})
endforeach()
set_target_properties(pxl_static PROPERTIES OUTPUT_NAME pxl)

#-----------------------------------------------------------------------------
# CLI
#-----------------------------------------------------------------------------
add_executable(pxltool tools/pxltool.c)
target_link_libraries(pxltool PRIVATE pxl_static)

# Test bench for candidate predictors. Not installed: it neither reads nor
# writes the format, it only measures the size of filter variants.
add_executable(predlab tools/predlab.c)
target_link_libraries(predlab PRIVATE pxl_static)
if(PXL_VENDORED)
    target_include_directories(predlab PRIVATE ${PXL_ZSTD_INCLUDE})
endif()

# Decode-only profiling target. Kept separate from pxl_bench_rawdec so that a
# profiler sees the decoder alone, without libpng and QOI in the same profile.
add_executable(pxl_bench_profdec bench/profdec.c)
target_link_libraries(pxl_bench_profdec PRIVATE pxl_static)
if(PXL_VENDORED)
    target_include_directories(pxl_bench_profdec PRIVATE ${PXL_ZSTD_INCLUDE})
endif()

# Pure-decode benchmark (compressed bytes -> pixels): PXL against QOI and
# libpng. Built only when a vendored qoi.h is present, since it is the baseline.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/qoi/qoi.h")
    add_executable(pxl_bench_rawdec bench/rawdec.c)
    target_link_libraries(pxl_bench_rawdec PRIVATE pxl_static)
    target_include_directories(pxl_bench_rawdec PRIVATE
                               "${CMAKE_CURRENT_SOURCE_DIR}/vendor/qoi")
    if(PXL_VENDORED)
        target_include_directories(pxl_bench_rawdec PRIVATE ${PXL_ZSTD_INCLUDE})
    endif()
endif()

# Decode time split by stage: zstd against unfiltering. It includes the codec's
# encode/decode units directly because the stage helpers are static, and exposing
# them would widen the public API for a benchmark. Hence linking the dependencies
# directly rather than pxl_static, which would duplicate the symbols.
add_executable(pxl_bench_stages bench/stages.c
                                src/pxl_codec_common.c
                                src/pxl_png.c
                                src/pxl_pngio.c
                                src/pxl_meta.c
                                src/pxl_io.c)
target_include_directories(pxl_bench_stages PRIVATE src)
target_link_libraries(pxl_bench_stages PRIVATE
                      ${PXL_ZSTD_TARGET} ${PXL_PNG_TARGET} ${PXL_ZLIB_TARGET})
if(PXL_VENDORED)
    target_include_directories(pxl_bench_stages PRIVATE ${PXL_ZSTD_INCLUDE})
    target_include_directories(pxl_bench_stages PRIVATE ${PXL_PNG_INCLUDE})
endif()

# What excluding ADAPTIVE, on top of what -p already excludes (BCIF), would
# cost in file size -- see bench/adaptivecost.c for why this needed
# measuring rather than reasoning from the PSP throughput numbers alone.
add_executable(pxl_bench_adaptivecost bench/adaptivecost.c
                                      src/pxl_codec_common.c
                                      src/pxl_png.c
                                      src/pxl_pngio.c
                                      src/pxl_meta.c
                                      src/pxl_io.c)
target_include_directories(pxl_bench_adaptivecost PRIVATE src)
target_link_libraries(pxl_bench_adaptivecost PRIVATE
                      ${PXL_ZSTD_TARGET} ${PXL_PNG_TARGET} ${PXL_ZLIB_TARGET})
if(PXL_VENDORED)
    target_include_directories(pxl_bench_adaptivecost PRIVATE ${PXL_ZSTD_INCLUDE})
    target_include_directories(pxl_bench_adaptivecost PRIVATE ${PXL_PNG_INCLUDE})
endif()

# Whether storing pixels pre-packed into a native PSP/GE format (RGB565,
# RGBA5551) compresses smaller and/or decodes faster than 8-bit-per-channel --
# see bench/packedformat.c for why this repeats-with-a-different-layout the
# already-rejected packed-10-bit experiment rather than reusing its answer.
add_executable(pxl_bench_packedformat bench/packedformat.c
                                      src/pxl_codec_common.c
                                      src/pxl_png.c
                                      src/pxl_pngio.c
                                      src/pxl_meta.c
                                      src/pxl_io.c)
target_include_directories(pxl_bench_packedformat PRIVATE src)
target_link_libraries(pxl_bench_packedformat PRIVATE
                      ${PXL_ZSTD_TARGET} ${PXL_PNG_TARGET} ${PXL_ZLIB_TARGET})
if(PXL_VENDORED)
    target_include_directories(pxl_bench_packedformat PRIVATE ${PXL_ZSTD_INCLUDE})
    target_include_directories(pxl_bench_packedformat PRIVATE ${PXL_PNG_INCLUDE})
endif()

# Generates psp/testdata.h: one valid .pxl file per (size, color filter) pair,
# forced (not chosen by the encoder) so the PSP program can exercise all four,
# plus one PNG per size (libpng's own default settings) as the baseline the
# on-device benchmark compares PXL's decode against. Host tool only -- never
# runs on the PSP itself. See bench/mkpsptest.c and psp/README.md.
add_executable(pxl_bench_mkpsptest bench/mkpsptest.c src/pxl_codec_common.c src/pxl_io.c)
target_include_directories(pxl_bench_mkpsptest PRIVATE src)
target_link_libraries(pxl_bench_mkpsptest PRIVATE ${PXL_ZSTD_TARGET} ${PXL_PNG_TARGET} ${PXL_ZLIB_TARGET})
if(PXL_VENDORED)
    target_include_directories(pxl_bench_mkpsptest PRIVATE ${PXL_ZSTD_INCLUDE} ${PXL_PNG_INCLUDE})
endif()

# Whether overlapping zstd decompression with unfiltering on a second thread
# beats doing them back to back. Includes the codec units directly for the same
# reason pxl_bench_stages does: unfilter_row and decode_rows_bounded are static.
add_executable(pxl_bench_pipeline bench/pipeline.c
                                  src/pxl_codec_common.c
                                  src/pxl_png.c
                                  src/pxl_pngio.c
                                  src/pxl_meta.c
                                  src/pxl_io.c)
target_include_directories(pxl_bench_pipeline PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(pxl_bench_pipeline PRIVATE
                      ${PXL_ZSTD_TARGET} ${PXL_PNG_TARGET} ${PXL_ZLIB_TARGET}
                      Threads::Threads)
if(PXL_VENDORED)
    target_include_directories(pxl_bench_pipeline PRIVATE ${PXL_ZSTD_INCLUDE})
    target_include_directories(pxl_bench_pipeline PRIVATE ${PXL_PNG_INCLUDE})
endif()

# Where encode time goes (filtering vs zstd, per candidate filter), and whether
# a cheap probe level ranks the candidates the same way the full level does.
# Includes pxl_codec_encode.c directly for the same reason pxl_bench_stages does:
# apply_filter is static.
add_executable(pxl_bench_encstages bench/encstages.c
                                   src/pxl_codec_common.c
                                   src/pxl_png.c
                                   src/pxl_pngio.c
                                   src/pxl_meta.c
                                   src/pxl_io.c)
target_include_directories(pxl_bench_encstages PRIVATE src)
target_link_libraries(pxl_bench_encstages PRIVATE
                      ${PXL_ZSTD_TARGET} ${PXL_PNG_TARGET} ${PXL_ZLIB_TARGET})
if(PXL_VENDORED)
    target_include_directories(pxl_bench_encstages PRIVATE ${PXL_ZSTD_INCLUDE})
    target_include_directories(pxl_bench_encstages PRIVATE ${PXL_PNG_INCLUDE})
endif()

# Cross-format decode speed, in process, every format to the same RGBA8888
# destination. Optional: only built when libjxl, libwebp and libavif are found,
# since timing their CLI tools instead would measure process startup and file
# writing rather than decoding.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FMTDEC QUIET libjxl libwebp libavif)
endif()
if(FMTDEC_FOUND)
    add_executable(pxl_bench_formatdec bench/formatdec.c)
    target_include_directories(pxl_bench_formatdec PRIVATE src ${FMTDEC_INCLUDE_DIRS})
    target_link_libraries(pxl_bench_formatdec PRIVATE
                          pxlcore ${FMTDEC_LIBRARIES} ${PXL_PNG_TARGET} ${PXL_ZLIB_TARGET})
    target_link_directories(pxl_bench_formatdec PRIVATE ${FMTDEC_LIBRARY_DIRS})
else()
    message(STATUS "libjxl/libwebp/libavif not found: skipping pxl_bench_formatdec")
endif()

# Dumps the filtered stream the encoder feeds to zstd, so a dictionary can be
# trained on the right material. Includes pxl_codec_encode.c directly because
# apply_filter is static, same as pxl_bench_stages.
add_executable(pxl_bench_dumpfiltered bench/dumpfiltered.c
                                      src/pxl_codec_common.c
                                      src/pxl_png.c
                                      src/pxl_pngio.c
                                      src/pxl_meta.c
                                      src/pxl_io.c)
target_include_directories(pxl_bench_dumpfiltered PRIVATE src)
target_link_libraries(pxl_bench_dumpfiltered PRIVATE
                      ${PXL_ZSTD_TARGET} ${PXL_PNG_TARGET} ${PXL_ZLIB_TARGET})
if(PXL_VENDORED)
    target_include_directories(pxl_bench_dumpfiltered PRIVATE ${PXL_ZSTD_INCLUDE})
    target_include_directories(pxl_bench_dumpfiltered PRIVATE ${PXL_PNG_INCLUDE})
endif()

# How much of a frame only a displaced copy can reach -- the headroom a MOVE
# primitive would have over the plain LZ matches APXL already gets from zstd.
add_executable(pxl_bench_motion bench/motion.c)
target_link_libraries(pxl_bench_motion PRIVATE pxl)
target_include_directories(pxl_bench_motion PRIVATE src)

# Whether zstd long-distance matching actually helps APXL's cross-frame
# stream, per shot, with the compression level held fixed so LDM is the only
# variable -- see bench/ldmsweep.c for why apxl_encode() itself can't answer
# this (LDM is tied to level, not exposed as its own knob).
add_executable(pxl_bench_ldmsweep bench/ldmsweep.c)
target_link_libraries(pxl_bench_ldmsweep PRIVATE pxl)
target_include_directories(pxl_bench_ldmsweep PRIVATE src)
if(PXL_VENDORED)
    target_include_directories(pxl_bench_ldmsweep PRIVATE ${PXL_ZSTD_INCLUDE})
endif()

# Compresses an arbitrary raw file with apxl_encode's real default zstd
# parameters -- lets an experiment on a byte layout apxl_encode never
# produces (e.g. indexed animation frames) be scored by the same compression
# a real .apxl file would get, without reimplementing apxl_encode's container.
add_executable(pxl_bench_rawzstd bench/rawzstd.c)
target_link_libraries(pxl_bench_rawzstd PRIVATE pxl)
target_include_directories(pxl_bench_rawzstd PRIVATE src)
if(PXL_VENDORED)
    target_include_directories(pxl_bench_rawzstd PRIVATE ${PXL_ZSTD_INCLUDE})
endif()

# Which per-row filters the adaptive filter actually picks, over a corpus.
# Includes pxl_codec_encode.c directly for the same reason pxl_bench_stages does:
# the row-filter helpers are static.
add_executable(pxl_bench_rowstats bench/rowstats.c
                                  src/pxl_codec_common.c
                                  src/pxl_png.c
                                  src/pxl_pngio.c
                                  src/pxl_meta.c
                                  src/pxl_io.c)
target_include_directories(pxl_bench_rowstats PRIVATE src)
target_link_libraries(pxl_bench_rowstats PRIVATE
                      ${PXL_ZSTD_TARGET} ${PXL_PNG_TARGET} ${PXL_ZLIB_TARGET})
if(PXL_VENDORED)
    target_include_directories(pxl_bench_rowstats PRIVATE ${PXL_ZSTD_INCLUDE})
    target_include_directories(pxl_bench_rowstats PRIVATE ${PXL_PNG_INCLUDE})
endif()

# What the per-row filter choice is worth in bytes, versus forcing one filter.
add_executable(pxl_bench_rowfix bench/rowfix.c
                                src/pxl_codec_common.c
                                src/pxl_png.c
                                src/pxl_pngio.c
                                src/pxl_meta.c
                                src/pxl_io.c)
target_include_directories(pxl_bench_rowfix PRIVATE src)
target_link_libraries(pxl_bench_rowfix PRIVATE
                      ${PXL_ZSTD_TARGET} ${PXL_PNG_TARGET} ${PXL_ZLIB_TARGET})
if(PXL_VENDORED)
    target_include_directories(pxl_bench_rowfix PRIVATE ${PXL_ZSTD_INCLUDE})
    target_include_directories(pxl_bench_rowfix PRIVATE ${PXL_PNG_INCLUDE})
endif()

#-----------------------------------------------------------------------------
# Tests
#-----------------------------------------------------------------------------
enable_testing()
add_executable(pxl_roundtrip tests/roundtrip.c)
target_link_libraries(pxl_roundtrip PRIVATE pxl_static)
add_test(NAME roundtrip
         COMMAND pxl_roundtrip
                 "${CMAKE_CURRENT_BINARY_DIR}/pxl_interop_tmp.png"
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests/data")

# Malformed-input robustness. Links only libpxlcore (no libpng): decoders are the
# attack surface for any front end.
add_executable(pxl_fuzz_decode tests/fuzz_decode.c)
target_link_libraries(pxl_fuzz_decode PRIVATE pxlcore_static)
# Keep the in-suite run short; raise the iteration count for a longer soak.
add_test(NAME fuzz_decode COMMAND pxl_fuzz_decode 20000)

# Metadata paths are attacker-controlled too (chunk lengths from the file) and
# the decoder fuzzer never reaches them. Needs the full lib for pxl_meta and
# zlib's crc32. This is where the 32-bit chunk-length overflow was found.
add_executable(pxl_fuzz_meta tests/fuzz_meta.c)
target_link_libraries(pxl_fuzz_meta PRIVATE pxl_static ${PXL_ZLIB_TARGET})
target_include_directories(pxl_fuzz_meta PRIVATE src)
add_test(NAME fuzz_meta COMMAND pxl_fuzz_meta 20000)

install(TARGETS pxl pxl_static pxlcore pxlcore_static pxltool
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)
# apxl_format.h is public alongside pxl_format.h: a front end that inspects
# container headers (as pxltool does) needs both. pxl_bytes.h / pxl_meta.h /
# pxl_pngio.h stay internal -- only our .c files include them.
install(FILES src/pxl.h src/pxl_png.h src/pxl_format.h
              src/apxl.h src/apxl_format.h src/apng.h src/gif.h DESTINATION include)

# pkg-config metadata. FFmpeg's configure locates optional external libraries
# with require_pkg_config, so the .pc files are what make --enable-libpxl work;
# they are equally what any other out-of-tree consumer expects to find.
foreach(mod libpxlcore libpxl)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/${mod}.pc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/${mod}.pc" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${mod}.pc"
            DESTINATION lib/pkgconfig)
endforeach()

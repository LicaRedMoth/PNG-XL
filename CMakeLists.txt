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
    add_subdirectory("${PXL_ZSTD_DIR}/build/cmake" zstd_build)
    set(PXL_ZSTD_TARGET libzstd_static)
    set(PXL_ZSTD_INCLUDE "${PXL_ZSTD_DIR}/lib")

    # libpng (depends on system zlib).
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
set(PXLCORE_SOURCES
    src/pxl_codec.c
    src/pxl_io.c
    src/apxl_codec.c)

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

install(TARGETS pxl pxl_static pxlcore pxlcore_static pxltool
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)
# apxl_format.h is public alongside pxl_format.h: a front end that inspects
# container headers (as pxltool does) needs both. pxl_bytes.h / pxl_meta.h /
# pxl_pngio.h stay internal -- only our .c files include them.
install(FILES src/pxl.h src/pxl_png.h src/pxl_format.h
              src/apxl.h src/apxl_format.h src/apng.h DESTINATION include)

# pkg-config metadata. FFmpeg's configure locates optional external libraries
# with require_pkg_config, so the .pc files are what make --enable-libpxl work;
# they are equally what any other out-of-tree consumer expects to find.
foreach(mod libpxlcore libpxl)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/${mod}.pc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/${mod}.pc" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${mod}.pc"
            DESTINATION lib/pkgconfig)
endforeach()

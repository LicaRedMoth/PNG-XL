cmake_minimum_required(VERSION 3.16)
project(pxl-kde-imageformat LANGUAGES CXX)

# Two plugins, both read-only (decoding for display) -- pxltool already
# covers encoding:
#
# - kimg_pxl: a QImageIOPlugin giving Gwenview (and any other Qt app) the
#   ability to open .pxl/.apxl.
# - pxlthumbnail: a KIO::ThumbnailCreator giving Dolphin thumbnails.
#   NOTE: this is *not* redundant with kimg_pxl. kio-extras' imagethumbnail.so
#   does not query QImageReader::supportedImageFormats() at runtime -- its
#   MimeTypes list is static metadata baked in at kio-extras' own build time,
#   so it never routes .pxl/.apxl to Qt regardless of which image plugins are
#   installed. Dolphin previews need this second, standalone plugin.
#
# Standalone project, not wired into the root CMakeLists.txt/CI, the same way
# ffmpeg/ is: a thin wrapper over libpxlcore's ABI that must track the format
# but carries no build-system coupling of its own.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# kcoreaddons_add_plugin() builds a STATIC plugin unless this is on -- we
# want pxlthumbnail as an actual loadable .so, same as kimg_pxl.
set(BUILD_SHARED_LIBS ON)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

find_package(Qt6 REQUIRED COMPONENTS Gui)
find_package(KF6CoreAddons REQUIRED)
find_package(KF6KIO REQUIRED)
find_package(ECM REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs6)

find_package(PkgConfig REQUIRED)
# Depends only on libpxlcore (PNG-free core): pxl_decode/apxl_decode already
# return raw pixels, so there is no need to pull in libpxl/libpng here -- same
# minimal-dependency reasoning as the ffmpeg module.
pkg_check_modules(PXLCORE REQUIRED IMPORTED_TARGET libpxlcore)

add_library(kimg_pxl MODULE
    kimg_pxl.cpp
    pxl_handler.cpp
)
# Qt only scans <QT_PLUGIN_PATH>/imageformats/, never the path root, so build
# into that subdirectory. With the plugin sitting at the build root instead,
# QT_PLUGIN_PATH=kde/build silently loads nothing and any already-installed
# copy under /usr answers instead -- which looks like success while actually
# testing the stale plugin.
set_target_properties(kimg_pxl PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/imageformats"
)
target_link_libraries(kimg_pxl PRIVATE Qt6::Gui PkgConfig::PXLCORE)

install(TARGETS kimg_pxl DESTINATION "${KDE_INSTALL_QTPLUGINDIR}/imageformats")
install(FILES x-pxl.xml DESTINATION "${KDE_INSTALL_MIMEDIR}")

kcoreaddons_add_plugin(pxlthumbnail
    SOURCES pxlthumbnail.cpp pxl_handler.cpp
    INSTALL_NAMESPACE "kf6/thumbcreator"
)
set_target_properties(pxlthumbnail PROPERTIES PREFIX "")
target_link_libraries(pxlthumbnail PRIVATE Qt6::Gui KF6::KIOGui PkgConfig::PXLCORE)

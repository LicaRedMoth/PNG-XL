cmake_minimum_required(VERSION 3.16)
project(pxl-kde-imageformat LANGUAGES CXX)

# Qt image plugin for .pxl/.apxl: gives Gwenview the ability to open both
# formats and gives Dolphin thumbnails for free, since kio-extras'
# imagethumbnail plugin just asks QImageReader what it can decode. Read-only
# (decoding for display) -- pxltool already covers encoding.
#
# Standalone project, not wired into the root CMakeLists.txt/CI, the same way
# ffmpeg/ is: a thin wrapper over libpxlcore's ABI that must track the format
# but carries no build-system coupling of its own.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Gui)
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
target_link_libraries(kimg_pxl PRIVATE Qt6::Gui PkgConfig::PXLCORE)

install(TARGETS kimg_pxl DESTINATION "${KDE_INSTALL_QTPLUGINDIR}/imageformats")
install(FILES x-pxl.xml DESTINATION "${KDE_INSTALL_MIMEDIR}")

# PXL plugins for KDE / Qt

Two read-only plugins that make `.pxl` (still) and `.apxl` (animated) files
behave like any other image format on a KDE desktop:

- **`kimg_pxl`** -- a `QImageIOPlugin` teaching Qt to decode both containers.
  With it **Gwenview** displays and browses `.pxl`/`.apxl` like any other
  format, including frame-by-frame animation playback for `.apxl`, and so does
  any other Qt application.
- **`pxlthumbnail`** -- a standalone `KIO::ThumbnailCreator` for **Dolphin**
  previews. This is *not*
  redundant with `kimg_pxl`. `kio-extras`' `imagethumbnail.so` does not ask
  `QImageReader` what it can decode at runtime; its `MimeTypes` list is static
  metadata baked in when `kio-extras` itself was built, so it never routes
  `.pxl`/`.apxl` to Qt no matter which image plugins are installed. The
  thumbnailer declares `image/x-pxl` and `image/x-apxl` itself and decodes
  through the same `PxlHandler`, reading just the first frame for `.apxl`.

Read-only: these plugins only decode for display. Encoding is `pxltool`'s job.

It lives here rather than in its own repository for the same reason as
[`../ffmpeg/`](../ffmpeg/README.md): it is a thin wrapper over `libpxlcore`'s
ABI, not an independent project, and a format change has to land in both
places at once. It is not wired into the root `CMakeLists.txt` or CI.

Depends only on **libpxlcore** (the PNG-free core: `pxl.h`/`apxl.h`), not on
`libpxl`/libpng -- `pxl_decode`/`apxl_decode` already hand back raw pixels,
so there is nothing here for libpng to do.

## Build

Requires Qt6 (`Gui`), `extra-cmake-modules`, and a built/installed
`libpxlcore.pc`. If you haven't installed the root project, point
`PKG_CONFIG_PATH` at the build directory's `.pc` file instead:

```sh
# from the repo root, assuming build/ already exists (cmake -B build && cmake --build build -j)
PKG_CONFIG_PATH="$PWD/build" cmake -B kde/build -S kde
cmake --build kde/build -j
```

## Try it without installing anything

Point Qt at the freshly built plugin directly:

```sh
QT_PLUGIN_PATH="$PWD/kde/build" gwenview tests/data/Animated_PNG_example_bouncing_beach_ball.apxl
```

Qt scans `<QT_PLUGIN_PATH>/imageformats/`, not the path root, which is why the
build puts `kimg_pxl.so` in `kde/build/imageformats/`. Worth knowing because
the failure mode is quiet: if the plugin sits at the build root, Qt finds
nothing there and a previously installed copy under `/usr` answers instead, so
a test can pass while exercising a stale plugin. To be sure you are testing the
local build, temporarily move any installed copy aside, or check that
`QT_DEBUG_PLUGINS=1` names a path inside your build directory.

For Dolphin thumbnails to pick it up, the mime types need to be registered
too, but only in your user's mime database, not the system one:

```sh
mkdir -p ~/.local/share/mime/packages
cp kde/x-pxl.xml ~/.local/share/mime/packages/
update-mime-database ~/.local/share/mime

mkdir -p ~/.local/lib/qt6/plugins/imageformats ~/.local/lib/qt6/plugins/kf6/thumbcreator
cp kde/build/imageformats/kimg_pxl.so ~/.local/lib/qt6/plugins/imageformats/
cp kde/build/kf6/thumbcreator/pxlthumbnail.so ~/.local/lib/qt6/plugins/kf6/thumbcreator/
QT_PLUGIN_PATH="$HOME/.local/lib/qt6/plugins:$QT_PLUGIN_PATH" dolphin
```

None of this touches `/usr` or requires `sudo`.

## System-wide install

This is the step that writes outside your home directory
(`/usr/lib/qt6/plugins/imageformats/`, `/usr/share/mime/packages/`) and needs
root:

```sh
sudo cmake --install kde/build
sudo update-mime-database /usr/share/mime
```

After that, Gwenview and Dolphin pick up `.pxl`/`.apxl` for every user on the
machine without any environment variables.

## Verifying decoding is correct

`magick`/ImageMagick does not load Qt image plugins, so verification needs a
Qt-based tool. `qimgv2png.cpp` below is a tiny throwaway that loads a file
through `QImageReader` (which finds the plugin via `QT_PLUGIN_PATH`) and
saves it as PNG, so the result can be compared against the known-good pixels
the same way [`bench/bench.sh`](../bench/README.md) checks round-trips
(`magick compare -metric AE`, 0 = pixel-identical):

```sh
cat > /tmp/qimgv2png.cpp <<'EOF'
#include <QCoreApplication>
#include <QImage>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QImage img(argv[1]);
    return img.isNull() ? 1 : !img.save(argv[2], "PNG");
}
EOF
g++ -fPIC $(pkg-config --cflags Qt6Gui Qt6Core) /tmp/qimgv2png.cpp -o /tmp/qimgv2png $(pkg-config --libs Qt6Gui Qt6Core)

QT_PLUGIN_PATH="$PWD/kde/build" /tmp/qimgv2png tests/data/RGB_24bits_palette_color_test_chart.pxl /tmp/check.png
magick compare -metric AE tests/data/RGB_24bits_palette_color_test_chart.png /tmp/check.png null:
```

For `.apxl`, check that the reported frame count and per-frame delay match
`pxltool ainfo tests/data/Animated_PNG_example_bouncing_beach_ball.apxl`.

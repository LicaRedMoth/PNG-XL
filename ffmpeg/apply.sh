#!/bin/sh
# Apply the PXL codec module to an FFmpeg checkout.
#
#   ./ffmpeg/apply.sh /path/to/FFmpeg
#
# Copies the four new source files into place and applies the patch that
# registers them (codec IDs, descriptors, Makefiles, configure, img2 probe).
# Then configure FFmpeg with --enable-libpxl; see ffmpeg/README.md.
set -eu

if [ $# -ne 1 ]; then
    echo "usage: $0 /path/to/FFmpeg" >&2
    exit 2
fi

ff=$1
here=$(cd "$(dirname "$0")" && pwd)

[ -f "$ff/configure" ] && [ -d "$ff/libavcodec" ] || {
    echo "error: $ff does not look like an FFmpeg source tree" >&2
    exit 1
}

cp "$here/libavcodec/libpxldec.c" "$here/libavcodec/libpxlenc.c" "$ff/libavcodec/"
cp "$here/libavformat/apxldec.c"  "$here/libavformat/apxlenc.c"  "$ff/libavformat/"

# Reject rather than half-apply: a partially registered codec fails to link in
# ways that are much harder to diagnose than a rejected hunk.
git -C "$ff" apply --check "$here/0001-register-pxl-in-ffmpeg.patch" 2>/dev/null ||
    patch -d "$ff" -p1 --dry-run < "$here/0001-register-pxl-in-ffmpeg.patch" >/dev/null || {
        echo "error: the registration patch does not apply cleanly to $ff" >&2
        echo "       (it was generated against FFmpeg 8.0.git a50d8c7)" >&2
        exit 1
    }

git -C "$ff" apply "$here/0001-register-pxl-in-ffmpeg.patch" 2>/dev/null ||
    patch -d "$ff" -p1 < "$here/0001-register-pxl-in-ffmpeg.patch"

echo "PXL module applied to $ff"
echo "next: PKG_CONFIG_PATH=<prefix>/lib/pkgconfig ./configure --enable-libpxl"

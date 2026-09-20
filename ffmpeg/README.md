# FFmpeg module for PXL

Adds `.pxl` (still) and `.apxl` (animated) support to FFmpeg as a wrapper around
`libpxlcore`, exactly the way `--enable-libjxl` wraps libjxl.

It has to be a wrapper rather than a native codec like `qoidec.c`: `.pxl` is
compressed with Zstandard, and FFmpeg has no zstd anywhere in its tree. A native
decoder would mean vendoring a zstd decompressor into libavcodec.

## What it provides

| component | name | notes |
|---|---|---|
| decoder | `libpxl` | `.pxl` -> frame, all 8 channel/depth combinations |
| encoder | `libpxl` | frame -> `.pxl`, options `-level`, `-progressive` |
| decoder | `libpxl_anim` | `.apxl` -> frame sequence |
| encoder | `libpxl_anim` | frame sequence -> `.apxl` |
| demuxer | `apxl` | whole-file single packet |
| muxer | `apxl` | writes the encoder's single packet |
| demuxer | `pxl_pipe` | content probe on the `PXL1` magic |

`.pxl` is also registered in the `image2` muxer/demuxer, so numbered sequences
(`out%03d.pxl`) work without naming the codec.

## Build

Install libpxl first — the module finds it through pkg-config:

```sh
cd /path/to/PXL
cmake -B build && cmake --build build -j
cmake --install build --prefix "$PWD/_inst"
```

Then apply and configure:

```sh
./ffmpeg/apply.sh /path/to/FFmpeg
cd /path/to/FFmpeg
PKG_CONFIG_PATH=/path/to/PXL/_inst/lib/pkgconfig ./configure --enable-libpxl
make -j
```

`apply.sh` copies the four new source files and applies
`0001-register-pxl-in-ffmpeg.patch`, which touches only registration points
(65 lines across 10 files: codec IDs, descriptors, both Makefiles, `configure`,
`allcodecs.c`, `allformats.c`, and the `img2` tag/probe tables). It refuses to
apply partially — a half-registered codec fails to link in confusing ways.

## Usage

```sh
ffmpeg -i in.png  -c:v libpxl out.pxl          # still, default level
ffmpeg -i in.png  -c:v libpxl -level 19 out.pxl
ffmpeg -i in.png  -c:v libpxl -progressive 1 out.pxl   # row-wise filters only
ffmpeg -i in.pxl  out.png

ffmpeg -i in.apng -c:v libpxl_anim out.apxl    # animation
ffmpeg -i in.apxl -fps_mode passthrough out.apng
```

`-fps_mode passthrough` matters when decoding animation: without it FFmpeg
resamples to a constant frame rate and duplicates frames, since `.apxl` (like
APNG) carries a per-frame delay rather than a fixed rate.

## Pixel formats

All eight combinations map one-to-one, with no conversion:

`gray`, `ya8`, `rgb24`, `rgba`, `gray16be`, `ya16be`, `rgb48be`, `rgba64be`

16-bit samples are stored big-endian in the container (PNG-native order), which
is why the `*be` variants are the native ones — a 16-bit round trip needs no
byte swapping.

## Design notes

**Animation is one packet, not one packet per frame.** `.apxl` compresses every
frame into a single zstd stream so long-distance matching can exploit
frame-to-frame redundancy — that is where its compression comes from. A frame is
therefore not independently decodable. The demuxer hands the whole file over as
one packet and the muxer writes the encoder's single packet; the encoder buffers
all frames and emits the container at EOF. This mirrors how `jpegxl_anim` is
wired.

**Timebase is 1/100000**, matching FFmpeg's APNG demuxer. `.apxl` frame delays
are rationals with 16-bit terms, and a coarser timebase loses them: at 1/1000 a
24 fps delay (1/24 s) rounds to 41 ms and drifts about 1.6% over a long
animation. The constant is duplicated in `libpxldec.c` and `apxldec.c` and the
two must stay in step.

**Metadata maps to what FFmpeg models, and only that.** EXIF and color
information survive in both directions:

| container | FFmpeg |
|---|---|
| `eXIf` | `AV_FRAME_DATA_EXIF` (via `ff_decode_exif_attach_buffer`, so an orientation tag becomes a display matrix) |
| `cICP` | `color_primaries` / `color_trc` / `color_range` |
| `sRGB`, `cHRM` | `color_primaries` (decode only; encoding writes `cICP`) |

Encoding writes `cICP` rather than `iCCP` because FFmpeg models color as CICP
code points, and `cICP` states exactly that in four bytes. Emitting an ICC
profile would mean synthesizing and deflating one, and `libpxlcore` links no
zlib on purpose — being zstd-only is what makes it usable from WASM and from
FFmpeg without pulling in an image library. For the same reason a `.pxl` that
carries an `iCCP` is not surfaced as `AV_FRAME_DATA_ICC_PROFILE`: the payload is
deflate-compressed and inflating it here is not possible. The chunk is preserved
in the container, so a `pxltool` round trip keeps it; only the FFmpeg-side
exposure is missing, and the decoder logs it at verbose level rather than
implying no profile exists.

Chunks FFmpeg has no representation for (`tEXt`, `tIME`, `pHYs`, ...) stay in the
file untouched and are simply not surfaced.

For `.apxl` the metadata block describes the whole file, so the encoder promotes
frame 0's to the animation's and the decoder applies it to every output frame,
since FFmpeg carries these properties per frame.

## Verification

Against FFmpeg 8.0.git (a50d8c7), built clean with no warnings in these four
files:

- All 8 pixel formats: encode then decode is bit-exact.
- `.pxl` written by FFmpeg is read by our own `pxltool`, and vice versa.
- Animation: 34-frame APNG -> `.apxl` -> 34 frames, every frame bit-exact, and
  frame pts/duration identical to the source APNG for all 34 frames.
- `APNG -> .apxl -> APNG` through FFmpeg is bit-exact.
- `image2` numbered sequences round-trip bit-exact.
- Content probing identifies both containers with the extension stripped.
- Truncated and garbage files are rejected with a diagnostic, no crash.
- Color properties through `PNG -> .pxl -> decode` match decoding the source PNG
  directly (`rgb24(pc, gbr/unknown/bt470m)` in both cases).
- `eXIf` survives `PNG -> .pxl -> PNG`, and the result is byte-identical to what
  `PNG -> PNG` through FFmpeg produces — the rewrite is FFmpeg's own IFD
  normalization, not a loss on our side.
- Animation metadata survives `APNG -> .apxl` with pixels still bit-exact.
- Encode and decode paths, still and animated, run clean under AddressSanitizer
  with `detect_leaks=1` (link against a sanitized libpxl build).

# PXL — PNG XL

A small lossless image format and library that pairs **libpng** (for reading and
writing real PNG files) with **Zstandard** (as the compressor). It is a modern
re-implementation of the idea behind [Zpng](https://github.com/catid/Zpng):
filter the pixels with a reversible color transform, then compress the result
with zstd instead of DEFLATE. On photographic content the `.pxl` file is
typically ~65–70% of the equivalent PNG.

`libpxl` exposes a clean C ABI so it can back editor plugins (GIMP, Krita) and
thumbnailers (Dolphin, Windows) later.

## Format philosophy

PXL is a modern lossless image format built around three priorities:
implementation simplicity, fast decoding, and a specification you can
reproduce. The idea is plain — take PNG's understandable architecture, keep its
strengths, drop the obsolete parts, add modern capabilities, and do all that
without turning the format into a sprawling research project.

It is the same PNG, but leaning on speed and on running well on weak hardware.
The target floor is the Sony PSP: 32 MB RAM, a 32-bit little-endian system, no
fast floating-point. That floor is also where the image size limit comes from:
the PSP screen is 480x272, and nobody needs huge pictures there, in any format.

Priorities, in order:

1. **Decode speed.** Decode matters more than encode. A file is encoded once
   and read thousands of times.
2. **Decoder size.** Not cosmetics, but a requirement of the target hardware.
   It is measured and recorded, see below.
3. **A simple, reproducible specification.** The format must be implementable
   from the document, without reading our source.
4. **File size.** At least 15% better than PNG on true-color photographic
   content, which is the content the format is aimed at. We do not claim to
   beat JPEG XL or WebP, and we do not pretend otherwise.

What we honestly do **not** do: chase the world's best compression ratio, add
features that break top-to-bottom streaming decode, or start V2 before V1 has
met its own priorities.

Where those priorities currently stand, per the 2026-07-28 measurements
(see [docs/BENCHMARKS.md](docs/BENCHMARKS.md)):

- Decode speed — **met**: 2.1x faster than libpng into raw pixels. But QOI is
  still about 30% faster than us, on all 24 files of the corpus.
- Decoder size — **not met**: 861 KB against 209 KB for libpng+zlib. Our own
  code is only 19 KB; the rest is libzstd, including the compressor the decoder
  never needs.
- File size — **met on photographs, not in general**: 85.5% of PNG across the
  Kodak photographic corpus, but 97.6% across the wider 396-file corpus. On the
  158 8-bit grayscale USC-SIPI plates we are at 100.3%, i.e. slightly *worse*
  than PNG: the color filter does nothing on one channel, and on high-frequency
  aerials and textures zstd's edge over DEFLATE nearly vanishes.

The journal of ideas, including the rejected ones and the reasons they were
rejected, lives in [docs/RESEARCH.md](docs/RESEARCH.md). The measurement
history is in [docs/BENCHMARKS.md](docs/BENCHMARKS.md), append-only.

## How it works

1. **Load** the source image with libpng into tightly packed pixels.
2. **Filter** — the encoder tries several reversible filters and keeps whichever
   compresses smallest:
   - *delta*: subtract each channel from the pixel to its left (from Zpng);
   - *BCIF* (8-bit RGB/RGBA): the color transform `y=b, u=g−b, v=g−r` plus a
     split into separate color planes (from Zpng);
   - *adaptive*: PNG-style per-row filters (None/Sub/Up/Average/Paeth), choosing
     the best predictor for each row — this matches or beats PNG's own filtering
     while still feeding zstd instead of DEFLATE.
3. **Compress** the filtered bytes with `ZSTD_compress`.

Decoding reverses these steps. All filters are exactly reversible, so `.pxl` is
lossless.

## File format

A 24-byte little-endian header, an optional metadata block, then one zstd frame:

| offset | size | field |
|-------:|-----:|-------|
| 0  | 4 | magic `"PXL1"` |
| 4  | 1 | version (=1) |
| 5  | 1 | channels (1–4) |
| 6  | 1 | bytes per channel (1 or 2) |
| 7  | 1 | color filter (0 delta, 1 BCIF, 2 adaptive) |
| 8  | 4 | width (uint32) |
| 12 | 4 | height (uint32) |
| 16 | 4 | raw byte count |
| 20 | 4 | metadata byte count |
| 24 | … | metadata block, then zstd frame |

## Metadata (EXIF, ICC, HDR, text)

Ancillary PNG chunks are preserved byte-for-byte across a round-trip: `eXIf`
(EXIF), `iCCP` (ICC color profile), `cICP` (HDR / BT.2100 signalling), `gAMA`,
`cHRM`, `sRGB`, `pHYs`, `tIME`, `tEXt`/`zTXt`/`iTXt`, and any unknown ancillary
chunk. They are stored in the metadata block and re-inserted when decoding back
to PNG.

This holds for animation too: an `.apxl` carries one metadata block for the whole
file, filled from the source APNG and written back out by `da`.

Chunks that describe the *original* pixel layout — `PLTE`, `tRNS`, `sBIT`,
`bKGD`, `hIST` — are intentionally **not** carried over, because PXL
canonicalizes palette / transparency / sub-8-bit images into real G/GA/RGB/RGBA
channels, which would make those chunks invalid. Neither are the animation
control chunks `acTL`/`fcTL`/`fdAT`: they are structural in the way `IDAT` is,
carrying the frames and their sequence numbers, which the encoder regenerates.

Through the FFmpeg module the picture is narrower, bounded by what FFmpeg itself
models: EXIF and color information map to frame side data and color properties in
both directions, while chunks it has no representation for stay in the file
without being surfaced. [`ffmpeg/README.md`](ffmpeg/README.md) has the table.

## Build

Requires a C99 compiler, CMake ≥ 3.10, and libpng + zstd.

```sh
cmake -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Building against the bundled sources (`-DPXL_VENDORED=ON`, below) requires the
git submodules under `vendor/`: clone with `git clone --recurse-submodules`,
or run `git submodule update --init --recursive` in an existing checkout. The
default build above (system libpng/zstd) needs neither.

The suite covers lossless round-trips at every bit depth, PNG and APNG interop,
progressive streaming, and a malformed-input fuzz pass over the decoders. It also
runs on real files from [`tests/data/`](tests/data/README.md) — a public-domain
PNG and APNG, plus `.pxl`/`.apxl` encoded by an earlier build. Those two encoded
references are decoded and compared against the source pixels, which is the one
check a round-trip cannot make: a round-trip only proves the encoder agrees with
itself, while these prove today's decoder still reads what an older encoder
wrote.

Since decoders run on untrusted files, they are also soaked under sanitizers
(this is what CI does, and how two memory-safety bugs were found):

```sh
cmake -B build-asan -DPXL_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=1 ./build-asan/pxl_fuzz_decode 300000
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure
```

By default it links the **system** libpng/zstd via pkg-config. To build against
the bundled sources instead (used by CI for reproducible, version-pinned
rebuilds):

```sh
cmake -B build -DPXL_VENDORED=ON
cmake --build build -j
```

## CLI

```sh
pxltool c     in.png  out.pxl  [-l LEVEL] [-p]  # PNG -> PXL (-p = progressive)
pxltool d     in.pxl  out.png              # PXL  -> PNG
pxltool info  in.pxl                       # print header + stats
pxltool ca    in.apng out.apxl [-l LEVEL]  # APNG -> APXL (animation)
pxltool da    in.apxl out.apng             # APXL -> APNG
pxltool ainfo in.apxl                      # print animation header + frame modes
```

`LEVEL` is the zstd compression level (default 1, matching Zpng).

## Animation (`.apxl`)

`.apxl` is the animated container: a canvas plus a sequence of frames. libpng
does not decode APNG, so PXL parses the `acTL`/`fcTL`/`fdAT` chunks itself and
composites each frame onto an RGBA canvas (honoring dispose/blend/offset). All
full-canvas frames are then concatenated and compressed as **one** zstd stream
with long-distance matching, so the compressor reuses the large redundancy
between frames — this beats per-frame streams, temporal deltas, and per-frame
filtering, all of which break cross-frame byte matches (measured, not assumed).

Because animation depends on long-distance matching (enabled at level ≥ 10),
`ca` defaults to level 12 rather than the still-image default of 1.

Round-trips are pixel-exact: every displayed frame decodes bit-for-bit, and the
regenerated APNG is accepted by third-party tools (verified with apngdis 2.9).
On the sample elephant animation (34 frames), `.apxl` is ~57% of the source
APNG at `-l 22`. The container is defined in [SPEC.md](SPEC.md) §10.

## Comparison with other lossless formats

PXL/APXL measured against PNG, GIF, APNG, JPEG XL, WebP, AVIF, and QOI, all in
their **lossless** mode. Generated by [`bench/bench.sh`](bench/bench.sh); see
[`bench/README.md`](bench/README.md) for methodology and how to reproduce
these numbers on your own machine.

<!-- BENCH:BEGIN -->
Measured on 2 CPU cores / 15Gi RAM, 5 runs per cell (median wall time).
Source files: [`tests/data/RGB_24bits_palette_color_test_chart.png`](tests/data/RGB_24bits_palette_color_test_chart.png)
(258×200 RGB) and [`tests/data/Animated_PNG_example_bouncing_beach_ball.apng`](tests/data/Animated_PNG_example_bouncing_beach_ball.apng)
(100×100, 20 frames). Reproduce with `bench/bench.sh`.

**Still image** (baseline: PNG)

| Format | Mode | Encode (median, ms) | Decode (median, ms) | Size (bytes) | % of baseline |
|---|---|---:|---:|---:|---:|
| PNG | lossless (native) | - | - | 30597 | 100.0% |
| PXL | lossless (native) | **16** | **16** | 5994 | 19.6% |
| GIF | palette (256 colors) | 391 | 116 | 15283 | 49.9% |
| JXL | lossless, effort 7/10 (-d 0) | 240 | 57 | 5963 | 19.5% |
| WebP | lossless (-lossless 1) | 285 | 191 | **3064** | 10.0% |
| AVIF | lossless, speed 6/10 (-l) | 160 | 21 | 12332 | 40.3% |
| QOI | lossless (native) | 21 | 37 | 71808 | 234.7% |

**Animation** (baseline: APNG)

| Format | Mode | Encode (median, ms) | Decode (median, ms) | Size (bytes) | % of baseline |
|---|---|---:|---:|---:|---:|
| APNG | lossless (native) | - | - | 61968 | 100.0% |
| APXL | lossless (native) | **31** | **36** | **47209** | 76.2% |
| GIF | palette (256 colors) | 3759 | 98 | 37713 | 60.9% |
| WebP | lossless (-lossless 1) | 467 | 77 | 53666 | 86.6% |
| AVIF | lossless, speed 6/10 (-l) | 603 | 89 | 73331 | 118.3% |
| JXL | lossless, effort 7/10 (-d 0) | 713 | 140 | 52509 | 84.7% |

Bold marks the smallest value in each numeric column (GIF excluded, see
below). Mode notes each tool's effort/speed setting where it has one — all
runs use tool defaults, not the slowest/smallest setting each encoder is
capable of, so this is not an exhaustive size-vs-speed sweep.

GIF is limited to a 256-color indexed palette, so its "lossless" encode is
only lossless relative to the quantized palette, not to the original
true-color pixels — its numbers are not directly comparable to the other
formats in these tables, and it is excluded from the bold "winner" markers
above for the same reason.

The decode column above times `pxltool d`, which re-encodes a PNG on the way
out, so it does not say much about the decoders themselves. Decoding compressed
bytes straight to raw pixels over the 24 Kodak photographs, median of 15 reps
per file (`bench/rawdec`, see [docs/BENCHMARKS.md](docs/BENCHMARKS.md)):

| Decoder | Total ms | Total bytes | % of PNG |
|---|---:|---:|---:|
| QOI | 124.4 | 16501520 | 103.5% |
| PXL | 163.7 | **13633519** | **85.5%** |
| libpng | 347.5 | 15941880 | 100% |

PXL decodes ~2.1x faster than libpng and its files are 14.5% smaller. **QOI is
faster than PXL**, by roughly 30%, on all 24 of 24 files — but QOI barely
compresses at all, and its files come out slightly larger than the source PNGs.
So PXL sits between the two: near QOI on speed, ahead of both on size.

PXL/APXL decode slower than they encode here, which looks backwards for a
zstd-based format (zstd itself decodes several times faster than it
compresses). The gap is not in the codec: `pxl_decode`/`apxl_decode` alone
run in well under a millisecond on these inputs. `pxltool d`/`da`, what this
benchmark actually times, also re-encodes the result as a PNG/APNG on the way
out, and libpng's zlib deflate on write costs roughly 10x what its inflate on
read costs — that PNG-write cost, not decompression, is what dominates the
decode column for PXL and APXL.
<!-- BENCH:END -->

### Across a whole corpus

The tables above are two files. These are every file in both committed
corpora, which is the number to judge the format by:

<!-- CORPUS:BEGIN -->
Corpus-wide totals over the 24 [Kodak](tests/data/Kodak-Lossless-True-Color-Image-Suite)
true-color photographs and the valid files of the
[official PNG test suite](tests/data/The-official-test-suite-for-PNG)
(deliberately-corrupt `x*.png` excluded), 186 files in total, on 2 CPU cores.
Every row sums only the files that format encoded successfully, and compares
against the source PNGs of that same subset. Reproduce with `bench/corpus.sh`.

| Format | Mode | Files | Total bytes | % of PNG | Encode (ms/file) |
|---|---|---:|---:|---:|---:|
| PXL | lossless, level 12 | 186 | 13712646 | 88.4% | 140.3 |
| JXL | lossless, effort 7/10 | 183 | 10339194 | 66.7% | 526.7 |
| WebP | lossless (-lossless 1) | 186 | 11385268 | 73.4% | 313.6 |
| AVIF | lossless, speed 6/10 (-l) | 186 | 13861287 | 89.4% | 309.4 |
| PNG (oxipng -o max) | lossless recompress | 186 | 14716975 | 94.9% | 1041.2 |

Encode time is total wall time divided by file count, so it includes process
startup per file — these are whole-corpus throughput figures, not the
carefully-median-ed per-call timings of the single-image tables above.
<!-- CORPUS:END -->

## Progressive (top-to-bottom) decoding

For web use, a `.pxl` can be painted while it downloads, the way a
non-interlaced PNG is. This needs no format change: rows decode independently
under the *delta* filter and depend only on the row above under *adaptive*, so
both stream. Only *BCIF* breaks it, because the color-plane split means no row
is complete until the last plane byte arrives.

Encode with `PXL_ENCODE_PROGRESSIVE` (`pxltool c … -p`) to exclude BCIF, then
push bytes into the streaming decoder in any chunk size:

```c
pxl_stream* s = pxl_stream_new(on_row, ctx);   /* on_row is called per row */
while (recv(&chunk, &n))
    if (pxl_stream_feed(s, chunk, n) < 0) { /* malformed */ }
uint32_t rows_ready;
const pxl_image* img = pxl_stream_image(s, &rows_ready);
int complete = pxl_stream_finish(s);
pxl_stream_free(s);
```

Rows become available a zstd block at a time (≤128 KiB of decompressed data), so
on a 2732×1536 photo they track download progress almost linearly — ~5% of the
bytes yields ~95 of 1536 rows. Any `.pxl` can be fed to the streaming decoder;
BCIF files simply deliver all their rows at `pxl_stream_finish`.

The tradeoff is size: on photographic content BCIF usually wins, so `-p` costs a
few percent (measured ~8.8% on the sample wallpaper). On graphics and gradients,
where adaptive already wins, `-p` costs nothing.

Since `libpxlcore` is PNG-free (zstd only), this decoder is what a future WASM
build will expose to the browser.

## Library API

```c
#include <pxl.h>

pxl_image  pxl_load_png(const char* path);
pxl_buffer pxl_encode(const pxl_image* img, int zstd_level);
pxl_buffer pxl_encode_ex(const pxl_image* img, int level, unsigned flags);
pxl_image  pxl_decode(pxl_buffer file);
int        pxl_save_png(const char* path, const pxl_image* img);
void       pxl_free(pxl_buffer* buf);
void       pxl_image_free(pxl_image* img);   /* frees pixels + metadata */

/* streaming decode -- see "Progressive decoding" above */
pxl_stream*      pxl_stream_new(pxl_row_cb cb, void* user);
int              pxl_stream_feed(pxl_stream* s, const void* data, size_t len);
const pxl_image* pxl_stream_image(const pxl_stream* s, uint32_t* rows_ready);
int              pxl_stream_finish(pxl_stream* s);
void             pxl_stream_free(pxl_stream* s);
```

Encoding is always lossless at every bit depth. 16-bit samples are kept in
PNG-native big-endian order, so 16-bit PNGs round-trip bit-for-bit. Both color
filters (delta, BCIF, adaptive) are tried at encode time and the smallest
result is kept.

## Automatic upstream tracking

Three workflows keep the project honest without anyone watching upstream by hand.

**Dependencies** — `rebuild.yml` runs daily, comparing the latest stable releases
of [pnggroup/libpng](https://github.com/pnggroup/libpng) and
[facebook/zstd](https://github.com/facebook/zstd) against the versions pinned in
`VERSIONS.json`. When either is newer it advances the `vendor/libpng`/`vendor/zstd`
git submodules to the new tag, rebuilds, runs the tests, and commits the bump
(`chore: bump libpng x / zstd y`) — or opens an issue if the build fails. A zstd
upgrade changes the compressed bytes, which is why the committed reference
files are verified by decoding rather than by comparing bytes.

**FFmpeg** — `ffmpeg-patch-check.yml` runs weekly, and on any change under
`ffmpeg/`. The registration patch edits ten files that upstream churns
constantly, so it rots on its own: nothing here changes, yet one day it stops
applying. The job clones FFmpeg master, applies the module, configures with
`--enable-libpxl`, builds, and round-trips both a still image and the 20-frame
APNG bit-exact through the resulting binary. On failure it opens one issue and
comments on it thereafter, rather than filing a fresh one every week. The format
itself is unaffected by a break here — nothing in the PXL build depends on
`ffmpeg/`.

**Our own code** — `ci.yml` covers every push: both dependency configurations,
the install and an out-of-tree consumer linking `libpxlcore` with zstd alone, the
pkg-config metadata, and the sanitized decoder soak.

## FFmpeg module

[`ffmpeg/`](ffmpeg/README.md) holds a module that teaches FFmpeg both containers:
decoders and encoders for `.pxl` and `.apxl`, an `apxl` demuxer/muxer, and `.pxl`
in the `image2` sequence handling. It is a wrapper around `libpxlcore` enabled
with `--enable-libpxl`, because FFmpeg carries no zstd of its own — a native
codec would mean vendoring a decompressor into libavcodec.

```sh
./ffmpeg/apply.sh /path/to/FFmpeg
PKG_CONFIG_PATH=$PWD/_inst/lib/pkgconfig ./configure --enable-libpxl
```

Verified against FFmpeg 8.0.git: all eight pixel formats round-trip bit-exact,
a 34-frame APNG survives `APNG → .apxl → APNG` with identical pixels and
identical per-frame timing, and files written by FFmpeg are readable by
`pxltool` (and vice versa).

It lives here rather than in its own repository because it is not an
independent project: every one of its four files is a thin translation layer
over this library's ABI, and a format change would have to land in both places
at once. Nothing in our build references `ffmpeg/` — no CMake target, no test —
so it is a directory of patch material, not a dependency. If it ever gets
accepted upstream, FFmpeg's tree becomes the home and this directory goes away.

## KDE / Qt plugins

[`kde/`](kde/README.md) holds two read-only plugins for a KDE desktop:
`kimg_pxl`, a `QImageIOPlugin` that lets Gwenview and any other Qt application
open `.pxl`/`.apxl` (with animation playback), and `pxlthumbnail`, a standalone
`KIO::ThumbnailCreator` for Dolphin previews. The second is not redundant:
`kio-extras`' image thumbnailer never asks `QImageReader` what it can decode at
runtime, so it ignores newly installed image plugins.

```sh
PKG_CONFIG_PATH="$PWD/build" cmake -B kde/build -S kde && cmake --build kde/build -j
sudo cmake --install kde/build && sudo update-mime-database /usr/share/mime
```

They decode only — encoding stays `pxltool`'s job — and depend on `libpxlcore`
alone, not on libpng. Like `ffmpeg/`, this directory is thin ABI glue that has
to track the format, so it lives here but is wired into neither the root
`CMakeLists.txt` nor CI.

## Specification

The byte format is defined formally in [SPEC.md](SPEC.md) — enough to write an
independent encoder/decoder (e.g. an ffmpeg codec) without reading this source.

## License

PXL adds no terms of its own. It is distributed under the combination of the
libpng license, the zstd BSD license, and the Zpng BSD-3 license (for the pixel
filters). See [LICENSE](LICENSE).

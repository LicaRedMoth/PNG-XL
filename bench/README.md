# Format comparison benchmark

`bench.sh` measures PXL/APXL against PNG, GIF, APNG, JPEG XL, WebP, AVIF, and
QOI, all used in their lossless mode, and writes two markdown tables (still
image, animation) with median encode/decode time and output size. The
smallest value in each numeric column is bolded (GIF excluded — see the
caveat below).

```sh
bench/bench.sh                 # print the tables
bench/bench.sh --update-readme # also splice them into README.md between
                                # the <!-- BENCH:BEGIN --> / BENCH:END markers
```

## Inputs

Only files already committed under [`tests/data/`](../tests/data/README.md)
are used, so anyone can reproduce the numbers without downloading anything
extra:

- `RGB_24bits_palette_color_test_chart.png` (258×200 RGB) for the still-image
  table, baselined against plain PNG.
- `Animated_PNG_example_bouncing_beach_ball.apng` (100×100, 20 frames) for the
  animation table, baselined against APNG.

## Per-format commands

| Format | Encode | Decode |
|---|---|---|
| PXL / APXL | `pxltool c` / `pxltool ca` | `pxltool d` / `pxltool da` |
| GIF | `magick ... gif:out.gif` (256-color palette) | `magick`/`ffmpeg` |
| JPEG XL | `cjxl -d 0` (mathematically lossless, effort 7/10 default) | `djxl` |
| WebP | `ffmpeg -c:v libwebp[_anim] -lossless 1` | `magick` (ffmpeg's own webp decoder cannot read the animated files it produces) |
| AVIF | `avifenc -l` (speed 6/10 default) | `avifdec` (`--index all` for animation) |
| QOI | `magick out.qoi` | `magick` (still image only — QOI has no animation extension) |

For animated AVIF, `avifenc` is given the individual source frames rather
than the APNG container, and `--timescale` is set to the frame count so the
sequence timescale matches — the encoder does not read APNG directly.

JPEG XL and AVIF are run at their tool's default effort/speed, not their
slowest/smallest setting — the tables note this in the Mode column so the
numbers aren't mistaken for each format's best-case size or worst-case speed.

Each encode/decode is timed `$BENCH_N` times (default 5) with wall-clock
`date +%s%N`, and the table reports the median. There is no dependency on
`hyperfine`; a plain bash loop keeps the script runnable on CI images and any
contributor's machine without installing anything beyond the codecs
themselves.

## What the script does NOT do

- It does not verify perceptual quality — only pixel-exact equality (via
  `magick compare -metric AE`) for round-trips where that is meaningful, and
  frame-count checks for animations. Any mismatch is printed as a warning on
  stderr rather than silently accepted.
- It does not benchmark AVIF/WebP/JXL encoder speed settings beyond their
  tool defaults (`-s`, `--effort`, etc.) — the comparison is about the
  lossless mode of each format, not an exhaustive speed/size sweep.
- Missing tools (e.g. no `avifenc` installed) are skipped with a warning,
  not treated as a hard failure — the script still produces tables for
  whatever is available.

## Why PXL/APXL decode slower than they encode here

This looks backwards for a zstd-based format — zstd itself decompresses
several times faster than it compresses. The asymmetry is not in the codec:
timed in isolation, `pxl_decode`/`apxl_decode` run in well under a
millisecond on these inputs, faster than `pxl_encode`/`apxl_encode` as
expected. What the benchmark actually times is `pxltool d`/`da`, which also
re-encodes the result as a PNG/APNG on the way out — and libpng's zlib
deflate on write costs roughly 10x what zlib inflate on read costs. That
PNG-write cost, not zstd decompression, is what dominates the decode column
for PXL and APXL in these tables.

## Hardware disclaimer

Timings are only meaningful together with the hardware they were measured
on. The script prints CPU core count and RAM at the top of its output for
that reason; treat encode/decode milliseconds as relative, not absolute.

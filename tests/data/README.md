# Test assets

Files here are committed to the repository so CI exercises the codec on real
images, not only on synthetic buffers. Keep this directory small and free of
third-party rights.

## Sources

### RGB_24bits_palette_color_test_chart.png

- 258×200, 8-bit RGB, non-interlaced, carries a `gAMA` chunk.
- Source: [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:RGB_24bits_palette_color_test_chart.png),
  public domain.

Used by the `real_png` case in `tests/roundtrip.c`: load via libpng, encode
progressively, decode both one-shot and through the streaming decoder, then
re-save to PNG and reload. Its flat color patches also make it a case where the
adaptive filter has to compete with BCIF.

### Animated_PNG_example_bouncing_beach_ball.apng

- 100×100, 20 frames, RGBA8, per-frame delays.
- Source: [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Animated_PNG_example_bouncing_beach_ball.png),
  public domain.

Used by the `real_apng` case. This is the only test input that exercises the
APNG chunk parser for real: `apng_load` has to honor dispose/blend ops,
per-frame offsets and sub-rectangles, none of which the synthetic full-canvas
frames in `check_anim` ever produce.

### LittleRunner.gif

- 32×32, 6 frames, global colour table (4 entries), `NETSCAPE2.0` infinite-loop
  extension.
- Source: [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:LittleRunner.gif),
  by Sudden, [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/) —
  attribution required if this file is ever redistributed rather than only
  used to run the test suite.

Used by the `gif_real` case: `gif_load` (native GIF decode, `src/gif.c`) on a
real downloaded file, then the full `apxl_anim_try_index` -> `apxl_encode` ->
`apxl_decode` -> `apng_save` -> `apng_load` round trip, checked lossless.
`check_gif` in the same file covers what this one small real GIF isn't
guaranteed to exercise — BACKGROUND/PREVIOUS disposal and transparency — on a
hand-built fixture instead, the same relationship `check_anim`/`real_apng`
already have on the APNG side.

## Encoded references

`RGB_24bits_palette_color_test_chart.pxl` and
`Animated_PNG_example_bouncing_beach_ball.apxl` were produced from the two
sources above with `pxltool` and are committed deliberately.

`ref_pxl` / `ref_apxl` decode them and compare against the pixels of the source
PNG/APNG — **not** against the reference bytes. The distinction matters: a `.pxl`
embeds a zstd frame, and a zstd upgrade (which `rebuild.yml` performs
automatically) legitimately changes those bytes while the format stays
identical. Comparing bytes would turn every compressor bump into a false
failure.

What these two files buy that the round-trip cases cannot is direction. A
round-trip only proves the encoder agrees with itself, so a change that broke
both halves symmetrically would still pass. These prove today's decoder agrees
with an encoder from the past, which is the property that matters for files
already written to disk.

Regenerate only when the format version changes on purpose:

```sh
./build/pxltool c  tests/data/RGB_24bits_palette_color_test_chart.png \
                   tests/data/RGB_24bits_palette_color_test_chart.pxl
./build/pxltool ca tests/data/Animated_PNG_example_bouncing_beach_ball.apng \
                   tests/data/Animated_PNG_example_bouncing_beach_ball.apxl
```

## Adding assets

Only add files that are public domain or explicitly licensed for redistribution,
and note the source here. Local scratch images used during development live
outside the repo (see `.gitignore`) precisely because their rights are unclear.

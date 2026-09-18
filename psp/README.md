# PSP decode benchmark

A cross-compiled MIPS build of the decoder for the actual target hardware
this project is aimed at: a correctness check across three sizes, PXL's four
filters and libpng, then a decode-throughput sweep at both 222 and 333 MHz —
the measurement [`ROADMAP.md`](../docs/ROADMAP.md)'s "measure decode on the
target hardware" entry asks for, with something to compare PXL's numbers
against rather than reporting them next to nothing.

**Verified on real hardware 2026-09-17, PSP-3008**, both clocks, PXL and
libpng both: all correctness cases came back bit-exact, and every number
(both formats, both sizes, both clocks) is in `BENCHMARKS.md` — see its
"decode measurement on the target hardware" entries, most recently "the
comparison the numbers above had nothing against: libpng". PXL beats libpng
2.8-3.5x on three of its four filters; the fourth, BCIF, does too at screen
size but **loses to libpng at texture size** — see the "Removing BCIF" entry
in `ROADMAP.md`'s Held section for what that adds up to.

**Added 2026-09-18, verified on real hardware the same day**:
`pxl_stream_new_ex`'s row-conversion to PSP-native RGB565/RGBA5551
(README's "Decoding straight into a GPU's native texture format") and
`pxl_convert_palette`'s palette conversion both matched on the PSP-3008, not
just under `PPSSPPHeadless`. `strm565`'s real throughput is in
`BENCHMARKS.md`'s "GPU-texture streaming and indexed-palette conversion,
real hardware" entry: decoding straight to RGB565 costs 37.7-43.5% more
decode time than a plain `pxl_decode()`, but still reconstructs pixels
roughly 2x faster than libpng's plain RGBA8888 decode on the same hardware.

**Added 2026-09-18, headless-verified, real-hardware confirmation still
needed**: an embedded 8-frame indexed `.apxl` animation, decoded via
`apxl_decode` (bit-exact under `PPSSPPHeadless`) and pushed through the
first `sceGu*` code in this project — `GU_PSM_T8` texture + one CLUT per
stream, per `ROADMAP.md`'s "Animated indexed `.apxl` texture playback on
PSP". The GE call sequence itself runs cleanly (no hang, no error) at both
clocks, but `PPSSPPHeadless`'s off-screen pixel read-back does not confirm
bit-exact output for a reason investigated but not identified — see
`RESEARCH.md`'s entry with the same name for the full investigation. The
program reports the real per-frame pixel-match percentage rather than
hiding the gap; treat it as unconfirmed until run on real hardware, the same
as every throughput number below.

## One-time setup

Needs the [pspdev](https://github.com/pspdev/pspdev) toolchain — not packaged
for most distros, so download a release directly:

```sh
curl -L -o pspdev.tar.gz \
  https://github.com/pspdev/pspdev/releases/latest/download/pspdev-ubuntu-latest-x86_64.tar.gz
tar xzf pspdev.tar.gz            # extracts to ./pspdev/, ~450 MB
export PSPDEV="$PWD/pspdev"
```

`PSPDEV` needs to be set for every build below; put it in your shell profile
if this becomes a regular thing.

## Building

```sh
cmake --build build --target pxl_bench_mkpsptest   # host tool, builds once
build/pxl_bench_mkpsptest > psp/testdata.h          # regenerate only if a
                                                     # size or filter changes
psp/build.sh
```

`psp/testdata.h` is generated, not hand-written — see
[`bench/mkpsptest.c`](../bench/mkpsptest.c) for what it embeds and why: one
valid `.pxl` file per (size, filter) pair (forced through that filter rather
than encoder-chosen, so all four get exercised at every size) plus one PNG
per size, same pixels, libpng's own default write settings — a baseline, not
a best case for either side. It's committed so `psp/build.sh` alone is enough
day to day. It does *not* embed the reference images to check decodes
against — [`pattern.h`](pattern.h) holds the one formula that generates them,
shared between the host generator and `main.c`'s correctness check, which is
what keeps the generated header under 2 MB rather than the several megabytes
a screen- and texture-sized raw reference image would cost as hex text.

`psp/build.sh` cross-compiles [`main.c`](main.c) together with the same
decode-only source set [`wasm/build.sh`](../wasm/build.sh) uses (no
`pxl_codec_encode.c`, since this only ever decodes), against PSPSDK's newlib
instead of the wasm build's freestanding shim — the PSP has a real libc, so
`pxl_decode()` is called directly with no wrapper. libpng is a pspdev portlib
(`png16`/`z`, already in the toolchain, nothing this project vendors or
builds), decoded through the identical `png_set_expand`/`strip_16`/
`gray_to_rgb`/`add_alpha` normalisation to RGBA8888 that
[`bench/formatdec.c`](../bench/formatdec.c) uses on x86, so the two MB/s
numbers mean the same thing. Output is a plain ELF (`psp/build/pxl_psp_bench`,
no extension — the toolchain doesn't add one) and a ready `EBOOT.PBP`.

## Running

No PSP needed for a correctness check (though not for a real timing — see
below) — [PPSSPP](https://www.ppsspp.org/) ships a headless mode built for
exactly this (its own automated test suite uses it):

```sh
PPSSPPHeadless -l psp/build/pxl_psp_bench | grep '^I stdout:'
```

`-l` is required: without it, headless mode prints nothing at all, matching
output or not, so a silently-failing run looks identical to a passing one.
Expected output (the throughput figures here are the *emulator's* speed, not
Allegrex — see below):

```
PXL PSP decode benchmark, libpxl 1.5.0

-- correctness --
[smoke    none    ] 64x64 4ch 16384 bytes -- MATCH
[smoke    delta   ] 64x64 4ch 16384 bytes -- MATCH
[smoke    adaptive] 64x64 4ch 16384 bytes -- MATCH
[smoke    bcif    ] 64x64 4ch 16384 bytes -- MATCH
[smoke    png     ] 64x64 4ch 16384 bytes -- MATCH
[screen   none    ] 480x272 4ch 522240 bytes -- MATCH
[screen   delta   ] 480x272 4ch 522240 bytes -- MATCH
[screen   adaptive] 480x272 4ch 522240 bytes -- MATCH
[screen   bcif    ] 480x272 4ch 522240 bytes -- MATCH
[screen   png     ] 480x272 4ch 522240 bytes -- MATCH
[texture  none    ] 512x512 4ch 1048576 bytes -- MATCH
[texture  delta   ] 512x512 4ch 1048576 bytes -- MATCH
[texture  adaptive] 512x512 4ch 1048576 bytes -- MATCH
[texture  bcif    ] 512x512 4ch 1048576 bytes -- MATCH
[texture  png     ] 512x512 4ch 1048576 bytes -- MATCH
correctness: ALL OK
-- streaming output-format conversion --
[smoke    RGB565  ] 64 rows -- MATCH
[smoke    RGBA5551] 64 rows -- MATCH
[screen   RGB565  ] 272 rows -- MATCH
[screen   RGBA5551] 272 rows -- MATCH
[texture  RGB565  ] 512 rows -- MATCH
[texture  RGBA5551] 512 rows -- MATCH
streaming output-format: ALL OK
[palette  RGBA5551] 8 bytes -- MATCH
convert_palette: ALL OK
-- throughput at 222 MHz requested, 222 MHz actual, 786432 bytes free --
  screen   none     median  16814 us over 15 reps, 522240 bytes ->   29.621 MB/s
  ...
  screen   png      median  82197 us over 15 reps, 522240 bytes ->    6.059 MB/s
  screen   strm565  median  26463 us over 15 reps, 261120 bytes ->    9.410 MB/s
  ...
-- throughput at 333 MHz requested, 333 MHz actual, 786432 bytes free --
  screen   none     median  11388 us over 15 reps, 522240 bytes ->   43.734 MB/s
  ...
  screen   png      median  54757 us over 15 reps, 522240 bytes ->    9.096 MB/s
  screen   strm565  median  17669 us over 15 reps, 261120 bytes ->   14.094 MB/s
  ...
Saved to results.txt next to this EBOOT. Press X to exit
```

`streaming output-format conversion` checks `pxl_stream_new_ex`'s `PXL_OUTPUT_RGB565`/`RGBA5551` conversion row-by-row against a MIPS-side reference computed independently of the decoder's own `convert_row` (same cross-check `tests/roundtrip.c` does on the host) -- this is [ROADMAP.md](../docs/ROADMAP.md)'s "decode straight into a GPU texture" item, verified for correctness here; `strm565` in the throughput sweep is its speed, still emulator-untrustworthy like every other timing on this page, but the number to read once real hardware is available: does converting every row as it streams cost anything over a plain `pxl_decode()`, or does it come for free inside the existing per-row write.

`convert_palette` is the same cross-check for `pxl_convert_palette` -- the piece that closes indexed mode onto the hardware entirely: an indexed `.pxl`'s index bytes already need no conversion (`GU_PSM_T4`/`T8` read them as-is), so once the palette is packed too there is nothing left to convert by hand between an indexed file and a texture the GE samples directly.

`MATCH` means the MIPS-compiled decoder (PXL's four filters, or libpng for the
`png` row) reconstructed the exact source pixels for that size — a `memcmp`
against pixels re-derived from `pattern.h`, not a checksum, so there is no
hash collision to worry about. The throughput sweep only runs if every
correctness case passed; a fast wrong answer is not a result.

### Where the output actually goes

Real hardware has no console, so `sceIoWrite(1, ...)` alone -- the primitive
PPSSPP's own test suite captures headless with no setup -- would print into a
void there: fd 1 is not connected to anything without a debug cable. Every
result is written to three places at once for that reason, each covering
where the other two fall short:

- **The PSP's own screen**, via `pspDebugScreenPrintf`. This is what a human
  looking at real hardware sees. Not touched under headless, which has no
  display.
- **`sceIoWrite(1, ...)`.** Captured by `PPSSPPHeadless -l`, ignored by real
  hardware.
- **`results.txt`, written next to the running EBOOT.** The only copy that
  outlives the process. On real hardware: turn on USB Connect from the PSP's
  Settings menu (or pull the memory stick), and `results.txt` sits in
  `PSP/GAME/PXLBENCH/` on a computer, plain text, ready to paste into
  `BENCHMARKS.md`.

The program waits for the X button before returning to the XMB (15 real
seconds on hardware, since `sceRtcGetCurrentTick` tracks real time there --
but the same 15 seconds of *emulated* ticks elapse in a fraction of a real
second under headless mode with no display to pace against, so the automated
check above still exits on its own in a couple of seconds). Plain libc
`stdout` was used for none of this because whether it is buffered, or wired to
fd 1 at all, depends on newlib's startup code in a way this project has not
verified -- `sceIoWrite` is one layer lower and leaves nothing to that
assumption.

On real hardware: copy `psp/build/EBOOT.PBP` to
`ms0:/PSP/GAME/PXLBENCH/EBOOT.PBP` and run it from the PSP's game menu.

## Why the headless numbers are not the answer

`PPSSPPHeadless`'s throughput figures time **the emulator's own model of
Allegrex timing, computed on an x86 box**, not real Allegrex silicon --
notice they scale by almost exactly 1.5x between the "222 MHz" and "333 MHz"
sections above (screen/none: 16814 vs 11388 µs, ratio 1.48; the other rows
land between 1.48 and 1.56), which is `scePowerSetClockFrequency`'s own
333/222 ratio. That is the emulator's internal cycle-accounting responding to
the clock it was told to pretend to run at -- the underlying x86 host obviously
does not get physically faster when a PSP program asks for a higher clock.
It looking exactly like a hardware clock-scaling result is what makes it
dangerous, not reassuring: this project already published two numbers that
were plausible and wrong (see [`RESEARCH.md`](../docs/RESEARCH.md)) from
measuring an adjacent thing and trusting it by mistake, and a synthetic
number that scales the way a real one would is a more convincing version of
that same error, not a less convincing one. What the headless run does
establish is everything *except* the number itself: the codec cross-compiles
for MIPS, links against PSPSDK's newlib, and decodes every filter and libpng
bit-exact at every size, on the real target's instruction set.

## Getting the numbers into BENCHMARKS.md

Copy `psp/build/EBOOT.PBP` to `ms0:/PSP/GAME/PXLBENCH/EBOOT.PBP`, run it, wait
for (or skip past, with X) the 15-second pause, then pull `results.txt` from
the same folder over USB Connect or a memory-stick read. That file has the
real Allegrex numbers at both clocks, split by size and filter (PXL's four
and libpng), with the actual clock and free memory it ran under printed
alongside -- state those next to the numbers in `BENCHMARKS.md`, the same way
`bench/bench.sh` states the load average it ran under. A measurement that
does not state its conditions is a failure mode this project has already
been burned by twice.

Every number this program prints is already in `BENCHMARKS.md` as of
2026-09-18 (the decode/libpng sweep as of 2026-09-17, `strm565` and
`convert_palette` added 2026-09-18). A fresh run now is a reproducibility
check, not a gap-filler -- and the last one landed within 0.2% of the
recorded numbers despite the binary growing from 528 KB to 919 KB in
between, which is itself worth knowing before trusting any of this further.

# PSP decode benchmark

A cross-compiled MIPS build of the decoder for the actual target hardware
this project is aimed at: a correctness check across three sizes and four
filters, then a decode-throughput sweep at both 222 and 333 MHz — the
measurement [`ROADMAP.md`](../docs/ROADMAP.md)'s "measure decode on the
target hardware" entry asks for.

**Verified on real hardware 2026-09-17, PSP-3008.** All twelve correctness
cases (three sizes × four filters) came back bit-exact. The throughput
numbers from that run are not yet in `BENCHMARKS.md` — see
[Getting the numbers into BENCHMARKS.md](#getting-the-numbers-into-benchmarksmd).

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
valid `.pxl` file per (size, filter) pair, forced through that filter rather
than encoder-chosen, so all four get exercised at every size. It's committed
so `psp/build.sh` alone is enough day to day. It does *not* embed the
reference images to check decodes against — [`pattern.h`](pattern.h) holds
the one formula that generates them, shared between the host generator and
`main.c`'s correctness check, which is what keeps the generated header under
1.5 MB rather than the several megabytes a screen- and texture-sized raw
reference image would cost as hex text.

`psp/build.sh` cross-compiles [`main.c`](main.c) together with the same
decode-only source set [`wasm/build.sh`](../wasm/build.sh) uses (no
`pxl_codec_encode.c`, since this only ever decodes), against PSPSDK's newlib
instead of the wasm build's freestanding shim — the PSP has a real libc, so
`pxl_decode()` is called directly with no wrapper. Output is a plain ELF
(`psp/build/pxl_psp_bench`, no extension — the toolchain doesn't add one) and
a ready `EBOOT.PBP`.

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
[screen   none    ] 480x272 4ch 522240 bytes -- MATCH
[screen   delta   ] 480x272 4ch 522240 bytes -- MATCH
[screen   adaptive] 480x272 4ch 522240 bytes -- MATCH
[screen   bcif    ] 480x272 4ch 522240 bytes -- MATCH
[texture  none    ] 512x512 4ch 1048576 bytes -- MATCH
[texture  delta   ] 512x512 4ch 1048576 bytes -- MATCH
[texture  adaptive] 512x512 4ch 1048576 bytes -- MATCH
[texture  bcif    ] 512x512 4ch 1048576 bytes -- MATCH
correctness: ALL OK
-- throughput at 222 MHz requested, 222 MHz actual, 786432 bytes free --
  screen   none     median  16814 us over 15 reps, 522240 bytes ->   29.621 MB/s
  ...
-- throughput at 333 MHz requested, 333 MHz actual, 786432 bytes free --
  screen   none     median  11433 us over 15 reps, 522240 bytes ->   43.562 MB/s
  ...
Saved to results.txt next to this EBOOT. Press X to exit
```

`MATCH` means the MIPS-compiled decoder reconstructed the exact source pixels
for that (size, filter) pair — a `memcmp` against pixels re-derived from
`pattern.h`, not a checksum, so there is no hash collision to worry about.
The throughput sweep only runs if every correctness case passed; a fast wrong
answer is not a result.

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

`PPSSPPHeadless`'s throughput figures time **the emulator's own JIT on an x86
box**, not the 222/333 MHz Allegrex core -- notice they don't even change
between the "222 MHz" and "333 MHz" sections above, because
`scePowerSetClockFrequency` there only changes what
`scePowerGetCpuClockFrequencyInt()` reports back, not anything the host CPU
actually runs at. This project already published two numbers that were
plausible and wrong (see [`RESEARCH.md`](../docs/RESEARCH.md)) from measuring
an adjacent thing and trusting it by mistake; an emulator timing presented as
a hardware figure would be the same error a third time. What the headless run
does establish is everything *except* the number itself: the codec
cross-compiles for MIPS, links against PSPSDK's newlib, and decodes every
filter bit-exact at every size, on the real target's instruction set.

## Getting the numbers into BENCHMARKS.md

Copy `psp/build/EBOOT.PBP` to `ms0:/PSP/GAME/PXLBENCH/EBOOT.PBP`, run it, wait
for (or skip past, with X) the 15-second pause, then pull `results.txt` from
the same folder over USB Connect or a memory-stick read. That file has the
real Allegrex numbers at both clocks, split by size and filter, with the
actual clock and free memory it ran under printed alongside -- state those
next to the numbers in `BENCHMARKS.md`, the same way `bench/bench.sh` states
the load average it ran under. A measurement that does not state its
conditions is a failure mode this project has already been burned by twice.

# PSP decode smoke test

A cross-compiled MIPS build of the decoder for the actual target hardware this
project is aimed at, plus a program that runs it and checks the result. It is
not the hardware benchmark [`ROADMAP.md`](../docs/ROADMAP.md) asks for — see
[Why this isn't a benchmark yet](#why-this-isnt-a-benchmark-yet).

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
build/pxl_bench_mkpsptest > psp/testdata.h          # regenerate only if the
                                                     # test image or filters change
psp/build.sh
```

`psp/testdata.h` is generated, not hand-written — see
[`bench/mkpsptest.c`](../bench/mkpsptest.c) for what it embeds and why
(one valid `.pxl` file per color filter, forced rather than encoder-chosen, so
all four get exercised; plus the known-correct pixels to check the decode
against). It's committed so `psp/build.sh` alone is enough day to day.

`psp/build.sh` cross-compiles [`main.c`](main.c) together with the same
decode-only source set [`wasm/build.sh`](../wasm/build.sh) uses (no
`pxl_codec_encode.c`, since a decode smoke test has no use for the compressor),
against PSPSDK's newlib instead of the wasm build's freestanding shim — the
PSP has a real libc, so `pxl_decode()` is called directly with no wrapper.
Output is a plain ELF (`psp/build/pxl_psp_bench`, no extension — the toolchain
doesn't add one) and, since the ELF happens to link cleanly enough for it, a
ready `EBOOT.PBP` for real hardware.

## Running

No PSP needed for a correctness check — [PPSSPP](https://www.ppsspp.org/)
ships a headless mode built for exactly this (its own automated test suite
uses it):

```sh
PPSSPPHeadless -l psp/build/pxl_psp_bench | grep '^I stdout:'
```

`-l` is required: without it, headless mode prints nothing at all, matching
output or not, so a silently-failing run looks identical to a passing one.
Expected output:

```
PXL PSP decode smoke test, libpxl 1.5.0
[none    ] 64x64 4ch 16384 bytes in 1481 us -- MATCH
[delta   ] 64x64 4ch 16384 bytes in 2487 us -- MATCH
[adaptive] 64x64 4ch 16384 bytes in 3563 us -- MATCH
[bcif    ] 64x64 4ch 16384 bytes in 1475 us -- MATCH
ALL OK
```

`MATCH` means the MIPS-compiled decoder reconstructed the exact source pixels
for that filter — a `memcmp` against pixels computed once on the host, not a
checksum, so there is no hash collision to worry about.

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
  `BENCHMARKS.md` or hand back for that.

The program waits for the X button before returning to the XMB (15 real
seconds on hardware, since `sceRtcGetCurrentTick` tracks real time there --
but the same 15 seconds of *emulated* ticks elapse in a fraction of a real
second under headless mode with no display to pace against, so the automated
smoke test above still exits on its own in well under a second). Plain libc
`stdout` was used for none of this because whether it is buffered, or wired to
fd 1 at all, depends on newlib's startup code in a way this project has not
verified -- `sceIoWrite` is one layer lower and leaves nothing to that
assumption.

On real hardware: copy `psp/build/EBOOT.PBP` to
`ms0:/PSP/GAME/PXLBENCH/EBOOT.PBP` and run it from the PSP's game menu.

## Why this isn't a benchmark yet

The program reads `sceRtcGetCurrentTick()` around each decode and prints
microseconds — the same code path the real measurement will use. But under
PPSSPPHeadless that number times **the emulator's JIT on this x86 box**, not
the 222/333 MHz Allegrex core. This project already published two numbers
that were plausible and wrong (see [`RESEARCH.md`](../docs/RESEARCH.md)) from
measuring an adjacent thing and trusting it by mistake; an emulator timing
presented as a hardware figure would be the same error a third time.

What today's run does establish, and what an arithmetic estimate alone
cannot: the codec cross-compiles for MIPS with `psp-gcc`, links against
PSPSDK's newlib with no changes, and decodes every filter bit-exact on that
target. What's left is entirely a hardware-availability problem, not a
code problem — see the "Measure decode on the target hardware" entry in
[`ROADMAP.md`](../docs/ROADMAP.md) for what the real run should cover (both
CPU clocks, a screen-sized and a texture-sized image, split by filter). When
a PSP is available again, that's `psp/build.sh` once and copying
`EBOOT.PBP` over — nothing here needs to change to become the real number.

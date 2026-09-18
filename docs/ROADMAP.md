# Roadmap

What is worth doing next, and why. Ordered by confidence, not by size. Anything
here that has already been measured links back to [`RESEARCH.md`](RESEARCH.md),
which is also where rejected ideas live — check it before reopening something.

The ground rule the project has earned the hard way: no change lands on the
strength of an argument. It lands on a measurement over a corpus, and the
measurement states its sampling.

## Now

Memory and decoder size are both closed (see *Done* below). Both pre-freeze
items below are done too — what actually stands between here and freezing the
specification is the freeze decision itself (see "Then freeze").

### Done — `sBIT` is kept

Decided and shipped 2026-09-16 (`c9b36e8`). PNG's significant-bits chunk was
dropped along with `PLTE`, `tRNS`, `bKGD` and `hIST` under a rule about chunks
that depend on pixel layout — too broad, since for a 16-bit non-indexed image
canonicalization does *not* change the layout, so `sBIT` stays valid. Keeping
it is what gives 10-bit content its actual value — a reader learns the range
is 10-bit — without a packed sample format, which was measured and rejected
because it compresses 23% *worse*. Metadata only; the decoder never looks at
it. Fixing this also surfaced an older chunk-ordering bug (`gAMA`/`sBIT`/etc.
must precede `PLTE`, which the injector wasn't guaranteeing on indexed
images), fixed in the same commit. Verified bit-exact on all 162 PngSuite
files, the 49 of which carry `sBIT`. See [`RESEARCH.md`](RESEARCH.md).

### Done — `bench/encstages` is written up

Answered 2026-09-16 and rejected as a default: the cheap probe does *not* rank
the candidates the way the full level does, disagreeing on 4-13% of files, so
the byte-identical output the idea rested on is not available. The closest safe
variant buys only 1.09-1.37x. Left open as a possible `PXL_ENCODE_FAST` opt-in
(encode speed; not implemented, and not to be confused with the unrelated,
shipped `PXL_ENCODE_FAST_DECODE` below, which trades size for a decode-speed
guarantee), with the measurement recorded so it need not be repeated. See
[`RESEARCH.md`](RESEARCH.md).

### Then freeze

A security audit of the untrusted-input paths (2026-09-16) found and fixed
two 32-bit integer-overflow OOB reads and is recorded in
[`RESEARCH.md`](RESEARCH.md); the pixel-format audit is done. What remains before
freezing is a decision to freeze — the format is pre-release with no outside
users, so the spec can still change without a version bump, and that window
closes at release.

### Verify: is `.apxl`/APNG actually finished?

Raised 2026-09-18, not assumed — checked against `SPEC.md`, `RESEARCH.md` and
the test tree rather than taken on faith, because it had not been re-checked
since the container shipped. Three things are still open:

1. **The LDM-per-stream decision is not made.** See "Decide cross-frame
   long-distance matching per stream" below — 16-frame evidence from two Anita
   shots only, no corpus sweep yet, no threshold. This is the one item that
   blocks calling the *encoder's* animation behaviour settled, as opposed to
   the container format.
2. **Done — indexed animation: decided.** Was out of `.apxl` version 1 by
   ordering accident, not a considered rejection (`SPEC.md` §10.1; the v1/v2
   header froze 2026-07-25/26, two days before indexed-still support existed
   at all, `314bb18`, 2026-07-27). Measured 2026-09-18 against a purpose-built
   corpus (`bench/gif_corpus.sh`, 66 freely-licensed animated GIFs from
   Wikimedia Commons): the design precondition — one global ≤256-colour
   palette per stream — holds on 100% of the PSP-relevant content sampled
   (Animated pixel art, Throbbers) and 76% overall; where it holds, indexed
   compresses ~33% smaller than RGBA at `.apxl`'s real settings, in 49 of 50
   files. See `RESEARCH.md`'s "Indexed `.apxl`" entry for the full numbers.
   **Decision: build it.** Not yet started — see "Implement indexed `.apxl`"
   below for the actual work this opens.
3. **Anita is the only animation corpus exercised.** `Screenrecords` (H.264,
   personal, gitignored) exists locally as a second one and is not wired into
   any bench script yet — see "Corpus gaps" below.

What is solid, so this is not a rewrite: `tests/roundtrip.c` exercises
`apxl_decode`/`apng_load`/`apng_save` round-trips, and the 2026-09-16 security
audit ran 3M decode-fuzz and 700k metadata-fuzz iterations under 32-bit
ASAN/UBSAN against these same paths, catching and fixing two real bugs
(`c812767`) — correctness and hardening are covered. What is not settled is
whether the *encoder's* LDM-always-on-at-level-≥10 default is the behaviour
this project wants to freeze on (item 1), and indexed animation now has a
decision but no implementation yet (item 2, see "Implement indexed `.apxl`"
below) — freezing the container format ahead of that would freeze it without
the feature this measurement just justified.

### Implement indexed `.apxl`

Decided 2026-09-18 (see item 2 above and `RESEARCH.md`'s "Indexed `.apxl`"
entry). Container, codec, auto-palette builder, CLI, the whole APNG round
trip, and a native GIF front end are done the same day; only the PSP/GE
path is not.

**Done:**

- **Header:** `.apxl` bumped to version 2, 32 → 36 bytes
  (`src/apxl_format.h`), carrying `PaletteCount`/`PaletteAlphaCount` the way
  the still `.pxl` header already does ([[indexed-mode]] design note,
  `314bb18`). Turned out simpler than expected: `.apxl` has no per-frame
  filter stage at all (unlike stills), so there was no `pixel_bytes`/
  `filter_width` trick to reuse — indexed frames are just 1-byte-per-pixel
  index rows concatenated exactly like any other frame already is.
- **Encoder/decoder (`apxl_codec.c`):** `apxl_encode` detects an indexed
  first frame, requires every other frame's palette to be byte-identical
  (rejects the encode otherwise — a per-frame palette isn't representable
  here, and silently keeping one frame's palette would be a lossy encode
  wearing a lossless format's name), and writes the palette once. `apxl_decode`
  reads it back into one `apxl_anim`-level buffer and points every frame's
  `pxl_image.palette`/`palette_alpha` at it (non-owning, freed once by
  `apxl_free` — same sharing shape already used for pixel storage).
- **Tests:** `tests/roundtrip.c`'s `check_anim_indexed` round-trips an indexed
  animation byte-exact (pixels, palette, alpha) and confirms a mismatched
  per-frame palette is rejected. `tests/data/Animated_PNG_example_...apxl`
  regenerated for the version bump (documented, expected — `tests/data/README.md`).
  500k `pxl_fuzz_decode` iterations under ASan/UBSan clean on the new parsing
  path (`palette_count`/`palette_alpha_count` are attacker-controlled).
- **Spec:** `SPEC.md` §10 rewritten for version 2.

**Also done, same day, after the first pass above:**

- **Encoder-side auto-palette (`apxl_anim_try_index`, `src/apxl_codec.c`):**
  builds the global palette itself from an already-composited RGB/RGBA
  animation (hash-table colour dedup, two-pass: check the union fits ≤256
  before mutating anything), converts in place on success, leaves the input
  completely untouched on failure (verified: pointer identity, not just
  content, in `tests/roundtrip.c`'s `check_anim_try_index`). This is the
  "fallback is mandatory" decision from the first pass — done, not open
  anymore.
- **CLI (`pxltool`):** `ca` gained `-i` ("try indexed, fall back to RGBA
  silently"); `ainfo` now prints palette info like still `info` already did.
- **`apng_save` writes indexed output:** PLTE/tRNS at the file level (once,
  matching APNG's single-palette convention), correct chunk ordering
  (cHRM/gAMA/iCCP/sBIT/sRGB/cICP before PLTE, same list `pxl_meta.c` already
  used for stills). `encode_frame_idat` generalized to take a color type and
  pixel width instead of assuming RGBA8.
- **`apng_load` reads indexed *input* correctly:** found by testing, not
  anticipated — `apng_load`'s per-frame decoder builds a synthetic
  single-frame PNG per `fcTL`/`fdAT`, and that synthesizer never carried PLTE
  through, so any indexed source (including `apng_save`'s own new output)
  failed to load with libpng's "Missing PLTE before IDAT". Fixed by capturing
  the file's real PLTE/tRNS during the existing chunk walk and passing them
  into the synthesized single-frame PNGs. Still always expands to RGBA on the
  way out (unchanged design), it just no longer errors getting there.
- **Real corpus verification, not just synthetic:** two `bench/gif_corpus.sh`
  files (`LittleRunner.gif`, `Cloud.gif`) run through GIF → APNG (ffmpeg) →
  `pxltool ca -i` → `da` → `magick compare -metric AE` against the original
  APNG: **0 pixels differ** both times. Cloud.gif: 117 483 → 73 285 bytes
  (37.6% smaller), in line with the corpus-wide measurement.
- **Safety on the new parsing path:** 500k `pxl_fuzz_decode` iterations
  (the `.apxl` palette header fields) plus a targeted 3000-mutation check
  against `apng_load`'s new PLTE/tRNS handling (truncation, byte flips,
  length-field corruption, PLTE removal, PLTE duplication) — both clean
  under ASan/UBSan. `apng_load` itself still has no *general* fuzz harness
  (only `apxl_decode`/`pxl_meta_extract`/`inject` do, via `pxl_fuzz_decode`/
  `pxl_fuzz_meta`) — worth building one, not done here.
- **Native GIF front end (`src/gif.c`, `pxlcore`, no libpng dependency).**
  Hand-rolled GIF87a/89a decoder: variable-width LZW (3-12 bit codes, KwKwK
  case), interlace, GCE/disposal (NONE/BACKGROUND/PREVIOUS, mirrors APNG's
  dispose-then-draw model), NETSCAPE loop extension. `pxltool cg` wires it
  straight into the indexed `.apxl` pipeline (`gif_load` → `apxl_anim_try_index`
  → `apxl_encode`). Verified three ways: a hand-built synthetic 4×4/4-colour
  GIF exercising every disposal method (`check_gif`), a real 6-frame file
  end-to-end through GIF → `.apxl` → APNG (`check_gif_real`), and a 66-file
  real-corpus batch (`bench/gif_corpus.sh`) compared pixel-for-pixel against
  ffmpeg's GIF→APNG: 63/66 exact, 3 "failures" root-caused to ffmpeg being a
  non-standard outlier (see below), not a bug here. Fuzzed 26 800 mutations
  (67 real-corpus seeds × 400 mutations, 4 strategies) under ASan/UBSan: **0
  crashes**; the fuzzer's 169 `TIMEOUT` flags were re-checked and are a
  harness-calibration artefact (5s per-case limit, ASan overhead, loaded
  machine) — the same large seeds decode in under 1.4s in a release build,
  confirming linear cost, not a hang.
  - **Found and fixed a genuine GIF-semantics ambiguity along the way:**
    canvas area no frame ever covers, and area a `BACKGROUND`-disposal frame
    clears, is specified by GIF89a's advisory text as fillable with the
    Logical Screen Descriptor's background colour, and ffmpeg's decoder
    implements that literally — but real browser rendering (verified via
    Chromium `<canvas>.getImageData()` against two corpus files where this
    mattered) renders it **fully transparent (0,0,0,0)** instead, same as an
    explicitly-transparent pixel. `gif.c` follows the browser behaviour
    (transparent), not ffmpeg's; this is why 3 of the 66 corpus files "fail"
    against an ffmpeg reference despite being correct.

**In progress — PSP/GE path.** See "Animated `.apxl` texture playback on
PSP" below: built 2026-09-18, headless-verified for decode correctness and
clean GE execution, real-hardware pixel confirmation still pending.

**Not done — still open:**

- **`apng_load` fuzz coverage**, per the safety note above.

### Real charts: texture load pipeline, PXL vs PNG, init to use, plus weight

Combines two items already below ("Plot the benchmarks instead of only
tabulating them", "Measure the full load pipeline, not just decode") into one
concrete ask, specifically for textures, moved up from Next because both of
its blockers may now be gone rather than because the scope changed:

- A **real** (measured on hardware, not projected) `Memory Stick -> file ->
  decode -> usable texture -> sceGuDrawArray` timer, PXL against libpng, on
  the PSP-3008. The "Measure the full load pipeline" entry below says this
  needs "real hardware, which is not available while the console is
  elsewhere" — that was true 2026-09-17; it no longer is, since two more
  rounds of real-hardware numbers have landed since (this file's and
  `BENCHMARKS.md`'s 2026-09-18 entries). The timer itself is still unbuilt.
- A **committed, generated** SVG chart — script-produced from the same data
  the tables use, not hand-drawn, corpus and sampling stated in the figure —
  for both the load-pipeline timing and the size ("weight") comparison, per
  the requirements "Plot the benchmarks" already states below. Neither chart
  exists yet; both are currently tables only.

### Animated indexed `.apxl` texture playback on PSP — built, headless-verified, real hardware pending

Scoped to the indexed case (`GU_PSM_T8` + one CLUT per stream via
`pxl_convert_palette`), since that's what the 2026-09-18 corpus measurement
says the actual target content (UI icons, throbbers) is — see
`RESEARCH.md`'s "Animated indexed `.apxl` on the PSP GE" entry for the full
account. First `sceGu*` code in this project.

**Done:** `src/apxl_codec.c` cross-compiled for this target (decode-only,
via a link-time stub for the zstd compressor symbols `apxl_encode` alone
needs — `psp/apxl_encode_stubs.c`); a small embedded 64×64/8-frame indexed
test animation (`bench/mkpsptest.c`'s `make_anim_apxl`); `apxl_decode` on
real MIPS verified bit-exact against `psp/pattern.h`'s formula (all 8
frames, the palette); the actual `sceGuInit` → per-frame
`sceGuTexImage`/`sceGuDrawArray` → `sceGuTerm` sequence runs cleanly under
`PPSSPPHeadless` (no hang, no error) across all 8 frames at both clocks.
Found and fixed a real bug along the way: `GU_TRANSFORM_2D` still needs
`sceGuOffset`/`sceGuViewport`/`sceGuScissor` (it only skips the vertex
*transform matrix*, not the separate clip stage) — every draw rasterized to
~1 pixel without them.

**Not done / not confirmed:** bit-exact pixel readback. `PPSSPPHeadless`'s
off-screen render target reads back 0% matching `clut[index]` after the fix
above, for a reason investigated at length but not identified — ruled out:
this program's own debug-console output landing through the altered GE
state (real, fixed, but not sufficient alone), an uninitialised-VRAM
artefact, vertex data provenance, cross-frame bleed-through. Most
consistent with a `PPSSPPHeadless`-specific GE-emulation gap when no
display is ever attached (never tested by any reference sample, all of
which always show a real display buffer). `run_anim_ge_correctness`
(`psp/main.c`) reports the real match percentage as data rather than
hiding it, and gates pass/fail on clean execution instead of pixel
equality, since this investigation could not make pixel equality reliable
under headless specifically. Real hardware (always has a display attached)
is the next data point — same two-stage pattern `psp/README.md` already
uses for everything else on this target. Throughput numbers exist in the
program's output already but, like every other headless timing on this
target, are the emulator's own model, not Allegrex — real numbers still
needed.

## Next

### Plot the benchmarks instead of only tabulating them

Every number this project has is a table, and tables hide shapes. Three plots
would each have caught something we found late or by accident:

- **Size against encode time across levels 1-22.** The still-image default is
  level 1 (`PXL_LEVEL_DEFAULT`), while every published figure is level 12. On a
  25-file sample that is 74.2% of PNG against 57.2% — seventeen points between
  what the tool produces and what the README claims, invisible in any table we
  keep. A curve would have made it obvious, and would also show whether level 19
  (51.6% on the same sample) is worth its encode cost or falls off a cliff.
- **Ratio by content class**, one bar group per corpus — photographs, synthetic
  stills, grayscale, indexed, animation. The format's behaviour differs by 30
  points across these, and a single "% of PNG" number averages that away.
- **Decode speed against size**, one point per format. This is the plot that
  actually states the format's case: PXL is third on size and second on speed,
  but the combination with decoder footprint is a position nobody else holds.
  A scatter shows that; a column of percentages does not.

Requirements, learned the hard way today: generated by a committed script from
the same data the tables use (no hand-drawn numbers), output as SVG so it
survives in git and stays readable in a diff, and every plot must state its
corpus and sampling in the figure itself — an unlabelled chart is worse than no
chart, because it travels further before anyone checks it.


### Done — decode measured on the target hardware

Everything claimed about the PSP-class target used to be arithmetic, not
measurement — the decoder fits in flash (174 KB of `.text`) and the still
path decodes in roughly the size of its output, but **no number in this
project had ever been taken on the hardware it is aimed at**, and throughput
was the one figure that could not be derived from the x86 runs.

**Code-risk side closed 2026-09-16**: the decoder cross-compiles for MIPS
with `psp-gcc`/PSPSDK unchanged, and decodes every filter bit-exact under
`PPSSPPHeadless` — see [`psp/README.md`](../psp/README.md).

**Measured on real hardware 2026-09-17**, a PSP-3008: decode throughput in
MB/s for a 480x272 screen and a 512x512 texture, split by filter, **at both
222 and 333 MHz**, all twelve correctness cases bit-exact on actual Allegrex
silicon first. The two clocks came from two separate runs — the console's
firmware pins the CPU clock regardless of what the app requests, and pinned
to the opposite end each time, which is how both ends of the ask got covered
without ever confirming the app's own `scePowerSetClockFrequency` call does
anything on this particular firmware. Numbers and method in `BENCHMARKS.md`'s
two "decode measurement on the target hardware" entries.

Two things came out of it that were not known going in, and are worth
carrying forward:

- **BCIF scales badly with size on this CPU specifically.** 5.3x slower than
  a linear prediction from its own screen-sized number at 333 MHz, and the
  same anomaly within 1% at 222 MHz — clock-independent, so not a fluke of
  one frequency, and invisible on every x86 measurement this project has
  taken. See the "Removing BCIF" entry below and the BENCHMARKS.md write-up
  for what's known and what's still a hypothesis (likely cache/TLB pressure
  from reading four separated planes at once — not confirmed, no PSP
  profiling tooling exists yet to confirm it).
- **The clock-scaling cross-check came out clean.** Every one of the eight
  (size, filter) cells scaled within 1.5-1.9% of the 1.5x that 333/222 MHz
  predicts, which is a useful confirmation that 15-rep medians on this
  hardware are precise enough to trust for future PSP measurements, not just
  this one.

Two things that would follow naturally now that a real result exists:

- **Done 2026-09-18 — decode straight into a GPU texture.** `pxl_stream_new_ex(cb,
  user, fmt)` converts each row to `PXL_OUTPUT_RGBA8888`/`RGB565`/`RGBA5551`/
  `RGBA4444` before the callback sees it, folding the conversion into the
  existing per-row write rather than adding a pass — exactly the row window
  the memory work already added, given a format parameter. Only defined for
  8-bit, non-indexed RGB/RGBA sources; `pxl_stream_image()` still returns the
  true native pixels regardless, since the per-row predictors need real 8-bit
  values to stay correct row to row. Verified: `tests/roundtrip.c` checks
  every converted byte against an independently-written reference for all
  three packed formats plus RGBA8888, on both a 4-channel and a 3-channel
  (alpha-defaults-opaque) source, and rejects the geometry that is not
  defined for it; `psp/main.c` re-runs the same check with a MIPS-side
  reference and adds a throughput row (`strm565`). **Measured on real
  hardware 2026-09-18** (same PSP-3008): correctness matched, and the
  streaming+conversion path costs 37.7-43.5% more decode time than a plain
  `pxl_decode()` — not free, but still ~2x libpng's plain RGBA8888 decode
  rate on the same hardware. See `BENCHMARKS.md`'s "GPU-texture streaming
  and indexed-palette conversion, real hardware" entry. Swizzled output (an
  8-row window instead of 1) is not done; nothing needs it without a
  concrete texture-cache-locality case to measure against, and one-row
  PXL_OUTPUT_* conversion did not need it either.
- **Done 2026-09-18 — indexed mode maps onto the hardware.** Confirmed against
  `pspgu.h` directly rather than from memory: `GU_PSM_T4`/`T8` are real,
  documented `sceGuTexMode` formats, and 5650/5551/4444/8888 are marked valid
  CLUT formats as well as texture ones. An indexed `.pxl`'s index bytes
  already need no conversion at all — `pxl_decode()` without
  `pxl_image_expand()` hands back exactly the packed indices `GU_PSM_T4`/`T8`
  read. `pxl_convert_palette(img, fmt, out)` closes the one remaining gap,
  the palette itself (≤256 entries, reusing `pxl_stream_new_ex`'s own bit
  math since a palette entry and a pixel are the same conversion): a caller
  now needs zero conversion code to get from an indexed `.pxl` to a texture
  the GE samples directly. For UI art that is a quarter of the memory and the
  bus traffic of the equivalent RGBA8888 texture. Verified against an
  independent reference on both x86 (`tests/roundtrip.c`) and real MIPS
  output (`psp/main.c`, headless-checked and, as of 2026-09-18, confirmed on
  real PSP-3008 hardware too). (`GU_PSM_DXT1/3/5` constants also
  exist in `pspgu.h`, but are absent from `sceGuTexMode`'s own documented
  format list — not something this project is relying on.)

Note BCIF is excluded from all of the above: its plane split completes no row
until the last byte, so it cannot stream into a texture. That was already the
third independent argument against it, and the hardware-throughput result
above added a fourth — see the "Removing BCIF" entry in Held, below.

### Measure the full load pipeline, not just decode

Raised 2026-09-17: decode MB/s is not the same claim as "time to a drawable
texture." Memory Stick I/O, any RGBA8888-to-native-format conversion, and the
VRAM upload all sit between `pxl_decode` returning and `sceGuDrawArray`
actually being able to use the result, and none of them are in the numbers
measured so far. What's known without a new measurement: I/O should favour
PXL further (its files are already smaller than PNG's, so there is less to
read off a slow Memory Stick), and the VRAM-copy step is a shared, roughly
equal cost for both formats that dilutes the *relative* percentage gain
without reversing which one is faster. The conversion step is the one that
could go either way today — neither PXL nor libpng outputs a native GE format
directly — but PXL is positioned to close it first via the GPU-texture item
above, which libpng has no equivalent path for.

What to build: a real `Memory Stick -> file -> decode -> usable texture ->
sceGuDrawArray` timer, against the same for libpng, on the PSP. The code and
correctness can be built and verified under `PPSSPPHeadless` now the same way
`psp/main.c` already is; the timing itself needs real hardware, which is not
available while the console is elsewhere. See `psp/README.md`'s pattern for
how to keep those two states honestly separate when this is picked up.

### Packed native pixel formats (RGB565 / RGBA5551) for PSP textures

Raised and measured 2026-09-17, not decided — see the RESEARCH.md entry of
the same name. Storing pixels already at PSP-native precision (not
lossily converted from 8-bit) compresses smaller and halves the raw bytes
zstd has to move, which this project's own numbers suggest should speed up
decode too. Needs a real container-format decision (a geometry PXL's header
cannot currently describe) and a corpus of natively-565 PSP textures this
project does not have yet, so it stays a recorded, evidenced option rather
than a plan.

### Get an official MIPS decoder-size number through the real build pipeline

Raised 2026-09-17 while checking whether WebP could even run on PSP: a quick
`bench/mindec_pxl.c` cross-compile came to 267 696 bytes of MIPS `.text`
against the published x86 figure of 174 066 — alarming at face value, 54%
over. **Chased down the same day and it was mostly measurement artefact, not
PXL's code** — see RESEARCH.md's correction under "Does WebP even run on
PSP?". Two things inflated the raw number: `bench/mindec_pxl.c`'s
`printf`/`fopen` calls cost nothing on x86 (dynamic glibc) but statically
pull in newlib's stdio/dtoa internals on PSP, and — the bigger one — an
*empty* PSPSDK program already costs 121 004 bytes of `.text` before `main`
runs (`libcglue.a`'s start-up glue calls `sprintf` unconditionally), against
x86's 265-byte empty baseline. Net of each platform's own floor, PXL's actual
code is **9.6% bigger on MIPS**, not 54% — an ordinary RISC-vs-CISC code
density difference.

What's left: an *official* number through this project's real CMake build
(the `pxlcore` target, not a hand-linked one-off), reported the same way the
x86 174 066 figure is — net of the PSPSDK floor, since quoting a raw
MIPS `.text` figure next to the x86 one without subtracting each side's own
empty-program cost is exactly the mismatched comparison that caused this
scare in the first place. Low priority now that the scary version turned out
to be a measurement bug: "decoder size — met" does not appear to be in
question, just not yet stated for MIPS with a citable, reproducible number.


### Corpus gaps, now that the capture folders are known

Personal, gitignored, never to be committed — `tests/data/Screenshots`
(lossless PNG), `tests/data/Screenrecords` and `tests/data/Nightrecords` (H.264
MP4). Only aggregate numbers from any of them may reach README.

- **Screenshots: done 2026-09-15.** Wired in as `--with-shots`, plus a
  `CORPUS_ONLY` override so one content class can be measured on its own. Both
  refuse `--update-readme`, because a personal corpus cannot back a
  reproducible README number. Result: PXL 73.2% of PNG against 88.4% on
  photographs, closing the gap to JXL from 22.7 points to 7.4 and beating
  `oxipng -o max` by 5.8. **What remains is a committed synthetic corpus** —
  freely-licensed screenshots or rendered diagrams — so the README can show the
  format's best case rather than only its worst.
- **Nightrecords** is worth decoding to frames as a *hard* stills corpus: real
  night footage is sensor-noise-heavy, and noise is the worst case for lossless
  coding. It would show where the ratio collapses. Caveat: lossy-sourced, so it
  measures H.264's rendering of noise, not raw sensor output.
- **Screenrecords** gives a second animation corpus for `.apxl` *size* work,
  where only Anita exists today. It cannot be used for exact-match questions —
  see the MOVE entry in [`RESEARCH.md`](RESEARCH.md).


### Decide cross-frame long-distance matching per stream

Currently `.apxl` enables LDM whenever the level is at least `APXL_LDM_MIN_LEVEL`.
Measured over the Anita dataset, that is right for finished frames and wrong for
line art: concatenated raw RGBA over 16 frames of one shot gains **71.7%** on
`composition` and **loses 2.3%** on `sketch`. The real encoder agrees on the
first half — 16 composition frames come out at 68.1% of the APNG.

So the flag should follow the content, not the level. What is missing is the
decision rule and its threshold, and neither can come from the two shots measured
so far. Sweep the corpus first.

Note this is a pure encoder-side choice: LDM affects how the stream was produced,
not how it is read, so nothing in the format or the decoder changes.

### Aggregate animation measurements per shot, never per frame

Consecutive frames within one shot are near-duplicates, so sampling "the first N
frames" of a corpus measures one shot and reports it as a corpus. This is not
hypothetical: on the same data and the same encoder, `sketch` read 56.7% under
frame sampling and 44.3% under one-frame-per-shot, and `composition` 75.4% vs
61.5%. Roughly 12 points of spread from sampling alone, which is wider than most
improvements worth chasing.

Any animation benchmark script added under `bench/` should aggregate per shot and
state its sampling in the output, so a stale number cannot be mistaken for a
regression later.

## Corpora

`tests/data/` holds a handful of small files actually committed to the repo
(see `tests/data/README.md`) plus a much larger set of gitignored downloads,
each named in a `.gitignore` comment. Correction, 2026-09-18: this section
previously called Kodak and the PNG test suite "committed" -- checked against
`.gitignore` directly rather than assumed, and they are not; only two tiny
reference files (one PNG, one APNG, plus their `.pxl`/`.apxl` encodes) are
actually tracked. Present locally:

- Kodak (24 photographs) and the official PNG test suite — gitignored, and
  what the corpus table measures. How they were fetched is not currently
  documented anywhere in the repo.
- CLIC 2020 mobile train, 1048 photographs, 3.8 GB. High-resolution
  photographic content, largely unexercised so far.
- Anita industrial animation, 16871 frames in 367 shots, 11 GB, three passes per
  shot. The first real hand-drawn animation available to the project and the
  source of the cross-frame findings above.
- Synthetic screenshots from Wikimedia Commons, fetched reproducibly by
  `bench/synthetic_png.sh` (not committed, same reasoning as below) — see
  "Corpus gaps" below for what this closed.
- Animated GIFs from Wikimedia Commons, fetched reproducibly by
  `bench/gif_corpus.sh`: 66 files, 101 MB, three categories (Animated pixel
  art, Throbbers, Animated diagrams) chosen for the indexed-animation
  question in "Verify: is `.apxl`/APNG actually finished?" above — GIF is
  always ≤256 colours/frame by format definition, so this is exactly the
  content class `.apxl` currently has no format for. Licences vary (mostly CC
  BY-SA, some public domain) and are recorded per file in the corpus's own
  `MANIFEST.tsv`, the same pattern `synthetic_png.sh` uses. Not yet measured
  against anything -- fetching it only closes the "no corpus exists" gap.

The gap worth naming: there is still no corpus of synthetic non-photographic
stills — UI screenshots, diagrams, rendered text. That is the content where a
PNG-replacement format is most often used in practice and where PXL's own
philosophy claims strength, and it is exactly what neither Kodak nor CLIC nor
Anita covers.

## Held

Not scheduled, but not forgotten.

- **Removing BCIF.** The decoder-size motive is gone — measurement showed
  BCIF was not what bloated the decoder. But 2026-09-17's PSP-3008 runs gave
  it two fresh ones. First, BCIF's one-shot decode scales 5.3x worse than a
  linear prediction between a screen-sized and a texture-sized image, on real
  Allegrex hardware, reproduced twice. Second, weighed against libpng rather
  than only against PXL's own other filters: BCIF beats libpng 2.9x at
  screen size like the rest of PXL does, but **loses to libpng at texture
  size** (0.54x — libpng decodes the same pixels in about half the time) —
  see BENCHMARKS.md for both. That is now a fourth independent argument
  (decoder size retracted, texture streaming, wider-corpus size, and now
  target-hardware throughput), still not acted on: the encoder does pick
  BCIF on `sketch`, so removing it is not free on line art, and the one
  hardware sample so far is one console, one image pattern.

  **Partial mitigation shipped 2026-09-17**: `-s` / `PXL_ENCODE_FAST_DECODE`
  (see RESEARCH.md) lets a caller opt BCIF (and ADAPTIVE) out at encode time,
  for 2.9% more size on real UI content. That is a user-facing escape hatch,
  not a resolution of this question — BCIF is still the *default* choice for
  photographic content, so anyone not already reaching for `-s`/`-p` still
  gets it, and removing BCIF from the format outright remains a separate,
  bigger decision from adding a flag that lets people avoid it.
- **A better test oracle.** PIL ignores `tRNS` on grayscale and palette images,
  which already produced one phantom bug that cost real time to retract. If the
  suite grows over PngSuite, compare with `magick compare -metric AE` or against
  decoded RGBA bytes.
- **Verify the PSP GE's 565/5551/4444 channel order independently.** A
  sibling project (the Mogeko Castle PSP port) reported 2026-09-18 that
  rendering on real PSP-3008 hardware came out red/blue-swapped, tracing it
  to the GE's packed colour formats being `B`-first, not `R`-first the way
  `pspgu.h`'s own doc comments and this project's `pxl_stream_new_ex`/
  `pxl_convert_palette` both assume — see `RESEARCH.md`'s entry with the
  same name. Secondhand, not yet reproduced on this project's own test
  content on real hardware; not acted on (no format change) until it is.
  Held rather than Now because reproducing it needs the PSP-3008 hardware
  window, same constraint every other real-hardware entry here has had.

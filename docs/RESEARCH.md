# Research Journal

What we tried, what worked, what we rejected and **why**. A rejected idea with a
reason is worth more than a forgotten idea: without a record, a month from now
we will spend a day on something we already checked.

Every entry: idea → why → result → decision. We do not duplicate numbers, we
reference [BENCHMARKS.md](BENCHMARKS.md).

---

## Accepted decisions

### zstd instead of DEFLATE
**Why:** the foundation of the format. DEFLATE in PNG is 1996, a 32 KB window.
**Result:** ~85–88% of PNG across the whole corpus, decode 2.1 times faster than libpng.
**Decision:** accepted, this is the core of the format. But see the size problem below.

### PNG-style per-row filters (adaptive)
**Why:** keep PNG's strong side — a predictor per row, cheap and works well on
real images.
**Result:** matches or beats PNG's own filtering, while feeding zstd instead of
DEFLATE. Progressive decode is preserved: a row depends only on the row above.
**Decision:** accepted, this is the format's main path.

### Metadata is carried over byte for byte
**Why:** PXL must be a safe replacement for PNG, not lose EXIF and ICC.
**Result:** `eXIf`, `iCCP`, `cICP`, `gAMA`, `cHRM`, `sRGB`, `pHYs`, `tIME`,
`tEXt`/`zTXt`/`iTXt` and unknown ancillary chunks make the round trip
unchanged.
**Decision:** accepted. `PLTE`, `tRNS`, `sBIT`, `bKGD`, `hIST` are deliberately
not carried over — the format canonicalizes palette and transparency into real
channels, and these chunks would become invalid.

---

## Rejected ideas

### Tiling (splitting into squares) — rejected
**Why it was proposed:** block processing gives better locality and opens up
parallelism.
**Why we rejected it:** it breaks three principles at once. Top-to-bottom
progressive decode stops working, the decoder grows heavier from tile
management, and it is a departure from PNG's per-row architecture, for the
simplicity of which the format exists in the first place. Plus this is
V2-level functionality, and V1 is not finished yet.
**Status:** closed for V1. Do not reopen without a measurement showing a gain
of more than 5% while keeping streaming decode.

### V2 in any form — deferred
**Why:** V1 is not ready. Until the "decoder size" priority is met, adding
capabilities is harmful: every new capability makes the decoder heavier, and it
already loses to libpng by a factor of four.
**Status:** frozen until the V1 goals are met.

---

## Open questions and confirmed problems

### Decoder size — priority not met
**Problem:** the built PXL decoder is 861 KB against 209 KB for libpng+zlib
(measurement 2). Our own code is only 19 KB, everything else is libzstd, and
the compressor (570 KB) gets pulled into the binary too, which the decoder does
not need.
**What is known for certain:** the floor for the zstd-based variant is 285 KB
(pure `ZSTD_decompress`), which is already more than all of libpng. The target
is 209 KB.
**What to do next:** first stop pulling the compressor into decoding builds —
this should give 861 → ~285 KB, and it is most likely "free", with no format
changes. Beyond that, 285 KB runs into libzstd itself, and that is already a
question of either zstd build options or the choice of compressor.
**Why this matters:** for the target hardware (PSP, 32 MB RAM, a 32-bit
little-endian system without fast floating point) decoder size is not
cosmetic.

### The comparison with QOI was stated incorrectly
**Problem:** it was claimed that PXL beats QOI. Measurement 3 shows the
opposite: QOI is faster on 24 out of 24 photographs, by roughly 30%.
**The truth:** PXL is 2.1 times faster than libpng, and files are 14.5% smaller
than PNG. QOI meanwhile barely compresses at all — 103.5% of PNG, meaning its
files are slightly larger than the originals.
**Decision:** fix the wording in the README to be honest. Catching up with QOI
on speed while keeping compression is an open task, not a stated fact.

### BCIF loses almost everywhere
**Problem:** `bcif-current` gives 88.15% of PNG, while plain `sub-med` gives
84.57% (measurement 1). On top of that BCIF is the only filter that breaks
progressive
decode, because splitting into color planes does not let a row be completed
before the last byte of the last plane arrives.
**Question:** why is it needed if it loses on both size and capabilities?
**Possible solution:** remove BCIF. This would simultaneously shrink the
decoder, simplify the specification and enable progressive decode for all files.
A measurement is needed: is there a class of images where BCIF wins noticeably.
If not — we remove it.

**Update 2026-07-26: the measurement was done, a class of images was found —
removal is on hold.** Per-stage breakdown (`bench/stages.c`, level 12, see
BENCHMARKS.md) on a 3000×3000 RGB photograph:

| filter | bytes | unfiltering ms |
|---|---:|---:|
| bcif | 11 458 339 | 20.9 |
| adaptive | 11 550 315 | 405.7 |
| delta | 12 198 006 | 19.9 |

So on photographic content BCIF gives the best size of all filters — 6% smaller
than delta and slightly smaller than adaptive — while unfiltering almost 20
times faster than adaptive. The claim "loses almost everywhere" rested on
measurement 1, where the comparison ran over the whole corpus, which is
dominated by the tiny files of the PNG test suite. BCIF loses only on palette
images (4 414 against 3 440 for delta), and it is unavailable for grayscale.
**What remains true:** BCIF still breaks progressive decode. That is an argument
against it, but now it is a conscious tradeoff of "size+speed versus
progressiveness", not a pure loss.
**What next:** the decision is deferred until adaptive unfiltering is optimized.
If adaptive can be sped up several times over, BCIF loses its speed advantage
and the conversation about removing it can be had again.

### Dropping libpng from the core
**Why:** libpng is needed only for reading and writing real PNGs, that is, for
conversion. The `.pxl` decoder itself does not need it.
**What is known:** the `.pxl` decoder already does not depend on libpng — the
minimal decoding program from measurement 2 builds with only pxlcore and libzstd.
**What next:** separate the converter (needs libpng) and the decoder (does not)
so that it is visible in the build. Then "decoder size" becomes an honestly
measurable number rather than the result of a manual experiment.

### Adaptive unfiltering is ~20 times slower than delta
**Problem (found 2026-07-26):** the stage measurement showed that with the
adaptive filter, unfiltering takes 65–87% of the whole decode time and is
1.8–6.5 times more expensive than ZSTD decompression. On a 3000×3000
photograph: 405.7 ms of unfiltering against 19.9 ms for delta, at a comparable
amount of work.
**Diagnosis (hypothesis, not confirmed by reading the code):** the row filter
choice is made inside the pixel loop, plus Paeth gives unpredictable branches.
**Side conclusion:** there is no point replacing ZSTD with something else. For
delta/bcif/none ZSTD already dominates (2–12% on unfiltering), meaning we are at
the floor there; for adaptive the bottleneck is our own code.
**Possible solution:** specialize the loops by row filter — hoist the choice
out, one tight loop for each of the five filters. The format, files and
compatibility do not change, so the improvement is "free" in the sense we need.
**The bet:** if adaptive gets to 60–80 ms, it becomes the default (best size at
acceptable decode), the conclusion about lagging behind QOI changes and the
question about BCIF reopens.

**Result 2026-07-26: done, the bet did not pay off.** The filter choice was
hoisted out of the pixel loop into `rowfilter_decode`: one tight loop per filter
type, a separate branch for the `prev == NULL` case (there UP reduces to a copy,
PAETH to SUB, AVG to a shift), the first `bpp` bytes were taken out of the loop
so that the main loop has no bounds check.

The numbers are in [BENCHMARKS.md](BENCHMARKS.md), the 2026-07-26 entry about
loop specialization: on a 3000×3000 RGB photo, adaptive unfiltering 405.7 →
339.6 ms (16%), on palette 0.656 → 0.485 ms (26%), on grayscale 136.7 → 106.5 ms (22%).

This is less than needed: we did not reach 60–80 ms, the gap with delta stayed
16-fold (339.6 against 20.8). The hypothesis "the switch in the loop is to
blame" was only partially confirmed — it explains 16–26%, not an order of
magnitude. So the main cost is in Paeth itself: three data-dependent comparisons
per byte, which the branch predictor does not guess.
**What this means for the conclusions:** none of the bet-dependent conclusions
change. Adaptive does not become the default, the lag behind QOI remains, BCIF
keeps its advantage in unfiltering speed (21.1 ms against 339.6), so the question
of removing it is still open, not settled.
**What next, if we come back to it:** what remains is to count filter choice
statistics over the corpus. If Paeth is chosen rarely, speeding it up is
pointless; if often — it is the only remaining place to apply effort, and it
requires either SIMD (which contradicts the "small portable decoder" goal) or
restricting the filter set, which is already a format change.
**Status:** the optimization is accepted and in the tree, the goal is not met.

### 32 rows at a time instead of one
**Idea (discussed):** compress not a row and not a square, but a band of ~32
rows.
**Status:** not checked. We take it only if it turns out to be "free" — that is,
without losing progressive decode and without growing the decoder. Requires a
measurement before any decision.

### Decoder size: dead code first, algorithms later
**Idea:** before cutting down the format for the sake of decoder size (shrinking
the set of
filters, throwing out BCIF), check how much in the decoder is simply superfluous.

**What was done.** A minimal decoder was built on each side — file → raw pixels,
no CLI, no metadata — and `.text` was measured after `strip`.

**Result.** Disabling `ZSTD_LEGACY_SUPPORT` and `ZSTD_MULTITHREAD` removed
137 KB out of 862. Another ~317 KB is the zstd compressor, which ends up in the
decoder only because `pxl_encode_ex` and `pxl_decode` live in the same
`src/pxl_codec.c`, and the linker cannot throw out half of an object file.
The numbers are in [BENCHMARKS.md](BENCHMARKS.md), the 2026-07-26 entry about decoder size.

**Why this changes the frame of the conversation.** Previously the conclusion
sounded like this: "the decoder is inevitably heavier than libpng because we
changed the compression algorithm". The measurement does not confirm that.
Against ~230 KB for libpng+inflate we have 725 KB now and ~408 KB after the TU
split — a comparable figure, obtained without a single change to the format. So
it is too early to trade compression for size: build first, algorithms later.

**What this means for the deferred decisions.** The argument "remove Paeth for
the sake of decoder size" weakened even further (it was already rejected: Paeth
accounts for 91% of unfiltered bytes). The question of removing BCIF stays open,
but the motive "it bloats the decoder" no longer works — it was not the one bloating it.

**What next:** split `src/pxl_codec.c` into encode and decode modules and
re-measure. This is purely mechanical work, the format is not affected, so there
are no compatibility risks; the difficulty is in the shared `static` helpers,
which will have to be moved into a third file.
**Status:** the legacy fix is accepted and in the tree, the TU split is the next step.

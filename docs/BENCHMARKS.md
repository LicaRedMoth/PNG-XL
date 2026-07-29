# Benchmark history

An **append-only** log. Old entries are never edited or deleted, even if the
numbers are out of date or turned out to be wrong — instead we add a new entry
below and explain what changed. The point of the log is to show the trend, not
to keep one "current" number.

Entry format: date, commit, hardware, what was measured, with what, result.
If a measurement is not reproducible (no tool, different hardware) — say so.

---

## 2026-07-28 — baseline measurements before fixing the philosophy

- **Commit:** `f5836cc` (+ uncommitted changes in the working tree)
- **Hardware:** Intel Pentium B960 @ 2.20GHz, 2 cores, 15.2 GiB RAM
- **Compiler:** gcc 16.1.1, Release build
- **Corpus:** 24 Kodak photographs + valid files from the official PNG test
  suite (`x*.png` excluded)

Three measurements needed so the philosophy would rest on facts, not belief.

### Measurement 1 — which filters actually get picked

`build/predlab -l 12` over the corpus. The tool runs every combination of
color transform and per-row predictor and prints the size of each. Out of the
186 corpus files `predlab` loaded 136 (50 skipped — files its simplified
loader cannot read; the full corpus is handled by `bench/corpus.sh`).

Total sizes over all 136 files, baseline is the original PNGs (15 498 076 bytes):

| variant | bytes | % of PNG |
|---|---:|---:|
| raw (no filter) | 17 877 176 | 115.35% |
| bcif-current (what the format uses today) | 13 662 066 | 88.15% |
| **sub-med** | **13 106 230** | **84.57%** |
| sub-paeth | 13 207 156 | 85.22% |
| ycocg-med | 13 575 791 | 87.60% |
| ycocg-paeth | 13 720 481 | 88.53% |
| sub-left | 13 662 066 | 88.15% |
| il-med | 15 288 895 | 98.65% |
| pl-left | 17 603 189 | 113.58% |

Winner distribution **by file** (136 files, the smallest-size variant for each
file, `raw` and `bcif-current` excluded):

| winner | files |
|---|---:|
| il-paeth | 30 |
| pl-paeth | 25 |
| il-left | 20 |
| sub-med | 18 |
| pl-left | 14 |
| il-up | 10 |
| pl-up | 5 |
| sub-left | 4 |
| il-med | 4 |
| sub-paeth | 3 |
| pl-med | 2 |
| ycocg-med | 1 |

Photographs only (24 Kodak files): `sub-med` 18, `sub-left` 4, `ycocg-med` 1,
`il-left` 1.

**What follows from this.** On photographs the choice is nearly degenerate —
`sub-med` takes 18 of 24. The spread of winners across the whole corpus comes
from the tiny PNG test suite files (32×32 and smaller), where the difference is
single bytes and overhead decides the outcome, not prediction quality. So the
many filter variants exist not for compression, but to avoid losing on
degenerate inputs. `sub-med` over the whole corpus gives 84.57% vs 88.15% for
the current BCIF — about 3.6 percentage points of headroom from swapping the
predictor, with no change to the format structure.

### Measurement 2 — decoder size vs libpng + zlib

Method: build minimal decoding programs (file into memory → pixels, nothing
else) statically, strip symbols (`strip`), compare the size of the `.text`
section. That way the linker pulls in exactly what the decoder actually uses.
An empty program for calibration is 281 bytes of `.text`.

| binary | `.text`, bytes |
|---|---:|
| empty program (baseline) | 281 |
| PXL decoder (pxlcore + libzstd.a) | 861 170 |
| libpng decoder (libpng.a + system libz.so) | 209 337 |
| `ZSTD_decompress` only | 284 978 |
| `ZSTD_compress` only | 570 165 |

For reference, individual object files and libraries:
`pxl_codec.c.o` — 18 808 bytes of `.text`, `pxl_io.c.o` — 413, `libpng.a` —
214 104, `libzstd.a` — 1 033 315, `libz.so` — 61 528.

**What follows from this, and it is bad news.** Our own decoding code is 19 KB,
an order of magnitude smaller than libpng. But the assembled decoder ends up
**4x heavier than libpng**: 861 KB vs 209 KB. The reason is that all of libzstd
lands in the binary, including the compressor (the compressor alone is 570 KB).
Even the pure zstd decoder is 285 KB, already more than all of libpng together
with zlib.

The "decoder size" priority is **not met** so far, and that has to be recorded
honestly. Useful figures for later: 285 KB is the floor for any zstd-based
variant (if the compressor pull-in is removed), 209 KB is the target, 19 KB is
our own contribution.

### Measurement 3 — honest decode into raw pixels vs QOI

`build/pxl_bench_rawdec <file> 15` over all 24 Kodak photographs. This measures
exactly what matters for a viewer or a game: the file is already in memory, we
decode into a pixel buffer. No re-encoding back into PNG, unlike the decode
column in `bench/bench.sh`.

Sum over 24 files (median of 15 repeats per file):

| decoder | total ms | total bytes | % of PNG |
|---|---:|---:|---:|
| QOI | 124.4 | 16 501 520 | 103.5% |
| **PXL** | 163.7 | **13 633 519** | **85.5%** |
| libpng | 347.5 | 15 941 880 | 100% |

Per file: QOI is faster than PXL on **24 of 24** files.

**What follows from this (see also the 2026-07-26 entry below: part of this
time turned out not to be unavoidable).** PXL decodes roughly 2.1x faster than
libpng and its files are 14.5% smaller — that is a real achievement and the
format's main argument. But the claim "we beat QOI on speed" is **wrong**: QOI
is faster than us on every file in the corpus, by roughly 30%. On the other
hand QOI does not compress — its files are even slightly larger than the
original PNGs (103.5%), while ours are 14.5% smaller. The correct phrasing:
PXL sits between QOI and libpng, closer to QOI on speed and better than both
on size.

---

## 2026-07-26 — decode broken down by stage: ZSTD vs unfiltering

The date on the previous entry (`2026-07-28`) has a typo; the measurements
actually ran in the same period. Not fixing it, per append-only; noted here.

- **Commit:** `f5836cc` (+ uncommitted changes in the working tree)
- **Hardware:** Intel Pentium B960 @ 2.20GHz, 2 cores, 15.2 GiB RAM
- **Compiler:** gcc 16.1.1, Release build
- **Tool:** `bench/stages.c` → `build/pxl_bench_stages <file> [reps] [level]`
- **Method:** the file is already in memory. `ZSTD_decompress` time and
  unfiltering-into-raw-pixels time are measured separately, for every
  available color filter. Median of 9 repeats.

The question was blunt: what costs more during decode — ZSTD decompression or
our unfiltering. The answer is "depends on the filter", and that matters more
than the question itself.

### Level 12

| image | filter | zstd ms | unfilter ms | total ms | unfilter share | bytes |
|---|---|---:|---:|---:|---:|---:|
| Photorealistic 3000×3000 RGB | adaptive | 222.213 | 405.655 | 627.868 | 64.6% | 11 550 315 |
| | bcif | 223.706 | 20.886 | 244.591 | 8.5% | 11 458 339 |
| | delta | 233.440 | 19.892 | 253.333 | 7.9% | 12 198 006 |
| | none | 221.216 | 4.915 | 226.131 | 2.2% | 17 515 996 |
| fs8 3000×3000 grayscale | adaptive | 74.227 | 136.717 | 210.944 | 64.8% | 4 454 269 |
| | delta | 69.220 | 9.715 | 78.936 | 12.3% | 4 382 065 |
| | none | 64.504 | 1.446 | 65.950 | 2.2% | 4 300 663 |
| palette chart 258×200 | adaptive | 0.101 | 0.656 | 0.757 | 86.7% | 5 473 |
| | bcif | 0.086 | 0.110 | 0.195 | 56.1% | 4 414 |
| | delta | 0.069 | 0.109 | 0.178 | 61.2% | 3 440 |
| | none | 0.165 | 0.016 | 0.180 | 8.7% | 27 196 |

### Level 1 (for comparison)

| image | filter | zstd ms | unfilter ms | total ms | unfilter share | bytes |
|---|---|---:|---:|---:|---:|---:|
| Photorealistic 3000×3000 RGB | adaptive | 86.723 | 389.339 | 476.061 | 81.8% | 12 013 840 |
| | bcif | 83.117 | 27.189 | 110.306 | 24.6% | 11 272 231 |
| | delta | 84.246 | 20.397 | 104.643 | 19.5% | 13 187 408 |
| | none | 86.818 | 5.453 | 92.271 | 5.9% | 21 006 764 |
| fs8 3000×3000 grayscale | adaptive | 25.750 | 124.388 | 150.138 | 82.8% | 4 409 386 |
| | delta | 26.366 | 9.922 | 36.287 | 27.3% | 4 531 605 |
| palette chart 258×200 | adaptive | 0.216 | 0.681 | 0.897 | 76.0% | 8 091 |
| | bcif | 0.105 | 0.111 | 0.216 | 51.5% | 5 954 |
| | delta | 0.174 | 0.106 | 0.281 | 37.9% | 6 711 |

**What follows from this.**

1. **For adaptive the bottleneck is our code, not ZSTD.** Unfiltering takes
   65–87% of the decode and is 1.8–6.5x more expensive than decompression.
   Swapping the compressor is pointless while that holds.
2. **For delta / bcif / none it is the opposite:** ZSTD dominates, unfiltering is
   2–12%. Here we are close to the floor that zstd sets.
3. **The adaptive vs delta gap in unfiltering time is roughly 20x**
   (405.7 vs 19.9 ms) for the same order of work. Not a property of the format
   but of the implementation: by the numbers, row filter selection is spinning
   inside the pixel loop. Specializing the loops by row filter changes neither
   the spec, nor the files, nor compatibility.
4. **On photo content BCIF does not lose, it wins.** 11 458 339 bytes vs
   12 198 006 for delta (6% smaller) and even smaller than adaptive
   (11 550 315), with unfiltering at 20.9 ms instead of 405.7. The previously
   held opinion that "BCIF loses everywhere" is not supported by these numbers.
   It only loses on the palette image (4 414 vs 3 440 for delta) and is
   unavailable for grayscale. The decision to remove BCIF is put on hold on the
   basis of this data.
5. Measurement 3 of the previous entry (163.7 ms total vs 124.4 for QOI) is
   worth rereading in this light: part of the gap behind QOI is adaptive
   unfiltering, i.e. potentially removable rather than inherent to the format.

---

## 2026-07-26 — specializing the unfiltering loops by filter type

- **Commit:** `f5836cc` + a change to `rowfilter_decode` in `src/pxl_codec.c`
- **Hardware:** Intel Pentium B960 @ 2.20GHz, 2 cores, 15.2 GiB RAM
- **Compiler:** gcc 16.1.1, Release build
- **Tool:** `build/pxl_bench_stages <file> 9 12`, median of 9 repeats
- **What changed:** filter selection lifted out of the pixel loop up to the row
  level. A separate tight loop per filter type, a separate branch for the first
  row (`prev == NULL`: UP → copy, PAETH → SUB, AVG → shift), the first `bpp`
  bytes handled outside the loop to keep the bounds check out of the body.
  Format, files and compatibility untouched; `ctest` — 2/2 pass.

### Level 12, after the change

| image | filter | zstd ms | unfilter ms | total ms | unfilter share | bytes |
|---|---|---:|---:|---:|---:|---:|
| Photorealistic 3000×3000 RGB | adaptive | 235.910 | 339.558 | 575.469 | 59.0% | 11 550 315 |
| | bcif | 208.902 | 21.135 | 230.036 | 9.2% | 11 458 339 |
| | delta | 238.333 | 20.820 | 259.152 | 8.0% | 12 198 006 |
| | none | 228.944 | 5.494 | 234.437 | 2.3% | 17 515 996 |
| fs8 3000×3000 grayscale | adaptive | 79.630 | 106.516 | 186.145 | 57.2% | 4 454 269 |
| | delta | 71.408 | 9.700 | 81.108 | 12.0% | 4 382 065 |
| | none | 70.982 | 1.432 | 72.414 | 2.0% | 4 300 663 |
| palette chart 258×200 | adaptive | 0.098 | 0.485 | 0.583 | 83.2% | 5 473 |
| | bcif | 0.087 | 0.112 | 0.199 | 56.2% | 4 414 |
| | delta | 0.070 | 0.107 | 0.177 | 60.6% | 3 440 |
| | none | 0.163 | 0.011 | 0.175 | 6.5% | 27 196 |

### Adaptive unfiltering: before and after

| image | before, ms | after, ms | gain |
|---|---:|---:|---:|
| Photorealistic 3000×3000 RGB | 405.655 | 339.558 | 16% |
| fs8 3000×3000 grayscale | 136.717 | 106.516 | 22% |
| palette chart 258×200 | 0.656 | 0.485 | 26% |

Sizes in bytes did not change by a single byte — expected, only the decoder
changed. The byte-for-byte match with the previous entry serves as a check that
the optimization broke nothing.

**What follows from this.**

1. **The goal was not reached.** We aimed for 60–80 ms, we got 339.6. The gap
   with delta shrank from 20x to 16x, the order of magnitude stayed.
2. **The hypothesis is only partly confirmed.** The switch inside the pixel loop
   did cost 16–26%, but it was not the main reason. The remainder falls on Paeth
   itself: three data-dependent comparisons per byte that do not predict. Branching
   did not go away just because we moved a different branch out of the loop.
3. **The conclusions of the previous entries stand.** Adaptive does not become
   the default, the gap behind QOI remains, BCIF keeps a 16x advantage in
   unfiltering at a smaller size on photos. The BCIF decision stays on hold, and
   the data is rather against removal than for it.
4. **Open question for the next step:** filter selection statistics over the
   corpus. If Paeth is chosen rarely there is no point optimizing it; if often,
   further speedup requires either SIMD (against the "small portable decoder"
   goal) or trimming the filter set, and that is already a format change.

---

## 2026-07-29 — filter selection statistics over the corpus

- **Commit:** `0a69963` (+ uncommitted: `bench/rowstats.c`, CMakeLists.txt)
- **Hardware:** Intel Pentium B960 @ 2.20GHz, 2 cores, 15 GiB RAM
- **Compiler:** gcc 16.1.1, Release build
- **Tool:** `build/pxl_bench_rowstats` (new, `bench/rowstats.c`)

Answers the open question of the previous entry: how often Paeth is chosen at
all. The tool counts not only "what adaptive would choose" but what actually
lands in the file: first it determines whether adaptive beats BCIF/delta/none on
size, and only for the winning files does it count the rows that the decoder
really unfilters.

### Photos (Kodak, 24 files)

BCIF wins the encoding in **all 24** files, adaptive in none. The Paeth share
among rows that adaptive *would* choose ranges from 7.2% (13.png) to 98.4%
(08.png), but those rows never land in the file. On photos the decoder does not
unfilter per-row filters at all.

### Rest of the corpus (165 readable files)

Adaptive wins in 57 files (34.5%). Over those winners only, i.e. over the rows
the decoder really unfilters:

| | rows | bytes |
|---|---:|---:|
| total | 5016 | 9 359 680 |
| of which Paeth | 4252 (84.77%) | 8 524 552 (91.08%) |

**What follows from this.**

1. **Paeth cannot be removed.** Where per-row filtering is applied at all, Paeth
   accounts for 91% of the unfiltered bytes. The idea of trimming the set to
   none/sub/up for the sake of decoder size is off: it would hit exactly those
   files where adaptive was chosen.
2. **The argument against removing BCIF got stronger.** 24/24 photos is the
   class of images our 14.5% vs PNG rests on. The decision stays on hold pending
   a timing measurement.
3. **Sample bias.** The 165 files are mostly 32×32 from the official PNG suite,
   so "34.5% of files" overstates adaptive's role. Quote the byte shares, not
   the file share.

## 2026-07-26 — decoder size: legacy zstd and the cost of a shared TU

**Commit:** 0a69963 (+ `ZSTD_LEGACY_SUPPORT` edit) · **Hardware:** Intel Pentium
B960 @ 2.20GHz · **Compiler:** GCC 16.1.1 · Release, `-O2`, `strip`, measuring
`.text` of a statically linked minimal decoder
(`bench/mindec_pxl.c`, `bench/mindec_png.c`: file → raw pixels, nothing else).

### Baseline: libpng

| | `.text` |
|---|---:|
| libpng (static) | 209 913 B |
| inflate + adler32/crc32 from libz | ~20 214 B |
| **total "PNG decoder"** | **~230 KB** |

The inflate contribution is computed from symbol sizes in `libz.so.1` (there is
no static zlib on the system), so it is an estimate, not an exact measurement.

### PXL

| | `.text` | Δ |
|---|---:|---|
| before | 862 KB | — |
| `ZSTD_LEGACY_SUPPORT=OFF`, `ZSTD_MULTITHREAD=OFF` | **725 106 B** | −137 KB |
| of which compress-side (unreachable for the decoder) | ~324 779 B | |
| of which decompress-side | ~130 398 B | |

The per-symbol breakdown is approximate: classification by name (`nm -S`), the
remainder falls on shared zstd code, the libc glue and PXL itself.

**What follows from this.**

1. **zstd legacy support was free fat.** PXL reads only the frames it wrote
   itself, it has no need for zstd 0.x frames. 137 KB went away without a single
   code edit and without a format change; the tests (`roundtrip`, `fuzz_decode`)
   pass.
2. **The main cost item is not the format but the build.** 317 KB of compressor
   land in the decoder only because `pxl_encode_ex` and `pxl_decode` sit in one
   object file (`src/pxl_codec.c`, 1465 lines): the linker pulls the whole section.
   Splitting the TU into encode/decode should give ~408 KB — already the same order
   as libpng rather than a threefold loss.
3. **The "decoder size" priority is achievable.** The earlier conclusion that
   "the decoder is heavier than PNG because the algorithm differs" was wrong: it
   was heavier because of dead code, not because of the algorithm.

### Aside: a bug in the harness, not in the library

`mindec_pxl` was built with `-Iinclude` (no such directory in the tree) and
silently picked up a stale `/usr/local/include/pxl.h` from an earlier install. On
the struct layout mismatch this produced `*** stack smashing detected ***`. With
`-Isrc` the decoder returns `258x200 sum=14022381` — byte for byte the same as the
libpng minimal decoder on the source PNG. The `.text` measurements were unaffected
(725 106 B in both cases), but the takeaway: `/usr/local` in build paths is a
source of false failures.

## 2026-07-26 — TU split: the decoder got lighter than libpng

**Commit:** working tree at 0a69963 · **Hardware:** Intel Pentium B960 @ 2.20GHz
· **Compiler:** GCC 16.1.1 · Release, `-O2`, `strip`.

`src/pxl_codec.c` (1465 lines) split into three translation units:
`pxl_codec_common.c` (geometry, palette, shared helpers),
`pxl_codec_encode.c`, `pxl_codec_decode.c`. Format and public API unchanged.

### Methodology fixed

The previous measurement (725 106 B) linked with `-static`, so all of glibc
landed in `.text`. In the current build that is 542 072 B out of 671 789 B — i.e.
the earlier figure was three quarters libc and comparing it to libpng was
meaningless. Here libc is dynamic for both decoders, `.text` holds library code only.

| decoder | `.text` |
|---|---:|
| PXL, shared TU (before) | 715 890 B |
| **PXL, split TU (after)** | **162 034 B** |
| libpng 1.6 (`libpng16.a`, zlib dynamic) | 209 913 B |
| + inflate/crc32 from libz (estimated via `nm`) | ~20 214 B |
| **total "PNG decoder"** | **~230 127 B** |

### What follows from this

1. **Minus 553 856 B, 4.4x — without a single algorithm edit.** No compressor is
   left in the decoder: `nm` finds zero `ZSTD_compress*` symbols. Exactly three
   objects link — `pxl_codec_common.o`, `pxl_codec_decode.o`, `pxl_io.o`.
2. **The "decoder size" priority is met: 162 KB vs ~230 KB for PNG, 30%
   lighter.** The "~408 KB" estimate from the previous entry turned out
   pessimistic because it was computed off a libc-polluted base.
3. **Verified it is the same decoder.** `roundtrip` and `fuzz_decode` pass; the
   old and new builds on `Photorealistic_cover` give an identical
   `3000x3000 sum=2878000537`, matching the libpng minimal decoder.

**For the future:** any `.text` measurement with dynamic libc only, otherwise you
are measuring glibc, not the format.

## 2026-07-26 — branchless PAETH, and why BCIF stays

**Commit:** working tree at 0a69963 · **Hardware:** Intel Pentium B960 @ 2.20GHz
· **Compiler:** GCC 16.1.1 · Release. Median of 5 runs.

Stage breakdown of decode time (`pxl_bench_stages`) ahead of the decision to
remove BCIF. Format and public API unchanged.

### 1. PAETH rewritten branchless

`pxl_paeth` picked the predictor via `if`. Image data makes the choice
unpredictable, and every misprediction costs the pipeline. Replacing it with
masks (`m_a`, `m_b`) removes the branch entirely, the result is bit for bit the same.

| stage | before | after |
|---|---:|---:|
| unfiltering, `Photorealistic_cover_fs8` (3000×3000, gray) | 405 ms | **105 ms** |
| unfiltering share of decode | 88% | 68.8% |

An isolated microbenchmark on 27 MB confirms the cause: the branchy version
79 MB/s, branchless 143 MB/s (1.8x).

### 2. Rejected: keeping neighbors a/c in registers

The hypothesis was that byte `i` depends on `i-bpp`, which the previous iteration
had just written, and that store-to-load forwarding sits on the critical path.
Specializations for bpp 1/2/3/4 were written, holding `a` and `c` in variables.

**Result: 177 ms vs 181 ms, no difference — code removed.** The hypothesis is
refuted by a control measurement: SUB has exactly the same dependency on
`cur[i-bpp]` yet runs at 1150 MB/s vs 143 for PAETH. So the ceiling is set by the
per-byte cost of the formula itself, not by memory access.

Order of magnitude for calibrating future ideas (27 MB, single thread):

| filter | speed |
|---|---:|
| UP | 3920 MB/s |
| SUB | 1150 MB/s |
| PAETH branchless | 143 MB/s |
| PAETH branchy | 79 MB/s |

### 3. BCIF is not removed: it wins on both size and speed

The plan was to remove BCIF as losing everywhere. The measurement refuted that.

| file | mode | size | unfiltering | decode total |
|---|---|---:|---:|---:|
| Photorealistic_cover (3000×3000 RGB) | adaptive | 12 013 840 B | 193.8 ms | 275.9 ms |
| | **bcif** | **11 272 231 B** | **22.3 ms** | **104.2 ms** |
| palette_color_test_chart (200 rows) | adaptive | 8 091 B | 0.39 ms | 0.61 ms |
| | **bcif** | **5 954 B** | **0.11 ms** | **0.22 ms**  |

On color files BCIF is 6.2% and 26.4% smaller and unfilters 8.7x and 3.6x
faster. The earlier conclusion that "BCIF loses everywhere" was drawn on an
incomplete set.

**Why.** `pxl_bench_rowstats`: adaptive picks PAETH on 94.8% of rows and 98.6%
of bytes, i.e. it drives the decoder into the most expensive branch. Yet on both
color files BCIF wins the encoding, so the number of rows the decoder really
unfilters in adaptive mode is **zero**. On this set the expensive PAETH branch is
simply never selected.

BCIF does not apply to 8-bit gray: there adaptive wins (4 569 905 B). The same
place shows a separate reserve — `delta` gives +0.37% size (4 586 860 B) but
unfiltering of 17.3 ms vs 105.2 and a full decode of 71.6 ms vs 153.0, twice as
fast. Filter selection optimizes size only and ignores the decode cost.

**Decision: BCIF stays.** Candidate for the next step — factor unfiltering cost
into filter selection; that is an encoder-only edit, the format does not change.

---

## 2026-07-28 — USC-SIPI converted to PNG, size across the whole corpus

- **Commit:** 0a69963
- **Hardware:** Intel Pentium B960 @ 2.20GHz, 2 cores, 15.2 GiB RAM
- **Measured:** PXL size vs source PNG over 396 files, split by color type
- **With:** `pxltool c -l 12`, source PNGs written by libpng at default settings

| Subset | Files | PNG, B | PXL, B | PXL/PNG |
|---|---:|---:|---:|---:|
| Kodak, RGB | 24 | — | — | 88.6% |
| USC-SIPI, RGB (ct2 bd8) | 51 | 70 743 605 | 69 597 080 | 98.4% |
| USC-SIPI, gray (ct0 bd8) | 158 | 33 586 150 | 33 700 173 | 100.3% |
| USC-SIPI, gray (ct0 bd1) | 1 | 1 648 | 1 888 | 114.6% |
| Whole corpus | 396 | — | — | 97.6% |

**Result: the "15% smaller than PNG" claim holds only for true-color
photographs.** On 8-bit grayscale USC-SIPI plates we are 0.3% *larger* than PNG,
on the single 1-bit file 14.6% larger.

**Why.** Two reasons, both mechanical. The color filter operates between
channels, so on one channel it does nothing and only row filters + zstd remain
against row filters + DEFLATE. And USC-SIPI aerials and textures are
high-frequency: there is little spatial correlation left for the filters to
remove, and zstd's edge over DEFLATE nearly vanishes. Kodak is smooth
photography, the gain there is real.

Not isolated: exactly why zstd-12 loses to DEFLATE on these plates. Noted
separately — on `misc/5.1.09.png` (256x256, 1 channel) PNG is 42 351 B, PXL
level 12 is 42 621 B and level **19 is 43 768 B**, i.e. a higher level makes the
file bigger. Worth a look on its own.

**Decision:** the README claim is narrowed to photographic content rather than
quietly kept. 1-bit input is a separate weak spot, no work done on it yet.

## 2026-07-29 — what the unfilter specialization cost in decoder size

**Commit:** 2d8d9ec · **Hardware:** Intel Pentium B960 @ 2.20GHz · **Compiler:**
GCC 16.1.1 · Release, `strip`, `.text` of `bench/mindec_pxl.c` linked against
`libpxlcore.a` + static `libzstd.a`, **libc dynamic** (the methodology fixed in
the TU-split entry).

The specialized unfilter loops duplicate code per filter type, and Release moved
to `-O3`. Both inflate `.text`, so the two factors are separated by rebuilding
the same source at `-O2`.

| build | `.text` | Δ |
|---|---:|---:|
| before specialization, `-O2` (previous entry) | 162 034 B | — |
| current code, `-O2` | 164 210 B | +2 176 B |
| **current code, `-O3` (shipping)** | **164 466 B** | **+256 B** |

**Specialization costs 2 176 B, `-O3` costs 256 B, 2 432 B / +1.5% in total** —
against the 2.5x decode speedup this is the cheapest trade in the project so far.

### Where the 164 KB actually sits

| object | `.text` |
|---|---:|
| `pxl_codec_decode.c.o` | 13 553 B |
| `pxl_codec_common.c.o` | 2 232 B |
| `pxl_io.c.o` | 413 B |
| **our code, total** | **16 198 B** |
| zstd decompressor (remainder) | ~148 268 B |

**90% of the decoder is zstd, 10% is PXL.** Shaving our own code further is
pointless: even deleting the whole codec would remove a tenth. Decoder size is
now a question about the zstd build configuration, not about our algorithms.

### Checks

- `nm`: zero `pxl_encode` and zero `ZSTD_compress` symbols — no encoder leaks in.
- The stripped binary decodes: `258x200 sum=14022381` on
  `RGB_24bits_palette_color_test_chart.pxl`.
- **Baseline not re-measured:** there is no static libpng on the system anymore,
  so the comparison keeps the recorded ~230 127 B (libpng16.a 209 913 +
  inflate ~20 214). Cross-check: `libpng16.so.16.58.0` has 170 733 B of `.text`,
  but that is PIC and includes the *encoder*, so it is not directly comparable.

**Result: 164 466 B vs ~230 127 B, 28.5% lighter than a PNG decoder** (was 29.6%
before specialization). The priority holds.

## 2026-07-29 — where decode time goes, per stage

**Commit:** 2d8d9ec · **Hardware:** Intel Pentium B960 @ 2.20GHz · **Compiler:**
GCC 16.1.1, `-O3` (confirmed in `flags.make`; no `-march=native`, the binary
stays portable) · `bench/stages.c`, level 12, 3 repetitions · corpus: every 5th
PNG of USC-SIPI + Kodak, 47 files.

The goal was the stopping rule agreed earlier: if zstd owns 75-80% of decode,
unfiltering is not worth optimizing further.

### Attributing each file to the filter the encoder actually picks

Earlier entries measured *forced* filter modes, which is not what ends up in a
file: the still encoder tries all filters and keeps the smallest. Weighting each
file by its winning mode gives the honest profile.

| mode | files | zstd | unfilter | total |
|---|---:|---:|---:|---:|
| adaptive | 32 | 95.8 ms (52.4%) | 87.0 ms (47.6%) | 182.8 ms |
| BCIF | 11 | 137.5 ms (86.5%) | 21.5 ms (13.5%) | 159.0 ms |
| none | 3 | 1.9 ms (95.3%) | 0.1 ms (4.7%) | 2.0 ms |
| delta | 1 | 0.0 ms (9.0%) | 0.3 ms (90.8%) | 0.4 ms |
| **all** | **47** | **235.2 ms (68.3%)** | **108.9 ms (31.7%)** | **344.1 ms** |

**The answer is bimodal, and the average hides it.** On BCIF files zstd is 86.5%
and the stopping rule is already met. On adaptive files unfiltering is 47.6%, and
adaptive wins 32 of 47 files (68%). Overall zstd is 68.3% — below the 75% line,
so the remaining headroom is real and it all sits in adaptive unfiltering.

### Which row filter that headroom is made of

`bench/rowstats.c`, same corpus: 26 112 rows, 33 423 360 filtered bytes.

| filter | rows | % rows | bytes | % bytes |
|---|---:|---:|---:|---:|
| AVG | 14 699 | 56.3% | 23 531 520 | **70.4%** |
| PAETH | 8 145 | 31.2% | 6 542 336 | 19.6% |
| SUB | 1 269 | 4.9% | 2 018 048 | 6.0% |
| UP | 1 924 | 7.4% | 1 270 272 | 3.8% |
| NONE | 75 | 0.3% | 61 184 | 0.2% |

Restricted to the 32 adaptive winners (16 896 rows a decoder really unfilters),
PAETH is 37.2% of rows but only 24.9% of bytes.

**Two earlier claims do not survive this measurement:**

1. A code comment in `pxl_codec_decode.c` read "AVG ~82%, PAETH ~12%". On the
   widened corpus it is AVG 70.4% / PAETH 19.6% / SUB 6.0%. The comment was
   written before the corpus grew; it is corrected in place. The conclusion it
   supported (specialize AVG and PAETH, leave SUB to the generic path) still
   holds, and SUB at 6% rather than 1% is still not worth the code size.
2. "BCIF loses everywhere, drop it." It does not. BCIF wins 11 of 47 files, and
   because those files are the large USC-SIPI images it wins the *summed bytes*
   on both corpora (USC-SIPI 12.88 MB vs adaptive 19.89 MB; Kodak 2.72 MB vs
   2.84 MB). Adaptive wins on *file count*, BCIF on *total size*. Both facts are
   true at once and neither is a bug — the encoder picks correctly per image.
   **BCIF stays.**

### Why AVG is hard to make faster

AVG reconstruction is `cur[i] = in[i] + ((cur[i-bpp] + prev[i]) >> 1)`. Each byte
depends on a byte `bpp` positions earlier in the same row, so the row is a serial
dependency chain. SIMD cannot break it; the most libpng's SSE2 paths do is work
on `bpp` bytes at once, which the compile-time-constant BPP specialization
already lets GCC do at `-O3`. An optimistic 1.7x on AVG alone would be ~10% of
total decode, and it would cost x86 intrinsics in a decoder whose portability
(PSP has no SSE2) and size are stated priorities.

**Decision: stop optimizing unfiltering.** zstd at 68.3% is close enough to the
75% line that the remaining win is ~10% of decode for a portability regression.
Decode is already 2.5x faster than before specialization. The next honest lever
is the zstd build configuration, which is also 90% of decoder size.

### Reproducing

```sh
cmake --build build --target pxl_bench_stages pxl_bench_rowstats -j4
FILES=$(find tests/data/USC-SIPI-Image-Database \
             tests/data/Kodak-Lossless-True-Color-Image-Suite \
        -name '*.png' | sort | awk 'NR%5==1')
./build/pxl_bench_rowstats --quiet -l 12 $FILES
# per-stage, weighted by the mode the encoder chooses (see the table above):
# for each file, `pxltool c -l 12` then `pxltool info | grep filter`, and take
# that mode's row from `pxl_bench_stages <file> 3 12`. Match case-insensitively:
# info prints "BCIF", stages prints "bcif".
```

## 2026-07-29 — callgrind: the unfilter loop is compulsory-miss bound, not cache bound

**Commit:** ab0a39e · **Hardware:** Intel Pentium B960 @ 2.20GHz, L1d 32 KiB/core,
L2 256 KiB/core, L3 2 MiB · **Tools:** valgrind-3.25.1 (callgrind, `--cache-sim=yes`),
perf 7.1.4 · **Build:** `build-prof`, `-O3 -fno-omit-frame-pointer`.

perf gave the *time* split (zstd 67.5% gray / 59.6% RGB of decode). It could not
say *why* the remaining third costs what it costs. Callgrind answers that with
exact counters instead of sampled ones.

### Isolating the unfilter

At 1 repetition, level-12 encoding is ~99% of all instructions, so a whole-run
profile drowns the decoder. `--toggle-collect=reverse_filter` restricts
collection to the unfilter pass only:

```sh
valgrind --tool=callgrind --toggle-collect=reverse_filter --cache-sim=yes \
  ./build-prof/pxl_bench_stages <file> 3 12
```

Each run unfilters the same 2.36 MB (stages decodes every candidate mode 3 times,
so gray = 512×512×1×3 modes×3 reps, RGB = 256×256×3×4 modes×3 reps).

| corpus | Ir/byte | D refs | D1 miss | LL miss | 1 D1 miss per |
|---|---:|---:|---:|---:|---:|
| 1.1.08.png, gray 1ch | 3.62 | 1,938,591 | 3.80% | 0.64% | 32.0 B |
| 4.1.01.png, RGB 3ch | 5.54 | 4,292,832 | 1.72% | 0.29% | 32.0 B |

### The cache is already optimal

One D1 miss per 32 bytes in *both* runs, on a 64-byte line: exactly two lines
touched per 64 bytes of output (streaming the zstd output in, the pixels out).
The previous row is never a miss, it was written moments earlier and is still
resident. These are compulsory misses, the unavoidable cost of reading input
once and writing output once.

LL misses are `0 rd + 12,289 wr` — every last-level miss is a write-back of
finished pixels. Nothing the decoder reads ever falls out of L2/L3.

So there is no locality left to win: no blocking, no prefetch, and no row-buffer
rearrangement can beat two compulsory lines per line of output. The remaining
third of decode time is ALU and dependency-chain latency, not memory.

### Why SIMD cannot fix the rest

The suggestion to reach for SIMD was checked and rejected on structure, not on
effort. PAETH, SUB and AVG all read the pixel `bpp` bytes to the left *after*
it has been reconstructed. That is a serial dependency along the row: lane *n*
of a vector would need the output of lane *n-1*. For 1-channel images the chain
is one byte long, the worst possible case, and gray is where the corpus spends
most of its bytes. libpng's SIMD filter code works around this only by
processing `bpp` bytes at a time (3 or 4 of 16 lanes used) — it vectorizes the
*channels*, never the row. That is a fraction of a lane-width, and it does not
apply to 1ch at all.

`pxl_paeth` is already inline and branchless (arithmetic-shift masks, no jumps),
so the per-byte work is a handful of dependent integer ops. At 3.62 Ir/byte for
gray there is not much fat left to trim.

### Compiler flags

`-O3` was already set in `CMakeLists.txt:17` and confirmed in `flags.make`.
`-Ofast` was rejected: over `-O3` it adds only `-ffast-math` and friends, and
the codec contains no floating point at all, so it can only add risk.

`-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` (LTO) was tried for cross-TU inlining
into zstd, and **rejected: it makes decode slower.** Best-of-5 alternating runs
of the same binary pair, `adaptive` mode, level 12, 5 reps each:

| file | build | zstd ms | unfilt ms | total ms |
|---|---|---:|---:|---:|
| 1.1.08 gray | `-O3` | 0.887 | 0.108 | 0.995 |
| 1.1.08 gray | `-O3` + LTO | 1.016 | 0.361 | 1.377 (**+38.4%**) |
| 4.1.01 RGB | `-O3` | 0.923 | 0.352 | 1.275 |
| 4.1.01 RGB | `-O3` + LTO | 0.963 | 0.497 | 1.460 (**+14.5%**) |

The unfilter is what regresses, 3.3× on gray. That is the signature of LTO
undoing the specialization: the per-filter unfilter variants are near-identical
bodies, and the global optimizer is free to merge them back into one generic
loop with the filter type live in a register, which is exactly the dispatch this
decoder was written to avoid. Single-run numbers were too noisy to see this
(the same gray file read 10.3% and 25.7% unfilter on consecutive runs), which is
why the comparison is best-of-5 and interleaved.

LTO also did not finish linking within 10 minutes at `-j$(nproc)` on the B960.
`build-lto/` was removed; the default build stays `-O3` without LTO.

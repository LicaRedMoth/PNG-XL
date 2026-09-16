# Benchmark history

An **append-only** log. Old entries are never edited or deleted, even if the
numbers are out of date or turned out to be wrong — instead we add a new entry
below and explain what changed. The point of the log is to show the trend, not
to keep one "current" number.

Entry format: date, commit, hardware, what was measured, with what, result.
If a measurement is not reproducible (no tool, different hardware) — say so.

**Measure on an idle machine.** A parallel build in the background inflates
every timing by 2-3x, uniformly enough that the numbers still look plausible
and the ratios still roughly hold, so there is nothing in the output to catch
the mistake later. `bench/bench.sh` refuses to write to README.md when the
1-minute load average is above 1.5 (`BENCH_LOAD_LIMIT` to change the limit,
`BENCH_ALLOW_LOAD=1` to override); it only warns when just printing.

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

---

## 2026-09-15 — regenerating the README tables on an idle machine

- **Commit:** `7b9b517`, plus the uncommitted working tree (the load guard in
  `bench/bench.sh`, the generated raw-decode section, `bench/encstages.c`)
- **Hardware:** Intel Pentium B960 @ 2.20GHz, 2 cores, 15.2 GiB RAM
- **Compiler:** gcc 16.2.1, Release build
- **Load at start:** 1.30 for `bench/bench.sh`, 1.37 for `bench/corpus.sh`,
  both under the 1.5 limit. `BENCH_ALLOW_LOAD` was **not** used.

Both README tables were regenerated with `bench/bench.sh --update-readme` and
`bench/corpus.sh --update-readme`, closing the roadmap's "make the README
benchmarks honest again" item.

### Third-party tool versions

Recorded here for the first time. Two of the changes below could not have been
attributed without guessing, because no previous entry says which versions it
measured against — that is the gap this table exists to close.

| tool | version |
|---|---|
| cjxl / djxl | 0.12.0 |
| avifenc / avifdec | 1.4.2 (aom v3.14.1, dav1d 1.5.4) |
| cwebp | 1.6.0 |
| ImageMagick | 7.1.2-29 Q16-HDRI |
| ffmpeg | n9.0.1 |
| oxipng | 10.2.0 |

### What did not move: our own sizes

Every PXL/APXL size came out byte-identical to the committed tables — 5994 on
the chart, 47209 on the beach ball, 13 712 646 over the 186-file corpus, and
13 633 519 for the raw-decode row. AVIF, QOI, GIF and oxipng are identical too.
So the roadmap's premise was half wrong: the tables were stale in their
*timings*, not in the sizes, and nothing in the filter work changed a byte of
output on these corpora.

### What did move, and why

**JXL got substantially smaller**, at the same default effort 7 (`cjxl --help`
confirms 7 is still the default; `bench/bench.sh` passes only `-d 0
--num_threads=0`, so the "effort 7/10" label in the table is accurate):

| measurement | before | after |
|---|---:|---:|
| chart | 5963 (19.5%) | 3538 (11.6%) |
| beach ball | 52509 (84.7%) | 51766 (83.5%) |
| corpus | 10 339 194 (66.7%) | 10 191 764 (65.7%) |

On the chart JXL went from level with PXL (19.5% vs 19.6%) to roughly half its
size. Size is deterministic for a fixed encoder and setting, so this is a
libjxl version change, not a measurement artifact. Which version it improved
over is unrecoverable — see the table above.

**WebP changed encoder path, not version.** The corpus row's own label records
it: `lossless (-lossless 1)` before, `lossless (-lossless)` after.
`bench/corpus.sh:133` prefers `cwebp` and only falls back to ffmpeg's libwebp,
so cwebp is now installed where it was not before. The 11 385 268 -> 11 390 236
bytes (+4968, +0.04%) is two implementations disagreeing, not a regression.

### The timing columns are not comparable across runs

Corpus encode time roughly halved on every row, PXL included:

| row | before (ms/file) | after (ms/file) |
|---|---:|---:|
| PXL | 140.3 | 85.6 |
| JXL | 526.7 | 320.8 |
| WebP | 313.6 | 102.2 |
| AVIF | 309.4 | 144.7 |
| oxipng | 1041.2 | 528.8 |

PXL's bytes are identical and its code has not changed since the previous run,
so its 1.64x is not a speedup — something outside the codec moved. The
single-image table does not tell the same story, though: there PXL barely
budged (encode 16 -> 15 ms) and decode got *worse* (16 -> 20 ms, APXL 36 -> 46),
while the third-party tools dropped 1.3x to 4.4x (GIF animation 3759 -> 847 ms).
`magick` and `avifenc` thread by default and contend badly on two cores;
`cjxl` was pinned single-threaded and still halved.

**Do not read a cause into this.** The previous run's load average was never
recorded, so "the old numbers were taken on a busy machine" is the likely
reading but not a measured one. It is exactly the ambiguity the load guard and
the version table above are meant to prevent from recurring, and the honest
conclusion today is narrower: timings from before 2026-09-15 are not
comparable with timings after it, and only the size columns carry across.

### A note for the next person benchmarking on this box

Idle here is not idle. With the KDE session, Claude Desktop, immich and a wine
service running, `vmstat 2` shows a steady 35% of both cores in use (us 21-24%,
sy 13-15%, ~9000 context switches/s) with no single process above 4%. That is a
load-average floor near 1.0, so the 1.5 limit is tight rather than generous:
expect to wait 70-90 seconds after any build before the guard will let a
publishing run through, and expect even a no-op `cmake --build` to push it back
over the line.

---

## 2026-09-15 — decoder size, re-measured after the TU split

- **Commit:** `dcfbec4`
- **Hardware / compiler:** Intel Pentium B960, gcc 16.2.1, Release
- **Method:** measurement 2 repeated exactly — minimal decoding programs
  (file into memory → pixels, no CLI, no metadata), project libraries linked
  statically, `strip`, compare `.text`. Sizes, so machine load is irrelevant.

Measurement 2 (2026-07-28) recorded the decoder at **861 170** bytes against
libpng's 209 337 and called the decoder-size priority "not met, and that has to
be recorded honestly". Two things landed since and were never re-measured: the
zstd build options (`ZSTD_LEGACY_SUPPORT` / `ZSTD_MULTITHREAD` off,
`CMakeLists.txt:51`) and the codec TU split in `2d8d9ec`, which was named as
the next step and then done seven weeks ago.

| binary | `.text`, bytes | vs 2026-07-28 |
|---|---:|---|
| empty program (baseline) | 265 | 281 |
| `ZSTD_decompress` only | 147 570 | 284 978 |
| **PXL decoder** | **174 066** | 861 170 (**-79.8%**) |
| libpng decoder (zlib dynamic, not counted) | 209 913 | 209 337 |

**The priority is met.** The PXL decoder is now 17% smaller than libpng's, and
the old figure was never a property of the format — it was the compressor being
dragged in by the linker. Verified rather than assumed: the stripped binary
decodes the test chart correctly (258x200), and `nm` shows `ZSTD_decompress`
and `pxl_decode` present with `ZSTD_compress`, `ZSTD_compressBound` and
`pxl_encode_ex` all absent.

Two corrections to figures that are now wrong elsewhere in this log:

- The "285 KB floor for any zstd-based variant" was measured against a libzstd
  built with legacy support and multithreading. With both off the floor is
  **147 570**, which is below libpng on its own.
- Our own decoder code is **26 496** bytes (174 066 - 147 570), up from the
  19 KB recorded earlier. The unfilter loop specialization in `2d8d9ec` bought
  its ~2.5x with about 7 KB of text, which is a trade worth knowing about.

The comparison is conservative against us. PXL links all of libzstd statically
and every byte is counted; libpng's 209 913 excludes zlib, which it loads
dynamically and which carries another 61 528 bytes of `.text`. Counted the same
way, libpng + zlib is 271 441 against our 174 066 — 36% smaller, not 17%.

**Caveat on the target hardware.** This says the decoder fits, not that it runs.
Nothing here measures RAM at decode time, and the PSP-class target is a 32 MB
budget; zstd's window and the full-image pixel buffer are the figures that
matter there, and neither has been measured yet.

---

## 2026-09-15 — decode-time memory against the 32 MB target

- **Commit:** `9321ffe`
- **Hardware / compiler:** Intel Pentium B960, gcc 16.2.1, Release
- **Method:** peak RSS of minimal decoding programs via `getrusage(RUSAGE_CHILDREN)`
  (`ru_maxrss`), max of 3 runs, minus an empty program's 1528 KiB baseline.
  GNU `time` is not installed on this machine; a 20-line fork/wait wrapper was
  used instead. Memory, not timing, so machine load is irrelevant.

The previous entry established that the decoder *fits* (174 KB of `.text`) but
explicitly did not say it *runs* in the 32 MB budget. This measures that.

### Still images, net MiB after baseline

| image | pixels | `pxl_decode` | `pxl_stream` | libpng |
|---|---:|---:|---:|---:|
| 768x512 RGB | 1.1 | 3.4 | 4.0 | 2.2 |
| 2250x2250 RGB | 14.5 | 40.9 | 30.8 | 15.6 |
| 3000x3000 RGB | 25.7 | **62.9** | **53.3** | **26.9** |

At 3000x3000 that is **2.44x the pixel buffer one-shot, 2.07x streaming,
against libpng's 1.04x**. libpng is essentially the output image and nothing
else; we are the output image twice over.

### The cause is structural, and it is in the code

`pxl_decode` (`src/pxl_codec_decode.c:439-464`) allocates `filtered` at
`raw_byte_count`, decompresses the whole frame into it, allocates `pixels` at
full size, unfilters across, and only then frees `filtered`. Both full-size
buffers are live simultaneously, on top of the caller's compressed input.

The streaming decoder does not fix this. `pxl_stream_new`
(`src/pxl_codec_decode.c:655-656`) allocates the same two full-size buffers;
`ZSTD_decompressStream` only spares it the compressed input, which is the
10.7 MiB of difference between the two columns above. Progressive decode buys
early rows, not a smaller footprint — the API says as much ("rows still
accumulate in the buffer exposed by `pxl_stream_image`"), but the consequence
was never measured. On the 768x512 file streaming is actually *worse* (4.0 vs
3.4 MiB), because the `ZSTD_DStream` window outweighs the input it saves.

### Animation is worse: 2x the entire animation

`apxl_decode` (`src/apxl_codec.c:212-245`) decompresses every frame into one
concatenated `raw` buffer, then mallocs a separate full canvas per frame and
memcpys into it, freeing `raw` only after the loop. Peak is therefore twice the
whole animation, regardless of frame count.

Measured on 8 frames of 1920x1080 RGBA from the Anita `pirate/sketch/204_a`
shot (63.3 MiB of pixels in total):

| encode | peak, net MiB | x whole animation |
|---|---:|---:|
| level 12 (LDM on) | 128.3 | 2.03x |
| level 9 (LDM off) | 131.8 | 2.08x |

### A correction: the 128 MiB LDM window is not allocated

It was suspected that `APXL_WINDOW_LOG 27` (`src/apxl_codec.c:22`) would force
a 128 MiB window on the decoder and put `.apxl` out of reach of the target
outright. **That is wrong.** `apxl_decode` uses one-shot `ZSTD_decompressDCtx`,
where the destination buffer serves as the window, so no window is allocated.
The two rows above differ by 3.5 MiB in the *opposite* direction to the
hypothesis. The `.apxl` memory problem is real but it is the double buffering,
not the window.

### What this means for the 32 MB target

Taking 32 MB as 30.5 MiB usable:

- The largest still the streaming path can decode is about **14.7 MiB of
  pixels, roughly 2270x2270 RGB**. The 3000x3000 photograph this project
  benchmarks with does not fit, at 53.3 MiB. libpng decodes it in 26.9 MiB and
  does fit.
- Animation holds about **30 frames at the PSP's native 480x272 RGBA**. One
  8-frame 1080p shot needs four times the entire budget.

So the honest status of the target hardware is: the decoder fits in flash, and
does not fit in RAM for anything above ~5 megapixels. Decoder size was the
priority that was tracked; memory is the one that actually binds, and it was
never measured until now.

**The fix is not a format change.** Unfiltering needs only the previous row, so
`filtered` does not have to be the whole image — a bounded ring buffer of a few
rows, filled from `ZSTD_decompressStream`, would take the streaming path from
2.07x to about 1.0x plus a constant, which is libpng's number. The same applies
per frame in `apxl_decode`. This is decoder-side only, byte-identical output,
no compatibility risk — the same class of change as the TU split that fixed
decoder size. It is not implemented and not measured; the numbers above are the
case for doing it.

---

## 2026-09-15 — decode memory after the one-row window

- **Commit:** `db07781` (measured against `d3151dd` as the before)
- **Method:** as the entry above — peak RSS via `getrusage`, max of 3, minus a
  1528 KiB empty-program baseline. Confirmed against `valgrind massif`, which
  is what identified the two 27 MB allocations in the first place.

Peak RSS, net MiB, on the same ladder as before:

| image | pixels | `pxl_decode` before → after | `pxl_stream` before → after | libpng |
|---|---:|---|---|---:|
| 768x512 adaptive | 1.1 | 3.7 → 3.3 | 4.0 → 3.0 | 2.2 |
| 2250x2250 adaptive | 14.5 | 41.5 → **27.6** | 30.8 → **16.2** | 15.6 |
| 3000x3000 adaptive | 25.7 | 63.6 → **38.6** | 53.2 → **27.6** | 26.9 |
| 3000x3000 BCIF | 25.7 | 62.8 → 62.8 | 53.4 → 53.3 | 26.9 |

**The streaming path now matches libpng** — 27.6 MiB against 26.9 on a
3000x3000 photograph, where it needed 53.2 before. At 2250x2250 it is 16.2 MiB
against 14.5 MiB of pixels, so 1.12x: the output buffer, one row, and zstd's
window. The 32 MB target now holds a 9-megapixel image; before it did not.

`pxl_decode` lands at 38.6 rather than 27.6 because its API takes the whole
compressed file as a buffer and the caller keeps it live — that is the 11 MiB
between the columns, and it is the caller's, not the decoder's.

### The caveat that matters

BCIF is unchanged by design: the plane split completes no row until the last
plane byte, so there is no row window to unfilter through. **The encoder picks
BCIF for photographs**, which is exactly the content the target hardware would
be opening, so on a default-encoded photo none of this helps. Encoding with
`-p` (progressive, BCIF excluded) is what buys the memory, and on this ladder
it costs 4.4% to 6.6% in file size:

| image | default | `-p` | cost |
|---|---:|---:|---:|
| 768x512 | 600 474 | 807 560 | +34.5% |
| 2250x2250 | 11 856 214 | 12 325 479 | +4.0% |
| 3000x3000 | 11 272 276 | 12 013 885 | +6.6% |

So the format now has a real memory/size dial rather than a wall, but the dial
has to be turned deliberately. This strengthens the standing argument against
BCIF: on 32 MB hardware the row-wise path is the only one that runs at all,
and BCIF is the one filter that cannot use it.

### Correctness

Byte-identical output over **372 decodes** — the 186-file committed corpus and
the same corpus re-encoded with `-p`, each through both the one-shot and the
streaming path, diffed against binaries built from the previous commit. Filter
coverage across those runs: 87 none, 56 adaptive, 26 BCIF, 17 delta. `ctest`
green.

### Found on the way: a heap overflow

Reading the two paths side by side to share their row logic turned up that
`stream_emit_rows` passed `h.width` to `unpack_delta_row` where the row is
`filter_width` bytes — smaller than the width for sub-8-bit images. A .pxl
pairing a sub-8-bit indexed image with DELTA therefore wrote past every row and
past the buffer on the last one, in the progressive path a viewer uses on
downloading data. The palette index check hides it unless the palette is fully
populated. Confirmed under ASAN and fixed separately in `cab6ccf`, ahead of
this work, so it can be taken on its own.

---

## 2026-09-15 — animation decode memory: frames point into the stream

- **Commit:** `db07781` as the before; method as the two entries above.

`apxl_decode` decompressed every frame into one concatenated block, then
allocated a separate canvas per frame and copied into it, freeing the block
only after the loop — so peak was twice the whole animation. But the block
*already is* the frame sequence, in order, so the copy bought nothing. Frames
now point into it, and `apxl_anim` carries the block as `storage`.

8 frames of 1920x1080 RGBA from the Anita `pirate/sketch/204_a` shot, 63.3 MiB
of pixels in total:

| encode | before | after | x animation |
|---|---:|---:|---:|
| level 12 (LDM on) | 128.3 | **65.2** | 1.03x |
| level 9 (LDM off) | 131.7 | **68.6** | 1.08x |

That is the floor for this API: `apxl_decode` returns every frame, so it cannot
hold less than every frame. Output verified identical, `ctest` green, ASAN
clean on both ownership shapes — decoded animations free the shared block,
hand-built ones (`apng_load`, the encoder's callers) still own their frames
individually and are unaffected.

**Why this stayed one-shot.** The obvious symmetry with the still path would be
to stream frame by frame, but that would be a large regression here: one-shot
`ZSTD_decompressDCtx` lets the destination serve as the window, while
`ZSTD_decompressStream` must allocate its own — and `APXL_WINDOW_LOG` is 27, so
LDM files would have added a 128 MiB window to buy back 63 MiB of frames. The
deeper reason is structural: cross-frame matching means the decoder must keep
previous frames reachable, and in this design the output buffer *is* that
window. Bounded memory and cross-frame matching pull against each other; the
copy was the accidental cost, the retained frames are the real one.

---

## 2026-09-15 — MOVE headroom over what zstd already finds

- **Commit:** `9cdefb0` + `bench/motion.c`, `bench/motion.sh`
- **Question:** APXL's single zstd stream already expresses COPY — an LZ match
  *is* "copy these bytes from further back", which is why finished frames
  compress well across a shot. What it cannot express cheaply is a *displaced*
  copy, because a moved block is not contiguous in raster order and costs one
  match per row instead of one vector per block. Is there anything there?
- **Method:** each frame cut into 16x16 blocks, each classified by exact match
  against the previous frame — COPY at the same position, FLAT (single colour,
  excluded: it matches anywhere and no vector would be spent on it), MOVE at
  some displacement, RESIDUAL nowhere. Search is over the whole previous frame,
  not a +-N window, so this is an upper bound rather than one search strategy's
  yield.
- **Sampling:** 24 shots per pass drawn evenly across the pass's whole sorted
  shot list (composition has only 18 in total, 17 usable), first 4 consecutive
  pairs per shot, every shot weighted once.

### The instrument was validated before its result was believed

| control | copy | flat | move | residual | dominant vector |
|---|---:|---:|---:|---:|---:|
| pure 12 px scroll of a real screenshot | 7.47% | 4.37% | **88.17%** | 0.00% | **99.1%** |
| two unrelated screenshots | 18.08% | 11.05% | 1.86% | 69.01% | 3.1% |

On pure translation the tool finds the motion and the vectors agree with each
other. On unrelated frames it invents almost none. So a low reading below is
about the content, not about the measurement.

### Anita

| pass | shots | copy | flat | move | residual | dominant vector |
|---|---:|---:|---:|---:|---:|---:|
| sketch | 24 | 88.87% | 2.66% | **0.73%** | 7.74% | 18.6% |
| composition | 17 | 50.95% | 4.14% | **0.86%** | 44.04% | 21.4% |
| color | 24 | 82.98% | 6.96% | **0.29%** | 9.77% | 17.5% |

**Under 1% of blocks on every pass**, against 88% on the positive control. And
the vectors do not agree — 17-21% dominant, where the control is 99% — so even
that fraction would spend most of its gain on coding the vectors.

The reason is in the content, not the codec: hand-drawn frames are *redrawn*,
not translated. A line is not moved three pixels, it is drawn again slightly
differently, and nothing matches exactly any more.

**The number that is actually interesting here is RESIDUAL on composition:
44%.** Nearly half the blocks of a finished frame match nowhere in the previous
one. That, not motion, is where the bytes of an animation are.

### Why the video captures cannot answer this question

Two local folders of personal H.264 captures were checked as possible MOVE
material and are unusable for it, which is worth recording so nobody retries:

| source | copy | move | residual |
|---|---:|---:|---:|
| synthetic scroll of a screenshot (PNG) | 7.47% | **88.17%** | 0.00% |
| screen recording of UI/gameplay (H.264) | 12.95% | **0.05%** | 86.89% |

Same kind of content, opposite answer. H.264 already performed motion
compensation and quantised the residual, so a block that genuinely moved decodes
to nearly-but-not-exactly the same pixels and no exact match survives. Night
footage reads move=7.30% but across 2827 distinct vectors with a 0.8% dominant
share — chance matches in dark regions, not motion.

Answering the MOVE question for screen content needs a **lossless** capture
(FFV1 or lossless x264, or a PNG frame sequence). The synthetic scroll above
already gives its ceiling: 88%.

**Decision:** for hand-drawn animation, MOVE is rejected on measurement — the
headroom is under 1% and incoherent. For translating content it would be worth
a great deal, but that is a different corpus and arguably a different product;
the existing rejection bar for block-based work (>5% gain while keeping
streaming decode, see RESEARCH.md on tiling) is not met by anything measured
here.

---

## 2026-09-15 — the synthetic-stills gap, measured at last

- **Commit:** `c849e62` + the `CORPUS_ONLY` / `--with-shots` additions
- **Corpus:** 442 real desktop screenshots in `tests/data/Screenshots` — a
  personal, gitignored folder, so **aggregate totals only** and no README row:
  `bench/corpus.sh` refuses to splice it. 441 encoded; the one failure is a
  0-byte file, not a codec problem. Character of the set: 92 under 0.1 Mpx,
  228 between 0.1 and 1 Mpx, 121 over 1 Mpx; 343 RGB, 97 RGBA, 1 gray.
- **Reproduce:** `CORPUS_ONLY=tests/data/Screenshots bench/corpus.sh`

Both `RESEARCH.md` and `ROADMAP.md` have named the same gap repeatedly: every
corpus available was photographic or animation, and nothing covered synthetic
non-photographic stills — the content a PNG replacement is actually pointed at,
and where the format's own philosophy claims its strength. That claim had never
been tested. It is now.

| Format | Files | Total bytes | % of PNG |
|---|---:|---:|---:|
| PXL | 441 | 49 542 325 | **73.2%** |
| JXL | 441 | 44 566 566 | 65.8% |
| WebP | 441 | 41 114 534 | **60.7%** |
| AVIF | 441 | 74 256 020 | 109.6% |
| PNG (oxipng -o max) | 441 | 53 469 076 | 79.0% |

### Against the photographic corpus, which is the point

| Format | 186 committed files | 442 screenshots | change |
|---|---:|---:|---:|
| PXL | 88.4% | **73.2%** | **-15.2 pts** |
| JXL | 65.7% | 65.8% | +0.1 |
| WebP | 73.4% | 60.7% | -12.7 |
| AVIF | 89.4% | 109.6% | +20.2 |
| oxipng -o max | 94.9% | 79.0% | -15.9 |

**The philosophy holds.** PXL gains 15 points moving from photographs to
synthetic content, while JXL does not move at all — so the narrowing is ours,
not a property of the corpus being easier. The gap to JXL closes from 22.7
points to **7.4**.

Two results worth stating plainly:

- **PXL beats `oxipng -o max` by 5.8 points** on the content PNG is most used
  for, against a 6.5-point lead on photographs. For a format whose pitch is
  "replace PNG", beating a maximum-effort PNG optimiser on its home ground is
  the comparison that matters most, and it is the first time it has been made
  on this content.
- **AVIF comes out larger than the source PNGs** (109.6%). It is a photographic
  codec and has nothing to offer here — worth remembering before anyone cites
  its 89.4% on Kodak as a general figure.

WebP still wins outright at 60.7%, as it does on photographs. Nothing here
changes that; what changes is the distance.

**Do not cite the encode-time column from this run.** It was taken at a
1-minute load average of 2.68, above the 1.5 limit `bench/bench.sh` enforces,
because the sizes were the question and sizes do not care about load. The
timings in that run are inflated and were not recorded here.

---

## 2026-09-15 — a committed synthetic corpus, and why it needs two numbers

- **Corpus:** 838 freely-licensed UI screenshots fetched by
  `bench/synthetic_png.sh` from Wikimedia Commons (MediaWiki, Firefox,
  Wikipedia, Inkscape, browsers, Emacs). 260 MB, gitignored like USC-SIPI —
  the script is committed, the data is not. Licences: 662 CC BY-SA 4.0,
  67 CC BY-SA 3.0, 44 CC0, 26 public domain, 22 GPL, the rest CC BY / MIT /
  LGPL, recorded per file with author and sha256 in `MANIFEST.tsv`.
- **Reproduce:** `bench/synthetic_png.sh` then
  `CORPUS_ONLY=tests/data/Synthetic-Screenshots CORPUS_ROWS="PXL oxipng" OXIPNG_LEVEL=2 bench/corpus.sh`
- 824 of 838 files encoded; the rest are 0-byte or malformed uploads.
- The corpus is genuinely synthetic, not photographs in PNG clothing: median
  **1046 unique colours per megapixel**, where photographs run past 100 000.

| Format | Files | Total bytes | % of source PNG |
|---|---:|---:|---:|
| PXL, level 12 | 824 | 151 970 262 | **57.9%** |
| PNG (oxipng -o 2) | 824 | 178 617 956 | **68.1%** |

### Both numbers, because either alone misleads

**57.9% is real but flattering.** Commons uploads come from hundreds of unknown
tools, so "the source PNG" is not a defined baseline here: `oxipng -o 2` alone
takes the same files to 68.1%, meaning roughly a quarter of the apparent win is
slack in other people's export settings rather than compression. A 60-file
sample said the same before the full run (75.3% for oxipng, 86.2% for PXL
against it), so this is not a sampling artefact.

**Against a competently encoded PNG, PXL is 85.0%** (57.9 / 68.1). That is the
figure to quote when the question is "how good is the compression", and 57.9%
is the figure to quote when the question is "what happens if I convert the PNGs
I actually find in the wild". Neither is wrong; quoting one without the other
is.

Re-encoding the corpus to normalise the baseline was tried and rejected:
ImageMagick drops a fully opaque alpha channel, turning RGBA files into RGB, and
a screenshot corpus that has lost its alpha channels is no longer the content
being modelled. Pixels were verified unchanged (AE=0) before that was ruled out
on other grounds.

**Not comparable to the personal-screenshot run above**, which used
`oxipng -o max`; this one uses `-o 2` because `-o max` costs seconds per file
and would have taken over an hour for one row. `bench/corpus.sh` now takes
`OXIPNG_LEVEL` and puts the level in the row label so the two cannot be mixed
up.

---

## 2026-09-15 — zstd dictionary, first measurement

- **Tool:** `bench/dumpfiltered` (new) writes the filtered stream the encoder
  feeds to zstd, so training material matches what is actually compressed.
- **Method:** `zstd --train` on one half of a corpus, measured on the disjoint
  other half, `zstd -12`, sizes summed. A dictionary measured on its own
  training data would measure nothing.

PNG test suite, 79 files train / 81 held out:

| dictionary | compressed | vs none |
|---|---:|---:|
| none | 21 987 | 100.0% |
| 4 KB | 21 885 | -0.5% |
| 16 KB | 20 481 | -6.8% |
| 9.6 KB effective | 19 710 | **-10.4%** |

Cross-class and large-image behaviour, same method:

| test set | no dict | PngSuite-trained | screenshot-trained |
|---|---:|---:|---:|
| PngSuite (held out) | 100% | **89.6%** | 97.5% |
| small screenshots (held out) | 100% | 97.9% | 104.6% |
| 6 large screenshots | 100% | 109.5% | - |

**The large-image regression is dictionary mode, not dictionary content.** On
one 7.5 MB filtered stream: plain 219 287 bytes, trained dictionary 226 207
(103.16%), and 9.6 KB of `/dev/urandom` 225 895 (103.01%). Random and trained
cost the same, so attaching any dictionary is what does it.

Conclusion recorded in RESEARCH.md: worth having as a per-file encoder choice,
never as a format-wide default.

---

## 2026-09-15 — zstd dictionary on real icons: the promise does not survive

- **Corpus:** 1706 icons from two locally installed themes, deduplicated by
  content, restricted to 32-64 px — Adwaita (colourful) and HighContrast
  (monochrome). Not committed; reproduce from `/usr/share/icons`.
- **Method:** as the previous dictionary entry — train on half, measure on the
  disjoint half and on the other theme.

| held-out set | no dict | Adwaita-trained | HighContrast-trained | both |
|---|---:|---:|---:|---:|
| Adwaita (373) | 649 312 | **99.3%** | 102.6% | 101.1% |
| HighContrast (480) | 175 364 | 104.0% | **103.3%** | 104.5% |

Against -10.4% on the PNG test suite, real icons give 0.7% at best, and
HighContrast is made worse by a dictionary trained on itself. PngSuite is
generated content whose files share byte patterns by construction; hand-drawn
icons do not.

**A biased first run read 96.6% instead of 99.3%.** `bench/dumpfiltered` matched
filter names case-sensitively while `pxltool info` prints BCIF capitalised, so
every BCIF file — which means every colourful one — was dropped and the sample
skewed toward flat icons. Fixed; recorded because the bug produced a plausible,
flattering number rather than an error.

---

## 2026-09-15 — zstd parameters beyond the level, and the level curve itself

- **Method:** over filtered streams dumped by `bench/dumpfiltered`, i.e. exactly
  what the encoder feeds zstd. Three classes kept separate because they behave
  differently enough that an average would describe none of them: 120 icons
  (0.8 MB), 10 small screenshots (2.3 MB), 6 large screenshots (92.8 MB).

### Tuning the parameters does not beat the presets

Small screenshots, all at level 12 unless stated, against level 12 default:

| variant | bytes | vs L12 | encode time |
|---|---:|---:|---:|
| L12 default | 96 002 | 100.0% | 1.0x |
| L12 `strat=btultra2` | 94 124 | 98.0% | 1.1x |
| L12 `mml=3` | 95 108 | 99.1% | ~1x |
| L12 `mml=7` | 98 069 | 102.2% | ~1x |
| L12 `strat=btultra2,tlen=256` | 88 984 | 92.7% | 2.4x |
| L12 `clog=24` | 96 002 | 100.0% | ~1x |
| **L19 default** | **86 576** | **90.2%** | 2.7x |
| L22 `--ultra` | 85 989 | 89.6% | 5.0x |

The best hand-tuned level-12 variant reaches 92.7% at 2.4x time; plain level 19
reaches 90.2% at 2.7x. **The preset dominates the tuning**, and `mml=3`, which
looked like a free win on this class, turns out to be byte-identical on icons
and 99.9% on large images — noise. zstd's levels are well chosen for this data
and there is nothing left on that table.

### The level curve, which is the real lever

| class | L1 | L6 | L12 | L19 | encode L12 → L19 |
|---|---:|---:|---:|---:|---|
| icons (0.8 MB) | 107.1% | 103.1% | 100% | 98.6% | 0.81s → 1.46s |
| small screenshots | 116.8% | 103.0% | 100% | **90.2%** | 0.19s → 0.73s |
| large screenshots | 143.9% | 111.1% | 100% | **87.3%** | 1.11s → 11.97s |

### And the level costs nothing at decode

Large screenshots, 92.8 MB of raw stream, three passes:

| level | compressed | decode | raw MB/s |
|---|---:|---:|---:|
| 1 | 1 640 204 | 0.12s | 784 |
| 6 | 1 266 258 | 0.11s | 857 |
| 12 | 1 139 872 | 0.11s | 831 |
| 19 | 994 638 | 0.12s | 805 |

**Decode speed is flat across levels** — the spread is noise — so the level is a
pure encode-time-for-size trade and does not touch the property the format is
actually sold on.

Which makes the still-image default hard to defend: `PXL_LEVEL_DEFAULT` is **1**,
and on large content that is 143.9% of level 12 — 44% larger — in exchange for
encode time that a one-off conversion pays once. Every published figure for this
format is measured at level 12.

---

## 2026-09-16 — correction: the level's decode cost depends on the content

The entry above concluded that "decode speed is flat across levels" and that the
level is therefore a pure encode-time trade. **That holds for synthetic content
and is wrong for photographs.** The earlier figure was taken with the `zstd` CLI
over screenshot streams, where process startup and pipe I/O are a large part of
each measurement — the same contamination this project already caught once in
README's decode column.

Re-measured in process with `bench/stages.c`, which separates zstd from
unfiltering:

| content | filter | L1 zstd | L12 zstd | L19 zstd | L1→L19 bytes |
|---|---|---:|---:|---:|---|
| screenshot 1894x989 | none | 5.78 ms | 5.68 ms | 6.14 ms | 285 820 → 193 036 |
| Kodak photograph | none | 3.57 ms | 6.93 ms | **10.42 ms** | 991 061 → 653 244 |

On the photograph, decompression is **three times slower at level 19 than at
level 1** while producing a third less data. On the screenshot it is flat. The
unfilter stage does not move in either case, so this is zstd, not our code.

The mechanism fits the content: photographic residue after filtering is close to
noise, so the stronger match finders earn their bytes from rare long-distance
matches, and decoding those means copies that reach far back and miss cache.
Synthetic content repeats genuinely and cheaply at any level.

**What this changes.** Raising the level on photographs costs encode time *and*
decode speed, which is the property the format is sold on. It strengthens the
case for the low default rather than weakening it — and it means "the level is
free at read time" must never be stated without naming the content class.

---

## 2026-09-16 — correction: the cross-format speed figures were overstated

The charts published earlier today compared throughput between decoders
producing different numbers of channels. `bench/formatdec.c` derived the output
volume as `width x height x 4`, which is right for libpng, JPEG XL, WebP and
AVIF — each is explicitly asked for RGBA — and wrong for PXL, whose native path
returns the image's own channel count. On 3-channel photographs that overstated
PXL by 4/3; on 1-bit grayscale, by 32x, which is how it was caught: a WASM
decode reported 34 900 MB/s.

The `PXL-RGBA` row did not fix it either, because `pxl_image_expand` widens
palettes and sub-byte depths but leaves an 8-bit RGB image alone.

Both fixed: decoders now report the bytes they actually produced, and PXL pays
for a genuine widening to RGBA exactly as libpng pays for the alpha it is told
to add. On one Kodak image the old method would report 217 MB/s where the
honest figure is 159.

Re-measured, every decoder producing 8-bit RGBA:

| photographs (24) | % of PNG | MB/s |
|---|---:|---:|
| PXL level 1 | 87.8 | 275.8 |
| PXL level 19 | 85.6 | 226.7 |
| PXL level 12 | 88.6 | 199.6 |
| libpng | 100.0 | 98.6 |
| WebP | 73.6 | 92.3 |
| AVIF | 88.9 | 16.3 |
| JXL | 65.8 | 5.2 |

| synthetic stills (30) | % of PNG | MB/s |
|---|---:|---:|
| WebP | 44.6 | **570.2** |
| PXL level 1 | 80.8 | 536.7 |
| PXL level 19 | 50.3 | 485.0 |
| PXL level 12 | 58.2 | 449.8 |
| libpng | 100.0 | 308.7 |
| AVIF | 165.5 | 85.4 |
| JXL | 93.8 | 12.5 |

**What changes.** PXL is about 2x libpng on photographs rather than 3.3x, and
1.5x on synthetic stills rather than 4.8x. On synthetic content **WebP is both
smaller and faster than PXL** — the earlier claim that PXL was the fastest
codec measured holds only for photographs. AVIF's and JPEG XL's collapse on flat
synthetic content is unaffected, since their figures were never wrong.

---

## 2026-09-16 — does a cheap probe level rank the filters like the full one?

- **Tool:** `bench/encstages.c`, driven over corpora by `bench/probesweep.sh`
- **Question:** `pxl_encode_ex` compresses every filter candidate at the
  requested level to find the smallest, throwing three results away. If a cheap
  level ranked them the same way, the encoder could compare cheaply and compress
  the winner once — for byte-identical output.
- **What is reported:** agreement alone decides nothing. A strategy that agrees
  95% of the time and loses 10% on the rest is worse than one that agrees less
  and loses nothing, so the size penalty is measured against the filter the full
  search would really have chosen.

Full level 12 throughout. *top-1* compresses only the probe's winner; *top-2*
compresses the probe's best two at full level and keeps the smaller.

| probe | corpus | files | top-1 agree | top-1 penalty | top-1 speed | top-2 agree | top-2 penalty | top-2 speed |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | Kodak | 24 | 95.8% | 0.064% | 2.64x | 95.8% | 0.064% | 1.50x |
| 1 | PngSuite | 158 | 86.7% | 1.329% | 2.37x | 97.5% | 0.134% | 1.49x |
| 1 | synthetic | 40 | 92.5% | 0.762% | 1.62x | **100%** | **0.000%** | 1.24x |
| 3 | Kodak | 24 | 95.8% | 0.064% | 1.99x | **100%** | **0.000%** | 1.28x |
| 3 | PngSuite | 158 | 91.1% | 0.345% | 2.26x | 98.1% | 0.120% | 1.37x |
| 3 | synthetic | 40 | 95.0% | 0.337% | 1.51x | **100%** | **0.000%** | 1.09x |

**The premise does not hold.** A probe level does not rank the candidates the
way the full level does: at probe 1 it disagrees on 4-13% of files, and on
PngSuite that costs 1.33% of the corpus and up to 4.4% on a single file. The
"byte-identical output for free" the idea was proposed on is not available.

**What comes closest is probe 3 with top-2**, which is exact on Kodak and on
synthetic stills and loses 0.120% on PngSuite — but buys only 1.09x to 1.37x,
because compressing two candidates at full level is most of the work of
compressing four.

**The trade that is actually on offer** is probe 3 with top-1: 1.5x to 2.3x
encode speed for 0.06% to 0.35% of size, varying by content.

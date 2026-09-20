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

### `-s` / `PXL_ENCODE_FAST_DECODE`: an opt-in filter set with a decode-speed guarantee
**Why:** the PSP-3008 measurement (2026-09-17, `docs/BENCHMARKS.md`) found BCIF
loses to libpng outright at texture sizes despite winning at screen size, and
ADAPTIVE only ties libpng's decode speed rather than beating it. The default
candidate set picks whichever compresses smallest, which is a size decision —
it does not guarantee PXL beats libpng in decode speed, priority #1. `-p`
already excludes BCIF for streaming reasons but still allows ADAPTIVE.

**Measured cost of also excluding ADAPTIVE** (`bench/adaptivecost.c`), by
corpus: Kodak photographs 3.685%, the official PNG test suite 19.6% (mostly
30x30 fixtures where the 28-byte header dominates — not representative),
Synthetic-Screenshots (821 real UI screenshots) **2.933%** — smaller than
`-p`'s own already-accepted 4–6.6% cost for excluding BCIF alone.

**Decision:** accepted as an opt-in flag (`-s` in `pxltool`,
`PXL_ENCODE_FAST_DECODE` in the API), not the default — the default's whole
point is "smallest wins," and file size is a stated priority too; forcing
every user to pay ADAPTIVE's tie-not-loss cost would trade one priority for
another that was not asked for. `-s` restricts candidates to {NONE, DELTA},
the two filters measured to always beat libpng in decode speed, and implies
`-p`'s BCIF exclusion.

### Metadata is carried over byte for byte
**Why:** PXL must be a safe replacement for PNG, not lose EXIF and ICC.
**Result:** `eXIf`, `iCCP`, `cICP`, `gAMA`, `cHRM`, `sRGB`, `pHYs`, `tIME`,
`tEXt`/`zTXt`/`iTXt` and unknown ancillary chunks make the round trip
unchanged.
**Decision:** accepted. `PLTE`, `tRNS`, `sBIT`, `bKGD`, `hIST` are deliberately
not carried over — the format canonicalizes palette and transparency into real
channels, and these chunks would become invalid.

---


### A zstd dictionary — rejected, the promise was a corpus artefact
**Raised and first measured 2026-09-15.** Never previously considered: the
research log has no mention of dictionaries, and `pxl_codec_encode.c` calls
plain `ZSTD_compress(dst, cap, src, size, level)` with a level and nothing else,
so the whole zstd parameter space is unexplored.

**Why it should help.** zstd builds matches from what it has already read. A
small file has read nothing, so its first kilobytes are almost all literals. A
trained dictionary is history handed over in advance. Decode cost is nil — the
dictionary is preloaded history, not work — and the blob costs a few KB against
a 174 KB decoder.

**Method.** `bench/dumpfiltered` writes the byte stream the encoder actually
hands to zstd (filtered rows, pre-compression), using the filter the encoder
really picked for that file. Dictionaries were trained with `zstd --train` on
one half of a corpus and measured on the other half, disjoint, because a
dictionary measured on its own training data measures nothing.

**Result on the PNG test suite** (79 files train, 81 held out, `zstd -12`,
summed):

| dictionary | compressed | vs none |
|---|---:|---:|
| none | 21 987 | 100.0% |
| 4 KB | 21 885 | -0.5% |
| 16 KB | 20 481 | -6.8% |
| 9.6 KB (asked for 64-110 KB; the training set could not fill more) | 19 710 | **-10.4%** |

**But it cannot be applied unconditionally, and this is the important half.**
On large images a dictionary *costs* 3-9%. The cause is not the dictionary's
content: a 9.6 KB dictionary of `/dev/urandom` costs the same as the trained one
(103.01% against 103.16% on the same file), so **it is dictionary mode itself**,
not what is in the dictionary. Per-file variance is wide even where the average
wins — one tiny file went to 114.7% with the trained dictionary while the set
averaged -10.4%.

**Transfer between content classes is weak.** A PngSuite-trained dictionary
gives -2.1% on small screenshots; a screenshot-trained one gives -2.5% on
PngSuite and *-4.6% worse than nothing* on screenshots themselves, the training
set there being too small to build more than a 135-byte dictionary.

**Update, same day: measured on real icons, and it does not work.** The -10.4%
above is from the PNG test suite, and point 1 of the unknowns below asked
whether a conformance suite says anything about real icons. It does not.

1706 icons from two locally installed themes, deduplicated by content and cut to
the 32-64 px sizes the curve above identifies as the sweet spot — Adwaita
(colourful, LGPL/CC-BY-SA) and HighContrast (monochrome, GPL). Trained on half,
measured on the held-out half and on the other theme entirely:

| held-out set | no dict | Adwaita-trained | HighContrast-trained | both |
|---|---:|---:|---:|---:|
| Adwaita (373) | 649 312 | **99.3%** | 102.6% | 101.1% |
| HighContrast (480) | 175 364 | 104.0% | **103.3%** | 104.5% |

**Even within one theme the best case is 0.7%**, and HighContrast gets *worse*
with a dictionary trained on HighContrast. Nothing here justifies a byte of
spec.

**Why the test suite lied.** PngSuite is generated: colour ramps, gradients and
the same shapes repeated at different bit depths, so its files share byte
patterns by construction. That is exactly what dictionary training looks for, so
it found plenty — and none of it exists between real icons drawn by hand.

**A methodological note worth more than the result.** The first icon run read
96.6% within-theme, better than the truth of 99.3%, because the dumper matched
filter names case-sensitively and `pxltool info` prints BCIF capitalised — so
every BCIF file, which means every colourful one, was silently dropped and the
set skewed flat. A near-miss worth recording: the bug did not crash anything,
it just quietly made the answer more flattering.

**Decision: rejected.** A dictionary would have to be printed in SPEC.md to keep
the format implementable from the spec alone, and it buys at most 0.7% on the
content it was supposed to be for. Do not reopen without a corpus that is
neither generated nor a single visual style.

**Superseded — what the earlier conclusion said.** The
encoder already tries several filters and keeps the smallest output; trying with
and without the dictionary is the same machinery and the same cost model, and
the header then records which was used. That keeps the win on the files that
want it and the loss off every file that does not.

**Where the benefit lives, measured rather than assumed.** 97 files across
three corpora, same 9.6 KB dictionary:

| filtered stream | files | vs no dict | saved per file |
|---|---:|---:|---:|
| < 2 KB | 58 | 96.1% | 6 B |
| **2 - 8 KB** | 23 | **84.9%** | 83 B |
| 32 - 128 KB | 1 | 96.5% | 190 B |
| 128 - 512 KB | 9 | 97.9% | 206 B |
| > 2 MB | 6 | **109.4%** | **-17 951 B** |

The saving is near-constant in bytes -- it plateaus around 200 and stops growing
-- because a dictionary only helps at the start of a stream. After that the
compressor's own history is richer than any generic dictionary, since it holds
this image's patterns rather than images-in-general. A constant saving divided
by file size is why the percentage collapses: 200 bytes is 15% of a 5 KB file
and 0.003% of a 7 MB one.

So the sweet spot is a 2-8 KB filtered stream, which is 32x32 to 48x48 RGBA --
icon dimensions, arrived at by measurement rather than chosen. Below 2 KB there
is too little to match (6 bytes saved); above 2 MB dictionary mode's own cost
takes over.

Photographs are doubly unsuited, and the second reason is independent of size: a
dictionary can only hold what recurs *between* files, and filtered photographic
residue is close to noise with nothing in common from one photo to the next.
Interfaces share fonts, borders and gradients; photographs share nothing.

**On shipping a dictionary at all — the reproducibility objection.** Raised, and
it is the right objection: a decoder written from SPEC.md alone must be able to
get everything it needs from SPEC.md. That does not rule a dictionary out, it
rules out an *external* one. Brotli prints its 120 KB dictionary in RFC 7932
Appendix A as normative text, and JPEG's example Huffman tables are in Annex K
of T.81; either is the precedent. PNG's own ban on preset dictionaries (the
zlib FDICT bit must be zero) is a 1996 portability decision about wrapping an
existing zlib stream, not a principle.

So the real cost is ~10 KB of hex in SPEC.md and the same blob in the decoder,
against a format whose stated virtue is being readable in an evening. That is a
judgement call, not a measurement, and it should be made after point 1 below.

**Deriving the dictionary from the image itself and storing it in metadata --
rejected, and it cannot be made to work.** A dictionary is worth something only
because it is *not transmitted*: both sides already have it, so its contents are
free. Store it in the file and it becomes ordinary data that costs its own size,
so an N-byte dictionary can save at most about N bytes. Worse, it is redundant
by construction: LZ already refers back to anything it has read in this same
stream, so every match a self-derived dictionary could offer is one the
compressor can already make. The only case where it would not be redundant is a
compression window smaller than the file, and at level 12 the window covers the
whole file (and `.apxl` sets 128 MiB outright). Recorded because the idea looks
so reasonable that it will be proposed again.

**What is not yet known, and would decide how much this is really worth:**
1. Whether it generalises. PngSuite is a conformance suite — tiny, synthetic and
   homogeneous — and the weak cross-class transfer above says a dictionary
   trained on it may do little for real icons and sprites. A corpus of small
   real-world images is needed before believing the -10.4%.
2. What fraction of a realistic workload is small enough to benefit at all. On
   the 838-file synthetic corpus only 20 files are under 0.12 Mpx.
3. Whether dictionary mode's cost on large inputs can be avoided rather than
   dodged — it may be an artefact of how the CLI attaches dictionaries, and the
   library API may behave differently.

## Rejected ideas

### Probing filters at a cheap zstd level — rejected as a default
**Measured 2026-09-16**, closing a question the tool `bench/encstages.c` had
been sitting in the tree to answer since before 2026-09-15.

**The idea.** `pxl_encode_ex` compresses all four filter candidates at the
requested level and keeps the smallest, so a level-12 encode pays for four
level-12 compressions and discards three. If a cheap level ranked the candidates
identically, the encoder could rank cheaply and compress the winner once, and
the file would come out **byte-identical** at a fraction of the encode cost.

**Result: the ranking does not survive the cheap level.** At probe level 1 the
winner differs on 4-13% of files depending on corpus, costing 1.33% of total
size on PngSuite and up to 4.4% on individual files. Numbers per corpus and
per strategy in [BENCHMARKS.md](BENCHMARKS.md).

Compressing the probe's *two* best at full level recovers almost all of it —
exact on Kodak and on synthetic stills at probe 3, 0.120% off on PngSuite — but
only speeds encoding by 1.09x to 1.37x, since two full compressions out of four
is most of the work.

**Decision: not adopted as the default.** The honest trade is 1.5-2.3x encode
speed for 0.06-0.35% of size, and that is the wrong way round for this project:
**encode speed is not among the four priorities and file size is**, last but
present. Trading a stated priority for an unstated one needs a better rate than
this.

**Left open as a possible opt-in.** The output stays a valid `.pxl` either way,
so a `PXL_ENCODE_FAST` flag costs the format nothing and would suit a batch
conversion of thousands of files, where 2x wall time matters and 0.1% of size
does not. Not implemented; recorded so the measurement does not have to be
repeated to justify it.

**A note on what the tool was for.** `bench/encstages.c` also breaks encode time
into the filter stage and the zstd stage, and the split is worth knowing on its
own: on a Kodak photograph, filtering is 0.2-27 ms against 150-300 ms of zstd,
so encode cost is the compressor almost entirely. Speeding up the filters, which
is where the obvious optimisations live, would move nothing.


### Motion vectors (MOVE) for animation — rejected on measurement
**Why it was proposed:** video codecs get their compression from COPY, MOVE and
RESIDUAL, and APXL has no notion of a displaced copy.

**What was already true.** COPY is not missing — zstd's long-distance matching
over the concatenated frame stream *is* COPY, at byte granularity, and it is
what earns the 71.7% on `composition`. A naive temporal delta is also already
rejected: it is in the format header, because subtracting frames turns
unchanged areas into zeros but changed areas into noise that matches nothing,
and LZ prefers identical bytes to small numbers. So MOVE was the only genuinely
absent primitive.

**Result.** `bench/motion.sh` over Anita, 24 shots per pass sampled evenly,
4 pairs per shot, 16x16 blocks, exact matching, whole-frame search: MOVE reaches
**0.73% of blocks on sketch, 0.86% on composition, 0.29% on color**, with the
dominant vector accounting for only 17-21% of those. The instrument was
validated first — on a pure 12 px scroll it reports 88.17% MOVE at 99.1%
vector agreement, and on unrelated frames 1.86%. Numbers in
[BENCHMARKS.md](BENCHMARKS.md).

**Why.** Hand-drawn frames are redrawn, not translated. Motion compensation
needs content that moves rigidly; a line redrawn by hand matches nothing
exactly, however small the change looks.

**Decision:** rejected for hand-drawn animation. Do not reopen on reasoning —
reopen only with a corpus of *translating* content (camera pans, scrolling,
sprites) captured **losslessly**. H.264 captures cannot answer it: their own
quantisation destroys exact matches, and a screen recording that should be
almost pure translation reads 0.05% MOVE against 88.17% for the same content
as lossless PNG.

**What the measurement pointed at instead:** 44% of composition blocks match
nowhere in the previous frame. Residual coding, not motion, is where an
animation's bytes are — and unlike MOVE that is an open question.


### A two-thread decode pipeline (zstd on one core, unfilter on another) — rejected on measurement
**Why it was proposed:** answering "should PXL offer a multicore build option"
for modern hardware. `bench/stages` had already shown decode splits into two
sequential stages whose ratio depends on the filter — for a 3000x3000 photo,
adaptive spends 59% of decode time unfiltering, delta/none spend 92-98% in
zstd. Neither stage can be split *within itself*: zstd's back-references are
sequential, and adaptive's Up/Avg/Paeth each read the row above. But the two
stages could in principle run *concurrently* on different rows through a
ring buffer, turning the sum into a max: `max(235.9, 339.6) = 339.6` vs
`575.5`, a **1.70x ceiling** for adaptive, 1.09x for delta.

**What was built.** `bench/pipeline.c`: a producer thread decompressing into
an N-row ring, a consumer thread unfiltering out of it, alongside two
controls run on identical input — `serial-1` (the shipping one-row-window
decoder) and `serial-N` (same single thread, same ring size), so a bigger
buffer's own speedup is not credited to threading. All three are checked
byte-identical every run. Verified race- and deadlock-free under
ThreadSanitizer, 0 warnings, all ring sizes and filters.

**The problem the measurement ran into.** This project's only test machine is
a 2-core Pentium B960 laptop, and it could not be gotten cleanly idle: even
after closing applications dropped the 1-minute load average to 0.09, a
control of **two fully independent decodes with zero synchronisation between
them** — the best case any 2-thread scheme could possibly hit — measured only
54-73% of the 200% CPU-seconds it should get, confirmed by comparing process
CPU time to wall time rather than trusting `/proc/loadavg`. Across every
sweep run (9 to 41 repetitions each, at default and at a `nice -10` priority
boost, the most this non-root shell was permitted), **zero repetitions**
reached an 85%-of-2-cores threshold. Real-time scheduling (`chrt`) was
available and would likely have gotten a clean reading, but was not used: two
non-yielding SCHED_FIFO threads on a 2-core box can starve every other
process including the one running this measurement, and the machine was
being administered remotely with nobody able to reach it physically. That
risk was judged not worth a benchmark number.

**What the contaminated readings agree on anyway.** Every sweep, under every
condition tried (loadavg 0.09 to 4.9, default and boosted priority), put the
pipeline's `pipe-N` time at **0.71x-1.04x of `serial-N`** — parity or a
slight loss, never a gain, and nowhere near the 1.09-1.70x arithmetic
ceiling. This held even in the runs where the *independent-decode control*
briefly touched 1.5-1.6x, i.e. when the machine did have some spare
parallelism to give, the pipeline still did not capture it. That is
consistent with synchronisation and cache-sharing overhead eating the
theoretical gain, though it cannot be fully separated from residual
contention without a genuinely idle two-core window.

**Decision: not adopted.** No sweep, under any condition reached this
session, showed the pipeline beating the equivalent-ring-size serial decoder.
Given that, and that the ring buffer and thread lifecycle are real
complexity and a real (if small) memory cost, a `PXL_DECODE_PIPELINE` build
option is not worth adding on the evidence collected. Reopen only with a
measurement taken on hardware that can sustain two genuinely idle cores for
the duration of the sweep — a rejection resting on a contaminated
measurement is weaker than the arithmetic it was checking, and should be
revisited rather than treated as final.

**What this does not affect.** `libpxlcore.a` has zero mutable global state
(checked with `nm`, against one in libpng), so decoding N independent images
across N cores already works today with no library changes — that is the
multicore story for this format, not a same-image pipeline. See the decoder
size and memory entries above for why the library stays this way.


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

### Pre-freeze audit of pixel formats — what the container actually carries
**Measured 2026-09-16**, before freezing the specification. Eighteen
combinations encoded and decoded back, compared against the source with
`magick compare -metric AE`: **every one is 0**.

Preserved in their source form, sub-byte depths included:

| source | stored as |
|---|---|
| indexed 1 / 2 / 4 / 8-bit | same depth, palette section |
| indexed 2-bit with per-index alpha (`tm3n3p02`) | 2-bit, palette + alpha |
| indexed 8-bit with alpha (`tbbn3p08`) | 8-bit, palette + alpha |
| gray / gray+alpha / RGB / RGBA at 8 and 16 bit | unchanged |

So **sub-8-bit with alpha does exist** — through the palette, which is also the
only way PNG expresses it. PNG has the same restriction we do: no gray+alpha
below 8 bits.

### Sub-8-bit grayscale was expanded to 8-bit — fixed 2026-09-16
**The container allows it** — SPEC 2.1 says depths 1/2/4 are meaningful "for
indexed and grayscale images" — but `pxl_png.c` calls
`png_set_expand_gray_1_2_4_to_8` on load, and the writer says outright that
sub-byte gray has no colour type of its own there. The format can express it;
the codec never produces it.

**Fixed 2026-09-16.** The loader now keeps sub-byte grayscale packed and
`pxl_save_png` writes it back as `PNG_COLOR_TYPE_GRAY` at its own depth.
Grayscale carrying `tRNS` still expands, because the transparency becomes an
alpha channel and there is no sub-byte alpha to hold it. On the same 1200x1600
bilevel document: **1-bit in the file, 240 000 raw bytes against 1 920 000, the
file 2746 bytes against 3567 (-23%), and decode peak 1.2 MiB against 4.3
(3.6x)**. All 162 PngSuite files round-trip bit-exact, `ctest` and the 200 000
iteration fuzz pass are clean under ASAN and UBSAN.

The cost it used to carry, measured on that same document before the fix:

| offered as | stored as | raw bytes | file | decode peak |
|---|---|---:|---:|---:|
| 1-bit grayscale | expanded to 8-bit | 1 920 000 | 3567 | **4.3 MiB** |
| 2-bit indexed | kept at 2-bit | 480 000 | **2849** | **1.5 MiB** |

20% of file size and **2.9x of decode memory**, and memory is the priority that
binds on the target. This is an implementation gap rather than a format change:
loading must stop expanding, and `pxl_save_png` must learn to emit
`PNG_COLOR_TYPE_GRAY` at depths 1/2/4.

### 10-bit as a packed sample format — rejected on measurement
**Why it was raised:** HDR delivery is 10-bit, `BitDepth` has no such value, and
storing 10-bit data in 16-bit containers looks like throwing away six bits per
sample.

**Result: packing would make files bigger.** The same 10-bit content, 1.89M
samples, `zstd -12`:

| layout | raw bytes | compressed |
|---|---:|---:|
| 16-bit containers, six bits unused (what happens today) | 3 780 000 | **1 684 088** |
| packed 10-bit, four samples per five bytes | 2 362 500 | **2 078 119** |

37.5% less raw data compresses to 23% *more*. In the padded layout every sample
occupies a fixed two-byte slot, so the unused bits sit in predictable places and
zstd models them away. Packed, sample boundaries wander across bytes and the
same value produces different byte patterns depending on its position, which is
exactly what defeats a byte-oriented matcher.

**Decision: rejected.** No packed sub-16-bit sample format. Note this also
argues against ever packing 12-bit, for the same reason.

**Done 2026-09-16: `sBIT` is kept.** It survives whenever the channel layout
does — every image except one whose `tRNS` becomes a real alpha channel, where
its per-channel entries would describe an image that no longer exists.
Verified on the 49 PngSuite files that carry one, indexed and truecolour both,
all 162 files still bit-exact.

Fixing it surfaced an older bug. `pxl_meta_inject` placed every preserved chunk
immediately before `IDAT`, which on an indexed image is *after* `PLTE` -- and
`sBIT`, `gAMA`, `cHRM`, `sRGB` and `iCCP` must precede it. libpng read those
files back, but ImageMagick had been warning "gAMA: out of place" on every
indexed file this codec ever wrote, and nobody had followed the warning. The
injector now writes two groups and the order comes out `IHDR gAMA sBIT PLTE
IDAT`, with no warnings across the corpus.

**The reasoning, kept:** PNG's
significant-bits chunk is currently dropped along with `PLTE`, `tRNS`, `bKGD`
and `hIST` as "pixel-layout dependent". For a 16-bit non-indexed image the
layout is *not* changed, so `sBIT` stays valid and the blanket rule is too
broad. Preserving it tells a reader the range is 10-bit at the cost of a
metadata chunk the decoder never looks at — all of the benefit, none of the
format change. Open.

**Not pursued, and deliberately:** float samples (rendering and compositing, not
delivery; needs float arithmetic the target lacks), CMYK and >4 channels,
progressive-by-resolution, and region decode. Each costs specification weight
and decoder size, which are priorities 2 and 3. Note also that "24-bit" in
imaging means 8 bits x 3 channels, which the format has always had; there is no
24-bit-per-channel format in use anywhere.

**Context for all of the above:** the format is pre-release with no outside
users, so the specification can still change without a version bump. That
window closes at release.


### Tuning zstd beyond the level — nothing there
**Why it was proposed:** `pxl_codec_encode.c` calls
`ZSTD_compress(dst, cap, src, size, level)` with a level and nothing else, and
the log had no entry for `strategy`, `targetLength`, `minMatch`, `chainLog` or
the literal modes. A whole parameter space looked unexplored, and unlike a
dictionary it would cost neither the spec nor the decoder a byte.

**Result: the presets win.** Over filtered streams, the best hand-tuned level-12
configuration (`strat=btultra2,tlen=256`) reaches 92.7% of level-12 default at
2.4x the encode time, while plain level 19 reaches 90.2% at 2.7x — smaller and
barely slower. `mml=3` looked like a free win on one class and turned out to be
byte-identical on icons and 99.9% on large images. Numbers in
[BENCHMARKS.md](BENCHMARKS.md).

**Decision: rejected.** zstd's level presets are well chosen for filtered image
data; there is no configuration off that curve worth carrying. Do not reopen
without a specific mechanism in mind rather than a parameter sweep.

### The zstd level is not monotonic on BCIF-filtered photographs
**Found 2026-09-15**, when the corpus table first carried a level-1 row next to
level 12 and level 1 came out *smaller*: 87.7% of PNG against 88.4%.

Not an encoder bug. Split by class, PngSuite (162 tiny files) behaves as
expected — level 12 is 89.2% of level 1 — while Kodak (24 photographs) has
level 12 at **100.9% of level 1**. Holding the filter fixed on one Kodak image
isolates it to zstd itself:

| level | BCIF-filtered bytes | adaptive-filtered |
|---|---:|---:|
| 1 | 600 171 | 807 487 |
| 6 | **609 440** | - |
| 12 | **603 664** | 725 327 |
| 19 | 597 356 | - |

Levels 6 and 12 are *worse than level 1* on the same BCIF stream, and only 19
recovers. The adaptive stream on the same image improves monotonically, so this
is specific to what BCIF produces: a plane-split, decorrelated stream that is
close to noise, where zstd's stronger match finders spend more on sequences than
they recover.

**Consequences.** Kodak carries 13.5 MB against PngSuite's 88 KB, so this one
class drives the whole corpus aggregate — which is why the committed-corpus
table appears to say the default level beats level 12 for everything. It does
not; it says photographs are a special case.

**Open.** The encoder picks the smallest filter *at the requested level*, which
is correct and is not affected. But it exposes that the level itself is a knob
the encoder trusts to be monotonic and which is not, at least for one filter on
one content class. Trying two levels and keeping the smaller would fix it at
double the encode cost; whether that is worth it is unmeasured, and it should
not be decided before the level curves from tonight's sweep are plotted.

### The still-image default level is 1, and every published number is level 12
**Found 2026-09-15 while measuring the level curve.** `PXL_LEVEL_DEFAULT` is 1
for stills (`src/pxl.h`), 12 for animation. `pxltool c` with no `-l` therefore
produces output that is 107% of level 12 on icons, 117% on small screenshots and
**144% on large ones** — while the README, the corpus tables and every research
entry quote level 12.

The level is *not* a pure encode-time trade, though it looked like one at
first: decode throughput measured flat from level 1 to 19 on synthetic content,
but re-measured in process on photographs, zstd decompression is three times
slower at level 19 than at level 1 (3.57 ms against 10.42 ms on one Kodak
image, unfilter unchanged). The first measurement used the zstd CLI, where
startup and I/O masked it. See the 2026-09-16 correction in BENCHMARKS.md. Against that, encoding a large
image goes from 1.11s to 11.97s between level 12 and 19, which is real for a
batch conversion.

**Resolved 2026-09-16: the default stays at 1, and the tables carry every
level.** `bench/bench.sh` and `bench/corpus.sh` take `PXL_LEVELS` and emit one
row per level with the default marked, so the comparison is in front of the
reader instead of hidden behind a qualifier. The correction above supports the
choice for a reason that was not known when it was made: on photographs a higher
level costs decode speed as well as encode time.


### Decoder size — was not met, met since 2026-09-15
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

**Update 2026-09-15: measured again, the priority is met.** Both of the "what to
do next" items had already landed — the zstd build options and the codec TU
split in `2d8d9ec` — and nobody re-ran measurement 2 for seven weeks. Repeating
it exactly gives **174 066** bytes of `.text` against libpng's 209 913, so the
decoder is 17% *smaller* than libpng, or 36% smaller if zlib is counted on
libpng's side the way libzstd is counted on ours. Numbers and verification in
[BENCHMARKS.md](BENCHMARKS.md).

The 861 KB was never a property of the format, only of the linker being unable
to split an object file. Two figures quoted above are now wrong and superseded:
the "285 KB floor for any zstd-based variant" is **147 570** once legacy support
and multithreading are off, and "our own code is 19 KB" is now 26 496 bytes,
the loop specialization having cost about 7 KB of text for its 2.5x.

**What this unblocks.** Every deferred decision that was waiting on decoder size
is now free of that constraint: removing BCIF no longer has a size motive (it
never did, per the entry below), restricting the filter set has no size motive,
and "V2 in any form — deferred" loses the reason it was frozen for, since it was
frozen until the decoder-size goal was met. Those decisions still need their own
measurements; they just no longer need this one.

**What is still unmeasured:** size is not footprint. The PSP-class target is a
32 MB RAM budget and nothing here measures decode-time memory — zstd's window
plus a full-image pixel buffer. That, not `.text`, is now the open question for
the target hardware.

### Decode-time memory — the priority that actually binds (fixed 2026-09-15)
**Measured 2026-09-15, same day as the entry above and directly out of it.**
Peak RSS of minimal decoders; numbers and method in [BENCHMARKS.md](BENCHMARKS.md).

**Problem.** On a 3000x3000 RGB photograph `pxl_decode` peaks at 62.9 MiB and
the streaming decoder at 53.3 MiB, against libpng's 26.9 MiB — that is 2.44x
and 2.07x the pixel buffer against libpng's 1.04x. `apxl_decode` peaks at twice
the entire animation, so one 8-frame 1080p shot needs 128 MiB. Against the
32 MB target the still path tops out around 2270x2270 and the animation path
around 30 frames at the PSP's native 480x272.

**Cause, read off the code rather than guessed.** `pxl_decode`
(`pxl_codec_decode.c:439-464`) holds `filtered` at full `raw_byte_count` and
`pixels` at full size simultaneously. `pxl_stream_new` (`:655-656`) allocates
the same two full buffers, so **progressive decode does not reduce footprint**
— it only streams the compressed input. `apxl_decode` (`apxl_codec.c:212-245`)
materializes all frames in one `raw` buffer and then copies each frame into its
own allocation before freeing `raw`.

**A hypothesis this killed.** The 128 MiB `APXL_WINDOW_LOG` was suspected of
forcing a huge window on the decoder. It does not: `apxl_decode` decompresses
one-shot, where the destination is the window. LDM on and off differ by 3.5 MiB
and in the opposite direction. Do not reopen this one.

**Possible solution, not yet measured.** Unfiltering depends only on the
previous row, so `filtered` need not be the whole image — a bounded ring buffer
of a few rows fed from `ZSTD_decompressStream` should bring the streaming path
to roughly 1.0x plus a constant, i.e. libpng's footprint, with byte-identical
output and no format change. Same per frame for `apxl_decode`. This is the same
class of decoder-side-only work as the TU split, and on the evidence above it is
the highest-value item open.

**Decision:** open, and it supersedes decoder size as the binding constraint on
the target hardware. Note the ordering lesson: `.text` was tracked for months
while the figure that actually decides whether a PSP can open a photograph was
never taken.

**Update 2026-09-15: done for the still path, and it lands where predicted.**
The filtered side is now consumed through a one-row window in both decode
paths. Streaming a 3000x3000 photograph fell from 53.2 to 27.6 MiB against
libpng's 26.9, so the still path reaches the reference footprint and a
9-megapixel image fits the 32 MB budget. Byte-identical over 372 decodes;
numbers in [BENCHMARKS.md](BENCHMARKS.md), code in `db07781`.

**What it did not fix, and this is the important half.** BCIF has no row window
by construction, and BCIF is what the encoder chooses for photographs. A
default-encoded photo still costs 53 MiB; only `-p` buys the saving, at +4% to
+6.6% in size on large images. The format now has a memory/size dial instead of
a wall, but it has to be turned on purpose.

**This is now the strongest argument yet against BCIF** — stronger than the size
and progressiveness arguments in the entry below, which were a matter of taste.
On the stated target hardware the row-wise path is the only one that runs, and
BCIF is the single filter that cannot use it. Removing it would make the good
memory behaviour the default rather than a flag. What that costs on photographic
size is already measured and is the counter-argument to weigh.

**Still open:** `apxl_decode` is untouched and still peaks at twice the whole
animation. It is the simpler fix of the two — decode frame by frame instead of
materialising every frame and then copying each one.

### The comparison with QOI was stated incorrectly
**Problem:** it was claimed that PXL beats QOI. Measurement 3 shows the
opposite: QOI is faster on 24 out of 24 photographs, by roughly 30%.
**The truth:** PXL is 2.1 times faster than libpng, and files are 14.5% smaller
than PNG. QOI meanwhile barely compresses at all — 103.5% of PNG, meaning its
files are slightly larger than the originals.
**Decision:** fix the wording in the README to be honest. Catching up with QOI
on speed while keeping compression is an open task, not a stated fact.

### Removing *adaptive* rather than BCIF — hypothesis, not yet measured
**Raised 2026-09-15.** Every discussion of trimming the filter set so far has
been about BCIF. Re-reading the 2026-07-26 stage numbers below with the target
hardware in mind suggests the wrong filter may have been on trial.

Those numbers, on a 3000x3000 RGB photograph:

| filter | bytes | unfiltering |
|---|---:|---:|
| bcif | 11 458 339 | 20.9 ms |
| adaptive | 11 550 315 | **405.7 ms** |
| delta | 12 198 006 | 19.9 ms |

**Adaptive is dominated on both axes by BCIF here** — very slightly larger and
twenty times slower — and it is beaten on speed by delta too, for 5.3% of size.
The entry below reads as "BCIF is 20x faster than adaptive", but the comparison
that matters for a small decoder is the other one: *delta is as fast as BCIF and
streams*, so BCIF's real price is 6% of size against delta, and adaptive's real
price is 405 ms against delta's 20.

**Why this matters more than it used to.** The cost of adaptive is Paeth: three
data-dependent comparisons per byte, on 91% of unfiltered bytes, which the
branch predictor cannot guess. The loop-specialisation attempt recovered only
16-26% of it and was accepted as "the goal is not met". On the PSP-class target
— MIPS at 222-333 MHz, no SIMD, small cache — that cost does not stay at 20x,
it grows, and 405 ms on a desktop is not a promising starting point.

**What argues against removing it, and these are not small.** Adaptive is the
only filter available for grayscale, since BCIF requires 8-bit RGB/RGBA with no
palette; it is the only predictor beyond a left delta for indexed and sub-8-bit
images; it is chosen for 56 of the 186 committed corpus files; and it is listed
above under *Accepted decisions* as the format's main path. Removing it would
leave grayscale and palette content with nothing but NONE and DELTA, and the
README already records grayscale at 100.3% of PNG — slightly *worse* than PNG —
so there is no headroom to give away there.

**What would have to be measured before this is anything but a hypothesis:**
1. Corpus size cost with adaptive excluded, split by content class — photographs,
   synthetic stills, grayscale, indexed. `predlab` already compares filter
   variants per file, so this needs no new tooling.
2. Paeth throughput on the actual target (see ROADMAP). If adaptive is merely
   slow there it is a tradeoff; if it is unusable it decides the question.
3. What the encoder would pick instead on the 56 files where it currently picks
   adaptive, and what that costs each of them.

**Status: open, and deliberately not acted on.** This contradicts an accepted
decision, so it needs better evidence than a re-reading of one photograph's
numbers. Recorded now so the idea is not lost and so nobody re-derives it from
scratch — and so the BCIF discussion below is read with the knowledge that
adaptive, not BCIF, is the filter that costs twenty times the rest.

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

### Filter probing on slices instead of the whole frame — rejected

**Why it was proposed:** the encoder tries every filter over the full frame, so
probing is the dominant cost. Estimating each filter on a few horizontal slices
should pick the same winner for a fraction of the work.

**Result:** it fixed the synthetic cases it was written for and made real
photographs worse: **+371 KB over 9 photos**, worst case `texmos2.p512` at
**+33%**. A slice is not representative of a texture — local statistics elect a
filter that loses over the full frame.

**Decision:** rejected, the experiment is reverted out of the tree. The cure was
worse than the disease. Do not reopen as a pure sampling trick; if encode probing
is attacked again it has to be by making the probe cheaper, not by making it see
less of the image.

### Reusing one `ZSTD_CCtx` across probes — rejected

**Why it was proposed:** the previous entry named it as the promising mechanical
follow-up. `pxl_codec_encode.c` calls one-shot `ZSTD_compress` per probe, while
`apxl_codec.c` already reuses a context. Reuse cannot change the output bytes,
only the time, so the risk profile looked like the opposite of slice probing.

**Result:** it is slower, not faster. Measured with `perf stat -e instructions:u`
on Kodak `01/10/19.png` against the unmodified build:

| level | reused `ZSTD_CCtx` vs one-shot |
|-------|-------------------------------|
| L1    | +1.2% … +1.3% |
| L6    | +0.38% … +0.41% |
| L12   | -0.04% … +0.05% (noise) |

Two variants were tried. `ZSTD_compress2` with the level set once via
`ZSTD_CCtx_setParameter` cost ~1.5M extra instructions per candidate;
`ZSTD_CCtx_setPledgedSrcSize` did not recover any of it. `ZSTD_compressCCtx`,
which takes the level directly and skips the advanced-parameter path, was
cheaper but still a net loss at every level.

**Why:** one-shot `ZSTD_compress` sizes its workspace to the exact input, so the
match tables it clears are only as large as the buffer at hand. A reused context
keeps the largest workspace any prior candidate needed and re-clears all of it on
every call. Clearing those tables costs more than the allocation the reuse saves,
and the effect is strongest at low levels where compression itself is cheap.
The saving is real in `apxl_codec.c` only because frames there are uniform in
size and the context is reused across a long sequence, not 3-4 candidates.

**Decision:** rejected, reverted, output verified bit-identical to baseline and
`ctest` green. Encode probing cost is dominated by zstd itself, not by
per-candidate setup; there is no mechanical win left here. Reducing the candidate
count on evidence is the only remaining lever, and that trades size for speed.

### The gray + `tRNS` round-trip "bug" did not exist — retracted

**Why it was recorded:** comparing decoded output against PIL reported pixel
mismatches on `tbbn0g04` and `tbwn0g16`, which looked like the codec mishandling
transparency on grayscale.

**Result:** the oracle was wrong, not the codec. PIL's `convert('RGBA')` ignores
the `tRNS` chunk for `L` and `I;16` images, so transparent source pixels were
being compared against opaque ones. Checked independently with
`magick compare -metric AE`: **0** on both files, the round trip is bit-exact.

**Decision:** retracted, no decoder change needed. The lasting lesson is about
tooling: PIL is not a trustworthy oracle for palette or grayscale-with-alpha
PNGs. If the suite is extended over PngSuite, compare via ImageMagick AE or
against decoded RGBA bytes directly, otherwise we will collect more phantom bugs.

### Animation: cross-frame coding splits the corpus in two

**Why it was measured:** the Anita industrial animation dataset (16871 frames in
367 shots, 11 GB) is the first corpus of real hand-drawn animation available to
us. Every shot ships three passes — `sketch`, `composition`, `color` — so it
separates line art from finished frames.

**Result:** still-image ratios against the source PNGs at `-l 12`, sampling the
first frame of each of the first 24 shots per pass (one frame per shot, so a
single long shot cannot dominate): `color` **34.2%**, `sketch` **44.3%**,
`composition` **61.5%** (the last over 18 shots — the remaining ones have no
frame at that index). The encoder picks BCIF on `sketch` and `none` on
`composition`/`color`, so those finished frames are stored essentially verbatim
before zstd.

A caveat that matters more than the numbers: an earlier pass over "the first 24
frames" rather than one frame per shot put `sketch` at 56.7% and `composition` at
75.4%. Same corpus, same encoder, ~12 points of spread purely from frame
selection, because consecutive frames of one shot are near-duplicates and the
sample collapses onto whatever that shot looks like. Treat any single-digit
comparison over this dataset as noise unless the sampling is stated, and prefer
per-shot aggregation.

The cross-frame result is the interesting one. Concatenating 16 raw RGBA frames
of one shot and compressing with `--long=27`, against compressing each frame
independently: `composition` gains **71.7%**, `sketch` **loses 2.3%**. The real
`.apxl` path agrees on the first half — 16 composition frames encode to 68.1% of
the APNG. Consecutive finished frames are nearly duplicates and long-distance
matching finds them; consecutive line-art frames share almost nothing, and the
larger window only costs bookkeeping.

**Decision:** open. It argues that cross-frame long-distance matching should be a
measured decision per stream rather than a flag tied to level >= 10, since on
line art it is a small net loss. Not acted on yet: this is 16-frame evidence from
two shots, and the threshold must come from a sweep over the corpus, not from
these numbers.

### The corpora do not cover the content the format is aimed at

**Observation, not a measurement.** Taking stock of what is available locally:
Kodak (24 photographs), the PNG test suite, CLIC 2020 mobile train (1048
photographs, 3.8 GB) and Anita (hand-drawn animation, 11 GB). Three of the four
are photographic; the fourth is animation.

Nothing covers synthetic non-photographic stills — UI screenshots, diagrams,
rendered text, charts. That is the content a PNG replacement actually gets
pointed at, and where the README's own claims are strongest, so it is the one
gap where a regression could go unnoticed indefinitely. Note the direction of the
bias: photographs are where PXL is *weakest* (on aerials and textures zstd's edge
over DEFLATE nearly vanishes), so the current corpora understate the format while
leaving its strong case unverified.

Adding CLIC to the corpus table is cheap and worth doing, but it deepens the
existing bias rather than fixing it.

**Update 2026-09-15: the gap is closed, and the claim survived.** 442 real
desktop screenshots (`tests/data/Screenshots`, personal and gitignored, so
aggregates only) put PXL at **73.2% of PNG against 88.4% on the committed
photographic corpus** — a 15-point gain on synthetic content, while JXL moves
0.1 points over the same shift. The narrowing is therefore ours and not the
corpus being easier: the gap to JXL closes from 22.7 points to 7.4, and PXL
beats `oxipng -o max` by 5.8 points on the content PNG is most used for.
AVIF turns out to *inflate* this content to 109.6% of the source PNGs.
Numbers in [BENCHMARKS.md](BENCHMARKS.md); reproduce with
`CORPUS_ONLY=tests/data/Screenshots bench/corpus.sh`.

The bias noted above is real but now bounded: the photographic corpora
understate the format by roughly 15 points relative to its own target content.
What is still missing is a *committed* synthetic corpus — this one cannot be
published, so the README's reproducible table still cannot show the format's
best case. A small set of freely-licensed UI screenshots or rendered diagrams
would fix that, and is the remaining piece.

### The README benchmark tables are stale

**Observation.** The still-image and animation tables were generated before the
filter work and have not been regenerated, so the committed numbers do not
describe the current encoder. They are reproducible — `bench/bench.sh
--update-readme` and `bench/corpus.sh` splice between marker comments — so this
is bookkeeping, not research.

One structural point while regenerating: the still-image decode column times
`pxltool d`, which writes a PNG on the way out, so a large part of what it
reports is libpng's deflate rather than PXL's decoder. The README explains this
in prose underneath, which is the wrong fix — `bench/rawdec` already measures
decode without the re-encode and should be the primary number.

### Security audit before freeze — two 32-bit overflow OOB reads, fixed
**Done 2026-09-16.** A pass over every entry point that takes untrusted bytes
(`pxl_decode`, the streaming decoder, `apxl_decode`, `pxl_meta_extract`/`inject`,
`apng_load`), run against a **32-bit ASAN build** because the stated target is a
32-bit platform and every bug here is invisible at 64.

**Bug 1 — chunk-length bounds, 6 sites** in `pxl_meta.c` and `apng.c`. The
pattern was `offset + (size_t)len + 4 > size`, with `len` an attacker-controlled
uint32 up to 0xFFFFFFFF. On a 32-bit `size_t` the left side wraps and passes,
after which the walker copies `len` bytes. A PNG chunk claiming 4 GB inside a
64-byte buffer produced an AddressSanitizer negative-size-param in `bb_append`.
Rewritten as subtraction from the known size. The telling part: `apng_load`
already guarded its *canvas allocation* against this exact wrap, with comments
naming the 32-bit hazard, while the chunk walk in the same function did not —
allocation sizes were hardened, iteration bounds were not.

**Bug 2 — `apxl_decode` frame-count check.** `canvas_bytes * frame_count` was a
`size_t` multiply; an 8192x8192x4 canvas with 16 frames makes it exactly 2^32,
which is 0 at 32 bits, matching a declared `raw_byte_count` of 0. The check
passed and the decoder returned 16 frame pointers spaced 2^28 apart into a
zero-byte buffer, each advertising `buffer.size == 2^28`. A consumer reading the
last frame took a SEGV under 32-bit ASAN. Fixed by doing the multiply in
`uint64_t`; since `raw_byte_count` is a uint32, a match proves the real total is
below 2^32 and the offsets stay in bounds.

**What was checked and found clean:** `pxl_geometry_of`/`pxl_row_bytes_of`
(64-bit intermediate with an explicit SIZE_MAX guard), the APNG compositing
rectangle (`(uint64_t)x + w > canvas_w`), the still-header offset checks (already
subtraction-form), and 3M decode-fuzz plus 700k metadata-fuzz iterations under
ASAN and UBSAN on both word sizes.

**Lasting note:** every survivor and every bug came down to one rule — validate
a file-supplied length by *subtracting from the known size*, never by adding to
an offset. On this project's 32-bit target that is a correctness rule, not a
style preference. `tests/fuzz_meta.c` was added because the metadata paths, where
bug 1 lived, had no fuzz coverage at all.

---

### Packed 16-bit native formats (RGB565, RGBA5551) — measured, not decided
**Raised 2026-09-17**, prompted by the ChatGPT-suggested full-pipeline
benchmark and the question of whether PXL could ingest PSP-native pixel
formats directly. First instinct was "already answered" — this project
already rejected packed sub-16-bit samples once (packed 10-bit, four samples
per five bytes: 37.5% less raw data compressed to 23% *more*, above). That
instinct was checked with `bench/packedformat.c` rather than trusted, and it
was wrong.

**Why the two cases differ.** The 10-bit rejection's mechanism was that
sample boundaries wander: 4 samples pack into 5 bytes, so a byte's role
(which sample, which bits of it) cycles with a 4-sample period, and a
byte-oriented matcher sees the same value produce different byte patterns
depending on its position. RGB565/5551 do not have this problem — every
pixel is *exactly* 2 bytes, always in the same bit layout, so byte 0 of pixel
N and byte 0 of pixel N-1 are always the same bit-field (e.g. always "R's top
5 bits + G's top 3 bits"). There is no wandering to defeat the matcher.

**Result, Kodak (24 photographs), 8-bit RGB truncated to 565:**

| | raw bytes | compressed |
|---|---:|---:|
| 8-bit (existing) | 100% | 100% |
| packed 565 | **66.7%** | **55.5%** |

Compressed size is smaller than the raw-byte ratio alone would predict —
565's precision loss removes some of the noise floor 8-bit had to spend bits
encoding, not just half the container. On 15 Synthetic-Screenshots files
(RGB565/RGBA5551 depending on source channels) the effect is smaller and
sometimes reverses slightly (compressed 64.3% against raw 53.7%) — flat UI
regions and sharp edges don't have a noise floor to truncate, so quantizing
them trades exact colour reproduction for a ratio close to, but not always
better than, proportional.

**The caveat that matters more than either number.** Both measurements above
*discard real bits* — they truncate genuine 8-bit source photographs and
screenshots to fewer bits per channel, which is lossy, full stop. That is not
something to build into a codec whose whole identity is "lossless at every
bit depth" as a way to shrink existing 8-bit assets. The measurement is
honest about what it tested (compressibility and raw-byte volume of 565 data,
regardless of where it came from) but the numbers above are not evidence for
"convert your photos to save space."

**Where this could be legitimate:** content that is *already* 565/5551/4444
before it ever reaches PXL — which describes a lot of real PSP game texture
authoring, where assets are exported at reduced precision on purpose to save
console memory and bandwidth. Storing *that* content packed, losslessly (no
bits are discarded that the source did not already lack), would get the
measured size and raw-byte benefits for free, and — since this project's own
numbers show zstd is 88-98% of decode time for every filter but adaptive —
the raw-byte reduction should translate into a real, roughly proportional
decode-speed gain on top, not just smaller files. It would also let the PSP
skip the RGBA8888-to-565 conversion pass entirely for such textures, since
the decoded bytes would already be in the exact layout the GE wants.

**Status: not decided.** This needs a real format decision, not a flag:
`.pxl`'s container currently describes geometry as channels (1-4) x depth
(1/2/4/8/16), which has no slot for "3 or 4 channels packed at 5-6-5 or
5-5-5-1 bits" — adding one means a SPEC.md change, a decoder change to
recognise it (trivial: it is still a fixed-width sample, unfilter does not
care what the bits mean), and a decision about whether a codec that has
never been lossy anywhere gains its first lossy-adjacent input path (lossless
*of the packed source*, but a tool built to load an 8-bit PNG and hand it to
`pxltool` would need to already have quantized before encoding, which is a
new failure mode to explain in the spec, not in the encoder). Left open
rather than accepted or rejected; revisit with an actual corpus of
natively-565 PSP game textures, which this project does not have yet.

---

### Does WebP even run on PSP? Checked instead of assumed
**Raised 2026-09-17**: README already says WebP lossless beats PXL on both
size and speed on synthetic stills (measured on x86). Before that comparison
is treated as settled for the actual target, it needed the same question
every other x86 number in this project has had to answer: does it hold on
MIPS, or is x86 an adjacent proxy again.

**libwebp cross-compiles for PSP, but not out of the box.** Its CMake build
hardcodes `POSITION_INDEPENDENT_CODE ON` in two places (once for the whole
project when `WEBP_LINK_STATIC` is set, which is the default; once directly
on the decoder's OBJECT library targets, unconditionally, "because it is not
ON by default") — `psp-gcc`'s `-mabi=eabi` cannot generate PIC at all, an
ordinary property of bare-metal/embedded MIPS toolchains. Both had to be
patched out by hand to get a build; there is no pspdev portlib for webp the
way there already is for libpng, so this patching is real, currently-undone
work, not a checkbox. Once past that, it built and linked clean.

**Functional correctness, confirmed under `PPSSPPHeadless`** (after also
discovering, the hard way, that a hand-linked PSP ELF needs `psp-fixup-imports`
run on it or every kernel-import call jumps into unpatched stub space and
crashes on the first syscall — CMake's `create_pbp_file` macro already does
this for `psp_psp_bench`, but a manual `psp-gcc` link does not): `WebPDecodeRGBA`
on an embedded lossless WebP file decoded to the correct 480x272 with no
crash. WebP genuinely can run on this target; the user's suspicion that it
might not was reasonable given no PSP port exists today, but wrong on the
merits.

**Decoder size, MIPS, both at `-O3 -DNDEBUG`** (this project's own Release
flags, `bench/mindec_pxl.c` for PXL, `WebPDecodeRGBA` for WebP, `psp-size`'s
`.text` column):

| decoder | MIPS `.text` |
|---|---:|
| PXL (minimal decode-only) | 267 696 |
| WebP (`WebPDecodeRGBA`, VP8+VP8L combined) | 295 660 |

PXL is 9.5% smaller here, but the comparison favours PXL more than it should:
`WebPDecodeRGBA` is a format-sniffing dispatcher that statically reaches both
the lossy VP8 decoder and the lossless VP8L one, and libwebp's public API has
no lossless-only entry point to call instead, so this number carries lossy
decode code no real use of it here would need. The true lossless-only
footprint is unmeasured and likely smaller than 295 660 — this table is
therefore inconclusive on decoder size, not a PXL win, and should not be
quoted as one.

**The finding that actually matters more than the WebP comparison**: PXL's
own MIPS decoder is **267 696 bytes**, against the published x86 figure of
**174 066** — 54% bigger on the real target than the number "decoder size —
met" has stood on. That x86 number was never re-verified on MIPS, exactly the
category of gap this project already found and fixed once for decode speed.
Filed as its own ROADMAP item; this project's own decoder-size claim needs
the same target-hardware treatment WebP's speed-and-size claim was just given,
before either can be trusted.

**Status: informative, not a decision.** Nothing here is committed to the
project (the libwebp source and patches used for this check are not
vendored) — this answers "would it even work" and "roughly how big," which is
what was asked, not "should PXL be replaced with WebP for this."

**Correction, same day: the 54% was almost entirely measurement artefact, not
PXL's code.** Asked to find out why, `bench/mindec_pxl.c`'s own `.text` was
broken down by symbol on MIPS (`psp-nm --size-sort`), and two things were
wrong with comparing it to the x86 figure at face value:

1. `bench/mindec_pxl.c` calls `printf` and `fopen`/`fread`. On x86 these
   resolve into dynamically-linked glibc and cost nothing in the static
   `.text` count. PSP homebrew has no shared libc, so every one of these
   calls pulls its entire object file's code into the binary — and newlib's
   `printf` is not modular enough to avoid dragging in float-to-string
   conversion (`_dtoa_r`, 7 068 B) and even `scanf`-family internals
   (`__ssvfscanf_r`, 8 948 B) that this program never uses. Rewriting the
   test PSP-idiomatically (`sceIoWrite` instead of `printf`, the compressed
   file compiled in instead of `fopen`/`fread`, matching how `psp/main.c`
   already does it) removed some of this, but not most of it, because —
2. An **empty PSP program that does nothing but call `sceKernelExitGame()`**
   already costs **121 004 bytes** of `.text`, against x86's empty-program
   baseline of 265. PSPSDK's standard newlib runtime start-up
   (`libcglue.a`'s kernel/environment/timezone glue, run before `main`) calls
   `sprintf` unconditionally as part of its own initialization — every
   PSPSDK homebrew binary pays this, regardless of what the program does,
   and it has no x86 counterpart because Linux's dynamic linker defers
   essentially all of it to shared libraries instead of statically baking it
   into every binary.

**Corrected comparison**, each side's own empty-program floor subtracted so
both measure only the code a program's own logic adds:

| | full `.text` | empty-program floor | net (PXL + zstd) |
|---|---:|---:|---:|
| x86 | 174 066 | 265 | 173 801 |
| MIPS | 311 456 (`-ffunction-sections -fdata-sections -Wl,--gc-sections`) | 121 004 | **190 452** |

**190 452 / 173 801 = 1.096 — PXL's own code is 9.6% bigger on MIPS, not
54%.** That remainder is an ordinary, expected RISC-vs-CISC code-density
difference (MIPS's fixed 32-bit instructions encode some operations, like
loading a large constant, in more instructions than x86's variable-length
ones do) and is not a red flag. The 121 004-byte floor is real and does
matter for anyone budgeting PSP flash/RAM against a "how big is my program"
number, but it is a cost of targeting PSPSDK's standard newlib runtime at
all, paid by every homebrew binary built this way, not something specific to
PXL or fixable in PXL's own source.

**What this means for the ROADMAP item it prompted**: "decoder size — met"
does not need retracting after all — but the *number to quote* for the MIPS
target is 190 452 (net of the unavoidable PSPSDK floor), not the raw 267 696
or 311 456 `.text` figures, and none of these three has gone through the
project's real build pipeline yet. That re-measurement is still worth doing
properly; it should no longer be expected to find a problem.

### Can an RPG Maker VX Ace game's Ruby scripting run fast enough on PSP? Measured, not assumed
**Raised 2026-09-17**, while scoping a port of *Mogeko Castle* (RPG Maker VX
Ace, RGSS3/Ruby 1.9.2) to PSP as a real-world consumer of PXL's own stated
target ("PSP-class, 32 MB RAM, no fast float"). Two structural blockers stood
between "PXL decodes fast enough" and "the game runs at all": PSP's GE is
fixed-function only (RGSS's `tone=`/`hue_change` compositing is normally done
via `mkxp`'s GLSL shaders, which the GE cannot run), and nobody has published
whether a Ruby-class interpreter is fast enough on a 222 MHz single-core,
in-order, non-JIT MIPS core to keep up with RGSS3's Fiber-driven event
interpreter. The renderer question resolved analytically (tone decomposes
into two ordinary alpha/additive fixed-function passes; hue is normally
static per sprite and cachable). The interpreter question could not — it
needed an actual number.

**Method:** cross-compiled `mruby` (not full MRI — mruby was picked as the
realistic candidate: RGSS3's core event loop is load-bearing on `Fiber`,
which mruby carries natively, unlike `Marshal`, which mruby lacks entirely)
for `mipsel-psp-elf` against a prebuilt `pspdev` toolchain, then ran three
synthetic workloads modeled on patterns read directly out of this game's own
decrypted `Game_Interpreter`/`Game_CharacterBase`/`Window_Message` scripts
(RGSS3A decryption and Marshal parsing for `Scripts.rvdata2` were written
from scratch for this — the format is documented in `mkxp`'s
`rgssad.cpp` and Ruby's own Marshal spec, no external tool needed). Timed on
real emulated PSP cycles via **PPSSPP's `-i` interpreter core**, which is
cycle-counted against a stock 222 MHz Allegrex and is not sped up by a host
JIT (`-j` would measure the emulator's host speed, not the console's) —
cross-checked every time against the same script run through a natively
compiled `mruby` on the (modest, 2011) host CPU, since a slowdown ratio miles
outside the ~10–20x a 2.2 GHz out-of-order core should have over a 222 MHz
in-order one would mean the measurement itself was broken, not the result.

| workload | PSP (`-i`, 222 MHz) | host (Pentium B960 @ 2.2 GHz) | ratio |
|---|---:|---:|---:|
| sprite/object `update` + command dispatch, per simulated frame | 1.774 ms | ~0.12 ms | ~15x |
| `Window_Message`-style `gsub`/`sprintf` escape-code processing, per message | 0.638 ms | ~0.046 ms | ~14x |
| custom binary serialize+deserialize (mruby has no `Marshal`), per round trip of a 1724-byte representative save graph (200 switches, 200 variables, 4-actor party, system flags) | 38.10 ms | ~1.87 ms | ~20x |

All three land in the same 14–20x band, which is what makes the number
trustworthy rather than a one-off. Against a 16.67 ms/frame budget at 60 fps,
1.77 ms of scripting overhead leaves roughly 9x headroom before the
interpreter itself is the bottleneck; 38 ms for a full save-data round trip
is a one-time action, not a per-frame cost, and stays well inside "instant"
even at several times this toy structure's size.

**What this does not show:** mruby is not MRI/YARV — its simpler VM is
almost certainly faster per Ruby op, so this is an optimistic signal for "a
Ruby-shaped interpreter," not proof that literal RGSS3+MRI hits the same
number. The workloads are synthetic, modeled on the real scripts rather than
replaying them. The measurement is the interpreter alone on an idle core; a
real port shares that one core with GE draw calls, DMA and audio. And
`mruby`'s missing `Marshal` means a real port needs its own save-file format
regardless of speed — this benchmark answers "is a hand-rolled one fast
enough" (yes), not "can we keep the original save format" (no, by
construction).

**Status: informative, not a decision.** Nothing here lands in PXL's own
code or format — like the WebP-on-PSP check above, this answers "would the
premise even survive contact with the hardware" for a downstream project
that plans to use PXL, not a question about PXL itself. The corpus
(`Scripts.rvdata2`, decrypted from the game's own `Game.rgss3a`) is the
user's legally-owned freeware game, not vendored anywhere, per this
project's usual rule against committing test assets.

### Sizing the Mogeko Castle asset budget: how much of the shared RTP does it actually need?
**Raised 2026-09-17**, same port-scoping thread as above. The game's own
`Game.rgss3a` (62.9 MiB) is not its whole asset footprint — `Game.ini`
declares `RTP=RPGVXAce`, RPG Maker VX Ace's free shared runtime package, and
any graphic/sound the game references but doesn't bundle itself has to come
from there. Before crediting PXL with "solving" a memory budget, the budget
itself needed a real number, not the RTP's full 195 MiB (downloaded from
`rpgmakerweb.com/run-time-package`, an Inno Setup installer extracted with
`innoextract` — no execution needed).

**Method:** extended the Marshal reader (built for `Scripts.rvdata2`, see
above) to walk arbitrary `RPG::*` object graphs generically — every Marshal
tag (`Object`, `Float`, `Hash`, symbol links, object links) rather than just
the array-of-triples shape `Scripts.rvdata2` needed — and ran it over all 238
`Data/*.rvdata2` files pulled from the same `Game.rgss3a`. Any string that
exactly matches an RTP file's basename, in a category the game doesn't
already supply itself, is a real addition to the port's budget.

| category | files | size |
|---|---:|---:|
| Animations | 93 | 44.82 MiB |
| Tilesets | 44 | 7.91 MiB |
| BGM | 3 | 5.52 MiB |
| SE | 185 | 1.99 MiB |
| Battlers | 30 | 1.71 MiB |
| Battlebacks 1+2 | 4 | 0.88 MiB |
| Faces | 3 | 0.50 MiB |
| Characters | 9 | 0.30 MiB |
| ME/BGS | 3 | 0.12 MiB |
| **total** | **374** | **63.76 MiB** |

**The Animations row is an artefact, caught by cross-checking rather than
trusting the first number:** 93 of 93 matched files is the *entire* RTP
`Graphics/Animations` folder, which was the tell — VX Ace's
`Animations.rvdata2` conventionally keeps the full stock animation list
regardless of whether anything in the actual game triggers each entry, so
"referenced somewhere in `Data/`" overcounts for this one category
specifically. Checked by tracing actual `@animation_id` usage from
`Skills.rvdata2`/`Items.rvdata2` into `Animations.rvdata2`'s entries: `Items`
parsed cleanly and uses only **6** of the 93 (`Heal1`–`Heal6`). `Skills`
(usually the larger source of animation references in a JRPG, since it
carries combat actions) hit a `RPG::UsableItem::Damage` ivar layout the
generic walker didn't anticipate and aborted rather than silently return a
wrong number — not fixed, since this was already a secondary refinement pass
on top of a defensible upper bound, and the honest partial result already
makes the point.

**Status: informative, not a decision.** Reliable part of the RTP addition
(everything except Animations, none of which showed this over-count pattern
under the same check) is **~19 MiB**. Animations sit somewhere between "a
handful of files" (consistent with the `Items` sample) and the 44.82 MiB
ceiling — resolving that gap means fixing the `Skills.rvdata2` parse, not
re-guessing. Combined with the game's own ~82 MiB (Graphics+Data archive plus
the loose `Audio` folder), the whole port's unique packed-asset footprint is
**roughly 100–146 MiB**, not vendored anywhere per this project's usual rule.

### Indexed `.apxl`: does the one-global-palette design fit real content, and is it worth building?

**Why measured.** `ROADMAP.md`'s "Verify: is `.apxl`/APNG actually finished?"
flagged that indexed animation being "left to a future version" (`SPEC.md`
§10.1) was never a considered rejection — checked against git history, the
`.apxl` v1/v2 header froze (`f5d0de6`/`9b05ea0`, 2026-07-25/26) two days
before indexed-still support existed at all (`314bb18`, 2026-07-27), and the
still format's own design already anticipated "one global palette per `.apxl`
stream." The PSP/GE work since (`pxl_convert_palette`, `GU_PSM_T4`/`T8`) gives
it a real motive: an animated UI icon/sprite is the natural next beneficiary
of the same decode-straight-to-a-texture, zero-per-frame-conversion win
stills already have. What was missing was a corpus and a number.

**Corpus.** No existing corpus is indexed/palette content — Anita is RGBA
hand-drawn, CLIC and Kodak are photographs. `bench/gif_corpus.sh` (2026-09-18)
fetches one reproducibly from Wikimedia Commons: 66 freely-licensed animated
GIFs, 101 MB, three categories chosen for the question rather than volume —
Animated pixel art and Throbbers for the actual PSP UI/game-icon use case,
Animated diagrams as a deliberately harder stress case (denser, more
dithered/gradient content). GIF is ≤256 colours/frame by format definition,
so this is exactly the content class `.apxl` currently has no format for.

**Method.** Two separate questions, since a size win is worthless if the
precondition never holds. For each GIF: composite every frame to a full RGBA
canvas (Pillow, disposal-aware), then build **one** palette from the unique
colours across the *whole* animation (not per frame) — this is the actual
precondition APXL's "one global palette per stream" design requires. If that
exceeds 256 colours, the file cannot be losslessly represented this way and is
recorded as a miss, nothing further computed. If it fits, concatenate frames
two ways — RGBA8 bytes (what `.apxl` stores today) and 1-byte indices into the
shared palette (what an indexed `.apxl` would store) — and compress each with
`bench/rawzstd` (new tool; apxl_encode's *real* default parameters: level 12,
LDM on, `windowLog` 27), so the comparison is scored the way a real `.apxl`
file is, not an arbitrary zstd setting.

**Result 1 — the precondition mostly holds, and cleanly splits by content
class.** 50 of 66 files (76%) fit under one global ≤256-colour palette. All 16
that don't are from the Animated diagrams stress category; **zero** misses
among Animated pixel art or Throbbers — the categories that actually match the
PSP UI/game-icon use case this is aimed at. The one-global-palette design is
not a universal fit, but for the content it is actually meant for in this
project, it held on every sample measured.

**Result 2 — for files that fit, indexed compresses smaller in 49 of 50
files.** Ratio is indexed-compressed / RGBA-compressed, same pixels, same
compressor settings:

| | value |
|---|---:|
| mean ratio (per file) | 0.667 |
| pooled ratio (total bytes) | 0.679 |
| best | 0.337 (`Flinthook_animation_-_menu_bounty.gif`) |
| worst | 1.049 (`Frame_rule_bus.gif`) |

Indexed averages **roughly a third smaller** than RGBA at identical zstd
settings — on top of, not instead of, whatever LDM already buys. The one loss
is a 220-byte difference on a 4.5 KB file (4480 → 4700 bytes) — the same
shape as the tiny-shot outliers in the LDM-per-stream sweep above: noise at
that scale, not a real counter-pattern. `corr(palette_size, ratio) = -0.032`
— the win comes from byte density (1 byte/pixel vs 4), essentially independent
of how many of the 256 slots are actually used.

**Decision: build it.** Both questions this needed to settle before touching
code came back positive: the design precondition holds for the target content
class, and the size win is real, substantial, and near-universal once it
does. **Container and codec done the same day**: `.apxl` bumped to version 2
(36-byte header, `PaletteCount`/`PaletteAlphaCount`, mirroring the still
`.pxl` header), `apxl_encode`/`apxl_decode` support one global palette shared
by every frame (encode rejects a per-frame mismatch rather than silently
picking one), `tests/roundtrip.c` round-trips it byte-exact and checks the
rejection case, and 500k fuzz iterations under ASan/UBSan are clean on the
new parsing path. Turned out simpler than expected: unlike still `.pxl`,
`.apxl` has no per-frame filter stage to special-case at all — indexed frames
are just 1-byte-per-pixel rows, no `pixel_bytes`/`filter_width` trick needed.

**Also done the same day**, once the container existed: `apxl_anim_try_index`
(the encoder-side decision this section's "not done" used to name — build a
global palette from a source animation, fall back to RGBA above 256 colours),
`pxltool ca -i`, and `apng_save`/`apng_load` both handling indexed APNGs (the
latter needed a real fix — its per-frame PNG synthesis never carried PLTE
through, so indexed input, including `apng_save`'s own new output, failed to
load until that was found and fixed). Verified end-to-end on two real corpus
files (`LittleRunner.gif`, `Cloud.gif`, via GIF → APNG → `-i` → decode →
`magick compare`): **0 pixels differ**, and `Cloud.gif` lands at 37.6%
smaller, matching this section's corpus-wide number. See `ROADMAP.md`'s
"Implement indexed `.apxl`" item for the full account, including what's
still open (PSP/GE playback, `apng_load` fuzz coverage).

### Native GIF front end: built, and a real GIF-semantics ambiguity resolved against browser ground truth

**Why built.** The indexed `.apxl` measurement and implementation above used
GIF as its corpus but never decoded one directly — verification went through
ffmpeg's GIF→APNG conversion. That leaves a gap the format is explicitly
aimed at closing (indexed animation support exists *because* GIF-shaped
content is common): nothing in this project could take a `.gif` file
straight in. `src/gif.c` closes it — a hand-rolled GIF87a/89a decoder in
`pxlcore` (no libpng dependency, deliberately duplicating a small file-read
helper rather than pulling in `pxl_pngio.h`): variable-width LZW (3-12 bit
codes, LSB-first packing, prefix-chain dictionary, KwKwK special case),
4-pass deinterlacing, and dispose-then-draw compositing (GIF disposal
0/1→NONE, 2→BACKGROUND, 3→PREVIOUS) mirroring `apng_load`'s existing model.
`pxltool cg` and `tools/pxltool.c`'s `cmd_gif_compress` wire it straight into
the indexed pipeline: `gif_load` → `apxl_anim_try_index` → `apxl_encode`.

**A real ambiguity, found by testing against a 66-file real corpus, not
assumed.** GIF89a's spec text advises filling canvas area no frame ever
covers, and area a `BACKGROUND`-disposal frame clears, with the Logical
Screen Descriptor's background colour. `gif.c` originally did the simpler
thing — leave it transparent — which is what "measured, not assumed" caught:
running the full `bench/gif_corpus.sh` set (66 files) through `gif_load` and
diffing against ffmpeg's GIF→APNG reference found 3 files disagreeing on
exactly this area (`Cuve_agitees_300L_500µm`, `Dramazol`, `Ebene_1_interp`).
Reading the disposal spec text literally and implementing background-colour
fill instead "fixed" those three — but broke 3 *different* files
(`Eulerianpath_drawing_without_first_frame`, `FoyerLentilleConvergente`,
`Implosion_nuclear_weapon_design3`), where frame 0 covers the full canvas
but *with* an explicitly transparent pixel at the disputed location, and
ffmpeg still showed alpha=0 there — directly contradicting the "always fill
with background colour" reading. Two real corpus files, both scored against
the same reference, wanted opposite answers: the spec text alone could not
settle this.

**Resolved against actual rendering, not the advisory text or a second
decoder's interpretation of it.** Served two representative GIFs
(`Cuve_agitees_300L_500µm`, `FoyerLentilleConvergente`) from a local
`python3 -m http.server` and read `<canvas>.getImageData()` in Chromium via
the browser tool at the exact disputed pixel/row in each. Both came back
fully transparent — `[0,0,0,0]` — for both the "never covered" case and the
explicit-transparent-pixel case. That makes ffmpeg's GIF decoder the
non-standard outlier here: it implements GIF89a's advisory background-colour
text literally, but real-world rendering (what a `.gif` file actually looks
like to anyone who opens one) does not follow it. `gif.c`'s original,
simpler transparent-everywhere behaviour was correct; the background-colour
"fix" was reverted in full.

**Verification.** A hand-built synthetic 4×4/4-colour GIF exercising every
disposal method (`check_gif`, `tests/roundtrip.c`); a real 6-frame file
(`LittleRunner.gif`, CC BY-SA 4.0, Wikimedia Commons) end-to-end through
GIF → `.apxl` → APNG (`check_gif_real`); the full 66-file `bench/gif_corpus.sh`
batch against ffmpeg: 63/66 exact, the remaining 3 being the resolved
ffmpeg-divergence above, independently spot-checked pixel-by-pixel (every
differing pixel is transparent on this side, matching the Chromium-verified
answer) rather than taken on faith. Fuzzed separately: 26 800 mutations (67
real-corpus seeds × 400 mutations, 4 strategies — truncation, byte flips,
length-field corruption, structural insert/delete) against an ASan/UBSan
build of `gif_load` — **0 crashes**. The fuzzer flagged 169 cases as
`TIMEOUT` (5s per-case limit); checked rather than dismissed, all 169 trace
to a small set of large (1.5-7 MB) real-corpus seeds that are simply slow to
decode *unmutated* under ASan on a loaded machine (`24-cell-3CP.gif`: 6.2s
under ASan vs 0.8s in a release build) — linear cost, not an algorithmic
blowup, and not a single one paired with a sanitizer report.

**Decision: ship it as-is.** See `ROADMAP.md`'s "Implement indexed `.apxl`"
item for where this leaves the feature as a whole (PSP/GE playback and
`apng_load`'s general fuzz coverage are the remaining open pieces, unrelated
to GIF decoding itself).

### Animated indexed `.apxl` on the PSP GE: first `sceGu*` code in this project, and what headless can't confirm

**Why built.** The two remaining pieces the indexed-`.apxl` decision opened
(`ROADMAP.md`'s "PSP/GE path" and "Animated `.apxl` texture playback on
PSP") are really one piece of work: decode a `.apxl` frame sequence and
drive `sceGuDrawArray` with it. Scoped to the indexed case specifically
(`GU_PSM_T8` + one CLUT per stream, via `pxl_convert_palette`) since that's
what the corpus measurement says the real target content (UI icons,
throbbers) actually is. `psp/main.c` gained a small embedded 64×64/8-frame
indexed animation (`bench/mkpsptest.c`'s `make_anim_apxl`, a fixed
16-colour palette and a coarse block-index formula in `psp/pattern.h`,
matching the existing still-image "formula, not data" discipline), decoded
via `apxl_decode` (newly cross-compiled for this target — `src/apxl_codec.c`
was decode-only source before) and pushed through `sceGuInit` →
per-frame `sceGuTexImage`/`sceGuDrawArray` → `sceGuTerm`.

**Decode correctness: clean.** `apxl_decode`'s indexed multi-frame path —
the actually-new PXL/APXL code here, everything downstream of it is either
already-verified library code (`pxl_convert_palette`) or GE plumbing — is
bit-exact on real MIPS under `PPSSPPHeadless`: all 8 frames' index bytes and
the shared 16-entry palette match `pattern.h`'s formula exactly.

**A real bug found and fixed: `GU_TRANSFORM_2D` still needs a viewport.**
The first attempt used no `sceGuOffset`/`sceGuViewport`/`sceGuScissor` calls
at all, reasoning that "coordinate passed directly to the rasterizer" (the
GU header's own description of `GU_TRANSFORM_2D`) meant the separate
viewport/clip stage didn't apply. Wrong, checked against
`psp/sdk/samples/gu/clut/clut.c` (a real, working reference in the pspdev
toolchain, doing exactly this: an indexed `GU_SPRITES` quad) — it sets all
three even though its sprite is drawn with `GU_TRANSFORM_2D` too.
`GU_TRANSFORM_2D` only skips the vertex *transform matrix*; the clip stage
is separate and defaults to a degenerate region. Without it, every draw
rasterized to roughly one pixel; with `sceGuOffset(2048-W/2, 2048-H/2)` +
`sceGuViewport(2048,2048,W,H)` + `sceGuScissor(0,0,W,H)` matching the
reference's pattern, the full W×H quad rasterizes.

**A second real finding, useful beyond this one test: offset 0 is not free
VRAM scratch space.** The read-back target was first placed at VRAM offset
0, the address every `pspgu` sample uses for its *display* buffer
(`sceGuDrawBuffer(...,(void*)0,...)`) — and also, it turns out, where
`pspDebugScreenInit`'s own text console lives. `put()`/`putf()` (this
program's logging, called constantly) draw through the GU via
`pspDebugScreenPrintf`, and once this code's own `sceGuInit()` replaces the
GE context, that console's drawing gets funneled through *this* code's
altered state (its tiny viewport, its `T8` texture mode, its CLUT) instead
of its own — output that looks exactly like a rendering bug, not a logging
collision, until traced to it. Two independent fixes followed from this:
moving the read-back target to `ANIM_FB_STRIDE * 272 * 4` bytes into VRAM
(a full screen-buffer's width past offset 0, the same "second buffer" offset
real double-buffered code uses), and moving every diagnostic print to *after*
`sceGuTerm()` hands the GE back, never interleaved with GE-active code.

**What's still unresolved: PPSSPPHeadless does not read back bit-exact
pixels from this off-screen target, for a reason not fully identified.**
After both fixes above, per-frame read-back still shows 0% of pixels
matching `clut[index]` — not corruption *resembling* something explicable,
just disagreement. Investigated systematically rather than assumed:
- **Not an ambient/background writer**: the target reads back as exactly the
  poison pattern written into it, with zero deviation, immediately before
  the first draw call of the run.
- **Not vertex data provenance**: switching from a plain stack array (with
  manual `sceKernelDcacheWritebackRange`) to `sceGuGetMemory`-allocated
  vertices (matching the reference sample exactly) produced a bit-identical
  result.
- **Not cross-frame bleed-through**: of the wrong pixels sampled in one
  investigation pass, the large majority matched *no* frame's expected
  colour at that position, ruling out a later frame's draw landing where an
  earlier frame's read-back was expected.
- **Not GE/CPU sync timing** in any simple sense: inserting an explicit
  delay or `sceDisplayWaitVblankStart` between frames changed *which* pixels
  disagreed without making the read-back agree.
- **Is timing-sensitive**: the exact disagreement pattern did shift under
  those last two changes, which is real evidence *something* about this
  target's timing matters — just not evidence that points at a fix.

Best explanation given all of the above: something specific to
`PPSSPPHeadless`'s GE emulation when no display is ever attached (this
deliberately never calls `sceGuDisplay`, since the point was checking GE
output by reading memory, not by a screenshot) — plausible since every
`pspgu` sample this was checked against always sets up and shows a real
display buffer, an untested code path this is the first to exercise in this
project. Not confirmed, because confirming it would need instrumenting
PPSSPP itself, out of scope here.

**Decision: ship the GE code, don't claim more than headless can support.**
`run_anim_ge_correctness` (`psp/main.c`) reports the real per-frame match
percentage as data, and gates its own pass/fail on `sceGuSync` completing
without error across all 8 frames (a real, if weaker, signal: the calls are
accepted and the run completes rather than hanging or erroring) — not on
pixel equality, since this investigation could not make that reliable under
headless specifically. The viewport/scissor fix and the VRAM-offset lesson
are both real and apply regardless. Bit-exact confirmation is deferred to
real hardware, which always has a display attached and is the next step per
`psp/README.md`'s two-stage pattern (headless for correctness-shaped
smoke-testing, hardware for the number and the final pixel check).

### The PSP GE's 16-bit colour formats are B-then-R, not R-then-B — reported from a sibling project's real hardware, not yet independently reproduced here

**Not this project's own measurement.** Flagged 2026-09-18 by the parallel
session working the Mogeko Castle PSP port (a separate codebase — this
entry documents the finding for this project's own benefit, since PXL ships
code that packs pixels for the same GE, not because that project's results
belong here): rendering an image on real PSP-3008 hardware came out with
red and blue swapped, traced to the GE's `GU_PSM_5650`/`GU_PSM_5551`/
`GU_PSM_4444` formats actually being bit-laid-out blue-first (`B5G6R5`,
`B5G5R5A1`, ...), not red-first (`R5G6B5`, ...) the way `pspgu.h`'s own doc
comments name them (`GU_COLOR_5650 - 16-bit color (R5G6B5A0)`) and the way
every PSP homebrew reference this project has read assumes.

**Why this project cares.** `pxl_stream_new_ex`'s `PXL_OUTPUT_RGB565`/
`RGBA5551` row conversion and `pxl_convert_palette` (`src/pxl_codec_*.c`)
both pack red into the top bits, matching the SDK-documented, conventional
layout — the same layout `psp/main.c`'s `expected_565`/`expected_5551` cross
-checks were written against. `psp/README.md` already claims this "matched
on the PSP-3008" (2026-09-18) — that claim is still true as far as it goes,
but it only checked that the MIPS build's packing is *internally
consistent* (same bytes as the x86 reference formula), never that those
bytes, once hardware-sampled by the GE and actually displayed, show the
colours a caller intended. If the channel-order finding above holds, PXL's
current RGB565/RGBA5551/RGBA4444 output is silently red/blue-swapped
whenever the GE samples and displays it — a real, if invisible-until-now,
gap between "verified" and "correct."

**Not acted on yet.** Per explicit instruction: no format change. `.pxl`'s
packed-format bytes stay red-first — changing that would be a breaking
container change on the strength of one secondhand report, exactly the kind
of thing this project's whole discipline exists to avoid. What *should*
happen before this is trusted further: reproduce it independently, on this
project's own test content, on real PSP-3008 hardware, the same "measured,
not assumed" way every other claim here is checked — the still-open item is
in `ROADMAP.md`'s Held section.

### Grayscale sources in the PSP-native output formats: a real gap, closed

**Found by actual use, not by review.** 2026-09-20, the Mogeko Castle PSP
port reported two real assets (`logo.png`, a Japanese warning label) coming
out invisible on their GE texture path — not a crash, just nothing drawn.
Traced to `pxl_stream_new_ex`'s `PXL_OUTPUT_RGB565`/`RGBA5551`/`RGBA4444`
conversion: `pxl_stream_feed` rejected both outright, since both PNGs are
grayscale (1 channel, no palette), and the geometry check gated packed-
format conversion to 3- or 4-channel sources only. The caller's own code
didn't crash on the rejection, it just silently fell back to a stub — so
the failure was invisible twice over, once in the library and once in the
caller.

**Was this ever actually undefined, or just unimplemented?** Checked
`convert_row` (`src/pxl_codec_decode.c`) rather than assumed: the geometry
check exists specifically *because* the function's R/G/B extraction
(`p[0]`, `p[1]`, `p[2]`) would read past a 1-byte-per-pixel row into the
next pixel's byte as if it were the green/blue channel — genuinely wrong
output, not just untested. So the rejection was protecting against a real
bug in the conversion, not an arbitrary restriction; the fix has to be in
`convert_row` itself, not just a relaxed check in front of unchanged code.

**The expansion is unambiguous.** Grayscale-to-RGB is R=G=B=the intensity
sample — the same thing libpng does for `PNG_COLOR_TYPE_GRAY`, no design
choice to make. Grayscale+alpha (2 channels) is the same with the source's
own second byte as alpha, distinct from the "no alpha channel of its own"
case (1 or 3 channels), which still reads fully opaque — the same
distinction `convert_row` already drew between 3- and 4-channel sources,
extended by one case rather than replaced.

**Decision: extend, not work around.** Fixed in `convert_row` (unified
R/G/B/A extraction per channel count: 1→gray gray gray opaque, 2→gray gray
gray alpha, 3→as before, 4→as before, one code path per output format
instead of duplicated per-format channel branches) and the geometry check
in `pxl_stream_new_ex`'s `stream_start` (now `bit_depth==8 &&
palette_count==0`, no channel-count restriction — indexed sources still
correctly excluded, since their samples are palette indices, not
intensity, regardless of channel count; `pxl_convert_palette` is the
dedicated indexed path). `.pxl`'s on-disk format is untouched — grayscale
still stores as 1 byte per pixel, no size regression; the expansion only
happens transiently in the row buffer handed to a caller. Alternative
considered: push the grayscale-to-RGB expansion into each caller (encode
already-expanded RGB `.pxl` files instead). Rejected — that fixes one
project at a time instead of the actual gap, and this project's whole
pitch for `PXL_OUTPUT_*` is that callers shouldn't have to hand-roll
texture-format conversion themselves.

**Verification.** `check_output_format_rejects_bad_geometry`
(`tests/roundtrip.c`) used to assert a 1-channel source *must* be rejected
— that assertion is now backwards, so it was retargeted at a genuinely
still-undefined geometry (16-bit-per-channel) instead of retired, so the
geometry-check machinery itself stays covered. Two new tests added:
`check_output_format_gray` (all four output formats, gray expanded to
R=G=B, opaque alpha) and `check_output_format_gray_alpha` (RGBA5551, real
alpha from the source's own second byte, not the opaque default) — both
clean under ASan/UBSan alongside the full existing suite.

**Spot-checked against real content, same day**: every PNG under
`/usr/share/icons` (5591 files, all installed themes — Adwaita, hicolor,
HighContrast, breeze, ...), deduplicated by content hash, filtered to
genuinely non-indexed 1-/2-channel sources (PIL `L`/`LA` — most
HighContrast icons turned out to be indexed (`P` mode) despite looking
monochrome, so this is a narrower slice than "the whole HighContrast
theme"). 256 unique files (`L`: 2, `LA`: 254) run through `pxl_encode` →
`pxl_stream_new_ex(PXL_OUTPUT_RGBA8888)`, checked against an expected
RGBA8888 buffer computed independently in Python (not reusing any PXL
code, so this isn't just the library agreeing with itself) — **256/256
bit-exact**. Not committed as a corpus or a script (nothing to fetch;
reproduce by pointing the same check at any installed icon theme), same
convention the 2026-09-15 dictionary-on-icons entry above used.

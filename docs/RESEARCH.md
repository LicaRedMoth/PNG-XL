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

### Sub-8-bit grayscale is expanded to 8-bit, and it need not be
**The container allows it** — SPEC 2.1 says depths 1/2/4 are meaningful "for
indexed and grayscale images" — but `pxl_png.c` calls
`png_set_expand_gray_1_2_4_to_8` on load, and the writer says outright that
sub-byte gray has no colour type of its own there. The format can express it;
the codec never produces it.

Cost, measured on a 1200x1600 bilevel document, the same image offered both ways:

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

**What would give 10-bit its actual value instead: keep `sBIT`.** PNG's
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

### Decode-time memory — the priority that actually binds
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

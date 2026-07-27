# PXL (PNG XL) Format Specification

Version 1 · lossless raster image container

This document defines the `.pxl` byte format precisely enough to write an
independent encoder or decoder without reference to the libpxl source. It is
implementation-agnostic: it says nothing about libpng, which is only one
possible source/sink of pixels.

---

## 1. Overview

A `.pxl` file stores a single raster image losslessly. Encoding is:

1. **Filter** the raw, tightly packed pixels with a reversible transform
   (section 5) to expose spatial/color redundancy.
2. **Compress** the filtered bytes with a single Zstandard frame (RFC 8878).

Decoding reverses both steps and always reproduces the original samples
bit-for-bit. There is no lossy mode.

All integers in the container header are **little-endian** and unaligned; a
conforming parser MUST read them byte-by-byte. (Byte order *inside* pixel
samples is defined separately in section 4.)

---

## 2. Container layout

```
+--------------------+  offset 0
|      Header        |  28 bytes
+--------------------+
|  Palette section   |  PaletteCount*3 + PaletteAlphaCount bytes (may be 0)
+--------------------+
|   Metadata block   |  MetaByteCount bytes (may be 0)
+--------------------+
|    Zstd frame      |  to end of file
+--------------------+
```

### 2.1 Header

| Offset | Size | Field           | Notes |
|-------:|-----:|-----------------|-------|
| 0  | 4 | `Magic`           | ASCII `"PXL1"` = `0x50 0x58 0x4C 0x31` |
| 4  | 1 | `Version`         | `1` |
| 5  | 1 | `Channels`        | `1`=Gray **or indexed**, `2`=Gray+Alpha, `3`=RGB, `4`=RGBA |
| 6  | 1 | `BitDepth`        | bits per channel: `1`, `2`, `4`, `8` or `16` |
| 7  | 1 | `ColorFilter`     | `0`=DELTA, `1`=BCIF, `2`=ADAPTIVE (section 5) |
| 8  | 4 | `Width`           | pixels, uint32 LE, ≥ 1 |
| 12 | 4 | `Height`          | pixels, uint32 LE, ≥ 1 |
| 16 | 4 | `RawByteCount`    | uint32 LE, size of the **filtered** stream (= decompressed frame size) |
| 20 | 4 | `MetaByteCount`   | uint32 LE, size of the metadata block (may be 0) |
| 24 | 2 | `PaletteCount`      | uint16 LE, `0` = not indexed, else `1..256` entries |
| 26 | 2 | `PaletteAlphaCount` | uint16 LE, `0..PaletteCount` |

The `Magic` identifies the container family; the `Version` byte exists so a
future revision can change this layout. This is the only version defined; a
decoder MUST reject any other `Version` value.

`Channels == 1` with `PaletteCount > 0` means **indexed color**: each sample is
a palette index, not a gray level. Sub-byte `BitDepth` values (`1`, `2`, `4`)
are only meaningful for indexed and grayscale images, matching PNG.

### 2.2 Invariants a decoder MUST enforce

- `Magic` equals `"PXL1"`.
- `Version` == 1.
- `Channels` ∈ {1, 2, 3, 4}.
- `BitDepth` ∈ {1, 2, 4, 8, 16}; values below 8 require `Channels == 1`.
- `ColorFilter` ∈ {0, 1, 2}; `1` (BCIF) requires `BitDepth == 8` and
  `Channels ∈ {3, 4}`.
- `PaletteCount` ≤ 256; if nonzero then `Channels == 1`.
- `PaletteAlphaCount` ≤ `PaletteCount`.
- `RowBytes = ceil(Width × Channels × BitDepth / 8)`, and `RowBytes × Height`
  is nonzero.
- `RawByteCount` equals the filtered-stream size for the chosen filter:
  - DELTA / BCIF: `RowBytes × Height`;
  - ADAPTIVE: `Height × (1 + RowBytes)`.
- The zstd frame decompresses to exactly `RawByteCount` bytes.

If any invariant fails, the file is invalid and MUST be rejected.

### 2.3 Palette section

Present only when `PaletteCount > 0`, immediately after the header:

```
PaletteBytes = PaletteCount * 3 + PaletteAlphaCount
```

- First `PaletteCount × 3` bytes: entries as `R, G, B` triplets, in index order.
- Then `PaletteAlphaCount` bytes: alpha for entries `0 .. PaletteAlphaCount-1`.
  Entries beyond that are fully opaque (255). This mirrors PNG `tRNS` for
  color type 3, which may be shorter than `PLTE`.

The palette is **structural, not metadata**: an indexed frame cannot be decoded
to color without it, so it is stored in the container rather than in the opaque
metadata block. It is stored uncompressed (at most 1024 bytes).

### 2.4 Frame offset

```
FrameOffset = 28 + PaletteBytes + MetaByteCount
```
The zstd frame occupies `[FrameOffset, EndOfFile)`.

---

## 3. Metadata block

The metadata block is an opaque, application-defined region reserved for
carrying source-format side data (for the PNG source: ancillary chunks such as
EXIF, ICC profile, HDR signalling, and text). It is **not** compressed and is
**not** required to decode the image; a decoder that does not care about
metadata skips `MetaByteCount` bytes.

### 3.1 Encoding used by this project

libpxl serializes preserved PNG ancillary chunks as a flat sequence of records:

```
record := ChunkType[4] · DataLength[4, LE] · Data[DataLength]
```

- `ChunkType` is the raw 4-byte PNG chunk type code (e.g. `eXIf`, `iCCP`,
  `cICP`, `gAMA`, `tEXt`).
- Records repeat until the block is consumed.

`PLTE` and `tRNS` are **not** stored here: for indexed images they are carried
structurally in the palette section (section 2.3), which is what makes the
indexed round-trip lossless. `sBIT`, `bKGD` and `hIST` are **not** stored,
because they are tied to a pixel layout a front end may canonicalize.
Structural chunks (`IHDR`, `IDAT`, `IEND`) are never stored.

A different application MAY define its own metadata-block contents; the
container only mandates its length via `MetaByteCount`.

---

## 4. Pixel model

- Pixels are row-major, top-to-bottom, left-to-right, tightly packed. Row
  stride is `RowBytes = ceil(Width × Channels × BitDepth / 8)`.
- For `BitDepth ∈ {1, 2, 4}` samples are bit-packed **MSB first** within each
  byte and every row is padded to a whole byte, exactly as in PNG. Padding bits
  at the end of a row are written as 0 and MUST be ignored on read. For
  `BitDepth ≥ 8` there is no padding.
- Channel order is natural: `G`, `GA` (gray, alpha), `RGB`, `RGBA`. For indexed
  images the single channel holds palette indices, which MUST be
  `< PaletteCount`.
- For `BitDepth == 16`, each sample is stored **big-endian**
  (most significant byte first), matching PNG's native sample order. This is
  fixed by the format and independent of host endianness, so files are
  portable and 16-bit round-trips are bit-exact.

The "raw pixel buffer" referenced by the filters below is this exact byte
layout, of length `RawByteCount`.

---

## 5. Filters

`PixelBytes = max(1, Channels × BitDepth / 8)` (1..8). For sub-8-bit depths
`PixelBytes` is 1, so filters operate on packed bytes rather than on samples —
the same rule PNG uses for its `bpp` offset.

Both filters are applied to the raw pixel buffer to produce a filtered buffer
of the same length, which is then zstd-compressed. Decoding decompresses first,
then inverts the filter. The encoder MAY try multiple filters and pick the
smallest; the chosen one is recorded in `ColorFilter`.

Filter arithmetic is performed on **8-bit lanes** with wraparound modulo 256
(`uint8_t` add/subtract). For 16-bit images (`BitDepth == 16`) the two
bytes of each sample are treated as independent 8-bit lanes of `PixelBytes`;
i.e. the delta filter operates byte-wise. This is lossless because the inverse
uses the same modular arithmetic.

### 5.1 DELTA (ColorFilter = 0) — any format

For every row independently, keep a per-lane running `prev`, initialized to 0
at the start of each row. For each pixel, for each lane `i` in `0..PixelBytes`:

```
Encode:  out[i] = (in[i] - prev[i]) mod 256;  prev[i] = in[i]
Decode:  in[i]  = (out[i] + prev[i]) mod 256; prev[i] = in[i]
```

Output stays in interleaved pixel order (no plane split).

### 5.2 BCIF (ColorFilter = 1) — 8-bit RGB and RGBA only

Permitted only when `BitDepth == 8` and `Channels ∈ {3, 4}`. Combines a
left-neighbor delta, a reversible color transform (from BCIF), and a split into
separate color planes. `PlaneSize = Width × Height`. Output planes are
concatenated: Y-plane, then U-plane, then V-plane (, then A-plane for RGBA).

Per row, `prev[]` (3 or 4 lanes) resets to 0. For each pixel with source
`R,G,B(,A)`:

```
Encode (per pixel):
    r = (R - prev[0]) mod 256
    g = (G - prev[1]) mod 256
    b = (B - prev[2]) mod 256
    prev[0]=R; prev[1]=G; prev[2]=B
    Y = b
    U = (g - b) mod 256
    V = (g - r) mod 256
    emit Y->Yplane, U->Uplane, V->Vplane
    # RGBA only:
    a = (A - prev[3]) mod 256;  prev[3]=A;  emit a->Aplane
```

```
Decode (per pixel, reading one byte from each plane):
    B_  = Y
    G_  = (U + B_) mod 256
    r   = (G_ - V) mod 256
    g   = G_
    b   = B_
    R = (r + prev[0]) mod 256
    G = (g + prev[1]) mod 256
    B = (b + prev[2]) mod 256
    prev[0]=R; prev[1]=G; prev[2]=B
    # RGBA only:
    A = (a + prev[3]) mod 256;  prev[3]=A
    write R,G,B(,A)
```

This transform is exactly reversible; it originates from the Zpng project.

### 5.3 ADAPTIVE (ColorFilter = 2) — any format

Per-row PNG-style filtering. This is the only filter whose output size differs
from the raw pixels: for each row it emits **one filter-type byte** followed by
the filtered row of `RowStride = Width × PixelBytes` bytes. Total filtered size
is `Height × (1 + RowStride)`.

Filtering is byte-wise over the row. For output byte index `i` in a row:

- `a` = the reconstructed byte `PixelBytes` positions to the left in the same
  row (`0` if `i < PixelBytes`);
- `b` = the byte at index `i` in the row above (`0` for the first row);
- `c` = the byte `PixelBytes` positions to the left in the row above (`0` if
  `i < PixelBytes` or first row).

The filter-type byte selects the predictor (identical to PNG):

| Type | Name    | Predictor `p` |
|-----:|---------|---------------|
| 0 | None    | `0` |
| 1 | Sub     | `a` |
| 2 | Up      | `b` |
| 3 | Average | `(a + b) >> 1` (integer, no rounding) |
| 4 | Paeth   | `PaethPredictor(a, b, c)` |

```
Encode:  out[i] = (cur[i] - p) mod 256
Decode:  cur[i] = (in[i]  + p) mod 256   # a,b,c read from already-decoded bytes
```

`PaethPredictor(a,b,c)`: let `p = a + b − c`; return whichever of `a`, `b`, `c`
has the smallest `|p − ·|`, breaking ties in the order `a, b, c` (per the PNG
spec).

The encoder MAY choose the per-row filter type freely (libpxl uses PNG's
minimum-sum-of-absolute-residuals heuristic). A decoder MUST honor the stored
type byte for each row and MUST reject any type byte greater than 4.

### 5.4 Progressive (top-to-bottom) decodability

`ColorFilter` also determines whether a file can be decoded **incrementally,
row by row, as its bytes arrive** — the property a web client needs to paint an
image while it downloads:

| ColorFilter | Row-progressive | Why |
|---|---|---|
| 0 DELTA | yes | `prev` resets per row, so every row is self-contained |
| 2 ADAPTIVE | yes | a row depends only on the row above, already reconstructed |
| 1 BCIF | **no** | the plane split interleaves all rows; no row is complete until the last plane byte arrives |

A decoder MAY therefore reconstruct row `y` of a DELTA or ADAPTIVE file as soon
as the filtered bytes for that row have been decompressed, without waiting for
the rest of the zstd frame. Granularity is the zstd block (at most 128 KiB of
decompressed output), not the byte.

An encoder that wants to guarantee this property MUST restrict itself to
`ColorFilter ∈ {0, 2}`. This is a purely encoder-side choice: no header field
or container change signals it, and decoders need no special mode. libpxl
exposes it as `PXL_ENCODE_PROGRESSIVE` (`pxltool c … -p`).

---

## 6. Compression

The filtered buffer is compressed into exactly one Zstandard frame (RFC 8878),
written verbatim as the file's frame region. Any zstd compression level MAY be
used; it does not affect the decoded result. Decoders MUST accept any valid
zstd frame that decompresses to `RawByteCount` bytes.

---

## 7. Decoder algorithm (normative summary)

1. Read and validate the 24-byte header (sections 2.1–2.2), including
   `MetaByteCount`.
2. Compute `FrameOffset` (section 2.3). Optionally copy the metadata block.
3. Zstd-decompress `[FrameOffset, EOF)` into a buffer of `RawByteCount` bytes;
   fail if the produced size differs.
4. Invert the filter named by `ColorFilter` (section 5) into the raw pixel
   buffer.
5. Interpret the pixels per section 4.

---

## 8. Versioning

Only `Version == 1` is defined. The byte exists so a future revision can change
the layout without a new magic; until then decoders MUST reject other values.

---

## 9. Conformance notes

- Multi-byte header fields are little-endian; 16-bit pixel samples are
  big-endian. These are independent and both are mandatory.
- Filter math is modulo 256 on 8-bit lanes; 16-bit images are filtered
  byte-wise. Implementations MUST use wraparound arithmetic to stay lossless.
- The metadata block is optional to interpret but its length is authoritative
  for locating the frame.

---

## 10. Animated PXL (`.apxl`), version 1

`.apxl` stores a lossless animation as a canvas plus N full-canvas frames. All
frames' pixels are concatenated in order and compressed as **one** zstd frame
with long-distance matching enabled, so the compressor exploits the (usually
large) redundancy between frames. Per-frame timing is kept in a small table.

This is a deliberate design: measurements on real animations showed that
per-frame streams, temporal deltas, and per-frame spatial filtering all
compress *worse* than a single cross-frame stream, because they destroy the
byte-level matches between successive frames.

Only `Version == 1` is defined for `.apxl`; decoders MUST reject any other
value.

### 10.1 File header (32 bytes, little-endian)

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| 0  | 4 | `Magic` | ASCII `"APXL"` = `0x41 0x50 0x58 0x4C` |
| 4  | 1 | `Version` | `1` |
| 5  | 1 | `Channels` | 1..4 (canonically 4 = RGBA) |
| 6  | 1 | `BytesPerChannel` | 1 or 2 |
| 7  | 1 | `Flags` | reserved, 0 |
| 8  | 4 | `CanvasWidth` | uint32 LE |
| 12 | 4 | `CanvasHeight` | uint32 LE |
| 16 | 4 | `FrameCount` | uint32 LE, ≥ 1 |
| 20 | 4 | `LoopCount` | uint32 LE, 0 = infinite |
| 24 | 4 | `MetaByteCount` | uint32 LE |
| 28 | 4 | `RawByteCount` | uint32 LE = `FrameCount × canvas_bytes` |

`canvas_bytes = CanvasWidth × CanvasHeight × Channels × BytesPerChannel`.

APXL version `1` carries no palette and no sub-byte depths: every frame is
byte-aligned with 8 or 16 bits per sample. An indexed still image is expanded to
`RGB`/`RGBA` before it enters an animation, so `canvas_bytes` above stays exact.
Indexed animation is left to a future version.

### 10.2 Body

```
+------------------------+ offset 32
|  metadata block        |  MetaByteCount bytes (may be 0; §3 convention)
+------------------------+
|  FrameTiming[N]        |  N × 4 bytes: DelayNum(2 LE) + DelayDen(2 LE)
+------------------------+
|  zstd frame            |  decompresses to RawByteCount bytes
+------------------------+
```

The zstd frame decompresses to `FrameCount` full-canvas frames laid out
back-to-back, each `canvas_bytes` long, in the pixel model of section 4.
`DelayDen == 0` is treated as 100 (per APNG timing).

### 10.3 Playback / decode

Decompress the single zstd frame into the `RawByteCount` buffer (a decoder
should raise its zstd window-log limit to match the encoder's, which uses up to
2^27). Frame `i` is the slice `[i × canvas_bytes, (i+1) × canvas_bytes)`. Each
frame is already a full canvas and is bit-exact.

APNG interop (loading an APNG into frames, or writing frames back as APNG) is a
front-end concern and is not part of this container definition.

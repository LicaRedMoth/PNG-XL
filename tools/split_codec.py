#!/usr/bin/env python3
"""One-shot refactor: split src/pxl_codec.c into common/encode/decode.

Parses top-level blocks, assigns each to a destination file by name, and
rewrites the three translation units. Deleted after use.
"""
import re
import sys

SRC = "src/pxl_codec.c"

# name -> destination
DEST = {
    "pxl_bit_depth": "c", "pxl_palette_count": "c", "pxl_is_indexed": "c",
    "pxl_row_bytes": "c", "geometry_ok": "c", "geometry_of": "c",
    "paeth": "c", "adaptive_filtered_size": "c", "filtered_size": "c",
    "sample_at": "c", "pxl_image_expand": "c", "pxl_free": "c",
    "pxl_image_free": "c", "pxl_version": "c",

    "palette_ok": "e", "pack_delta": "e", "pack_bcif3": "e", "pack_bcif4": "e",
    "rowfilter_encode": "e", "row_score": "e", "pack_adaptive": "e",
    "choose_filter": "e", "apply_filter": "e", "pxl_encode": "e",
    "pxl_encode_ex": "e",

    "row_indices_ok": "d", "indices_ok": "d", "unpack_delta_row": "d",
    "unpack_delta": "d", "unpack_bcif3": "d", "unpack_bcif4": "d",
    "rowfilter_decode": "d", "unpack_adaptive": "d", "reverse_filter": "d",
    "pxl_decode": "d", "stream_emit_rows": "d", "stream_begin": "d",
    "pxl_stream_new": "d", "pxl_stream_feed": "d", "pxl_stream_image": "d",
    "pxl_stream_finish": "d", "pxl_stream_free": "d",
}

DEF_RE = re.compile(r"^(?:static\s+)?[A-Za-z_][\w \t*]*?([A-Za-z_]\w*)\s*\(")

lines = open(SRC).read().split("\n")

# Locate every top-level definition: a matching line whose body ends at the
# first line that is exactly "}" at column 0.
blocks = []  # (name, start_idx, end_idx) 0-based inclusive
i = 0
while i < len(lines):
    m = DEF_RE.match(lines[i])
    if m and not lines[i].lstrip().startswith(("#", "/*", "*", "//")):
        name = m.group(1)
        if name in DEST:
            j = i
            while j < len(lines) and lines[j] != "}":
                j += 1
            if j >= len(lines):
                sys.exit("unterminated block: " + name)
            blocks.append([name, i, j])
            i = j + 1
            continue
    i += 1

found = {b[0] for b in blocks}
missing = set(DEST) - found
if missing:
    sys.exit("not found: " + ", ".join(sorted(missing)))
print("blocks:", len(blocks))

# Extend each block backwards to absorb its leading comments/blank gap: the
# gap starts right after the previous block's last line.
prev_end = blocks[0][1] - 1  # original prologue is replaced per-file
for b in blocks:
    b.append(prev_end + 1)  # gap_start
    prev_end = b[2]

# PXL_ROWF_* move to pxl_codec_internal.h (both halves need them).
DROP = re.compile(r"^#define PXL_ROWF_\w+\s")

out = {"c": [], "e": [], "d": []}
for name, start, end, gap in blocks:
    body = [ln for ln in lines[gap:end + 1] if not DROP.match(ln)]
    out[DEST[name]].extend(body + [""])

text = {k: "\n".join(v).rstrip("\n") + "\n" for k, v in out.items()}

# Shared helpers lose `static` and gain the pxl_ prefix.
for k in text:
    t = text[k]
    t = t.replace("static int geometry_of(", "int pxl_geometry_of(")
    t = t.replace("static size_t filtered_size(", "size_t pxl_filtered_size(")
    t = t.replace("static uint8_t paeth(", "uint8_t pxl_paeth(")
    t = re.sub(r"\bgeometry_of\(", "pxl_geometry_of(", t)
    t = re.sub(r"(?<!_)\bfiltered_size\(", "pxl_filtered_size(", t)
    t = re.sub(r"(?<![_\w])paeth\(", "pxl_paeth(", t)
    t = t.replace("int pxl_pxl_geometry_of(", "int pxl_geometry_of(")
    t = t.replace("size_t pxl_pxl_filtered_size(", "size_t pxl_filtered_size(")
    t = t.replace("uint8_t pxl_pxl_paeth(", "uint8_t pxl_paeth(")
    text[k] = t

# adaptive_filtered_size stays static in common; decode goes through the
# public wrapper instead.
text["d"] = text["d"].replace(
    "adaptive_pxl_filtered_size(width, height, pixel_bytes)",
    "pxl_filtered_size(PXL_FILTER_ADAPTIVE, width, height, pixel_bytes)")
text["c"] = text["c"].replace("adaptive_pxl_filtered_size",
                              "adaptive_filtered_size")

for k, path in (("c", "src/pxl_codec_common.c"), ("e", "src/pxl_codec_encode.c"),
                ("d", "src/pxl_codec_decode.c")):
    open(path, "w").write(text[k])
    print(path, len(text[k].split("\n")), "lines")


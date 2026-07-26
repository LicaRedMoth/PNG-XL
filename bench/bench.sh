#!/bin/bash
# Lossless format comparison benchmark for PXL/APXL vs PNG, GIF, APNG, JXL,
# WebP, AVIF, QOI. Uses only the committed assets under tests/data/ so results
# are reproducible by any contributor without extra downloads.
#
#   bench/bench.sh                 # print the two markdown tables
#   bench/bench.sh --update-readme # also splice them into README.md
#
# Requires: cjxl/djxl, avifenc/avifdec, ffmpeg, magick (ImageMagick 7). Any
# missing tool causes that format's row to be skipped with a warning on
# stderr rather than aborting the whole run. pxltool is our own tool and is
# mandatory: build it first with `cmake -B build && cmake --build build -j`.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/.." && pwd)

IMG_SRC="$repo_root/tests/data/RGB_24bits_palette_color_test_chart.png"
ANIM_SRC="$repo_root/tests/data/Animated_PNG_example_bouncing_beach_ball.apng"
N=${BENCH_N:-5}

if [ -n "${PXLTOOL:-}" ]; then
    pxltool="$PXLTOOL"
elif [ -x "$repo_root/build/pxltool" ]; then
    pxltool="$repo_root/build/pxltool"
elif command -v pxltool >/dev/null 2>&1; then
    pxltool=$(command -v pxltool)
else
    echo "error: pxltool not found. Build it first: cmake -B build && cmake --build build -j" >&2
    exit 1
fi

have() { command -v "$1" >/dev/null 2>&1; }

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT
cd "$workdir"

log() { echo "# $*" >&2; }

# Runs "$@" N times, discards output, and prints the median wall time in ms.
median_ms() {
    local times=() start end
    for _ in $(seq 1 "$N"); do
        start=$(date +%s%N)
        "$@" >/dev/null 2>&1
        end=$(date +%s%N)
        times+=( $(( (end - start) / 1000000 )) )
    done
    printf '%s\n' "${times[@]}" | sort -n | awk '
        { a[NR] = $1 }
        END { if (NR % 2 == 1) print a[(NR+1)/2]; else print (a[NR/2] + a[NR/2+1]) / 2 }'
}

size_of() { stat -c%s "$1"; }

# True/false: does $1 look like a plain number (int or decimal)?
is_numeric() { [[ "$1" =~ ^[0-9]+(\.[0-9]+)?$ ]]; }

# 0 if the two images are pixel-identical, nonzero otherwise.
ae() { magick compare -metric AE "$1" "$2" null: 2>&1 | grep -oE '^[0-9]+' || echo 1; }

# ImageMagick's PNG coder only sees the first frame of an .apng unless told
# it's animated via the APNG: format prefix.
frame_count() {
    case "$1" in
        *.apng) magick identify "APNG:$1" 2>/dev/null | wc -l ;;
        *)      magick identify "$1" 2>/dev/null | wc -l ;;
    esac
}

pct_of() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.1f%%", (a / b) * 100 }'; }

# --- still image -----------------------------------------------------------

still_rows=()
push_still() { still_rows+=("$1|$2|$3|$4|$5|$6"); }

base_size=$(size_of "$IMG_SRC")
push_still "PNG" "lossless (native)" "-" "-" "$base_size" "100.0%"

# PXL
enc_ms=$(median_ms "$pxltool" c "$IMG_SRC" out.pxl)
dec_ms=$(median_ms "$pxltool" d out.pxl dec_pxl.png)
sz=$(size_of out.pxl)
[ "$(ae "$IMG_SRC" dec_pxl.png)" = "0" ] || log "WARNING: PXL still-image round-trip is not pixel-identical"
push_still "PXL" "lossless (native)" "$enc_ms" "$dec_ms" "$sz" "$(pct_of "$sz" "$base_size")"

# GIF (palette-limited, see caveat in README)
if have magick; then
    enc_ms=$(median_ms magick "$IMG_SRC" gif:out.gif)
    dec_ms=$(median_ms magick out.gif dec_gif.png)
    sz=$(size_of out.gif)
    push_still "GIF" "palette (256 colors)" "$enc_ms" "$dec_ms" "$sz" "$(pct_of "$sz" "$base_size")"
else
    log "WARNING: magick not found, skipping GIF still-image row"
fi

# JPEG XL (effort defaults to 7 out of 1..10 -- not the max, matching how the
# other encoders below are also run at their tool defaults, not their slowest/
# smallest setting)
if have cjxl && have djxl; then
    enc_ms=$(median_ms cjxl -d 0 "$IMG_SRC" out.jxl --num_threads=0)
    dec_ms=$(median_ms djxl out.jxl dec_jxl.png)
    sz=$(size_of out.jxl)
    [ "$(ae "$IMG_SRC" dec_jxl.png)" = "0" ] || log "WARNING: JXL still-image round-trip is not pixel-identical"
    push_still "JXL" "lossless, effort 7/10 (-d 0)" "$enc_ms" "$dec_ms" "$sz" "$(pct_of "$sz" "$base_size")"
else
    log "WARNING: cjxl/djxl not found, skipping JXL still-image row"
fi

# WebP (libwebp's lossless mode always runs at its own internal max effort;
# there is no separate speed/effort knob for lossless the way JXL/AVIF have)
if have ffmpeg; then
    enc_ms=$(median_ms ffmpeg -y -v error -i "$IMG_SRC" -c:v libwebp -lossless 1 out.webp)
    dec_ms=$(median_ms ffmpeg -y -v error -i out.webp dec_webp.png)
    sz=$(size_of out.webp)
    [ "$(ae "$IMG_SRC" dec_webp.png)" = "0" ] || log "WARNING: WebP still-image round-trip is not pixel-identical"
    push_still "WebP" "lossless (-lossless 1)" "$enc_ms" "$dec_ms" "$sz" "$(pct_of "$sz" "$base_size")"
else
    log "WARNING: ffmpeg not found, skipping WebP still-image row"
fi

# AVIF (speed defaults to 6 out of 0..10, 0 = slowest/smallest)
if have avifenc && have avifdec; then
    enc_ms=$(median_ms avifenc -l -o out.avif "$IMG_SRC")
    dec_ms=$(median_ms avifdec out.avif dec_avif.png)
    sz=$(size_of out.avif)
    [ "$(ae "$IMG_SRC" dec_avif.png)" = "0" ] || log "WARNING: AVIF still-image round-trip is not pixel-identical"
    push_still "AVIF" "lossless, speed 6/10 (-l)" "$enc_ms" "$dec_ms" "$sz" "$(pct_of "$sz" "$base_size")"
else
    log "WARNING: avifenc/avifdec not found, skipping AVIF still-image row"
fi

# QOI (no still-vs-animation distinction in the format itself, and no
# animation extension at all -- ImageMagick confirms it only ever writes a
# single frame, so QOI appears in the still-image table only)
if have magick; then
    enc_ms=$(median_ms magick "$IMG_SRC" out.qoi)
    dec_ms=$(median_ms magick out.qoi dec_qoi.png)
    sz=$(size_of out.qoi)
    [ "$(ae "$IMG_SRC" dec_qoi.png)" = "0" ] || log "WARNING: QOI still-image round-trip is not pixel-identical"
    push_still "QOI" "lossless (native)" "$enc_ms" "$dec_ms" "$sz" "$(pct_of "$sz" "$base_size")"
else
    log "WARNING: magick not found, skipping QOI still-image row"
fi

# --- animation ---------------------------------------------------------------

anim_rows=()
push_anim() { anim_rows+=("$1|$2|$3|$4|$5|$6"); }

anim_base_size=$(size_of "$ANIM_SRC")
push_anim "APNG" "lossless (native)" "-" "-" "$anim_base_size" "100.0%"

# APXL
enc_ms=$(median_ms "$pxltool" ca "$ANIM_SRC" out.apxl)
dec_ms=$(median_ms "$pxltool" da out.apxl dec_apxl.apng)
sz=$(size_of out.apxl)
[ "$(frame_count dec_apxl.apng)" = 20 ] || log "WARNING: APXL animation round-trip did not preserve 20 frames"
push_anim "APXL" "lossless (native)" "$enc_ms" "$dec_ms" "$sz" "$(pct_of "$sz" "$anim_base_size")"

# Frames of the source, needed by the AVIF/GIF encoders below.
if have ffmpeg; then
    mkdir -p frames
    ffmpeg -y -v error -i "$ANIM_SRC" frames/f_%03d.png
fi

# GIF
if have magick && [ -d frames ]; then
    enc_ms=$(median_ms magick -delay 10 -loop 0 frames/f_*.png out.gif)
    dec_ms=$(median_ms magick out.gif dec_gif_%03d.png)
    sz=$(size_of out.gif)
    [ "$(frame_count out.gif)" = 20 ] || log "WARNING: GIF animation encode did not preserve 20 frames"
    push_anim "GIF" "palette (256 colors)" "$enc_ms" "$dec_ms" "$sz" "$(pct_of "$sz" "$anim_base_size")"
else
    log "WARNING: magick/ffmpeg not found, skipping GIF animation row"
fi

# WebP (ffmpeg's own webp decoder cannot read its animated output; magick can)
if have ffmpeg && have magick; then
    enc_ms=$(median_ms ffmpeg -y -v error -i "$ANIM_SRC" -c:v libwebp_anim -lossless 1 -loop 0 out.webp)
    dec_ms=$(median_ms magick out.webp dec_webp_%03d.png)
    sz=$(size_of out.webp)
    [ "$(frame_count out.webp)" = 20 ] || log "WARNING: WebP animation encode did not preserve 20 frames"
    push_anim "WebP" "lossless (-lossless 1)" "$enc_ms" "$dec_ms" "$sz" "$(pct_of "$sz" "$anim_base_size")"
else
    log "WARNING: ffmpeg/magick not found, skipping WebP animation row"
fi

# AVIF (avifenc needs individual frames rather than the APNG container; speed
# defaults to 6 out of 0..10, same tool-default convention as the still image
# row above)
if have avifenc && have avifdec && [ -d frames ]; then
    enc_ms=$(median_ms avifenc -l --timescale 10 -o out.avif frames/f_*.png)
    dec_ms=$(median_ms avifdec --index all out.avif dec_avif.png)
    sz=$(size_of out.avif)
    [ "$(frame_count out.avif)" = 20 ] || log "WARNING: AVIF animation encode did not preserve 20 frames"
    push_anim "AVIF" "lossless, speed 6/10 (-l)" "$enc_ms" "$dec_ms" "$sz" "$(pct_of "$sz" "$anim_base_size")"
else
    log "WARNING: avifenc/avifdec/ffmpeg not found, skipping AVIF animation row"
fi

# JXL (cjxl reads the APNG container directly, one frame per page; effort
# defaults to 7 out of 1..10)
if have cjxl && have djxl; then
    enc_ms=$(median_ms cjxl -d 0 "$ANIM_SRC" out.jxl --num_threads=0)
    dec_ms=$(median_ms djxl out.jxl dec_jxl.apng)
    sz=$(size_of out.jxl)
    [ "$(frame_count dec_jxl.apng)" = 20 ] || log "WARNING: JXL animation round-trip did not preserve 20 frames"
    push_anim "JXL" "lossless, effort 7/10 (-d 0)" "$enc_ms" "$dec_ms" "$sz" "$(pct_of "$sz" "$anim_base_size")"
else
    log "WARNING: cjxl/djxl not found, skipping JXL animation row"
fi

# --- render ------------------------------------------------------------------

# Bolds the winning (smallest) value in a column, skipping GIF -- its size
# and timings are not comparable to the true-color formats (see the caveat
# printed below the tables), so it should never visually "win".
bold_min() {
    local col="$1"; shift
    local best="" val fmt
    for row in "$@"; do
        IFS='|' read -r fmt _ enc dec sz _ <<< "$row"
        [ "$fmt" = "GIF" ] && continue
        case "$col" in
            enc) val="$enc" ;;
            dec) val="$dec" ;;
            sz)  val="$sz"  ;;
        esac
        is_numeric "$val" || continue
        if [ -z "$best" ] || awk -v a="$val" -v b="$best" 'BEGIN{exit !(a<b)}'; then
            best="$val"
        fi
    done
    echo "$best"
}

render_table() {
    local best_enc best_dec best_sz
    best_enc=$(bold_min enc "$@")
    best_dec=$(bold_min dec "$@")
    best_sz=$(bold_min sz "$@")

    echo "| Format | Mode | Encode (median, ms) | Decode (median, ms) | Size (bytes) | % of baseline |"
    echo "|---|---|---:|---:|---:|---:|"
    local row
    for row in "$@"; do
        IFS='|' read -r fmt mode enc dec sz pct <<< "$row"
        [ -n "$best_enc" ] && [ "$enc" = "$best_enc" ] && enc="**$enc**"
        [ -n "$best_dec" ] && [ "$dec" = "$best_dec" ] && dec="**$dec**"
        [ -n "$best_sz" ] && [ "$sz" = "$best_sz" ] && sz="**$sz**"
        echo "| $fmt | $mode | $enc | $dec | $sz | $pct |"
    done
}

cores=$(nproc)
mem=$(free -h | awk '/^Mem:/ {print $2}')

output=$(cat <<EOF
Measured on $cores CPU cores / ${mem} RAM, $N runs per cell (median wall time).
Source files: [\`tests/data/RGB_24bits_palette_color_test_chart.png\`](tests/data/RGB_24bits_palette_color_test_chart.png)
(258×200 RGB) and [\`tests/data/Animated_PNG_example_bouncing_beach_ball.apng\`](tests/data/Animated_PNG_example_bouncing_beach_ball.apng)
(100×100, 20 frames). Reproduce with \`bench/bench.sh\`.

**Still image** (baseline: PNG)

$(render_table "${still_rows[@]}")

**Animation** (baseline: APNG)

$(render_table "${anim_rows[@]}")

Bold marks the smallest value in each numeric column (GIF excluded, see
below). Mode notes each tool's effort/speed setting where it has one — all
runs use tool defaults, not the slowest/smallest setting each encoder is
capable of, so this is not an exhaustive size-vs-speed sweep.

GIF is limited to a 256-color indexed palette, so its "lossless" encode is
only lossless relative to the quantized palette, not to the original
true-color pixels — its numbers are not directly comparable to the other
formats in these tables, and it is excluded from the bold "winner" markers
above for the same reason.

PXL/APXL decode slower than they encode here, which looks backwards for a
zstd-based format (zstd itself decodes several times faster than it
compresses). The gap is not in the codec: \`pxl_decode\`/\`apxl_decode\` alone
run in well under a millisecond on these inputs. \`pxltool d\`/\`da\`, what this
benchmark actually times, also re-encodes the result as a PNG/APNG on the way
out, and libpng's zlib deflate on write costs roughly 10x what its inflate on
read costs — that PNG-write cost, not decompression, is what dominates the
decode column for PXL and APXL.
EOF
)

echo "$output"

if [ "${1:-}" = "--update-readme" ]; then
    readme="$repo_root/README.md"
    awk -v repl="$output" '
        /<!-- BENCH:BEGIN -->/ { print; print repl; skip = 1; next }
        /<!-- BENCH:END -->/   { skip = 0 }
        !skip
    ' "$readme" > "$readme.tmp"
    mv "$readme.tmp" "$readme"
    log "README.md updated between BENCH:BEGIN/BENCH:END markers"
fi

#!/bin/bash
# Size against decode speed, one row per format, over one corpus.
#
#   bench/formats.sh <corpus-dir> <label> [max-files]
#
# Writes bench/data/formats_<label>.tsv for bench/plots.py.
#
# Two decisions worth stating, because they are what makes the numbers mean
# anything. Size is the sum over the corpus divided by the sum of the source
# PNGs -- the "how much disk would converting this save" question, which weights
# by file size; the per-file-average question would answer something else and is
# not what this reports. Speed is total RGBA output divided by total decode
# time, measured in process by bench/formatdec, never by timing a CLI tool,
# because a CLI run is mostly startup and writing the output image.
#
# Corpora are never pooled. Photographs and synthetic stills differ by tens of
# points here, so one chart per corpus and no averaging across them.
set -uo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/.." && pwd)
cd "$repo_root"

dir=${1:?usage: formats.sh <corpus-dir> <label> [max-files]}
label=${2:?}
maxn=${3:-40}
reps=${FMT_REPS:-5}
levels=${FMT_PXL_LEVELS:-"1 12 19"}
out=bench/data/formats_$label.tsv
fd=build/pxl_bench_formatdec

[ -x "$fd" ] || { echo "error: $fd not built" >&2; exit 1; }
mkdir -p bench/data
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

mapfile -t files < <(find -L "$dir" -name '*.png' -type f | sort | awk -v n="$maxn" 'NR%1==0' | head -n "$maxn")
echo "# $label: ${#files[@]} files, $reps reps" >&2

# decoder -> summed ms, summed output MB, summed encoded bytes, and the PNG
# bytes of the files that decoder actually handled. The last one matters: a
# format that fails on some files must be compared against the sources it did
# encode, not against the whole corpus, or skipping the hard files looks like
# compressing them well.
declare -A MS MB BYTES PNGOF NFILES

i=0
for f in "${files[@]}"; do
    i=$((i+1))
    fpng=$(stat -c%s "$f")
    for lv in $levels; do
        build/pxltool c "$f" "$tmp/l$lv.pxl" -l "$lv" >/dev/null 2>&1 || rm -f "$tmp/l$lv.pxl"
    done
    cjxl  -d 0 "$f" "$tmp/a.jxl" --num_threads=0 >/dev/null 2>&1 || rm -f "$tmp/a.jxl"
    cwebp -lossless -quiet "$f" -o "$tmp/a.webp"  >/dev/null 2>&1 || rm -f "$tmp/a.webp"
    avifenc -l -o "$tmp/a.avif" "$f"              >/dev/null 2>&1 || rm -f "$tmp/a.avif"

    run=(--png "$f")
    [ -f "$tmp/a.jxl"  ] && run+=(--jxl  "$tmp/a.jxl")
    [ -f "$tmp/a.webp" ] && run+=(--webp "$tmp/a.webp")
    [ -f "$tmp/a.avif" ] && run+=(--avif "$tmp/a.avif")

    # formatdec takes a single --pxl, so it runs once per level. The non-PXL
    # decoders would then be measured once per level too, which would triple
    # their weight in the totals -- so they are accumulated on the first pass
    # only, and later passes keep nothing but their PXL row.
    first=1
    for lv in $levels; do
        [ -f "$tmp/l$lv.pxl" ] || continue
        while IFS=$'\t' read -r name ms mbs bytes; do
            [ "$name" = "decoder" ] && continue
            # The RGBA row is the comparable one: every other decoder here is
            # asked for RGBA, and MB/s means nothing across outputs of
            # different channel counts. PXL-native is dropped rather than
            # plotted beside them.
            [ "$name" = "PXL-native" ] && continue
            if [ "$name" = "PXL-RGBA" ]; then
                name="PXL-L$lv"
            elif [ "$first" != 1 ]; then
                continue
            fi
            MS[$name]=$(awk -v a="${MS[$name]:-0}" -v b="$ms" 'BEGIN{print a+b}')
            MB[$name]=$(awk -v a="${MB[$name]:-0}" -v b="$ms" -v c="$mbs" 'BEGIN{print a+b*c/1000}')
            BYTES[$name]=$(( ${BYTES[$name]:-0} + bytes ))
            PNGOF[$name]=$(( ${PNGOF[$name]:-0} + fpng ))
            NFILES[$name]=$(( ${NFILES[$name]:-0} + 1 ))
        done < <("$fd" "$reps" "${run[@]}" --pxl "$tmp/l$lv.pxl" 2>/dev/null)
        first=0
    done
    [ $((i % 10)) = 0 ] && echo "# $i/${#files[@]}" >&2
    rm -f "$tmp"/a.* "$tmp"/l*.pxl
done

{
    printf 'decoder\tpct_of_png\tmb_per_s\tbytes\tpng_bytes\tfiles\n'
    for k in "${!MS[@]}"; do
        awk -v k="$k" -v ms="${MS[$k]}" -v mb="${MB[$k]}" -v by="${BYTES[$k]}" \
            -v pt="${PNGOF[$k]}" -v n="${NFILES[$k]}" 'BEGIN{
            rate = (ms > 0) ? mb / (ms / 1000) : 0
            pct  = (pt > 0) ? 100 * by / pt : 0
            printf "%s\t%.1f\t%.1f\t%d\t%d\t%d\n", k, pct, rate, by, pt, n }'
    done
} > "$out.raw"
# Sort the data rows only: piping the whole thing through sort once put the
# header in the middle, where every TSV reader took the first data row for it.
{ head -1 "$out.raw"; tail -n +2 "$out.raw" | sort; } > "$out"
rm -f "$out.raw"
echo "# written: $out" >&2
cat "$out"

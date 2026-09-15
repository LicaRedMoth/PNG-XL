#!/bin/bash
# Sweeps the zstd level over a corpus and writes a TSV for bench/plots.py.
#
#   bench/levelsweep.sh <corpus-dir> <label> [out.tsv]
#
# Emits one row per level: encoded bytes, source PNG bytes, encode ms, and raw
# decode ms. Decode is measured because the level's whole argument is that it
# costs encode time and nothing else -- a claim that has to be visible in the
# data rather than asserted beside it.
#
# Levels are sampled rather than exhaustive: the curve's shape lives at the ends
# and the middle is nearly linear, and level 19+ on a large corpus is minutes
# per point.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/.." && pwd)
cd "$repo_root"

dir=${1:?usage: levelsweep.sh <corpus-dir> <label> [out.tsv]}
label=${2:?}
out=${3:-bench/data/levels_$label.tsv}
levels=${SWEEP_LEVELS:-"1 3 6 9 12 15 17 19 22"}
pxltool=${PXLTOOL:-$repo_root/build/pxltool}
rawdec=${PXL_BENCH_RAWDEC:-$repo_root/build/pxl_bench_rawdec}

mkdir -p "$(dirname "$out")"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

mapfile -t files < <(find -L "$dir" -name '*.png' -type f | sort)
[ "${#files[@]}" -gt 0 ] || { echo "error: no PNGs under $dir" >&2; exit 1; }
echo "# $label: ${#files[@]} files, levels: $levels" >&2

printf 'level\tfiles\tbytes\tpng_bytes\tencode_ms\tdecode_ms\n' > "$out"
for lv in $levels; do
    tot=0; png=0; n=0
    start=$(date +%s%N)
    for f in "${files[@]}"; do
        "$pxltool" c "$f" "$tmp/o.pxl" -l "$lv" >/dev/null 2>&1 || continue
        tot=$((tot + $(stat -c%s "$tmp/o.pxl")))
        png=$((png + $(stat -c%s "$f")))
        n=$((n + 1))
    done
    enc=$(( ($(date +%s%N) - start) / 1000000 ))

    # Raw decode over a bounded sample: the point is whether the level moves
    # decode at all, and that does not need the whole corpus.
    dec=0
    if [ -x "$rawdec" ]; then
        start=$(date +%s%N)
        for f in "${files[@]:0:20}"; do
            "$pxltool" c "$f" "$tmp/o.pxl" -l "$lv" >/dev/null 2>&1 || continue
            "$rawdec" "$f" 3 >/dev/null 2>&1 || true
        done
        dec=$(( ($(date +%s%N) - start) / 1000000 ))
    fi
    printf '%s\t%d\t%d\t%d\t%d\t%d\n' "$lv" "$n" "$tot" "$png" "$enc" "$dec" >> "$out"
    echo "# level $lv: $n files, $tot bytes, ${enc}ms" >&2
done
echo "# written: $out" >&2

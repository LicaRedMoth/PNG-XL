#!/bin/bash
# Headroom for a MOVE primitive over what zstd's long-distance matching already
# finds, measured across the Anita animation dataset.
#
#   bench/motion.sh [shots_per_pass] [pairs_per_shot]
#
# Sampling, which this script prints with its results because the project has
# been burned by unstated sampling before: shots are drawn evenly across the
# whole sorted shot list of each pass, not taken from the front, so one title
# with 60 shots cannot stand in for the pass. Within a shot the first N+1
# frames give N consecutive pairs. Every shot then counts once in the average,
# regardless of length -- consecutive frames of one shot are near-duplicates,
# so weighting by frame count would report the longest shot as the corpus.
#
# Passes are reported separately and never pooled: sketch is line art,
# composition and color are finished frames, and they behave differently enough
# that a combined number would describe neither.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/.." && pwd)
tool=${MOTION_BIN:-$repo_root/build/pxl_bench_motion}
data=${ANITA_DIR:-$repo_root/tests/data/AnitaDataset}

[ -x "$tool" ] || { echo "error: $tool not built (cmake --build build --target pxl_bench_motion)" >&2; exit 1; }
[ -d "$data" ] || { echo "error: Anita dataset not found at $data" >&2; exit 1; }

SHOTS=${1:-24}
PAIRS=${2:-4}
BLOCK=${MOTION_BLOCK:-16}

echo "# Anita MOVE headroom: $SHOTS shots per pass (evenly spread), first $PAIRS pairs per shot, ${BLOCK}x${BLOCK} blocks"
echo

printf "%-13s %6s %8s %8s %8s %10s %12s %9s\n" \
       "pass" "shots" "copy%" "flat%" "move%" "residual%" "dominant-vec" "capped"

for pass in sketch composition color; do
    mapfile -t all < <(find "$data" -mindepth 3 -maxdepth 3 -type d -path "*/$pass/*" | sort)
    n=${#all[@]}
    [ "$n" -gt 0 ] || continue

    # Even stride over the whole list rather than the first $SHOTS entries.
    step=$(( n / SHOTS )); [ "$step" -lt 1 ] && step=1
    picked=()
    for (( i = 0; i < n && ${#picked[@]} < SHOTS; i += step )); do picked+=("${all[$i]}"); done

    : > "/tmp/motion_$pass.$$"
    used=0
    for shot in "${picked[@]}"; do
        mapfile -t frames < <(find "$shot" -maxdepth 1 -name '*.png' | sort | head -n $((PAIRS + 1)))
        [ "${#frames[@]}" -ge 2 ] || continue
        MOTION_BLOCK="$BLOCK" "$tool" "${frames[@]}" 2>/dev/null >> "/tmp/motion_$pass.$$" || continue
        used=$((used + 1))
    done

    awk -v pass="$pass" -v shots="$used" '
        {
            for (i = 1; i <= NF; i++) {
                split($i, kv, "=")
                v = kv[2]; sub(/%$/, "", v)
                sum[kv[1]] += v + 0
            }
            rows++
        }
        END {
            if (!rows) { printf "%-13s %6s %8s\n", pass, shots, "no data"; exit }
            printf "%-13s %6s %7.2f%% %7.2f%% %7.2f%% %9.2f%% %11.1f%% %9d\n",
                   pass, shots, sum["copy"]/rows, sum["flat"]/rows, sum["move"]/rows,
                   sum["residual"]/rows, sum["move_dominant_vec"]/rows, sum["capped"]
        }' "/tmp/motion_$pass.$$"
    rm -f "/tmp/motion_$pass.$$"
done

echo
echo "# copy%     already free: zstd finds these as plain LZ matches today."
echo "# flat%     a single colour throughout: matches anywhere, so excluded from move."
echo "# move%     the prize: identical only at a displacement. Anything a motion vector could buy."
echo "# residual% identical nowhere; needs real residual coding whatever else changes."
echo "# dominant-vec: share of MOVE blocks agreeing on the single most common vector."
echo "#              Low means scattered vectors, which cost about as much to code as they save."
echo "# capped:    blocks whose search hit the candidate limit. Non-zero understates move%."

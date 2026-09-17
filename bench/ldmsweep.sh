#!/bin/bash
# LDM per-stream sweep: how much does zstd long-distance matching change
# APXL-style cross-frame compression, holding the level fixed, per shot,
# across the Anita animation dataset.
#
#   bench/ldmsweep.sh [shots_per_pass] [frames_per_shot]
#
# Sampling, stated here for the same reason bench/motion.sh states its own:
# shots are drawn evenly across the whole sorted shot list of each pass, not
# taken from the front, so one long-running title cannot stand in for the
# pass. Passes are reported separately and never pooled -- sketch is line
# art, composition and color are finished frames, and RESEARCH.md's
# "Animation: cross-frame coding splits the corpus in two" already found them
# behaving oppositely on 16-frame, two-shot evidence. This is what turns that
# into a real, corpus-wide, reproducible number.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/.." && pwd)
tool=${LDMSWEEP_BIN:-$repo_root/build/pxl_bench_ldmsweep}
data=${ANITA_DIR:-$repo_root/tests/data/AnitaDataset}

[ -x "$tool" ] || { echo "error: $tool not built (cmake --build build --target pxl_bench_ldmsweep)" >&2; exit 1; }
[ -d "$data" ] || { echo "error: Anita dataset not found at $data" >&2; exit 1; }

SHOTS=${1:-24}
FRAMES=${2:-16}
LEVEL=${LDMSWEEP_LEVEL:-12}
export LDMSWEEP_LEVEL="$LEVEL"

echo "# LDM per-stream sweep: $SHOTS shots per pass (evenly spread), first $FRAMES frames per shot, level $LEVEL"
echo

printf "%-13s %6s %9s %9s %9s %8s %6s\n" \
       "pass" "shots" "avg" "min" "max" "helped" "hurt"

for pass in sketch composition color; do
    mapfile -t all < <(find "$data" -mindepth 3 -maxdepth 3 -type d -path "*/$pass/*" | sort)
    n=${#all[@]}
    [ "$n" -gt 0 ] || continue

    # Even stride over the whole list rather than the first $SHOTS entries.
    step=$(( n / SHOTS )); [ "$step" -lt 1 ] && step=1
    picked=()
    for (( i = 0; i < n && ${#picked[@]} < SHOTS; i += step )); do picked+=("${all[$i]}"); done

    : > "/tmp/ldmsweep_$pass.$$"
    used=0
    for shot in "${picked[@]}"; do
        mapfile -t frames < <(find "$shot" -maxdepth 1 -name '*.png' | sort | head -n "$FRAMES")
        [ "${#frames[@]}" -ge 2 ] || continue
        "$tool" "${frames[@]}" 2>/dev/null >> "/tmp/ldmsweep_$pass.$$" || continue
        used=$((used + 1))
    done

    awk -v pass="$pass" -v shots="$used" '
        {
            for (i = 1; i <= NF; i++) {
                split($i, kv, "=")
                if (kv[1] == "delta") {
                    v = kv[2]; sub(/%$/, "", v); d = v + 0
                    sum += d; rows++
                    if (rows == 1 || d < mn) { mn = d }
                    if (rows == 1 || d > mx) { mx = d }
                    if (d < 0) { helped++ } else { hurt++ }
                }
            }
        }
        END {
            if (!rows) { printf "%-13s %6s %8s\n", pass, shots, "no data"; exit }
            printf "%-13s %6s %8.2f%% %8.2f%% %8.2f%% %7d %5d\n",
                   pass, shots, sum/rows, mn, mx, helped + 0, hurt + 0
        }' "/tmp/ldmsweep_$pass.$$"
    rm -f "/tmp/ldmsweep_$pass.$$"
done

echo
echo "# avg/min/max: delta% = (LDM-on size - LDM-off size) / LDM-off size, per shot, at level $LEVEL."
echo "#              Negative means LDM made that shot smaller (good); positive means bigger (worse)."
echo "# helped/hurt: shots where LDM-on was smaller / not smaller than LDM-off."
echo "# This compares the single cross-frame stream .apxl already always uses, LDM on vs off -- level,"
echo "# frame count and geometry held fixed per shot. It is not the per-frame-stream comparison"
echo "# RESEARCH.md's original 16-frame numbers used; see ldmsweep.c's header for why."

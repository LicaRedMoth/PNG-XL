#!/bin/bash
# The full benchmark pass: regenerate both README tables, sweep the level curve
# over each content class, and render the charts.
#
#   bench/overnight.sh [logfile]
#
# Meant to run on an idle machine and take hours. It waits for the load average
# to fall before the publishing steps, because bench.sh refuses to write README
# from a loaded machine and would otherwise abort the whole run at step one.
#
# Nothing here uses `set -e`: a benchmark that dies three hours in because one
# optional encoder is missing is worse than one that records the gap and
# continues. Every step logs its own start, finish and exit status.
set -uo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/.." && pwd)
cd "$repo_root"

log=${1:-bench/data/overnight.log}
mkdir -p "$(dirname "$log")" bench/data docs/img

say() { printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$log"; }

step() {
    local name="$1"; shift
    local t0 rc
    say "START  $name"
    t0=$(date +%s)
    "$@" >>"$log" 2>&1
    rc=$?
    say "$([ $rc = 0 ] && echo 'DONE  ' || echo 'FAILED') $name  (${rc}, $(( $(date +%s) - t0 ))s)"
    return 0
}

wait_idle() {
    local limit=${BENCH_LOAD_LIMIT:-1.5} l
    for _ in $(seq 1 240); do
        l=$(awk '{print $1}' /proc/loadavg)
        awk -v l="$l" -v m="$limit" 'BEGIN{exit !(l<m)}' && { say "load $l, below $limit"; return 0; }
        sleep 15
    done
    say "WARNING: load never fell below $limit; publishing steps will be skipped"
    return 1
}

say "=== overnight pass on $(hostname), $(nproc) cores ==="
say "commit $(git rev-parse --short HEAD)"

if wait_idle; then
    step "README still/animation tables" env PXL_LEVELS="1 12" bench/bench.sh --update-readme
    step "README corpus table"           env PXL_LEVELS="1 12 19" bench/corpus.sh --update-readme
else
    say "SKIP   both --update-readme steps"
fi

# Level curves. Kodak and PngSuite are committed; the other two are local, so
# they are swept only if present, and the synthetic set is sampled because 838
# files times nine levels is a night by itself.
step "level sweep: Kodak" bench/levelsweep.sh \
     tests/data/Kodak-Lossless-True-Color-Image-Suite/PhotoCD_PCD0992 kodak
step "level sweep: PngSuite" bench/levelsweep.sh \
     tests/data/The-official-test-suite-for-PNG pngsuite

if [ -d tests/data/Synthetic-Screenshots ]; then
    sample=bench/data/.sample_synth
    rm -rf "$sample"; mkdir -p "$sample"
    find -L tests/data/Synthetic-Screenshots -name '*.png' | sort | awk 'NR%7==0' | head -120 \
        | while IFS= read -r f; do cp "$f" "$sample/"; done
    step "level sweep: synthetic (120 sampled)" bench/levelsweep.sh "$sample" synthetic
    rm -rf "$sample"
fi

step "charts" python3 bench/plots.py docs/img

say "=== finished ==="
say "tables: git diff README.md   charts: docs/img/   log: $log"

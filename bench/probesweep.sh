#!/bin/bash
# Does a cheap probe level rank the filter candidates the way the full level
# does? If it does, the encoder can compare candidates cheaply and compress the
# winner once, for byte-identical output at a fraction of the encode cost.
#
#   bench/probesweep.sh <corpus-dir> <label> [max-files] [full-level] [probe-level]
#
# Agreement alone is not the answer. What decides it is the cost of the
# disagreements: picking the second-best filter loses bytes, and a strategy that
# agrees 95% of the time but loses 10% on the rest is worse than one that agrees
# less and loses nothing. So this reports both, and the size penalty is measured
# against the filter the full search would really have chosen.
set -uo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/.." && pwd)
cd "$repo_root"

dir=${1:?usage: probesweep.sh <corpus-dir> <label> [max-files] [full] [probe]}
label=${2:?}
maxn=${3:-60}
full=${4:-12}
probe=${5:-1}
tool=build/pxl_bench_encstages
[ -x "$tool" ] || { echo "error: $tool not built" >&2; exit 1; }

mapfile -t files < <(find -L "$dir" -name '*.png' -type f | sort | head -n "$maxn")
echo "# $label: ${#files[@]} files, full level $full, probe level $probe" >&2

tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
n=0
for f in "${files[@]}"; do
    "$tool" "$f" 1 "$full" "$probe" 2>/dev/null > "$tmp.one" || continue
    grep -q "^agreement" "$tmp.one" || continue
    # full bytes per filter, the probe's pick, and the two wall times
    awk -v file="$f" '
        /^(none|delta|adaptive|bcif) / { full[$1] = $5 }
        /^full-level winner/  { fw = $4 }
        /^probe winner/       { pw = $4 }
        /^search-all time/    { st = $4 }
        /^probe-then-one/     { pt = $3 }
        /^probe top-2       :/ { t2b = $5 }
        /^probe top-2 time/    { t2t = $5 }
        END {
            best = 0
            for (k in full) { if (best == 0 || full[k] < best) best = full[k] }
            chosen = (pw in full) ? full[pw] : best
            # Files with only one candidate never print a top-2 line; they
            # cannot lose anything, so they count as matching at full cost.
            if (t2b == "") { t2b = best; t2t = st }
            printf "%s\t%s\t%s\t%d\t%d\t%s\t%s\t%d\t%s\n",
                   file, fw, pw, best, chosen, st, pt, t2b, t2t
        }' "$tmp.one" >> "$tmp"
    n=$((n + 1))
    [ $((n % 20)) = 0 ] && echo "# $n/${#files[@]}" >&2
done
rm -f "$tmp.one"

awk -F'\t' -v label="$label" -v full="$full" -v probe="$probe" '
{
    rows++
    if ($2 == $3) agree++
    best += $4; chosen += $5
    if ($5 > $4) { loss_files++; loss += $5 - $4 }
    st += $6; pt += $7
    t2 += $8; t2t += $9
    if ($8 > $4) { t2_loss_files++ }
}
END {
    if (!rows) { print "no data"; exit }
    printf "\n%-12s %5s | %8s %10s %8s | %8s %10s %8s\n",
        "corpus", "files", "top1", "penalty", "speedup", "top2", "penalty", "speedup"
    printf "%-12s %5d | %7.1f%% %9.3f%% %7.2fx | %7.1f%% %9.3f%% %7.2fx\n",
        label, rows,
        100*agree/rows, 100*(chosen-best)/best, st/pt,
        100*(rows-t2_loss_files)/rows, 100*(t2-best)/best, st/t2t
}' "$tmp"

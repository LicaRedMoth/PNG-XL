#!/bin/bash
# Cross-format lossless comparison over a whole corpus, not a single image.
# bench.sh answers "how does PXL do on this one chart"; this answers "how does
# PXL do on 24 photographs and 162 PNG conformance files", which is the number
# that actually says whether the format is competitive.
#
#   bench/corpus.sh                 # print the markdown table
#   bench/corpus.sh --update-readme # splice it into README.md between the
#                                   # <!-- CORPUS:BEGIN --> / CORPUS:END markers
#   bench/corpus.sh --with-usc      # also sweep the USC-SIPI corpus (210 more
#                                   # files, not committed; see bench/usc_png.sh)
#
# Totals are summed over every file a format encoded successfully, so a format
# is only ever compared against PNG on the same file set (reported per row).
# Requires: cjxl/djxl, cwebp or ffmpeg, avifenc, magick. Missing tools skip
# their row with a warning instead of aborting.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/.." && pwd)
cd "$repo_root"

if [ -n "${PXLTOOL:-}" ]; then
    pxltool="$PXLTOOL"
elif [ -x build/pxltool ]; then
    pxltool="$repo_root/build/pxltool"
else
    echo "error: pxltool not found. Build it first: cmake -B build && cmake --build build -j" >&2
    exit 1
fi

have() { command -v "$1" >/dev/null 2>&1; }
log() { echo "# $*" >&2; }

update_readme=0
with_usc=0
for arg in "$@"; do
    case "$arg" in
        --update-readme) update_readme=1 ;;
        --with-usc) with_usc=1 ;;
        *) echo "usage: $0 [--update-readme] [--with-usc]" >&2; exit 1 ;;
    esac
done

LEVEL=${PXL_LEVEL:-12}

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

# The corpus: Kodak true-color photographs plus the official PNG test suite.
# Both are committed under tests/data/, so this is reproducible as-is.
files=()
for f in tests/data/Kodak-Lossless-True-Color-Image-Suite/PhotoCD_PCD0992/*.png; do
    [ -e "$f" ] && files+=("$f")
done
for f in tests/data/The-official-test-suite-for-PNG/*.png; do
    # x*.png in the suite are deliberately corrupt files; they are not valid
    # input for any encoder and would only measure who rejects them fastest.
    case "$(basename "$f")" in x*) continue ;; esac
    [ -e "$f" ] && files+=("$f")
done

# USC-SIPI is opt-in: it is not committed, and it is 210 files of aerials,
# textures and grayscale plates whose character differs enough from Kodak that
# folding it in unasked would silently change what the README's numbers mean.
usc_dir=tests/data/USC-SIPI-Image-Database/png
usc_n=0
if [ "$with_usc" = 1 ]; then
    if [ ! -d "$usc_dir" ]; then
        echo "error: $usc_dir not found. Run bench/usc_png.sh first." >&2
        exit 1
    fi
    while IFS= read -r f; do
        files+=("$f")
        usc_n=$(( usc_n + 1 ))
    done < <(find "$usc_dir" -name '*.png' | sort)
    log "USC-SIPI: $usc_n files"
fi
log "corpus: ${#files[@]} files"

rows=()

# Runs encoder "$@" where %IN%/%OUT% are substituted, over the whole corpus.
# Prints "<encoded bytes> <png bytes of those same files> <count> <ms>".
sweep() {
    local ext="$1"; shift
    local total=0 png_total=0 n=0 start end ms=0
    local f out
    start=$(date +%s%N)
    for f in "${files[@]}"; do
        out="$workdir/out.$ext"
        rm -f "$out"
        local cmd=() a
        for a in "$@"; do
            a=${a//%IN%/$f}
            a=${a//%OUT%/$out}
            cmd+=("$a")
        done
        if "${cmd[@]}" >/dev/null 2>&1 && [ -s "$out" ]; then
            total=$(( total + $(stat -c%s "$out") ))
            png_total=$(( png_total + $(stat -c%s "$f") ))
            n=$(( n + 1 ))
        fi
    done
    end=$(date +%s%N)
    ms=$(( (end - start) / 1000000 ))
    echo "$total $png_total $n $ms"
}

push() { rows+=("$1|$2|$3|$4|$5"); }

emit() {
    local name="$1" mode="$2" res="$3"
    read -r total png_total n ms <<< "$res"
    if [ "$n" = 0 ]; then
        log "WARNING: $name encoded no files, skipping row"
        return
    fi
    push "$name" "$mode" "$n" "$total" \
        "$(awk -v a="$total" -v b="$png_total" -v m="$ms" -v n="$n" \
             'BEGIN{printf "%.1f%%|%.1f", 100*a/b, m/n}')"
}

emit "PXL" "lossless, level $LEVEL" \
    "$(sweep pxl "$pxltool" c %IN% %OUT% -l "$LEVEL")"

if have cjxl; then
    emit "JXL" "lossless, effort 7/10" "$(sweep jxl cjxl -d 0 %IN% %OUT% --num_threads=0)"
else
    log "WARNING: cjxl not found, skipping JXL row"
fi

if have cwebp; then
    emit "WebP" "lossless (-lossless)" "$(sweep webp cwebp -lossless -quiet %IN% -o %OUT%)"
elif have ffmpeg; then
    emit "WebP" "lossless (-lossless 1)" \
        "$(sweep webp ffmpeg -y -v error -i %IN% -c:v libwebp -lossless 1 %OUT%)"
else
    log "WARNING: neither cwebp nor ffmpeg found, skipping WebP row"
fi

if have avifenc; then
    emit "AVIF" "lossless, speed 6/10 (-l)" "$(sweep avif avifenc -l -o %OUT% %IN%)"
else
    log "WARNING: avifenc not found, skipping AVIF row"
fi

if have oxipng; then
    emit "PNG (oxipng -o max)" "lossless recompress" \
        "$(sweep png oxipng -o max --quiet --out %OUT% %IN%)"
else
    log "WARNING: oxipng not found, skipping PNG-recompress row"
fi

# --- render ------------------------------------------------------------------

table() {
    echo "| Format | Mode | Files | Total bytes | % of PNG | Encode (ms/file) |"
    echo "|---|---|---:|---:|---:|---:|"
    local row
    for row in "${rows[@]}"; do
        IFS='|' read -r fmt mode n total pct ms <<< "$row"
        echo "| $fmt | $mode | $n | $total | $pct | $ms |"
    done
}

cores=$(nproc)
usc_note=""
[ "$with_usc" = 1 ] && usc_note=", plus the $usc_n
[USC-SIPI](tests/data/USC-SIPI-Image-Database) aerials/textures/misc plates
converted to PNG at ImageMagick defaults (\`bench/usc_png.sh\`)"
output=$(cat <<EOF
Corpus-wide totals over the 24 [Kodak](tests/data/Kodak-Lossless-True-Color-Image-Suite)
true-color photographs and the valid files of the
[official PNG test suite](tests/data/The-official-test-suite-for-PNG)
(deliberately-corrupt \`x*.png\` excluded)$usc_note, ${#files[@]} files in total, on $cores CPU cores.
Every row sums only the files that format encoded successfully, and compares
against the source PNGs of that same subset. Reproduce with \`bench/corpus.sh\`.

$(table)

Encode time is total wall time divided by file count, so it includes process
startup per file — these are whole-corpus throughput figures, not the
carefully-median-ed per-call timings of the single-image tables above.
EOF
)

if [ "$update_readme" = 1 ]; then
    readme="$repo_root/README.md"
    grep -q '<!-- CORPUS:BEGIN -->' "$readme" || {
        echo "error: README.md has no <!-- CORPUS:BEGIN --> marker" >&2; exit 1; }
    python3 - "$readme" <<PY
import re, sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
new = """<!-- CORPUS:BEGIN -->
$output
<!-- CORPUS:END -->"""
s = re.sub(r'<!-- CORPUS:BEGIN -->.*?<!-- CORPUS:END -->', lambda _: new, s, flags=re.S)
open(p, 'w', encoding='utf-8').write(s)
PY
    log "README.md updated between CORPUS:BEGIN/CORPUS:END markers"
fi

echo "$output"

#!/bin/bash
# Converts the USC-SIPI image database from TIFF to PNG so it can be used as a
# benchmark corpus. The corpus itself is not committed (see .gitignore); this
# script is, so the conversion is reproducible from the upstream download.
#
#   bench/usc_png.sh          # convert tests/data/USC-SIPI-Image-Database
#   bench/usc_png.sh --check  # only verify an existing conversion
#
# PNGs are written to a png/ subdirectory mirroring the volume layout
# (aerials/, misc/, sequences/, textures/) and left at ImageMagick's default
# settings on purpose: the corpus is meant to represent PNG as encoders
# actually emit it by default, not PNG tuned to its smallest possible output.
# Every file is verified pixel-exact against its TIFF source with
# `magick compare -metric AE`, because a corpus that silently lost a channel
# or got quantized would make every size comparison downstream meaningless.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/.." && pwd)
cd "$repo_root"

src=tests/data/USC-SIPI-Image-Database
dst="$src/png"

command -v magick >/dev/null 2>&1 || { echo "error: magick not found" >&2; exit 1; }
[ -d "$src" ] || { echo "error: $src not found. See bench/README.md for the download." >&2; exit 1; }

check_only=0
[ "${1:-}" = "--check" ] && check_only=1

# One file: convert (unless --check) and prove the result is pixel-identical.
convert_one() {
    local tiff="$1" png="$2"
    if [ "$check_only" = 0 ]; then
        mkdir -p "$(dirname "$png")"
        magick "$tiff" "$png"
    fi
    [ -f "$png" ] || { echo "MISSING $png" >&2; return 1; }
    local ae
    ae=$(magick compare -metric AE "$tiff" "$png" null: 2>&1 || true)
    case "$ae" in
        0|"0 (0)") return 0 ;;
        *) echo "MISMATCH $png (AE=$ae)" >&2; return 1 ;;
    esac
}
export -f convert_one
export check_only

# Conversion is CPU-bound and per-file independent, so fan it out over the
# cores. NUL-separated names keep this correct for the few sources whose names
# carry dots and dashes.
mapfile -t tiffs < <(find "$src" -name '*.tiff' | sort)
[ "${#tiffs[@]}" -gt 0 ] || { echo "error: no .tiff under $src" >&2; exit 1; }

echo "# ${#tiffs[@]} sources, $( [ "$check_only" = 1 ] && echo verifying || echo converting ) with $(nproc) jobs" >&2

for tiff in "${tiffs[@]}"; do
    rel=${tiff#"$src"/}
    printf '%s\0%s\0' "$tiff" "$dst/${rel%.tiff}.png"
done | xargs -0 -n2 -P "$(nproc)" bash -c 'convert_one "$0" "$1"' || {
    echo "error: conversion or verification failed" >&2
    exit 1
}

count=$(find "$dst" -name '*.png' | wc -l)
bytes=$(find "$dst" -name '*.png' -printf '%s\n' | awk '{s+=$1} END {print s}')
echo "# ok: $count PNG, $(( bytes / 1024 / 1024 )) MiB total, all pixel-exact vs TIFF" >&2

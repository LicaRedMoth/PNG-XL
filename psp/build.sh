#!/bin/bash
# Cross-compiles the PSP decode smoke test with the pspdev toolchain
# (github.com/pspdev/pspdev). Needs PSPDEV pointing at an extracted
# pspdev-*-x86_64.tar.gz release and psp/testdata.h already generated -- see
# psp/README.md for both one-time steps.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)

: "${PSPDEV:?set PSPDEV to the extracted pspdev toolchain, e.g. ~/pspdev-toolchain/pspdev}"
[ -x "$PSPDEV/bin/psp-gcc" ] || { echo "error: $PSPDEV/bin/psp-gcc not found" >&2; exit 1; }
[ -f "$here/testdata.h" ] || {
    echo "error: $here/testdata.h missing; run bench/pxl_bench_mkpsptest > psp/testdata.h" >&2
    exit 1
}

export PSPDEV
export PATH="$PSPDEV/bin:$PATH"

build="$here/build"
cmake -S "$here" -B "$build" \
    -DCMAKE_TOOLCHAIN_FILE="$PSPDEV/psp/share/pspdev.cmake" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" -j"$(nproc)"

echo
echo "Built: $build/pxl_psp_bench  (a plain ELF, despite the missing extension --"
echo "                              this cross toolchain doesn't add one)"
[ -f "$build/EBOOT.PBP" ] && echo "        $build/EBOOT.PBP  (copy to ms0:/PSP/GAME/PXLBENCH/ on real hardware)"
echo
echo "Smoke-test in the emulator (no hardware, no GUI). -l is required: without"
echo "it PPSSPPHeadless runs silently and prints nothing, matching output or not."
echo "  PPSSPPHeadless -l \"$build/pxl_psp_bench\" | grep '^I stdout:'"

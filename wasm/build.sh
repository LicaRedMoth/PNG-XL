#!/bin/bash
# Builds the freestanding wasm32 decoder. Needs clang with the wasm32 target
# and wasm-ld, both of which ship with a normal LLVM install -- there is no
# Emscripten dependency and deliberately so, see wasm/README.md.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
cd "$root"

Z=vendor/zstd/lib
[ -d "$Z" ] || { echo "error: $Z missing; run git submodule update --init" >&2; exit 1; }

out=$(mktemp -d); trap 'rm -rf "$out"' EXIT

# ZSTD_DISABLE_ASM drops the x86 assembly path; the legacy and multithread
# switches keep the module to the decompressor and nothing else.
CF=(--target=wasm32 -nostdlib -ffreestanding -O3 -fno-builtin -fno-stack-protector
    -DZSTD_DISABLE_ASM=1 -DZSTD_LEGACY_SUPPORT=0 -DZSTD_MULTITHREAD=0
    -I wasm/include -I src -I "$Z" -I "$Z/common")

srcs=(wasm/shim.c wasm/pxl_wasm.c
      src/pxl_codec_decode.c src/pxl_codec_common.c src/pxl_io.c
      "$Z/common/debug.c" "$Z/common/entropy_common.c" "$Z/common/error_private.c"
      "$Z/common/fse_decompress.c" "$Z/common/xxhash.c" "$Z/common/zstd_common.c"
      "$Z/decompress/huf_decompress.c" "$Z/decompress/zstd_ddict.c"
      "$Z/decompress/zstd_decompress.c" "$Z/decompress/zstd_decompress_block.c")

for f in "${srcs[@]}"; do
    clang "${CF[@]}" -c "$f" -o "$out/$(basename "$f" .c).o"
done

wasm-ld --no-entry --strip-all -O2 --export-memory --initial-memory=1048576 \
    --export=pxl_wasm_alloc --export=pxl_wasm_decode --export=pxl_wasm_decode_rgba \
    --export=pxl_wasm_width --export=pxl_wasm_height --export=pxl_wasm_channels \
    --export=pxl_wasm_depth --export=pxl_wasm_pixels --export=pxl_wasm_pixel_bytes \
    --export=pxl_wasm_rgba --export=pxl_wasm_rgba_bytes --export=pxl_wasm_rgba_channels \
    --export=pxl_wasm_free \
    "$out"/*.o -o wasm/pxl.wasm

printf 'wasm/pxl.wasm  %s bytes' "$(stat -c%s wasm/pxl.wasm)"
command -v brotli >/dev/null && printf '  (brotli: %s)' "$(brotli -q 11 -c wasm/pxl.wasm | wc -c)"
echo

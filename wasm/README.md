# WASM build

A freestanding `wasm32` build of the decoder — no Emscripten, no JavaScript
runtime, no libc beyond the few functions in [`shim.c`](shim.c).

That is a deliberate choice rather than an inconvenience. Emscripten would
supply a complete libc and its own runtime, and the whole argument for this
decoder is that it drags in almost nothing: 174 KB of `.text` natively. A build
that shipped a megabyte of runtime beside it would throw the argument away.

| artefact | bytes |
|---|---:|
| `pxl.wasm` | 83 724 |
| gzip -9 | 30 417 |
| **brotli -q 11** | **24 727** |

The last row is what a browser actually fetches.

## Building

Needs clang with the `wasm32` target and `wasm-ld` — both ship with a normal
LLVM install, nothing else to download:

```sh
wasm/build.sh
```

## Using it

The module exports a flat API because `pxl_decode()` passes structs by value,
which is awkward across the WASM boundary. The host allocates inside linear
memory, writes the compressed file there, and reads results through scalar
getters:

```js
const { instance } = await WebAssembly.instantiate(wasmBytes, {});
const X = instance.exports;

const ptr = X.pxl_wasm_alloc(file.length);
new Uint8Array(X.memory.buffer, ptr, file.length).set(file);

if (X.pxl_wasm_decode_rgba(ptr, file.length)) {
    const w = X.pxl_wasm_width(), h = X.pxl_wasm_height();
    const px = new Uint8ClampedArray(
        X.memory.buffer, X.pxl_wasm_rgba(), X.pxl_wasm_rgba_bytes());
    ctx.putImageData(new ImageData(px, w, h), 0, 0);
}
X.pxl_wasm_free();          /* reclaims the whole arena */
```

`pxl_wasm_decode` is the other entry point: it hands back the file's own pixel
layout — 1/2/4-bit gray, indexed, 16-bit — which is what a converter wants and
what a `<canvas>` does not.

Copy the pixels out before calling `pxl_wasm_free()`: the view points into
linear memory, which the next decode reuses.

## Verifying

[`test.mjs`](test.mjs) decodes a `.pxl` under Node and prints geometry and a
checksum, for comparison against the native decoder:

```sh
node wasm/test.mjs image.pxl
node wasm/bench.mjs image.pxl 15      # decode throughput
```

Verified bit-exact against the native decoder on 1-bit grayscale, 4-bit
indexed, 8-bit RGBA and 16-bit grayscale. Throughput is roughly a third of
native, which is the usual WASM ratio.

## The allocator

`shim.c` provides a bump allocator that never reuses freed blocks. That is the
right shape here, not a shortcut: a decode allocates a handful of buffers, runs,
and is discarded, so the host calls `pxl_wasm_free()` between images and the
arena is reclaimed whole. A general-purpose free list would be more code than
the decoder saves.

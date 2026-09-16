/* Decodes a .pxl through the wasm module and prints what it got, so the result
   can be compared against the native decoder rather than trusted. */
import { readFileSync } from "node:fs";

const wasmBytes = readFileSync(new URL("./pxl.wasm", import.meta.url));
const { instance } = await WebAssembly.instantiate(wasmBytes, {});
const X = instance.exports;

export function decode(fileBytes, rgba = false) {
    const ptr = X.pxl_wasm_alloc(fileBytes.length);
    new Uint8Array(X.memory.buffer, ptr, fileBytes.length).set(fileBytes);
    const ok = rgba ? X.pxl_wasm_decode_rgba(ptr, fileBytes.length)
                    : X.pxl_wasm_decode(ptr, fileBytes.length);
    if (!ok) { X.pxl_wasm_free(); return null; }
    const out = {
        width: X.pxl_wasm_width(), height: X.pxl_wasm_height(),
        channels: X.pxl_wasm_channels(), depth: X.pxl_wasm_depth(),
    };
    const p = rgba ? X.pxl_wasm_rgba() : X.pxl_wasm_pixels();
    const n = rgba ? X.pxl_wasm_rgba_bytes() : X.pxl_wasm_pixel_bytes();
    if (rgba) { out.channels = X.pxl_wasm_rgba_channels(); }
    /* copy out before the arena is reclaimed */
    out.pixels = new Uint8Array(X.memory.buffer, p, n).slice();
    X.pxl_wasm_free();
    return out;
}

if (process.argv[2]) {
    const r = decode(readFileSync(process.argv[2]), process.argv[3] === "rgba");
    if (!r) { console.log("decode failed"); process.exit(1); }
    let sum = 0n;
    for (const b of r.pixels) { sum += BigInt(b); }
    console.log(`${r.width}x${r.height} ch=${r.channels} depth=${r.depth} bytes=${r.pixels.length} sum=${sum}`);
}

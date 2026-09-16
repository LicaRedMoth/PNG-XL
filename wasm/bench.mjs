/* Decode throughput of the wasm module, for comparison with the native figure
   in docs/BENCHMARKS.md. Same shape of measurement: warm up, then median. */
import { readFileSync } from "node:fs";
const wasmBytes = readFileSync(new URL("./pxl.wasm", import.meta.url));
const { instance } = await WebAssembly.instantiate(wasmBytes, {});
const X = instance.exports;
const file = readFileSync(process.argv[2]);
const reps = Number(process.argv[3] || 15);

function once() {
    const ptr = X.pxl_wasm_alloc(file.length);
    new Uint8Array(X.memory.buffer, ptr, file.length).set(file);
    const t0 = performance.now();
    const ok = X.pxl_wasm_decode(ptr, file.length);
    const t1 = performance.now();
    const n = ok ? X.pxl_wasm_pixel_bytes() : 0;
    const w = X.pxl_wasm_width(), h = X.pxl_wasm_height();
    X.pxl_wasm_free();
    return { ms: t1 - t0, n, w, h, ok };
}
once();                                   /* warm up */
const t = [];
let info = null;
for (let i = 0; i < reps; i++) { const r = once(); if (!r.ok) { console.log("failed"); process.exit(1); } t.push(r.ms); info = r; }
t.sort((a, b) => a - b);
const med = t[t.length >> 1];
console.log(`${info.w}x${info.h}  ${med.toFixed(2)} ms  ${(info.n / 1048576 / (med / 1000)).toFixed(1)} MB/s`);

/* Asserts compile away: the decoder is fuzzed natively, and a trap in the
   browser would be a worse failure mode than the error paths zstd already has. */
#ifndef PXL_WASM_ASSERT_H
#define PXL_WASM_ASSERT_H
#define assert(x) ((void)0)
#endif

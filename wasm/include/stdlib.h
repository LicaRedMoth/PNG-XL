/* Minimal stdlib for the freestanding wasm32 build. The allocator lives in
   wasm/shim.c; nothing here has a definition, these are declarations only. */
#ifndef PXL_WASM_STDLIB_H
#define PXL_WASM_STDLIB_H
#include <stddef.h>
void*  malloc(size_t);
void*  calloc(size_t, size_t);
void*  realloc(void*, size_t);
void   free(void*);
/* zstd calls abort() on states it considers impossible. There is no host to
   report to, so trap: an unreachable instruction faults the module rather than
   letting it continue with corrupt state. */
static inline void abort(void) { __builtin_trap(); }
static inline void exit(int c) { (void)c; __builtin_trap(); }
#endif

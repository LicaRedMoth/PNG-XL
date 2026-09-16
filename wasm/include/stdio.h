/* zstd's debug builds print; release builds do not call any of this. Declared
   so the headers compile, never defined, so a stray call fails at link time
   rather than silently pulling in a runtime. */
#ifndef PXL_WASM_STDIO_H
#define PXL_WASM_STDIO_H
#include <stddef.h>
typedef struct _PXL_FILE FILE;
extern FILE* stderr;
int fprintf(FILE*, const char*, ...);
int printf(const char*, ...);
#endif

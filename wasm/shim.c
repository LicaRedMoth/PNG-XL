/** \file shim.c
    \brief The few libc pieces a freestanding wasm32 build still needs.

    Built without Emscripten on purpose. Emscripten would supply a full libc and
    a JavaScript runtime, and the point of this module is the opposite: the
    decoder is 174 KB of .text natively because it drags in almost nothing, and
    a WASM build that shipped a megabyte of runtime alongside it would throw
    that away. wasm-ld links this and nothing else.

    The allocator is a bump allocator over the linear memory that never reuses
    freed blocks. That is the right shape here rather than a shortcut: a decode
    allocates a handful of buffers, runs, and is thrown away, so the host calls
    pxl_wasm_reset() between images and the whole arena is reclaimed at once.
    A general-purpose free list would be more code than the decoder saves.
*/

typedef unsigned long size_t;

extern unsigned char __heap_base;

static unsigned char* g_brk;
static unsigned char* g_arena_start;

/* Linear memory grows in 64 KiB pages; __builtin_wasm_memory_grow returns the
   previous size in pages, or -1 when the host refuses. */
static int grow_to(unsigned char* need)
{
    unsigned long have = (unsigned long)__builtin_wasm_memory_size(0) * 65536UL;
    unsigned long want = (unsigned long)need;
    unsigned long pages;
    if (want <= have) { return 1; }
    pages = (want - have + 65535UL) / 65536UL;
    return __builtin_wasm_memory_grow(0, (unsigned long)pages) != (unsigned long)-1;
}

void* malloc(size_t n)
{
    unsigned char* p;
    if (!g_brk) { g_brk = g_arena_start = &__heap_base; }
    n = (n + 15UL) & ~15UL;             /* keep 16-byte alignment for zstd */
    p = g_brk;
    if (!grow_to(p + n)) { return 0; }
    g_brk = p + n;
    return p;
}

void free(void* p) { (void)p; }         /* see the file comment */

void* calloc(size_t n, size_t sz)
{
    size_t total = n * sz;
    unsigned char* p;
    size_t i;
    if (sz && total / sz != n) { return 0; }   /* overflow */
    p = (unsigned char*)malloc(total);
    if (p) { for (i = 0; i < total; ++i) { p[i] = 0; } }
    return p;
}

void* realloc(void* old, size_t n)
{
    /* Only ever grows, and copies conservatively: without block headers the
       old size is unknown, so it copies n bytes, which is safe because the
       arena beyond the old block is either unused or about to be. */
    unsigned char* p = (unsigned char*)malloc(n);
    unsigned char* q = (unsigned char*)old;
    size_t i;
    if (p && q) { for (i = 0; i < n; ++i) { p[i] = q[i]; } }
    return p;
}

void* memcpy(void* d, const void* s, size_t n)
{
    unsigned char* a = (unsigned char*)d;
    const unsigned char* b = (const unsigned char*)s;
    size_t i;
    for (i = 0; i < n; ++i) { a[i] = b[i]; }
    return d;
}

void* memmove(void* d, const void* s, size_t n)
{
    unsigned char* a = (unsigned char*)d;
    const unsigned char* b = (const unsigned char*)s;
    size_t i;
    if (a == b || n == 0) { return d; }
    if (a < b) { for (i = 0; i < n; ++i) { a[i] = b[i]; } }
    else       { for (i = n; i-- > 0; )  { a[i] = b[i]; } }
    return d;
}

void* memset(void* d, int c, size_t n)
{
    unsigned char* a = (unsigned char*)d;
    size_t i;
    for (i = 0; i < n; ++i) { a[i] = (unsigned char)c; }
    return d;
}

int memcmp(const void* x, const void* y, size_t n)
{
    const unsigned char* a = (const unsigned char*)x;
    const unsigned char* b = (const unsigned char*)y;
    size_t i;
    for (i = 0; i < n; ++i) { if (a[i] != b[i]) { return a[i] < b[i] ? -1 : 1; } }
    return 0;
}

size_t strlen(const char* s) { size_t n = 0; while (s[n]) { ++n; } return n; }

/* Reclaims every allocation since the module was instantiated. The host calls
   this between images; there is nothing to free individually. */
void pxl_wasm_reset(void) { g_brk = g_arena_start ? g_arena_start : &__heap_base; }

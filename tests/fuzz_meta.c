/* Fuzzes pxl_meta_extract and pxl_meta_inject, which the decoder fuzzer never
   reaches. Builds a PNG-shaped buffer, then mutates chunk lengths, types and
   the truncation point. Roundtrips extract -> inject so both bound checks run. */
#include "pxl_meta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long st = 99;
static unsigned r(void){ st = st*6364136223846793005UL+1442695040888963407UL; return (unsigned)((st>>33)&0xFFFFFFFFUL); }

int main(int argc, char** argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 500000;
    static const unsigned char sig[8] = {137,'P','N','G',13,10,26,10};
    long it;
    for (it = 0; it < iters; ++it) {
        unsigned char buf[512];
        size_t n = 8 + (r() % (sizeof(buf) - 8));
        pxl_buffer meta, back;
        size_t p;
        memcpy(buf, sig, 8);
        for (p = 8; p < n; ++p) buf[p] = (unsigned char)r();
        /* sprinkle real chunk types so the walker takes its live paths */
        if (n > 20) {
            static const char* t[] = {"tEXt","gAMA","sBIT","iCCP","IDAT","IEND","cHRM"};
            memcpy(buf + 12, t[r() % 7], 4);
        }
        meta = pxl_meta_extract(buf, n, r() & 1);
        if (meta.data) {
            back = pxl_meta_inject(buf, n, meta.data, meta.size);
            free(back.data);
            free(meta.data);
        }
    }
    printf("metafuzz: %ld iterations, no crash\n", iters);
    return 0;
}

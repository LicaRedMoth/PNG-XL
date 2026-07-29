/* Decode-only profiling target: read a .pxl, decode it N times, nothing else.
 *
 * Exists so that a sampling profiler (perf) or an instrumented one (callgrind)
 * sees only the decoder. bench/rawdec.c cannot serve this purpose: it encodes
 * through libpng and QOI first, so their symbols land in the same profile.
 *
 *   bench/profdec <image.pxl> [reps]
 */
#include <stdio.h>
#include <stdlib.h>

#include "pxl.h"

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <image.pxl> [reps]\n", argv[0]);
        return 2;
    }
    int reps = argc > 2 ? atoi(argv[2]) : 50;
    if (reps < 1) reps = 1;

    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 1; }

    unsigned char* raw = (unsigned char*)malloc((size_t)n);
    if (!raw) { fclose(f); return 1; }
    if (fread(raw, 1, (size_t)n, f) != (size_t)n) {
        free(raw); fclose(f); return 1;
    }
    fclose(f);

    pxl_buffer file = { raw, (size_t)n };
    unsigned long long sum = 0;
    unsigned w = 0, h = 0;

    for (int i = 0; i < reps; i++) {
        pxl_image image = pxl_decode(file);
        if (!image.buffer.data) { free(raw); return 1; }
        /* Touch the pixels so the decode cannot be optimised away. */
        for (size_t k = 0; k < image.buffer.size; k += 4096)
            sum += image.buffer.data[k];
        w = image.width; h = image.height;
        pxl_free(&image.buffer);
    }

    printf("%ux%u reps=%d sum=%llu\n", w, h, reps, sum);
    free(raw);
    return 0;
}

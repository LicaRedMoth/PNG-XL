/* Minimal PXL decoder: file -> raw pixels, nothing else.
   Exists only to measure how much .text a real decoder pulls in. */
#include <stdio.h>
#include <stdlib.h>

#include "pxl.h"

int main(int argc, char** argv) {
    if (argc < 2) return 2;

    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 1; }

    unsigned char* raw = (unsigned char*)malloc((size_t)n);
    if (!raw) { fclose(f); return 1; }
    if (fread(raw, 1, (size_t)n, f) != (size_t)n) { free(raw); fclose(f); return 1; }
    fclose(f);

    pxl_buffer file  = { raw, (size_t)n };
    pxl_image  image = pxl_decode(file);
    if (!image.buffer.data) { free(raw); return 1; }

    /* Touch the pixels so nothing can be optimised away. */
    unsigned long long sum = 0;
    for (size_t i = 0; i < image.buffer.size; i++) sum += image.buffer.data[i];
    printf("%ux%u sum=%llu\n", image.width, image.height, sum);

    pxl_free(&image.buffer);
    free(raw);
    return 0;
}

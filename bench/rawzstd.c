/** \file rawzstd.c
    \brief Compresses a raw file with exactly apxl_encode's real default zstd
           parameters, and prints the compressed size.

    Exists so an experiment outside the codec (e.g. comparing two different
    byte layouts of the same pixels) can be scored by the same compression
    apxl_encode() would actually apply, without duplicating apxl_encode's
    container-writing logic just to get at its zstd call. See
    src/apxl_codec.c for the constants this must be kept in sync with.

    Usage:
      bench/rawzstd <file>   [env: RAWZSTD_LEVEL, default APXL_LEVEL_DEFAULT]
    Prints: bytes=<input size> csize=<compressed size>
*/
#include "../src/apxl.h" /* APXL_LEVEL_DEFAULT */

#include <zstd.h>

#include <stdio.h>
#include <stdlib.h>

/* Must match src/apxl_codec.c's APXL_LDM_MIN_LEVEL / APXL_WINDOW_LOG. */
#define LDM_MIN_LEVEL 10
#define LDM_WINDOW_LOG 27

int main(int argc, char** argv)
{
    int level = APXL_LEVEL_DEFAULT;
    const char* env = getenv("RAWZSTD_LEVEL");
    FILE* f;
    long size;
    unsigned char* raw;
    ZSTD_CCtx* cctx;
    size_t bound, csize;
    unsigned char* out;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 2;
    }
    if (env && atoi(env) > 0) { level = atoi(env); }

    f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    raw = (unsigned char*)malloc((size_t)size);
    if (!raw || fread(raw, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "read failed\n");
        fclose(f);
        return 1;
    }
    fclose(f);

    cctx = ZSTD_createCCtx();
    if (!cctx) { fprintf(stderr, "ZSTD_createCCtx failed\n"); return 1; }
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
    if (level >= LDM_MIN_LEVEL) {
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, LDM_WINDOW_LOG);
    }
    bound = ZSTD_compressBound((size_t)size);
    out = (unsigned char*)malloc(bound);
    if (!out) { fprintf(stderr, "out of memory\n"); return 1; }
    csize = ZSTD_compress2(cctx, out, bound, raw, (size_t)size);
    ZSTD_freeCCtx(cctx);
    free(raw);
    free(out);
    if (ZSTD_isError(csize)) { fprintf(stderr, "compress failed\n"); return 1; }

    printf("bytes=%ld csize=%zu\n", size, csize);
    return 0;
}

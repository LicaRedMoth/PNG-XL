/** \file apxl_encode_stubs.c
    \brief Link-time stubs for the zstd compressor symbols apxl_encode needs.

    src/apxl_codec.c has no encode/decode split the way pxl_codec_encode.c /
    pxl_codec_decode.c already do -- main.c only ever calls apxl_decode, but
    apxl_encode lives in the same translation unit and references zstd's
    *compressor* (ZSTD_compress2 et al.), which this decode-only PSP target's
    zstd source list deliberately excludes (see psp/CMakeLists.txt), the same
    way wasm/build.sh's does. Pulling the real compressor files in just to
    satisfy an unreachable function would cost several KB of flash for zero
    benefit -- apxl_encode is dead code here.

    These stubs exist only to satisfy the linker. They abort() if ever
    actually reached, which would mean apxl_encode got called from somewhere
    after all and this file needs revisiting, not that the stub silently
    produced a wrong result.
*/
#include <zstd.h>

#include <stdlib.h>

size_t ZSTD_compressBound(size_t srcSize)
{
    (void)srcSize;
    abort();
}

ZSTD_CCtx* ZSTD_createCCtx(void)
{
    abort();
}

size_t ZSTD_CCtx_setParameter(ZSTD_CCtx* cctx, ZSTD_cParameter param, int value)
{
    (void)cctx; (void)param; (void)value;
    abort();
}

size_t ZSTD_compress2(ZSTD_CCtx* cctx, void* dst, size_t dstCapacity,
                      const void* src, size_t srcSize)
{
    (void)cctx; (void)dst; (void)dstCapacity; (void)src; (void)srcSize;
    abort();
}

size_t ZSTD_freeCCtx(ZSTD_CCtx* cctx)
{
    (void)cctx;
    abort();
}

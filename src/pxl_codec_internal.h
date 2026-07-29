/** \file pxl_codec_internal.h
    \brief Helpers shared by the encode and decode halves of the codec.

    The codec is split across three translation units on purpose:

      - pxl_codec_common.c  geometry/validation/filter primitives (both sides)
      - pxl_codec_encode.c  pxl_encode, pack_*, filter selection
      - pxl_codec_decode.c  pxl_decode, the streaming decoder, unpack_*

    The split is what lets a decode-only program drop the encoder at link time.
    While pxl_encode and pxl_decode lived in one object file, any binary that
    called pxl_decode also pulled in the zstd compressor (~317 KB of .text),
    because the linker cannot discard half an object file.

    Nothing here is public API: no symbol is declared in pxl.h, and all of it
    may change without notice.
*/
#ifndef PXL_CODEC_INTERNAL_H
#define PXL_CODEC_INTERNAL_H

#include "pxl.h"

#include <stddef.h>
#include <stdint.h>

/* Decode-side resource limits. A 24-byte header can claim any geometry, and
   the filtered stream that backs it compresses to almost nothing (a run of
   zeros), so without a cap a few-hundred-byte file forces multi-gigabyte
   allocations in pxl_decode/stream_begin. Mirrors APNG_MAX_DIM/
   APNG_MAX_PIXELS in apng.c. */
#define PXL_MAX_DIM     1000000u
#define PXL_MAX_PIXELS  ((uint64_t)1 << 28)

/* Filter geometry for one image. The filters work on byte rows, so a sub-byte
   depth (1/2/4) is filtered as a row of packed bytes: filter_width becomes the
   packed row length and pixel_bytes becomes 1, which is exactly what PNG does
   for depths below 8. For 8/16-bit rows this is the old behaviour unchanged. */
typedef struct {
    unsigned pixel_bytes;  /* bytes per filter unit (1 for sub-byte depths) */
    uint32_t filter_width; /* units per row as seen by the filters */
    size_t   row_bytes;    /* packed bytes per scanline */
    size_t   raw_bytes;    /* row_bytes * height */
} pxl_geometry;

/* Returns 1 and fills g on success, 0 if the geometry is invalid or too big. */
int pxl_geometry_of(uint32_t width, uint32_t height, uint8_t channels,
                    uint8_t depth, pxl_geometry* g);

/** Size of the filtered stream for \a filter, or 0 if the filter is unknown. */
size_t pxl_filtered_size(uint8_t filter, uint32_t width, uint32_t height,
                         unsigned pixel_bytes);

/* Adaptive per-row filter types, values as stored in the filtered stream. */
#define PXL_ROWF_NONE 0
#define PXL_ROWF_SUB  1
#define PXL_ROWF_UP   2
#define PXL_ROWF_AVG  3
#define PXL_ROWF_PAETH 4

/** PNG's Paeth predictor: picks whichever of a/b/c is closest to a+b-c.

    Inline and branchless on purpose. This runs once per byte and PAETH is the
    row filter chosen for ~99% of photographic rows, so it is the hottest code
    in the decoder. Out of line it cost a call per byte across a TU boundary;
    with data-dependent branches it also mispredicted on noisy images.

    The branchless form works on the standard identity: the winner is `a` when
    pa is the smallest, else `b` when pb <= pc, else `c`. Each comparison
    becomes an arithmetic-shift mask (0 or -1) instead of a jump. Values are
    bytes, so p fits in int and no overflow is possible. */
static inline uint8_t pxl_paeth(uint8_t a, uint8_t b, uint8_t c)
{
    int p  = (int)a + (int)b - (int)c;
    int pa = p > (int)a ? p - (int)a : (int)a - p;
    int pb = p > (int)b ? p - (int)b : (int)b - p;
    int pc = p > (int)c ? p - (int)c : (int)c - p;
    /* m_a = -1 when a wins; m_b = -1 when b beats c. */
    int m_a = -(int)(pa <= pb && pa <= pc);
    int m_b = -(int)(pb <= pc);
    int bc  = ((int)b & m_b) | ((int)c & ~m_b);
    return (uint8_t)(((int)a & m_a) | (bc & ~m_a));
}

#endif /* PXL_CODEC_INTERNAL_H */

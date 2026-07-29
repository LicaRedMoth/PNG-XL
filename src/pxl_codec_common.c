/** \file pxl_codec_common.c
    \brief Geometry, accessors and filter primitives shared by both halves.

    Split out of pxl_codec.c so that a decode-only binary does not drag in the
    zstd compressor: see pxl_codec_internal.h for the rationale.
*/
#include "pxl.h"
#include "pxl_codec_internal.h"
#include "pxl_format.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

uint8_t pxl_bit_depth(const pxl_image* img)
{
    if (!img) { return 0; }
    if (img->bit_depth) { return img->bit_depth; }
    return (uint8_t)(img->bytes_per_channel * 8u);
}


unsigned pxl_palette_count(const pxl_image* img)
{
    if (!img || !img->palette.data) { return 0; }
    return (unsigned)(img->palette.size / 3u);
}


int pxl_is_indexed(const pxl_image* img)
{
    return pxl_palette_count(img) > 0;
}


size_t pxl_row_bytes(const pxl_image* img)
{
    if (!img) { return 0; }
    return pxl_row_bytes_of(img->width, img->channels, pxl_bit_depth(img));
}


/* Returns 1 if the header geometry is within the decode limits. */
static int geometry_ok(uint32_t width, uint32_t height, unsigned pixel_bytes)
{
    if (width == 0 || height == 0) {
        return 0;
    }
    if (width > PXL_MAX_DIM || height > PXL_MAX_DIM) {
        return 0;
    }
    if ((uint64_t)width * height > PXL_MAX_PIXELS) {
        return 0;
    }
    /* With pixels capped at 2^28 and pixel_bytes at 8, width*height*pixel_bytes
       tops out at 2^31 and cannot overflow size_t on any supported target. */
    (void)pixel_bytes;
    return 1;
}


/* Returns 1 and fills g on success, 0 if the geometry is invalid or too big.
   pxl_geometry itself lives in pxl_codec_internal.h. */
int pxl_geometry_of(uint32_t width, uint32_t height, uint8_t channels,
                       uint8_t depth, pxl_geometry* g)
{
    size_t row = pxl_row_bytes_of(width, channels, depth);

    if (row == 0 || !geometry_ok(width, height, 1)) {
        return 0;
    }
    if (depth >= 8) {
        g->pixel_bytes = (unsigned)channels * (unsigned)(depth / 8u);
        g->filter_width = width;
    } else {
        g->pixel_bytes = 1;
        /* Packed rows stay under PXL_MAX_DIM bytes, so this fits uint32_t. */
        g->filter_width = (uint32_t)row;
    }
    if (!geometry_ok(g->filter_width, height, g->pixel_bytes)) {
        return 0;
    }
    g->row_bytes = row;
    g->raw_bytes = row * (size_t)height;
    return 1;
}


/*----------------------------------------------------------------------------
  Adaptive PNG-style per-row filters (None/Sub/Up/Average/Paeth).

  These operate byte-wise. The left neighbor `a` is the byte `bpp` positions
  back (the same byte in the previous pixel), `b` is the byte directly above,
  `c` is the byte above-left -- exactly as in the PNG specification. The
  encoder chooses, per row, the filter minimizing the sum of absolute signed
  residuals (PNG's standard minimum-sum-of-absolute-differences heuristic).

  The filtered stream stores, per row: one filter-type byte followed by the
  filtered row. So its length is height * (1 + row_stride), larger than the raw
  pixels -- but far more compressible.
----------------------------------------------------------------------------*/


/* pxl_paeth now lives in pxl_codec_internal.h as static inline: it is called
   once per byte and must not cost a cross-TU call. */


static size_t adaptive_filtered_size(uint32_t width, uint32_t height, unsigned pixel_bytes)
{
    size_t stride = (size_t)width * pixel_bytes;
    return (size_t)height * (1 + stride);
}


/* Size of the filtered buffer produced by `filter` for the given geometry. */
size_t pxl_filtered_size(uint8_t filter, uint32_t width, uint32_t height,
                            unsigned pixel_bytes)
{
    if (filter == PXL_FILTER_ADAPTIVE) {
        return adaptive_filtered_size(width, height, pixel_bytes);
    }
    return (size_t)width * height * pixel_bytes;
}


/* Reads sample \p x of a packed MSB-first row at sub-byte \p depth. */
static unsigned sample_at(const unsigned char* row, uint32_t x, uint8_t depth)
{
    unsigned per_byte = 8u / depth;
    unsigned mask     = (1u << depth) - 1u;
    unsigned shift    = 8u - depth * (unsigned)(x % per_byte) - depth;
    return (unsigned)((row[x / per_byte] >> shift) & mask);
}


pxl_image pxl_image_expand(const pxl_image* img)
{
    pxl_image out;
    uint8_t   depth;
    unsigned  count, has_alpha;
    size_t    src_row, dst_row, total;
    uint32_t  x, y;

    memset(&out, 0, sizeof(out));
    if (!img || !img->buffer.data) {
        return out;
    }
    depth   = pxl_bit_depth(img);
    src_row = pxl_row_bytes(img);
    if (src_row == 0 || img->buffer.size < src_row * (size_t)img->height) {
        return out;
    }

    /* Nothing to expand: hand back a deep copy of the pixels only. */
    if (!pxl_is_indexed(img) && depth >= 8) {
        out.buffer.data = (unsigned char*)malloc(img->buffer.size);
        if (!out.buffer.data) { return out; }
        memcpy(out.buffer.data, img->buffer.data, img->buffer.size);
        out.buffer.size       = img->buffer.size;
        out.width             = img->width;
        out.height            = img->height;
        out.channels          = img->channels;
        out.bytes_per_channel = img->bytes_per_channel;
        out.bit_depth         = depth;
        return out;
    }

    count     = pxl_palette_count(img);
    has_alpha = (count && img->palette_alpha.data &&
                 img->palette_alpha.size) ? 1u : 0u;
    out.width             = img->width;
    out.height            = img->height;
    out.channels          = count ? (uint8_t)(has_alpha ? 4 : 3) : 1;
    out.bytes_per_channel = 1;
    out.bit_depth         = 8;

    dst_row = pxl_row_bytes(&out);
    if (dst_row == 0 || dst_row / out.channels < img->width) {
        memset(&out, 0, sizeof(out));
        return out;
    }
    total = dst_row * (size_t)img->height;
    out.buffer.data = (unsigned char*)malloc(total);
    if (!out.buffer.data) {
        memset(&out, 0, sizeof(out));
        return out;
    }
    out.buffer.size = total;

    for (y = 0; y < img->height; ++y) {
        const unsigned char* in = img->buffer.data + (size_t)y * src_row;
        unsigned char* dst      = out.buffer.data + (size_t)y * dst_row;

        for (x = 0; x < img->width; ++x) {
            unsigned s = (depth == 8) ? in[x] : sample_at(in, x, depth);

            if (count) {
                /* Indices were validated on decode, but this entry point is
                   public: clamp rather than read past the palette. */
                if (s >= count) { s = count - 1u; }
                *dst++ = img->palette.data[s * 3u + 0u];
                *dst++ = img->palette.data[s * 3u + 1u];
                *dst++ = img->palette.data[s * 3u + 2u];
                if (has_alpha) {
                    *dst++ = (s < img->palette_alpha.size)
                             ? img->palette_alpha.data[s] : 0xFFu;
                }
            } else {
                /* Gray 1/2/4 -> 8 bits, scaled so the max value stays white. */
                *dst++ = (unsigned char)(s * 255u / ((1u << depth) - 1u));
            }
        }
    }
    return out;
}


void pxl_free(pxl_buffer* buffer)
{
    if (buffer && buffer->data) {
        free(buffer->data);
        buffer->data = NULL;
        buffer->size = 0;
    }
}


void pxl_image_free(pxl_image* img)
{
    if (img) {
        pxl_free(&img->buffer);
        pxl_free(&img->metadata);
        pxl_free(&img->palette);
        pxl_free(&img->palette_alpha);
    }
}


const char* pxl_version(void)
{
    return "1.5.0";
}

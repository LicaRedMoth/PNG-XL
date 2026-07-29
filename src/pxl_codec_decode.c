/** \file pxl_codec_decode.c
    \brief Decoder: zstd decompression, reverse filters, streaming decoder.

    References only ZSTD_decompress* and friends, never the compressor, so a
    program that decodes but never encodes links a much smaller binary.
*/
#include "pxl.h"
#include "pxl_codec_internal.h"
#include "pxl_format.h"

#include <zstd.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Rejects indices that address a palette entry that does not exist.

   This is a hard requirement, not a nicety: consumers (the Qt plugin, the
   Dolphin thumbnailer, ffmpeg) index the palette directly with the sample
   value, so an out-of-range index in a crafted or corrupt file would be an
   out-of-bounds read in *their* address space. A palette shorter than the
   depth allows (a 5-entry PLTE at 8 bits, which PNG permits) makes that
   reachable, so the check has to look at every sample.

   `rows` * `row_bytes` is the pixel buffer; at depths below 8 the padding bits
   at the end of a row are not samples and must not be validated. Returns 1 if
   every index is < count. */
static int row_indices_ok(const uint8_t* row, uint32_t width, uint8_t depth,
                          unsigned count)
{
    uint32_t x;

    if (depth == 8) {
        for (x = 0; x < width; ++x) {
            if (row[x] >= count) { return 0; }
        }
        return 1;
    }
    /* Sub-byte depths: unpack MSB-first, ignoring row padding. */
    {
        unsigned per_byte = 8u / depth;
        unsigned mask = (1u << depth) - 1u;
        for (x = 0; x < width; ++x) {
            unsigned shift = 8u - depth * (unsigned)(x % per_byte) - depth;
            if ((unsigned)((row[x / per_byte] >> shift) & mask) >= count) {
                return 0;
            }
        }
    }
    return 1;
}


/* Whole-buffer form of the above, for the one-shot decoder. */
static int indices_ok(const uint8_t* pixels, size_t row_bytes, uint32_t width,
                      uint32_t height, uint8_t depth, unsigned count)
{
    uint32_t y;
    for (y = 0; y < height; ++y) {
        if (!row_indices_ok(pixels + (size_t)y * row_bytes, width, depth,
                            count)) {
            return 0;
        }
    }
    return 1;
}


/* Reconstruct one DELTA row. Each row is independent (prev resets to 0), so
   this is reused by the streaming decoder. */
static void unpack_delta_row(const uint8_t* input, uint8_t* output,
                             uint32_t width, unsigned pixel_bytes)
{
    uint8_t prev[8] = { 0 };
    uint32_t x;
    unsigned i;
    for (x = 0; x < width; ++x) {
        for (i = 0; i < pixel_bytes && i < sizeof(prev); ++i) {
            uint8_t a = (uint8_t)(input[i] + prev[i]);
            output[i] = a;
            prev[i] = a;
        }
        input += pixel_bytes;
        output += pixel_bytes;
    }
}


static void unpack_delta(const uint8_t* input, uint8_t* output,
                         uint32_t width, uint32_t height, unsigned pixel_bytes)
{
    uint32_t y;
    size_t stride = (size_t)width * pixel_bytes;
    for (y = 0; y < height; ++y) {
        unpack_delta_row(input + (size_t)y * stride,
                         output + (size_t)y * stride, width, pixel_bytes);
    }
}


static void unpack_bcif3(const uint8_t* input, uint8_t* output,
                         uint32_t width, uint32_t height)
{
    const uint32_t plane = width * height;
    const uint8_t* in_y = input;
    const uint8_t* in_u = input + plane;
    const uint8_t* in_v = input + plane * 2;
    uint32_t row, x;

    for (row = 0; row < height; ++row) {
        uint8_t prev[3] = { 0 };
        for (x = 0; x < width; ++x) {
            uint8_t Y = *in_y++;
            uint8_t U = *in_u++;
            uint8_t V = *in_v++;

            uint8_t B = Y;
            uint8_t G = (uint8_t)(U + B);
            uint8_t r = (uint8_t)(G - V);
            uint8_t g = G;
            uint8_t b = B;

            r = (uint8_t)(r + prev[0]);
            g = (uint8_t)(g + prev[1]);
            b = (uint8_t)(b + prev[2]);

            output[0] = r;
            output[1] = g;
            output[2] = b;
            prev[0] = r;
            prev[1] = g;
            prev[2] = b;

            output += 3;
        }
    }
}


static void unpack_bcif4(const uint8_t* input, uint8_t* output,
                         uint32_t width, uint32_t height)
{
    const uint32_t plane = width * height;
    const uint8_t* in_y = input;
    const uint8_t* in_u = input + plane;
    const uint8_t* in_v = input + plane * 2;
    const uint8_t* in_a = input + plane * 3;
    uint32_t row, x;

    for (row = 0; row < height; ++row) {
        uint8_t prev[4] = { 0 };
        for (x = 0; x < width; ++x) {
            uint8_t Y = *in_y++;
            uint8_t U = *in_u++;
            uint8_t V = *in_v++;
            uint8_t a = *in_a++;

            uint8_t B = Y;
            uint8_t G = (uint8_t)(U + B);
            uint8_t r = (uint8_t)(G - V);
            uint8_t g = G;
            uint8_t b = B;

            r = (uint8_t)(r + prev[0]);
            g = (uint8_t)(g + prev[1]);
            b = (uint8_t)(b + prev[2]);
            a = (uint8_t)(a + prev[3]);

            output[0] = r;
            output[1] = g;
            output[2] = b;
            output[3] = a;
            prev[0] = r;
            prev[1] = g;
            prev[2] = b;
            prev[3] = a;

            output += 4;
        }
    }
}

/* Reconstruct one row filtered with `type` from `in` (stride bytes) into the
   output row `cur` (which doubles as the source of already-reconstructed
   left/above-left samples). */
static void rowfilter_decode_n(uint8_t type, const uint8_t* in, const uint8_t* prev,
                               uint8_t* cur, size_t stride, unsigned bpp)
{
    size_t i;
    size_t head = (size_t)bpp < stride ? (size_t)bpp : stride;

    /* One tight loop per filter type. The filter is constant for the whole row,
       so the switch belongs outside the per-byte loop, not inside it. The first
       `bpp` bytes are peeled off because there the left (a) and above-left (c)
       neighbours are defined as zero; the bulk loop then needs no bounds test.
       When `prev` is NULL the above (b) and above-left (c) neighbours are zero,
       which collapses UP to NONE, PAETH to SUB and AVG to a >> 1. */
    if (prev == NULL) {
        switch (type) {
            case PXL_ROWF_SUB:
            case PXL_ROWF_PAETH:
                for (i = 0; i < head; ++i) cur[i] = in[i];
                for (i = head; i < stride; ++i)
                    cur[i] = (uint8_t)(in[i] + cur[i - bpp]);
                return;
            case PXL_ROWF_AVG:
                for (i = 0; i < head; ++i) cur[i] = in[i];
                for (i = head; i < stride; ++i)
                    cur[i] = (uint8_t)(in[i] + (cur[i - bpp] >> 1));
                return;
            default: /* NONE, UP */
                memcpy(cur, in, stride);
                return;
        }
    }

    switch (type) {
        case PXL_ROWF_SUB:
            for (i = 0; i < head; ++i) cur[i] = in[i];
            for (i = head; i < stride; ++i)
                cur[i] = (uint8_t)(in[i] + cur[i - bpp]);
            return;
        case PXL_ROWF_UP:
            for (i = 0; i < stride; ++i)
                cur[i] = (uint8_t)(in[i] + prev[i]);
            return;
        case PXL_ROWF_AVG:
            for (i = 0; i < head; ++i)
                cur[i] = (uint8_t)(in[i] + (prev[i] >> 1));
            for (i = head; i < stride; ++i)
                cur[i] = (uint8_t)(in[i] +
                                   (uint8_t)(((int)cur[i - bpp] + (int)prev[i]) >> 1));
            return;
        case PXL_ROWF_PAETH:
            for (i = 0; i < head; ++i)
                cur[i] = (uint8_t)(in[i] + prev[i]);
            for (i = head; i < stride; ++i)
                cur[i] = (uint8_t)(in[i] +
                                   pxl_paeth(cur[i - bpp], prev[i], prev[i - bpp]));
            return;
        default: /* NONE */
            memcpy(cur, in, stride);
            return;
    }
}


/* Left-neighbour filters with the pixel stride as a compile-time constant. That
   turns `cur[i - bpp]` into a fixed offset, so the compiler can unroll a whole
   pixel per iteration and interleave the BPP independent dependency chains
   instead of walking one byte at a time.

   Only AVG and PAETH are instantiated, and only for a non-NULL `prev`. Keeping
   the decoder small matters as much as keeping it fast here, so everything that
   does not pay for its code size goes through rowfilter_decode_n: NONE and UP do
   not depend on the pixel width, the first row of an image is one row out of
   `height`, and SUB accounts for ~6% of the filtered bytes this corpus actually
   produces, against AVG ~70% and PAETH ~20% (see docs/BENCHMARKS.md). */
#define PXL_ROWFILTER_DECODE_FIXED(NAME, BPP)                                  \
static void NAME(uint8_t type, const uint8_t* in, const uint8_t* prev,         \
                 uint8_t* cur, size_t stride)                                  \
{                                                                              \
    size_t i;                                                                  \
    size_t head = (size_t)(BPP) < stride ? (size_t)(BPP) : stride;             \
                                                                               \
    switch (type) {                                                            \
        case PXL_ROWF_AVG:                                                     \
            for (i = 0; i < head; ++i)                                         \
                cur[i] = (uint8_t)(in[i] + (prev[i] >> 1));                    \
            for (i = head; i < stride; ++i)                                    \
                cur[i] = (uint8_t)(in[i] +                                     \
                    (uint8_t)(((int)cur[i - (BPP)] + (int)prev[i]) >> 1));     \
            return;                                                            \
        case PXL_ROWF_PAETH:                                                   \
            for (i = 0; i < head; ++i)                                         \
                cur[i] = (uint8_t)(in[i] + prev[i]);                           \
            for (i = head; i < stride; ++i)                                    \
                cur[i] = (uint8_t)(in[i] +                                     \
                    pxl_paeth(cur[i - (BPP)], prev[i], prev[i - (BPP)]));      \
            return;                                                            \
        default:                                                               \
            memcpy(cur, in, stride);                                           \
            return;                                                            \
    }                                                                          \
}

PXL_ROWFILTER_DECODE_FIXED(rowfilter_decode_1, 1)
PXL_ROWFILTER_DECODE_FIXED(rowfilter_decode_2, 2)
PXL_ROWFILTER_DECODE_FIXED(rowfilter_decode_3, 3)
PXL_ROWFILTER_DECODE_FIXED(rowfilter_decode_4, 4)

/* Dispatch one row to the specialization matching its pixel width. */
static void rowfilter_decode(uint8_t type, const uint8_t* in, const uint8_t* prev,
                            uint8_t* cur, size_t stride, unsigned bpp)
{
    size_t i;

    if (type == PXL_ROWF_NONE) {
        memcpy(cur, in, stride);
        return;
    }
    if (type == PXL_ROWF_UP) {
        if (prev == NULL) {
            memcpy(cur, in, stride);
        } else {
            for (i = 0; i < stride; ++i) {
                cur[i] = (uint8_t)(in[i] + prev[i]);
            }
        }
        return;
    }
    if (prev == NULL || type == PXL_ROWF_SUB) {
        rowfilter_decode_n(type, in, prev, cur, stride, bpp);
        return;
    }
    switch (bpp) {
        case 1:  rowfilter_decode_1(type, in, prev, cur, stride); return;
        case 2:  rowfilter_decode_2(type, in, prev, cur, stride); return;
        case 3:  rowfilter_decode_3(type, in, prev, cur, stride); return;
        case 4:  rowfilter_decode_4(type, in, prev, cur, stride); return;
        default: rowfilter_decode_n(type, in, prev, cur, stride, bpp); return;
    }
}


/* Reverse per-row adaptive filtering. Returns 1 on success, 0 on malformed
   input (e.g. bad filter-type byte or size mismatch). */
static int unpack_adaptive(const uint8_t* input, size_t input_size, uint8_t* output,
                           uint32_t width, uint32_t height, unsigned pixel_bytes)
{
    size_t stride = (size_t)width * pixel_bytes;
    const uint8_t* ip = input;
    uint32_t y;
    unsigned bpp = pixel_bytes;

    if (input_size != pxl_filtered_size(PXL_FILTER_ADAPTIVE, width, height, pixel_bytes)) {
        return 0;
    }
    for (y = 0; y < height; ++y) {
        uint8_t type = *ip++;
        uint8_t* cur = output + (size_t)y * stride;
        const uint8_t* prev = y ? output + (size_t)(y - 1) * stride : NULL;
        if (type > PXL_ROWF_PAETH) {
            return 0;
        }
        rowfilter_decode(type, ip, prev, cur, stride, bpp);
        ip += stride;
    }
    return 1;
}


/* Reverse `filter`. `input`/`input_size` is the decompressed filtered data;
   output receives width*height*pixel_bytes pixel bytes. Returns 1 on success. */
static int reverse_filter(uint8_t filter, unsigned pixel_bytes,
                          const uint8_t* input, size_t input_size, uint8_t* output,
                          uint32_t width, uint32_t height)
{
    if (filter == PXL_FILTER_ADAPTIVE) {
        return unpack_adaptive(input, input_size, output, width, height, pixel_bytes);
    }
    if ((size_t)width * height * pixel_bytes != input_size) {
        return 0;
    }
    if (filter == PXL_FILTER_BCIF) {
        /* BCIF is defined only for 8-bit RGB/RGBA (SPEC 2.2). A crafted header
           pairing it with any other geometry would send e.g. 2-byte gray pixels
           through the 4-plane path and read past the buffer, so reject it here
           rather than trusting the caller to have validated. */
        if (pixel_bytes == 3) {
            unpack_bcif3(input, output, width, height);
        } else if (pixel_bytes == 4) {
            unpack_bcif4(input, output, width, height);
        } else {
            return 0;
        }
    } else if (filter == PXL_FILTER_NONE) {
        memcpy(output, input, input_size);
    } else {
        unpack_delta(input, output, width, height, (unsigned)pixel_bytes);
    }
    return 1;
}


pxl_image pxl_decode(pxl_buffer file)
{
    pxl_image img;
    pxl_header h;
    uint8_t* filtered = NULL;
    uint8_t* pixels = NULL;
    size_t dsize, frame_off, pixel_bytes_total, palette_bytes;
    unsigned pixel_bytes;
    pxl_geometry g;

    memset(&img, 0, sizeof(img));

    if (!file.data || !pxl_header_read(file.data, file.size, &h)) {
        return img;
    }

    if (!pxl_geometry_of(h.width, h.height, h.channels, h.bit_depth, &g)) {
        return img;
    }
    pixel_bytes = g.pixel_bytes;

    /* raw_byte_count is the size of the (decompressed) filtered stream, which
       depends on the filter. Validate it against the expected size for this
       filter/geometry so we never trust the header blindly for allocation. */
    if (h.color_filter > PXL_FILTER_MAX ||
        (size_t)h.raw_byte_count !=
            pxl_filtered_size(h.color_filter, g.filter_width, h.height, pixel_bytes)) {
        return img;
    }
    pixel_bytes_total = g.raw_bytes;
    /* BCIF is defined only for 8-bit RGB/RGBA; any other geometry with that
       filter byte is a malformed (or crafted) file. An indexed image never uses
       it either (its samples are indices, not color). */
    if (h.color_filter == PXL_FILTER_BCIF &&
        (h.bit_depth != 8 || h.palette_count != 0 ||
         (h.channels != 3 && h.channels != 4))) {
        return img;
    }

    /* Palette section, then metadata block, then the zstd frame. Each offset is
       checked against the remaining bytes so the sum can never wrap size_t. */
    palette_bytes = pxl_header_palette_bytes(&h);
    if (file.size < PXL_HEADER_BYTES ||
        palette_bytes > file.size - PXL_HEADER_BYTES) {
        return img;
    }
    frame_off = PXL_HEADER_BYTES + palette_bytes;
    if (h.meta_byte_count > file.size - frame_off) {
        return img;
    }
    frame_off += h.meta_byte_count;

    filtered = (uint8_t*)malloc(h.raw_byte_count);
    if (!filtered) {
        return img;
    }

    dsize = ZSTD_decompress(filtered, h.raw_byte_count,
                            file.data + frame_off,
                            file.size - frame_off);
    if (ZSTD_isError(dsize) || dsize != h.raw_byte_count) {
        free(filtered);
        return img;
    }

    pixels = (uint8_t*)malloc(pixel_bytes_total);
    if (!pixels) {
        free(filtered);
        return img;
    }

    if (!reverse_filter(h.color_filter, pixel_bytes, filtered, dsize, pixels,
                        g.filter_width, h.height)) {
        free(filtered);
        free(pixels);
        return img;
    }
    free(filtered);

    /* Copy out the palette. It is structural: without it an indexed image is
       undecodable, so OOM here fails the whole decode (unlike metadata). */
    if (palette_bytes) {
        size_t rgb = (size_t)h.palette_count * 3;
        unsigned char* pal = (unsigned char*)malloc(rgb);
        unsigned char* pa  = NULL;
        if (h.palette_alpha_count) {
            pa = (unsigned char*)malloc(h.palette_alpha_count);
        }
        if (!pal || (h.palette_alpha_count && !pa)) {
            free(pal);
            free(pa);
            free(pixels);
            return img;
        }
        memcpy(pal, file.data + PXL_HEADER_BYTES, rgb);
        img.palette.data = pal;
        img.palette.size = rgb;
        if (pa) {
            memcpy(pa, file.data + PXL_HEADER_BYTES + rgb, h.palette_alpha_count);
            img.palette_alpha.data = pa;
            img.palette_alpha.size = h.palette_alpha_count;
        }
    }

    /* With the palette known, verify no sample points past its end. */
    if (h.palette_count &&
        !indices_ok(pixels, g.row_bytes, h.width, h.height, h.bit_depth,
                    h.palette_count)) {
        pxl_free(&img.palette);
        pxl_free(&img.palette_alpha);
        free(pixels);
        memset(&img, 0, sizeof(img));
        return img;
    }

    /* Copy out the preserved metadata block, if present. It sits after the
       palette section. */
    if (h.meta_byte_count) {
        unsigned char* md = (unsigned char*)malloc(h.meta_byte_count);
        if (md) {
            memcpy(md, file.data + PXL_HEADER_BYTES + palette_bytes,
                   h.meta_byte_count);
            img.metadata.data = md;
            img.metadata.size = h.meta_byte_count;
        }
        /* On OOM we simply drop metadata; pixels are still valid. */
    }

    img.buffer.data = pixels;
    img.buffer.size = pixel_bytes_total;
    img.width = h.width;
    img.height = h.height;
    img.channels = h.channels;
    img.bytes_per_channel = (uint8_t)(h.bit_depth == 16 ? 2 : 1);
    img.bit_depth = h.bit_depth;
    return img;
}


/*----------------------------------------------------------------------------
  Streaming (progressive) decode

  The container is parsed with a small state machine so the caller can push
  bytes in arbitrary chunks: the fixed header, then the palette section, then
  the metadata block, then the zstd frame fed through ZSTD_decompressStream.

  Rows are reconstructed as soon as their filtered bytes exist:
    - DELTA:    each row is self-contained (prev resets per row);
    - ADAPTIVE: a row needs only the row above, already reconstructed;
    - BCIF:     the plane split means no row is complete before the last plane
                byte arrives, so those rows are emitted from pxl_stream_finish.
----------------------------------------------------------------------------*/

#define PXL_ST_HEADER  0
#define PXL_ST_PALETTE 1
#define PXL_ST_META    2
#define PXL_ST_FRAME   3
#define PXL_ST_ERROR   (-1)

/* Matches the encoder's window (APXL uses 2^27); still images never need more,
   but raising the limit costs nothing and keeps the two paths consistent. */
#define PXL_STREAM_WINDOW_LOG_MAX 27

struct pxl_stream {
    pxl_row_cb cb;
    void*      user;
    int        state;

    uint8_t    hdr[PXL_HEADER_BYTES];
    size_t     hdr_have;
    pxl_header h;
    pxl_geometry g;

    /* Palette section, consumed between the header and the metadata block. */
    size_t     pal_have;

    unsigned   pixel_bytes;
    size_t     row_stride;   /* pixel bytes per row */
    size_t     frow_stride;  /* filtered bytes per row (adaptive adds the type byte) */

    pxl_image  img;          /* progressively filled pixels + metadata */
    uint8_t*   filtered;     /* decompressed filtered stream */
    size_t     filtered_have;
    size_t     meta_have;
    uint32_t   rows_done;

    ZSTD_DStream* ds;
    int        frame_done;
};

/* Reconstruct every row whose filtered bytes have arrived. Returns the number
   of newly completed rows, or -1 on malformed data. */
static int stream_emit_rows(pxl_stream* s)
{
    int emitted = 0;

    if (s->h.color_filter == PXL_FILTER_BCIF) {
        return 0; /* not row-progressive; handled in pxl_stream_finish */
    }

    while (s->rows_done < s->h.height &&
           s->filtered_have >= (size_t)(s->rows_done + 1) * s->frow_stride) {
        const uint8_t* in = s->filtered + (size_t)s->rows_done * s->frow_stride;
        uint8_t* cur = s->img.buffer.data + (size_t)s->rows_done * s->row_stride;

        if (s->h.color_filter == PXL_FILTER_ADAPTIVE) {
            uint8_t type = *in++;
            const uint8_t* prev = s->rows_done ? cur - s->row_stride : NULL;
            if (type > PXL_ROWF_PAETH) {
                s->state = PXL_ST_ERROR;
                return -1;
            }
            rowfilter_decode(type, in, prev, cur, s->row_stride, s->pixel_bytes);
        } else if (s->h.color_filter == PXL_FILTER_NONE) {
            memcpy(cur, in, s->row_stride);
        } else {
            unpack_delta_row(in, cur, s->h.width, s->pixel_bytes);
        }

        /* Check before the callback: the consumer will use these samples as
           palette subscripts the moment it sees them. */
        if (s->h.palette_count &&
            !row_indices_ok(cur, s->h.width, s->h.bit_depth,
                            s->h.palette_count)) {
            s->state = PXL_ST_ERROR;
            return -1;
        }

        if (s->cb) {
            s->cb(s->user, s->rows_done, cur, s->row_stride);
        }
        ++s->rows_done;
        ++emitted;
    }
    return emitted;
}


/* Validate the parsed header and allocate the working buffers. */
static int stream_begin(pxl_stream* s)
{
    size_t pixels_total;

    if (!pxl_header_read(s->hdr, PXL_HEADER_BYTES, &s->h)) {
        return 0;
    }
    if (!pxl_geometry_of(s->h.width, s->h.height, s->h.channels, s->h.bit_depth,
                     &s->g)) {
        return 0;
    }
    s->pixel_bytes = s->g.pixel_bytes;
    if (s->h.color_filter > PXL_FILTER_MAX ||
        (size_t)s->h.raw_byte_count !=
            pxl_filtered_size(s->h.color_filter, s->g.filter_width, s->h.height,
                          s->pixel_bytes)) {
        return 0;
    }
    if (s->h.color_filter == PXL_FILTER_BCIF &&
        (s->h.bit_depth != 8 || s->h.palette_count != 0 ||
         (s->h.channels != 3 && s->h.channels != 4))) {
        return 0;
    }

    pixels_total = s->g.raw_bytes;
    s->row_stride = (size_t)s->g.filter_width * s->pixel_bytes;
    s->frow_stride = (s->h.color_filter == PXL_FILTER_ADAPTIVE)
                         ? s->row_stride + 1 : s->row_stride;

    s->filtered = (uint8_t*)malloc(s->h.raw_byte_count);
    s->img.buffer.data = (unsigned char*)malloc(pixels_total);
    if (!s->filtered || !s->img.buffer.data) {
        return 0;
    }
    s->img.buffer.size = pixels_total;
    s->img.width = s->h.width;
    s->img.height = s->h.height;
    s->img.channels = s->h.channels;
    s->img.bytes_per_channel = (uint8_t)(s->h.bit_depth == 16 ? 2 : 1);
    s->img.bit_depth = s->h.bit_depth;

    s->ds = ZSTD_createDStream();
    if (!s->ds) {
        return 0;
    }
    if (ZSTD_isError(ZSTD_initDStream(s->ds))) {
        return 0;
    }
    ZSTD_DCtx_setParameter(s->ds, ZSTD_d_windowLogMax, PXL_STREAM_WINDOW_LOG_MAX);

    if (s->h.meta_byte_count) {
        s->img.metadata.data = (unsigned char*)malloc(s->h.meta_byte_count);
        if (!s->img.metadata.data) {
            return 0;
        }
        s->img.metadata.size = s->h.meta_byte_count;
    }

    /* The palette is filled in by PXL_ST_PALETTE as the bytes arrive; its
       buffers must exist (and be sized) before that. */
    if (s->h.palette_count) {
        s->img.palette.data = (unsigned char*)malloc((size_t)s->h.palette_count * 3);
        if (!s->img.palette.data) {
            return 0;
        }
        s->img.palette.size = (size_t)s->h.palette_count * 3;
        if (s->h.palette_alpha_count) {
            s->img.palette_alpha.data =
                (unsigned char*)malloc(s->h.palette_alpha_count);
            if (!s->img.palette_alpha.data) {
                return 0;
            }
            s->img.palette_alpha.size = s->h.palette_alpha_count;
        }
    }
    return 1;
}


pxl_stream* pxl_stream_new(pxl_row_cb cb, void* user)
{
    pxl_stream* s = (pxl_stream*)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->cb = cb;
    s->user = user;
    s->state = PXL_ST_HEADER;
    return s;
}


int pxl_stream_feed(pxl_stream* s, const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    int total = 0;

    if (!s || s->state == PXL_ST_ERROR) {
        return -1;
    }
    if (!p && len) {
        return -1;
    }

    while (len) {
        if (s->state == PXL_ST_HEADER) {
            size_t need = PXL_HEADER_BYTES - s->hdr_have;
            size_t take = len < need ? len : need;
            memcpy(s->hdr + s->hdr_have, p, take);
            s->hdr_have += take;
            p += take;
            len -= take;
            if (s->hdr_have < PXL_HEADER_BYTES) {
                break;
            }
            if (!stream_begin(s)) {
                s->state = PXL_ST_ERROR;
                return -1;
            }
            s->state = PXL_ST_PALETTE;
            continue;
        }

        if (s->state == PXL_ST_PALETTE) {
            size_t total_pal = pxl_header_palette_bytes(&s->h);
            size_t need = total_pal - s->pal_have;
            size_t take = len < need ? len : need;
            while (take) {
                /* RGB triples first, then the alpha bytes. */
                size_t rgb = s->img.palette.size;
                size_t n;
                if (s->pal_have < rgb) {
                    n = rgb - s->pal_have;
                    if (n > take) { n = take; }
                    memcpy(s->img.palette.data + s->pal_have, p, n);
                } else {
                    n = take;
                    memcpy(s->img.palette_alpha.data + (s->pal_have - rgb), p, n);
                }
                s->pal_have += n;
                p += n;
                len -= n;
                take -= n;
            }
            if (s->pal_have < total_pal) {
                break;
            }
            s->state = PXL_ST_META;
            continue;
        }

        if (s->state == PXL_ST_META) {
            size_t need = s->h.meta_byte_count - s->meta_have;
            size_t take = len < need ? len : need;
            if (take) {
                if (s->img.metadata.data) {
                    memcpy(s->img.metadata.data + s->meta_have, p, take);
                }
                s->meta_have += take;
                p += take;
                len -= take;
            }
            if (s->meta_have < s->h.meta_byte_count) {
                break;
            }
            s->state = PXL_ST_FRAME;
            continue;
        }

        /* PXL_ST_FRAME */
        if (s->frame_done) {
            break; /* trailing bytes after the zstd frame are ignored */
        }
        {
            ZSTD_inBuffer in;
            ZSTD_outBuffer out;
            in.src = p;
            in.size = len;
            in.pos = 0;
            out.dst = s->filtered;
            out.size = s->h.raw_byte_count;
            out.pos = s->filtered_have;

            while (in.pos < in.size && !s->frame_done) {
                size_t in_before = in.pos, out_before = out.pos;
                size_t ret = ZSTD_decompressStream(s->ds, &out, &in);
                int rows;
                if (ZSTD_isError(ret)) {
                    s->state = PXL_ST_ERROR;
                    return -1;
                }
                s->filtered_have = out.pos;
                rows = stream_emit_rows(s);
                if (rows < 0) {
                    return -1;
                }
                total += rows;
                if (ret == 0) {
                    s->frame_done = 1;
                    break;
                }
                if (in.pos == in_before && out.pos == out_before) {
                    break; /* output full: the frame is larger than declared */
                }
            }
            p += in.pos;
            len -= in.pos;
            if (!s->frame_done && in.pos == 0) {
                break; /* no progress possible with this chunk */
            }
        }
    }
    return total;
}


const pxl_image* pxl_stream_image(const pxl_stream* s, uint32_t* rows_ready)
{
    if (!s || !s->img.buffer.data) {
        if (rows_ready) { *rows_ready = 0; }
        return NULL;
    }
    if (rows_ready) {
        *rows_ready = s->rows_done;
    }
    return &s->img;
}


int pxl_stream_finish(pxl_stream* s)
{
    if (!s || s->state != PXL_ST_FRAME || !s->frame_done ||
        s->filtered_have != s->h.raw_byte_count) {
        return 0;
    }

    if (s->h.color_filter == PXL_FILTER_BCIF && s->rows_done == 0) {
        uint32_t y;
        if (s->pixel_bytes == 3) {
            unpack_bcif3(s->filtered, s->img.buffer.data, s->h.width, s->h.height);
        } else {
            unpack_bcif4(s->filtered, s->img.buffer.data, s->h.width, s->h.height);
        }
        s->rows_done = s->h.height;
        if (s->cb) {
            for (y = 0; y < s->h.height; ++y) {
                s->cb(s->user, y, s->img.buffer.data + (size_t)y * s->row_stride,
                      s->row_stride);
            }
        }
    }

    return s->rows_done == s->h.height;
}


void pxl_stream_free(pxl_stream* s)
{
    if (!s) {
        return;
    }
    if (s->ds) {
        ZSTD_freeDStream(s->ds);
    }
    free(s->filtered);
    pxl_image_free(&s->img);
    free(s);
}

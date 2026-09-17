/** \file pxl_codec_encode.c
    \brief Encoder: filter selection, forward filters, zstd compression.

    This is the only object file that references the zstd compressor. Keeping
    it separate from pxl_codec_decode.c is what lets the linker drop ~317 KB of
    compressor code from decode-only binaries.

    The filter stage is ported from Zpng (Christopher A. Taylor, BSD):
      - generic path: subtract each channel value from the pixel to its left;
      - RGB/RGBA 8-bit: additionally apply the BCIF color transform
        (y=b, u=g-b, v=g-r) and split output into separate color planes.
*/
#include "pxl.h"
#include "pxl_codec_internal.h"
#include "pxl_format.h"

#include <zstd.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Validates the palette section against the image. On success writes the entry
   and alpha counts. Indexed images are single-channel by construction. */
static int palette_ok(const pxl_image* img, uint8_t depth,
                      unsigned* count, unsigned* alpha)
{
    unsigned n = pxl_palette_count(img);
    size_t na = img->palette_alpha.data ? img->palette_alpha.size : 0;

    if (n == 0) {
        /* No palette: a sub-byte depth is still legal (PNG gray 1/2/4). */
        if (na != 0) { return 0; } /* alpha without a palette is meaningless */
        *count = 0;
        *alpha = 0;
        return 1;
    }
    if (img->palette.size % 3u != 0 || n > PXL_MAX_PALETTE) { return 0; }
    if (img->channels != 1 || depth > 8) { return 0; }
    /* Indices must be representable at this depth. */
    if (depth < 8 && n > (1u << depth)) { return 0; }
    if (na > n) { return 0; }
    *count = n;
    *alpha = (unsigned)na;
    return 1;
}


/*----------------------------------------------------------------------------
  Generic filter: per-channel left delta, for any bytes-per-pixel 1..8.
  Operates on the interleaved pixel buffer in place-to-output.
----------------------------------------------------------------------------*/

static void pack_delta(const uint8_t* input, uint8_t* output,
                       uint32_t width, uint32_t height, unsigned pixel_bytes)
{
    uint32_t y, x;
    unsigned i;
    for (y = 0; y < height; ++y) {
        uint8_t prev[8] = { 0 };
        for (x = 0; x < width; ++x) {
            for (i = 0; i < pixel_bytes; ++i) {
                uint8_t a = input[i];
                output[i] = (uint8_t)(a - prev[i]);
                prev[i] = a;
            }
            input += pixel_bytes;
            output += pixel_bytes;
        }
    }
}


/*----------------------------------------------------------------------------
  BCIF filter for 8-bit RGB (3 channels): delta-left + YUV transform + planes.
----------------------------------------------------------------------------*/

static void pack_bcif3(const uint8_t* input, uint8_t* output,
                       uint32_t width, uint32_t height)
{
    const uint32_t plane = width * height;
    uint8_t* out_y = output;
    uint8_t* out_u = output + plane;
    uint8_t* out_v = output + plane * 2;
    uint32_t row, x;

    for (row = 0; row < height; ++row) {
        uint8_t prev[3] = { 0 };
        for (x = 0; x < width; ++x) {
            uint8_t r = (uint8_t)(input[0] - prev[0]);
            uint8_t g = (uint8_t)(input[1] - prev[1]);
            uint8_t b = (uint8_t)(input[2] - prev[2]);
            prev[0] = input[0];
            prev[1] = input[1];
            prev[2] = input[2];

            *out_y++ = b;
            *out_u++ = (uint8_t)(g - b);
            *out_v++ = (uint8_t)(g - r);

            input += 3;
        }
    }
}


/*----------------------------------------------------------------------------
  BCIF filter for 8-bit RGBA (4 channels): as RGB, plus a plain delta alpha
  plane.
----------------------------------------------------------------------------*/

static void pack_bcif4(const uint8_t* input, uint8_t* output,
                       uint32_t width, uint32_t height)
{
    const uint32_t plane = width * height;
    uint8_t* out_y = output;
    uint8_t* out_u = output + plane;
    uint8_t* out_v = output + plane * 2;
    uint8_t* out_a = output + plane * 3;
    uint32_t row, x;

    for (row = 0; row < height; ++row) {
        uint8_t prev[4] = { 0 };
        for (x = 0; x < width; ++x) {
            uint8_t r = (uint8_t)(input[0] - prev[0]);
            uint8_t g = (uint8_t)(input[1] - prev[1]);
            uint8_t b = (uint8_t)(input[2] - prev[2]);
            uint8_t a = (uint8_t)(input[3] - prev[3]);
            prev[0] = input[0];
            prev[1] = input[1];
            prev[2] = input[2];
            prev[3] = input[3];

            *out_y++ = b;
            *out_u++ = (uint8_t)(g - b);
            *out_v++ = (uint8_t)(g - r);
            *out_a++ = a;

            input += 4;
        }
    }
}


/* Encode one row with a given filter type into `out` (row_stride bytes). */
static void rowfilter_encode(uint8_t type, const uint8_t* cur, const uint8_t* prev,
                             uint8_t* out, size_t stride, unsigned bpp)
{
    size_t i;
    for (i = 0; i < stride; ++i) {
        uint8_t x = cur[i];
        uint8_t a = (i >= bpp) ? cur[i - bpp] : 0;
        uint8_t b = prev ? prev[i] : 0;
        uint8_t c = (prev && i >= bpp) ? prev[i - bpp] : 0;
        uint8_t pred;
        switch (type) {
            case PXL_ROWF_SUB:   pred = a; break;
            case PXL_ROWF_UP:    pred = b; break;
            case PXL_ROWF_AVG:   pred = (uint8_t)(((int)a + (int)b) >> 1); break;
            case PXL_ROWF_PAETH: pred = pxl_paeth(a, b, c); break;
            default:             pred = 0; break; /* NONE */
        }
        out[i] = (uint8_t)(x - pred);
    }
}


/* Sum of absolute signed residuals -- PNG's filter-selection heuristic. */
static unsigned long row_score(const uint8_t* row, size_t stride)
{
    unsigned long s = 0;
    size_t i;
    for (i = 0; i < stride; ++i) {
        int v = (signed char)row[i];
        s += (unsigned long)(v < 0 ? -v : v);
    }
    return s;
}


/* Pack pixels with per-row adaptive filtering. Returns bytes written, or 0 on
   allocation failure. */
static size_t pack_adaptive(const uint8_t* input, uint8_t* output,
                            uint32_t width, uint32_t height, unsigned pixel_bytes)
{
    size_t stride = (size_t)width * pixel_bytes;
    uint8_t* cand = (uint8_t*)malloc(stride);
    uint8_t* best = (uint8_t*)malloc(stride);
    uint8_t* op = output;
    uint32_t y;
    unsigned bpp = pixel_bytes;

    if (!cand || !best) { free(cand); free(best); return 0; }

    for (y = 0; y < height; ++y) {
        const uint8_t* cur = input + (size_t)y * stride;
        const uint8_t* prev = y ? input + (size_t)(y - 1) * stride : NULL;
        uint8_t best_type = PXL_ROWF_NONE;
        unsigned long best_s = (unsigned long)-1;
        uint8_t t;

        for (t = 0; t <= PXL_ROWF_PAETH; ++t) {
            unsigned long s;
            rowfilter_encode(t, cur, prev, cand, stride, bpp);
            s = row_score(cand, stride);
            if (s < best_s) {
                best_s = s;
                best_type = t;
                { uint8_t* tmp = best; best = cand; cand = tmp; }
            }
        }
        *op++ = best_type;
        memcpy(op, best, stride);
        op += stride;
    }

    free(cand);
    free(best);
    return (size_t)(op - output);
}


/*----------------------------------------------------------------------------
  Filter selection
----------------------------------------------------------------------------*/

/* Pick the color filter for the given geometry. BCIF is used only for 8-bit
   RGB/RGBA (where the YUV transform and plane split apply); everything else
   uses the generic per-channel delta. */
static uint8_t choose_filter(uint8_t channels, uint8_t bytes_per_channel)
{
    if (bytes_per_channel == 1 && (channels == 3 || channels == 4)) {
        return PXL_FILTER_BCIF;
    }
    return PXL_FILTER_DELTA;
}


/* Apply `filter`, writing into output. Returns bytes written, 0 on failure. */
static size_t apply_filter(uint8_t filter, unsigned pixel_bytes,
                           const uint8_t* input, uint8_t* output,
                           uint32_t width, uint32_t height)
{
    size_t raw = (size_t)width * height * pixel_bytes;
    if (filter == PXL_FILTER_ADAPTIVE) {
        return pack_adaptive(input, output, width, height, pixel_bytes);
    }
    if (filter == PXL_FILTER_NONE) {
        /* Palette indices are labels, not magnitudes: subtracting neighboring
           indices manufactures noise out of a smooth image. Storing them
           verbatim lets zstd match the byte patterns directly, which is both
           smaller on such images and the cheapest possible decode. */
        memcpy(output, input, raw);
        return raw;
    }
    if (filter == PXL_FILTER_BCIF) {
        if (pixel_bytes == 3) {
            pack_bcif3(input, output, width, height);
        } else {
            pack_bcif4(input, output, width, height);
        }
    } else {
        pack_delta(input, output, width, height, (unsigned)pixel_bytes);
    }
    return raw;
}


/*----------------------------------------------------------------------------
  Public codec API
----------------------------------------------------------------------------*/

pxl_buffer pxl_encode(const pxl_image* img, int zstd_level)
{
    return pxl_encode_ex(img, zstd_level, 0);
}


pxl_buffer pxl_encode_ex(const pxl_image* img, int zstd_level, unsigned flags)
{
    pxl_buffer out;
    uint8_t* filtered = NULL;
    uint8_t* best = NULL;
    uint8_t* work = NULL;
    uint8_t* best_frame = NULL;
    uint8_t* file = NULL;
    size_t raw_bytes, bound, csize = 0, meta_size, palette_bytes;
    size_t max_filtered, filtered_bytes = 0;
    unsigned pixel_bytes;
    unsigned pal_count, pal_alpha;
    uint8_t depth;
    pxl_geometry g;
    uint8_t filter = PXL_FILTER_DELTA;
    /* One slot per filter ID: every candidate is tried at most once, so this can
       never overflow as long as it tracks PXL_FILTER_MAX. */
    uint8_t candidates[PXL_FILTER_MAX + 1];
    int n_candidates = 0;
    int ci;
    pxl_header h;

    out.data = NULL;
    out.size = 0;

    if (!img || !img->buffer.data || img->width == 0 || img->height == 0) {
        return out;
    }
    if (img->channels < 1 || img->channels > 4) {
        return out;
    }
    if (img->bytes_per_channel != 1 && img->bytes_per_channel != 2) {
        return out;
    }
    depth = pxl_bit_depth(img);
    /* bit_depth and bytes_per_channel must agree: 16-bit samples occupy two
       bytes, everything else (1/2/4/8) exactly one. */
    if (img->bytes_per_channel != (depth == 16 ? 2u : 1u)) {
        return out;
    }
    if (!palette_ok(img, depth, &pal_count, &pal_alpha)) {
        return out;
    }
    if (!pxl_geometry_of(img->width, img->height, img->channels, depth, &g)) {
        return out;
    }
    pixel_bytes = g.pixel_bytes;
    raw_bytes = g.raw_bytes;
    if (img->buffer.size < raw_bytes) {
        return out; /* caller's buffer is too small for the stated geometry */
    }

    if (zstd_level <= 0) {
        zstd_level = PXL_LEVEL_DEFAULT;
    } else if (zstd_level > PXL_LEVEL_MAX) {
        zstd_level = PXL_LEVEL_MAX;
    }

    /* Build the candidate filter list. We always try the generic delta and no
       filter at all; the adaptive PNG-style filter and, for 8-bit RGB/RGBA,
       BCIF join them unless FAST_DECODE says otherwise. The smallest
       compressed result among the candidates tried wins.

       "No filter" is not redundant: on palette images the samples are labels
       rather than magnitudes, so every differencing filter turns a smooth image
       into noise and loses to storing the indices verbatim. It is also the
       fastest possible decode, so ties are worth taking.

       The list is ordered cheapest-to-decode first, and a later candidate must
       be strictly smaller to displace an earlier one, so an exact size tie is
       resolved in favor of the faster decode. */
    candidates[n_candidates++] = PXL_FILTER_NONE;
    candidates[n_candidates++] = PXL_FILTER_DELTA;
    /* ADAPTIVE only ties libpng's decode speed rather than beating it (measured
       on real PSP hardware, docs/BENCHMARKS.md 2026-09-17), so FAST_DECODE
       drops it: every remaining candidate is one PXL has measured to always
       decode faster than libpng, which is the guarantee FAST_DECODE exists to
       make. */
    if (!(flags & PXL_ENCODE_FAST_DECODE)) {
        candidates[n_candidates++] = PXL_FILTER_ADAPTIVE;
    }
    /* BCIF splits into color planes, which breaks top-to-bottom streaming, so
       PROGRESSIVE excludes it; it also loses outright to libpng at texture
       sizes despite winning at screen size, so FAST_DECODE excludes it too. */
    if (!(flags & (PXL_ENCODE_PROGRESSIVE | PXL_ENCODE_FAST_DECODE)) && pal_count == 0 &&
        choose_filter(img->channels, (uint8_t)(depth / 8u)) == PXL_FILTER_BCIF) {
        candidates[n_candidates++] = PXL_FILTER_BCIF;
    }

    /* Filtered buffers vary in size; adaptive is the largest. Size the scratch
       buffer and compress bound to the largest candidate. */
    max_filtered = raw_bytes;
    for (ci = 0; ci < n_candidates; ++ci) {
        size_t fs = pxl_filtered_size(candidates[ci], g.filter_width, img->height, pixel_bytes);
        if (fs > max_filtered) { max_filtered = fs; }
    }
    bound = ZSTD_compressBound(max_filtered);
    filtered = (uint8_t*)malloc(max_filtered);
    /* best/work hold the compressed frame only (no header). We swap them
       whenever the current candidate is smaller, so the best frame is never
       overwritten by a later, larger one. */
    best = (uint8_t*)malloc(bound);
    work = (uint8_t*)malloc(bound);
    if (!filtered || !best || !work) {
        free(filtered);
        free(best);
        free(work);
        return out;
    }

    for (ci = 0; ci < n_candidates; ++ci) {
        uint8_t cand = candidates[ci];
        size_t fsize, csz;
        fsize = apply_filter(cand, pixel_bytes, img->buffer.data, filtered,
                             g.filter_width, img->height);
        if (fsize == 0) {
            continue; /* filter failed (e.g. OOM in adaptive) */
        }
        csz = ZSTD_compress(work, bound, filtered, fsize, zstd_level);
        if (ZSTD_isError(csz)) {
            continue;
        }
        if (best_frame == NULL || csz < csize) {
            uint8_t* tmp = best; /* promote work -> best, reuse old best as work */
            best = work;
            work = tmp;
            csize = csz;
            filter = cand;
            filtered_bytes = fsize;
            best_frame = best; /* marker that we have a result */
        }
    }
    free(filtered);
    free(work);

    if (best_frame == NULL) {
        free(best);
        return out;
    }

    /* Assemble container: header + palette section + metadata block + frame. */
    meta_size = (img->metadata.data && img->metadata.size) ? img->metadata.size : 0;
    palette_bytes = (size_t)pal_count * 3u + (size_t)pal_alpha;
    file = (uint8_t*)malloc(PXL_HEADER_BYTES + palette_bytes + meta_size + csize);
    if (!file) {
        free(best);
        return out;
    }

    h.version = PXL_VERSION;
    h.channels = img->channels;
    h.bit_depth = depth;
    h.color_filter = filter;
    h.width = img->width;
    h.height = img->height;
    h.raw_byte_count = (uint32_t)filtered_bytes; /* size of the filtered stream */
    h.meta_byte_count = (uint32_t)meta_size;
    h.palette_count = (uint16_t)pal_count;
    h.palette_alpha_count = (uint16_t)pal_alpha;
    pxl_header_write(file, &h);
    if (pal_count) {
        memcpy(file + PXL_HEADER_BYTES, img->palette.data, (size_t)pal_count * 3u);
        if (pal_alpha) {
            memcpy(file + PXL_HEADER_BYTES + (size_t)pal_count * 3u,
                   img->palette_alpha.data, pal_alpha);
        }
    }
    if (meta_size) {
        memcpy(file + PXL_HEADER_BYTES + palette_bytes, img->metadata.data, meta_size);
    }
    memcpy(file + PXL_HEADER_BYTES + palette_bytes + meta_size, best, csize);
    free(best);

    out.data = file;
    out.size = PXL_HEADER_BYTES + palette_bytes + meta_size + csize;
    return out;
}

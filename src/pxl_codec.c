/** \file pxl_codec.c
    \brief PNG XL codec core: reversible filtering + zstd compression.

    The filter stage is ported from Zpng (Christopher A. Taylor, BSD):
      - generic path: subtract each channel value from the pixel to its left;
      - RGB/RGBA 8-bit: additionally apply the BCIF color transform
        (y=b, u=g-b, v=g-r) and split output into separate color planes.
    The filtered bytes are then compressed with zstd.
*/
#include "pxl.h"
#include "pxl_format.h"

#include <zstd.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Decode-side resource limits. A 24-byte header can claim any geometry, and
   the filtered stream that backs it compresses to almost nothing (a run of
   zeros), so without a cap a few-hundred-byte file forces multi-gigabyte
   allocations in pxl_decode/stream_begin. Mirrors APNG_MAX_DIM/
   APNG_MAX_PIXELS in apng.c. */
#define PXL_MAX_DIM     1000000u
#define PXL_MAX_PIXELS  ((uint64_t)1 << 28)

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

#define PXL_ROWF_NONE 0
#define PXL_ROWF_SUB  1
#define PXL_ROWF_UP   2
#define PXL_ROWF_AVG  3
#define PXL_ROWF_PAETH 4

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
    int p = (int)a + (int)b - (int)c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
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
            case PXL_ROWF_PAETH: pred = paeth(a, b, c); break;
            default:             pred = 0; break; /* NONE */
        }
        out[i] = (uint8_t)(x - pred);
    }
}

/* Reconstruct one row filtered with `type` from `in` (stride bytes) into the
   output row `cur` (which doubles as the source of already-reconstructed
   left/above-left samples). */
static void rowfilter_decode(uint8_t type, const uint8_t* in, const uint8_t* prev,
                             uint8_t* cur, size_t stride, unsigned bpp)
{
    size_t i;
    for (i = 0; i < stride; ++i) {
        uint8_t a = (i >= bpp) ? cur[i - bpp] : 0;
        uint8_t b = prev ? prev[i] : 0;
        uint8_t c = (prev && i >= bpp) ? prev[i - bpp] : 0;
        uint8_t pred;
        switch (type) {
            case PXL_ROWF_SUB:   pred = a; break;
            case PXL_ROWF_UP:    pred = b; break;
            case PXL_ROWF_AVG:   pred = (uint8_t)(((int)a + (int)b) >> 1); break;
            case PXL_ROWF_PAETH: pred = paeth(a, b, c); break;
            default:             pred = 0; break;
        }
        cur[i] = (uint8_t)(in[i] + pred);
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

static size_t adaptive_filtered_size(uint32_t width, uint32_t height, unsigned pixel_bytes)
{
    size_t stride = (size_t)width * pixel_bytes;
    return (size_t)height * (1 + stride);
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

/* Reverse per-row adaptive filtering. Returns 1 on success, 0 on malformed
   input (e.g. bad filter-type byte or size mismatch). */
static int unpack_adaptive(const uint8_t* input, size_t input_size, uint8_t* output,
                           uint32_t width, uint32_t height, unsigned pixel_bytes)
{
    size_t stride = (size_t)width * pixel_bytes;
    const uint8_t* ip = input;
    uint32_t y;
    unsigned bpp = pixel_bytes;

    if (input_size != adaptive_filtered_size(width, height, pixel_bytes)) {
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

/* Size of the filtered buffer produced by `filter` for the given geometry. */
static size_t filtered_size(uint8_t filter, uint32_t width, uint32_t height,
                            unsigned pixel_bytes)
{
    if (filter == PXL_FILTER_ADAPTIVE) {
        return adaptive_filtered_size(width, height, pixel_bytes);
    }
    return (size_t)width * height * pixel_bytes;
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
    } else {
        unpack_delta(input, output, width, height, (unsigned)pixel_bytes);
    }
    return 1;
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
    size_t pixel_count, raw_bytes, bound, csize = 0, meta_size;
    size_t max_filtered, filtered_bytes = 0;
    unsigned pixel_bytes;
    uint8_t filter = PXL_FILTER_DELTA;
    uint8_t candidates[3];
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

    pixel_bytes = (unsigned)img->channels * img->bytes_per_channel;
    pixel_count = (size_t)img->width * img->height;
    raw_bytes = pixel_count * pixel_bytes;

    if (zstd_level <= 0) {
        zstd_level = PXL_LEVEL_DEFAULT;
    } else if (zstd_level > PXL_LEVEL_MAX) {
        zstd_level = PXL_LEVEL_MAX;
    }

    /* Build the candidate filter list. We always try the adaptive PNG-style
       filter (it matches or beats a single global filter on almost any image)
       and the generic delta; for 8-bit RGB/RGBA we also try BCIF. The smallest
       compressed result wins. */
    candidates[n_candidates++] = PXL_FILTER_ADAPTIVE;
    candidates[n_candidates++] = PXL_FILTER_DELTA;
    /* BCIF splits into color planes, which breaks top-to-bottom streaming, so
       the progressive flag excludes it. */
    if (!(flags & PXL_ENCODE_PROGRESSIVE) &&
        choose_filter(img->channels, img->bytes_per_channel) == PXL_FILTER_BCIF) {
        candidates[n_candidates++] = PXL_FILTER_BCIF;
    }

    /* Filtered buffers vary in size; adaptive is the largest. Size the scratch
       buffer and compress bound to the largest candidate. */
    max_filtered = raw_bytes;
    for (ci = 0; ci < n_candidates; ++ci) {
        size_t fs = filtered_size(candidates[ci], img->width, img->height, pixel_bytes);
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
                             img->width, img->height);
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

    /* Assemble container: 24-byte header + metadata block + zstd frame. */
    meta_size = (img->metadata.data && img->metadata.size) ? img->metadata.size : 0;
    file = (uint8_t*)malloc(PXL_HEADER_BYTES + meta_size + csize);
    if (!file) {
        free(best);
        return out;
    }

    h.version = PXL_VERSION;
    h.channels = img->channels;
    h.bytes_per_channel = img->bytes_per_channel;
    h.color_filter = filter;
    h.width = img->width;
    h.height = img->height;
    h.raw_byte_count = (uint32_t)filtered_bytes; /* size of the filtered stream */
    h.meta_byte_count = (uint32_t)meta_size;
    pxl_header_write(file, &h);
    if (meta_size) {
        memcpy(file + PXL_HEADER_BYTES, img->metadata.data, meta_size);
    }
    memcpy(file + PXL_HEADER_BYTES + meta_size, best, csize);
    free(best);

    out.data = file;
    out.size = PXL_HEADER_BYTES + meta_size + csize;
    return out;
}

pxl_image pxl_decode(pxl_buffer file)
{
    pxl_image img;
    pxl_header h;
    uint8_t* filtered = NULL;
    uint8_t* pixels = NULL;
    size_t dsize, frame_off, pixel_bytes_total;
    unsigned pixel_bytes;

    memset(&img, 0, sizeof(img));

    if (!file.data || !pxl_header_read(file.data, file.size, &h)) {
        return img;
    }

    pixel_bytes = (unsigned)h.channels * h.bytes_per_channel;

    /* raw_byte_count is the size of the (decompressed) filtered stream, which
       depends on the filter. Validate it against the expected size for this
       filter/geometry so we never trust the header blindly for allocation.
       geometry_ok() must run before any width*height arithmetic. */
    if (h.color_filter > PXL_FILTER_ADAPTIVE ||
        !geometry_ok(h.width, h.height, pixel_bytes) ||
        (size_t)h.raw_byte_count !=
            filtered_size(h.color_filter, h.width, h.height, pixel_bytes)) {
        return img;
    }
    pixel_bytes_total = (size_t)h.width * h.height * pixel_bytes;
    /* BCIF is defined only for 8-bit RGB/RGBA; any other geometry with that
       filter byte is a malformed (or crafted) file. */
    if (h.color_filter == PXL_FILTER_BCIF &&
        (h.bytes_per_channel != 1 || (h.channels != 3 && h.channels != 4))) {
        return img;
    }

    /* The metadata block sits between the header and the zstd frame. */
    frame_off = PXL_HEADER_BYTES + h.meta_byte_count;
    if (frame_off > file.size) {
        return img;
    }

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
                        h.width, h.height)) {
        free(filtered);
        free(pixels);
        return img;
    }
    free(filtered);

    /* Copy out the preserved metadata block, if present. */
    if (h.meta_byte_count) {
        unsigned char* md = (unsigned char*)malloc(h.meta_byte_count);
        if (md) {
            memcpy(md, file.data + PXL_HEADER_BYTES, h.meta_byte_count);
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
    img.bytes_per_channel = h.bytes_per_channel;
    return img;
}

/*----------------------------------------------------------------------------
  Streaming (progressive) decode

  The container is parsed with a small state machine so the caller can push
  bytes in arbitrary chunks: 24-byte header, then the metadata block, then the
  zstd frame fed through ZSTD_decompressStream.

  Rows are reconstructed as soon as their filtered bytes exist:
    - DELTA:    each row is self-contained (prev resets per row);
    - ADAPTIVE: a row needs only the row above, already reconstructed;
    - BCIF:     the plane split means no row is complete before the last plane
                byte arrives, so those rows are emitted from pxl_stream_finish.
----------------------------------------------------------------------------*/

#define PXL_ST_HEADER 0
#define PXL_ST_META   1
#define PXL_ST_FRAME  2
#define PXL_ST_ERROR  (-1)

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
        } else {
            unpack_delta_row(in, cur, s->h.width, s->pixel_bytes);
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
    s->pixel_bytes = (unsigned)s->h.channels * s->h.bytes_per_channel;
    /* geometry_ok() must run before any width*height arithmetic. */
    if (s->h.color_filter > PXL_FILTER_ADAPTIVE ||
        !geometry_ok(s->h.width, s->h.height, s->pixel_bytes) ||
        (size_t)s->h.raw_byte_count !=
            filtered_size(s->h.color_filter, s->h.width, s->h.height, s->pixel_bytes)) {
        return 0;
    }
    if (s->h.color_filter == PXL_FILTER_BCIF &&
        (s->h.bytes_per_channel != 1 ||
         (s->h.channels != 3 && s->h.channels != 4))) {
        return 0;
    }

    pixels_total = (size_t)s->h.width * s->h.height * s->pixel_bytes;
    s->row_stride = (size_t)s->h.width * s->pixel_bytes;
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
    s->img.bytes_per_channel = s->h.bytes_per_channel;

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
    }
}

const char* pxl_version(void)
{
    return "1.5.0";
}

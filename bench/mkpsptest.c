/** \file mkpsptest.c
    \brief Generates the embedded test data for the PSP decode smoke test
           (psp/testdata.h): one valid .pxl file per color filter, plus the
           known-correct raw pixels to check the PSP decode against.

    Why embedded rather than files on a memory stick: the PSP program is run
    under PPSSPPHeadless with no interactive setup and no physical console, so
    the test data travels inside the ELF as C arrays. That also sidesteps
    needing to get PPSSPPHeadless's virtual memory-stick root right.

    Why every filter is forced rather than letting the encoder choose: the
    point of psp/main.c is a per-filter decode check (and eventually timing),
    and pxl_encode_ex always picks whichever candidate compresses smallest, so
    a normal encode of any one image would only exercise one filter. This
    reimplements just the encode tail (apply one named filter, compress,
    assemble the container) instead of the full candidate search --
    apply_filter and pxl_header_write are exactly what pxl_encode_ex itself
    calls, so the files this produces are what the real encoder would produce
    if it had chosen that filter.

    The source image is synthetic (a formula, not a file) so this generator
    has no input dependency: 64x64 RGBA8888 with enough curvature in two of
    its three channels that NONE, DELTA and ADAPTIVE each compress
    differently, which is the point -- a flat or purely linear image would
    make every filter look the same.

    Usage:
      bench/mkpsptest > psp/testdata.h
*/

#include "../src/pxl_codec_encode.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static pxl_buffer encode_forced(const pxl_image* img, int zstd_level, uint8_t filter)
{
    pxl_buffer out;
    uint8_t* filtered = NULL;
    uint8_t* frame = NULL;
    uint8_t* file = NULL;
    size_t raw_bytes, bound, csize, meta_size, palette_bytes, fsize, max_filtered;
    unsigned pixel_bytes, pal_count, pal_alpha;
    uint8_t depth;
    pxl_geometry g;
    pxl_header h;

    out.data = NULL;
    out.size = 0;

    depth = pxl_bit_depth(img);
    if (!palette_ok(img, depth, &pal_count, &pal_alpha)) {
        return out;
    }
    if (!pxl_geometry_of(img->width, img->height, img->channels, depth, &g)) {
        return out;
    }
    pixel_bytes = g.pixel_bytes;
    raw_bytes   = g.raw_bytes;

    max_filtered = pxl_filtered_size(filter, g.filter_width, img->height, pixel_bytes);
    if (max_filtered < raw_bytes) {
        max_filtered = raw_bytes;
    }
    bound    = ZSTD_compressBound(max_filtered);
    filtered = (uint8_t*)malloc(max_filtered);
    frame    = (uint8_t*)malloc(bound);
    if (!filtered || !frame) {
        free(filtered);
        free(frame);
        return out;
    }

    fsize = apply_filter(filter, pixel_bytes, img->buffer.data, filtered,
                         g.filter_width, img->height);
    if (fsize == 0) {
        free(filtered);
        free(frame);
        return out;
    }
    csize = ZSTD_compress(frame, bound, filtered, fsize, zstd_level);
    if (ZSTD_isError(csize)) {
        free(filtered);
        free(frame);
        return out;
    }

    meta_size     = 0;
    palette_bytes = (size_t)pal_count * 3u + (size_t)pal_alpha;
    file = (uint8_t*)malloc(PXL_HEADER_BYTES + palette_bytes + meta_size + csize);
    if (!file) {
        free(filtered);
        free(frame);
        return out;
    }

    h.version             = PXL_VERSION;
    h.channels            = img->channels;
    h.bit_depth           = depth;
    h.color_filter        = filter;
    h.width               = img->width;
    h.height              = img->height;
    h.raw_byte_count      = (uint32_t)fsize;
    h.meta_byte_count     = (uint32_t)meta_size;
    h.palette_count       = (uint16_t)pal_count;
    h.palette_alpha_count = (uint16_t)pal_alpha;
    pxl_header_write(file, &h);
    memcpy(file + PXL_HEADER_BYTES + palette_bytes + meta_size, frame, csize);

    free(filtered);
    free(frame);
    out.data = file;
    out.size = PXL_HEADER_BYTES + palette_bytes + meta_size + csize;
    return out;
}

static void emit_array(const char* name, const unsigned char* data, size_t len)
{
    size_t i;
    printf("static const unsigned char %s[] = {\n", name);
    for (i = 0; i < len; i++) {
        printf("%s0x%02x,%s", (i % 16 == 0) ? "  " : "",
               data[i], (i % 16 == 15) ? "\n" : "");
    }
    if (len % 16 != 0) {
        printf("\n");
    }
    printf("};\n");
    printf("static const unsigned int %s_len = %uu;\n\n", name, (unsigned)len);
}

int main(void)
{
    static const struct { const char* name; uint8_t filter; } cases[] = {
        { "none",     PXL_FILTER_NONE },
        { "delta",    PXL_FILTER_DELTA },
        { "adaptive", PXL_FILTER_ADAPTIVE },
        { "bcif",     PXL_FILTER_BCIF },
    };
    pxl_image img;
    uint8_t* raw;
    uint32_t x, y;
    size_t ci;

    raw = (uint8_t*)malloc((size_t)W * H * 4);
    if (!raw) {
        return 1;
    }
    /* r: diagonal ramp, g: xor checkerboard (high-frequency, punishes any
       predictor), b: quadratic curve (rewards Paeth over a plain left-delta),
       a: constant -- deliberately not all the same shape, so the four
       filters land on genuinely different compressed sizes rather than
       coincidentally tying. */
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint8_t* p = raw + ((size_t)y * W + x) * 4;
            p[0] = (uint8_t)(x * 3 + y * 5);
            p[1] = (uint8_t)(x ^ y);
            p[2] = (uint8_t)(x * x + y * y);
            p[3] = 255;
        }
    }

    memset(&img, 0, sizeof img);
    img.buffer.data       = raw;
    img.buffer.size       = (size_t)W * H * 4;
    img.width             = W;
    img.height            = H;
    img.channels          = 4;
    img.bytes_per_channel = 1;
    img.bit_depth         = 8;

    printf("/* Generated by bench/mkpsptest.c -- do not edit by hand.\n"
           "   %dx%d RGBA8888, one valid .pxl file per color filter, plus the\n"
           "   known-correct decoded pixels to check against. */\n"
           "#ifndef PXL_PSP_TESTDATA_H\n"
           "#define PXL_PSP_TESTDATA_H\n\n", W, H);

    emit_array("pxl_psp_expected_pixels", raw, img.buffer.size);

    for (ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
        pxl_buffer enc = encode_forced(&img, PXL_LEVEL_DEFAULT, cases[ci].filter);
        char name[64];
        if (!enc.data) {
            fprintf(stderr, "error: forcing filter '%s' failed\n", cases[ci].name);
            return 1;
        }
        snprintf(name, sizeof name, "pxl_psp_file_%s", cases[ci].name);
        emit_array(name, enc.data, enc.size);
        free(enc.data);
    }

    printf("#endif\n");
    free(raw);
    return 0;
}

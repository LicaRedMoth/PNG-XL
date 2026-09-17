/** \file mkpsptest.c
    \brief Generates the embedded test data for the PSP program (psp/testdata.h):
           one valid .pxl file per (size, color filter) pair, plus the
           known-correct raw pixels to check each size's decode against.

    Why embedded rather than files on a memory stick: the smoke-test path runs
    under PPSSPPHeadless with no interactive setup and no physical console, so
    the test data travels inside the ELF as C arrays. That also sidesteps
    needing to get PPSSPPHeadless's virtual memory-stick root right, and means
    the real-hardware benchmark needs nothing copied onto the memory stick
    beyond the one EBOOT.PBP.

    Why every filter is forced rather than letting the encoder choose:
    pxl_encode_ex always picks whichever candidate compresses smallest, so a
    normal encode of any one image would only exercise one filter, and the
    point here is a per-filter number. This reimplements just the encode tail
    (apply one named filter, compress, assemble the container) instead of the
    full candidate search -- apply_filter and pxl_header_write are exactly
    what pxl_encode_ex itself calls, so the files this produces are what the
    real encoder would produce if it had chosen that filter.

    Three sizes, matching ROADMAP.md's "measure decode on the target hardware"
    entry: a small "smoke" case cheap enough to always run (correctness only,
    64x64 -- too small for a throughput number to mean anything, per-call
    overhead dominates), a "screen" case (480x272, the PSP's own display
    resolution) and a "texture" case (512x512, a common GE texture size). The
    latter two are what the on-device throughput sweep times.

    Every size uses the same synthetic pattern (psp/pattern.h -- a formula,
    not a file, so this generator has no input dependency), tuned to be
    closer to real UI/game content than pure noise: a smooth diagonal ramp, a
    coarse 16px block checkerboard (edges like panel borders, not per-pixel
    noise), and a quadratic curve, so NONE/DELTA/ADAPTIVE/BCIF land on
    genuinely different compressed sizes without either tying (a flat image)
    or being unrepresentatively harsh (per-pixel noise compresses worse than
    almost anything real, which would understate every filter's decode speed
    since zstd dominates decode time and zstd's speed depends on how much
    there actually is to decompress).

    Only the compressed .pxl files are embedded, not the reference pixels:
    main.c re-derives them from the same formula instead, which is what keeps
    the generated header under 1.5 MB rather than the ~9.6 MB a screen- and
    texture-sized raw reference image would cost as hex text.

    Usage:
      bench/mkpsptest > psp/testdata.h
*/

#include "../src/pxl_codec_encode.c"
#include "../psp/pattern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char* name;
    uint32_t    w, h;
} pxl_psp_size;

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

static uint8_t* make_pattern(uint32_t w, uint32_t h)
{
    uint8_t* raw = (uint8_t*)malloc((size_t)w * h * 4);
    uint32_t x, y;
    if (!raw) {
        return NULL;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            pxl_psp_pattern_pixel(x, y, raw + ((size_t)y * w + x) * 4);
        }
    }
    return raw;
}

int main(void)
{
    static const struct { const char* name; uint8_t filter; } filters[] = {
        { "none",     PXL_FILTER_NONE },
        { "delta",    PXL_FILTER_DELTA },
        { "adaptive", PXL_FILTER_ADAPTIVE },
        { "bcif",     PXL_FILTER_BCIF },
    };
    /* "smoke": cheap, always-run correctness check. "screen"/"texture": what
       ROADMAP.md's throughput sweep times -- the PSP's own display resolution
       and a common GE texture size. */
    static const pxl_psp_size sizes[] = {
        { "smoke",   64,  64  },
        { "screen",  480, 272 },
        { "texture", 512, 512 },
    };
    size_t si, ci;

    printf("/* Generated by bench/mkpsptest.c -- do not edit by hand.\n"
           "   One valid .pxl file per (size, color filter) pair, plus the\n"
           "   known-correct decoded pixels to check each size's decode\n"
           "   against. See bench/mkpsptest.c for what each size is for. */\n"
           "#ifndef PXL_PSP_TESTDATA_H\n"
           "#define PXL_PSP_TESTDATA_H\n\n");

    for (si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
        pxl_image img;
        uint8_t* raw = make_pattern(sizes[si].w, sizes[si].h);
        char name[64];

        if (!raw) {
            fprintf(stderr, "error: out of memory generating '%s'\n", sizes[si].name);
            return 1;
        }
        memset(&img, 0, sizeof img);
        img.buffer.data       = raw;
        img.buffer.size       = (size_t)sizes[si].w * sizes[si].h * 4;
        img.width             = sizes[si].w;
        img.height            = sizes[si].h;
        img.channels          = 4;
        img.bytes_per_channel = 1;
        img.bit_depth         = 8;

        fprintf(stderr, "%-8s %ux%u, %zu raw bytes\n", sizes[si].name,
                sizes[si].w, sizes[si].h, img.buffer.size);

        for (ci = 0; ci < sizeof filters / sizeof filters[0]; ci++) {
            pxl_buffer enc = encode_forced(&img, PXL_LEVEL_DEFAULT, filters[ci].filter);
            if (!enc.data) {
                fprintf(stderr, "error: forcing filter '%s' on '%s' failed\n",
                        filters[ci].name, sizes[si].name);
                return 1;
            }
            fprintf(stderr, "  %-8s -> %zu compressed bytes\n", filters[ci].name, enc.size);
            snprintf(name, sizeof name, "pxl_psp_%s_file_%s", sizes[si].name, filters[ci].name);
            emit_array(name, enc.data, enc.size);
            free(enc.data);
        }
        free(raw);
    }

    printf("#endif\n");
    return 0;
}

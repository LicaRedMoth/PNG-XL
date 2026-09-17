/** \file packedformat.c
    \brief Does storing pixels pre-packed into a native PSP/GE format (RGB565,
           RGBA5551, RGBA4444) instead of 8-bit-per-channel compress smaller,
           and does it decode faster -- these are different questions.

    Why this needed asking again rather than reusing the old answer: this
    project already rejected packed sub-16-bit samples (RESEARCH.md, "packed
    10-bit, four samples per five bytes" -- 37.5% less raw data compressed to
    23% *more*, because packed sample boundaries wander across bytes and defeat
    a byte-oriented matcher). RGB565/5551/4444 are a different pack ratio (2
    bytes carrying 3-4 unaligned channels, not 5 bytes carrying 4 aligned
    samples), so the mechanism transfers but the magnitude was never measured
    for this specific layout, and this layout is far more practically relevant
    than 10-bit HDR ever was.

    The other reason to measure rather than reuse the old answer: the old
    experiment only asked about file SIZE. Even if packing loses on size the
    same way, it could still win on decode SPEED -- half the raw bytes per
    pixel means half as much for zstd to decompress, and this project's own
    numbers (docs/BENCHMARKS.md) show zstd is 88-98% of decode time for every
    filter except adaptive. Size and decode speed are different axes and
    this tool reports both, not just the one the precedent already answered.

    Method: for each PNG, build three pixel buffers from the same source --
    RGB888 (the existing baseline), RGB565 (2 bytes/pixel, 5-6-5 truncation,
    no dithering) and RGBA8888->RGBA5551 if the source has alpha. Each is run
    through the real apply_filter/ZSTD_compress calls at every candidate
    filter, exactly as the shipping encoder would, and the smallest per format
    is reported alongside its raw (pre-compression) byte count.

    Usage:
      bench/packedformat <image.png>...
*/

#include "../src/pxl_codec_encode.c"
#include "../src/pxl_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Best compressed size among {NONE, DELTA, ADAPTIVE} for a buffer described
   by (channels, depth) exactly as pxl_geometry_of expects -- BCIF is
   excluded, it is only defined for 8-bit RGB/RGBA and has no meaning on a
   packed 16-bit sample. For the packed formats, `channels=1, depth=16`
   describes a packed sample as one opaque 2-byte unit, which is a fiction
   (565 has no real per-channel decomposition) but is exactly what
   pxl_geometry_of needs to hand back pixel_bytes=2. */
static size_t best_size(const uint8_t* pixels, uint32_t w, uint32_t h,
                        uint8_t channels, uint8_t depth, int level, size_t* raw_out)
{
    static const uint8_t FILTERS[] = { PXL_FILTER_NONE, PXL_FILTER_DELTA, PXL_FILTER_ADAPTIVE };
    pxl_geometry g;
    uint8_t* filtered;
    uint8_t* frame;
    unsigned pixel_bytes;
    size_t max_filtered, bound, best = 0;
    int i;

    if (!pxl_geometry_of(w, h, channels, depth, &g)) {
        return 0;
    }
    pixel_bytes = g.pixel_bytes;
    max_filtered = (size_t)w * h * pixel_bytes;
    for (i = 0; i < 3; i++) {
        size_t fs = pxl_filtered_size(FILTERS[i], g.filter_width, h, pixel_bytes);
        if (fs > max_filtered) { max_filtered = fs; }
    }
    bound    = ZSTD_compressBound(max_filtered);
    filtered = (uint8_t*)malloc(max_filtered);
    frame    = (uint8_t*)malloc(bound);
    if (!filtered || !frame) { free(filtered); free(frame); return 0; }

    for (i = 0; i < 3; i++) {
        size_t fsize = apply_filter(FILTERS[i], pixel_bytes, pixels, filtered,
                                    g.filter_width, h);
        size_t csz;
        if (fsize == 0) { continue; }
        csz = ZSTD_compress(frame, bound, filtered, fsize, level);
        if (ZSTD_isError(csz)) { continue; }
        if (best == 0 || csz < best) { best = csz; }
    }
    free(filtered);
    free(frame);
    if (raw_out) { *raw_out = (size_t)w * h * pixel_bytes; }
    return best;
}

static uint8_t* pack_565(const uint8_t* rgb, uint32_t w, uint32_t h)
{
    uint8_t* out = (uint8_t*)malloc((size_t)w * h * 2);
    size_t i, n = (size_t)w * h;
    if (!out) { return NULL; }
    for (i = 0; i < n; i++) {
        unsigned r = rgb[i * 3 + 0] >> 3, g = rgb[i * 3 + 1] >> 2, b = rgb[i * 3 + 2] >> 3;
        unsigned v = (r << 11) | (g << 5) | b;
        out[i * 2 + 0] = (uint8_t)(v & 0xFF);
        out[i * 2 + 1] = (uint8_t)(v >> 8);
    }
    return out;
}

static uint8_t* pack_5551(const uint8_t* rgba, uint32_t w, uint32_t h)
{
    uint8_t* out = (uint8_t*)malloc((size_t)w * h * 2);
    size_t i, n = (size_t)w * h;
    if (!out) { return NULL; }
    for (i = 0; i < n; i++) {
        unsigned r = rgba[i * 4 + 0] >> 3, g = rgba[i * 4 + 1] >> 3,
                 b = rgba[i * 4 + 2] >> 3, a = rgba[i * 4 + 3] >> 7;
        unsigned v = (r << 11) | (g << 6) | (b << 1) | a;
        out[i * 2 + 0] = (uint8_t)(v & 0xFF);
        out[i * 2 + 1] = (uint8_t)(v >> 8);
    }
    return out;
}

int main(int argc, char** argv)
{
    int argi;
    size_t tot_raw8 = 0, tot_c8 = 0, tot_raw16 = 0, tot_c16 = 0;
    long n = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <image.png>...\n", argv[0]);
        return 2;
    }

    printf("%-28s  %10s %10s   %10s %10s   %6s %6s\n",
           "file", "raw8", "best8", "raw16", "best16", "raw%", "cmp%");

    for (argi = 1; argi < argc; argi++) {
        pxl_image img = pxl_load_png(argv[argi]);
        uint8_t* packed;
        size_t raw8, c8, raw16, c16;

        if (!img.buffer.data || img.bytes_per_channel != 1 ||
            (img.channels != 3 && img.channels != 4)) {
            fprintf(stderr, "skip %s: not 8-bit RGB/RGBA\n", argv[argi]);
            pxl_image_free(&img);
            continue;
        }

        c8 = best_size(img.buffer.data, img.width, img.height,
                       img.channels, 8, PXL_LEVEL_DEFAULT, &raw8);
        packed = (img.channels == 3) ? pack_565(img.buffer.data, img.width, img.height)
                                     : pack_5551(img.buffer.data, img.width, img.height);
        if (!packed || c8 == 0) {
            fprintf(stderr, "skip %s: encode failed\n", argv[argi]);
            free(packed); pxl_image_free(&img);
            continue;
        }
        c16 = best_size(packed, img.width, img.height, 1, 16, PXL_LEVEL_DEFAULT, &raw16);

        printf("%-28s  %10zu %10zu   %10zu %10zu   %5.1f%% %5.1f%%\n",
               argv[argi], raw8, c8, raw16, c16,
               100.0 * (double)raw16 / (double)raw8,
               100.0 * (double)c16 / (double)c8);

        tot_raw8 += raw8; tot_c8 += c8; tot_raw16 += raw16; tot_c16 += c16;
        n++;
        free(packed);
        pxl_image_free(&img);
    }

    if (n > 0) {
        printf("\n%ld files. Packed-16 raw bytes: %.1f%% of 8-bit (this is the\n"
               "zstd-input-size proxy for decode speed). Packed-16 compressed\n"
               "bytes: %.1f%% of 8-bit (this is the file-size question the\n"
               "10-bit experiment already answered for a different pack ratio).\n",
               n, 100.0 * (double)tot_raw16 / (double)tot_raw8,
               100.0 * (double)tot_c16 / (double)tot_c8);
    }
    return 0;
}

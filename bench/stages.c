/** \file stages.c
    \brief Splits PXL decode time into its two stages: zstd decompression and
           unfiltering.

    The question this answers: when we decode a .pxl, where does the time go?
    If zstd dominates, decode speed is capped by the compressor and only a
    format change moves it. If unfiltering dominates, the time sits in our own
    ~19 KB of code and is ours to optimise.

    pxl_decode() does both stages back to back, so timing it from outside
    cannot separate them. The stage helpers (geometry_of, filtered_size,
    reverse_filter) are static inside pxl_codec.c, so this tool includes that
    translation unit directly and reimplements the decode path stage by stage.
    That keeps the measurement honest (same code, same flags) without widening
    the library's public surface for a benchmark.

    Usage:
      bench/stages <image.png> [reps] [level]

    Prints, per colour filter available for the image, the median wall time of
    each stage over `reps` runs.
*/

#include "../src/pxl_codec.c"

#include "../src/pxl_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmp_double(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static double median(double* v, int n)
{
    qsort(v, (size_t)n, sizeof *v, cmp_double);
    return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

static const char* filter_name(uint8_t f)
{
    switch (f) {
    case PXL_FILTER_NONE:     return "none";
    case PXL_FILTER_DELTA:    return "delta";
    case PXL_FILTER_BCIF:     return "bcif";
    case PXL_FILTER_ADAPTIVE: return "adaptive";
    default:                  return "?";
    }
}

/* Filters `img` with one specific colour filter and compresses it, then times
   the two decode stages separately. Returns 0 if this filter cannot represent
   the image.

   pxl_encode_ex() chooses the filter itself and offers no per-filter entry
   point, so this builds the zstd frame directly with apply_filter (the same
   function the encoder uses). Skipping the container costs nothing here: the
   header, palette and metadata are memcpy'd at decode time and are not part of
   either stage being measured. */
static int measure(const pxl_image* img, uint8_t filter, int level, int reps,
                   double* t)
{
    pxl_geometry g;
    uint8_t*     staging  = NULL;  /* filter output at encode time */
    uint8_t*     frame    = NULL;  /* compressed bytes */
    uint8_t*     filtered = NULL;  /* stage 1 output (decode side) */
    uint8_t*     pixels   = NULL;  /* stage 2 output */
    size_t       max_filtered, filtered_bytes, bound, frame_size;
    double       zstd_ms, unfilter_ms;
    uint8_t      depth = pxl_bit_depth(img);
    int          i;

    /* BCIF is defined only for 8-bit RGB/RGBA without a palette (SPEC 2.2). */
    if (filter == PXL_FILTER_BCIF &&
        (depth != 8 || img->palette.size != 0 ||
         (img->channels != 3 && img->channels != 4))) {
        return 0;
    }

    if (!geometry_of(img->width, img->height, img->channels, depth, &g)) {
        return 0;
    }

    max_filtered = filtered_size(filter, g.filter_width, img->height,
                                 g.pixel_bytes);
    bound        = ZSTD_compressBound(max_filtered);
    staging  = (uint8_t*)malloc(max_filtered);
    frame    = (uint8_t*)malloc(bound);
    filtered = (uint8_t*)malloc(max_filtered);
    pixels   = (uint8_t*)malloc(g.raw_bytes);
    if (!staging || !frame || !filtered || !pixels) {
        goto fail;
    }

    filtered_bytes = apply_filter(filter, g.pixel_bytes, img->buffer.data,
                                  staging, g.filter_width, img->height);
    if (filtered_bytes == 0 || filtered_bytes != max_filtered) {
        goto fail;
    }

    frame_size = ZSTD_compress(frame, bound, staging, filtered_bytes, level);
    if (ZSTD_isError(frame_size)) {
        goto fail;
    }

    /* Stage 1: compressed frame -> filtered bytes. */
    for (i = 0; i < reps; i++) {
        double t0 = now_sec();
        size_t dsize = ZSTD_decompress(filtered, max_filtered,
                                       frame, frame_size);
        t[i] = now_sec() - t0;
        if (ZSTD_isError(dsize) || dsize != filtered_bytes) {
            fprintf(stderr, "error: zstd stage failed (%s)\n",
                    filter_name(filter));
            goto fail;
        }
    }
    zstd_ms = median(t, reps) * 1e3;

    /* Stage 2: filtered bytes -> pixels. reverse_filter does not modify its
       input, so every rep starts from identical state. */
    for (i = 0; i < reps; i++) {
        double t0 = now_sec();
        int ok = reverse_filter(filter, g.pixel_bytes, filtered,
                                filtered_bytes, pixels, g.filter_width,
                                img->height);
        t[i] = now_sec() - t0;
        if (!ok) {
            fprintf(stderr, "error: unfilter stage failed (%s)\n",
                    filter_name(filter));
            goto fail;
        }
    }
    unfilter_ms = median(t, reps) * 1e3;

    /* Confirm the two stages together really reproduce the image, so we are not
       timing a path that silently decodes to garbage. */
    if (memcmp(pixels, img->buffer.data, g.raw_bytes) != 0) {
        fprintf(stderr, "error: staged decode is not lossless (%s)\n",
                filter_name(filter));
        goto fail;
    }

    printf("%-9s %10.3f %10.3f %10.3f %7.1f%% %14zu\n",
           filter_name(filter), zstd_ms, unfilter_ms, zstd_ms + unfilter_ms,
           100.0 * unfilter_ms / (zstd_ms + unfilter_ms), frame_size);

    free(staging);
    free(frame);
    free(filtered);
    free(pixels);
    return 1;

fail:
    free(staging);
    free(frame);
    free(filtered);
    free(pixels);
    return 0;
}

int main(int argc, char** argv)
{
    static const uint8_t filters[] = {
        PXL_FILTER_ADAPTIVE, PXL_FILTER_BCIF, PXL_FILTER_DELTA, PXL_FILTER_NONE
    };
    pxl_image img;
    double*   t;
    int       reps, level;
    size_t    fi;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <image.png> [reps] [level]\n", argv[0]);
        return 2;
    }
    reps  = argc > 2 ? atoi(argv[2]) : 9;
    level = argc > 3 ? atoi(argv[3]) : PXL_LEVEL_DEFAULT;
    if (reps < 1) reps = 1;

    img = pxl_load_png(argv[1]);
    if (!img.buffer.data) {
        fprintf(stderr, "error: cannot read %s as PNG\n", argv[1]);
        return 1;
    }

    t = (double*)malloc(sizeof *t * (size_t)reps);
    if (!t) {
        pxl_image_free(&img);
        return 1;
    }

    printf("%s  %ux%u  %u channels  %u-bit  raw %zu bytes  (median of %d, "
           "level %d)\n\n",
           argv[1], img.width, img.height, img.channels, img.bit_depth,
           img.buffer.size, reps, level);
    printf("%-9s %10s %10s %10s %8s %14s\n",
           "filter", "zstd ms", "unfilt ms", "total ms", "unfilt%",
           "encoded bytes");

    for (fi = 0; fi < sizeof filters / sizeof filters[0]; fi++) {
        measure(&img, filters[fi], level, reps, t);
    }

    free(t);
    pxl_image_free(&img);
    return 0;
}

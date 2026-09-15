/** \file encstages.c
    \brief Splits PXL encode time into filtering and zstd compression, and
           tests whether a cheap probe level picks the same filter as the
           full-level search.

    The question this answers: pxl_encode_ex() tries every candidate colour
    filter at the requested zstd level and keeps the smallest result, so a
    level-12 encode pays for four full-level compressions and throws three
    away. Two things decide whether that is worth it:

      1. Where the time actually goes (filter vs zstd, per candidate).
      2. Whether compressing each candidate at a cheap probe level ranks the
         filters the same way the full level does. If the ranking agrees, the
         encoder can probe cheaply, then compress the winner once at the real
         level, and the output stays byte-identical to today's.

    This is the encode-side counterpart to stages.c and follows it exactly:
    apply_filter is static inside the codec, so the translation unit is
    included directly rather than widening the public API for a benchmark.

    Usage:
      bench/encstages <image.png> [reps] [level] [probe]

    Prints, per colour filter available for the image, the median wall time of
    the filter stage and of the zstd stage, plus the encoded size at both the
    full level and the probe level. The final lines report which filter each
    level would choose and the wall time of the two strategies.
*/

#include "../src/pxl_codec_encode.c"

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

/* Per-candidate measurement, mirroring one iteration of the encoder's search
   loop. Returns 0 if this filter cannot represent the image. */
typedef struct {
    uint8_t filter;
    double  filter_ms;
    double  zstd_ms;    /* compression at the full level */
    double  probe_ms;   /* compression at the probe level */
    size_t  full_bytes;
    size_t  probe_bytes;
} cand_result;

static int measure(const pxl_image* img, uint8_t filter, int level, int probe,
                   int reps, double* t, cand_result* out)
{
    pxl_geometry g;
    uint8_t*     staging = NULL;  /* filter output */
    uint8_t*     frame   = NULL;  /* compressed bytes */
    size_t       max_filtered, filtered_bytes = 0, bound, csz;
    uint8_t      depth = pxl_bit_depth(img);
    int          i;

    /* BCIF is defined only for 8-bit RGB/RGBA without a palette (SPEC 2.2). */
    if (filter == PXL_FILTER_BCIF &&
        (depth != 8 || img->palette.size != 0 ||
         (img->channels != 3 && img->channels != 4))) {
        return 0;
    }

    if (!pxl_geometry_of(img->width, img->height, img->channels, depth, &g)) {
        return 0;
    }

    max_filtered = pxl_filtered_size(filter, g.filter_width, img->height,
                                     g.pixel_bytes);
    bound   = ZSTD_compressBound(max_filtered);
    staging = (uint8_t*)malloc(max_filtered);
    frame   = (uint8_t*)malloc(bound);
    if (!staging || !frame) {
        goto fail;
    }

    /* Stage 1: pixels -> filtered bytes. apply_filter does not modify its
       input, so every rep starts from identical state. */
    for (i = 0; i < reps; i++) {
        double t0 = now_sec();
        filtered_bytes = apply_filter(filter, g.pixel_bytes, img->buffer.data,
                                      staging, g.filter_width, img->height);
        t[i] = now_sec() - t0;
        if (filtered_bytes == 0 || filtered_bytes != max_filtered) {
            goto fail;
        }
    }
    out->filter_ms = median(t, reps) * 1e3;

    /* Stage 2: filtered bytes -> compressed frame, at the requested level. */
    for (i = 0; i < reps; i++) {
        double t0 = now_sec();
        csz = ZSTD_compress(frame, bound, staging, filtered_bytes, level);
        t[i] = now_sec() - t0;
        if (ZSTD_isError(csz)) {
            fprintf(stderr, "error: zstd stage failed (%s)\n",
                    filter_name(filter));
            goto fail;
        }
        out->full_bytes = csz;
    }
    out->zstd_ms = median(t, reps) * 1e3;

    /* Same again at the probe level, to compare both ranking and cost. */
    for (i = 0; i < reps; i++) {
        double t0 = now_sec();
        csz = ZSTD_compress(frame, bound, staging, filtered_bytes, probe);
        t[i] = now_sec() - t0;
        if (ZSTD_isError(csz)) {
            fprintf(stderr, "error: zstd probe failed (%s)\n",
                    filter_name(filter));
            goto fail;
        }
        out->probe_bytes = csz;
    }
    out->probe_ms = median(t, reps) * 1e3;

    out->filter = filter;
    free(staging);
    free(frame);
    return 1;

fail:
    free(staging);
    free(frame);
    return 0;
}

int main(int argc, char** argv)
{
    /* Same candidate set and tie-breaking order as pxl_encode_ex(): cheapest
       decode first, and a later candidate must be strictly smaller to win. */
    static const uint8_t candidates[] = {
        PXL_FILTER_NONE, PXL_FILTER_DELTA, PXL_FILTER_ADAPTIVE,
        PXL_FILTER_BCIF
    };
    cand_result res[sizeof candidates / sizeof candidates[0]];
    pxl_image   img;
    double*     t;
    int         reps, level, probe, i, n = 0;
    int         best_full = -1, best_probe = -1;
    double      search_ms = 0.0, probe_total_ms = 0.0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <image.png> [reps] [level] [probe]\n",
                argv[0]);
        return 2;
    }
    reps  = argc > 2 ? atoi(argv[2]) : 9;
    level = argc > 3 ? atoi(argv[3]) : PXL_LEVEL_DEFAULT;
    probe = argc > 4 ? atoi(argv[4]) : 1;
    if (reps < 1) { reps = 1; }

    img = pxl_load_png(argv[1]);
    if (img.buffer.data == NULL) {
        fprintf(stderr, "error: cannot read %s as PNG\n", argv[1]);
        return 1;
    }

    t = (double*)malloc((size_t)reps * sizeof *t);
    if (!t) {
        pxl_image_free(&img);
        return 1;
    }

    printf("%s  %ux%u  %u channels  %u-bit  raw %zu bytes  "
           "(median of %d, level %d, probe %d)\n\n",
           argv[1], img.width, img.height, img.channels, pxl_bit_depth(&img),
           img.buffer.size, reps, level, probe);
    printf("%-9s %10s %10s %10s %12s %12s\n",
           "filter", "filter ms", "zstd ms", "probe ms",
           "full bytes", "probe bytes");

    for (i = 0; i < (int)(sizeof candidates / sizeof candidates[0]); ++i) {
        cand_result r;
        memset(&r, 0, sizeof r);
        if (!measure(&img, candidates[i], level, probe, reps, t, &r)) {
            continue;
        }
        res[n++] = r;
        printf("%-9s %10.3f %10.3f %10.3f %12zu %12zu\n",
               filter_name(r.filter), r.filter_ms, r.zstd_ms, r.probe_ms,
               r.full_bytes, r.probe_bytes);
    }

    /* Pick a winner under each level, using the encoder's strict-improvement
       rule so ties go to the earlier (faster-decoding) candidate. */
    for (i = 0; i < n; ++i) {
        if (best_full < 0 || res[i].full_bytes < res[best_full].full_bytes) {
            best_full = i;
        }
        if (best_probe < 0 || res[i].probe_bytes < res[best_probe].probe_bytes) {
            best_probe = i;
        }
        /* Today's encoder: filter + full-level compress, every candidate. */
        search_ms += res[i].filter_ms + res[i].zstd_ms;
        /* Probe strategy: filter + cheap compress for every candidate. */
        probe_total_ms += res[i].filter_ms + res[i].probe_ms;
    }
    if (n == 0) {
        fprintf(stderr, "error: no filter could represent this image\n");
        free(t);
        pxl_image_free(&img);
        return 1;
    }
    /* Then one full-level compress of the winner (its filter output has to be
       regenerated, hence its filter stage is paid twice overall). */
    probe_total_ms += res[best_probe].filter_ms + res[best_probe].zstd_ms;

    /* Second-best under the probe. A "probe top-2" strategy compresses both of
       the probe's leaders at the full level and keeps the smaller, which
       recovers the true winner whenever the probe merely misranks it by one. */
    {
        int second = -1;
        for (i = 0; i < n; ++i) {
            if (i == best_probe) { continue; }
            if (second < 0 || res[i].probe_bytes < res[second].probe_bytes) {
                second = i;
            }
        }
        if (second >= 0) {
            int top2 = res[second].full_bytes < res[best_probe].full_bytes
                           ? second : best_probe;
            double top2_ms = probe_total_ms + res[second].filter_ms
                             + res[second].zstd_ms;
            printf("\nprobe top-2       : %-9s %zu bytes  (%s)\n",
                   filter_name(res[top2].filter), res[top2].full_bytes,
                   res[top2].full_bytes == res[best_full].full_bytes
                       ? "matches full search"
                       : "still worse than full search");
            printf("probe top-2 time  : %8.3f ms  (%.2fx)\n", top2_ms,
                   top2_ms > 0.0 ? search_ms / top2_ms : 0.0);
        }
    }

    printf("\nfull-level winner : %-9s %zu bytes\n",
           filter_name(res[best_full].filter), res[best_full].full_bytes);
    printf("probe winner      : %-9s %zu bytes at level %d\n",
           filter_name(res[best_probe].filter), res[best_probe].probe_bytes,
           probe);
    printf("agreement         : %s\n",
           res[best_full].filter == res[best_probe].filter
               ? "same filter (probe is safe here)"
               : "DIFFERENT filter");
    if (res[best_full].filter != res[best_probe].filter) {
        size_t got  = res[best_probe].full_bytes;
        size_t want = res[best_full].full_bytes;
        printf("cost of probe     : %zu vs %zu bytes (%+.2f%%)\n",
               got, want, 100.0 * ((double)got - (double)want) / (double)want);
    }
    printf("search-all time   : %8.3f ms\n", search_ms);
    printf("probe-then-one    : %8.3f ms  (%.2fx)\n", probe_total_ms,
           probe_total_ms > 0.0 ? search_ms / probe_total_ms : 0.0);

    free(t);
    pxl_image_free(&img);
    return 0;
}

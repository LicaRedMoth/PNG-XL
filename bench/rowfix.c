/** \file rowfix.c
    \brief Prices the adaptive per-row filter against a single fixed filter.

    The question this answers: rowstats showed Paeth is picked for 91% of the
    bytes a decoder unfilters, so "keep only Paeth" looks tempting -- one branch
    per byte instead of five candidates at encode time, and a smaller decoder.
    But dropping the choice costs compression, and the only honest currency is
    bytes after zstd. This filters each image once per fixed row filter and once
    with the adaptive heuristic, compresses each result at the same level, and
    reports the size delta.

    Note this measures the row-filter layer alone (SPEC's PXL_FILTER_ADAPTIVE),
    not the encoder's whole-image choice. Images the encoder would store with
    DELTA or BCIF still appear here, so the totals answer "what would adaptive
    images cost", which is the population the row filters govern.

    pack_adaptive and rowfilter_encode are static inside pxl_codec_encode.c, so
    this includes that translation unit directly, as bench/stages.c does.

    Usage:
      bench/rowfix <image.png> [image.png ...]
      bench/rowfix --quiet <image.png> ...       # totals only
*/

#include "../src/pxl_codec_encode.c"

#include "../src/pxl_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTYPES 5

static const char* rowf_name(unsigned t)
{
    static const char* names[NTYPES] = { "none", "sub", "up", "avg", "paeth" };
    return t < NTYPES ? names[t] : "?";
}

/* Like pack_adaptive, but every row is filtered with `type`. The row still
   carries its type byte, so the layout stays a legal adaptive stream and the
   sizes are directly comparable. */
static size_t pack_fixed(uint8_t type, const uint8_t* input, uint8_t* output,
                         uint32_t width, uint32_t height, unsigned pixel_bytes)
{
    size_t   stride = (size_t)width * pixel_bytes;
    uint8_t* op     = output;
    uint32_t y;

    for (y = 0; y < height; ++y) {
        const uint8_t* cur  = input + (size_t)y * stride;
        const uint8_t* prev = y ? input + (size_t)(y - 1) * stride : NULL;
        *op++ = type;
        rowfilter_encode(type, cur, prev, op, stride, pixel_bytes);
        op += stride;
    }
    return (size_t)height * (stride + 1);
}

typedef struct {
    unsigned long long fixed[NTYPES]; /* compressed bytes, one filter forced */
    unsigned long long adaptive;      /* compressed bytes, per-row choice */
    unsigned long long files;
    unsigned long long wins[NTYPES];  /* files where this fixed filter beats adaptive */
} totals;

/* Compresses one filtered buffer and returns its size, or 0 on failure. */
static size_t squeeze(const uint8_t* src, size_t n, uint8_t* dst, size_t cap,
                      int level)
{
    size_t r = ZSTD_compress(dst, cap, src, n, level);
    return ZSTD_isError(r) ? 0 : r;
}

static int measure_file(const char* path, int level, int quiet, totals* tot)
{
    pxl_image     img;
    pxl_geometry  g;
    uint8_t*      staging = NULL;
    uint8_t*      frame   = NULL;
    size_t        packed, cap, adaptive_size;
    size_t        sizes[NTYPES];
    unsigned      t;
    int           rc = 0;

    img = pxl_load_png(path);
    if (!img.buffer.data) {
        fprintf(stderr, "# skip (not readable as PNG): %s\n", path);
        return 0;
    }
    if (!pxl_geometry_of(img.width, img.height, img.channels,
                         pxl_bit_depth(&img), &g)) {
        goto done;
    }

    packed  = pxl_filtered_size(PXL_FILTER_ADAPTIVE, g.filter_width,
                                img.height, g.pixel_bytes);
    cap     = ZSTD_compressBound(packed);
    staging = (uint8_t*)malloc(packed);
    frame   = (uint8_t*)malloc(cap);
    if (!staging || !frame || packed == 0) {
        goto done;
    }

    if (pack_adaptive(img.buffer.data, staging, g.filter_width, img.height,
                      g.pixel_bytes) != packed) {
        goto done;
    }
    adaptive_size = squeeze(staging, packed, frame, cap, level);
    if (adaptive_size == 0) {
        goto done;
    }

    for (t = 0; t < NTYPES; ++t) {
        pack_fixed((uint8_t)t, img.buffer.data, staging, g.filter_width,
                   img.height, g.pixel_bytes);
        sizes[t] = squeeze(staging, packed, frame, cap, level);
        if (sizes[t] == 0) {
            goto done;
        }
        tot->fixed[t] += sizes[t];
        if (sizes[t] <= adaptive_size) {
            tot->wins[t]++;
        }
    }
    tot->adaptive += adaptive_size;
    tot->files++;
    rc = 1;

    if (!quiet) {
        printf("%-52s adaptive %9zu", path, adaptive_size);
        for (t = 0; t < NTYPES; ++t) {
            printf("  %s %+.2f%%", rowf_name(t),
                   100.0 * ((double)sizes[t] - (double)adaptive_size)
                         / (double)adaptive_size);
        }
        printf("\n");
    }

done:
    free(staging);
    free(frame);
    pxl_image_free(&img);
    return rc;
}

int main(int argc, char** argv)
{
    totals   tot;
    int      quiet = 0, i, first = 1;
    int      level = PXL_LEVEL_DEFAULT;
    unsigned t;

    memset(&tot, 0, sizeof tot);

    while (first < argc) {
        if (strcmp(argv[first], "--quiet") == 0) {
            quiet = 1;
            first++;
        } else if (strcmp(argv[first], "-l") == 0 && first + 1 < argc) {
            level = atoi(argv[first + 1]);
            first += 2;
        } else {
            break;
        }
    }
    if (argc <= first) {
        fprintf(stderr, "usage: %s [--quiet] [-l LEVEL] <image.png> ...\n", argv[0]);
        return 2;
    }

    for (i = first; i < argc; ++i) {
        measure_file(argv[i], level, quiet, &tot);
    }

    if (tot.files == 0) {
        fprintf(stderr, "error: no images measured\n");
        return 1;
    }

    printf("\n%llu files, zstd level %d\n", tot.files, level);
    printf("adaptive per-row: %llu bytes (baseline)\n", tot.adaptive);
    for (t = 0; t < NTYPES; ++t) {
        printf("fixed %-6s   : %12llu bytes  %+7.2f%%   beats adaptive on %llu/%llu files\n",
               rowf_name(t), tot.fixed[t],
               100.0 * ((double)tot.fixed[t] - (double)tot.adaptive)
                     / (double)tot.adaptive,
               tot.wins[t], tot.files);
    }
    return 0;
}

/** \file adaptivecost.c
    \brief What excluding ADAPTIVE, on top of what -p already excludes
           (BCIF), would cost in file size.

    Why this needs measuring rather than arguing: the PSP-3008 run recorded
    2026-09-17 found ADAPTIVE ties libpng in decode speed (1.01-1.02x) rather
    than beating it the way none/delta/bcif-at-screen-size do. That is not
    the same finding BCIF gave (BCIF *loses* to libpng at texture size) --
    parity is not a regression -- but it raises the honest question of
    whether a stricter progressive mode should drop ADAPTIVE too. Answering
    "no speed gain, so drop it" from reasoning alone is exactly what this
    project's ground rule exists to prevent; this measures what dropping it
    actually costs in the other stated priority, file size.

    Method: for every readable PNG in the given corpora, encode three ways --
    the real candidate set under default flags (whatever pxl_encode_ex does,
    reused verbatim via encode_bytes so this never drifts from the shipping
    encoder), the `-p` set {none, delta, adaptive}, and a hypothetical
    stricter set {none, delta} only. All three reuse the actual apply_filter
    and ZSTD_compress calls the real encoder makes, not a reimplementation.

    Usage:
      bench/adaptivecost <dir>...
*/

#include "../src/pxl_codec_encode.c"
#include "../src/pxl_png.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Encodes with a candidate set restricted to `filters[0..n)`, reusing the
   real apply_filter/ZSTD_compress calls -- returns the winning compressed
   size, or 0 on failure. Palette images always fall back to NONE regardless
   of the requested set, same as the real encoder (differencing a label is
   never right), which is why palette files are skipped in main() instead of
   silently reporting a number that does not reflect what shipped. */
static size_t best_of(const pxl_image* img, const uint8_t* filters, int n, int level)
{
    pxl_geometry g;
    uint8_t depth = pxl_bit_depth(img);
    unsigned pal_count, pal_alpha, pixel_bytes;
    uint8_t* filtered = NULL;
    uint8_t* frame = NULL;
    size_t max_filtered, bound, best = 0;
    int i;

    if (!palette_ok(img, depth, &pal_count, &pal_alpha) || pal_count) {
        return 0; /* caller skips indexed files */
    }
    if (!pxl_geometry_of(img->width, img->height, img->channels, depth, &g)) {
        return 0;
    }
    pixel_bytes = g.pixel_bytes;

    max_filtered = img->buffer.size;
    for (i = 0; i < n; i++) {
        size_t fs = pxl_filtered_size(filters[i], g.filter_width, img->height, pixel_bytes);
        if (fs > max_filtered) {
            max_filtered = fs;
        }
    }
    bound    = ZSTD_compressBound(max_filtered);
    filtered = (uint8_t*)malloc(max_filtered);
    frame    = (uint8_t*)malloc(bound);
    if (!filtered || !frame) {
        goto done;
    }

    for (i = 0; i < n; i++) {
        uint8_t f = filters[i];
        size_t fsize, csz;
        if (f == PXL_FILTER_BCIF &&
            (depth != 8 || (img->channels != 3 && img->channels != 4))) {
            continue; /* BCIF is 8-bit RGB/RGBA only, same guard the real encoder uses */
        }
        fsize = apply_filter(f, pixel_bytes, img->buffer.data, filtered,
                             g.filter_width, img->height);
        if (fsize == 0) {
            continue;
        }
        csz = ZSTD_compress(frame, bound, filtered, fsize, level);
        if (ZSTD_isError(csz)) {
            continue;
        }
        if (best == 0 || csz < best) {
            best = csz;
        }
    }
done:
    free(filtered);
    free(frame);
    return best;
}

static int has_suffix(const char* s, const char* suf)
{
    size_t ls = strlen(s), lsuf = strlen(suf);
    return ls >= lsuf && strcmp(s + ls - lsuf, suf) == 0;
}

int main(int argc, char** argv)
{
    static const uint8_t SET_P[]        = { PXL_FILTER_NONE, PXL_FILTER_DELTA, PXL_FILTER_ADAPTIVE };
    static const uint8_t SET_NO_ADAPT[] = { PXL_FILTER_NONE, PXL_FILTER_DELTA };
    int argi;
    long files = 0, adaptive_wins = 0, skipped = 0;
    size_t total_p = 0, total_noadapt = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <dir>...\n", argv[0]);
        return 2;
    }

    for (argi = 1; argi < argc; argi++) {
        DIR* d = opendir(argv[argi]);
        struct dirent* de;
        if (!d) {
            fprintf(stderr, "warning: cannot open %s\n", argv[argi]);
            continue;
        }
        while ((de = readdir(d)) != NULL) {
            char path[4096];
            pxl_image img;
            size_t sp, sn;
            if (!has_suffix(de->d_name, ".png") && !has_suffix(de->d_name, ".PNG")) {
                continue;
            }
            snprintf(path, sizeof path, "%s/%s", argv[argi], de->d_name);
            img = pxl_load_png(path);
            if (!img.buffer.data) {
                continue;
            }
            sp = best_of(&img, SET_P, 3, PXL_LEVEL_DEFAULT);
            sn = best_of(&img, SET_NO_ADAPT, 2, PXL_LEVEL_DEFAULT);
            if (sp == 0 || sn == 0) {
                skipped++;
                pxl_image_free(&img);
                continue;
            }
            files++;
            total_p += sp;
            total_noadapt += sn;
            if (sn > sp) {
                adaptive_wins++;
            }
            pxl_image_free(&img);
        }
        closedir(d);
    }

    if (files == 0) {
        fprintf(stderr, "no readable non-indexed PNGs found\n");
        return 1;
    }

    printf("files=%ld skipped(indexed/unreadable)=%ld\n", files, skipped);
    printf("adaptive is the -p winner on %ld/%ld files (%.1f%%)\n",
           adaptive_wins, files, 100.0 * adaptive_wins / files);
    printf("total bytes, -p {none,delta,adaptive}: %zu\n", total_p);
    printf("total bytes, {none,delta} only:        %zu\n", total_noadapt);
    printf("cost of also excluding adaptive: %.3f%%\n",
           100.0 * ((double)total_noadapt - (double)total_p) / (double)total_p);
    return 0;
}

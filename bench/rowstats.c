/** \file rowstats.c
    \brief Counts which per-row filters the adaptive filter actually picks.

    The question this answers: is Paeth worth optimising? The stage benchmark
    showed unfiltering an adaptive image costs 16x more than delta, and that the
    remaining cost is Paeth's three data-dependent comparisons per byte. Before
    spending effort there (SIMD, or restricting the filter set, which would
    change the format) we need to know how often Paeth is chosen at all. If it
    is rare, the slow path is rare and there is nothing to win.

    Method: for every image, run the encoder's own row-filter selection
    (pack_adaptive's heuristic, reimplemented here so the per-row choice can be
    observed) and histogram the chosen types, both by row count and by bytes --
    bytes being what decode time is proportional to. Also reports whether
    PXL_FILTER_ADAPTIVE actually wins the encode for that image, since row
    filters only ever run at decode time when it does.

    pack_adaptive() and friends are static inside pxl_codec_encode.c, so this
    includes that translation unit directly, as bench/stages.c does.

    Usage:
      bench/rowstats <image.png> [image.png ...]     # one line per image + total
      bench/rowstats --quiet <image.png> ...         # totals only
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

typedef struct {
    unsigned long long rows[NTYPES];
    unsigned long long bytes[NTYPES];
    unsigned long long files;          /* files counted */
    unsigned long long adaptive_wins;  /* files where adaptive wins the encode */
    unsigned long long adaptive_rows;  /* rows in files where adaptive wins */
    unsigned long long adaptive_bytes;
    unsigned long long paeth_rows_live;   /* paeth rows that a decoder will run */
    unsigned long long paeth_bytes_live;
} stats;

/* Mirrors pack_adaptive's choice for one row: the filter with the smallest sum
   of absolute signed residuals. Kept in step with pxl_codec.c by using the same
   rowfilter_encode and row_score. */
static int histogram_rows(const pxl_image* img, const pxl_geometry* g,
                          unsigned long long* rows, unsigned long long* bytes)
{
    size_t   stride = (size_t)g->filter_width * g->pixel_bytes;
    uint8_t* cand   = (uint8_t*)malloc(stride);
    uint32_t y;

    if (!cand) {
        return 0;
    }
    for (y = 0; y < img->height; ++y) {
        const uint8_t* cur  = img->buffer.data + (size_t)y * stride;
        const uint8_t* prev = y ? img->buffer.data + (size_t)(y - 1) * stride
                                : NULL;
        uint8_t       best_type = PXL_ROWF_NONE;
        unsigned long best_s    = (unsigned long)-1;
        uint8_t       t;

        for (t = 0; t <= PXL_ROWF_PAETH; ++t) {
            unsigned long s;
            rowfilter_encode(t, cur, prev, cand, stride, g->pixel_bytes);
            s = row_score(cand, stride);
            if (s < best_s) {
                best_s    = s;
                best_type = t;
            }
        }
        rows[best_type]  += 1;
        bytes[best_type] += (unsigned long long)stride;
    }
    free(cand);
    return 1;
}

/* Which colour filter does the real encoder settle on? Re-encoding is the only
   honest way to ask: the choice depends on compressed size, not on a rule. */
static int winning_filter(const pxl_image* img, int level, uint8_t* out)
{
    pxl_buffer enc = pxl_encode_ex(img, level, 0);
    pxl_header h;
    int        ok = 0;

    if (enc.data && pxl_header_read(enc.data, enc.size, &h)) {
        *out = h.color_filter;
        ok   = 1;
    }
    pxl_free(&enc);
    return ok;
}

static void report(const char* label, const unsigned long long* rows,
                   const unsigned long long* bytes)
{
    unsigned long long trow = 0, tbyte = 0;
    unsigned t;

    for (t = 0; t < NTYPES; ++t) {
        trow  += rows[t];
        tbyte += bytes[t];
    }
    if (trow == 0) {
        return;
    }
    printf("\n%s: %llu rows, %llu bytes\n", label, trow, tbyte);
    printf("%-7s %12s %7s %16s %7s\n", "filter", "rows", "%", "bytes", "%");
    for (t = 0; t < NTYPES; ++t) {
        printf("%-7s %12llu %6.2f%% %16llu %6.2f%%\n", rowf_name(t), rows[t],
               100.0 * (double)rows[t] / (double)trow, bytes[t],
               tbyte ? 100.0 * (double)bytes[t] / (double)tbyte : 0.0);
    }
}

int main(int argc, char** argv)
{
    stats st;
    int   quiet = 0, level = PXL_LEVEL_DEFAULT, i, first = 1;

    memset(&st, 0, sizeof st);

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
            first = i + 1;
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            level = atoi(argv[++i]);
            first = i + 1;
        } else {
            break;
        }
    }
    if (first >= argc) {
        fprintf(stderr, "usage: %s [--quiet] [-l level] <image.png> ...\n",
                argv[0]);
        return 2;
    }

    if (!quiet) {
        printf("%-44s %6s %8s %8s  %s\n", "file", "rows", "paeth%", "pbytes%",
               "winner");
    }

    for (i = first; i < argc; ++i) {
        pxl_image          img = pxl_load_png(argv[i]);
        pxl_geometry       g;
        unsigned long long rows[NTYPES], bytes[NTYPES];
        unsigned long long trow = 0, tbyte = 0;
        uint8_t            depth, winner = PXL_FILTER_NONE;
        unsigned           t;
        int                have_winner;

        if (!img.buffer.data) {
            fprintf(stderr, "# skip (not readable as PNG): %s\n", argv[i]);
            continue;
        }
        depth = pxl_bit_depth(&img);
        if (!pxl_geometry_of(img.width, img.height, img.channels, depth, &g)) {
            fprintf(stderr, "# skip (bad geometry): %s\n", argv[i]);
            pxl_image_free(&img);
            continue;
        }

        memset(rows, 0, sizeof rows);
        memset(bytes, 0, sizeof bytes);
        if (!histogram_rows(&img, &g, rows, bytes)) {
            fprintf(stderr, "# skip (out of memory): %s\n", argv[i]);
            pxl_image_free(&img);
            continue;
        }
        have_winner = winning_filter(&img, level, &winner);

        for (t = 0; t < NTYPES; ++t) {
            trow  += rows[t];
            tbyte += bytes[t];
            st.rows[t]  += rows[t];
            st.bytes[t] += bytes[t];
        }
        st.files += 1;
        if (have_winner && winner == PXL_FILTER_ADAPTIVE) {
            st.adaptive_wins  += 1;
            st.adaptive_rows  += trow;
            st.adaptive_bytes += tbyte;
            st.paeth_rows_live  += rows[PXL_ROWF_PAETH];
            st.paeth_bytes_live += bytes[PXL_ROWF_PAETH];
        }

        if (!quiet) {
            const char* wname = !have_winner ? "?"
                              : winner == PXL_FILTER_ADAPTIVE ? "adaptive"
                              : winner == PXL_FILTER_BCIF     ? "bcif"
                              : winner == PXL_FILTER_DELTA    ? "delta"
                              : "none";
            const char* base = strrchr(argv[i], '/');
            printf("%-44s %6llu %7.2f%% %7.2f%%  %s\n", base ? base + 1 : argv[i],
                   trow,
                   trow ? 100.0 * (double)rows[PXL_ROWF_PAETH] / (double)trow : 0.0,
                   tbyte ? 100.0 * (double)bytes[PXL_ROWF_PAETH] / (double)tbyte : 0.0,
                   wname);
        }
        pxl_image_free(&img);
    }

    report("all files", st.rows, st.bytes);

    printf("\nfiles: %llu counted, %llu where adaptive wins the encode "
           "(%.1f%%)\n",
           st.files, st.adaptive_wins,
           st.files ? 100.0 * (double)st.adaptive_wins / (double)st.files : 0.0);
    printf("rows a decoder actually unfilters (adaptive winners only): %llu, "
           "%llu bytes\n", st.adaptive_rows, st.adaptive_bytes);
    printf("of those, paeth: %llu rows (%.2f%%), %llu bytes (%.2f%%)\n",
           st.paeth_rows_live,
           st.adaptive_rows
               ? 100.0 * (double)st.paeth_rows_live / (double)st.adaptive_rows
               : 0.0,
           st.paeth_bytes_live,
           st.adaptive_bytes
               ? 100.0 * (double)st.paeth_bytes_live / (double)st.adaptive_bytes
               : 0.0);
    return 0;
}

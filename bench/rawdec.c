/* Decode-only benchmark: compressed bytes -> raw pixels, nothing else.
 *
 * bench.sh times `pxltool d`, which also re-encodes a PNG on the way out, so
 * its decode column is dominated by libpng's deflate and says little about our
 * decoder. This measures the operation that actually matters for a viewer or a
 * game loading a texture: file already in memory, decode to a pixel buffer.
 *
 * Compares PXL against QOI and libpng on the same source image, reporting
 * throughput in megabytes of output pixels per second.
 *
 *   bench/rawdec <image.png> [reps]
 *
 * Built by CMake as `pxl_bench_rawdec` when vendor/qoi/qoi.h is present.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pxl.h"
#include "pxl_png.h"

#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include "qoi.h"

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Median of n doubles (sorts in place). */
static int cmp_double(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static double median(double* v, int n)
{
    qsort(v, (size_t)n, sizeof *v, cmp_double);
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

static unsigned char* read_file(const char* path, size_t* out_size)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    unsigned char* buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

/* Reports one row: name, median ms, MB/s of decoded pixels, encoded size. */
static void report(const char* name, double* times, int reps,
                   size_t raw_bytes, size_t enc_bytes)
{
    double ms = median(times, reps) * 1e3;
    double mbps = ms > 0.0 ? ((double)raw_bytes / (1024.0 * 1024.0)) / (ms / 1e3) : 0.0;
    printf("%-8s %9.3f %12.1f %13zu\n", name, ms, mbps, enc_bytes);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <image.png> [reps]\n", argv[0]);
        return 2;
    }
    const char* src = argv[1];
    int reps = argc > 2 ? atoi(argv[2]) : 9;
    if (reps < 1) reps = 1;

    /* Load the source through our own PNG front end, then expand palette and
       sub-8-bit depths: QOI only handles 8-bit RGB/RGBA, so all three decoders
       must be given the same plain 8-bit image for the comparison to be fair. */
    pxl_image img = pxl_load_png(src);
    if (!img.buffer.data) {
        fprintf(stderr, "error: cannot read %s as PNG\n", src);
        return 1;
    }
    if (pxl_is_indexed(&img) || pxl_bit_depth(&img) < 8) {
        pxl_image e = pxl_image_expand(&img);
        pxl_image_free(&img);
        if (!e.buffer.data) {
            fprintf(stderr, "error: cannot expand %s to 8-bit\n", src);
            return 1;
        }
        img = e;
    }
    if (img.bytes_per_channel != 1 || (img.channels != 3 && img.channels != 4)) {
        fprintf(stderr, "skip: %s is %u-channel/%u-bit; QOI needs 8-bit RGB or RGBA\n",
                src, img.channels, (unsigned)(img.bytes_per_channel * 8));
        pxl_image_free(&img);
        return 3;
    }

    size_t raw_bytes = img.buffer.size;
    double* t = malloc(sizeof *t * (size_t)reps);
    if (!t) { pxl_image_free(&img); return 1; }

    printf("%s  %ux%u  %u channels  raw %zu bytes  (median of %d)\n\n",
           src, img.width, img.height, img.channels, raw_bytes, reps);
    printf("%-8s %9s %12s %13s\n", "decoder", "ms", "MB/s out", "encoded bytes");

    /* --- PXL ------------------------------------------------------------- */
    pxl_buffer pxl_file = pxl_encode(&img, 12);
    if (pxl_file.data) {
        for (int i = 0; i < reps; i++) {
            double t0 = now_sec();
            pxl_image d = pxl_decode(pxl_file);
            t[i] = now_sec() - t0;
            if (!d.buffer.data) { fprintf(stderr, "error: pxl_decode failed\n"); return 1; }
            if (d.buffer.size != raw_bytes ||
                memcmp(d.buffer.data, img.buffer.data, raw_bytes) != 0) {
                fprintf(stderr, "error: PXL decode is not lossless\n");
                return 1;
            }
            pxl_image_free(&d);
        }
        report("PXL", t, reps, raw_bytes, pxl_file.size);
    }

    /* --- QOI ------------------------------------------------------------- */
    qoi_desc qd;
    qd.width = img.width;
    qd.height = img.height;
    qd.channels = img.channels;
    qd.colorspace = QOI_SRGB;
    int qlen = 0;
    void* qbuf = qoi_encode(img.buffer.data, &qd, &qlen);
    if (qbuf) {
        for (int i = 0; i < reps; i++) {
            qoi_desc od;
            double t0 = now_sec();
            void* px = qoi_decode(qbuf, qlen, &od, (int)img.channels);
            t[i] = now_sec() - t0;
            if (!px) { fprintf(stderr, "error: qoi_decode failed\n"); return 1; }
            if (memcmp(px, img.buffer.data, raw_bytes) != 0) {
                fprintf(stderr, "error: QOI decode is not lossless\n");
                return 1;
            }
            free(px);
        }
        report("QOI", t, reps, raw_bytes, (size_t)qlen);
    }

    /* --- libpng ---------------------------------------------------------- */
    /* Re-encode the expanded image as PNG so all three decode identical
       pixels, then time reading it back. */
    if (pxl_save_png("/tmp/pxl_rawdec_tmp.png", &img)) {
        size_t png_size = 0;
        unsigned char* png_bytes = read_file("/tmp/pxl_rawdec_tmp.png", &png_size);
        for (int i = 0; i < reps; i++) {
            double t0 = now_sec();
            pxl_image d = pxl_load_png("/tmp/pxl_rawdec_tmp.png");
            t[i] = now_sec() - t0;
            if (!d.buffer.data) { fprintf(stderr, "error: libpng read failed\n"); return 1; }
            pxl_image_free(&d);
        }
        report("libpng", t, reps, raw_bytes, png_size);
        free(png_bytes);
        remove("/tmp/pxl_rawdec_tmp.png");
    }

    free(t);
    free(qbuf);
    pxl_free(&pxl_file);
    pxl_image_free(&img);
    return 0;
}

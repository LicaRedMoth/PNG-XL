/** \file formatdec.c
    \brief Decodes one image in several formats to RGBA8888 in memory, and times
           each. The point is a cross-format speed comparison that measures
           decoding and nothing else.

    Why this exists rather than timing djxl/dwebp/avifdec. A CLI run includes
    process startup, file reading and writing the output image, and this project
    has already been caught by exactly that: README's decode column timed
    `pxltool d`, which re-encodes a PNG on the way out, so most of what it
    reported was libpng's deflate. Shelling out to the other decoders would make
    the same mistake four more times.

    Every format is decoded to the same destination -- 8-bit RGBA, top-down,
    tightly packed -- because that is what a viewer or a game actually wants and
    because MB/s is not comparable between a 3-channel and a 4-channel output.
    PXL is additionally reported in its native channel count, since decoding
    straight into a texture without the widening pass is a live roadmap item and
    the difference between the two rows is exactly what it would save.

    Threading: every decoder here runs single-threaded. libjxl and libavif can
    use worker threads in production and are given none, so their rows describe
    one core rather than what `djxl` does on a desktop. That is the right
    comparison for this project -- the target is a 32 MB single-core handheld --
    but it is not the number to quote at someone benchmarking on a laptop, and
    the plots say so.

    Usage:
      bench/formatdec [reps] --png a.png [--pxl a.pxl] [--jxl a.jxl]
                             [--webp a.webp] [--avif a.avif]

    Prints one TSV row per decoder: name, median ms, MB/s of RGBA output,
    encoded bytes. Missing inputs are skipped silently so a driver can pass
    whatever it managed to encode.
*/

#include "../src/pxl.h"

#include <png.h>
#include <jxl/decode.h>
#include <webp/decode.h>
#include <avif/avif.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int cmp_d(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static unsigned char* slurp(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    unsigned char* buf;
    if (!f) { return NULL; }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char*)malloc(*n ? *n : 1);
    if (!buf || fread(buf, 1, *n, f) != *n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    return buf;
}

/* ---- per-format decode into RGBA8888 -------------------------------------
   Each returns the pixel buffer and sets w/h, or NULL. The caller frees. */

typedef struct { const unsigned char* data; size_t size, pos; } mem_reader;

static void png_read_mem(png_structp p, png_bytep out, png_size_t len)
{
    mem_reader* r = (mem_reader*)png_get_io_ptr(p);
    if (r->pos + len > r->size) { png_error(p, "short read"); }
    memcpy(out, r->data + r->pos, len);
    r->pos += len;
}

static unsigned char* dec_png(const unsigned char* buf, size_t n, uint32_t* w, uint32_t* h)
{
    png_structp p = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = p ? png_create_info_struct(p) : NULL;
    mem_reader r = { buf, n, 0 };
    unsigned char* out = NULL;
    png_bytep* rows = NULL;
    uint32_t y;

    if (!p || !info) { goto fail; }
    if (setjmp(png_jmpbuf(p))) { goto fail; }
    png_set_read_fn(p, &r, png_read_mem);
    png_read_info(p, info);

    /* Normalise everything to 8-bit RGBA, which is what the other decoders are
       asked for too. */
    png_set_expand(p);
    png_set_strip_16(p);
    png_set_gray_to_rgb(p);
    png_set_add_alpha(p, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(p, info);

    *w = png_get_image_width(p, info);
    *h = png_get_image_height(p, info);
    out = (unsigned char*)malloc((size_t)*w * *h * 4);
    rows = (png_bytep*)malloc((size_t)*h * sizeof(png_bytep));
    if (!out || !rows) { goto fail; }
    for (y = 0; y < *h; ++y) { rows[y] = out + (size_t)y * *w * 4; }
    png_read_image(p, rows);
    free(rows);
    png_destroy_read_struct(&p, &info, NULL);
    return out;
fail:
    free(out); free(rows);
    if (p) { png_destroy_read_struct(&p, info ? &info : NULL, NULL); }
    return NULL;
}

static unsigned char* dec_jxl(const unsigned char* buf, size_t n, uint32_t* w, uint32_t* h)
{
    JxlDecoder* d = JxlDecoderCreate(NULL);
    JxlBasicInfo info;
    JxlPixelFormat fmt = { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
    unsigned char* out = NULL;
    size_t need = 0;

    if (!d) { return NULL; }
    if (JxlDecoderSubscribeEvents(d, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) { goto done; }
    if (JxlDecoderSetInput(d, buf, n) != JXL_DEC_SUCCESS) { goto done; }
    JxlDecoderCloseInput(d);

    for (;;) {
        JxlDecoderStatus st = JxlDecoderProcessInput(d);
        if (st == JXL_DEC_ERROR || st == JXL_DEC_NEED_MORE_INPUT) { free(out); out = NULL; goto done; }
        if (st == JXL_DEC_BASIC_INFO) {
            if (JxlDecoderGetBasicInfo(d, &info) != JXL_DEC_SUCCESS) { goto done; }
            *w = info.xsize; *h = info.ysize;
        } else if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            if (JxlDecoderImageOutBufferSize(d, &fmt, &need) != JXL_DEC_SUCCESS) { goto done; }
            out = (unsigned char*)malloc(need);
            if (!out) { goto done; }
            if (JxlDecoderSetImageOutBuffer(d, &fmt, out, need) != JXL_DEC_SUCCESS) { free(out); out = NULL; goto done; }
        } else if (st == JXL_DEC_FULL_IMAGE || st == JXL_DEC_SUCCESS) {
            break;
        }
    }
done:
    JxlDecoderDestroy(d);
    return out;
}

static unsigned char* dec_webp(const unsigned char* buf, size_t n, uint32_t* w, uint32_t* h)
{
    int iw = 0, ih = 0;
    uint8_t* p = WebPDecodeRGBA(buf, n, &iw, &ih);
    if (!p) { return NULL; }
    *w = (uint32_t)iw; *h = (uint32_t)ih;
    return p;   /* WebPFree, but plain free() matches the default allocator */
}

static unsigned char* dec_avif(const unsigned char* buf, size_t n, uint32_t* w, uint32_t* h)
{
    avifDecoder* d = avifDecoderCreate();
    avifRGBImage rgb;
    unsigned char* out = NULL;
    if (!d) { return NULL; }
    if (avifDecoderSetIOMemory(d, buf, n) != AVIF_RESULT_OK) { goto done; }
    if (avifDecoderParse(d) != AVIF_RESULT_OK) { goto done; }
    if (avifDecoderNextImage(d) != AVIF_RESULT_OK) { goto done; }
    avifRGBImageSetDefaults(&rgb, d->image);
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = 8;
    if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK) { goto done; }
    if (avifImageYUVToRGB(d->image, &rgb) != AVIF_RESULT_OK) { avifRGBImageFreePixels(&rgb); goto done; }
    *w = rgb.width; *h = rgb.height;
    out = (unsigned char*)malloc((size_t)rgb.width * rgb.height * 4);
    if (out) { memcpy(out, rgb.pixels, (size_t)rgb.width * rgb.height * 4); }
    avifRGBImageFreePixels(&rgb);
done:
    avifDecoderDestroy(d);
    return out;
}

/* PXL, twice: as the decoder natively produces it, and widened to RGBA. */
static unsigned char* dec_pxl(const unsigned char* buf, size_t n, uint32_t* w, uint32_t* h,
                              int to_rgba)
{
    pxl_buffer file = { (unsigned char*)buf, n };
    pxl_image img = pxl_decode(file), ex;
    unsigned char* out;
    if (!img.buffer.data) { return NULL; }
    *w = img.width; *h = img.height;
    if (!to_rgba) {
        out = img.buffer.data;
        img.buffer.data = NULL;
        pxl_image_free(&img);
        return out;
    }
    ex = pxl_image_expand(&img);
    pxl_image_free(&img);
    if (!ex.buffer.data) { return NULL; }
    out = ex.buffer.data;
    ex.buffer.data = NULL;
    pxl_image_free(&ex);
    return out;
}

typedef unsigned char* (*decfn)(const unsigned char*, size_t, uint32_t*, uint32_t*);

static void bench(const char* name, decfn fn, const unsigned char* buf, size_t n, int reps)
{
    double* t;
    uint32_t w = 0, h = 0;
    int i, got = 0;

    if (!buf) { return; }
    t = (double*)malloc((size_t)reps * sizeof(double));
    if (!t) { return; }
    /* One untimed pass first. Without it the decoder measured first pays for
       cold caches and page faults the others do not, which once made the
       heavier of two PXL paths look like the faster one. */
    {
        unsigned char* warm = fn(buf, n, &w, &h);
        if (!warm) { free(t); fprintf(stderr, "# %s: decode failed\n", name); return; }
        free(warm);
    }
    for (i = 0; i < reps; ++i) {
        double a = now_ms();
        unsigned char* px = fn(buf, n, &w, &h);
        double b = now_ms();
        if (!px) { free(t); fprintf(stderr, "# %s: decode failed\n", name); return; }
        free(px);
        t[got++] = b - a;
    }
    qsort(t, (size_t)got, sizeof(double), cmp_d);
    {
        double med = t[got / 2];
        double mb = ((double)w * h * 4) / 1048576.0;
        printf("%s\t%.3f\t%.1f\t%zu\n", name, med, med > 0 ? mb / (med / 1000.0) : 0.0, n);
    }
    free(t);
}

static unsigned char* g_pxl_buf; static size_t g_pxl_n;
static unsigned char* pxl_native(const unsigned char* b, size_t n, uint32_t* w, uint32_t* h)
{ (void)b; (void)n; return dec_pxl(g_pxl_buf, g_pxl_n, w, h, 0); }
static unsigned char* pxl_rgba(const unsigned char* b, size_t n, uint32_t* w, uint32_t* h)
{ (void)b; (void)n; return dec_pxl(g_pxl_buf, g_pxl_n, w, h, 1); }

int main(int argc, char** argv)
{
    int reps = 7, i;
    const char *p_png = NULL, *p_pxl = NULL, *p_jxl = NULL, *p_webp = NULL, *p_avif = NULL;

    for (i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--png")  && i + 1 < argc) { p_png  = argv[++i]; }
        else if (!strcmp(argv[i], "--pxl")  && i + 1 < argc) { p_pxl  = argv[++i]; }
        else if (!strcmp(argv[i], "--jxl")  && i + 1 < argc) { p_jxl  = argv[++i]; }
        else if (!strcmp(argv[i], "--webp") && i + 1 < argc) { p_webp = argv[++i]; }
        else if (!strcmp(argv[i], "--avif") && i + 1 < argc) { p_avif = argv[++i]; }
        else { reps = atoi(argv[i]) > 0 ? atoi(argv[i]) : reps; }
    }
    if (!p_png) { fprintf(stderr, "usage: %s [reps] --png a.png [--pxl a.pxl] ...\n", argv[0]); return 2; }

    printf("decoder\tms\tmb_per_s\tencoded_bytes\n");
    {
        size_t n; unsigned char* b;
        if ((b = slurp(p_png, &n)))  { bench("libpng", dec_png,  b, n, reps); free(b); }
        if (p_jxl  && (b = slurp(p_jxl,  &n))) { bench("JXL",  dec_jxl,  b, n, reps); free(b); }
        if (p_webp && (b = slurp(p_webp, &n))) { bench("WebP", dec_webp, b, n, reps); free(b); }
        if (p_avif && (b = slurp(p_avif, &n))) { bench("AVIF", dec_avif, b, n, reps); free(b); }
        if (p_pxl  && (g_pxl_buf = slurp(p_pxl, &g_pxl_n))) {
            bench("PXL-native", pxl_native, g_pxl_buf, g_pxl_n, reps);
            bench("PXL-RGBA",   pxl_rgba,   g_pxl_buf, g_pxl_n, reps);
            free(g_pxl_buf);
        }
    }
    return 0;
}

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

    Also emits one PNG per size, same pixels, default libpng write settings
    (whatever filter/compression libpng picks on its own -- this is a
    baseline, not a best case for either side) -- so psp/main.c has something
    to compare PXL's decode throughput against, the same way BENCHMARKS.md's
    x86 tables always carry libpng alongside PXL rather than reporting a
    number with nothing to hold it up against.

    Usage:
      bench/mkpsptest > psp/testdata.h
*/

#include "../src/pxl_codec_encode.c"
#include "../src/apxl_codec.c"
#include "../psp/pattern.h"

#include <png.h>

#include <setjmp.h>
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

typedef struct { unsigned char* data; size_t size, cap; } mem_writer;

static void png_write_mem(png_structp p, png_bytep data, png_size_t len)
{
    mem_writer* w = (mem_writer*)png_get_io_ptr(p);
    if (w->size + len > w->cap) {
        size_t ncap = w->cap ? w->cap * 2 : 4096;
        while (ncap < w->size + len) {
            ncap *= 2;
        }
        w->data = (unsigned char*)realloc(w->data, ncap);
        w->cap = ncap;
    }
    memcpy(w->data + w->size, data, len);
    w->size += len;
}

static void png_flush_mem(png_structp p) { (void)p; }

/* Same raw pixels as encode_forced() sees, written as an ordinary PNG with
   libpng's own default filter/compression choices -- this is the baseline
   psp/main.c's libpng decode reads back, not a hand-picked best case. */
static pxl_buffer encode_png(const uint8_t* raw, uint32_t w, uint32_t h)
{
    pxl_buffer out = { NULL, 0 };
    png_structp p = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = p ? png_create_info_struct(p) : NULL;
    mem_writer mw = { NULL, 0, 0 };
    png_bytep* rows = NULL;
    uint32_t y;

    if (!p || !info) {
        goto fail;
    }
    if (setjmp(png_jmpbuf(p))) {
        goto fail;
    }
    png_set_write_fn(p, &mw, png_write_mem, png_flush_mem);
    png_set_IHDR(p, info, w, h, 8, PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
                PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(p, info);
    rows = (png_bytep*)malloc((size_t)h * sizeof(png_bytep));
    if (!rows) {
        goto fail;
    }
    for (y = 0; y < h; y++) {
        rows[y] = (png_bytep)(raw + (size_t)y * w * 4);
    }
    png_write_image(p, rows);
    png_write_end(p, NULL);
    free(rows);
    png_destroy_write_struct(&p, &info);
    out.data = mw.data;
    out.size = mw.size;
    return out;
fail:
    free(rows);
    free(mw.data);
    if (p) {
        png_destroy_write_struct(&p, info ? &info : NULL);
    }
    return out;
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

/* Builds the embedded animated .apxl for the "Animated .apxl texture
   playback on PSP" / "PSP/GE path" ROADMAP items: PXL_PSP_ANIM_FRAMES
   indexed frames, PXL_PSP_ANIM_W x PXL_PSP_ANIM_H, from pattern.h's
   pxl_psp_anim_index()/pxl_psp_anim_palette -- hand-built directly (one
   pxl_image per frame, all sharing the one fixed palette) rather than via
   apxl_anim_try_index, since the point here is testing apxl_decode and the
   GE path on real MIPS, not re-testing the auto-palette builder (already
   covered by tests/roundtrip.c on host). Palette has no alpha buffer (every
   entry is opaque), exercising pxl_convert_palette's "no alpha" default the
   same way tests/roundtrip.c's check_convert_palette already does on host. */
static pxl_buffer make_anim_apxl(void)
{
    apxl_anim a;
    pxl_buffer enc;
    uint32_t f, x, y;
    size_t frame_bytes = (size_t)PXL_PSP_ANIM_W * PXL_PSP_ANIM_H;
    uint8_t pal_rgb[PXL_PSP_ANIM_PALETTE_COUNT * 3];

    for (f = 0; f < PXL_PSP_ANIM_PALETTE_COUNT; f++) {
        pal_rgb[f * 3 + 0] = pxl_psp_anim_palette[f][0];
        pal_rgb[f * 3 + 1] = pxl_psp_anim_palette[f][1];
        pal_rgb[f * 3 + 2] = pxl_psp_anim_palette[f][2];
    }

    memset(&a, 0, sizeof a);
    a.frames = (apxl_frame*)calloc(PXL_PSP_ANIM_FRAMES, sizeof(apxl_frame));
    if (!a.frames) {
        pxl_buffer none = { NULL, 0 };
        return none;
    }
    a.frame_count = PXL_PSP_ANIM_FRAMES;
    a.loop_count = 0; /* infinite */
    a.canvas_w = PXL_PSP_ANIM_W;
    a.canvas_h = PXL_PSP_ANIM_H;
    a.channels = 1;
    a.bytes_per_channel = 1;

    for (f = 0; f < PXL_PSP_ANIM_FRAMES; f++) {
        uint8_t* buf = (uint8_t*)malloc(frame_bytes);
        if (!buf) {
            pxl_buffer none = { NULL, 0 };
            for (y = 0; y < f; y++) { free(a.frames[y].image.buffer.data); }
            free(a.frames);
            return none;
        }
        for (y = 0; y < PXL_PSP_ANIM_H; y++) {
            for (x = 0; x < PXL_PSP_ANIM_W; x++) {
                buf[y * PXL_PSP_ANIM_W + x] = pxl_psp_anim_index(x, y, f);
            }
        }
        a.frames[f].image.buffer.data = buf;
        a.frames[f].image.buffer.size = frame_bytes;
        a.frames[f].image.width = PXL_PSP_ANIM_W;
        a.frames[f].image.height = PXL_PSP_ANIM_H;
        a.frames[f].image.channels = 1;
        a.frames[f].image.bytes_per_channel = 1;
        /* Static storage, freed only once below via pal_rgb going out of
           scope -- every frame's palette.data points at the same bytes, as
           apxl_encode requires. */
        a.frames[f].image.palette.data = pal_rgb;
        a.frames[f].image.palette.size = sizeof pal_rgb;
        a.frames[f].delay_num = 1;
        a.frames[f].delay_den = 12; /* 12 fps, an ordinary UI-animation rate */
    }

    enc = apxl_encode(&a, APXL_LEVEL_DEFAULT);

    for (f = 0; f < PXL_PSP_ANIM_FRAMES; f++) {
        free(a.frames[f].image.buffer.data);
    }
    free(a.frames);
    return enc;
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

        {
            pxl_buffer png = encode_png(raw, sizes[si].w, sizes[si].h);
            if (!png.data) {
                fprintf(stderr, "error: PNG encode of '%s' failed\n", sizes[si].name);
                return 1;
            }
            fprintf(stderr, "  %-8s -> %zu PNG bytes\n", "png", png.size);
            snprintf(name, sizeof name, "pxl_psp_%s_png", sizes[si].name);
            emit_array(name, png.data, png.size);
            free(png.data);
        }

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

    {
        pxl_buffer anim = make_anim_apxl();
        if (!anim.data) {
            fprintf(stderr, "error: building embedded animated .apxl failed\n");
            return 1;
        }
        fprintf(stderr, "anim     %ux%u, %u frames, %u-colour palette -> %zu APXL bytes\n",
                (unsigned)PXL_PSP_ANIM_W, (unsigned)PXL_PSP_ANIM_H,
                (unsigned)PXL_PSP_ANIM_FRAMES, (unsigned)PXL_PSP_ANIM_PALETTE_COUNT,
                anim.size);
        emit_array("pxl_psp_anim_file", anim.data, anim.size);
        free(anim.data);
    }

    printf("#endif\n");
    return 0;
}

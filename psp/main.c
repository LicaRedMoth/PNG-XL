/** \file main.c
    \brief PSP decode benchmark: correctness on real MIPS/Allegrex hardware,
           then decode throughput at both 222 and 333 MHz, split by filter --
           the measurement ROADMAP.md's "measure decode on the target
           hardware" entry asks for.

    Every result goes to three places at once, because each is missing on one
    of the two places this runs:
      - sceIoWrite(1, ...) -- the primitive PPSSPP's own "pspautotests" suite
        uses; headless mode captures it as "emulated printfs" with no setup.
        On real hardware fd 1 is not connected to anything without a debug
        cable, so this alone would print into a void there.
      - pspDebugScreenPrintf -- draws text on the PSP's own screen via the GU.
        This is what a human looking at real hardware actually sees; headless
        mode has no display; it is not touched there.
      - a file on the memory stick -- the only copy that survives after the
        program exits, and the one meant to leave the console: pull the memory
        stick or connect over USB, read results.txt, and the numbers in it are
        what should go into BENCHMARKS.md. The screen and the emulator log are
        both read-it-once; this one is not.
    Plain libc stdout was not used for any of this because whether it is
    buffered, or wired to fd 1 at all, depends on newlib's startup code in a
    way this project has not verified -- sceIoWrite is one layer lower and
    leaves nothing to that assumption.

    Correctness check: three sizes (smoke/screen/texture, see
    bench/mkpsptest.c) times four filters plus libpng, each decoded and
    compared byte-for-byte against pixels re-derived from psp/pattern.h on the
    spot -- not an embedded reference image, which would have cost several
    megabytes as hex text for no reason a formula doesn't already cover.

    Throughput sweep: screen (480x272, the PSP's own display resolution) and
    texture (512x512, a common GE texture size) only -- the smoke case is too
    small for a throughput number to mean anything, per-call overhead
    dominates it. Run at both 222 and 333 MHz via scePowerSetClockFrequency,
    median of several reps per (clock, size, filter-or-libpng) cell. libpng
    decodes the same pixels via the same normalise-to-RGBA8888 path
    bench/formatdec.c uses on x86 (png_set_expand/strip_16/gray_to_rgb/
    add_alpha), so the two numbers mean the same thing: MB/s of finished
    RGBA8888 output, not MB/s of whatever each format's own native layout
    happens to be. Without this row there was nothing to compare PXL's PSP
    numbers against except each other. The actual clock in effect and the
    free memory at the time are printed alongside every result, the same way
    bench/bench.sh states the load average it ran under: a measurement that
    does not state its conditions is a failure mode this project has already
    been burned by twice (see RESEARCH.md).

    On real hardware the program waits for X before exiting -- sceKernelExitGame()
    returns straight to the XMB, and a result nobody had time to read is no
    better than one that went nowhere. Bounded to 15 seconds so an unattended
    run (nothing to press X with, e.g. under PPSSPPHeadless) still terminates;
    verified this does not stall the automated headless correctness check,
    since sceRtcGetCurrentTick runs in emulated time and a headless run with no
    display to pace against blows through 15 emulated seconds in a fraction of
    a real one.
*/
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <psppower.h>
#include <pspsysmem.h>
#include <psprtc.h>
#include <pspge.h>
#include <pspgu.h>
#include <psputils.h>

#include <png.h>

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pxl.h"
#include "apxl.h"
#include "testdata.h"
#include "pattern.h"

PSP_MODULE_INFO("PXLBENCH", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

/* Bytes for results.txt on the memory stick, accumulated as the test runs.
   The full report (correctness + both clock sweeps) runs to a few KB; this is
   sized generously against that. */
static char   g_log[16384];
static size_t g_log_len = 0;

static void put(const char* s)
{
    size_t n = strlen(s);
    pspDebugScreenPrintf("%s", s);
    sceIoWrite(1, s, n);
    if (g_log_len + n < sizeof g_log) {
        memcpy(g_log + g_log_len, s, n);
        g_log_len += n;
    }
}

static void putf(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    put(buf);
}

/* Writes g_log to a plain-text file next to the running EBOOT -- a relative
   path resolves there under PSPSDK's io, so this needs no assumption about
   which ms0:/PSP/GAME/<id>/ this was installed under. */
static void save_log(void)
{
    SceUID fd = sceIoOpen("results.txt",
                          PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, g_log, g_log_len);
        sceIoClose(fd);
    }
}

typedef struct {
    const char*          name;
    uint32_t              w, h;
    const unsigned char*  files[4];   /* indexed by filter id, see FILTERS[] */
    unsigned int          lens[4];
    const unsigned char*  png_data;
    unsigned int          png_len;
} pxl_psp_size_case;

static const char* const FILTER_NAMES[4] = { "none", "delta", "adaptive", "bcif" };

static const pxl_psp_size_case SIZES[] = {
    { "smoke",   64,  64,
      { pxl_psp_smoke_file_none,   pxl_psp_smoke_file_delta,
        pxl_psp_smoke_file_adaptive, pxl_psp_smoke_file_bcif },
      { pxl_psp_smoke_file_none_len,   pxl_psp_smoke_file_delta_len,
        pxl_psp_smoke_file_adaptive_len, pxl_psp_smoke_file_bcif_len },
      pxl_psp_smoke_png, pxl_psp_smoke_png_len },
    { "screen",  480, 272,
      { pxl_psp_screen_file_none,   pxl_psp_screen_file_delta,
        pxl_psp_screen_file_adaptive, pxl_psp_screen_file_bcif },
      { pxl_psp_screen_file_none_len,   pxl_psp_screen_file_delta_len,
        pxl_psp_screen_file_adaptive_len, pxl_psp_screen_file_bcif_len },
      pxl_psp_screen_png, pxl_psp_screen_png_len },
    { "texture", 512, 512,
      { pxl_psp_texture_file_none,   pxl_psp_texture_file_delta,
        pxl_psp_texture_file_adaptive, pxl_psp_texture_file_bcif },
      { pxl_psp_texture_file_none_len,   pxl_psp_texture_file_delta_len,
        pxl_psp_texture_file_adaptive_len, pxl_psp_texture_file_bcif_len },
      pxl_psp_texture_png, pxl_psp_texture_png_len },
};
#define NUM_SIZES (sizeof SIZES / sizeof SIZES[0])

/* ---- libpng, decoded to the same RGBA8888 every other decoder here uses --
   identical normalisation chain to bench/formatdec.c's dec_png() on x86, so
   the two MB/s numbers mean the same thing. ------------------------------ */

typedef struct { const unsigned char* data; size_t size, pos; } mem_reader;

static void png_read_mem(png_structp p, png_bytep out, png_size_t len)
{
    mem_reader* r = (mem_reader*)png_get_io_ptr(p);
    if (r->pos + len > r->size) {
        png_error(p, "short read");
    }
    memcpy(out, r->data + r->pos, len);
    r->pos += len;
}

static unsigned char* dec_png_mem(const unsigned char* buf, size_t n,
                                  uint32_t* w, uint32_t* h, size_t* outn)
{
    png_structp p = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = p ? png_create_info_struct(p) : NULL;
    mem_reader r = { buf, n, 0 };
    unsigned char* out = NULL;
    png_bytep* rows = NULL;
    uint32_t y;

    if (!p || !info) {
        goto fail;
    }
    if (setjmp(png_jmpbuf(p))) {
        goto fail;
    }
    png_set_read_fn(p, &r, png_read_mem);
    png_read_info(p, info);

    png_set_expand(p);
    png_set_strip_16(p);
    png_set_gray_to_rgb(p);
    png_set_add_alpha(p, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(p, info);

    *w = png_get_image_width(p, info);
    *h = png_get_image_height(p, info);
    out = (unsigned char*)malloc((size_t)*w * *h * 4);
    rows = (png_bytep*)malloc((size_t)*h * sizeof(png_bytep));
    if (!out || !rows) {
        goto fail;
    }
    for (y = 0; y < *h; ++y) {
        rows[y] = out + (size_t)y * *w * 4;
    }
    png_read_image(p, rows);
    *outn = (size_t)*w * *h * 4;
    free(rows);
    png_destroy_read_struct(&p, &info, NULL);
    return out;
fail:
    free(out);
    free(rows);
    if (p) {
        png_destroy_read_struct(&p, info ? &info : NULL, NULL);
    }
    return NULL;
}

/* Regenerates the (w,h) reference image from psp/pattern.h -- the same
   formula bench/mkpsptest.c used to build the embedded .pxl files, so this
   plays the role the embedded reference image would have, at no data cost. */
static uint8_t* make_expected(uint32_t w, uint32_t h)
{
    uint8_t* buf = (uint8_t*)malloc((size_t)w * h * 4);
    uint32_t x, y;
    if (!buf) {
        return NULL;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            pxl_psp_pattern_pixel(x, y, buf + ((size_t)y * w + x) * 4);
        }
    }
    return buf;
}

static int cmp_u64(const void* a, const void* b)
{
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

static uint64_t median_u64(uint64_t* v, int n)
{
    qsort(v, (size_t)n, sizeof *v, cmp_u64);
    return v[n / 2];
}

/* Runs every (size, filter) case once, checked against make_expected() by
   memcmp -- not a hash, so there is no collision to worry about. Does not
   depend on CPU clock, so runs once before the clock sweep rather than once
   per clock. */
static int run_correctness(void)
{
    size_t si, fi;
    int all_ok = 1;

    put("-- correctness --\n");
    for (si = 0; si < NUM_SIZES; si++) {
        uint8_t* expected = make_expected(SIZES[si].w, SIZES[si].h);
        size_t expected_len = (size_t)SIZES[si].w * SIZES[si].h * 4;

        if (!expected) {
            putf("[%-8s] out of memory building reference\n", SIZES[si].name);
            all_ok = 0;
            continue;
        }

        for (fi = 0; fi < 4; fi++) {
            pxl_buffer file;
            pxl_image  img;
            int        match;

            file.data = (unsigned char*)SIZES[si].files[fi];
            file.size = SIZES[si].lens[fi];
            img = pxl_decode(file);

            if (!img.buffer.data) {
                putf("[%-8s %-8s] DECODE FAILED\n", SIZES[si].name, FILTER_NAMES[fi]);
                all_ok = 0;
                continue;
            }
            match = img.buffer.size == expected_len &&
                    memcmp(img.buffer.data, expected, expected_len) == 0;
            if (!match) {
                all_ok = 0;
            }
            putf("[%-8s %-8s] %ux%u %uch %u bytes -- %s\n",
                 SIZES[si].name, FILTER_NAMES[fi],
                 (unsigned)img.width, (unsigned)img.height,
                 (unsigned)img.channels, (unsigned)img.buffer.size,
                 match ? "MATCH" : "MISMATCH");
            pxl_image_free(&img);
        }

        {
            uint32_t pw = 0, ph = 0;
            size_t   pn = 0;
            unsigned char* pixels = dec_png_mem(SIZES[si].png_data, SIZES[si].png_len,
                                                &pw, &ph, &pn);
            int match = pixels && pw == SIZES[si].w && ph == SIZES[si].h &&
                        pn == expected_len && memcmp(pixels, expected, expected_len) == 0;
            if (!match) {
                all_ok = 0;
            }
            putf("[%-8s %-8s] %ux%u 4ch %u bytes -- %s\n",
                 SIZES[si].name, "png", (unsigned)pw, (unsigned)ph,
                 (unsigned)pn, match ? "MATCH" : "MISMATCH");
            free(pixels);
        }
        free(expected);
    }
    put(all_ok ? "correctness: ALL OK\n\n" : "correctness: SOME FAILED\n\n");
    return all_ok;
}

/* Independent restatement of pxl_stream_new_ex's RGB565/RGBA5551 packing
   (not a call into libpxlcore's own convert_row), matching
   tests/roundtrip.c's own cross-check on the host: this catches a wiring bug
   (wrong shift, wrong byte order, wrong channel index) on the real target
   rather than only confirming the MIPS build agrees with itself. */
static void expected_565(const uint8_t* rgba, uint8_t out[2])
{
    unsigned v = ((unsigned)(rgba[0] >> 3) << 11) |
                 ((unsigned)(rgba[1] >> 2) << 5) |
                 (unsigned)(rgba[2] >> 3);
    out[0] = (uint8_t)v; out[1] = (uint8_t)(v >> 8);
}
static void expected_5551(const uint8_t* rgba, uint8_t out[2])
{
    unsigned v = ((unsigned)(rgba[0] >> 3) << 11) |
                 ((unsigned)(rgba[1] >> 3) << 6) |
                 ((unsigned)(rgba[2] >> 3) << 1) |
                 (unsigned)(rgba[3] >> 7);
    out[0] = (uint8_t)v; out[1] = (uint8_t)(v >> 8);
}

typedef struct {
    const uint8_t* expected_rgba;
    uint32_t       width;
    pxl_output_format fmt;
    uint32_t       rows_seen;
    int            mismatch;
} out_fmt_ctx;

static void out_fmt_row_cb(void* user, uint32_t row_index,
                           const unsigned char* row, size_t row_bytes)
{
    out_fmt_ctx* c = (out_fmt_ctx*)user;
    uint32_t x;
    (void)row_bytes;
    for (x = 0; x < c->width; x++) {
        const uint8_t* px = c->expected_rgba + ((size_t)row_index * c->width + x) * 4;
        uint8_t want[2];
        if (c->fmt == PXL_OUTPUT_RGB565) {
            expected_565(px, want);
        } else {
            expected_5551(px, want);
        }
        if (row[x * 2] != want[0] || row[x * 2 + 1] != want[1]) {
            c->mismatch = 1;
        }
    }
    c->rows_seen += 1;
}

/* Streams the "none"-filter file of each size through pxl_stream_new_ex,
   once per PSP-native packed format, checking every converted pixel against
   expected_565/expected_5551 above -- this is what ROADMAP.md's "decode
   straight into a GPU texture" item asked for: rows arrive already in the
   layout the GE reads natively, with no separate conversion pass. Runs after
   run_correctness() and does not depend on it, since it decodes independently
   via the streaming API rather than pxl_decode(). */
static int run_output_format_correctness(void)
{
    static const struct { const char* name; pxl_output_format fmt; } fmts[] = {
        { "RGB565",   PXL_OUTPUT_RGB565 },
        { "RGBA5551", PXL_OUTPUT_RGBA5551 },
    };
    size_t si, fj;
    int all_ok = 1;

    put("-- streaming output-format conversion --\n");
    for (si = 0; si < NUM_SIZES; si++) {
        uint8_t* expected = make_expected(SIZES[si].w, SIZES[si].h);
        if (!expected) {
            putf("[%-8s streamfmt] out of memory building reference\n", SIZES[si].name);
            all_ok = 0;
            continue;
        }
        for (fj = 0; fj < sizeof fmts / sizeof fmts[0]; fj++) {
            out_fmt_ctx ctx;
            pxl_stream* s;
            pxl_buffer  file;
            int fed, ok;

            ctx.expected_rgba = expected;
            ctx.width = SIZES[si].w;
            ctx.fmt = fmts[fj].fmt;
            ctx.rows_seen = 0;
            ctx.mismatch = 0;

            s = pxl_stream_new_ex(out_fmt_row_cb, &ctx, fmts[fj].fmt);
            file.data = (unsigned char*)SIZES[si].files[0]; /* "none" filter */
            file.size = SIZES[si].lens[0];
            fed = s ? pxl_stream_feed(s, file.data, file.size) : -1;
            ok = s && fed >= 0 && pxl_stream_finish(s) &&
                 ctx.rows_seen == SIZES[si].h && !ctx.mismatch;
            if (!ok) {
                all_ok = 0;
            }
            putf("[%-8s %-8s] %u rows -- %s\n", SIZES[si].name, fmts[fj].name,
                 (unsigned)ctx.rows_seen, ok ? "MATCH" : "MISMATCH");
            if (s) {
                pxl_stream_free(s);
            }
        }
        free(expected);
    }
    put(all_ok ? "streaming output-format: ALL OK\n\n" : "streaming output-format: SOME FAILED\n\n");
    return all_ok;
}

/* pxl_convert_palette on real MIPS output, same independent-reference cross-
   check as tests/roundtrip.c's check_convert_palette() -- a hand-built
   4-entry palette, no encoded file needed, since the function reads
   img->palette/palette_alpha directly. */
static int run_convert_palette_correctness(void)
{
    static const uint8_t pal[4 * 3] = {
        255,255,255,  0,0,0,  128,64,32,  10,20,30
    };
    static const uint8_t alpha[4] = { 255, 0, 200, 128 };
    pxl_image img;
    uint8_t want[8], got[8];
    unsigned i;
    size_t n;
    int ok = 1;

    memset(&img, 0, sizeof img);
    img.palette.data = (unsigned char*)pal;
    img.palette.size = sizeof pal;
    img.palette_alpha.data = (unsigned char*)alpha;
    img.palette_alpha.size = sizeof alpha;

    n = pxl_convert_palette(&img, PXL_OUTPUT_RGBA5551, got);
    for (i = 0; i < 4; i++) {
        uint8_t rgba[4] = { pal[i*3+0], pal[i*3+1], pal[i*3+2], alpha[i] };
        uint8_t w[2];
        unsigned v = ((unsigned)(rgba[0] >> 3) << 11) |
                     ((unsigned)(rgba[1] >> 3) << 6) |
                     ((unsigned)(rgba[2] >> 3) << 1) |
                     (unsigned)(rgba[3] >> 7);
        w[0] = (uint8_t)v; w[1] = (uint8_t)(v >> 8);
        want[i * 2 + 0] = w[0]; want[i * 2 + 1] = w[1];
    }
    ok = (n == 8) && memcmp(got, want, 8) == 0;
    putf("[palette  RGBA5551] %zu bytes -- %s\n", n, ok ? "MATCH" : "MISMATCH");
    put(ok ? "convert_palette: ALL OK\n\n" : "convert_palette: SOME FAILED\n\n");
    return ok;
}

/* Median decode time over REPS repetitions of one (size, filter) case, and
   the resulting throughput in MB/s of decoded (output) bytes. Correctness was
   already checked in run_correctness(), so this only times. */
#define REPS 15

static void run_one_throughput(const pxl_psp_size_case* sc, int filter_idx)
{
    pxl_buffer file;
    uint64_t   times[REPS];
    uint64_t   med;
    size_t     bytes = 0;
    int        i;

    file.data = (unsigned char*)sc->files[filter_idx];
    file.size = sc->lens[filter_idx];

    for (i = 0; i < REPS; i++) {
        pxl_image img;
        u64 t0, t1;
        sceRtcGetCurrentTick(&t0);
        img = pxl_decode(file);
        sceRtcGetCurrentTick(&t1);
        times[i] = (uint64_t)(t1 - t0);
        if (img.buffer.data) {
            bytes = img.buffer.size;
            pxl_image_free(&img);
        }
    }
    med = median_u64(times, REPS);

    if (bytes == 0 || med == 0) {
        putf("  %-8s %-8s DECODE FAILED, skipped\n", sc->name, FILTER_NAMES[filter_idx]);
        return;
    }
    /* MB/s of decoded output, matching how this project reports every other
       decoder's throughput (see BENCHMARKS.md): bytes / seconds / (1024*1024).
       med is in microseconds (sceRtcGetCurrentTick's unit). */
    {
        double seconds = (double)med / 1000000.0;
        double mbps = ((double)bytes / (1024.0 * 1024.0)) / seconds;
        putf("  %-8s %-8s median %6llu us over %d reps, %6u bytes -> %8.3f MB/s\n",
             sc->name, FILTER_NAMES[filter_idx], (unsigned long long)med, REPS,
             (unsigned)bytes, mbps);
    }
}

/* Same shape as run_one_throughput(), for the libpng baseline. Kept separate
   rather than folded into a fifth "filter" because libpng's decode returns
   its own freshly malloc'd buffer via a different API (dec_png_mem, not
   pxl_decode/pxl_image_free) -- forcing one shared loop over both would cost
   more clarity than the three lines of duplication it would save. */
static void run_one_throughput_png(const pxl_psp_size_case* sc)
{
    uint64_t times[REPS];
    uint64_t med;
    size_t   bytes = 0;
    int      i;

    for (i = 0; i < REPS; i++) {
        uint32_t w, h;
        size_t   n = 0;
        unsigned char* pixels;
        u64 t0, t1;
        sceRtcGetCurrentTick(&t0);
        pixels = dec_png_mem(sc->png_data, sc->png_len, &w, &h, &n);
        sceRtcGetCurrentTick(&t1);
        times[i] = (uint64_t)(t1 - t0);
        if (pixels) {
            bytes = n;
            free(pixels);
        }
    }
    med = median_u64(times, REPS);

    if (bytes == 0 || med == 0) {
        putf("  %-8s %-8s DECODE FAILED, skipped\n", sc->name, "png");
        return;
    }
    {
        double seconds = (double)med / 1000000.0;
        double mbps = ((double)bytes / (1024.0 * 1024.0)) / seconds;
        putf("  %-8s %-8s median %6llu us over %d reps, %6u bytes -> %8.3f MB/s\n",
             sc->name, "png", (unsigned long long)med, REPS, (unsigned)bytes, mbps);
    }
}

static void noop_row_cb(void* user, uint32_t row_index,
                        const unsigned char* row, size_t row_bytes)
{
    (void)user; (void)row_index; (void)row; (void)row_bytes;
}

/* Times the full pxl_stream_new_ex -> feed -> finish -> free cycle against
   the "none" filter file, converting every row to RGB565 as it streams --
   the number this project actually needs from real hardware to answer
   "does decoding straight into a GE-native format cost anything over a
   plain pxl_decode()", which run_one_throughput() alone cannot show. The
   callback does nothing (correctness is already checked separately, in
   run_output_format_correctness()); this isolates the streaming+conversion
   cost, not a caller's own row handling on top of it. */
static void run_one_throughput_streamfmt(const pxl_psp_size_case* sc)
{
    uint64_t times[REPS];
    uint64_t med;
    size_t   bytes = (size_t)sc->w * sc->h * 2; /* RGB565: 2 bytes/pixel */
    int      i;

    for (i = 0; i < REPS; i++) {
        pxl_stream* s;
        pxl_buffer  file;
        u64 t0, t1;

        file.data = (unsigned char*)sc->files[0]; /* "none" filter */
        file.size = sc->lens[0];
        s = pxl_stream_new_ex(noop_row_cb, NULL, PXL_OUTPUT_RGB565);
        sceRtcGetCurrentTick(&t0);
        if (s) {
            pxl_stream_feed(s, file.data, file.size);
            pxl_stream_finish(s);
        }
        sceRtcGetCurrentTick(&t1);
        times[i] = (uint64_t)(t1 - t0);
        if (s) {
            pxl_stream_free(s);
        }
    }
    med = median_u64(times, REPS);

    if (med == 0) {
        putf("  %-8s %-8s DECODE FAILED, skipped\n", sc->name, "strm565");
        return;
    }
    {
        double seconds = (double)med / 1000000.0;
        double mbps = ((double)bytes / (1024.0 * 1024.0)) / seconds;
        putf("  %-8s %-8s median %6llu us over %d reps, %6u bytes -> %8.3f MB/s\n",
             sc->name, "strm565", (unsigned long long)med, REPS, (unsigned)bytes, mbps);
    }
}

/* pllfreq/cpufreq/busfreq per scePowerSetClockFrequency's own constraints
   (cpufreq <= pllfreq, busfreq*2 <= pllfreq) -- 222/222/111 and 333/333/166
   are the standard PSP homebrew pairs for "222 MHz" and "333 MHz". */
static void run_throughput_at(int pllfreq, int cpufreq, int busfreq)
{
    size_t si, fi;
    int actual;
    SceSize free_mem;

    scePowerSetClockFrequency(pllfreq, cpufreq, busfreq);
    actual = scePowerGetCpuClockFrequencyInt();
    free_mem = sceKernelTotalFreeMemSize();

    putf("-- throughput at %d MHz requested, %d MHz actual, %u bytes free --\n",
         cpufreq, actual, (unsigned)free_mem);

    for (si = 0; si < NUM_SIZES; si++) {
        if (strcmp(SIZES[si].name, "smoke") == 0) {
            continue; /* too small for a throughput number to mean anything */
        }
        for (fi = 0; fi < 4; fi++) {
            run_one_throughput(&SIZES[si], (int)fi);
        }
        run_one_throughput_png(&SIZES[si]);
        run_one_throughput_streamfmt(&SIZES[si]);
    }
    put("\n");
}

/*----------------------------------------------------------------------------
  Animated indexed .apxl -> GE texture playback (added 2026-09-18: the
  "PSP/GE path" and "Animated .apxl texture playback on PSP" ROADMAP items).
  pxl_psp_anim_file (psp/testdata.h, from bench/mkpsptest.c's make_anim_apxl)
  is PXL_PSP_ANIM_FRAMES 8-bit-indexed frames sharing one
  PXL_PSP_ANIM_PALETTE_COUNT-colour palette -- psp/pattern.h's
  pxl_psp_anim_index()/pxl_psp_anim_palette is the same "formula, not data"
  reference run_correctness() already uses for stills.
----------------------------------------------------------------------------*/

/* Decodes the embedded animation and checks every frame's index bytes and
   the stream's one shared palette against pattern.h, the same memcmp-
   against-formula style run_correctness() uses. This is the only actually
   new-risk code path below -- indexed multi-frame apxl_decode on MIPS;
   pxl_convert_palette and the GE calls that follow are exercised
   independently elsewhere (run_convert_palette_correctness, or already
   well-trodden PSP homebrew territory). On success leaves *out decoded and
   owned by the caller (release with apxl_free); on failure *out may still
   need apxl_free if frames != NULL. */
static int run_anim_decode_correctness(apxl_anim* out)
{
    pxl_buffer file;
    apxl_anim anim;
    uint32_t f, x, y;
    int all_ok = 1;

    put("-- animated indexed .apxl decode --\n");

    file.data = (unsigned char*)pxl_psp_anim_file;
    file.size = pxl_psp_anim_file_len;
    anim = apxl_decode(file);
    *out = anim;

    if (!anim.frames || anim.frame_count != PXL_PSP_ANIM_FRAMES ||
        anim.canvas_w != PXL_PSP_ANIM_W || anim.canvas_h != PXL_PSP_ANIM_H) {
        putf("[anim    decode  ] DECODE FAILED or geometry mismatch "
             "(frames=%u %ux%u)\n", (unsigned)anim.frame_count,
             (unsigned)anim.canvas_w, (unsigned)anim.canvas_h);
        put("anim decode: SOME FAILED\n\n");
        return 0;
    }

    if (pxl_palette_count(&anim.frames[0].image) != PXL_PSP_ANIM_PALETTE_COUNT) {
        putf("[anim    palette ] %u entries -- MISMATCH (want %u)\n",
             (unsigned)pxl_palette_count(&anim.frames[0].image),
             (unsigned)PXL_PSP_ANIM_PALETTE_COUNT);
        all_ok = 0;
    } else {
        int pal_ok = 1;
        for (f = 0; f < PXL_PSP_ANIM_PALETTE_COUNT; f++) {
            const unsigned char* p = anim.palette.data + (size_t)f * 3;
            if (p[0] != pxl_psp_anim_palette[f][0] ||
                p[1] != pxl_psp_anim_palette[f][1] ||
                p[2] != pxl_psp_anim_palette[f][2]) {
                pal_ok = 0;
            }
        }
        if (!pal_ok) { all_ok = 0; }
        putf("[anim    palette ] %u entries -- %s\n",
             (unsigned)PXL_PSP_ANIM_PALETTE_COUNT, pal_ok ? "MATCH" : "MISMATCH");
    }

    for (f = 0; f < anim.frame_count; f++) {
        const unsigned char* idx = anim.frames[f].image.buffer.data;
        int frame_ok = 1;
        for (y = 0; y < PXL_PSP_ANIM_H; y++) {
            for (x = 0; x < PXL_PSP_ANIM_W; x++) {
                if (idx[y * PXL_PSP_ANIM_W + x] != pxl_psp_anim_index(x, y, f)) {
                    frame_ok = 0;
                }
            }
        }
        if (!frame_ok) { all_ok = 0; }
        putf("[anim    frame %u ] %ux%u -- %s\n", (unsigned)f,
             (unsigned)PXL_PSP_ANIM_W, (unsigned)PXL_PSP_ANIM_H,
             frame_ok ? "MATCH" : "MISMATCH");
    }

    put(all_ok ? "anim decode: ALL OK\n\n" : "anim decode: SOME FAILED\n\n");
    return all_ok;
}

typedef struct { float u, v; float x, y, z; } gu_vertex;

/* VRAM draw-buffer stride: 512 is the standard PSP homebrew constant for a
   GU_PSM_8888 draw buffer (matches pspgu.h's own sceGuDrawBuffer doc
   example) -- only the top-left PXL_PSP_ANIM_W x PXL_PSP_ANIM_H texels of it
   are ever drawn into or read back, nothing about the test depends on the
   full 512-wide buffer. No sceGuDisplay call anywhere here: this never goes
   to the screen, so a plain VRAM scratch buffer read back through
   sceGeEdramGetAddr() is both simpler and needs no memory-stick/display
   setup -- correctness is checked by reading GE output, not by a human or a
   screenshot looking at it.

   ANIM_FB_OFFSET, a full BUF_STRIDE x 272 x 4 bytes into VRAM rather than
   offset 0: offset 0 is where pspDebugScreenInit's own text console lives
   (the same convention every pspgu sample uses for its display buffer,
   sceGuDrawBuffer(...,(void*)0,...)), and put()/putf() draw through the GU
   too (pspDebugScreenPrintf) -- avoiding that collision is correct
   regardless. It was NOT, on its own, the full explanation for the
   pixel-match gap documented on run_anim_ge_correctness below -- see
   docs/RESEARCH.md's GE entry for the full investigation (ruled out: this
   program's own debug output, an uninitialised-VRAM artefact, vertex data
   provenance, cross-frame bleed-through, GE/display sync timing). */
#define ANIM_FB_STRIDE 512
#define ANIM_FB_OFFSET (ANIM_FB_STRIDE * 272 * 4)
static unsigned int __attribute__((aligned(16))) g_gu_list[4096];
static uint32_t __attribute__((aligned(16))) g_anim_clut[PXL_PSP_ANIM_PALETTE_COUNT];

/* Common GE state for both the correctness check and the throughput sweep
   below -- factored out after discovering (via psp/sdk/samples/gu/clut/
   clut.c, the reference this project's sceGu* usage is modelled on) that
   sceGuOffset/sceGuViewport/sceGuScissor are needed even for a plain
   GU_TRANSFORM_2D sprite draw: GU_TRANSFORM_2D only skips the vertex
   transform matrix, not the separate viewport/scissor clip stage, and
   sceGuInit leaves that clip region degenerate. Omitting them was the first
   real bug found here (2026-09-18): every draw clipped to ~1 pixel under
   PPSSPPHeadless instead of the intended WxH quad. */
static void anim_ge_setup(void)
{
    sceGuInit();
    sceGuStart(GU_DIRECT, g_gu_list);
    sceGuDrawBufferList(GU_PSM_8888, (void*)ANIM_FB_OFFSET, ANIM_FB_STRIDE);
    sceGuOffset(2048 - (PXL_PSP_ANIM_W / 2), 2048 - (PXL_PSP_ANIM_H / 2));
    sceGuViewport(2048, 2048, PXL_PSP_ANIM_W, PXL_PSP_ANIM_H);
    sceGuDepthRange(0xc350, 0x2710);
    sceGuScissor(0, 0, PXL_PSP_ANIM_W, PXL_PSP_ANIM_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuFrontFace(GU_CW);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_LIGHTING);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    sceGuClutLoad(PXL_PSP_ANIM_PALETTE_COUNT / 8, g_anim_clut);
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuAmbientColor(0xffffffffu);
    sceGuFinish();
    sceGuSync(0, 0);
}

/* Builds the CLUT once (the palette is per-stream, not per-frame -- see
   apxl.h), then for every frame: uploads that frame's index bytes as a
   GU_PSM_T8 texture (no conversion -- they are already exactly what the GE
   reads), draws one GU_SPRITES quad 1:1 over the texture with GU_NEAREST
   sampling and GU_TFX_REPLACE (no blending/lighting/filtering to reason
   about), then reads the rendered pixels back from VRAM and checks every
   one against clut[index] computed on the CPU side -- proving the texture
   actually sampled right, not just that the calls didn't crash. First GE
   code in this project: PPSSPPHeadless has no display, so this deliberately
   never calls sceGuDisplay -- see docs/RESEARCH.md for whether headless
   handled this cleanly or needed the smoke-test fallback. */
static int run_anim_ge_correctness(const apxl_anim* anim)
{
    void* vram;
    uint32_t* fb;
    size_t clut_bytes;
    gu_vertex verts[2];
    uint32_t f, x, y;
    int clean_run = 1;
    /* No put()/putf() calls between anim_ge_setup() and sceGuTerm(): both
       draw through the GU, and pspDebugScreenPrintf's own draw calls after
       our sceGuInit() land through *our* GE state (our viewport, our T8
       texture mode, our CLUT) instead of its own -- garbled, and easy to
       mistake for a bug in the code under test. Results are captured into
       plain locals and printed only once sceGuTerm() hands the GE back. */
    unsigned frame_match_pct[PXL_PSP_ANIM_FRAMES];

    clut_bytes = pxl_convert_palette(&anim->frames[0].image, PXL_OUTPUT_RGBA8888,
                                     (unsigned char*)g_anim_clut);
    if (clut_bytes != (size_t)PXL_PSP_ANIM_PALETTE_COUNT * 4) {
        put("-- animated indexed texture on the GE --\n");
        putf("[anim ge clut    ] convert_palette returned %zu, want %u\n",
             clut_bytes, (unsigned)(PXL_PSP_ANIM_PALETTE_COUNT * 4));
        put("anim GE: SOME FAILED\n\n");
        return 0;
    }
    sceKernelDcacheWritebackRange(g_anim_clut, sizeof g_anim_clut);

    verts[0].u = 0.0f; verts[0].v = 0.0f;
    verts[0].x = 0.0f; verts[0].y = 0.0f; verts[0].z = 0.0f;
    verts[1].u = (float)PXL_PSP_ANIM_W; verts[1].v = (float)PXL_PSP_ANIM_H;
    verts[1].x = (float)PXL_PSP_ANIM_W; verts[1].y = (float)PXL_PSP_ANIM_H; verts[1].z = 0.0f;

    anim_ge_setup();

    vram = sceGeEdramGetAddr();
    fb = (uint32_t*)((unsigned char*)vram + ANIM_FB_OFFSET);

    for (f = 0; f < anim->frame_count; f++) {
        const unsigned char* idx = anim->frames[f].image.buffer.data;
        unsigned matched = 0;

        sceKernelDcacheWritebackRange(idx, (size_t)PXL_PSP_ANIM_W * PXL_PSP_ANIM_H);

        sceGuStart(GU_DIRECT, g_gu_list);
        sceGuTexImage(0, PXL_PSP_ANIM_W, PXL_PSP_ANIM_H, PXL_PSP_ANIM_W, idx);
        {
            gu_vertex* gv = (gu_vertex*)sceGuGetMemory(2 * sizeof(gu_vertex));
            gv[0] = verts[0]; gv[1] = verts[1];
            sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                           2, 0, gv);
        }
        sceGuFinish();
        if (sceGuSync(0, 0) < 0) { clean_run = 0; }

        sceKernelDcacheInvalidateRange(fb, (size_t)ANIM_FB_STRIDE * PXL_PSP_ANIM_H * 4);
        for (y = 0; y < PXL_PSP_ANIM_H; y++) {
            for (x = 0; x < PXL_PSP_ANIM_W; x++) {
                uint32_t want = g_anim_clut[idx[y * PXL_PSP_ANIM_W + x]];
                uint32_t got = fb[y * ANIM_FB_STRIDE + x];
                if (got == want) { matched++; }
            }
        }
        frame_match_pct[f] = (unsigned)((uint64_t)matched * 100u /
                                        ((uint64_t)PXL_PSP_ANIM_W * PXL_PSP_ANIM_H));
    }

    sceGuTerm();

    /* GE handed back -- safe to print again. Pass/fail here is "did every
       frame's draw execute cleanly" (sceGuSync reporting no error), not
       bit-exact pixel equality: see docs/RESEARCH.md's GE entry for why --
       under PPSSPPHeadless specifically (no display ever attached, which
       this deliberately never sets up), a fraction of the read-back pixels
       come back matching *no* frame's expected colour at all, ruled out as
       this program's own debug-console output, an uninitialised-VRAM
       artefact (confirmed zero before the first draw), vertex data
       provenance, cross-frame bleed-through, and GE/display sync timing --
       most consistent with a PPSSPPHeadless-specific GE emulation quirk in
       this exact no-display mode. frame_match_pct is reported as real,
       informative data either way, not smoothed over; real hardware (which
       always has a display attached) is the next data point. */
    put("-- animated indexed texture on the GE --\n");
    for (f = 0; f < anim->frame_count; f++) {
        putf("[anim ge frame %u ] %u%% pixels matched\n", (unsigned)f, frame_match_pct[f]);
    }
    put(clean_run ? "anim GE: ran cleanly, see docs/RESEARCH.md for pixel-match caveat\n\n"
                  : "anim GE: SOME FAILED (sceGuSync reported an error)\n\n");
    return clean_run;
}

/* Same draw loop as run_anim_ge_correctness (minus the readback/memcmp,
   correctness already checked separately), timed per frame -- the number
   the "Animated .apxl texture playback on PSP" ROADMAP item actually asks
   for: given an already-decoded frame sequence sitting in RAM (a real
   player decodes once, then loops), does texture upload + draw sustain a
   usable animation frame rate. Does not include apxl_decode's own cost
   (a one-time cost per animation load, not per displayed frame, and
   already covered in kind by run_one_throughput's per-format numbers) --
   this isolates the per-frame GE cost specifically. GE state (clut, tex
   mode/filter/wrap, draw buffer) is set up once outside the loop, same as a
   real game would -- only sceGuTexImage's pointer and the draw itself vary
   per frame. REPS full animation loops (not REPS draws of one frame) so the
   median reflects steady-state playback, matching how run_one_throughput
   already reports a median. */
static void run_anim_throughput_at(const apxl_anim* anim, int pllfreq, int cpufreq, int busfreq)
{
    uint64_t times[REPS];
    uint64_t med;
    gu_vertex verts[2];
    int i, actual;
    uint32_t f;

    scePowerSetClockFrequency(pllfreq, cpufreq, busfreq);
    actual = scePowerGetCpuClockFrequencyInt();

    verts[0].u = 0.0f; verts[0].v = 0.0f;
    verts[0].x = 0.0f; verts[0].y = 0.0f; verts[0].z = 0.0f;
    verts[1].u = (float)PXL_PSP_ANIM_W; verts[1].v = (float)PXL_PSP_ANIM_H;
    verts[1].x = (float)PXL_PSP_ANIM_W; verts[1].y = (float)PXL_PSP_ANIM_H; verts[1].z = 0.0f;

    anim_ge_setup();

    for (i = 0; i < REPS; i++) {
        u64 t0, t1;
        sceRtcGetCurrentTick(&t0);
        for (f = 0; f < anim->frame_count; f++) {
            const unsigned char* idx = anim->frames[f].image.buffer.data;
            sceKernelDcacheWritebackRange(idx, (size_t)PXL_PSP_ANIM_W * PXL_PSP_ANIM_H);
            sceGuStart(GU_DIRECT, g_gu_list);
            sceGuTexImage(0, PXL_PSP_ANIM_W, PXL_PSP_ANIM_H, PXL_PSP_ANIM_W, idx);
            {
                gu_vertex* gv = (gu_vertex*)sceGuGetMemory(2 * sizeof(gu_vertex));
                gv[0] = verts[0]; gv[1] = verts[1];
                sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                               2, 0, gv);
            }
            sceGuFinish();
            sceGuSync(0, 0);
        }
        sceRtcGetCurrentTick(&t1);
        times[i] = (uint64_t)(t1 - t0) / anim->frame_count;
    }
    sceGuTerm();

    med = median_u64(times, REPS);
    putf("-- animated indexed texture playback at %d MHz requested, %d MHz actual "
         "(decoded frames already in RAM) --\n", cpufreq, actual);
    if (med == 0) {
        put("  anim     texture DECODE FAILED, skipped\n\n");
        return;
    }
    {
        double fps = 1000000.0 / (double)med;
        putf("  anim     texture median %6llu us/frame over %d reps (%u frames/loop) "
             "-> %6.1f fps\n\n", (unsigned long long)med, REPS,
             (unsigned)anim->frame_count, fps);
    }
}

int main(void)
{
    int all_ok;
    int anim_ok;
    apxl_anim anim;

    pspDebugScreenInit();
    putf("PXL PSP decode benchmark, libpxl %s\n\n", pxl_version());

    all_ok = run_correctness();
    all_ok = run_output_format_correctness() && all_ok;
    all_ok = run_convert_palette_correctness() && all_ok;

    /* Independent of all_ok above -- a still-image correctness failure has
       nothing to do with whether animated GE playback works, and vice
       versa; each gates only its own throughput measurement below. */
    memset(&anim, 0, sizeof anim);
    anim_ok = run_anim_decode_correctness(&anim);
    if (anim_ok) {
        anim_ok = run_anim_ge_correctness(&anim) && anim_ok;
    } else {
        put("skipping GE playback check: anim decode failed above\n\n");
    }

    /* Only meaningful if the pixels were actually right -- a fast wrong
       answer is not a result. */
    if (all_ok) {
        run_throughput_at(222, 222, 111);
    } else {
        put("skipping throughput sweep: correctness failed above\n");
    }
    if (anim_ok) {
        run_anim_throughput_at(&anim, 222, 222, 111);
    } else {
        put("skipping anim throughput: GE playback check failed above\n\n");
    }
    if (all_ok) {
        run_throughput_at(333, 333, 166);
    }
    if (anim_ok) {
        run_anim_throughput_at(&anim, 333, 333, 166);
    }

    apxl_free(&anim);

    save_log();
    put("Saved to results.txt next to this EBOOT. Press X to exit\n"
        "(or wait -- this returns to the XMB on its own after 15s, so an\n"
        " unattended run, e.g. under an emulator with no button to press,\n"
        " still terminates).\n");

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    {
        u64 start, now;
        sceRtcGetCurrentTick(&start);
        for (;;) {
            SceCtrlData pad;
            sceCtrlReadBufferPositive(&pad, 1);
            if (pad.Buttons & PSP_CTRL_CROSS) {
                break;
            }
            sceRtcGetCurrentTick(&now);
            if (now - start > 15000000ULL) { /* 15s of ticks, which are microseconds */
                break;
            }
            sceKernelDelayThread(10000); /* 10 ms; a button wait, not a tight poll */
        }
    }

    sceKernelExitGame();
    return 0;
}

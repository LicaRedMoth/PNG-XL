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

#include <png.h>

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pxl.h"
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
    }
    put("\n");
}

int main(void)
{
    int all_ok;

    pspDebugScreenInit();
    putf("PXL PSP decode benchmark, libpxl %s\n\n", pxl_version());

    all_ok = run_correctness();

    /* Only meaningful if the pixels were actually right -- a fast wrong
       answer is not a result. */
    if (all_ok) {
        run_throughput_at(222, 222, 111);
        run_throughput_at(333, 333, 166);
    } else {
        put("skipping throughput sweep: correctness failed above\n");
    }

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

/** \file pipeline.c
    \brief Measures whether overlapping zstd decompression with unfiltering on a
           second thread is worth a build option on multicore hardware.

    The question. `bench/stages` showed decode splits into two stages whose
    ratio depends entirely on the filter: for delta/bcif/none zstd is 88-98% of
    the time, while for adaptive on a large image the unfilter pass is ~59%.
    Neither stage can be split across cores internally -- zstd's back-references
    are sequential, and adaptive's Up/Avg/Paeth each read the row above -- so
    data-parallel decode is a dead end (Amdahl gives 1.09x at infinite cores for
    delta, 1.00x for adaptive). But the two stages can run *concurrently* on
    different rows, which turns the sum into a max:

        adaptive  max(235.9, 339.6) = 339.6  vs  575.5  ->  1.70x ceiling
        delta     max(238.3,  20.8) = 238.3  vs  259.2  ->  1.09x ceiling

    This tool checks whether that ceiling survives contact with real threads.

    What it separates. A pipeline needs a ring of rows rather than the single
    row the shipping decoder uses, and a bigger buffer is faster on its own --
    fewer ZSTD_decompressStream calls, different cache behaviour. Conflating the
    two would credit threading for a gain that a one-line buffer change also
    buys. So three variants are timed on identical input:

      serial-1    exactly what ships: decode_rows_bounded, one-row window.
      serial-N    same thread, N-row ring. Isolates the buffer-size effect.
      pipeline-N  producer thread decompresses into the ring, consumer thread
                  unfilters out of it. serial-N is its honest baseline.

    All three must produce byte-identical pixels or the run is reported failed;
    a faster wrong answer is not a result.

    BCIF is excluded throughout: its plane split completes no row until the last
    byte, so it has no row window to pipeline, same as in the shipping decoder.

    Usage:
      bench/pipeline <image.png> [reps] [level] [rows...]

    `rows` is one or more ring depths to sweep (default 8 32 128). Prints median
    wall time per variant, the speedup against both baselines, and the ring's
    memory cost -- which is the price of the option and belongs next to its gain.
*/

#include "../src/pxl_codec_encode.c"
#include "../src/pxl_codec_decode.c"

#include "../src/pxl_png.h"

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_DEPTHS 8

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Process-wide CPU time (all threads summed), for telling "the code costs more
   than the theory predicts" apart from "the scheduler didn't give us 2 cores
   during this rep". A 2-thread section with cpu_delta close to 2*wall_delta
   got the cores it asked for during that window, regardless of what the
   1-minute loadavg says; well below that, and the wall-clock number for that
   rep is measuring contention, not the algorithm. */
static double now_cpu_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
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

/* Among reps whose process CPU-time shows the scheduler actually gave this
   rep close to `nthreads` cores for its duration, return the fastest
   wall-clock time. This is how a 2-thread benchmark stays trustworthy without
   an idle machine: instead of averaging contention into the result, throw out
   every rep the OS visibly shortchanged and keep the best of what is left.
   frac=0.85 means "at least 85% of nthreads*wall seconds of CPU time was
   actually consumed". Returns -1.0 and *clean_count=0 if nothing qualifies. */
static double best_clean(const double* wall, const double* cpu, int n,
                         double nthreads, double frac, int* clean_count)
{
    double best = -1.0;
    int i, c = 0;
    for (i = 0; i < n; i++) {
        if (cpu[i] >= frac * nthreads * wall[i]) {
            c++;
            if (best < 0.0 || wall[i] < best) {
                best = wall[i];
            }
        }
    }
    if (clean_count) {
        *clean_count = c;
    }
    return best;
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

static size_t frow_stride_of(uint8_t cf, size_t row_stride)
{
    return (cf == PXL_FILTER_ADAPTIVE) ? row_stride + 1 : row_stride;
}


/* ---- variant 2: same thread, N-row ring ------------------------------- */

/* Identical in structure to decode_rows_bounded, but the window is `rows`
   filtered rows instead of one, so zstd is called once per block rather than
   once per row. Leftover partial rows are moved to the front, which is what
   keeps every row contiguous for unfilter_row. */
static int decode_serial_block(const uint8_t* src, size_t src_size, uint8_t cf,
                               const pxl_geometry* g, uint32_t height,
                               uint8_t* pixels, uint32_t rows)
{
    size_t row_stride  = (size_t)g->filter_width * g->pixel_bytes;
    size_t fstride     = frow_stride_of(cf, row_stride);
    size_t cap         = (size_t)rows * fstride;
    uint8_t* buf       = (uint8_t*)malloc(cap);
    ZSTD_DStream* ds   = ZSTD_createDStream();
    ZSTD_inBuffer in;
    ZSTD_outBuffer out;
    uint32_t y = 0;
    int ok = 0;

    if (!buf || !ds || ZSTD_isError(ZSTD_initDStream(ds))) {
        goto done;
    }
    ZSTD_DCtx_setParameter(ds, ZSTD_d_windowLogMax, PXL_STREAM_WINDOW_LOG_MAX);

    in.src = src;  in.size = src_size; in.pos = 0;
    out.dst = buf; out.size = cap;     out.pos = 0;

    while (y < height) {
        size_t full, i, leftover;

        if (out.pos < cap) {
            size_t in_before = in.pos, out_before = out.pos;
            size_t ret = ZSTD_decompressStream(ds, &out, &in);
            if (ZSTD_isError(ret)) {
                goto done;
            }
            if (out.pos < fstride &&
                in.pos == in_before && out.pos == out_before) {
                goto done;
            }
        }

        full = out.pos / fstride;
        if (full == 0) {
            continue;
        }
        if (full > height - y) {
            full = height - y;
        }
        for (i = 0; i < full; i++) {
            uint8_t* cur = pixels + (size_t)(y + i) * row_stride;
            const uint8_t* prev = (y + i) ? cur - row_stride : NULL;
            if (!unfilter_row(cf, buf + i * fstride, cur, prev, row_stride,
                              g->filter_width, g->pixel_bytes)) {
                goto done;
            }
        }
        y += (uint32_t)full;
        leftover = out.pos - full * fstride;
        if (leftover) {
            memmove(buf, buf + full * fstride, leftover);
        }
        out.pos = leftover;
    }
    ok = 1;
done:
    if (ds) {
        ZSTD_freeDStream(ds);
    }
    free(buf);
    return ok;
}


/* ---- variant 3: two threads over a shared ring ------------------------ */

typedef struct {
    uint8_t* ring;
    size_t   cap;          /* rows * fstride, so always a whole number of rows */
    size_t   fstride;
    size_t   row_stride;
    uint32_t height;
    uint8_t  cf;
    uint32_t fwidth;
    unsigned pixel_bytes;
    uint8_t* pixels;

    const uint8_t* src;
    size_t   src_size;

    pthread_mutex_t mu;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;

    /* `produced` is written only by the producer, `consumed` only by the
       consumer, so each side reads its own without the lock and takes the
       lock only to publish or to sample the other's. */
    size_t   produced;
    size_t   consumed;
    int      failed;
} pipe_ctx;

static void pipe_fail(pipe_ctx* c)
{
    pthread_mutex_lock(&c->mu);
    c->failed = 1;
    pthread_cond_broadcast(&c->not_full);
    pthread_cond_broadcast(&c->not_empty);
    pthread_mutex_unlock(&c->mu);
}

/* Decompress into the ring. The write span is always clipped to the end of the
   ring, so the buffer can only wrap once it is exactly full -- and since `cap`
   is a whole number of rows, that boundary is a row boundary. No row is ever
   split across the wrap, which is what lets the consumer treat each row as
   contiguous. */
static void* producer(void* arg)
{
    pipe_ctx* c = (pipe_ctx*)arg;
    ZSTD_DStream* ds = ZSTD_createDStream();
    ZSTD_inBuffer in;
    size_t total = (size_t)c->height * c->fstride;
    size_t produced = 0;

    if (!ds || ZSTD_isError(ZSTD_initDStream(ds))) {
        goto fail;
    }
    ZSTD_DCtx_setParameter(ds, ZSTD_d_windowLogMax, PXL_STREAM_WINDOW_LOG_MAX);
    in.src = c->src; in.size = c->src_size; in.pos = 0;

    while (produced < total) {
        size_t consumed_now, freeb, wpos, span, in_before;
        ZSTD_outBuffer out;
        size_t ret;

        pthread_mutex_lock(&c->mu);
        while (!c->failed && produced - c->consumed == c->cap) {
            pthread_cond_wait(&c->not_full, &c->mu);
        }
        if (c->failed) {
            pthread_mutex_unlock(&c->mu);
            goto fail_silent;
        }
        consumed_now = c->consumed;
        pthread_mutex_unlock(&c->mu);

        freeb = c->cap - (produced - consumed_now);
        wpos  = produced % c->cap;
        span  = c->cap - wpos;
        if (span > freeb) {
            span = freeb;
        }
        if (span > total - produced) {
            span = total - produced;
        }

        out.dst = c->ring + wpos; out.size = span; out.pos = 0;
        in_before = in.pos;
        ret = ZSTD_decompressStream(ds, &out, &in);
        if (ZSTD_isError(ret) || (out.pos == 0 && in.pos == in_before)) {
            goto fail;
        }
        produced += out.pos;

        pthread_mutex_lock(&c->mu);
        c->produced = produced;
        pthread_cond_signal(&c->not_empty);
        pthread_mutex_unlock(&c->mu);
    }
    ZSTD_freeDStream(ds);
    return NULL;
fail:
    pipe_fail(c);
fail_silent:
    if (ds) {
        ZSTD_freeDStream(ds);
    }
    return NULL;
}

/* Unfilter rows out of the ring, strictly in order -- which is what makes the
   adaptive filter safe here: row y reads row y-1 from `pixels`, and this thread
   is the only writer of `pixels` and wrote that row already. */
static void* consumer(void* arg)
{
    pipe_ctx* c = (pipe_ctx*)arg;
    uint32_t rows_done = 0;
    size_t consumed = 0;

    while (rows_done < c->height) {
        size_t produced_now, rpos;
        uint32_t avail, to_end, n, i;

        pthread_mutex_lock(&c->mu);
        while (!c->failed &&
               (uint32_t)(c->produced / c->fstride) == rows_done) {
            pthread_cond_wait(&c->not_empty, &c->mu);
        }
        if (c->failed) {
            pthread_mutex_unlock(&c->mu);
            return NULL;
        }
        produced_now = c->produced;
        pthread_mutex_unlock(&c->mu);

        avail  = (uint32_t)(produced_now / c->fstride) - rows_done;
        rpos   = consumed % c->cap;
        to_end = (uint32_t)((c->cap - rpos) / c->fstride);
        n      = avail < to_end ? avail : to_end;
        if (n > c->height - rows_done) {
            n = c->height - rows_done;
        }

        for (i = 0; i < n; i++) {
            uint8_t* cur = c->pixels + (size_t)(rows_done + i) * c->row_stride;
            const uint8_t* prev = (rows_done + i) ? cur - c->row_stride : NULL;
            if (!unfilter_row(c->cf, c->ring + rpos + (size_t)i * c->fstride,
                              cur, prev, c->row_stride, c->fwidth,
                              c->pixel_bytes)) {
                pipe_fail(c);
                return NULL;
            }
        }
        rows_done += n;
        consumed  += (size_t)n * c->fstride;

        pthread_mutex_lock(&c->mu);
        c->consumed = consumed;
        pthread_cond_signal(&c->not_full);
        pthread_mutex_unlock(&c->mu);
    }
    return NULL;
}

static int decode_pipelined(const uint8_t* src, size_t src_size, uint8_t cf,
                            const pxl_geometry* g, uint32_t height,
                            uint8_t* pixels, uint32_t rows)
{
    pipe_ctx c;
    pthread_t tp, tc;
    int ok = 0;

    memset(&c, 0, sizeof c);
    c.row_stride  = (size_t)g->filter_width * g->pixel_bytes;
    c.fstride     = frow_stride_of(cf, c.row_stride);
    c.cap         = (size_t)rows * c.fstride;
    c.height      = height;
    c.cf          = cf;
    c.fwidth      = g->filter_width;
    c.pixel_bytes = g->pixel_bytes;
    c.pixels      = pixels;
    c.src         = src;
    c.src_size    = src_size;
    c.ring        = (uint8_t*)malloc(c.cap);
    if (!c.ring) {
        return 0;
    }
    pthread_mutex_init(&c.mu, NULL);
    pthread_cond_init(&c.not_full, NULL);
    pthread_cond_init(&c.not_empty, NULL);

    if (pthread_create(&tp, NULL, producer, &c) != 0) {
        goto done;
    }
    if (pthread_create(&tc, NULL, consumer, &c) != 0) {
        pipe_fail(&c);
        pthread_join(tp, NULL);
        goto done;
    }
    pthread_join(tp, NULL);
    pthread_join(tc, NULL);
    ok = !c.failed && c.consumed == (size_t)height * c.fstride;
done:
    pthread_cond_destroy(&c.not_empty);
    pthread_cond_destroy(&c.not_full);
    pthread_mutex_destroy(&c.mu);
    free(c.ring);
    return ok;
}


/* ---- control: how much parallelism this machine can actually give ----- */

/* A 2-thread result is only interpretable against the speedup the machine can
   deliver on a workload that is perfectly parallel by construction. Two
   *independent* decodes into separate buffers share nothing, so their 2-thread
   speedup is this box's ceiling for any 2-thread scheme at this moment. On an
   idle 2-core machine it lands near 2.0; with one core already busy it lands
   near 1.0, and a pipeline measuring 0.9x should then be read against that,
   not against the theoretical 2.0. Measuring the ceiling alongside the result
   is what makes the run publishable on a machine that is not perfectly idle. */
typedef struct {
    const uint8_t*      frame;
    size_t              frame_size;
    uint8_t             cf;
    const pxl_geometry* g;
    uint32_t            height;
    uint8_t*            pixels;
    int                 ok;
} dec_job;

static void* dec_thread(void* arg)
{
    dec_job* j = (dec_job*)arg;
    j->ok = decode_rows_bounded(j->frame, j->frame_size, j->cf, j->g,
                                j->height, j->pixels);
    return NULL;
}

static double control_speedup(const uint8_t* frame, size_t fs, uint8_t cf,
                              const pxl_geometry* g, uint32_t height,
                              uint8_t* pa, uint8_t* pb, int reps, double* t,
                              double* cpu_pct_out)
{
    dec_job ja, jb;
    pthread_t ta, tb;
    double one, two;
    double* tc = (double*)malloc(sizeof *tc * (size_t)reps);
    int i;

    ja.frame = frame; ja.frame_size = fs; ja.cf = cf;
    ja.g = g; ja.height = height; ja.pixels = pa; ja.ok = 0;
    jb = ja; jb.pixels = pb;

    for (i = 0; i < reps; i++) {
        double t0 = now_sec();
        dec_thread(&ja);
        dec_thread(&jb);
        t[i] = now_sec() - t0;
        if (!ja.ok || !jb.ok) {
            free(tc);
            return 0.0;
        }
    }
    one = median(t, reps);

    for (i = 0; i < reps; i++) {
        double t0 = now_sec(), tc0 = now_cpu_sec();
        if (pthread_create(&ta, NULL, dec_thread, &ja) != 0) {
            free(tc);
            return 0.0;
        }
        if (pthread_create(&tb, NULL, dec_thread, &jb) != 0) {
            pthread_join(ta, NULL);
            free(tc);
            return 0.0;
        }
        pthread_join(ta, NULL);
        pthread_join(tb, NULL);
        t[i] = now_sec() - t0;
        if (tc) {
            tc[i] = now_cpu_sec() - tc0;
        }
        if (!ja.ok || !jb.ok) {
            free(tc);
            return 0.0;
        }
    }
    two = median(t, reps);
    if (cpu_pct_out) {
        *cpu_pct_out = tc ? 100.0 * median(tc, reps) / (2.0 * two) : -1.0;
    }
    if (tc) {
        int clean_n = 0;
        double clean_two = best_clean(t, tc, reps, 2.0, 0.85, &clean_n);
        if (clean_n > 0 && clean_two > 0.0) {
            printf("%-9s %6s %9s %11s %11s %11s %9.2fx %9s %8s   "
                   "(%d/%d reps got >=85%% of 2 cores; clean ceiling %.2fx)\n",
                   "", "", "", "", "", "clean-only", one / clean_two, "", "",
                   clean_n, reps, one / clean_two);
        } else {
            printf("%-9s %6s %9s %11s %11s %11s %9s %9s %8s   "
                   "(0/%d reps got >=85%% of 2 cores -- no clean sample)\n",
                   "", "", "", "", "", "clean-only", "", "", "", reps);
        }
    }
    free(tc);
    return two > 0.0 ? one / two : 0.0;
}

/* ---- driver ----------------------------------------------------------- */

static void print_conditions(void)
{
    FILE* f = fopen("/proc/loadavg", "r");
    char  buf[64] = "?";
    long  cores = sysconf(_SC_NPROCESSORS_ONLN);

    if (f) {
        if (!fgets(buf, sizeof buf, f)) {
            strcpy(buf, "?");
        }
        fclose(f);
    }
    /* A two-thread result on a loaded machine measures the scheduler, not the
       pipeline, so the conditions go in the output next to the numbers. */
    printf("# %ld cores online, loadavg %.*s\n",
           cores, (int)strcspn(buf, "\n"), buf);
}

static int measure(const pxl_image* img, uint8_t filter, int level, int reps,
                   const uint32_t* depths, int ndepths, double* t)
{
    pxl_geometry g;
    uint8_t*     staging = NULL;
    uint8_t*     frame   = NULL;
    uint8_t*     ref     = NULL;
    uint8_t*     pixels  = NULL;
    size_t       max_filtered, filtered_bytes, bound, frame_size;
    size_t       row_stride, fstride;
    double       ser1_ms;
    double*      tcpu;
    double       pipe_cpu_pct;
    uint8_t      depth = pxl_bit_depth(img);
    int          i, d, rc = 0;

    /* BCIF has no row window: its plane split completes no row until the final
       plane byte. Excluded here exactly as it is in decode_rows_bounded. */
    if (filter == PXL_FILTER_BCIF) {
        return 0;
    }
    if (!pxl_geometry_of(img->width, img->height, img->channels, depth, &g)) {
        return 0;
    }

    max_filtered = pxl_filtered_size(filter, g.filter_width, img->height,
                                     g.pixel_bytes);
    bound        = ZSTD_compressBound(max_filtered);
    staging = (uint8_t*)malloc(max_filtered);
    frame   = (uint8_t*)malloc(bound);
    ref     = (uint8_t*)malloc(g.raw_bytes);
    pixels  = (uint8_t*)malloc(g.raw_bytes);
    tcpu    = (double*)malloc(sizeof *tcpu * (size_t)reps);
    if (!staging || !frame || !ref || !pixels || !tcpu) {
        goto done;
    }

    filtered_bytes = apply_filter(filter, g.pixel_bytes, img->buffer.data,
                                  staging, g.filter_width, img->height);
    if (filtered_bytes == 0 || filtered_bytes != max_filtered) {
        goto done;
    }
    frame_size = ZSTD_compress(frame, bound, staging, filtered_bytes, level);
    if (ZSTD_isError(frame_size)) {
        goto done;
    }

    row_stride = (size_t)g.filter_width * g.pixel_bytes;
    fstride    = frow_stride_of(filter, row_stride);

    /* Reference output: the shipping path. Everything else is compared to it. */
    if (!decode_rows_bounded(frame, frame_size, filter, &g, img->height, ref)) {
        fprintf(stderr, "error: reference decode failed (%s)\n",
                filter_name(filter));
        goto done;
    }
    if (memcmp(ref, img->buffer.data, g.raw_bytes) != 0) {
        fprintf(stderr, "error: reference decode does not round-trip (%s)\n",
                filter_name(filter));
        goto done;
    }

    for (i = 0; i < reps; i++) {
        double t0 = now_sec();
        int ok = decode_rows_bounded(frame, frame_size, filter, &g,
                                     img->height, pixels);
        t[i] = now_sec() - t0;
        if (!ok) {
            goto done;
        }
    }
    ser1_ms = median(t, reps) * 1e3;

    for (d = 0; d < ndepths; d++) {
        uint32_t rows = depths[d];
        double serN_ms, pipe_ms;
        int bad = 0;

        if (rows > img->height) {
            rows = img->height;
        }

        for (i = 0; i < reps; i++) {
            double t0, el;
            int ok;
            memset(pixels, 0, g.raw_bytes);
            t0 = now_sec();
            ok = decode_serial_block(frame, frame_size, filter, &g,
                                     img->height, pixels, rows);
            el = now_sec() - t0;
            t[i] = el;
            bad |= !ok || memcmp(pixels, ref, g.raw_bytes) != 0;
        }
        serN_ms = median(t, reps) * 1e3;

        for (i = 0; i < reps; i++) {
            double t0, tc0, el, cpu;
            int ok;
            memset(pixels, 0, g.raw_bytes);
            t0 = now_sec();
            tc0 = now_cpu_sec();
            ok = decode_pipelined(frame, frame_size, filter, &g,
                                  img->height, pixels, rows);
            el  = now_sec() - t0;
            cpu = now_cpu_sec() - tc0;
            t[i] = el;
            tcpu[i] = cpu;
            bad |= !ok || memcmp(pixels, ref, g.raw_bytes) != 0;
        }
        pipe_ms = median(t, reps) * 1e3;
        pipe_cpu_pct = 100.0 * median(tcpu, reps) / (2.0 * median(t, reps));

        printf("%-9s %6u %9.1f %11.3f %11.3f %11.3f %9.2fx %9.2fx %8.0f%%  %s\n",
               filter_name(filter), rows,
               (double)((size_t)rows * fstride) / 1024.0,
               ser1_ms, serN_ms, pipe_ms,
               ser1_ms / pipe_ms, serN_ms / pipe_ms, pipe_cpu_pct,
               bad ? "MISMATCH" : "ok");
        {
            int clean_n = 0;
            double clean_ms = best_clean(t, tcpu, reps, 2.0, 0.85, &clean_n) * 1e3;
            if (clean_n > 0) {
                printf("%-9s %6s %9s %11s %11s %11.3f %9s %9.2fx %8s   (%d/%d reps got >=85%% of 2 cores)\n",
                       "", "", "", "", "", clean_ms, "",
                       serN_ms / clean_ms, "", clean_n, reps);
            } else {
                printf("%-9s %6s %9s %11s %11s %11s %9s %9s %8s   (0/%d reps got >=85%% of 2 cores -- no clean sample)\n",
                       "", "", "", "", "", "-", "", "", "", reps);
            }
        }
        fflush(stdout);
    }
    {
        uint8_t* pb = (uint8_t*)malloc(g.raw_bytes);
        if (pb) {
            double ceil_cpu_pct = -1.0;
            double ceil2 = control_speedup(frame, frame_size, filter, &g,
                                           img->height, pixels, pb, reps, t,
                                           &ceil_cpu_pct);
            printf("%-9s %6s %9s %11s %11s %11s %9.2fx %9s %8.0f%%  %s\n",
                   filter_name(filter), "-", "-", "-", "-", "2x independent",
                   ceil2, "-", ceil_cpu_pct, "machine ceiling");
            free(pb);
        }
    }
    fflush(stdout);
    rc = 1;
done:
    free(staging);
    free(frame);
    free(ref);
    free(pixels);
    free(tcpu);
    return rc;
}

int main(int argc, char** argv)
{
    static const uint8_t filters[] = {
        PXL_FILTER_ADAPTIVE, PXL_FILTER_DELTA, PXL_FILTER_NONE
    };
    uint32_t  depths[MAX_DEPTHS] = { 8, 32, 128 };
    int       ndepths = 3;
    pxl_image img;
    double*   t;
    int       reps, level;
    size_t    fi;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <image.png> [reps] [level] [rows...]\n",
                argv[0]);
        return 2;
    }
    reps  = argc > 2 ? atoi(argv[2]) : 9;
    level = argc > 3 ? atoi(argv[3]) : PXL_LEVEL_DEFAULT;
    if (reps < 1) {
        reps = 1;
    }
    if (argc > 4) {
        int i;
        ndepths = 0;
        for (i = 4; i < argc && ndepths < MAX_DEPTHS; i++) {
            int v = atoi(argv[i]);
            if (v > 0) {
                depths[ndepths++] = (uint32_t)v;
            }
        }
        if (ndepths == 0) {
            ndepths = 1;
            depths[0] = 32;
        }
    }

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

    print_conditions();
    printf("# %s  %ux%u  %u channels  %u-bit  raw %zu bytes  "
           "(median of %d, level %d)\n",
           argv[1], img.width, img.height, img.channels, img.bit_depth,
           img.buffer.size, reps, level);
    printf("# serial-1 is the shipping decoder; serial-N isolates the ring\n"
           "# size from threading, so 'vs ser-N' is the pipeline's real gain.\n");
    printf("%-9s %6s %9s %11s %11s %11s %10s %10s %9s  %s\n",
           "filter", "rows", "ring KiB", "serial-1 ms", "serial-N ms",
           "pipe-N ms", "vs ser-1", "vs ser-N", "cpu%(2)", "match");

    for (fi = 0; fi < sizeof filters / sizeof filters[0]; fi++) {
        measure(&img, filters[fi], level, reps, depths, ndepths, t);
    }

    free(t);
    pxl_image_free(&img);
    return 0;
}

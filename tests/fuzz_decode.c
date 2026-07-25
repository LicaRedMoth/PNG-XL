/** \file fuzz_decode.c
    \brief Robustness check: feed malformed .pxl / .apxl bytes to every decoder.

    Decoders are the attack surface -- an ffmpeg codec or a thumbnailer runs them
    on untrusted files. This harness builds valid streams, mutates and truncates
    them, and also throws pure garbage with a plausible magic, then runs each
    through pxl_decode, the streaming decoder, and apxl_decode. Success means "no
    crash and no leak", so it is meant to be built with sanitizers:

      cc -g -O1 -fsanitize=address,undefined -Isrc \
         tests/fuzz_decode.c src/pxl_codec.c src/pxl_io.c src/apxl_codec.c \
         -o fuzz_decode $(pkg-config --cflags --libs libzstd)
      ASAN_OPTIONS=detect_leaks=1 ./fuzz_decode

    It only needs libpxlcore (no libpng). The PRNG is a fixed-seed LCG so a
    failure reproduces exactly; pass an iteration count to run longer.
*/
#include "pxl.h"
#include "apxl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long rng_state = 12345;

static unsigned next_rand(void)
{
    rng_state = rng_state * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned)(rng_state >> 33);
}

/* Run every decoder over one candidate buffer. Any crash/overflow is the bug we
   are hunting; a rejected file is the expected outcome. */
static void try_all_decoders(const unsigned char* buf, size_t len)
{
    {   /* one-shot still decode */
        pxl_buffer f;
        pxl_image d;
        f.data = (unsigned char*)buf;
        f.size = len;
        d = pxl_decode(f);
        pxl_image_free(&d);
    }
    {   /* streaming decode, fed in small uneven chunks */
        pxl_stream* s = pxl_stream_new(NULL, NULL);
        size_t off, chunk = 1 + next_rand() % 23;
        if (s) {
            for (off = 0; off < len; off += chunk) {
                size_t n = len - off < chunk ? len - off : chunk;
                if (pxl_stream_feed(s, buf + off, n) < 0) { break; }
            }
            pxl_stream_finish(s);
            pxl_stream_free(s);
        }
    }
    {   /* animation decode */
        pxl_buffer f;
        apxl_anim a;
        f.data = (unsigned char*)buf;
        f.size = len;
        a = apxl_decode(f);
        apxl_free(&a);
    }
}

/* Encode a small image so we have structurally valid bytes to corrupt. */
static pxl_buffer make_valid(uint8_t channels, uint8_t bpc, unsigned flags)
{
    pxl_image src;
    pxl_buffer out;
    size_t size = (size_t)64 * 40 * channels * bpc, i;

    memset(&src, 0, sizeof(src));
    src.buffer.data = (unsigned char*)malloc(size);
    if (!src.buffer.data) { out.data = NULL; out.size = 0; return out; }
    for (i = 0; i < size; ++i) {
        src.buffer.data[i] = (unsigned char)(i * 7 + (i >> 4));
    }
    src.buffer.size = size;
    src.width = 64; src.height = 40;
    src.channels = channels; src.bytes_per_channel = bpc;

    out = pxl_encode_ex(&src, 3, flags);
    pxl_image_free(&src);
    return out;
}

int main(int argc, char** argv)
{
    pxl_buffer good[3];
    long iterations = (argc > 1) ? atol(argv[1]) : 200000;
    long iter;
    int i, n_good = 0;

    good[n_good++] = make_valid(4, 1, 0);                        /* BCIF likely */
    good[n_good++] = make_valid(4, 1, PXL_ENCODE_PROGRESSIVE);   /* row-wise */
    good[n_good++] = make_valid(1, 2, 0);                        /* 16-bit gray */
    for (i = 0; i < n_good; ++i) {
        if (!good[i].data) {
            printf("fuzz_decode: could not build seed inputs\n");
            return 1;
        }
    }

    /* Phase 1: mutate and truncate valid files. This is what found a header
       claiming BCIF on a geometry BCIF does not support. */
    for (iter = 0; iter < iterations; ++iter) {
        pxl_buffer g = good[next_rand() % (unsigned)n_good];
        int mode = (int)(next_rand() % 4);
        size_t n = (mode == 0) ? next_rand() % (g.size + 8) : g.size;
        unsigned char* b = (unsigned char*)malloc(n ? n : 1);
        if (!b) { break; }
        memcpy(b, g.data, n < g.size ? n : g.size);
        if (n > g.size) { memset(b + g.size, 0, n - g.size); }
        if (mode != 0 && n) {
            int k = 1 + (int)(next_rand() % 6), j;
            for (j = 0; j < k; ++j) {
                b[next_rand() % n] = (unsigned char)next_rand();
            }
        }
        try_all_decoders(b, n);
        free(b);
    }

    /* Phase 2: short random buffers, half of them carrying a valid magic so the
       header parsers commit to a geometry before hitting nonsense. */
    for (iter = 0; iter < iterations; ++iter) {
        unsigned char b[64];
        size_t n = 1 + next_rand() % sizeof(b);
        size_t j;
        for (j = 0; j < n; ++j) { b[j] = (unsigned char)next_rand(); }
        if (next_rand() % 2) {
            memcpy(b, "PXL1", n < 4 ? n : 4);
            if (n > 4) { b[4] = 1; }
        } else {
            memcpy(b, "APXL", n < 4 ? n : 4);
            if (n > 4) { b[4] = 2; }
        }
        try_all_decoders(b, n);
    }

    for (i = 0; i < n_good; ++i) { pxl_free(&good[i]); }
    printf("fuzz_decode: %ld iterations x 2 phases, no crash\n", iterations);
    return 0;
}

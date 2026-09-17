/** \file ldmsweep.c
    \brief Measures zstd long-distance matching's actual effect on APXL-style
           cross-frame compression, per shot, holding the compression level
           fixed so LDM is the only variable.

    The question (ROADMAP.md's "Decide cross-frame long-distance matching per
    stream"). `apxl_encode` (src/apxl_codec.c) ties LDM to compression level
    alone -- it enables LDM, at a fixed windowLog, whenever the chosen level is
    >= APXL_LDM_MIN_LEVEL, and there is no way to ask the shipped encoder for
    "this level, LDM off" as a counterfactual. This tool reproduces
    apxl_encode's exact compression parameters (same level, same windowLog --
    see the constants below, which must be kept in sync with apxl_codec.c)
    around an explicit on/off flag instead of deriving it from the level. It
    does not write an .apxl container; only the payload zstd stream size is
    compared, since that is the only thing LDM can change -- the header,
    metadata and timing table are identical either way.

    This is deliberately NOT the same comparison RESEARCH.md's original 16-
    frame numbers used ("concatenated + --long=27" against "compressing each
    frame independently", i.e. no cross-frame stream at all). Per-frame
    streams were already measured and rejected elsewhere (SPEC.md section 10)
    on their own merits, so they are not the live question. The live question
    is narrower: given the single cross-frame stream .apxl already always
    uses, should LDM's larger window be on or off. That is what this tool
    isolates.

    Method. Load N consecutive frames of one shot (same canvas geometry
    required, matching apxl_encode's own precondition), concatenate their raw
    pixels in order exactly as apxl_encode does, and compress the whole thing
    twice at the same level: once with LDM off, once on. Report both sizes and
    the percentage difference.

    Usage:
      bench/ldmsweep <frame.png> <frame.png> ...   [env: LDMSWEEP_LEVEL]

    Frames are given in order and treated as one shot; the summary is per
    shot, never per frame -- see bench/motion.c and RESEARCH.md's "Animation:
    cross-frame coding splits the corpus in two" for why frame-level sampling
    on this corpus is misleading (~12 points of spread from sampling alone).
*/
#include "../src/apxl.h"    /* APXL_LEVEL_DEFAULT */
#include "../src/pxl_png.h" /* pxl_load_png */

#include <zstd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Must match src/apxl_codec.c's own APXL_LDM_MIN_LEVEL / APXL_WINDOW_LOG. If
   either changes there, update it here too, or this tool stops measuring what
   the shipped encoder actually does. */
#define LDM_MIN_LEVEL_REFERENCE 10
#define LDM_WINDOW_LOG          27

static size_t compress_once(const unsigned char* raw, size_t raw_size,
                            int level, int ldm)
{
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    size_t bound, csize;
    unsigned char* out;

    if (!cctx) { return 0; }
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
    if (ldm) {
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, LDM_WINDOW_LOG);
    }
    bound = ZSTD_compressBound(raw_size);
    out = (unsigned char*)malloc(bound);
    if (!out) { ZSTD_freeCCtx(cctx); return 0; }
    csize = ZSTD_compress2(cctx, out, bound, raw, raw_size);
    ZSTD_freeCCtx(cctx);
    free(out);
    if (ZSTD_isError(csize)) { return 0; }
    return csize;
}

int main(int argc, char** argv)
{
    int level = APXL_LEVEL_DEFAULT;
    const char* env = getenv("LDMSWEEP_LEVEL");
    pxl_image first;
    unsigned char* raw;
    size_t canvas_bytes, raw_size, off_size, on_size;
    int i, n;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <frame.png> <frame.png> ...  (one shot, in order)\n", argv[0]);
        return 2;
    }
    if (env && atoi(env) > 0) { level = atoi(env); }

    memset(&first, 0, sizeof(first));
    first = pxl_load_png(argv[1]);
    if (!first.buffer.data) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }

    canvas_bytes = (size_t)first.width * first.height *
                   (size_t)first.channels * (size_t)first.bytes_per_channel;
    raw_size = canvas_bytes * (size_t)(argc - 1);
    raw = (unsigned char*)malloc(raw_size);
    if (!raw) {
        fprintf(stderr, "out of memory\n");
        pxl_image_free(&first);
        return 1;
    }
    memcpy(raw, first.buffer.data, canvas_bytes);
    pxl_image_free(&first);
    n = 1;

    for (i = 2; i < argc; ++i) {
        pxl_image cur = pxl_load_png(argv[i]);
        size_t cur_bytes;

        if (!cur.buffer.data) {
            fprintf(stderr, "cannot load %s\n", argv[i]);
            free(raw);
            return 1;
        }
        cur_bytes = (size_t)cur.width * cur.height *
                    (size_t)cur.channels * (size_t)cur.bytes_per_channel;
        if (cur_bytes != canvas_bytes) {
            fprintf(stderr, "geometry changes at %s -- every frame of a shot "
                            "must match, like apxl_encode requires\n", argv[i]);
            pxl_image_free(&cur);
            free(raw);
            return 1;
        }
        memcpy(raw + (size_t)n * canvas_bytes, cur.buffer.data, canvas_bytes);
        pxl_image_free(&cur);
        ++n;
    }

    off_size = compress_once(raw, raw_size, level, 0);
    on_size  = compress_once(raw, raw_size, level, 1);
    free(raw);

    if (!off_size || !on_size) { fprintf(stderr, "compression failed\n"); return 1; }

    printf("frames=%d level=%d raw=%zu off=%zu on=%zu delta=%.2f%%\n",
           n, level, raw_size, off_size, on_size,
           100.0 * ((double)on_size - (double)off_size) / (double)off_size);

    if (level < LDM_MIN_LEVEL_REFERENCE) {
        fprintf(stderr, "warning: level %d is below APXL_LDM_MIN_LEVEL (%d) -- "
                        "apxl_encode itself never enables LDM there, so the "
                        "'on' column is hypothetical, not what real files at "
                        "this level get today\n", level, LDM_MIN_LEVEL_REFERENCE);
    }

    return 0;
}

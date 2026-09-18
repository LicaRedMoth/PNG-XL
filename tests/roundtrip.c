/** \file roundtrip.c
    \brief Lossless round-trip test for the PNG XL codec.

    Generates synthetic images (8-bit gray, GA, RGB, RGBA and 16-bit RGB),
    runs pxl_encode -> pxl_decode, and verifies the pixels come back
    byte-for-byte identical. Also exercises the PNG interop path via a
    temporary file.
*/
#include "pxl.h"
#include "pxl_png.h"
#include "pxl_format.h"
#include "apxl.h"
#include "apng.h"
#include "gif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check_roundtrip(const char* name, uint32_t w, uint32_t h,
                           uint8_t channels, uint8_t bpc)
{
    pxl_image src;
    pxl_buffer enc;
    pxl_image dec;
    size_t size, i;
    int ok = 1;

    size = (size_t)w * h * channels * bpc;
    memset(&src, 0, sizeof(src));
    src.buffer.data = (unsigned char*)malloc(size);
    src.buffer.size = size;
    src.width = w;
    src.height = h;
    src.channels = channels;
    src.bytes_per_channel = bpc;

    if (!src.buffer.data) {
        printf("[FAIL] %s: alloc\n", name);
        return 0;
    }

    /* Deterministic pseudo-image with gradients + noise. */
    for (i = 0; i < size; ++i) {
        src.buffer.data[i] = (unsigned char)((i * 131 + (i >> 3) * 17 + i / (w + 1)) & 0xFF);
    }

    enc = pxl_encode(&src, 3);
    if (!enc.data) {
        printf("[FAIL] %s: encode\n", name);
        free(src.buffer.data);
        return 0;
    }

    dec = pxl_decode(enc);
    if (!dec.buffer.data) {
        printf("[FAIL] %s: decode\n", name);
        pxl_free(&enc);
        free(src.buffer.data);
        return 0;
    }

    if (dec.width != w || dec.height != h ||
        dec.channels != channels || dec.bytes_per_channel != bpc ||
        dec.buffer.size != size) {
        printf("[FAIL] %s: geometry mismatch\n", name);
        ok = 0;
    } else if (memcmp(src.buffer.data, dec.buffer.data, size) != 0) {
        printf("[FAIL] %s: pixel data differs\n", name);
        ok = 0;
    }

    if (ok) {
        printf("[ OK ] %s: %ux%u c=%u bpc=%u  %zu -> %zu bytes\n",
               name, w, h, channels, bpc, size, enc.size);
    }

    pxl_free(&enc);
    pxl_free(&dec.buffer);
    free(src.buffer.data);
    return ok;
}

/* PNG interop round-trip: encode a synthetic RGB image to PNG, load it back,
   and confirm the codec pipeline works end to end through libpng. */
static int check_png_interop(const char* tmp_png)
{
    pxl_image src, loaded;
    size_t size, i;
    uint32_t w = 64, h = 48;
    int ok = 1;

    size = (size_t)w * h * 3;
    memset(&src, 0, sizeof(src));
    src.buffer.data = (unsigned char*)malloc(size);
    src.buffer.size = size;
    src.width = w;
    src.height = h;
    src.channels = 3;
    src.bytes_per_channel = 1;
    if (!src.buffer.data) {
        printf("[FAIL] png_interop: alloc\n");
        return 0;
    }
    for (i = 0; i < size; ++i) {
        src.buffer.data[i] = (unsigned char)((i * 7) & 0xFF);
    }

    if (!pxl_save_png(tmp_png, &src)) {
        printf("[FAIL] png_interop: save_png\n");
        free(src.buffer.data);
        return 0;
    }

    loaded = pxl_load_png(tmp_png);
    if (!loaded.buffer.data) {
        printf("[FAIL] png_interop: load_png\n");
        free(src.buffer.data);
        return 0;
    }

    if (loaded.width != w || loaded.height != h ||
        loaded.channels != 3 || loaded.bytes_per_channel != 1 ||
        memcmp(loaded.buffer.data, src.buffer.data, size) != 0) {
        printf("[FAIL] png_interop: mismatch after PNG round-trip\n");
        ok = 0;
    } else {
        printf("[ OK ] png_interop: PNG save/load lossless\n");
    }

    remove(tmp_png);
    free(src.buffer.data);
    pxl_free(&loaded.buffer);
    return ok;
}

/* Metadata survives the codec: attach a synthetic metadata blob to an image,
   encode, decode, and confirm it comes back identical. */
static int check_metadata(void)
{
    pxl_image src, dec;
    pxl_buffer enc;
    size_t size = 32 * 32 * 3, i;
    const char* meta = "iCCPfake-profile\x00\x01\x02\x03 and some text bytes";
    size_t meta_len = strlen(meta) + 4; /* include trailing bytes past the NUL */
    int ok = 1;

    memset(&src, 0, sizeof(src));
    src.buffer.data = (unsigned char*)malloc(size);
    src.buffer.size = size;
    src.width = 32; src.height = 32; src.channels = 3; src.bytes_per_channel = 1;
    src.metadata.data = (unsigned char*)malloc(meta_len);
    src.metadata.size = meta_len;
    if (!src.buffer.data || !src.metadata.data) {
        printf("[FAIL] metadata: alloc\n");
        free(src.buffer.data); free(src.metadata.data);
        return 0;
    }
    for (i = 0; i < size; ++i) src.buffer.data[i] = (unsigned char)(i * 3);
    memcpy(src.metadata.data, meta, meta_len);

    enc = pxl_encode(&src, 3);
    if (!enc.data) { printf("[FAIL] metadata: encode\n"); pxl_image_free(&src); return 0; }
    dec = pxl_decode(enc);
    if (!dec.buffer.data) { printf("[FAIL] metadata: decode\n"); pxl_free(&enc); pxl_image_free(&src); return 0; }

    if (dec.metadata.size != meta_len ||
        memcmp(dec.metadata.data, src.metadata.data, meta_len) != 0) {
        printf("[FAIL] metadata: blob not preserved (%zu vs %zu)\n",
               dec.metadata.size, meta_len);
        ok = 0;
    } else if (memcmp(dec.buffer.data, src.buffer.data, size) != 0) {
        printf("[FAIL] metadata: pixels differ\n");
        ok = 0;
    } else {
        printf("[ OK ] metadata: %zu-byte blob preserved, pixels lossless\n", meta_len);
    }

    pxl_free(&enc);
    pxl_image_free(&src);
    pxl_image_free(&dec);
    return ok;
}

/* A vertical gradient (each row nearly equal to the one above) is a case the
   PNG-style adaptive filter handles far better than the left-only delta. This
   verifies the adaptive filter is actually selected AND that its decode path
   round-trips losslessly. */
static int check_adaptive(void)
{
    pxl_image src, dec;
    pxl_buffer enc;
    pxl_header h;
    uint32_t w = 200, h_ = 200, x, y;
    size_t size = (size_t)w * h_ * 3;
    int ok = 1;

    memset(&src, 0, sizeof(src));
    src.buffer.data = (unsigned char*)malloc(size);
    src.buffer.size = size;
    src.width = w; src.height = h_; src.channels = 3; src.bytes_per_channel = 1;
    if (!src.buffer.data) { printf("[FAIL] adaptive: alloc\n"); return 0; }

    /* Value depends mostly on the row -> vertical predictors win big. */
    for (y = 0; y < h_; ++y)
        for (x = 0; x < w; ++x) {
            size_t o = ((size_t)y * w + x) * 3;
            src.buffer.data[o + 0] = (unsigned char)y;
            src.buffer.data[o + 1] = (unsigned char)(y + (x >> 4));
            src.buffer.data[o + 2] = (unsigned char)(255 - y);
        }

    enc = pxl_encode(&src, 12);
    if (!enc.data) { printf("[FAIL] adaptive: encode\n"); pxl_image_free(&src); return 0; }
    if (!pxl_header_read(enc.data, enc.size, &h)) {
        printf("[FAIL] adaptive: header\n"); pxl_free(&enc); pxl_image_free(&src); return 0;
    }
    dec = pxl_decode(enc);
    if (!dec.buffer.data) { printf("[FAIL] adaptive: decode\n"); pxl_free(&enc); pxl_image_free(&src); return 0; }

    if (h.color_filter != PXL_FILTER_ADAPTIVE) {
        printf("[FAIL] adaptive: expected adaptive filter to win, got %u\n", h.color_filter);
        ok = 0;
    } else if (dec.buffer.size != size ||
               memcmp(dec.buffer.data, src.buffer.data, size) != 0) {
        printf("[FAIL] adaptive: pixels differ after round-trip\n");
        ok = 0;
    } else {
        printf("[ OK ] adaptive: filter selected, %zu -> %zu bytes, lossless\n",
               size, enc.size);
    }

    pxl_free(&enc);
    pxl_image_free(&src);
    pxl_image_free(&dec);
    return ok;
}

/* PXL_ENCODE_FAST_DECODE must exclude ADAPTIVE, not just BCIF: reuses
   check_adaptive()'s exact content (a vertical gradient, proven above to make
   the default encoder pick ADAPTIVE) and confirms FAST_DECODE picks something
   else. If this content ever stops selecting ADAPTIVE by default, this test
   would silently stop covering the path it exists for -- same guard
   check_adaptive() already applies to itself. */
static int check_fast_decode_excludes_adaptive(void)
{
    pxl_image src, dec;
    pxl_buffer enc;
    pxl_header h;
    uint32_t w = 200, h_ = 200, x, y;
    size_t size = (size_t)w * h_ * 3;
    int ok = 1;

    memset(&src, 0, sizeof(src));
    src.buffer.data = (unsigned char*)malloc(size);
    src.buffer.size = size;
    src.width = w; src.height = h_; src.channels = 3; src.bytes_per_channel = 1;
    if (!src.buffer.data) { printf("[FAIL] fast_decode: alloc\n"); return 0; }

    for (y = 0; y < h_; ++y)
        for (x = 0; x < w; ++x) {
            size_t o = ((size_t)y * w + x) * 3;
            src.buffer.data[o + 0] = (unsigned char)y;
            src.buffer.data[o + 1] = (unsigned char)(y + (x >> 4));
            src.buffer.data[o + 2] = (unsigned char)(255 - y);
        }

    enc = pxl_encode(&src, 12);
    if (!enc.data || !pxl_header_read(enc.data, enc.size, &h) ||
        h.color_filter != PXL_FILTER_ADAPTIVE) {
        printf("[FAIL] fast_decode: fixture no longer selects adaptive by default\n");
        pxl_free(&enc); pxl_image_free(&src);
        return 0;
    }
    pxl_free(&enc);

    enc = pxl_encode_ex(&src, 12, PXL_ENCODE_FAST_DECODE);
    if (!enc.data || !pxl_header_read(enc.data, enc.size, &h)) {
        printf("[FAIL] fast_decode: encode\n"); pxl_image_free(&src); return 0;
    }
    if (h.color_filter != PXL_FILTER_NONE && h.color_filter != PXL_FILTER_DELTA) {
        printf("[FAIL] fast_decode: expected none/delta, got filter %u\n", h.color_filter);
        ok = 0;
    }
    dec = pxl_decode(enc);
    if (!dec.buffer.data || dec.buffer.size != size ||
        memcmp(dec.buffer.data, src.buffer.data, size) != 0) {
        printf("[FAIL] fast_decode: pixels differ after round-trip\n");
        ok = 0;
    } else if (ok) {
        printf("[ OK ] fast_decode: adaptive excluded, filter %u, %zu -> %zu bytes, lossless\n",
               h.color_filter, size, enc.size);
    }

    pxl_free(&enc);
    pxl_image_free(&src);
    pxl_image_free(&dec);
    return ok;
}

/* Animation round-trip: a moving block over a gradient, plus one unchanged
   frame. Verifies every full canvas frame comes back bit-exact, exercising
   keyframe, delta, and unchanged-frame paths. */
static int check_anim(void)
{
    uint32_t W = 80, H = 60, N = 6, f, x, y;
    uint8_t ch = 4, bpc = 1;
    unsigned pb = ch * bpc;
    size_t cb = (size_t)W * H * pb;
    apxl_anim a, d;
    pxl_buffer enc;
    uint8_t** ref = NULL;
    int ok = 1;

    memset(&a, 0, sizeof(a));
    a.frames = (apxl_frame*)calloc(N, sizeof(apxl_frame));
    ref = (uint8_t**)calloc(N, sizeof(uint8_t*));
    if (!a.frames || !ref) { printf("[FAIL] anim: alloc\n"); free(a.frames); free(ref); return 0; }
    a.frame_count = N; a.loop_count = 0;
    a.canvas_w = W; a.canvas_h = H; a.channels = ch; a.bytes_per_channel = bpc;

    for (f = 0; f < N; ++f) {
        uint8_t* buf = (uint8_t*)malloc(cb);
        ref[f] = (uint8_t*)malloc(cb);
        if (!buf || !ref[f]) { printf("[FAIL] anim: frame alloc\n"); ok = 0; break; }
        for (y = 0; y < H; ++y)
            for (x = 0; x < W; ++x) {
                size_t o = ((size_t)y * W + x) * 4;
                buf[o+0] = (uint8_t)x; buf[o+1] = (uint8_t)y;
                buf[o+2] = (uint8_t)(x + y); buf[o+3] = 255;
            }
        /* Frame 3 is identical to frame 2 (unchanged-frame path). */
        if (f != 3) {
            uint32_t bx = f * 8, by = f * 4;
            for (y = 0; y < 10 && by + y < H; ++y)
                for (x = 0; x < 10 && bx + x < W; ++x) {
                    size_t o = ((size_t)(by + y) * W + (bx + x)) * 4;
                    buf[o+0] = 220; buf[o+1] = 30; buf[o+2] = 30; buf[o+3] = 255;
                }
        } else {
            /* copy frame 2 exactly */
            memcpy(buf, ref[2], cb);
        }
        memcpy(ref[f], buf, cb);
        a.frames[f].image.buffer.data = buf;
        a.frames[f].image.buffer.size = cb;
        a.frames[f].image.width = W; a.frames[f].image.height = H;
        a.frames[f].image.channels = ch; a.frames[f].image.bytes_per_channel = bpc;
        a.frames[f].delay_num = 1; a.frames[f].delay_den = 10;
    }

    enc = ok ? apxl_encode(&a, 12) : (pxl_buffer){ NULL, 0 };
    if (ok && !enc.data) { printf("[FAIL] anim: encode\n"); ok = 0; }

    memset(&d, 0, sizeof(d));
    if (ok) {
        d = apxl_decode(enc);
        if (!d.frames || d.frame_count != N) { printf("[FAIL] anim: decode\n"); ok = 0; }
    }
    if (ok) {
        for (f = 0; f < N; ++f) {
            if (memcmp(d.frames[f].image.buffer.data, ref[f], cb) != 0) {
                printf("[FAIL] anim: frame %u differs\n", f); ok = 0; break;
            }
        }
    }
    if (ok) {
        printf("[ OK ] anim: %u frames %ux%u -> %zu bytes, all lossless\n",
               N, W, H, enc.size);
    }

    pxl_free(&enc);
    apxl_free(&a);
    apxl_free(&d);
    if (ref) { for (f = 0; f < N; ++f) free(ref[f]); free(ref); }
    return ok;
}

/* Indexed .apxl (version 2, added 2026-09-18 -- see docs/RESEARCH.md's
   "Indexed .apxl" entry): one global palette shared by every frame, index
   bytes instead of expanded channels. Also checks that apxl_encode rejects a
   frame whose palette does not byte-match the first frame's, since a
   per-frame palette is not representable in this container and silently
   picking one would be a lossy encode wearing a lossless format's name. */
static int check_anim_indexed(void)
{
    uint32_t W = 16, H = 12, N = 5, f, x, y;
    size_t cb = (size_t)W * H;                 /* 1 index byte per pixel */
    static const uint8_t pal_rgb[6 * 3] = {
        20, 20, 20,   200, 30, 30,   30, 200, 30,
        30, 30, 200,  220, 220, 30,  0, 0, 0
    };
    static const uint8_t pal_a[6] = { 255, 255, 255, 128, 255, 0 };
    apxl_anim a, d;
    pxl_buffer enc;
    uint8_t** ref = NULL;
    int ok = 1;

    memset(&a, 0, sizeof(a));
    a.frames = (apxl_frame*)calloc(N, sizeof(apxl_frame));
    ref = (uint8_t**)calloc(N, sizeof(uint8_t*));
    if (!a.frames || !ref) { printf("[FAIL] anim_indexed: alloc\n"); free(a.frames); free(ref); return 0; }
    a.frame_count = N; a.loop_count = 0;
    a.canvas_w = W; a.canvas_h = H; a.channels = 1; a.bytes_per_channel = 1;

    for (f = 0; f < N; ++f) {
        uint8_t* buf = (uint8_t*)malloc(cb);
        ref[f] = (uint8_t*)malloc(cb);
        if (!buf || !ref[f]) { printf("[FAIL] anim_indexed: frame alloc\n"); ok = 0; break; }
        for (y = 0; y < H; ++y) {
            for (x = 0; x < W; ++x) {
                buf[y * W + x] = (uint8_t)((x + y + f) % 6);
            }
        }
        memcpy(ref[f], buf, cb);
        a.frames[f].image.buffer.data = buf;
        a.frames[f].image.buffer.size = cb;
        a.frames[f].image.width = W; a.frames[f].image.height = H;
        a.frames[f].image.channels = 1; a.frames[f].image.bytes_per_channel = 1;
        a.frames[f].image.palette.data = (unsigned char*)pal_rgb;
        a.frames[f].image.palette.size = sizeof(pal_rgb);
        a.frames[f].image.palette_alpha.data = (unsigned char*)pal_a;
        a.frames[f].image.palette_alpha.size = sizeof(pal_a);
        a.frames[f].delay_num = 1; a.frames[f].delay_den = 10;
    }

    enc = ok ? apxl_encode(&a, 12) : (pxl_buffer){ NULL, 0 };
    if (ok && !enc.data) { printf("[FAIL] anim_indexed: encode\n"); ok = 0; }

    memset(&d, 0, sizeof(d));
    if (ok) {
        d = apxl_decode(enc);
        if (!d.frames || d.frame_count != N) { printf("[FAIL] anim_indexed: decode\n"); ok = 0; }
    }
    if (ok && (d.palette.size != sizeof(pal_rgb) ||
               memcmp(d.palette.data, pal_rgb, sizeof(pal_rgb)) != 0)) {
        printf("[FAIL] anim_indexed: palette RGB differs\n"); ok = 0;
    }
    if (ok && (d.palette_alpha.size != sizeof(pal_a) ||
               memcmp(d.palette_alpha.data, pal_a, sizeof(pal_a)) != 0)) {
        printf("[FAIL] anim_indexed: palette alpha differs\n"); ok = 0;
    }
    if (ok) {
        for (f = 0; f < N; ++f) {
            if (memcmp(d.frames[f].image.buffer.data, ref[f], cb) != 0) {
                printf("[FAIL] anim_indexed: frame %u differs\n", f); ok = 0; break;
            }
            if (d.frames[f].image.palette.data != d.palette.data) {
                printf("[FAIL] anim_indexed: frame %u palette not shared\n", f); ok = 0; break;
            }
        }
    }
    if (ok) {
        printf("[ OK ] anim_indexed: %u frames %ux%u, %u-colour palette -> %zu bytes, all lossless\n",
               N, W, H, (unsigned)(sizeof(pal_rgb) / 3), enc.size);
    }
    pxl_free(&enc);
    apxl_free(&d);

    /* Negative case: frame N-1 gets a palette that differs by one byte.
       apxl_encode must refuse rather than silently keep frame 0's palette. */
    if (ok) {
        static uint8_t bad_rgb[6 * 3];
        pxl_buffer bad_enc;
        memcpy(bad_rgb, pal_rgb, sizeof(pal_rgb));
        bad_rgb[0] ^= 1u;
        a.frames[N - 1].image.palette.data = bad_rgb;
        bad_enc = apxl_encode(&a, 12);
        if (bad_enc.data) {
            printf("[FAIL] anim_indexed: mismatched per-frame palette was not rejected\n");
            ok = 0;
            pxl_free(&bad_enc);
        } else {
            printf("[ OK ] anim_indexed: mismatched per-frame palette rejected\n");
        }
        a.frames[N - 1].image.palette.data = (unsigned char*)pal_rgb; /* restore before free */
    }

    /* apxl_free never touches frame-level .image.palette/.palette_alpha (only
       the anim-level copies, which `a` never sets), so it's safe here even
       though those fields point at static storage -- same as check_anim. */
    apxl_free(&a);
    if (ref) { for (f = 0; f < N; ++f) free(ref[f]); free(ref); }
    return ok;
}

/* apxl_anim_try_index (added 2026-09-18): builds the global palette itself,
   rather than being handed one already matching like check_anim_indexed
   above -- this is the path a real caller (pxltool ca -i) actually uses.
   Two cases: a source under 256 colours converts and round-trips through the
   full apxl_encode/decode/apng_save loop bit-exact; a source with too many
   colours is rejected and left completely untouched. */
static int check_anim_try_index(void)
{
    uint32_t W = 20, H = 15, N = 4, f, x, y;
    uint8_t ch = 4, bpc = 1;
    size_t cb_rgba = (size_t)W * H * ch;
    apxl_anim a, d;
    pxl_buffer enc;
    uint8_t** ref_rgba = NULL; /* original pixels, kept for comparison */
    int ok = 1;

    /* Case 1: a small, deliberately-below-256 palette (9 colours: (x%3,y%3,f)
       triples scaled up, plus a constant alpha). */
    memset(&a, 0, sizeof(a));
    a.frames = (apxl_frame*)calloc(N, sizeof(apxl_frame));
    ref_rgba = (uint8_t**)calloc(N, sizeof(uint8_t*));
    if (!a.frames || !ref_rgba) { printf("[FAIL] anim_try_index: alloc\n"); free(a.frames); free(ref_rgba); return 0; }
    a.frame_count = N; a.loop_count = 0;
    a.canvas_w = W; a.canvas_h = H; a.channels = ch; a.bytes_per_channel = bpc;

    for (f = 0; f < N; ++f) {
        uint8_t* buf = (uint8_t*)malloc(cb_rgba);
        ref_rgba[f] = (uint8_t*)malloc(cb_rgba);
        if (!buf || !ref_rgba[f]) { printf("[FAIL] anim_try_index: frame alloc\n"); ok = 0; break; }
        for (y = 0; y < H; ++y) {
            for (x = 0; x < W; ++x) {
                size_t o = ((size_t)y * W + x) * 4;
                buf[o + 0] = (uint8_t)((x % 3) * 80);
                buf[o + 1] = (uint8_t)((y % 3) * 80);
                buf[o + 2] = (uint8_t)(f * 60);
                buf[o + 3] = 255;
            }
        }
        memcpy(ref_rgba[f], buf, cb_rgba);
        a.frames[f].image.buffer.data = buf;
        a.frames[f].image.buffer.size = cb_rgba;
        a.frames[f].image.width = W; a.frames[f].image.height = H;
        a.frames[f].image.channels = ch; a.frames[f].image.bytes_per_channel = bpc;
        a.frames[f].delay_num = 1; a.frames[f].delay_den = 10;
    }

    if (ok && !apxl_anim_try_index(&a)) {
        printf("[FAIL] anim_try_index: expected conversion (<=256 colours)\n");
        ok = 0;
    }
    if (ok && a.channels != 1) {
        printf("[FAIL] anim_try_index: channels not set to 1 after conversion\n");
        ok = 0;
    }
    /* Every frame's index, looked up through the palette apxl_anim_try_index
       built itself, must reconstruct the exact original RGBA pixels. */
    if (ok) {
        for (f = 0; f < N && ok; ++f) {
            const uint8_t* idx = a.frames[f].image.buffer.data;
            for (y = 0; y < H && ok; ++y) {
                for (x = 0; x < W && ok; ++x) {
                    uint8_t s = idx[y * W + x];
                    const uint8_t* rgb = a.palette.data + (size_t)s * 3;
                    const uint8_t* orig = ref_rgba[f] + ((size_t)y * W + x) * 4;
                    if (rgb[0] != orig[0] || rgb[1] != orig[1] || rgb[2] != orig[2]) {
                        printf("[FAIL] anim_try_index: frame %u (%u,%u) palette lookup mismatch\n",
                               f, x, y);
                        ok = 0;
                    }
                }
            }
        }
    }

    enc = ok ? apxl_encode(&a, 12) : (pxl_buffer){ NULL, 0 };
    if (ok && !enc.data) { printf("[FAIL] anim_try_index: encode\n"); ok = 0; }
    memset(&d, 0, sizeof(d));
    if (ok) {
        d = apxl_decode(enc);
        if (!d.frames || d.frame_count != N) { printf("[FAIL] anim_try_index: decode\n"); ok = 0; }
    }
    if (ok) {
        /* apng_save must now accept an indexed apxl_anim (2026-09-18) and
           reproduce the same pixels through libpng's own PLTE/IDAT reader. */
        char path[] = "pxl_try_index_tmp.apng";
        apxl_anim reloaded;
        memset(&reloaded, 0, sizeof(reloaded));
        if (!apng_save(path, &d)) {
            printf("[FAIL] anim_try_index: apng_save rejected an indexed anim\n");
            ok = 0;
        } else {
            reloaded = apng_load(path);
            remove(path);
            if (!reloaded.frames || reloaded.frame_count != N) {
                printf("[FAIL] anim_try_index: apng_load of the saved file failed\n");
                ok = 0;
            } else {
                for (f = 0; f < N && ok; ++f) {
                    if (memcmp(reloaded.frames[f].image.buffer.data, ref_rgba[f], cb_rgba) != 0) {
                        printf("[FAIL] anim_try_index: frame %u differs after APNG round trip\n", f);
                        ok = 0;
                    }
                }
            }
        }
        apxl_free(&reloaded);
    }
    if (ok) {
        printf("[ OK ] anim_try_index: %u frames %ux%u, auto-built %u-colour palette, "
               "APXL %zu bytes, full APNG round trip lossless\n",
               N, W, H, (unsigned)(a.palette.size / 3), enc.size);
    }
    pxl_free(&enc);
    apxl_free(&a);
    apxl_free(&d);
    if (ref_rgba) { for (f = 0; f < N; ++f) free(ref_rgba[f]); free(ref_rgba); }
    if (!ok) { return 0; }

    /* Case 2: more than 256 colours (a smooth gradient) must be rejected,
       and `a` must come back completely untouched -- still RGBA, same
       pixels, still encodable as a normal (non-indexed) animation. */
    {
        uint32_t W2 = 32, H2 = 32, N2 = 1;
        size_t cb2 = (size_t)W2 * H2 * 4;
        apxl_anim g;
        uint8_t* buf;
        pxl_buffer enc2;

        memset(&g, 0, sizeof(g));
        g.frames = (apxl_frame*)calloc(N2, sizeof(apxl_frame));
        buf = (uint8_t*)malloc(cb2);
        if (!g.frames || !buf) { printf("[FAIL] anim_try_index: gradient alloc\n"); free(g.frames); free(buf); return 0; }
        g.frame_count = N2; g.canvas_w = W2; g.canvas_h = H2;
        g.channels = 4; g.bytes_per_channel = 1;
        for (y = 0; y < H2; ++y) {
            for (x = 0; x < W2; ++x) {
                size_t o = ((size_t)y * W2 + x) * 4;
                /* Every pixel distinct: > 256 colours across a 32x32 image. */
                buf[o + 0] = (uint8_t)x; buf[o + 1] = (uint8_t)y;
                buf[o + 2] = (uint8_t)(x ^ y); buf[o + 3] = 255;
            }
        }
        g.frames[0].image.buffer.data = buf;
        g.frames[0].image.buffer.size = cb2;
        g.frames[0].image.width = W2; g.frames[0].image.height = H2;
        g.frames[0].image.channels = 4; g.frames[0].image.bytes_per_channel = 1;
        g.frames[0].delay_num = 1; g.frames[0].delay_den = 10;

        if (apxl_anim_try_index(&g)) {
            printf("[FAIL] anim_try_index: >256-colour source was not rejected\n");
            ok = 0;
        } else if (g.channels != 4 || g.palette.data != NULL ||
                   g.frames[0].image.buffer.data != buf) {
            /* Pointer identity, not content: proves no reallocation/mutation
               happened at all, a stronger check than comparing a buffer to
               itself would be. */
            printf("[FAIL] anim_try_index: rejected source was not left untouched\n");
            ok = 0;
        } else {
            enc2 = apxl_encode(&g, 12);
            if (!enc2.data) {
                printf("[FAIL] anim_try_index: rejected source no longer encodes as RGBA\n");
                ok = 0;
            } else {
                printf("[ OK ] anim_try_index: >256-colour source rejected and left as RGBA, "
                       "still encodes (%zu bytes)\n", enc2.size);
                pxl_free(&enc2);
            }
        }
        apxl_free(&g);
    }
    return ok;
}

/*----------------------------------------------------------------------------
  GIF (added 2026-09-18, see docs/RESEARCH.md's "Indexed .apxl" entry and
  src/gif.c)
----------------------------------------------------------------------------*/

/* Minimal growbuf and GIF-writing helpers, local to this test: gif_load has
   no symmetric gif_save (this project only ever needs to read GIF), so a
   hand-built byte stream is the only way to check gif_load against exact
   known pixels rather than only against real downloaded files. */
typedef struct { unsigned char* data; size_t size, cap; } gif_test_buf;

static void gtb_put(gif_test_buf* b, const void* src, size_t n)
{
    if (b->size + n > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 64;
        unsigned char* nd;
        while (ncap < b->size + n) { ncap *= 2; }
        nd = (unsigned char*)realloc(b->data, ncap);
        if (!nd) { return; }
        b->data = nd; b->cap = ncap;
    }
    memcpy(b->data + b->size, src, n);
    b->size += n;
}
static void gtb_u8(gif_test_buf* b, unsigned char v) { gtb_put(b, &v, 1); }
static void gtb_le16(gif_test_buf* b, uint16_t v)
{
    unsigned char t[2];
    t[0] = (unsigned char)(v & 0xFF); t[1] = (unsigned char)(v >> 8);
    gtb_put(b, t, 2);
}

/* Encodes `n` index bytes as a valid GIF LZW sub-block stream: Clear, then
   every pixel as its own root code, never a back-reference. gif_lzw_decode's
   dictionary bookkeeping happens the same way regardless of whether entries
   are ever reused, so this is valid (if uncompressed) input -- far simpler
   to get right by hand than a real compressor, and the point here is
   correctness of the decoder, not the size of the fixture. next_code/
   code_size are tracked exactly as the decoder tracks them so the emitted
   bit widths agree with what it expects to read. */
static void write_gif_image_data(gif_test_buf* out, const unsigned char* indices, size_t n,
                                 int min_code_size)
{
    gif_test_buf packed;
    int clear_code = 1 << min_code_size;
    int end_code = clear_code + 1;
    int code_size = min_code_size + 1;
    int next_code = end_code + 1;
    uint32_t bitbuf = 0;
    int bitcount = 0;
    size_t i;
    unsigned char sizebyte;

    memset(&packed, 0, sizeof(packed));

#define GIF_EMIT(code) do { \
    bitbuf |= (uint32_t)(code) << bitcount; bitcount += code_size; \
    while (bitcount >= 8) { \
        unsigned char byte_ = (unsigned char)(bitbuf & 0xFF); \
        gtb_put(&packed, &byte_, 1); \
        bitbuf >>= 8; bitcount -= 8; \
    } \
} while (0)

    GIF_EMIT(clear_code);
    for (i = 0; i < n; ++i) {
        GIF_EMIT(indices[i]);
        /* The decoder defines no new entry for the first code after Clear
           (its prev_code is -1 then); every code after that defines one. */
        if (i > 0 && next_code < 4096) {
            ++next_code;
            if (next_code == (1 << code_size) && code_size < 12) { ++code_size; }
        }
    }
    GIF_EMIT(end_code);
    if (bitcount > 0) { unsigned char byte_ = (unsigned char)(bitbuf & 0xFF); gtb_put(&packed, &byte_, 1); }
#undef GIF_EMIT

    gtb_u8(out, (unsigned char)min_code_size);
    for (i = 0; i < packed.size; ) {
        size_t chunk = packed.size - i;
        if (chunk > 255) { chunk = 255; }
        sizebyte = (unsigned char)chunk;
        gtb_put(out, &sizebyte, 1);
        gtb_put(out, packed.data + i, chunk);
        i += chunk;
    }
    sizebyte = 0;
    gtb_put(out, &sizebyte, 1);
    free(packed.data);
}

static void write_gce(gif_test_buf* b, uint8_t disposal, int transparent_flag,
                      uint8_t transparent_index, uint16_t delay_cs)
{
    unsigned char packed = (unsigned char)(((disposal & 0x07) << 2) | (transparent_flag ? 1 : 0));
    gtb_u8(b, 0x21); gtb_u8(b, 0xF9); gtb_u8(b, 4);
    gtb_u8(b, packed);
    gtb_le16(b, delay_cs);
    gtb_u8(b, transparent_index);
    gtb_u8(b, 0);
}

static void write_gif_frame(gif_test_buf* b, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            const unsigned char* indices, int min_code_size)
{
    gtb_u8(b, 0x2C);
    gtb_le16(b, x); gtb_le16(b, y); gtb_le16(b, w); gtb_le16(b, h);
    gtb_u8(b, 0x00); /* no local colour table, no interlace */
    write_gif_image_data(b, indices, (size_t)w * h, min_code_size);
}

/* Exercises every disposal method and transparency on a hand-built 4x4,
   4-colour GIF -- real corpus files (see check_gif_real below) may never
   happen to use BACKGROUND/PREVIOUS disposal, or a first frame smaller than
   the canvas, at all, so this is the only thing that pins their exact
   compositing semantics down.

   Canvas area no frame has ever drawn to, and whatever a BACKGROUND
   disposal clears, are both transparent -- measured against Chromium's own
   GIF renderer (2026-09-18) after ffmpeg's GIF decoder briefly looked like
   the opposite: ffmpeg fills such area with the Logical Screen Descriptor's
   background colour, matching GIF89a's advisory text literally, but
   Chromium (the rendering that actually matters for "what a GIF looks
   like") renders it transparent regardless of the background colour index,
   on both "a first frame smaller than the canvas" and "a frame's own
   transparent-index pixels" -- ffmpeg is the outlier here, not this
   project's original guess.

   Four frames on a 4x4 canvas, palette {0:black, 1:red, 2:green, 3:blue}:
     A: full canvas red, disposal=NONE.
     B: 2x2 blue at (1,1), disposal=BACKGROUND -- clears that rect to
        transparent for whatever comes next.
     C: 2x2 green at (0,0) but its (1,1) corner is transparent (index 0,
        declared transparent), disposal=PREVIOUS -- undoes C entirely
        afterwards, restoring exactly what B's disposal left.
     D: full canvas blue, disposal=UNSPECIFIED -- the "no GCE-defined
        behaviour" case, which should behave like NONE. */
static int check_gif(void)
{
    gif_test_buf f;
    unsigned char gct[4 * 3] = {
        0, 0, 0,     /* 0: black -- doubles as the transparent index in frame C */
        255, 0, 0,   /* 1: red */
        0, 255, 0,   /* 2: green */
        0, 0, 255,   /* 3: blue */
    };
    unsigned char red_full[16], blue_sq[4], green_with_hole[4], blue_full[16];
    int i;
    apxl_anim a;
    int ok = 1;
    char path[] = "pxl_gif_check_tmp.gif";
    int x, y;

    for (i = 0; i < 16; ++i) { red_full[i] = 1; blue_full[i] = 3; }
    for (i = 0; i < 4; ++i) { blue_sq[i] = 3; }
    green_with_hole[0] = 2; green_with_hole[1] = 2; green_with_hole[2] = 2; green_with_hole[3] = 0;

    memset(&f, 0, sizeof(f));
    gtb_put(&f, "GIF89a", 6);
    gtb_le16(&f, 4); gtb_le16(&f, 4);
    gtb_u8(&f, (unsigned char)(0x80 | 0x01)); /* global colour table, size field 1 -> 4 entries */
    gtb_u8(&f, 0); gtb_u8(&f, 0);
    gtb_put(&f, gct, sizeof(gct));

    /* NETSCAPE2.0 loop extension, loop forever -- exercises that path too. */
    gtb_u8(&f, 0x21); gtb_u8(&f, 0xFF); gtb_u8(&f, 11);
    gtb_put(&f, "NETSCAPE2.0", 11);
    gtb_u8(&f, 3); gtb_u8(&f, 1); gtb_le16(&f, 0);
    gtb_u8(&f, 0);

    write_gce(&f, 1 /* NONE */, 0, 0, 10);
    write_gif_frame(&f, 0, 0, 4, 4, red_full, 2);

    write_gce(&f, 2 /* BACKGROUND */, 0, 0, 20);
    write_gif_frame(&f, 1, 1, 2, 2, blue_sq, 2);

    write_gce(&f, 3 /* PREVIOUS */, 1, 0, 30);
    write_gif_frame(&f, 0, 0, 2, 2, green_with_hole, 2);

    write_gce(&f, 0 /* UNSPECIFIED */, 0, 0, 5);
    write_gif_frame(&f, 0, 0, 4, 4, blue_full, 2);

    gtb_u8(&f, 0x3B);

    {
        FILE* fp = fopen(path, "wb");
        if (!fp || fwrite(f.data, 1, f.size, fp) != f.size) { printf("[FAIL] gif: write tmp\n"); ok = 0; }
        if (fp) { fclose(fp); }
    }
    free(f.data);
    if (!ok) { return 0; }

    a = gif_load(path);
    remove(path);

    if (!a.frames || a.frame_count != 4 || a.canvas_w != 4 || a.canvas_h != 4 ||
        a.channels != 4 || a.bytes_per_channel != 1 || a.loop_count != 0) {
        printf("[FAIL] gif: load shape (frames=%u %ux%u ch=%u loop=%u)\n",
               a.frame_count, a.canvas_w, a.canvas_h, a.channels, a.loop_count);
        apxl_free(&a);
        return 0;
    }

#define GIF_PX(buf, px, py) ((buf) + ((size_t)(py) * 4 + (px)) * 4)
    for (y = 0; y < 4 && ok; ++y) {
        for (x = 0; x < 4 && ok; ++x) {
            const uint8_t* pa = GIF_PX(a.frames[0].image.buffer.data, x, y);
            const uint8_t* pb = GIF_PX(a.frames[1].image.buffer.data, x, y);
            const uint8_t* pc = GIF_PX(a.frames[2].image.buffer.data, x, y);
            const uint8_t* pd = GIF_PX(a.frames[3].image.buffer.data, x, y);
            int inside_b = (x >= 1 && x <= 2 && y >= 1 && y <= 2);

            if (pa[0] != 255 || pa[1] != 0 || pa[2] != 0 || pa[3] != 255) {
                printf("[FAIL] gif: frame A (%d,%d) not red\n", x, y); ok = 0; break;
            }
            if (inside_b) {
                if (pb[0] != 0 || pb[1] != 0 || pb[2] != 255 || pb[3] != 255) {
                    printf("[FAIL] gif: frame B (%d,%d) not blue\n", x, y); ok = 0; break;
                }
            } else if (pb[0] != 255 || pb[1] != 0 || pb[2] != 0 || pb[3] != 255) {
                printf("[FAIL] gif: frame B (%d,%d) not red\n", x, y); ok = 0; break;
            }
            if ((x == 0 && y == 0) || (x == 1 && y == 0) || (x == 0 && y == 1)) {
                if (pc[0] != 0 || pc[1] != 255 || pc[2] != 0 || pc[3] != 255) {
                    printf("[FAIL] gif: frame C (%d,%d) not green\n", x, y); ok = 0; break;
                }
            } else if (inside_b) { /* B's BACKGROUND disposal, never redrawn opaquely by C */
                if (pc[0] != 0 || pc[1] != 0 || pc[2] != 0 || pc[3] != 0) {
                    printf("[FAIL] gif: frame C (%d,%d) not transparent\n", x, y); ok = 0; break;
                }
            } else if (pc[0] != 255 || pc[1] != 0 || pc[2] != 0 || pc[3] != 255) {
                printf("[FAIL] gif: frame C (%d,%d) not red\n", x, y); ok = 0; break;
            }
            if (pd[0] != 0 || pd[1] != 0 || pd[2] != 255 || pd[3] != 255) {
                printf("[FAIL] gif: frame D (%d,%d) not blue\n", x, y); ok = 0; break;
            }
        }
    }
#undef GIF_PX

    if (ok && (a.frames[0].delay_num != 10 || a.frames[1].delay_num != 20 ||
               a.frames[2].delay_num != 30 || a.frames[3].delay_num != 5 ||
               a.frames[0].delay_den != 100)) {
        printf("[FAIL] gif: delays wrong (%u/%u %u/%u %u/%u %u/%u)\n",
               a.frames[0].delay_num, a.frames[0].delay_den, a.frames[1].delay_num, a.frames[1].delay_den,
               a.frames[2].delay_num, a.frames[2].delay_den, a.frames[3].delay_num, a.frames[3].delay_den);
        ok = 0;
    }

    if (ok) {
        printf("[ OK ] gif: 4 frames 4x4, NONE/BACKGROUND/PREVIOUS/UNSPECIFIED disposal "
               "and transparency all composited correctly, infinite loop\n");
    }
    apxl_free(&a);
    return ok;
}

/* Loads a real, freely-licensed GIF (see tests/data/README.md) end to end:
   gif_load -> apxl_anim_try_index -> apxl_encode -> apxl_decode -> apng_save
   -> apng_load, comparing the final RGBA pixels against a copy of
   gif_load's own output saved before apxl_anim_try_index converts it to
   indexed in place. This is the same role real_apng plays for the APNG
   path: proof that a real downloaded file works, not just the hand-built
   shapes in check_gif above. */
static int check_gif_real(const char* gif_path, const char* tmp_apng)
{
    apxl_anim direct, reloaded;
    pxl_buffer enc;
    uint32_t f;
    int ok = 1;
    uint8_t** rgba_before = NULL; /* direct's own pixels, saved before
                                     apxl_anim_try_index mutates it in place */

    direct = gif_load(gif_path);
    if (!direct.frames) {
        printf("[SKIP] gif_real: cannot read '%s'\n", gif_path);
        return 1; /* asset-backed case: absence is not this test's failure */
    }

    rgba_before = (uint8_t**)calloc(direct.frame_count, sizeof(uint8_t*));
    if (!rgba_before) { printf("[FAIL] gif_real: alloc\n"); apxl_free(&direct); return 0; }
    for (f = 0; f < direct.frame_count; ++f) {
        pxl_buffer* b = &direct.frames[f].image.buffer;
        rgba_before[f] = (uint8_t*)malloc(b->size);
        if (!rgba_before[f]) { ok = 0; break; }
        memcpy(rgba_before[f], b->data, b->size);
    }
    if (!ok) {
        for (f = 0; f < direct.frame_count; ++f) { free(rgba_before[f]); }
        free(rgba_before);
        printf("[FAIL] gif_real: alloc\n");
        apxl_free(&direct);
        return 0;
    }

    if (!apxl_anim_try_index(&direct)) {
        printf("[FAIL] gif_real: '%s' composited to more than 256 colours "
               "(expected the committed fixture to fit)\n", gif_path);
        for (f = 0; f < direct.frame_count; ++f) { free(rgba_before[f]); }
        free(rgba_before);
        apxl_free(&direct);
        return 0;
    }

    enc = apxl_encode(&direct, 12);
    if (!enc.data) {
        printf("[FAIL] gif_real: encode\n");
        for (f = 0; f < direct.frame_count; ++f) { free(rgba_before[f]); }
        free(rgba_before);
        apxl_free(&direct);
        return 0;
    }

    memset(&reloaded, 0, sizeof(reloaded));
    {
        apxl_anim decoded = apxl_decode(enc);
        if (!decoded.frames) { printf("[FAIL] gif_real: decode\n"); ok = 0; }
        else if (!apng_save(tmp_apng, &decoded)) {
            printf("[FAIL] gif_real: apng_save of the indexed result\n"); ok = 0;
        } else {
            reloaded = apng_load(tmp_apng);
            remove(tmp_apng);
            if (!reloaded.frames || reloaded.frame_count != direct.frame_count) {
                printf("[FAIL] gif_real: apng_load of the saved file\n"); ok = 0;
            }
        }
        apxl_free(&decoded);
    }

    if (ok) {
        for (f = 0; f < direct.frame_count; ++f) {
            /* Both sides are RGBA8 here: rgba_before was saved before
               apxl_anim_try_index converted `direct` to indexed in place,
               and reloaded went indexed -> RGBA through the real file
               format (apng_load always yields RGBA8). Comparing against a
               re-expansion of `direct` itself would be wrong whenever the
               palette turned out fully opaque -- pxl_image_expand then
               yields 3-channel RGB, not 4-channel RGBA, a stride mismatch
               against reloaded's RGBA that a raw memcmp can't paper over. */
            pxl_buffer* rb = &reloaded.frames[f].image.buffer;
            if (rb->size != (size_t)direct.canvas_w * direct.canvas_h * 4 ||
                memcmp(rgba_before[f], rb->data, rb->size) != 0) {
                printf("[FAIL] gif_real: frame %u differs after the full round trip\n", f);
                ok = 0;
            }
        }
    }

    if (ok) {
        printf("[ OK ] gif_real: %s (%u frames, %ux%u) -> indexed .apxl (%zu bytes) -> "
               "APNG, lossless\n", gif_path, direct.frame_count, direct.canvas_w, direct.canvas_h,
               enc.size);
    }

    for (f = 0; f < direct.frame_count; ++f) { free(rgba_before[f]); }
    free(rgba_before);

    pxl_free(&enc);
    apxl_free(&direct);
    apxl_free(&reloaded);
    return ok;
}

/*----------------------------------------------------------------------------
  Streaming (progressive) decode
----------------------------------------------------------------------------*/

typedef struct {
    uint32_t next_row;  /* the row index we expect next */
    uint32_t seen;      /* how many callbacks fired */
    int      order_ok;  /* rows arrived strictly in order 0,1,2,... */
    size_t   stride;
} stream_probe;

static void stream_cb(void* user, uint32_t row_index,
                      const unsigned char* row, size_t row_bytes)
{
    stream_probe* p = (stream_probe*)user;
    (void)row;
    if (row_index != p->next_row || row_bytes != p->stride) {
        p->order_ok = 0;
    }
    p->next_row = row_index + 1;
    ++p->seen;
}

/* Feed an encoded .pxl through the streaming decoder in fixed-size chunks and
   verify: rows arrive top-to-bottom in order, rows_ready never goes backwards,
   and the final image matches the one-shot pxl_decode byte for byte. */
static int stream_feed_check(const char* name, pxl_buffer enc, size_t chunk,
                            const unsigned char* expect, size_t expect_size,
                            uint32_t height, size_t stride, int expect_progressive)
{
    pxl_stream* s;
    stream_probe probe;
    const pxl_image* img;
    uint32_t ready = 0, last_ready = 0;
    size_t off;
    int mid_rows_seen = 0;
    int ok = 1;

    probe.next_row = 0; probe.seen = 0; probe.order_ok = 1; probe.stride = stride;

    s = pxl_stream_new(stream_cb, &probe);
    if (!s) { printf("[FAIL] stream/%s: alloc\n", name); return 0; }

    for (off = 0; off < enc.size; off += chunk) {
        size_t n = enc.size - off < chunk ? enc.size - off : chunk;
        if (pxl_stream_feed(s, enc.data + off, n) < 0) {
            printf("[FAIL] stream/%s: feed error at offset %zu\n", name, off);
            pxl_stream_free(s);
            return 0;
        }
        img = pxl_stream_image(s, &ready);
        if (ready < last_ready) {
            printf("[FAIL] stream/%s: rows_ready went backwards\n", name);
            ok = 0;
            break;
        }
        /* Rows delivered before the last byte arrived == truly progressive. */
        if (img && ready > 0 && off + n < enc.size) {
            mid_rows_seen = 1;
        }
        last_ready = ready;
    }

    if (ok && !pxl_stream_finish(s)) {
        printf("[FAIL] stream/%s: finish reported incomplete\n", name);
        ok = 0;
    }
    img = pxl_stream_image(s, &ready);
    if (ok && (!img || ready != height)) {
        printf("[FAIL] stream/%s: %u of %u rows ready\n", name, ready, height);
        ok = 0;
    }
    if (ok && (!probe.order_ok || probe.seen != height)) {
        printf("[FAIL] stream/%s: rows out of order or missing (%u of %u)\n",
               name, probe.seen, height);
        ok = 0;
    }
    if (ok && (img->buffer.size != expect_size ||
               memcmp(img->buffer.data, expect, expect_size) != 0)) {
        printf("[FAIL] stream/%s: pixels differ from pxl_decode\n", name);
        ok = 0;
    }
    if (ok && expect_progressive && !mid_rows_seen) {
        printf("[FAIL] stream/%s: no rows delivered before the final byte\n", name);
        ok = 0;
    }

    pxl_stream_free(s);
    return ok;
}

/* Encode an image (optionally with PXL_ENCODE_PROGRESSIVE) and stream-decode it
   with 1-byte and 7-byte chunks. */
static int check_stream_one(const char* name, uint32_t w, uint32_t h,
                           uint8_t channels, uint8_t bpc, unsigned flags,
                           uint8_t expect_not_filter, int low_color,
                           uint8_t expect_filter)
{
    pxl_image src, dec;
    pxl_buffer enc;
    pxl_header hdr;
    size_t size = (size_t)w * h * channels * bpc, i;
    size_t stride = (size_t)w * channels * bpc;
    int progressive;
    int ok = 1;

    memset(&src, 0, sizeof(src));
    src.buffer.data = (unsigned char*)malloc(size);
    src.buffer.size = size;
    src.width = w; src.height = h;
    src.channels = channels; src.bytes_per_channel = bpc;
    if (!src.buffer.data) { printf("[FAIL] stream/%s: alloc\n", name); return 0; }
    if (low_color) {
        /* A handful of flat colors in wide runs: every differencing filter turns
           this into noise, so the encoder picks PXL_FILTER_NONE. */
        static const unsigned char tone[4][4] = {
            { 0x0A, 0x14, 0x1E, 0xFF }, { 0xC8, 0x0A, 0x0A, 0x40 },
            { 0x05, 0xF0, 0x5A, 0x80 }, { 0xFA, 0xFA, 0xFA, 0x10 }
        };
        size_t pxb = (size_t)channels * bpc;
        for (i = 0; i < size; ++i) {
            size_t p = i / pxb, c = (i % pxb) / bpc;
            src.buffer.data[i] = tone[p & 3u][c & 3u];
        }
    } else {
        for (i = 0; i < size; ++i) {
            src.buffer.data[i] = (unsigned char)((i * 91 + (i >> 5) * 13) & 0xFF);
        }
    }

    enc = pxl_encode_ex(&src, 6, flags);
    if (!enc.data || !pxl_header_read(enc.data, enc.size, &hdr)) {
        printf("[FAIL] stream/%s: encode\n", name);
        pxl_image_free(&src); pxl_free(&enc);
        return 0;
    }
    if (expect_not_filter != 0xFF && hdr.color_filter == expect_not_filter) {
        printf("[FAIL] stream/%s: filter %u should have been excluded\n",
               name, hdr.color_filter);
        ok = 0;
    }
    /* Guard the fixture itself: if the encoder stops choosing this filter the
       case silently stops covering the path it was written for. */
    if (expect_filter != 0xFF && hdr.color_filter != expect_filter) {
        printf("[FAIL] stream/%s: expected filter %u, got %u\n",
               name, expect_filter, hdr.color_filter);
        ok = 0;
    }

    dec = pxl_decode(enc);
    if (!dec.buffer.data) {
        printf("[FAIL] stream/%s: reference decode\n", name);
        pxl_image_free(&src); pxl_free(&enc);
        return 0;
    }

    /* Rows become available a zstd block at a time (a block holds at most 128 KB
       of decompressed output), so only images bigger than that are guaranteed to
       deliver rows before the last byte arrives. BCIF is never row-progressive:
       its plane layout means all rows land at finish. */
    progressive = (hdr.color_filter != PXL_FILTER_BCIF) &&
                  (size_t)hdr.raw_byte_count > 256u * 1024u;

    if (ok) ok = stream_feed_check(name, enc, 1, dec.buffer.data, size, h, stride, progressive);
    if (ok) ok = stream_feed_check(name, enc, 7, dec.buffer.data, size, h, stride, progressive);
    if (ok) ok = stream_feed_check(name, enc, enc.size, dec.buffer.data, size, h, stride, 0);

    if (ok) {
        printf("[ OK ] stream/%s: filter %u, %u rows in order, lossless%s\n",
               name, hdr.color_filter, h,
               progressive ? ", rows arrived early" :
               (hdr.color_filter == PXL_FILTER_BCIF ? " (BCIF: rows at finish)"
                                                    : " (single zstd block)"));
    }

    pxl_free(&enc);
    pxl_image_free(&src);
    pxl_image_free(&dec);
    return ok;
}

/* PXL_OUTPUT_* row conversion: collects every delivered row's bytes in
   order, for comparison against hand-computed expected packed values --
   not just "did the stream finish without crashing". */
typedef struct {
    unsigned char* out;
    size_t cap;
    size_t len;
    int overflowed;
} out_collect;

static void collect_row_cb(void* user, uint32_t row_index, const unsigned char* row, size_t row_bytes)
{
    out_collect* c = (out_collect*)user;
    (void)row_index;
    if (c->len + row_bytes > c->cap) { c->overflowed = 1; return; }
    memcpy(c->out + c->len, row, row_bytes);
    c->len += row_bytes;
}

/* Independent restatement of convert_row's bit layout (not a call into it),
   so this catches a wiring bug (wrong shift, wrong byte order, wrong channel
   index) rather than only confirming the implementation agrees with itself. */
static void expected_565(uint8_t r, uint8_t g, uint8_t b, uint8_t out[2])
{
    unsigned v = ((unsigned)(r >> 3) << 11) | ((unsigned)(g >> 2) << 5) | (unsigned)(b >> 3);
    out[0] = (uint8_t)v; out[1] = (uint8_t)(v >> 8);
}
static void expected_5551(uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t out[2])
{
    unsigned v = ((unsigned)(r >> 3) << 11) | ((unsigned)(g >> 3) << 6) |
                 ((unsigned)(b >> 3) << 1) | (unsigned)(a >> 7);
    out[0] = (uint8_t)v; out[1] = (uint8_t)(v >> 8);
}
static void expected_4444(uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t out[2])
{
    unsigned v = ((unsigned)(r >> 4) << 12) | ((unsigned)(g >> 4) << 8) |
                 ((unsigned)(b >> 4) << 4) | (unsigned)(a >> 4);
    out[0] = (uint8_t)v; out[1] = (uint8_t)(v >> 8);
}

/* Encodes a 2x2 RGBA image with deliberately chosen pixels (opaque white and
   black, plus two arbitrary colours that exercise every bit position of the
   3/2/3 and 4/4/4/4 packings), stream-decodes it under every non-native
   output format, and checks the delivered bytes against expected_* exactly. */
static int check_output_format_rgba(void)
{
    static const uint8_t px[4][4] = {
        { 255,255,255,255 }, {   0,  0,  0,  0 },
        { 128, 64, 32,200 }, {  10, 20, 30,128 }
    };
    pxl_image src;
    pxl_buffer enc;
    int fi, ok = 1;
    static const pxl_output_format fmts[] = {
        PXL_OUTPUT_RGBA8888, PXL_OUTPUT_RGB565, PXL_OUTPUT_RGBA5551, PXL_OUTPUT_RGBA4444
    };

    memset(&src, 0, sizeof src);
    src.buffer.size = sizeof px;
    src.buffer.data = (unsigned char*)malloc(src.buffer.size);
    if (!src.buffer.data) { printf("[FAIL] output_format: alloc\n"); return 0; }
    memcpy(src.buffer.data, px, sizeof px);
    src.width = 2; src.height = 2; src.channels = 4; src.bytes_per_channel = 1;

    enc = pxl_encode(&src, 6);
    if (!enc.data) { printf("[FAIL] output_format: encode\n"); pxl_image_free(&src); return 0; }

    for (fi = 0; fi < (int)(sizeof fmts / sizeof fmts[0]); fi++) {
        pxl_output_format fmt = fmts[fi];
        size_t unit = (fmt == PXL_OUTPUT_RGBA8888) ? 4 : 2;
        uint8_t want[16]; /* flat, packed at `unit` bytes/pixel -- no per-pixel
                              padding, matching how the callback's rows are
                              actually laid out (a [4][4] array here would
                              silently pad every pixel to 4 bytes and compare
                              the wrong bytes for the 2-byte formats). */
        uint8_t got_buf[16];
        out_collect c;
        pxl_stream* s;
        int i, fed;

        for (i = 0; i < 4; i++) {
            uint8_t r = px[i][0], g = px[i][1], b = px[i][2], a = px[i][3];
            uint8_t* w = want + (size_t)i * unit;
            if (fmt == PXL_OUTPUT_RGBA8888) {
                w[0]=r; w[1]=g; w[2]=b; w[3]=a;
            } else if (fmt == PXL_OUTPUT_RGB565) {
                expected_565(r, g, b, w);
            } else if (fmt == PXL_OUTPUT_RGBA5551) {
                expected_5551(r, g, b, a, w);
            } else {
                expected_4444(r, g, b, a, w);
            }
        }

        c.out = got_buf; c.cap = sizeof got_buf; c.len = 0; c.overflowed = 0;
        s = pxl_stream_new_ex(collect_row_cb, &c, fmt);
        if (!s) { printf("[FAIL] output_format: stream_new_ex\n"); ok = 0; continue; }
        fed = pxl_stream_feed(s, enc.data, enc.size);
        if (fed < 0 || !pxl_stream_finish(s) || c.overflowed ||
            c.len != (size_t)4 * unit ||
            memcmp(c.out, want, (size_t)4 * unit) != 0) {
            printf("[FAIL] output_format: fmt=%d mismatch (fed=%d len=%zu)\n",
                   (int)fmt, fed, c.len);
            ok = 0;
        } else {
            printf("[ OK ] output_format: fmt=%d, %zu bytes/pixel, bytes match\n",
                   (int)fmt, unit);
        }
        pxl_stream_free(s);
    }

    pxl_free(&enc);
    pxl_image_free(&src);
    return ok;
}

/* A 3-channel (no alpha) source asked for an alpha-carrying format must read
   as fully opaque, not garbage or zero. */
static int check_output_format_rgb_no_alpha(void)
{
    static const uint8_t px[2][3] = { { 200, 100, 50 }, { 0, 0, 0 } };
    pxl_image src;
    pxl_buffer enc;
    uint8_t want[2][2];
    uint8_t got[4];
    out_collect c;
    pxl_stream* s;
    int fed, ok;

    memset(&src, 0, sizeof src);
    src.buffer.size = sizeof px;
    src.buffer.data = (unsigned char*)malloc(src.buffer.size);
    if (!src.buffer.data) { printf("[FAIL] output_format_rgb: alloc\n"); return 0; }
    memcpy(src.buffer.data, px, sizeof px);
    src.width = 2; src.height = 1; src.channels = 3; src.bytes_per_channel = 1;

    enc = pxl_encode(&src, 6);
    if (!enc.data) { printf("[FAIL] output_format_rgb: encode\n"); pxl_image_free(&src); return 0; }

    /* src_channels==3 in convert_row's alpha branch: 1 for 5551 (its alpha is
       a single bit), 0xF for 4444 -- both mean "fully opaque". */
    { unsigned v = ((unsigned)(px[0][0]>>3)<<11)|((unsigned)(px[0][1]>>3)<<6)|((unsigned)(px[0][2]>>3)<<1)|1u;
      want[0][0]=(uint8_t)v; want[0][1]=(uint8_t)(v>>8); }
    { unsigned v = ((unsigned)(px[1][0]>>3)<<11)|((unsigned)(px[1][1]>>3)<<6)|((unsigned)(px[1][2]>>3)<<1)|1u;
      want[1][0]=(uint8_t)v; want[1][1]=(uint8_t)(v>>8); }

    c.out = got; c.cap = sizeof got; c.len = 0; c.overflowed = 0;
    s = pxl_stream_new_ex(collect_row_cb, &c, PXL_OUTPUT_RGBA5551);
    fed = s ? pxl_stream_feed(s, enc.data, enc.size) : -1;
    ok = s && fed >= 0 && pxl_stream_finish(s) && !c.overflowed &&
         c.len == sizeof want && memcmp(c.out, want, sizeof want) == 0;
    if (ok) {
        printf("[ OK ] output_format_rgb: 3-channel source, alpha reads opaque under 5551\n");
    } else {
        printf("[FAIL] output_format_rgb: 3-channel source alpha handling\n");
    }
    if (s) { pxl_stream_free(s); }
    pxl_free(&enc);
    pxl_image_free(&src);
    return ok;
}

/* Requesting a packed output format on geometry it is not defined for
   (anything but 8-bit, non-indexed, 3/4-channel) must fail once the header
   says so, the same way any other geometry mismatch is reported. */
static int check_output_format_rejects_bad_geometry(void)
{
    pxl_image src;
    pxl_buffer enc;
    pxl_stream* s;
    int fed, ok;
    uint8_t px[4] = { 10, 20, 30, 40 }; /* 2x2 8-bit GRAY, 1 channel */

    memset(&src, 0, sizeof src);
    src.buffer.size = sizeof px;
    src.buffer.data = (unsigned char*)malloc(src.buffer.size);
    memcpy(src.buffer.data, px, sizeof px);
    src.width = 2; src.height = 2; src.channels = 1; src.bytes_per_channel = 1;

    enc = pxl_encode(&src, 6);
    if (!enc.data) { printf("[FAIL] output_format_reject: encode\n"); pxl_image_free(&src); return 0; }

    s = pxl_stream_new_ex(NULL, NULL, PXL_OUTPUT_RGB565);
    fed = s ? pxl_stream_feed(s, enc.data, enc.size) : -1;
    ok = (fed == -1);
    if (ok) {
        printf("[ OK ] output_format_reject: 1-channel source + RGB565 correctly rejected\n");
    } else {
        printf("[FAIL] output_format_reject: should have failed, fed=%d\n", fed);
    }
    if (s) { pxl_stream_free(s); }
    pxl_free(&enc);
    pxl_image_free(&src);
    return ok;
}

/* pxl_convert_palette: a 4-entry palette (with alpha), converted under
   RGB565 (alpha ignored) and RGBA5551 (alpha used), checked against
   expected_565/expected_5551 -- the same independent-of-convert_row
   reference the row-conversion tests above already use, since a palette
   entry and a pixel go through the exact same bit math. Does not need an
   encoded file at all: pxl_convert_palette reads img->palette/palette_alpha
   directly, which a hand-built pxl_image already has. */
static int check_convert_palette(void)
{
    static const uint8_t pal[4 * 3] = {
        255,255,255,  0,0,0,  128,64,32,  10,20,30
    };
    static const uint8_t alpha[4] = { 255, 0, 200, 128 };
    pxl_image img;
    uint8_t want[8], got[8];
    size_t n;
    int i, ok = 1;

    memset(&img, 0, sizeof img);
    img.palette.data = (unsigned char*)pal;
    img.palette.size = sizeof pal;

    n = pxl_convert_palette(&img, PXL_OUTPUT_RGB565, got);
    for (i = 0; i < 4; i++) {
        expected_565(pal[i*3+0], pal[i*3+1], pal[i*3+2], want + i*2);
    }
    if (n != 8 || memcmp(got, want, 8) != 0) {
        printf("[FAIL] convert_palette: RGB565 mismatch (n=%zu)\n", n);
        ok = 0;
    } else {
        printf("[ OK ] convert_palette: RGB565, 4 entries, no alpha buffer\n");
    }

    img.palette_alpha.data = (unsigned char*)alpha;
    img.palette_alpha.size = sizeof alpha;
    n = pxl_convert_palette(&img, PXL_OUTPUT_RGBA5551, got);
    for (i = 0; i < 4; i++) {
        expected_5551(pal[i*3+0], pal[i*3+1], pal[i*3+2], alpha[i], want + i*2);
    }
    if (n != 8 || memcmp(got, want, 8) != 0) {
        printf("[FAIL] convert_palette: RGBA5551 mismatch (n=%zu)\n", n);
        ok = 0;
    } else {
        printf("[ OK ] convert_palette: RGBA5551, 4 entries, alpha used\n");
    }

    if (pxl_convert_palette(&img, PXL_OUTPUT_NATIVE, got) != 0) {
        printf("[FAIL] convert_palette: PXL_OUTPUT_NATIVE should be rejected\n");
        ok = 0;
    } else {
        printf("[ OK ] convert_palette: PXL_OUTPUT_NATIVE correctly rejected\n");
    }

    img.palette.size = 0; /* no longer indexed */
    if (pxl_convert_palette(&img, PXL_OUTPUT_RGB565, got) != 0) {
        printf("[FAIL] convert_palette: non-indexed image should be rejected\n");
        ok = 0;
    } else {
        printf("[ OK ] convert_palette: non-indexed image correctly rejected\n");
    }

    return ok;
}

/* Truncated and corrupt inputs must be rejected, not silently accepted. */
static int check_stream_errors(void)
{
    pxl_image src;
    pxl_buffer enc;
    pxl_stream* s;
    size_t size = 64 * 32 * 3, i;
    int ok = 1;

    memset(&src, 0, sizeof(src));
    src.buffer.data = (unsigned char*)malloc(size);
    src.buffer.size = size;
    src.width = 64; src.height = 32; src.channels = 3; src.bytes_per_channel = 1;
    if (!src.buffer.data) { printf("[FAIL] stream_errors: alloc\n"); return 0; }
    for (i = 0; i < size; ++i) src.buffer.data[i] = (unsigned char)(i * 5);

    enc = pxl_encode_ex(&src, 6, PXL_ENCODE_PROGRESSIVE);
    if (!enc.data) { printf("[FAIL] stream_errors: encode\n"); pxl_image_free(&src); return 0; }

    /* Truncated file: finish must refuse. */
    s = pxl_stream_new(NULL, NULL);
    if (pxl_stream_feed(s, enc.data, enc.size - 4) < 0) {
        printf("[FAIL] stream_errors: unexpected error on partial feed\n");
        ok = 0;
    } else if (pxl_stream_finish(s)) {
        printf("[FAIL] stream_errors: truncated file accepted\n");
        ok = 0;
    }
    pxl_stream_free(s);

    /* Bad magic: feed must fail as soon as the header is complete. */
    if (ok) {
        unsigned char bad[PXL_HEADER_BYTES];
        memcpy(bad, enc.data, PXL_HEADER_BYTES);
        bad[0] = 'X';
        s = pxl_stream_new(NULL, NULL);
        if (pxl_stream_feed(s, bad, PXL_HEADER_BYTES) != -1) {
            printf("[FAIL] stream_errors: bad magic accepted\n");
            ok = 0;
        }
        pxl_stream_free(s);
    }

    /* Corrupt compressed payload: must be detected, not decoded. */
    if (ok) {
        unsigned char* copy = (unsigned char*)malloc(enc.size);
        if (copy) {
            memcpy(copy, enc.data, enc.size);
            copy[enc.size - 1] ^= 0xFF;
            copy[enc.size / 2] ^= 0x5A;
            s = pxl_stream_new(NULL, NULL);
            if (pxl_stream_feed(s, copy, enc.size) >= 0 && pxl_stream_finish(s)) {
                printf("[FAIL] stream_errors: corrupt frame accepted\n");
                ok = 0;
            }
            pxl_stream_free(s);
            free(copy);
        }
    }

    if (ok) printf("[ OK ] stream_errors: truncated/bad-magic/corrupt all rejected\n");

    pxl_free(&enc);
    pxl_image_free(&src);
    return ok;
}

/* A crafted header claiming BCIF with a geometry BCIF does not support (BCIF is
   8-bit RGB/RGBA only) used to reach the 4-plane unpack path and read past the
   pixel buffer. Both decoders must reject it instead. */
static int check_bad_filter_geometry(void)
{
    pxl_image src, dec;
    pxl_buffer enc, bad;
    unsigned char* copy;
    pxl_stream* s;
    size_t size = 64 * 40 * 2, i;
    int ok = 1;

    memset(&src, 0, sizeof(src));
    src.buffer.data = (unsigned char*)malloc(size);
    src.buffer.size = size;
    src.width = 64; src.height = 40;
    src.channels = 1; src.bytes_per_channel = 2;  /* pixel_bytes == 2 */
    if (!src.buffer.data) { printf("[FAIL] bad_geometry: alloc\n"); return 0; }
    for (i = 0; i < size; ++i) src.buffer.data[i] = (unsigned char)(i * 11);

    enc = pxl_encode(&src, 3);
    if (!enc.data) {
        printf("[FAIL] bad_geometry: encode\n");
        pxl_image_free(&src);
        return 0;
    }

    copy = (unsigned char*)malloc(enc.size);
    if (!copy) {
        printf("[FAIL] bad_geometry: alloc\n");
        pxl_free(&enc); pxl_image_free(&src);
        return 0;
    }
    memcpy(copy, enc.data, enc.size);
    copy[7] = PXL_FILTER_BCIF;   /* claim BCIF on 16-bit gray */
    bad.data = copy; bad.size = enc.size;

    dec = pxl_decode(bad);
    if (dec.buffer.data) {
        printf("[FAIL] bad_geometry: pxl_decode accepted BCIF on 16-bit gray\n");
        ok = 0;
        pxl_image_free(&dec);
    }

    s = pxl_stream_new(NULL, NULL);
    if (pxl_stream_feed(s, copy, enc.size) != -1 || pxl_stream_finish(s)) {
        printf("[FAIL] bad_geometry: streaming decoder accepted it\n");
        ok = 0;
    }
    pxl_stream_free(s);

    if (ok) printf("[ OK ] bad_geometry: BCIF with unsupported geometry rejected\n");

    free(copy);
    pxl_free(&enc);
    pxl_image_free(&src);
    return ok;
}

/* Round-trip a real PNG file (not a synthetic buffer): load with libpng, encode,
   decode, stream-decode, and re-save. Synthetic images miss whatever a real
   encoder emits -- gamma chunks, palette-derived colors, actual filter choices.
   The asset is a public-domain color test chart from Wikimedia Commons. */
static int check_real_png(const char* path, const char* tmp_png)
{
    pxl_image src, dec, reloaded;
    pxl_buffer enc;
    pxl_stream* s;
    const pxl_image* streamed;
    uint32_t ready = 0;
    int ok = 1;

    src = pxl_load_png(path);
    if (!src.buffer.data) {
        printf("[SKIP] real_png: cannot read '%s'\n", path);
        return 1; /* asset missing is not a code failure */
    }

    enc = pxl_encode_ex(&src, 12, PXL_ENCODE_PROGRESSIVE);
    if (!enc.data) {
        printf("[FAIL] real_png: encode\n");
        pxl_image_free(&src);
        return 0;
    }

    dec = pxl_decode(enc);
    if (!dec.buffer.data || dec.buffer.size != src.buffer.size ||
        memcmp(dec.buffer.data, src.buffer.data, src.buffer.size) != 0) {
        printf("[FAIL] real_png: pixels differ after codec round-trip\n");
        ok = 0;
    }

    /* Same file through the streaming decoder, in awkward 13-byte chunks. */
    if (ok) {
        size_t off;
        s = pxl_stream_new(NULL, NULL);
        for (off = 0; off < enc.size; off += 13) {
            size_t n = enc.size - off < 13 ? enc.size - off : 13;
            if (pxl_stream_feed(s, enc.data + off, n) < 0) { break; }
        }
        if (!pxl_stream_finish(s)) {
            printf("[FAIL] real_png: streaming decode incomplete\n");
            ok = 0;
        } else {
            streamed = pxl_stream_image(s, &ready);
            if (!streamed || ready != src.height ||
                memcmp(streamed->buffer.data, src.buffer.data, src.buffer.size) != 0) {
                printf("[FAIL] real_png: streamed pixels differ\n");
                ok = 0;
            }
        }
        pxl_stream_free(s);
    }

    /* And back out to PNG, then in again -- exercises metadata re-injection. */
    if (ok) {
        if (!pxl_save_png(tmp_png, &dec)) {
            printf("[FAIL] real_png: save_png\n");
            ok = 0;
        } else {
            reloaded = pxl_load_png(tmp_png);
            if (!reloaded.buffer.data ||
                reloaded.buffer.size != src.buffer.size ||
                memcmp(reloaded.buffer.data, src.buffer.data, src.buffer.size) != 0) {
                printf("[FAIL] real_png: pixels differ after PNG re-save\n");
                ok = 0;
            }
            pxl_image_free(&reloaded);
            remove(tmp_png);
        }
    }

    if (ok) {
        printf("[ OK ] real_png: %ux%u c=%u, %zu -> %zu bytes, lossless via file+stream\n",
               src.width, src.height, src.channels, src.buffer.size, enc.size);
    }

    pxl_free(&enc);
    pxl_image_free(&src);
    pxl_image_free(&dec);
    return ok;
}

/* Ancillary chunks must survive APNG -> .apxl -> APNG, byte for byte. The
   container has always had a metadata field, but the animated front end did not
   fill it, so EXIF/ICC were silently dropped on the .apxl path -- this is the
   test that would have caught that.

   Builds its own APNG by splicing chunks into a real one rather than committing
   a second asset: the point is control over exactly which chunks are present. */
static int check_apng_metadata(const char* src_apng, const char* tmp_apng)
{
    static const char* kept[] = { "gAMA", "cHRM", "eXIf", "tEXt", NULL };
    apxl_anim a, b;
    pxl_buffer enc;
    int ok = 1, i;

    a = apng_load(src_apng);
    if (!a.frames || a.frame_count == 0) {
        printf("[SKIP] apng_metadata: cannot read '%s'\n", src_apng);
        return 1;
    }

    /* Synthesize a metadata block in the container's record format:
       type[4] | length[4, LE] | data. */
    {
        static const unsigned char payload[] = { 0xDE, 0xAD, 0xBE, 0xEF };
        size_t n = 0, need = 0;
        unsigned char* m;
        for (i = 0; kept[i]; ++i) { need += 8 + sizeof(payload); }
        m = (unsigned char*)malloc(need);
        if (!m) { printf("[FAIL] apng_metadata: alloc\n"); apxl_free(&a); return 0; }
        for (i = 0; kept[i]; ++i) {
            memcpy(m + n, kept[i], 4);
            m[n+4] = (unsigned char)sizeof(payload);
            m[n+5] = m[n+6] = m[n+7] = 0;
            memcpy(m + n + 8, payload, sizeof(payload));
            n += 8 + sizeof(payload);
        }
        pxl_free(&a.metadata);
        a.metadata.data = m;
        a.metadata.size = n;
    }

    enc = apxl_encode(&a, APXL_LEVEL_DEFAULT);
    if (!enc.data) {
        printf("[FAIL] apng_metadata: encode\n");
        apxl_free(&a);
        return 0;
    }

    b = apxl_decode(enc);
    if (!b.frames) {
        printf("[FAIL] apng_metadata: decode\n");
        ok = 0;
    } else if (b.metadata.size != a.metadata.size ||
               memcmp(b.metadata.data, a.metadata.data, a.metadata.size) != 0) {
        printf("[FAIL] apng_metadata: block not preserved through .apxl (%zu vs %zu)\n",
               b.metadata.size, a.metadata.size);
        ok = 0;
    }

    /* Now out to a real APNG file and back: proves apng_save emits the chunks
       and apng_load finds them again, which is the part FFmpeg-independent
       front ends rely on. */
    if (ok) {
        apxl_anim c;
        if (!apng_save(tmp_apng, &b)) {
            printf("[FAIL] apng_metadata: apng_save\n");
            ok = 0;
        } else {
            c = apng_load(tmp_apng);
            if (!c.frames) {
                printf("[FAIL] apng_metadata: reload\n");
                ok = 0;
            } else {
                for (i = 0; kept[i]; ++i) {
                    /* Search the reloaded block for each type we injected. */
                    size_t p = 0;
                    int found = 0;
                    while (p + 8 <= c.metadata.size) {
                        uint32_t len = (uint32_t)c.metadata.data[p+4] |
                                       ((uint32_t)c.metadata.data[p+5] << 8) |
                                       ((uint32_t)c.metadata.data[p+6] << 16) |
                                       ((uint32_t)c.metadata.data[p+7] << 24);
                        if (memcmp(c.metadata.data + p, kept[i], 4) == 0) { found = 1; break; }
                        if (len > c.metadata.size - p - 8) { break; }
                        p += 8 + len;
                    }
                    if (!found) {
                        printf("[FAIL] apng_metadata: %s lost through the APNG file\n", kept[i]);
                        ok = 0;
                    }
                }
                apxl_free(&c);
            }
            remove(tmp_apng);
        }
    }

    if (ok) {
        printf("[ OK ] apng_metadata: %zu-byte block survives APNG -> apxl -> APNG\n",
               a.metadata.size);
    }

    pxl_free(&enc);
    apxl_free(&a);
    apxl_free(&b);
    return ok;
}

/* Full APNG interop on a real file: composite it, encode, decode, and compare
   every frame. check_anim covers the same path on synthetic frames, but only a
   real APNG exercises the chunk parser -- dispose/blend ops, per-frame offsets
   and sub-rectangles, which synthetic full-canvas frames never produce. */
static int check_real_apng(const char* path)
{
    apxl_anim src, dec;
    pxl_buffer enc;
    uint32_t i;
    int ok = 1;

    src = apng_load(path);
    if (!src.frames || src.frame_count == 0) {
        printf("[SKIP] real_apng: cannot read '%s'\n", path);
        return 1; /* asset missing is not a code failure */
    }

    enc = apxl_encode(&src, APXL_LEVEL_DEFAULT);
    if (!enc.data) {
        printf("[FAIL] real_apng: encode\n");
        apxl_free(&src);
        return 0;
    }

    dec = apxl_decode(enc);
    if (!dec.frames || dec.frame_count != src.frame_count) {
        printf("[FAIL] real_apng: frame count %u != %u\n",
               dec.frames ? dec.frame_count : 0, src.frame_count);
        ok = 0;
    } else {
        for (i = 0; i < src.frame_count; ++i) {
            if (dec.frames[i].image.buffer.size != src.frames[i].image.buffer.size ||
                memcmp(dec.frames[i].image.buffer.data,
                       src.frames[i].image.buffer.data,
                       src.frames[i].image.buffer.size) != 0) {
                printf("[FAIL] real_apng: frame %u differs\n", i);
                ok = 0;
                break;
            }
        }
    }

    if (ok) {
        printf("[ OK ] real_apng: %u frames %ux%u, %zu -> %zu bytes, lossless\n",
               src.frame_count, src.canvas_w, src.canvas_h,
               (size_t)src.frame_count * src.frames[0].image.buffer.size, enc.size);
    }

    pxl_free(&enc);
    apxl_free(&src);
    apxl_free(&dec);
    return ok;
}

/* Read a whole file into a pxl_buffer. Returns data == NULL if unreadable. */
static pxl_buffer read_file(const char* path)
{
    pxl_buffer b;
    FILE* f;
    long n;
    b.data = NULL; b.size = 0;
    f = fopen(path, "rb");
    if (!f) { return b; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return b; }
    n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return b; }
    b.data = malloc((size_t)n);
    if (!b.data) { fclose(f); return b; }
    if (fread(b.data, 1, (size_t)n, f) != (size_t)n) {
        free(b.data); b.data = NULL;
    } else {
        b.size = (size_t)n;
    }
    fclose(f);
    return b;
}

/* A committed .pxl produced by an earlier build must still decode to exactly
   the pixels of its source PNG.
   Deliberately not a byte comparison of the .pxl itself: the container embeds a
   zstd frame, and a zstd upgrade (which the rebuild-on-upstream workflow
   performs automatically) legitimately changes those bytes without changing the
   format. What must never change is what a decoder reconstructs. That makes
   this the regression test the round-trip cases cannot be: they only prove the
   encoder agrees with itself, this proves today's decoder agrees with
   yesterday's encoder. */
static int check_reference_pxl(const char* pxl_path, const char* png_path)
{
    pxl_buffer file;
    pxl_image dec, src;
    int ok = 1;

    file = read_file(pxl_path);
    if (!file.data) {
        printf("[SKIP] ref_pxl: cannot read '%s'\n", pxl_path);
        return 1; /* asset missing is not a code failure */
    }
    src = pxl_load_png(png_path);
    if (!src.buffer.data) {
        printf("[SKIP] ref_pxl: cannot read '%s'\n", png_path);
        free(file.data);
        return 1;
    }

    dec = pxl_decode(file);
    if (!dec.buffer.data) {
        printf("[FAIL] ref_pxl: decode failed -- format regression?\n");
        ok = 0;
    } else if (dec.width != src.width || dec.height != src.height ||
               dec.channels != src.channels ||
               dec.bytes_per_channel != src.bytes_per_channel) {
        printf("[FAIL] ref_pxl: geometry differs from source PNG\n");
        ok = 0;
    } else if (dec.buffer.size != src.buffer.size ||
               memcmp(dec.buffer.data, src.buffer.data, src.buffer.size) != 0) {
        printf("[FAIL] ref_pxl: pixels differ from source PNG\n");
        ok = 0;
    }

    if (ok) {
        printf("[ OK ] ref_pxl: %s decodes to the source pixels (%ux%u c=%u)\n",
               pxl_path, dec.width, dec.height, dec.channels);
    }
    free(file.data);
    pxl_image_free(&dec);
    pxl_image_free(&src);
    return ok;
}

/* Same contract for animation: a committed .apxl must still decode frame for
   frame, pixel for pixel, to what its source APNG composites to. */
static int check_reference_apxl(const char* apxl_path, const char* apng_path)
{
    pxl_buffer file;
    apxl_anim dec, src;
    uint32_t i;
    int ok = 1;

    file = read_file(apxl_path);
    if (!file.data) {
        printf("[SKIP] ref_apxl: cannot read '%s'\n", apxl_path);
        return 1;
    }
    src = apng_load(apng_path);
    if (!src.frames) {
        printf("[SKIP] ref_apxl: cannot read '%s'\n", apng_path);
        free(file.data);
        return 1;
    }

    dec = apxl_decode(file);
    if (!dec.frames) {
        printf("[FAIL] ref_apxl: decode failed -- format regression?\n");
        ok = 0;
    } else if (dec.frame_count != src.frame_count ||
               dec.canvas_w != src.canvas_w || dec.canvas_h != src.canvas_h) {
        printf("[FAIL] ref_apxl: canvas/frame count differs from source APNG\n");
        ok = 0;
    } else {
        for (i = 0; i < dec.frame_count; ++i) {
            const pxl_image* a = &dec.frames[i].image;
            const pxl_image* b = &src.frames[i].image;
            if (a->buffer.size != b->buffer.size ||
                memcmp(a->buffer.data, b->buffer.data, b->buffer.size) != 0) {
                printf("[FAIL] ref_apxl: frame %u pixels differ\n", i);
                ok = 0;
                break;
            }
            if (dec.frames[i].delay_num != src.frames[i].delay_num ||
                dec.frames[i].delay_den != src.frames[i].delay_den) {
                printf("[FAIL] ref_apxl: frame %u timing differs\n", i);
                ok = 0;
                break;
            }
        }
    }

    if (ok) {
        printf("[ OK ] ref_apxl: %s decodes to the source frames (%u frames, %ux%u)\n",
               apxl_path, dec.frame_count, dec.canvas_w, dec.canvas_h);
    }
    free(file.data);
    apxl_free(&dec);
    apxl_free(&src);
    return ok;
}

int main(int argc, char** argv)
{
    int failures = 0;
    const char* tmp_png = (argc > 1) ? argv[1] : "pxl_interop_tmp.png";
    /* tests/data, passed by CMake so the test runs from any build directory.
       Without it the asset-backed cases report SKIP rather than fail. */
    const char* data_dir = (argc > 2) ? argv[2] : NULL;
    char real_png[512], ref_pxl[512], ref_apng[512], ref_apxl[512], real_gif[512];
    /* Scratch APNG, written next to tmp_png so it lands in the build directory
       rather than the source tree. Removed by the case that uses it. */
    char tmp_apng[560], tmp_gif_apng[560];

    snprintf(tmp_apng, sizeof(tmp_apng), "%s.tmp.apng", tmp_png);
    snprintf(tmp_gif_apng, sizeof(tmp_gif_apng), "%s.tmp.gif.apng", tmp_png);

    if (data_dir) {
        snprintf(real_png, sizeof(real_png),
                 "%s/RGB_24bits_palette_color_test_chart.png", data_dir);
        snprintf(ref_pxl, sizeof(ref_pxl),
                 "%s/RGB_24bits_palette_color_test_chart.pxl", data_dir);
        snprintf(ref_apng, sizeof(ref_apng),
                 "%s/Animated_PNG_example_bouncing_beach_ball.apng", data_dir);
        snprintf(ref_apxl, sizeof(ref_apxl),
                 "%s/Animated_PNG_example_bouncing_beach_ball.apxl", data_dir);
        snprintf(real_gif, sizeof(real_gif), "%s/LittleRunner.gif", data_dir);
    }

    failures += !check_roundtrip("gray8",  100, 80, 1, 1);
    failures += !check_roundtrip("ga8",     64, 64, 2, 1);
    failures += !check_roundtrip("rgb8",   128, 96, 3, 1);
    failures += !check_roundtrip("rgba8",  128, 96, 4, 1);
    failures += !check_roundtrip("rgb16",   40, 40, 3, 2);
    failures += !check_roundtrip("gray16",  33, 17, 1, 2);
    failures += !check_roundtrip("one_px",   1,  1, 4, 1);

    failures += !check_png_interop(tmp_png);
    failures += !check_metadata();
    failures += !check_adaptive();
    failures += !check_fast_decode_excludes_adaptive();
    failures += !check_anim();
    failures += !check_anim_indexed();
    failures += !check_anim_try_index();

    /* Progressive encode must never pick BCIF, and must stream row by row. */
    failures += !check_stream_one("gray8_prog",  100, 80, 1, 1,
                                  PXL_ENCODE_PROGRESSIVE, PXL_FILTER_BCIF, 0, 0xFF);
    failures += !check_stream_one("rgb8_prog",   128, 96, 3, 1,
                                  PXL_ENCODE_PROGRESSIVE, PXL_FILTER_BCIF, 0, 0xFF);
    failures += !check_stream_one("rgba8_prog",  128, 96, 4, 1,
                                  PXL_ENCODE_PROGRESSIVE, PXL_FILTER_BCIF, 0, 0xFF);
    failures += !check_stream_one("rgb16_prog",   40, 40, 3, 2,
                                  PXL_ENCODE_PROGRESSIVE, PXL_FILTER_BCIF, 0, 0xFF);
    /* Larger than one zstd block (>256 KB filtered), so rows MUST arrive before
       the last byte -- this is the actual progressive-loading guarantee. */
    failures += !check_stream_one("rgb8_big_prog", 512, 400, 3, 1,
                                  PXL_ENCODE_PROGRESSIVE, PXL_FILTER_BCIF, 0, 0xFF);
    /* Fast-decode encode must also never pick BCIF (same fixtures as above,
       different flag -- ADAPTIVE exclusion is covered separately since none
       of these fixtures select it in the first place). */
    failures += !check_stream_one("gray8_fast",  100, 80, 1, 1,
                                  PXL_ENCODE_FAST_DECODE, PXL_FILTER_BCIF, 0, 0xFF);
    failures += !check_stream_one("rgb8_fast",   128, 96, 3, 1,
                                  PXL_ENCODE_FAST_DECODE, PXL_FILTER_BCIF, 0, 0xFF);
    failures += !check_stream_one("rgba8_fast",  128, 96, 4, 1,
                                  PXL_ENCODE_FAST_DECODE, PXL_FILTER_BCIF, 0, 0xFF);
    /* Flat-color content makes PXL_FILTER_NONE win, which is the only way to
       exercise the unfiltered row path in the streaming decoder. */
    failures += !check_stream_one("rgb8_none",   128, 96, 3, 1,
                                  0, 0xFF, 1, PXL_FILTER_NONE);
    failures += !check_stream_one("rgb8_big_none", 512, 400, 3, 1,
                                  0, 0xFF, 1, PXL_FILTER_NONE);
    /* Default encode: whatever filter wins (possibly BCIF) must still stream. */
    failures += !check_stream_one("rgb8_any",    128, 96, 3, 1, 0, 0xFF, 0, 0xFF);
    failures += !check_stream_one("rgba8_any",   128, 96, 4, 1, 0, 0xFF, 0, 0xFF);
    failures += !check_stream_one("one_px",        1,  1, 4, 1, 0, 0xFF, 0, 0xFF);
    failures += !check_output_format_rgba();
    failures += !check_output_format_rgb_no_alpha();
    failures += !check_output_format_rejects_bad_geometry();
    failures += !check_convert_palette();
    failures += !check_stream_errors();
    failures += !check_bad_filter_geometry();
    failures += !check_gif();

    if (data_dir) {
        failures += !check_real_png(real_png, tmp_png);
        failures += !check_real_apng(ref_apng);
        failures += !check_apng_metadata(ref_apng, tmp_apng);
        /* Committed .pxl/.apxl from an earlier build must still decode to the
           same pixels -- catches an accidental format change. */
        failures += !check_reference_pxl(ref_pxl, real_png);
        failures += !check_reference_apxl(ref_apxl, ref_apng);
        failures += !check_gif_real(real_gif, tmp_gif_apng);
    }

    if (failures) {
        printf("\n%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("\nAll tests passed\n");
    return 0;
}

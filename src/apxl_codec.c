/** \file apxl_codec.c
    \brief Animated PNG XL codec (v1): all full-canvas frames concatenated into
           one zstd stream with long-distance matching.

    Rationale (measured on real animations): compressing every frame together in
    a single zstd stream lets the compressor reuse the large redundancy between
    frames. Per-frame streams, temporal deltas, and per-frame filtering all did
    worse because they break cross-frame byte matches.
*/
#include "apxl.h"
#include "apxl_format.h"
#include "pxl_bytes.h"

#include <zstd.h>

#include <stdlib.h>
#include <string.h>

/* Enable zstd long-distance matching at/above this level, so cross-frame
   matches beyond the default window are found. */
#define APXL_LDM_MIN_LEVEL 10
#define APXL_WINDOW_LOG     27  /* 128 MiB match window */

/* Sanity limits on canvas geometry and frame count, so a crafted header cannot
   wrap the size_t arithmetic behind the frame allocations. Mirrors
   PXL_MAX_DIM/PXL_MAX_PIXELS in pxl_codec.c. */
#define APXL_MAX_DIM      1000000u
#define APXL_MAX_PIXELS   ((uint64_t)1 << 28)
#define APXL_MAX_FRAMES   1000000u

/* Returns 1 if the canvas geometry and frame count are within decode limits.
   pb is 1..8, so pixels <= 2^28 keeps canvas_bytes <= 2^31, and frames <= 2^20
   keeps canvas_bytes*frame_count <= 2^51: both clear of size_t overflow. */
static int apxl_geometry_ok(uint32_t w, uint32_t h, uint32_t frames)
{
    if (w == 0 || h == 0 || frames == 0) { return 0; }
    if (w > APXL_MAX_DIM || h > APXL_MAX_DIM) { return 0; }
    if ((uint64_t)w * h > APXL_MAX_PIXELS) { return 0; }
    if (frames > APXL_MAX_FRAMES) { return 0; }
    return 1;
}

void apxl_header_write(unsigned char* out, const apxl_file_header* h)
{
    out[0] = APXL_MAGIC0; out[1] = APXL_MAGIC1;
    out[2] = APXL_MAGIC2; out[3] = APXL_MAGIC3;
    out[4] = h->version;
    out[5] = h->channels;
    out[6] = h->bytes_per_channel;
    out[7] = h->flags;
    pxl_put_le32(out + 8,  h->canvas_w);
    pxl_put_le32(out + 12, h->canvas_h);
    pxl_put_le32(out + 16, h->frame_count);
    pxl_put_le32(out + 20, h->loop_count);
    pxl_put_le32(out + 24, h->meta_byte_count);
    pxl_put_le32(out + 28, h->raw_byte_count);
}

int apxl_header_read(const unsigned char* in, size_t in_size, apxl_file_header* h)
{
    if (in_size < APXL_HEADER_BYTES) { return 0; }
    if (in[0] != APXL_MAGIC0 || in[1] != APXL_MAGIC1 ||
        in[2] != APXL_MAGIC2 || in[3] != APXL_MAGIC3) { return 0; }
    h->version           = in[4];
    h->channels          = in[5];
    h->bytes_per_channel = in[6];
    h->flags             = in[7];
    h->canvas_w          = pxl_get_le32(in + 8);
    h->canvas_h          = pxl_get_le32(in + 12);
    h->frame_count       = pxl_get_le32(in + 16);
    h->loop_count        = pxl_get_le32(in + 20);
    h->meta_byte_count   = pxl_get_le32(in + 24);
    h->raw_byte_count    = pxl_get_le32(in + 28);
    if (h->version != APXL_VERSION) { return 0; }
    if (h->channels < 1 || h->channels > 4) { return 0; }
    if (h->bytes_per_channel != 1 && h->bytes_per_channel != 2) { return 0; }
    if (!apxl_geometry_ok(h->canvas_w, h->canvas_h, h->frame_count)) { return 0; }
    return 1;
}

/*----------------------------------------------------------------------------
  Encode
----------------------------------------------------------------------------*/

pxl_buffer apxl_encode(const apxl_anim* anim, int zstd_level)
{
    pxl_buffer out;
    apxl_file_header fh;
    unsigned pb;
    size_t canvas_bytes, raw_bytes, meta_size, timing_bytes, bound, csize;
    size_t header_region, i;
    uint8_t* raw = NULL;
    unsigned char* file = NULL;
    ZSTD_CCtx* cctx = NULL;

    out.data = NULL; out.size = 0;

    if (!anim || !anim->frames || anim->frame_count == 0) { return out; }
    if (anim->channels < 1 || anim->channels > 4) { return out; }
    if (anim->bytes_per_channel != 1 && anim->bytes_per_channel != 2) { return out; }
    if (!apxl_geometry_ok(anim->canvas_w, anim->canvas_h, anim->frame_count)) { return out; }

    if (zstd_level <= 0) { zstd_level = PXL_LEVEL_DEFAULT; }
    else if (zstd_level > PXL_LEVEL_MAX) { zstd_level = PXL_LEVEL_MAX; }

    pb = (unsigned)anim->channels * anim->bytes_per_channel;
    canvas_bytes = (size_t)anim->canvas_w * anim->canvas_h * pb;
    raw_bytes = canvas_bytes * anim->frame_count;
    /* RawByteCount is a uint32 header field. The geometry limits above allow a
       much larger product (up to 2^51), so refuse anything that would not
       survive the round-trip instead of silently truncating the header and
       emitting a file whose own decoder rejects it. */
    if (raw_bytes > 0xFFFFFFFFu) { return out; }
    meta_size = (anim->metadata.data && anim->metadata.size) ? anim->metadata.size : 0;
    timing_bytes = (size_t)anim->frame_count * APXL_TIMING_BYTES;

    /* Gather all frames into one contiguous buffer. */
    raw = (uint8_t*)malloc(raw_bytes ? raw_bytes : 1);
    if (!raw) { return out; }
    for (i = 0; i < anim->frame_count; ++i) {
        const pxl_image* fi = &anim->frames[i].image;
        if (!fi->buffer.data ||
            fi->width != anim->canvas_w || fi->height != anim->canvas_h ||
            fi->channels != anim->channels ||
            fi->bytes_per_channel != anim->bytes_per_channel) {
            free(raw); return out; /* every frame must be a matching full canvas */
        }
        memcpy(raw + i * canvas_bytes, fi->buffer.data, canvas_bytes);
    }

    /* Compress the whole thing as one zstd stream, with LDM at higher levels. */
    bound = ZSTD_compressBound(raw_bytes);
    header_region = APXL_HEADER_BYTES + meta_size + timing_bytes;
    file = (unsigned char*)malloc(header_region + bound);
    cctx = ZSTD_createCCtx();
    if (!file || !cctx) { free(raw); free(file); if (cctx) ZSTD_freeCCtx(cctx); return out; }

    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, zstd_level);
    if (zstd_level >= APXL_LDM_MIN_LEVEL) {
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, APXL_WINDOW_LOG);
    }
    csize = ZSTD_compress2(cctx, file + header_region, bound, raw, raw_bytes);
    ZSTD_freeCCtx(cctx);
    free(raw);
    if (ZSTD_isError(csize)) { free(file); return out; }

    /* Header. */
    fh.version = APXL_VERSION;
    fh.channels = anim->channels;
    fh.bytes_per_channel = anim->bytes_per_channel;
    fh.flags = 0;
    fh.canvas_w = anim->canvas_w;
    fh.canvas_h = anim->canvas_h;
    fh.frame_count = anim->frame_count;
    fh.loop_count = anim->loop_count;
    fh.meta_byte_count = (uint32_t)meta_size;
    fh.raw_byte_count = (uint32_t)raw_bytes;
    apxl_header_write(file, &fh);

    if (meta_size) {
        memcpy(file + APXL_HEADER_BYTES, anim->metadata.data, meta_size);
    }
    /* Frame timing table. */
    for (i = 0; i < anim->frame_count; ++i) {
        unsigned char* t = file + APXL_HEADER_BYTES + meta_size + i * APXL_TIMING_BYTES;
        pxl_put_le16(t, anim->frames[i].delay_num);
        pxl_put_le16(t + 2, anim->frames[i].delay_den);
    }

    out.data = file;
    out.size = header_region + csize;
    return out;
}

/*----------------------------------------------------------------------------
  Decode
----------------------------------------------------------------------------*/

apxl_anim apxl_decode(pxl_buffer file)
{
    apxl_anim anim;
    apxl_file_header fh;
    unsigned pb;
    size_t canvas_bytes, meta_size, timing_bytes, header_region, frame_off;
    size_t dsize, i;
    const unsigned char* timing;
    uint8_t* raw = NULL;
    ZSTD_DCtx* dctx = NULL;

    memset(&anim, 0, sizeof(anim));

    if (!file.data || !apxl_header_read(file.data, file.size, &fh)) { return anim; }

    pb = (unsigned)fh.channels * fh.bytes_per_channel;
    /* canvas_w*canvas_h is bounded by APXL_MAX_PIXELS in apxl_header_read, so
       this product is at most 2^28 * 8 and fits size_t even at 32 bits. */
    canvas_bytes = (size_t)fh.canvas_w * fh.canvas_h * pb;
    /* But canvas_bytes * frame_count can reach ~2^51, which wraps a 32-bit
       size_t -- the target platform. A crafted 8192x8192x4 / 16-frame header
       made the product 2^32, i.e. 0, matching raw_byte_count = 0; the check
       passed and the frame loop then handed back pointers 2^28 apart into a
       zero-byte buffer. Do the multiply in 64 bits. raw_byte_count is a uint32,
       so a match guarantees the true total is below 2^32 and every frame offset
       used below stays in bounds. Confirmed with a 32-bit ASAN build. */
    if (canvas_bytes == 0 ||
        (uint64_t)canvas_bytes * fh.frame_count != (uint64_t)fh.raw_byte_count) {
        return anim;
    }

    meta_size = fh.meta_byte_count;
    timing_bytes = (size_t)fh.frame_count * APXL_TIMING_BYTES;
    /* Compute in 64-bit first: meta_byte_count is attacker-controlled up to
       UINT32_MAX, which can wrap header_region where size_t is 32-bit. */
    if ((uint64_t)APXL_HEADER_BYTES + meta_size + timing_bytes > file.size) { return anim; }
    header_region = APXL_HEADER_BYTES + meta_size + timing_bytes;
    frame_off = header_region;
    timing = file.data + APXL_HEADER_BYTES + meta_size;

    /* Decompress the single stream into the concatenated frame buffer. */
    raw = (uint8_t*)malloc(fh.raw_byte_count);
    dctx = ZSTD_createDCtx();
    if (!raw || !dctx) { free(raw); if (dctx) ZSTD_freeDCtx(dctx); return anim; }
    ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, APXL_WINDOW_LOG + 1);
    dsize = ZSTD_decompressDCtx(dctx, raw, fh.raw_byte_count,
                                file.data + frame_off, file.size - frame_off);
    ZSTD_freeDCtx(dctx);
    if (ZSTD_isError(dsize) || dsize != fh.raw_byte_count) { free(raw); return anim; }

    anim.frames = (apxl_frame*)calloc(fh.frame_count, sizeof(apxl_frame));
    if (!anim.frames) { free(raw); return anim; }

    if (meta_size) {
        anim.metadata.data = (unsigned char*)malloc(meta_size);
        if (anim.metadata.data) {
            memcpy(anim.metadata.data, file.data + APXL_HEADER_BYTES, meta_size);
            anim.metadata.size = meta_size;
        }
    }

    /* Point each frame into the decompressed block instead of copying it out.
       The copy used to double peak memory -- the whole animation in `raw` plus
       the whole animation again in per-frame buffers -- for no benefit, since
       the block already holds exactly the frame sequence in order. */
    anim.storage.data = raw;
    anim.storage.size = fh.raw_byte_count;
    for (i = 0; i < fh.frame_count; ++i) {
        anim.frames[i].image.buffer.data = raw + (size_t)i * canvas_bytes;
        anim.frames[i].image.buffer.size = canvas_bytes;
        anim.frames[i].image.width = fh.canvas_w;
        anim.frames[i].image.height = fh.canvas_h;
        anim.frames[i].image.channels = fh.channels;
        anim.frames[i].image.bytes_per_channel = fh.bytes_per_channel;
        anim.frames[i].delay_num = pxl_get_le16(timing + i * APXL_TIMING_BYTES);
        anim.frames[i].delay_den = pxl_get_le16(timing + i * APXL_TIMING_BYTES + 2);
    }

    anim.frame_count = fh.frame_count;
    anim.loop_count = fh.loop_count;
    anim.canvas_w = fh.canvas_w;
    anim.canvas_h = fh.canvas_h;
    anim.channels = fh.channels;
    anim.bytes_per_channel = fh.bytes_per_channel;
    return anim;
}

void apxl_free(apxl_anim* anim)
{
    if (!anim) { return; }
    if (anim->frames) {
        uint32_t i;
        for (i = 0; i < anim->frame_count; ++i) {
            /* Frames own their pixels only when there is no shared block; with
               one they are interior pointers and must not be freed. */
            if (!anim->storage.data) {
                pxl_free(&anim->frames[i].image.buffer);
            }
            pxl_free(&anim->frames[i].image.metadata);
        }
        free(anim->frames);
    }
    pxl_free(&anim->storage);
    pxl_free(&anim->metadata);
    memset(anim, 0, sizeof(*anim));
}

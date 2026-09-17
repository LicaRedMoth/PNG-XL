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
    pxl_put_le16(out + 32, h->palette_count);
    pxl_put_le16(out + 34, h->palette_alpha_count);
}

int apxl_header_read(const unsigned char* in, size_t in_size, apxl_file_header* h)
{
    if (in_size < APXL_HEADER_BYTES) { return 0; }
    if (in[0] != APXL_MAGIC0 || in[1] != APXL_MAGIC1 ||
        in[2] != APXL_MAGIC2 || in[3] != APXL_MAGIC3) { return 0; }
    h->version              = in[4];
    h->channels             = in[5];
    h->bytes_per_channel    = in[6];
    h->flags                = in[7];
    h->canvas_w             = pxl_get_le32(in + 8);
    h->canvas_h             = pxl_get_le32(in + 12);
    h->frame_count          = pxl_get_le32(in + 16);
    h->loop_count           = pxl_get_le32(in + 20);
    h->meta_byte_count      = pxl_get_le32(in + 24);
    h->raw_byte_count       = pxl_get_le32(in + 28);
    h->palette_count        = pxl_get_le16(in + 32);
    h->palette_alpha_count  = pxl_get_le16(in + 34);
    if (h->version != APXL_VERSION) { return 0; }
    if (h->channels < 1 || h->channels > 4) { return 0; }
    if (h->bytes_per_channel != 1 && h->bytes_per_channel != 2) { return 0; }
    if (!apxl_geometry_ok(h->canvas_w, h->canvas_h, h->frame_count)) { return 0; }
    if (h->palette_count > APXL_MAX_PALETTE) { return 0; }
    if (h->palette_alpha_count > h->palette_count) { return 0; }
    /* An indexed stream is channels==1, index bytes only -- the palette
       colour count is unrelated to how many index *values* actually appear,
       but a palette without indexed geometry (or vice versa) is malformed. */
    if (h->palette_count > 0 && (h->channels != 1 || h->bytes_per_channel != 1)) { return 0; }
    return 1;
}

/*----------------------------------------------------------------------------
  Optional pre-encode step: build one global palette across every frame of an
  already-composited RGB/RGBA animation (measured 2026-09-18, see apxl.h and
  docs/RESEARCH.md's "Indexed .apxl" entry).
----------------------------------------------------------------------------*/

/* Power of 2, comfortably above APXL_MAX_PALETTE so the table never exceeds a
   25% load factor even full -- linear probing stays cheap and always
   terminates (an empty slot is always reachable). */
#define APXL_INDEX_HASH_SLOTS 1024u

static uint32_t apxl_color_key(const unsigned char* px, unsigned channels)
{
    /* RGB (no alpha channel) is treated as fully opaque, the same convention
       already used elsewhere in this project (e.g. pxl_convert_palette's
       RGB-source handling). */
    uint32_t k = (uint32_t)px[0] | ((uint32_t)px[1] << 8) | ((uint32_t)px[2] << 16);
    k |= (uint32_t)(channels == 4 ? px[3] : 0xFFu) << 24;
    return k;
}

/* xxhash-style avalanche -- cheap, good enough spread for a fixed 1024-slot
   table holding at most 256 real entries. */
static uint32_t apxl_hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x85ebca6bu;
    x ^= x >> 13; x *= 0xc2b2ae35u;
    x ^= x >> 16;
    return x;
}

typedef struct { uint32_t key; int32_t idx; } apxl_index_slot;

/* Looks up `key` in the open-addressed table, inserting it with the next
   free index if new. Returns the index, or -1 if `key` is new and the table
   already holds APXL_MAX_PALETTE entries. `table` must have
   APXL_INDEX_HASH_SLOTS entries, every `idx` initialized to -1. */
static int32_t apxl_index_lookup_or_insert(apxl_index_slot* table, uint32_t* count,
                                            uint32_t key)
{
    uint32_t mask = APXL_INDEX_HASH_SLOTS - 1u;
    uint32_t h = apxl_hash32(key) & mask;
    for (;;) {
        if (table[h].idx < 0) {
            if (*count >= APXL_MAX_PALETTE) { return -1; }
            table[h].key = key;
            table[h].idx = (int32_t)(*count)++;
            return table[h].idx;
        }
        if (table[h].key == key) { return table[h].idx; }
        h = (h + 1u) & mask;
    }
}

int apxl_anim_try_index(apxl_anim* anim)
{
    apxl_index_slot* table;
    uint32_t count = 0, i;
    size_t f, px_count, p;
    unsigned channels;
    unsigned char palette_rgb[APXL_MAX_PALETTE * 3];
    unsigned char palette_a[APXL_MAX_PALETTE];
    int has_alpha = 0, ok = 1;
    uint8_t** new_bufs = NULL;

    if (!anim || !anim->frames || anim->frame_count == 0) { return 0; }
    channels = anim->channels;
    if ((channels != 3 && channels != 4) || anim->bytes_per_channel != 1) { return 0; }
    for (f = 0; f < anim->frame_count; ++f) {
        const pxl_image* fi = &anim->frames[f].image;
        if (pxl_is_indexed(fi) || fi->channels != channels ||
            fi->bytes_per_channel != 1 || pxl_bit_depth(fi) != 8 || !fi->buffer.data) {
            return 0;
        }
    }

    table = (apxl_index_slot*)malloc(APXL_INDEX_HASH_SLOTS * sizeof(apxl_index_slot));
    if (!table) { return 0; }
    for (i = 0; i < APXL_INDEX_HASH_SLOTS; ++i) { table[i].idx = -1; }

    px_count = (size_t)anim->canvas_w * anim->canvas_h;

    /* Pass 1: does the whole animation's colour union fit under 256? Nothing
       is mutated yet -- a caller must be able to fall back to plain RGBA on
       failure as if this were never called. */
    for (f = 0; ok && f < anim->frame_count; ++f) {
        const unsigned char* buf = anim->frames[f].image.buffer.data;
        for (p = 0; p < px_count; ++p) {
            if (apxl_index_lookup_or_insert(table, &count,
                    apxl_color_key(buf + p * channels, channels)) < 0) {
                ok = 0; break;
            }
        }
    }
    if (!ok) { free(table); return 0; }

    /* Materialize the palette in insertion order == the index every pixel
       already resolved to above. */
    for (i = 0; i < APXL_INDEX_HASH_SLOTS; ++i) {
        int32_t idx = table[i].idx;
        if (idx < 0) { continue; }
        palette_rgb[idx * 3 + 0] = (unsigned char)(table[i].key & 0xFFu);
        palette_rgb[idx * 3 + 1] = (unsigned char)((table[i].key >> 8) & 0xFFu);
        palette_rgb[idx * 3 + 2] = (unsigned char)((table[i].key >> 16) & 0xFFu);
        palette_a[idx] = (unsigned char)((table[i].key >> 24) & 0xFFu);
        if (palette_a[idx] != 0xFFu) { has_alpha = 1; }
    }

    /* Pass 2: every colour is already in `table`, so this only ever hits the
       lookup branch -- build each frame's 1-byte-per-pixel index buffer. */
    new_bufs = (uint8_t**)calloc(anim->frame_count, sizeof(uint8_t*));
    if (!new_bufs) { free(table); return 0; }
    for (f = 0; ok && f < anim->frame_count; ++f) {
        const unsigned char* buf = anim->frames[f].image.buffer.data;
        uint8_t* out = (uint8_t*)malloc(px_count ? px_count : 1);
        if (!out) { ok = 0; break; }
        for (p = 0; p < px_count; ++p) {
            out[p] = (uint8_t)apxl_index_lookup_or_insert(table, &count,
                         apxl_color_key(buf + p * channels, channels));
        }
        new_bufs[f] = out;
    }
    free(table);
    if (!ok) {
        for (f = 0; f < anim->frame_count; ++f) { free(new_bufs[f]); }
        free(new_bufs);
        return 0;
    }

    /* Commit: replace each frame's buffer and point every frame at the one
       owned, shared palette -- apxl_encode validates every frame's palette
       matches (trivially true, it's the same bytes) and apxl_free releases
       it once, the same non-owning-alias shape already used for anim.storage
       and for frames decoded by apxl_decode. */
    {
        unsigned char* pal_rgb_owned = (unsigned char*)malloc((size_t)count * 3u);
        unsigned char* pal_a_owned = has_alpha ? (unsigned char*)malloc(count) : NULL;
        if (!pal_rgb_owned || (has_alpha && !pal_a_owned)) {
            free(pal_rgb_owned); free(pal_a_owned);
            for (f = 0; f < anim->frame_count; ++f) { free(new_bufs[f]); }
            free(new_bufs);
            return 0;
        }
        memcpy(pal_rgb_owned, palette_rgb, (size_t)count * 3u);
        if (has_alpha) { memcpy(pal_a_owned, palette_a, count); }

        anim->palette.data = pal_rgb_owned;
        anim->palette.size = (size_t)count * 3u;
        if (has_alpha) {
            anim->palette_alpha.data = pal_a_owned;
            anim->palette_alpha.size = count;
        }
        for (f = 0; f < anim->frame_count; ++f) {
            pxl_image* fi = &anim->frames[f].image;
            free(fi->buffer.data);
            fi->buffer.data = new_bufs[f];
            fi->buffer.size = px_count;
            fi->channels = 1;
            fi->bytes_per_channel = 1;
            fi->bit_depth = 0; /* derives to 8 -- matches bytes_per_channel */
            fi->palette = anim->palette;
            fi->palette_alpha = anim->palette_alpha;
        }
        anim->channels = 1;
        anim->bytes_per_channel = 1;
    }
    free(new_bufs);
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
    size_t header_region, palette_bytes, i;
    uint8_t* raw = NULL;
    unsigned char* file = NULL;
    ZSTD_CCtx* cctx = NULL;
    unsigned palette_count = 0, palette_alpha_count = 0;
    const pxl_image* first;

    out.data = NULL; out.size = 0;

    if (!anim || !anim->frames || anim->frame_count == 0) { return out; }
    if (anim->channels < 1 || anim->channels > 4) { return out; }
    if (anim->bytes_per_channel != 1 && anim->bytes_per_channel != 2) { return out; }
    if (!apxl_geometry_ok(anim->canvas_w, anim->canvas_h, anim->frame_count)) { return out; }

    if (zstd_level <= 0) { zstd_level = PXL_LEVEL_DEFAULT; }
    else if (zstd_level > PXL_LEVEL_MAX) { zstd_level = PXL_LEVEL_MAX; }

    /* Indexed animation (measured 2026-09-18, docs/RESEARCH.md): one global
       palette for the whole stream, taken from the first frame. Every other
       frame must carry the byte-identical palette -- a per-frame palette is
       not representable in this container and silently picking one frame's
       palette over another's would be a lossy encode wearing a lossless
       format's name. Only 8-bit indices are supported; a caller with a
       sub-8-bit indexed source must expand it before calling here. */
    first = &anim->frames[0].image;
    if (pxl_is_indexed(first)) {
        if (anim->channels != 1 || anim->bytes_per_channel != 1 ||
            pxl_bit_depth(first) != 8) {
            return out;
        }
        palette_count = pxl_palette_count(first);
        if (palette_count == 0 || palette_count > APXL_MAX_PALETTE) { return out; }
        palette_alpha_count = (first->palette_alpha.data && first->palette_alpha.size)
                               ? (unsigned)first->palette_alpha.size : 0;
        if (palette_alpha_count > palette_count) { return out; }
        for (i = 1; i < anim->frame_count; ++i) {
            const pxl_image* fi = &anim->frames[i].image;
            if (fi->palette.size != first->palette.size ||
                memcmp(fi->palette.data, first->palette.data, first->palette.size) != 0 ||
                fi->palette_alpha.size != first->palette_alpha.size ||
                (first->palette_alpha.size &&
                 memcmp(fi->palette_alpha.data, first->palette_alpha.data,
                        first->palette_alpha.size) != 0)) {
                return out; /* every frame must share the one global palette */
            }
        }
    }

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
    palette_bytes = (size_t)palette_count * 3u + palette_alpha_count;

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
    header_region = APXL_HEADER_BYTES + palette_bytes + meta_size + timing_bytes;
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
    fh.palette_count = (uint16_t)palette_count;
    fh.palette_alpha_count = (uint16_t)palette_alpha_count;
    apxl_header_write(file, &fh);

    if (palette_count) {
        unsigned char* p = file + APXL_HEADER_BYTES;
        memcpy(p, first->palette.data, (size_t)palette_count * 3u);
        if (palette_alpha_count) {
            memcpy(p + (size_t)palette_count * 3u, first->palette_alpha.data,
                   palette_alpha_count);
        }
    }
    if (meta_size) {
        memcpy(file + APXL_HEADER_BYTES + palette_bytes, anim->metadata.data, meta_size);
    }
    /* Frame timing table. */
    for (i = 0; i < anim->frame_count; ++i) {
        unsigned char* t = file + APXL_HEADER_BYTES + palette_bytes + meta_size
                            + i * APXL_TIMING_BYTES;
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
    size_t canvas_bytes, meta_size, timing_bytes, palette_bytes, header_region, frame_off;
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

    palette_bytes = (size_t)fh.palette_count * 3u + fh.palette_alpha_count;
    meta_size = fh.meta_byte_count;
    timing_bytes = (size_t)fh.frame_count * APXL_TIMING_BYTES;
    /* Compute in 64-bit first: meta_byte_count is attacker-controlled up to
       UINT32_MAX, which can wrap header_region where size_t is 32-bit.
       palette_bytes cannot (palette_count/palette_alpha_count are uint16,
       already bounded to APXL_MAX_PALETTE by apxl_header_read). */
    if ((uint64_t)APXL_HEADER_BYTES + palette_bytes + meta_size + timing_bytes > file.size) {
        return anim;
    }
    header_region = APXL_HEADER_BYTES + palette_bytes + meta_size + timing_bytes;
    frame_off = header_region;
    timing = file.data + APXL_HEADER_BYTES + palette_bytes + meta_size;

    if (fh.palette_count) {
        const unsigned char* p = file.data + APXL_HEADER_BYTES;
        size_t rgb_bytes = (size_t)fh.palette_count * 3u;
        anim.palette.data = (unsigned char*)malloc(rgb_bytes);
        if (!anim.palette.data) { return anim; }
        memcpy(anim.palette.data, p, rgb_bytes);
        anim.palette.size = rgb_bytes;
        if (fh.palette_alpha_count) {
            anim.palette_alpha.data = (unsigned char*)malloc(fh.palette_alpha_count);
            if (!anim.palette_alpha.data) { pxl_free(&anim.palette); return anim; }
            memcpy(anim.palette_alpha.data, p + rgb_bytes, fh.palette_alpha_count);
            anim.palette_alpha.size = fh.palette_alpha_count;
        }
    }

    /* Decompress the single stream into the concatenated frame buffer. */
    raw = (uint8_t*)malloc(fh.raw_byte_count);
    dctx = ZSTD_createDCtx();
    if (!raw || !dctx) {
        free(raw); if (dctx) ZSTD_freeDCtx(dctx);
        pxl_free(&anim.palette); pxl_free(&anim.palette_alpha);
        return anim;
    }
    ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, APXL_WINDOW_LOG + 1);
    dsize = ZSTD_decompressDCtx(dctx, raw, fh.raw_byte_count,
                                file.data + frame_off, file.size - frame_off);
    ZSTD_freeDCtx(dctx);
    if (ZSTD_isError(dsize) || dsize != fh.raw_byte_count) {
        free(raw);
        pxl_free(&anim.palette); pxl_free(&anim.palette_alpha);
        return anim;
    }

    anim.frames = (apxl_frame*)calloc(fh.frame_count, sizeof(apxl_frame));
    if (!anim.frames) {
        free(raw);
        pxl_free(&anim.palette); pxl_free(&anim.palette_alpha);
        return anim;
    }

    if (meta_size) {
        anim.metadata.data = (unsigned char*)malloc(meta_size);
        if (anim.metadata.data) {
            memcpy(anim.metadata.data, file.data + APXL_HEADER_BYTES + palette_bytes, meta_size);
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
        /* bit_depth left 0 ("derive from bytes_per_channel", pxl.h) -- correct
           for indexed frames too, since those always have bytes_per_channel
           == 1 here, which derives to 8, the only depth apxl_encode accepts.
           Non-owning: every frame shares the one anim-level palette above;
           apxl_free releases it once, not per frame -- same shape as buffer
           sharing via anim.storage just above. */
        anim.frames[i].image.palette = anim.palette;
        anim.frames[i].image.palette_alpha = anim.palette_alpha;
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
               one they are interior pointers and must not be freed. Palette
               fields are always a non-owning alias of anim->palette/
               palette_alpha (set by apxl_decode, or left empty by a
               hand-assembled anim), freed once below either way -- never
               per frame, or an indexed decode would double-free. */
            if (!anim->storage.data) {
                pxl_free(&anim->frames[i].image.buffer);
            }
            pxl_free(&anim->frames[i].image.metadata);
        }
        free(anim->frames);
    }
    pxl_free(&anim->storage);
    pxl_free(&anim->metadata);
    pxl_free(&anim->palette);
    pxl_free(&anim->palette_alpha);
    memset(anim, 0, sizeof(*anim));
}

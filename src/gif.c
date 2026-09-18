/** \file gif.c
    \brief GIF87a/89a decoder: blocks, sub-blocks, and GIF's own LZW variant,
           hand-rolled -- no external dependency, unlike the PNG side which
           gets DEFLATE from zlib via libpng.

    Every frame is composited to a full-canvas RGBA8 apxl_anim, the same
    shape apng_load() produces, using the same dispose-then-draw model (GIF's
    disposal method maps directly onto APNG's dispose_op: 0/1 = do nothing,
    2 = restore to background, 3 = restore to previous). A transparent GIF
    pixel is written as RGBA (0,0,0,0) uniformly -- the underlying palette
    RGB a transparent index carries is never visible, so preserving it would
    only inflate the composited colour count apxl_anim_try_index later has to
    re-derive a palette from, for no benefit.

    Not attempted: decoding straight to indices when every frame shares the
    global colour table and no local table appears. It looked appealing
    (GIF's global colour table already IS the "one global palette" .apxl
    wants), but disposal-to-background needs a genuinely transparent pixel,
    which has no index in a table that is already using all 256 entries for
    real colours -- exactly the kind of edge case that turns a shortcut into
    a second, less-tested code path. Compositing to RGBA and handing the
    result to the already-tested apxl_anim_try_index (docs/RESEARCH.md's
    "Indexed .apxl" entry, 2026-09-18) gets the same outcome whenever the
    source actually was simple, correctly, for every source, with one
    compositor instead of two.
*/
#include "gif.h"
#include "pxl_bytes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mirrors apng.c's own limits -- same reasoning: bound canvas geometry before
   any multiplication reaches size_t, on a crafted header. */
#define GIF_MAX_DIM     1000000u
#define GIF_MAX_PIXELS  ((uint64_t)1 << 28)
#define GIF_MAX_FRAMES  1000000u

/* GIF disposal methods (raw 3-bit field from the Graphic Control Extension). */
#define GIF_DISPOSE_UNSPECIFIED  0
#define GIF_DISPOSE_NONE         1
#define GIF_DISPOSE_BACKGROUND   2
#define GIF_DISPOSE_PREVIOUS     3

/*----------------------------------------------------------------------------
  Whole-file read. Not pxl_slurp (pxl_pngio.h) on purpose: that header pulls
  in <png.h> for its memory-reader callbacks, which this module has no other
  reason to depend on -- GIF decoding needs no external library at all.
----------------------------------------------------------------------------*/
static unsigned char* gif_slurp(const char* path, size_t* out_size)
{
    FILE* f = fopen(path, "rb");
    unsigned char* buf;
    long n;

    if (!f) { return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (unsigned char*)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

/* A simple growable byte buffer, same shape as apng.c's own (not shared --
   see gif.h's file comment on why this module stays dependency-free). */
typedef struct { unsigned char* data; size_t size, cap; int failed; } growbuf;

static void gb_put(growbuf* b, const void* src, size_t n)
{
    if (b->failed) { return; }
    if (b->size + n > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 256;
        unsigned char* nd;
        while (ncap < b->size + n) { ncap *= 2; }
        nd = (unsigned char*)realloc(b->data, ncap);
        if (!nd) { b->failed = 1; return; }
        b->data = nd; b->cap = ncap;
    }
    memcpy(b->data + b->size, src, n);
    b->size += n;
}

/*----------------------------------------------------------------------------
  GIF's own LZW variant.

  Variable-width codes (min_code_size+1 bits, growing to 12), packed
  LSB-first across byte boundaries -- the opposite convention from PNG's
  chunk fields, which is why this needs its own bit reader rather than
  anything in pxl_bytes.h (that header is explicitly big/little-endian
  *byte* fields, not a sub-byte bit-packing scheme).
----------------------------------------------------------------------------*/
#define LZW_MAX_CODE 4096 /* 12-bit code space ceiling */

typedef struct {
    const unsigned char* data;
    size_t size, pos;
    uint32_t bitbuf;
    int bitcount;
} lzw_bits;

/* Returns the next `n` bits (n <= 12, so bitbuf, which holds at most 7
   leftover + 8 fresh = 15 bits before extraction, never overflows a 32-bit
   accumulator), or -1 once the byte stream is exhausted mid-code -- a
   truncated file, not a crash. */
static int lzw_read_bits(lzw_bits* b, int n)
{
    while (b->bitcount < n) {
        if (b->pos >= b->size) { return -1; }
        b->bitbuf |= (uint32_t)b->data[b->pos++] << b->bitcount;
        b->bitcount += 8;
    }
    {
        int code = (int)(b->bitbuf & ((1u << n) - 1u));
        b->bitbuf >>= n;
        b->bitcount -= n;
        return code;
    }
}

typedef struct { int16_t prefix; uint8_t suffix; } lzw_entry;

/* Decodes `in` (all of one image's sub-blocks already concatenated) into
   exactly `out_len` index bytes. Returns 1 on success. A stream that runs
   out of data, references an invalid code, or tries to produce more than
   out_len bytes is rejected rather than silently truncated or overrun --
   untrusted input, same posture as every other decoder in this project. */
static int gif_lzw_decode(const unsigned char* in, size_t in_len, int min_code_size,
                          unsigned char* out, size_t out_len)
{
    lzw_bits bits;
    lzw_entry* table;
    unsigned char* stack;
    int clear_code, end_code, code_size, next_code;
    int prev_code = -1;
    size_t out_pos = 0;
    int ok = 1;

    if (min_code_size < 2 || min_code_size > 8) { return 0; }
    clear_code = 1 << min_code_size;
    end_code = clear_code + 1;

    table = (lzw_entry*)malloc(LZW_MAX_CODE * sizeof(lzw_entry));
    stack = (unsigned char*)malloc(LZW_MAX_CODE);
    if (!table || !stack) { free(table); free(stack); return 0; }

    memset(&bits, 0, sizeof(bits));
    bits.data = in; bits.size = in_len;
    code_size = min_code_size + 1;
    next_code = end_code + 1;
    /* Root codes 0..clear_code-1 are single bytes, defined once; codes
       clear_code/end_code are control, never expanded; everything from
       end_code+1 up is (re)built after each Clear. */
    {
        int i;
        for (i = 0; i < clear_code; ++i) { table[i].prefix = -1; table[i].suffix = (uint8_t)i; }
    }

    for (;;) {
        int code = lzw_read_bits(&bits, code_size);
        if (code < 0) { ok = 0; break; }

        if (code == clear_code) {
            code_size = min_code_size + 1;
            next_code = end_code + 1;
            prev_code = -1;
            continue;
        }
        if (code == end_code) {
            break;
        }
        /* By this point code is neither clear_code nor end_code. Exactly
           three cases remain: an already-known code (root byte or a string
           this loop defined earlier), the one-past-the-end "KwKwK" code GIF
           LZW allows, or a corrupt stream naming a code nobody defined. */
        if (code < next_code) {
            /* Known code: expand by walking the prefix chain onto `stack`,
               then emit it reversed (LZW's table stores each string as
               "earlier string plus one byte", so the walk comes out
               backwards). */
            int c = code, sp = 0;
            while (c >= 0) {
                if (sp >= LZW_MAX_CODE) { ok = 0; break; } /* pathological chain, refuse rather than overrun */
                stack[sp++] = table[c].suffix;
                c = table[c].prefix;
            }
            if (!ok) { break; }
            if (prev_code >= 0 && next_code < LZW_MAX_CODE) {
                table[next_code].prefix = (int16_t)prev_code;
                table[next_code].suffix = stack[sp - 1]; /* first byte of the string just expanded */
                ++next_code;
                if (next_code == (1 << code_size) && code_size < 12) { ++code_size; }
            }
            while (sp > 0) {
                if (out_pos >= out_len) { ok = 0; break; }
                out[out_pos++] = stack[--sp];
            }
            if (!ok) { break; }
        } else if (code == next_code && prev_code >= 0) {
            /* GIF LZW's classic "KwKwK" pattern: a code one past the last
               one defined so far names (previous string) + (previous
               string's own first byte) -- not yet in the table, but legal. */
            int c = prev_code, sp = 0;
            unsigned char first;
            while (c >= 0) {
                if (sp >= LZW_MAX_CODE) { ok = 0; break; }
                stack[sp++] = table[c].suffix;
                c = table[c].prefix;
            }
            if (!ok) { break; }
            first = stack[sp - 1];
            if (next_code < LZW_MAX_CODE) {
                table[next_code].prefix = (int16_t)prev_code;
                table[next_code].suffix = first;
                ++next_code;
                if (next_code == (1 << code_size) && code_size < 12) { ++code_size; }
            }
            while (sp > 0) {
                if (out_pos >= out_len) { ok = 0; break; }
                out[out_pos++] = stack[--sp];
            }
            if (!ok) { break; }
            if (out_pos >= out_len) { ok = 0; break; }
            out[out_pos++] = first; /* the "K" that closes KwKwK */
        } else {
            ok = 0; break; /* code references a string never defined -- corrupt stream */
        }
        prev_code = code;
    }

    free(table);
    free(stack);
    return ok && out_pos == out_len;
}

/*----------------------------------------------------------------------------
  Block-level parsing.
----------------------------------------------------------------------------*/

/* Reads a GIF "data sub-blocks" run starting at *pos (a size byte, 1..255,
   followed by that many bytes; repeated; terminated by a size-0 byte) and
   appends the concatenated payload to `out`. Advances *pos past the
   terminator. Returns 1 on success, 0 if the file runs out first. */
static int read_subblocks(const unsigned char* file, size_t file_size, size_t* pos, growbuf* out)
{
    for (;;) {
        unsigned char n;
        if (*pos >= file_size) { return 0; }
        n = file[(*pos)++];
        if (n == 0) { return 1; }
        if (*pos + n > file_size) { return 0; }
        if (out) { gb_put(out, file + *pos, n); }
        *pos += n;
    }
}

/* One parsed colour table: `count` entries, 3 bytes/entry RGB, pointing
   directly into the source file buffer (never copied until a frame actually
   needs its own palette, see gif_load). */
typedef struct { const unsigned char* rgb; unsigned count; } gif_palette;

static int read_color_table(const unsigned char* file, size_t file_size, size_t* pos,
                            unsigned size_field, gif_palette* out)
{
    unsigned count = 2u << size_field; /* size_field is N in 2^(N+1) */
    size_t bytes = (size_t)count * 3u;
    if (*pos + bytes > file_size) { return 0; }
    out->rgb = file + *pos;
    out->count = count;
    *pos += bytes;
    return 1;
}

/* Pending state from the most recent Graphic Control Extension, applying to
   the very next Image Descriptor only (GIF allows at most one GCE per
   image; this project does not need to detect a malformed file reusing one,
   since consuming it once and resetting is enough to decode correctly). */
typedef struct {
    int has_gce;
    uint8_t disposal;
    int transparent_flag;
    uint8_t transparent_index;
    uint16_t delay_cs;
} gif_gce;

/* One already-decoded image, ready to composite: local rect, its own index
   pixels (LZW-decoded, one byte per pixel, in display row order -- interlace
   already undone), the palette it uses (local or a copy of the caller's
   global one), and the GCE that preceded it, if any. */
typedef struct {
    uint32_t x, y, w, h;
    unsigned char* indices; /* w*h bytes, owned */
    gif_palette pal;
    gif_gce gce;
} gif_image;

static void gif_image_free(gif_image* im) { free(im->indices); im->indices = NULL; }

/* Un-interlaces `src` (rows in GIF's 4-pass interlace order) into `dst`
   (rows in display order), row stride `w` bytes each, `h` rows total. GIF's
   passes: every 8th row starting at 0, every 8th starting at 4, every 4th
   starting at 2, every 2nd starting at 1 -- always in that pass order. */
static void deinterlace(const unsigned char* src, unsigned char* dst, uint32_t w, uint32_t h)
{
    static const uint32_t start[4] = { 0, 4, 2, 1 };
    static const uint32_t step[4]  = { 8, 8, 4, 2 };
    uint32_t pass, row, sy = 0;
    for (pass = 0; pass < 4; ++pass) {
        for (row = start[pass]; row < h; row += step[pass]) {
            memcpy(dst + (size_t)row * w, src + (size_t)sy * w, w);
            ++sy;
        }
    }
}

/* Reads one Image Descriptor (the caller has already consumed the 0x2C
   introducer) plus its local colour table, if any, and LZW-decodes its
   pixels. `gce` is consumed (copied in) and the caller should reset its
   `has_gce` afterwards -- one GCE belongs to exactly one image. */
static int read_image(const unsigned char* file, size_t file_size, size_t* pos,
                      const gif_palette* global_pal, const gif_gce* gce,
                      uint32_t canvas_w, uint32_t canvas_h, gif_image* out)
{
    unsigned char packed, min_code_size;
    int local_table, interlace;
    unsigned lct_size;
    growbuf lzw_data;
    unsigned char* raw;

    memset(out, 0, sizeof(*out));
    memset(&lzw_data, 0, sizeof(lzw_data));

    if (*pos + 9 > file_size) { return 0; }
    out->x = pxl_get_le16(file + *pos);
    out->y = pxl_get_le16(file + *pos + 2);
    out->w = pxl_get_le16(file + *pos + 4);
    out->h = pxl_get_le16(file + *pos + 6);
    packed = file[*pos + 8];
    *pos += 9;

    local_table = (packed & 0x80) != 0;
    interlace   = (packed & 0x40) != 0;
    lct_size    = packed & 0x07;

    if (out->w == 0 || out->h == 0 ||
        (uint64_t)out->x + out->w > canvas_w || (uint64_t)out->y + out->h > canvas_h) {
        return 0;
    }

    if (local_table) {
        if (!read_color_table(file, file_size, pos, lct_size, &out->pal)) { return 0; }
    } else {
        out->pal = *global_pal;
    }
    if (out->pal.count == 0) { return 0; } /* no local table and no global one either */

    if (*pos >= file_size) { return 0; }
    min_code_size = file[(*pos)++];

    if (!read_subblocks(file, file_size, pos, &lzw_data)) { free(lzw_data.data); return 0; }

    /* out->w and out->h are both > 0, checked above, so this product is a
       real, nonzero allocation size -- no zero-size malloc edge case here. */
    raw = (unsigned char*)malloc((size_t)out->w * out->h);
    out->indices = (unsigned char*)malloc((size_t)out->w * out->h);
    if (!raw || !out->indices ||
        !gif_lzw_decode(lzw_data.data, lzw_data.size, min_code_size, raw,
                        (size_t)out->w * out->h)) {
        free(raw); free(lzw_data.data);
        gif_image_free(out);
        return 0;
    }
    free(lzw_data.data);

    if (interlace) {
        deinterlace(raw, out->indices, out->w, out->h);
        free(raw);
    } else {
        free(out->indices);
        out->indices = raw;
    }

    if (gce->has_gce) { out->gce = *gce; }
    return 1;
}

/*----------------------------------------------------------------------------
  Load
----------------------------------------------------------------------------*/

apxl_anim gif_load(const char* path)
{
    apxl_anim anim;
    unsigned char* file = NULL;
    size_t file_size = 0, pos;
    uint32_t canvas_w, canvas_h;
    unsigned char packed;
    gif_palette global_pal;
    gif_gce pending_gce;
    uint32_t loop_count = 1; /* GIF default: play once, unless NETSCAPE2.0 says otherwise */
    int saw_netscape_loop = 0;
    size_t canvas_bytes;
    uint8_t* canvas = NULL;
    uint8_t* prevbuf = NULL;
    gif_image* images = NULL;
    uint32_t icap = 0, icount = 0;
    uint32_t i;

    memset(&anim, 0, sizeof(anim));
    memset(&global_pal, 0, sizeof(global_pal));
    memset(&pending_gce, 0, sizeof(pending_gce));

    file = gif_slurp(path, &file_size);
    if (!file || file_size < 13 || memcmp(file, "GIF8", 4) != 0 ||
        (memcmp(file + 4, "7a", 2) != 0 && memcmp(file + 4, "9a", 2) != 0)) {
        free(file); return anim;
    }

    canvas_w = pxl_get_le16(file + 6);
    canvas_h = pxl_get_le16(file + 8);
    packed = file[10];
    pos = 13; /* skip header(6) + logical screen descriptor(7) */

    if (canvas_w == 0 || canvas_h == 0 || canvas_w > GIF_MAX_DIM || canvas_h > GIF_MAX_DIM ||
        (uint64_t)canvas_w * canvas_h > GIF_MAX_PIXELS) {
        free(file); return anim;
    }

    if (packed & 0x80) {
        if (!read_color_table(file, file_size, &pos, packed & 0x07, &global_pal)) {
            free(file); return anim;
        }
    }
    /* Bytes 11-12 (background colour index, pixel aspect ratio) are not
       used. Measured, not assumed, and worth recording so it isn't
       re-tried: GIF89a's own text advises filling canvas area no frame
       ever covers with the background colour, and three real corpus files
       (a first frame smaller than the canvas) seemed to confirm it against
       ffmpeg's GIF decoder -- but checked against Chromium's, the actual
       target that matters for "how a GIF is expected to look" today, both
       that case AND a frame's own transparent-index pixels render fully
       transparent (0,0,0,0), never the background colour. ffmpeg turns out
       to be the outlier here, implementing the spec's advisory text
       literally where real-world rendering does not. Canvas area no frame
       has ever drawn to, and whatever a BACKGROUND disposal clears, are
       therefore both just transparent -- calloc/memset(0) below, no special
       handling needed. */

    /* First pass: walk blocks, decoding each image as it's found. Frames are
       composited in a second pass below, mirroring apng_load's structure --
       collect first, so a mid-file parse failure still lets already-decoded
       images be freed uniformly rather than needing two different cleanup
       paths for "still parsing" vs "compositing". */
    for (;;) {
        unsigned char introducer;
        if (pos >= file_size) { goto fail; } /* no trailer: truncated file */
        introducer = file[pos++];

        if (introducer == 0x3B) { /* trailer */
            break;
        } else if (introducer == 0x21) { /* extension */
            unsigned char label;
            if (pos >= file_size) { goto fail; }
            label = file[pos++];
            if (label == 0xF9) { /* Graphic Control Extension */
                unsigned char size;
                if (pos >= file_size) { goto fail; }
                size = file[pos++];
                if (size < 4 || pos + 4 > file_size) { goto fail; }
                pending_gce.has_gce = 1;
                pending_gce.disposal = (file[pos] >> 2) & 0x07;
                pending_gce.transparent_flag = file[pos] & 0x01;
                pending_gce.delay_cs = pxl_get_le16(file + pos + 1);
                pending_gce.transparent_index = file[pos + 3];
                pos += size; /* size is normally exactly 4; skip whatever's declared */
                if (!read_subblocks(file, file_size, &pos, NULL)) { goto fail; } /* block terminator */
            } else if (label == 0xFF) { /* Application Extension */
                unsigned char size;
                growbuf app_id;
                memset(&app_id, 0, sizeof(app_id));
                if (pos >= file_size) { goto fail; }
                size = file[pos++];
                if (pos + size > file_size) { goto fail; }
                gb_put(&app_id, file + pos, size);
                pos += size;
                if (size == 11 && memcmp(app_id.data, "NETSCAPE2.0", 11) == 0) {
                    /* Loop sub-block: [size=3][1][loop count LE 2 bytes]. */
                    growbuf loop_data;
                    memset(&loop_data, 0, sizeof(loop_data));
                    if (!read_subblocks(file, file_size, &pos, &loop_data)) {
                        free(app_id.data); free(loop_data.data); goto fail;
                    }
                    if (loop_data.size >= 3 && loop_data.data[0] == 1) {
                        loop_count = pxl_get_le16(loop_data.data + 1);
                        saw_netscape_loop = 1;
                    }
                    free(loop_data.data);
                } else {
                    if (!read_subblocks(file, file_size, &pos, NULL)) { free(app_id.data); goto fail; }
                }
                free(app_id.data);
            } else {
                /* Comment (0xFE), Plain Text (0x01), or anything unknown:
                   contributes nothing to the decoded pixels, so just skip
                   its sub-blocks (Plain Text has a fixed 12-byte header
                   before its sub-blocks begin). */
                if (label == 0x01) {
                    if (pos + 13 > file_size) { goto fail; }
                    pos += 13; /* block size (always 12) + the 12 header bytes */
                }
                if (!read_subblocks(file, file_size, &pos, NULL)) { goto fail; }
            }
        } else if (introducer == 0x2C) { /* image descriptor */
            gif_image im;
            if (!read_image(file, file_size, &pos, &global_pal, &pending_gce,
                            canvas_w, canvas_h, &im)) {
                goto fail;
            }
            pending_gce.has_gce = 0;
            if (icount == icap) {
                uint32_t nc = icap ? icap * 2 : 8;
                gif_image* ni;
                if (nc < icap || nc > GIF_MAX_FRAMES) { gif_image_free(&im); goto fail; }
                ni = (gif_image*)realloc(images, (size_t)nc * sizeof(gif_image));
                if (!ni) { gif_image_free(&im); goto fail; }
                images = ni; icap = nc;
            }
            images[icount++] = im;
        } else {
            goto fail; /* unrecognized introducer byte */
        }
    }

    if (icount == 0) { goto fail; }

    /* Second pass: composite exactly like apng_load does -- dispose-then-draw
       onto a persistent RGBA8 canvas, one full-canvas snapshot per frame. */
    canvas_bytes = (size_t)canvas_w * canvas_h * 4;
    anim.frames = (apxl_frame*)calloc(icount, sizeof(apxl_frame));
    anim.frame_count = icount;
    canvas = (uint8_t*)calloc(1, canvas_bytes); /* transparent -- see the background-colour note above */
    prevbuf = (uint8_t*)malloc(canvas_bytes);
    if (!anim.frames || !canvas || !prevbuf) { goto fail; }

    for (i = 0; i < icount; ++i) {
        gif_image* im = &images[i];
        uint32_t rx, ry;
        int has_trans = im->gce.has_gce && im->gce.transparent_flag;
        uint8_t trans_idx = im->gce.transparent_index;

        memcpy(prevbuf, canvas, canvas_bytes);

        for (ry = 0; ry < im->h; ++ry) {
            uint8_t* dst = canvas + ((size_t)(im->y + ry) * canvas_w + im->x) * 4;
            const unsigned char* srow = im->indices + (size_t)ry * im->w;
            for (rx = 0; rx < im->w; ++rx) {
                unsigned char idx = srow[rx];
                uint8_t* d = dst + (size_t)rx * 4;
                if (has_trans && idx == trans_idx) { continue; } /* canvas shows through */
                if (idx >= im->pal.count) { idx = (unsigned char)(im->pal.count - 1); } /* clamp, don't OOB-read */
                d[0] = im->pal.rgb[(size_t)idx * 3 + 0];
                d[1] = im->pal.rgb[(size_t)idx * 3 + 1];
                d[2] = im->pal.rgb[(size_t)idx * 3 + 2];
                d[3] = 255;
            }
        }

        {
            uint8_t* framebuf = (uint8_t*)malloc(canvas_bytes);
            if (!framebuf) { goto fail; }
            memcpy(framebuf, canvas, canvas_bytes);
            anim.frames[i].image.buffer.data = framebuf;
            anim.frames[i].image.buffer.size = canvas_bytes;
            anim.frames[i].image.width = canvas_w;
            anim.frames[i].image.height = canvas_h;
            anim.frames[i].image.channels = 4;
            anim.frames[i].image.bytes_per_channel = 1;
            /* GIF delay is centiseconds; delay_num/delay_den is a ratio in
               seconds (apxl.h), so denominator 100 carries it exactly. A
               delay of 0 is common in GIFs that expect "as fast as
               possible" -- left as 0/100 rather than substituting a guess;
               a player's own minimum-frame-time policy is a display-time
               decision, not something this loader should bake in. */
            anim.frames[i].delay_num = im->gce.has_gce ? im->gce.delay_cs : 0;
            anim.frames[i].delay_den = 100;
        }

        if (im->gce.has_gce && im->gce.disposal == GIF_DISPOSE_BACKGROUND) {
            for (ry = 0; ry < im->h; ++ry) {
                memset(canvas + ((size_t)(im->y + ry) * canvas_w + im->x) * 4, 0, (size_t)im->w * 4);
            }
        } else if (im->gce.has_gce && im->gce.disposal == GIF_DISPOSE_PREVIOUS) {
            memcpy(canvas, prevbuf, canvas_bytes);
        }
        /* Unspecified/do-not-dispose: leave the canvas as drawn. */
    }

    anim.canvas_w = canvas_w; anim.canvas_h = canvas_h;
    anim.channels = 4; anim.bytes_per_channel = 1;
    anim.loop_count = saw_netscape_loop ? loop_count : 1;

    free(canvas); free(prevbuf);
    for (i = 0; i < icount; ++i) { gif_image_free(&images[i]); }
    free(images);
    free(file);
    return anim;

fail:
    free(canvas); free(prevbuf);
    if (images) { for (i = 0; i < icount; ++i) { gif_image_free(&images[i]); } free(images); }
    free(file);
    apxl_free(&anim);
    memset(&anim, 0, sizeof(anim));
    return anim;
}

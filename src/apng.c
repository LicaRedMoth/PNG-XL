/** \file apng.c
    \brief APNG <-> full-canvas frame sequence via libpng + manual chunk work.

    Load: walk the PNG chunk stream, collect acTL/fcTL and the image data for
    each frame (IDAT for the default image, fdAT for later frames), synthesize a
    standalone single-frame PNG for each in memory, decode it to RGBA8 with
    libpng, then composite onto the canvas honoring dispose_op/blend_op.

    Save: emit IHDR, acTL, then per frame a fcTL plus IDAT (frame 0) or fdAT
    (later frames) carrying that frame's full-canvas RGBA8 image, with correct,
    gap-free sequence numbers.
*/
#include "apng.h"
#include "pxl_pngio.h"
#include "pxl_bytes.h"
#include "pxl_meta.h"

#include <png.h>
#include <zlib.h>

#include <stdlib.h>
#include <string.h>

/* Sanity limits on geometry read from an untrusted file. PNG itself allows
   dimensions up to 2^31-1, which multiplied out would overflow or demand
   absurd allocations, so we refuse anything past these bounds. 2^28 pixels is
   1 GiB of RGBA canvas -- larger than any real image, and libpng's own default
   limits are comparable. */
#define APNG_MAX_DIM     1000000u
#define APNG_MAX_PIXELS  ((uint64_t)1 << 28)

/* APNG dispose / blend ops. */
#define DISPOSE_NONE       0
#define DISPOSE_BACKGROUND 1
#define DISPOSE_PREVIOUS   2
#define BLEND_SOURCE       0
#define BLEND_OVER         1

/*----------------------------------------------------------------------------
  Growable byte buffer
----------------------------------------------------------------------------*/

typedef struct { unsigned char* data; size_t size, cap; int failed; } growbuf;

static void gb_put(growbuf* b, const void* src, size_t n)
{
    if (b->failed) { return; }
    if (b->size + n > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 8192;
        unsigned char* nd;
        while (ncap < b->size + n) { ncap *= 2; }
        nd = (unsigned char*)realloc(b->data, ncap);
        if (!nd) { b->failed = 1; return; }
        b->data = nd; b->cap = ncap;
    }
    memcpy(b->data + b->size, src, n);
    b->size += n;
}

/* Append a complete PNG chunk (length, type, data, CRC) to a growbuf. */
static void gb_chunk(growbuf* b, const char type[4],
                     const unsigned char* data, size_t len)
{
    unsigned char tmp[4];
    uLong crc;
    pxl_put_be32(tmp, (uint32_t)len);
    gb_put(b, tmp, 4);
    gb_put(b, type, 4);
    if (len) { gb_put(b, data, len); }
    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef*)type, 4);
    if (len) { crc = crc32(crc, data, (uInt)len); }
    pxl_put_be32(tmp, (uint32_t)crc);
    gb_put(b, tmp, 4);
}

static const unsigned char PNG_SIG[8] = {137,80,78,71,13,10,26,10};

/*----------------------------------------------------------------------------
  Decode one synthesized single-frame PNG (in memory) to RGBA8.
----------------------------------------------------------------------------*/

static int decode_frame_png(const unsigned char* png_bytes, size_t png_size,
                            uint32_t expect_w, uint32_t expect_h,
                            uint8_t* out_rgba /* expect_w*expect_h*4 */)
{
    png_structp png = NULL;
    png_infop info = NULL;
    png_bytep* volatile rows = NULL;
    pxl_mem_reader reader;
    png_uint_32 w, h, y;
    int bit_depth, color_type;
    volatile int ok = 0;

    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { return 0; }
    info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); return 0; }

    if (setjmp(png_jmpbuf(png))) {
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        return ok;
    }

    reader.data = png_bytes; reader.size = png_size; reader.pos = 0;
    png_set_read_fn(png, &reader, pxl_png_read_mem);
    /* Bound what libpng will allocate for a hostile IHDR / chunk stream. */
    png_set_user_limits(png, APNG_MAX_DIM, APNG_MAX_DIM);
    png_read_info(png, info);
    png_get_IHDR(png, info, &w, &h, &bit_depth, &color_type, NULL, NULL, NULL);

    if (w != expect_w || h != expect_h) { png_longjmp(png, 1); }

    /* Force RGBA8 output regardless of source format. */
    if (color_type == PNG_COLOR_TYPE_PALETTE) { png_set_palette_to_rgb(png); }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) { png_set_tRNS_to_alpha(png); }
    if (bit_depth == 16) { png_set_strip_16(png); }
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA) { png_set_gray_to_rgb(png); }
    /* Ensure an alpha channel exists (filler if none). */
    png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);

    rows = (png_bytep*)malloc(sizeof(png_bytep) * h);
    if (!rows) { png_longjmp(png, 1); }
    for (y = 0; y < h; ++y) {
        rows[y] = out_rgba + (size_t)y * w * 4;
    }
    png_read_image(png, rows);
    png_read_end(png, NULL);
    ok = 1;

    free(rows);
    rows = NULL;
    png_destroy_read_struct(&png, &info, NULL);
    return ok;
}

/* Build a standalone single-frame PNG (RGBA8, w x h) from raw image-data chunk
   bytes (concatenated IDAT/fdAT payloads) plus a source IHDR to copy depth /
   color type from. Returns a growbuf the caller must free. */
static growbuf synth_png(const unsigned char* ihdr, uint32_t w, uint32_t h,
                         const unsigned char* idat, size_t idat_len)
{
    growbuf g;
    unsigned char hdr[13];
    memset(&g, 0, sizeof(g));

    /* IHDR: frame dims, but copy bit depth/color type/etc from the source. */
    pxl_put_be32(hdr + 0, w);
    pxl_put_be32(hdr + 4, h);
    hdr[8]  = ihdr[8];   /* bit depth */
    hdr[9]  = ihdr[9];   /* color type */
    hdr[10] = ihdr[10];  /* compression */
    hdr[11] = ihdr[11];  /* filter */
    hdr[12] = ihdr[12];  /* interlace */

    gb_put(&g, PNG_SIG, 8);
    gb_chunk(&g, "IHDR", hdr, 13);
    gb_chunk(&g, "IDAT", idat, idat_len);
    gb_chunk(&g, "IEND", NULL, 0);
    return g;
}

/*----------------------------------------------------------------------------
  Load
----------------------------------------------------------------------------*/

/* Per-frame control gathered from fcTL. */
typedef struct {
    uint32_t w, h, x, y;
    uint16_t delay_num, delay_den;
    uint8_t dispose, blend;
    growbuf data;      /* concatenated frame image data (raw zlib stream) */
    int has_data;
} frame_ctl;

apxl_anim apng_load(const char* path)
{
    apxl_anim anim;
    unsigned char* file = NULL;
    size_t file_size = 0, pos;
    unsigned char ihdr[13];
    int have_ihdr = 0;
    uint32_t canvas_w = 0, canvas_h = 0, num_plays = 0;
    frame_ctl* fctls = NULL;
    uint32_t fcap = 0, fcount = 0;
    int default_is_frame = 0;   /* IDAT belongs to frame 0 (had a fcTL before IDAT) */
    growbuf idat_default;       /* raw IDAT stream (default image) */
    int idat_seen = 0;
    uint8_t* canvas = NULL;
    uint8_t* prevbuf = NULL;    /* for DISPOSE_PREVIOUS */
    uint32_t i;
    size_t canvas_bytes;

    memset(&anim, 0, sizeof(anim));
    memset(&idat_default, 0, sizeof(idat_default));

    file = pxl_slurp(path, &file_size);
    if (!file || file_size < 8 || png_sig_cmp(file, 0, 8) != 0) {
        free(file); return anim;
    }

    /* First pass: walk chunks, collect IHDR, acTL, fcTL list, frame data. */
    pos = 8;
    while (pos + 8 <= file_size) {
        uint32_t len = pxl_get_be32(file + pos);
        const unsigned char* type = file + pos + 4;
        const unsigned char* data = file + pos + 8;
        if ((size_t)pos + 12 + len > file_size) { break; }

        if (memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            memcpy(ihdr, data, 13);
            canvas_w = pxl_get_be32(data + 0);
            canvas_h = pxl_get_be32(data + 4);
            have_ihdr = 1;
        } else if (memcmp(type, "acTL", 4) == 0 && len >= 8) {
            num_plays = pxl_get_be32(data + 4);
        } else if (memcmp(type, "fcTL", 4) == 0 && len >= 26) {
            frame_ctl fc;
            memset(&fc, 0, sizeof(fc));
            fc.w = pxl_get_be32(data + 4);
            fc.h = pxl_get_be32(data + 8);
            fc.x = pxl_get_be32(data + 12);
            fc.y = pxl_get_be32(data + 16);
            fc.delay_num = pxl_get_be16(data + 20);
            fc.delay_den = pxl_get_be16(data + 22);
            fc.dispose = data[24];
            fc.blend = data[25];
            if (fcount == fcap) {
                uint32_t nc = fcap ? fcap * 2 : 8;
                /* Size in size_t: nc * sizeof(frame_ctl) would overflow if
                   computed in 32-bit on a pathological file. */
                frame_ctl* nf = (frame_ctl*)realloc(
                    fctls, (size_t)nc * sizeof(frame_ctl));
                if (nc < fcap || !nf) { goto fail; }
                fctls = nf; fcap = nc;
            }
            fctls[fcount++] = fc;
            if (!idat_seen && fcount == 1) { default_is_frame = 1; }
        } else if (memcmp(type, "IDAT", 4) == 0) {
            gb_put(&idat_default, data, len);
            idat_seen = 1;
        } else if (memcmp(type, "fdAT", 4) == 0 && len >= 4) {
            /* fdAT = 4-byte sequence number + IDAT-like data; attach to the
               most recent fcTL frame. */
            if (fcount == 0) { goto fail; }
            gb_put(&fctls[fcount - 1].data, data + 4, len - 4);
            fctls[fcount - 1].has_data = 1;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + len;
    }

    /* Canvas dimensions come from the untrusted IHDR. Two hazards: w*h*4 can
       overflow size_t (yielding an allocation far smaller than the writes that
       follow), and a few-byte file can declare a huge canvas to force a giant
       allocation. Bound the pixel count in 64-bit arithmetic before any
       multiplication reaches size_t. */
    if (!have_ihdr || canvas_w == 0 || canvas_h == 0 ||
        canvas_w > APNG_MAX_DIM || canvas_h > APNG_MAX_DIM ||
        (uint64_t)canvas_w * canvas_h > APNG_MAX_PIXELS) { goto fail; }
    canvas_bytes = (size_t)canvas_w * canvas_h * 4;

    /* Plain PNG (no animation chunks): single frame from IDAT. */
    if (fcount == 0) {
        growbuf spng;
        uint8_t* rgba = (uint8_t*)malloc(canvas_bytes);
        if (!rgba || !idat_default.data) { free(rgba); goto fail; }
        spng = synth_png(ihdr, canvas_w, canvas_h, idat_default.data, idat_default.size);
        if (spng.failed || !decode_frame_png(spng.data, spng.size, canvas_w, canvas_h, rgba)) {
            free(spng.data); free(rgba); goto fail;
        }
        free(spng.data);
        anim.frames = (apxl_frame*)calloc(1, sizeof(apxl_frame));
        if (!anim.frames) { free(rgba); goto fail; }
        anim.frames[0].image.buffer.data = rgba;
        anim.frames[0].image.buffer.size = canvas_bytes;
        anim.frames[0].image.width = canvas_w;
        anim.frames[0].image.height = canvas_h;
        anim.frames[0].image.channels = 4;
        anim.frames[0].image.bytes_per_channel = 1;
        anim.frames[0].delay_num = 0;
        anim.frames[0].delay_den = 0;
        anim.frame_count = 1;
        anim.canvas_w = canvas_w; anim.canvas_h = canvas_h;
        anim.channels = 4; anim.bytes_per_channel = 1;
        anim.loop_count = 0;
        anim.metadata = pxl_meta_extract(file, file_size);
        free(file);
        free(fctls);
        free(idat_default.data);
        return anim;
    }

    /* Animated: composite frames onto the canvas. Set frame_count up front, so
       that bailing out to `fail` mid-loop still lets apxl_free release the
       frames filled in so far (it iterates frame_count entries, and the array is
       calloc'd so untouched frames hold NULL buffers). */
    anim.frames = (apxl_frame*)calloc(fcount, sizeof(apxl_frame));
    anim.frame_count = fcount;
    canvas = (uint8_t*)calloc(1, canvas_bytes);
    prevbuf = (uint8_t*)malloc(canvas_bytes);
    if (!anim.frames || !canvas || !prevbuf) { goto fail; }

    for (i = 0; i < fcount; ++i) {
        frame_ctl* fc = &fctls[i];
        const unsigned char* fdata;
        size_t fdlen;
        growbuf spng;
        uint8_t* region = NULL;
        uint8_t* framebuf;
        uint32_t rx, ry;

        /* Frame image data: default IDAT for frame 0 if it owns it, else fdAT. */
        if (i == 0 && default_is_frame) {
            fdata = idat_default.data; fdlen = idat_default.size;
        } else {
            fdata = fc->data.data; fdlen = fc->data.size;
        }
        if (!fdata || fdlen == 0) { goto fail; }
        if (fc->w == 0 || fc->h == 0 ||
            (uint64_t)fc->x + fc->w > canvas_w ||
            (uint64_t)fc->y + fc->h > canvas_h) { goto fail; }

        region = (uint8_t*)malloc((size_t)fc->w * fc->h * 4);
        if (!region) { goto fail; }
        spng = synth_png(ihdr, fc->w, fc->h, fdata, fdlen);
        if (spng.failed || !decode_frame_png(spng.data, spng.size, fc->w, fc->h, region)) {
            free(spng.data); free(region); goto fail;
        }
        free(spng.data);

        /* Save canvas for DISPOSE_PREVIOUS before drawing. */
        memcpy(prevbuf, canvas, canvas_bytes);

        /* Composite region into the canvas rect (x,y,w,h). */
        for (ry = 0; ry < fc->h; ++ry) {
            uint8_t* dst = canvas + ((size_t)(fc->y + ry) * canvas_w + fc->x) * 4;
            const uint8_t* src = region + (size_t)ry * fc->w * 4;
            if (fc->blend == BLEND_OVER) {
                for (rx = 0; rx < fc->w; ++rx) {
                    uint8_t* d = dst + (size_t)rx * 4;
                    const uint8_t* s = src + (size_t)rx * 4;
                    uint32_t sa = s[3];
                    if (sa == 255) {
                        memcpy(d, s, 4);
                    } else if (sa != 0) {
                        /* Standard source-over with 8-bit alpha. */
                        uint32_t da = d[3];
                        uint32_t out_a = sa + da * (255 - sa) / 255;
                        int k;
                        if (out_a == 0) { d[0]=d[1]=d[2]=d[3]=0; }
                        else {
                            for (k = 0; k < 3; ++k) {
                                uint32_t v = (uint32_t)s[k] * sa +
                                             (uint32_t)d[k] * da * (255 - sa) / 255;
                                d[k] = (uint8_t)(v / out_a);
                            }
                            d[3] = (uint8_t)out_a;
                        }
                    }
                    /* sa == 0: leave dst unchanged */
                }
            } else {
                memcpy(dst, src, (size_t)fc->w * 4);
            }
        }
        free(region);

        /* Snapshot the composited canvas as this frame. */
        framebuf = (uint8_t*)malloc(canvas_bytes);
        if (!framebuf) { goto fail; }
        memcpy(framebuf, canvas, canvas_bytes);
        anim.frames[i].image.buffer.data = framebuf;
        anim.frames[i].image.buffer.size = canvas_bytes;
        anim.frames[i].image.width = canvas_w;
        anim.frames[i].image.height = canvas_h;
        anim.frames[i].image.channels = 4;
        anim.frames[i].image.bytes_per_channel = 1;
        anim.frames[i].delay_num = fc->delay_num;
        anim.frames[i].delay_den = fc->delay_den;

        /* Apply dispose for the next frame. */
        if (fc->dispose == DISPOSE_BACKGROUND) {
            for (ry = 0; ry < fc->h; ++ry) {
                memset(canvas + ((size_t)(fc->y + ry) * canvas_w + fc->x) * 4,
                       0, (size_t)fc->w * 4);
            }
        } else if (fc->dispose == DISPOSE_PREVIOUS) {
            memcpy(canvas, prevbuf, canvas_bytes);
        }
        /* DISPOSE_NONE: leave as-is. */
    }

    anim.canvas_w = canvas_w; anim.canvas_h = canvas_h;
    anim.channels = 4; anim.bytes_per_channel = 1;
    anim.loop_count = num_plays;
    /* Ancillary chunks live in the outer file, alongside acTL/fcTL, so they are
       properties of the animation rather than of any one frame. Extracted from
       the original bytes for the same reason as in the still path: libpng only
       reports the chunks it knows. */
    anim.metadata = pxl_meta_extract(file, file_size);

    free(canvas); free(prevbuf); free(file);
    for (i = 0; i < fcount; ++i) { free(fctls[i].data.data); }
    free(fctls);
    free(idat_default.data);
    return anim;

fail:
    free(canvas); free(prevbuf); free(file);
    if (fctls) { for (i = 0; i < fcount; ++i) { free(fctls[i].data.data); } free(fctls); }
    free(idat_default.data);
    apxl_free(&anim);
    return anim;
}

/*----------------------------------------------------------------------------
  Save: emit a valid APNG
----------------------------------------------------------------------------*/

/* Compress one full-canvas RGBA8 frame into a PNG IDAT-style zlib stream by
   letting libpng write a standalone PNG to memory, then extracting its IDAT
   payload(s) concatenated. Returns malloc'd buffer via *out (caller frees). */
static int encode_frame_idat(const uint8_t* rgba, uint32_t w, uint32_t h,
                             unsigned char** out, size_t* out_len)
{
    png_structp png = NULL;
    png_infop info = NULL;
    png_bytep* volatile rows = NULL;
    pxl_mem_writer writer;
    size_t pos;
    growbuf idat;

    memset(&writer, 0, sizeof(writer));
    memset(&idat, 0, sizeof(idat));

    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { return 0; }
    info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); return 0; }

    if (setjmp(png_jmpbuf(png))) {
        free(rows); free(writer.data); free(idat.data);
        png_destroy_write_struct(&png, &info);
        return 0;
    }

    png_set_write_fn(png, &writer, pxl_png_write_mem, pxl_png_flush_mem);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB_ALPHA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    {
        png_uint_32 y;
        rows = (png_bytep*)malloc(sizeof(png_bytep) * h);
        if (!rows) { png_longjmp(png, 1); }
        for (y = 0; y < h; ++y) {
            rows[y] = (png_bytep)(rgba + (size_t)y * w * 4);
        }
        png_write_image(png, rows);
    }
    png_write_end(png, NULL);
    free(rows); rows = NULL;
    png_destroy_write_struct(&png, &info);

    /* Extract and concatenate IDAT payloads from writer.data. */
    pos = 8;
    while (pos + 8 <= writer.size) {
        uint32_t len = pxl_get_be32(writer.data + pos);
        const unsigned char* type = writer.data + pos + 4;
        if (pos + 12 + len > writer.size) { break; }
        if (memcmp(type, "IDAT", 4) == 0) {
            gb_put(&idat, writer.data + pos + 8, len);
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + len;
    }
    free(writer.data);
    if (idat.failed || idat.size == 0) { free(idat.data); return 0; }
    *out = idat.data; *out_len = idat.size;
    return 1;
}

int apng_save(const char* path, const apxl_anim* anim)
{
    growbuf g;
    unsigned char ihdr[13], actl[8], fctl[26];
    uint32_t seq = 0, i;
    int ok = 0;

    memset(&g, 0, sizeof(g));
    if (!anim || !anim->frames || anim->frame_count == 0) { return 0; }

    gb_put(&g, PNG_SIG, 8);

    /* IHDR: canvas, RGBA8. */
    pxl_put_be32(ihdr + 0, anim->canvas_w);
    pxl_put_be32(ihdr + 4, anim->canvas_h);
    ihdr[8] = 8; ihdr[9] = PNG_COLOR_TYPE_RGB_ALPHA;
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    gb_chunk(&g, "IHDR", ihdr, 13);

    /* Preserved ancillary chunks, immediately after IHDR. Every type we keep
       (iCCP, gAMA, cHRM, sRGB, cICP, eXIf, text, ...) is legal there, and the
       ones PNG constrains -- the color-space chunks -- MUST precede IDAT, so
       this position satisfies all of them at once. Emitted before acTL to keep
       the animation control chunks adjacent to the frames they describe. */
    if (anim->metadata.data && anim->metadata.size) {
        const unsigned char* m = anim->metadata.data;
        size_t mpos = 0;
        while (mpos + 8 <= anim->metadata.size) {
            uint32_t mlen = pxl_get_le32(m + mpos + 4);
            if (mpos + 8 + (size_t)mlen > anim->metadata.size) {
                break;  /* truncated record: stop, keep what we have */
            }
            gb_chunk(&g, (const char*)(m + mpos), m + mpos + 8, mlen);
            mpos += 8 + mlen;
        }
    }

    /* acTL: num_frames, num_plays. */
    pxl_put_be32(actl + 0, anim->frame_count);
    pxl_put_be32(actl + 4, anim->loop_count);
    gb_chunk(&g, "acTL", actl, 8);

    for (i = 0; i < anim->frame_count; ++i) {
        const pxl_image* im = &anim->frames[i].image;
        unsigned char* idat = NULL;
        size_t idat_len = 0;
        uint8_t* rgba = im->buffer.data;
        uint8_t* tmp = NULL;

        if (!rgba) { goto done; }

        /* Ensure RGBA8; if the frame isn't, we don't attempt conversion here
           (apng_load always yields RGBA8, which is our normal path). */
        if (im->channels != 4 || im->bytes_per_channel != 1 ||
            im->width != anim->canvas_w || im->height != anim->canvas_h) {
            goto done;
        }

        /* fcTL: whole canvas each frame (simple, always valid). */
        pxl_put_be32(fctl + 0, seq++);
        pxl_put_be32(fctl + 4, anim->canvas_w);
        pxl_put_be32(fctl + 8, anim->canvas_h);
        pxl_put_be32(fctl + 12, 0);
        pxl_put_be32(fctl + 16, 0);
        {
            uint16_t dn = anim->frames[i].delay_num;
            uint16_t dd = anim->frames[i].delay_den;
            fctl[20] = (unsigned char)(dn >> 8); fctl[21] = (unsigned char)dn;
            fctl[22] = (unsigned char)(dd >> 8); fctl[23] = (unsigned char)dd;
        }
        fctl[24] = DISPOSE_NONE;
        fctl[25] = BLEND_SOURCE;
        gb_chunk(&g, "fcTL", fctl, 26);

        if (!encode_frame_idat(rgba, anim->canvas_w, anim->canvas_h, &idat, &idat_len)) {
            free(tmp); goto done;
        }
        free(tmp);

        if (i == 0) {
            gb_chunk(&g, "IDAT", idat, idat_len);
        } else {
            /* fdAT = seq (4 bytes) + IDAT payload. */
            unsigned char* fd = (unsigned char*)malloc(4 + idat_len);
            if (!fd) { free(idat); goto done; }
            pxl_put_be32(fd, seq++);
            memcpy(fd + 4, idat, idat_len);
            gb_chunk(&g, "fdAT", fd, 4 + idat_len);
            free(fd);
        }
        free(idat);
        if (g.failed) { goto done; }
    }

    gb_chunk(&g, "IEND", NULL, 0);
    if (g.failed) { goto done; }

    ok = pxl_spit(path, g.data, g.size);

done:
    free(g.data);
    return ok;
}

/** \file pxl_png.c
    \brief PNG <-> raw pixel interop using libpng's low-level API.

    We deliberately avoid the simplified png_image_* API because it applies a
    gamma/linear transform to 16-bit images, which is lossy. The low-level API
    lets us read the raw samples unchanged.

    I/O goes through memory buffers (custom read/write callbacks) so we can
    extract ancillary chunks from the source bytes and inject preserved chunks
    into the output bytes -- see pxl_meta.c.

    Canonicalization on read:
      - indexed (color type 3) images are kept indexed: the PLTE chunk becomes
        img.palette, tRNS becomes img.palette_alpha, and the samples stay as
        raw indices at their original 1/2/4/8-bit depth. Expanding them to RGB
        would still be lossless in color, but it would silently change the
        image's PNG type, so re-encoding could never reproduce the original
        file. The format's promise is the whole file back, palette included;
      - grayscale below 8 bits is expanded to 8;
      - a tRNS chunk on a non-indexed image is expanded to a real alpha channel.
    The result is either an indexed image (1 channel of indices plus a palette)
    or 1 (G), 2 (GA), 3 (RGB), 4 (RGBA) channels at 8 or 16 bits per channel.

    Byte order: 16-bit samples are kept in PNG's native big-endian order both
    in memory and in the .pxl payload (we never call png_set_swap). This makes
    the round-trip bit-exact and independent of host endianness.
*/
#include "pxl_png.h"
#include "pxl_format.h" /* PXL_MAX_PALETTE */
#include "pxl_meta.h"
#include "pxl_pngio.h"

#include <png.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------
  Load
----------------------------------------------------------------------------*/

/** Copy PLTE/tRNS from an indexed PNG into \p img.

    palette holds PaletteCount RGB triples; palette_alpha holds the first
    PaletteAlphaCount entries' alpha. PNG allows tRNS to be shorter than PLTE
    (missing entries are opaque), and we keep that shortened form verbatim so
    the chunk is reproduced byte for byte on write.

    \return 1 on success, 0 if the palette is malformed or allocation fails.
            Caller must longjmp/bail on 0; img keeps ownership of what it got. */
static int load_palette(png_structp png, png_infop info, pxl_image* img)
{
    png_colorp pal = NULL;
    int n = 0, i;

    if (!png_get_PLTE(png, info, &pal, &n) || !pal || n <= 0 ||
        (unsigned)n > PXL_MAX_PALETTE) {
        return 0;
    }
    img->palette.data = (unsigned char*)malloc((size_t)n * 3u);
    if (!img->palette.data) { return 0; }
    img->palette.size = (size_t)n * 3u;
    for (i = 0; i < n; ++i) {
        img->palette.data[i * 3 + 0] = pal[i].red;
        img->palette.data[i * 3 + 1] = pal[i].green;
        img->palette.data[i * 3 + 2] = pal[i].blue;
    }

    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_bytep alpha = NULL;
        int n_alpha = 0;
        if (png_get_tRNS(png, info, &alpha, &n_alpha, NULL) && alpha &&
            n_alpha > 0) {
            if (n_alpha > n) { n_alpha = n; } /* tRNS may not exceed PLTE */
            img->palette_alpha.data = (unsigned char*)malloc((size_t)n_alpha);
            if (!img->palette_alpha.data) { return 0; }
            memcpy(img->palette_alpha.data, alpha, (size_t)n_alpha);
            img->palette_alpha.size = (size_t)n_alpha;
        }
    }
    return 1;
}

pxl_image pxl_load_png(const char* path)
{
    pxl_image img;
    unsigned char* volatile file = NULL;
    volatile size_t file_size = 0;
    png_structp png = NULL;
    png_infop info = NULL;
    png_bytep* volatile rows = NULL;
    pxl_mem_reader reader;
    png_uint_32 w, h, y;
    int bit_depth, color_type;
    uint8_t channels, bpc;
    size_t stride, size;
    int indexed;

    memset(&img, 0, sizeof(img));

    {
        size_t sz = 0;
        unsigned char* tmp = pxl_slurp(path, &sz);
        file = tmp;
        file_size = sz;
    }
    if (!file || file_size < 8 || png_sig_cmp(file, 0, 8) != 0) {
        free(file);
        return img;
    }

    /* Preserve ancillary chunks (best-effort; empty on failure). */
    img.metadata = pxl_meta_extract(file, file_size);

    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { goto fail; }
    info = png_create_info_struct(png);
    if (!info) { goto fail; }

    if (setjmp(png_jmpbuf(png))) {
        goto fail;
    }

    reader.data = file;
    reader.size = file_size;
    reader.pos = 0;
    png_set_read_fn(png, &reader, pxl_png_read_mem);
    png_read_info(png, info);

    png_get_IHDR(png, info, &w, &h, &bit_depth, &color_type, NULL, NULL, NULL);

    indexed = (color_type == PNG_COLOR_TYPE_PALETTE);

    if (indexed) {
        /* Copy PLTE (and tRNS as per-entry alpha) out before any transform.
           No png_set_* expansion here: the indices are the pixel data. */
        if (!load_palette(png, info, &img)) { png_longjmp(png, 1); }
    } else {
        if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
            png_set_expand_gray_1_2_4_to_8(png);
        }
        if (png_get_valid(png, info, PNG_INFO_tRNS)) {
            png_set_tRNS_to_alpha(png);
        }
    }
    /* Interlaced (Adam7) sources must be de-interlaced, otherwise
       png_read_image writes only part of each row and the rest of the buffer is
       left as-is. */
    png_set_interlace_handling(png);

    /* Keep 16-bit big-endian (PNG native): no png_set_swap. */
    png_read_update_info(png, info);

    bit_depth = png_get_bit_depth(png, info);
    channels  = (uint8_t)png_get_channels(png, info);
    bpc       = (uint8_t)(bit_depth == 16 ? 2 : 1);

    if (indexed) {
        /* An index is a single sample, at most 8 bits deep. */
        if (channels != 1 || bit_depth > 8) { png_longjmp(png, 1); }
        img.bit_depth = (uint8_t)bit_depth;
    } else if ((bit_depth != 8 && bit_depth != 16) ||
               channels < 1 || channels > 4) {
        png_longjmp(png, 1);
    }

    /* Sub-byte indices are bit-packed MSB-first, exactly as PNG stores them,
       so let libpng state the row length rather than assuming whole bytes. */
    stride = png_get_rowbytes(png, info);
    if (stride < (size_t)((w * (png_uint_32)channels * bit_depth + 7u) / 8u)) {
        png_longjmp(png, 1);
    }
    size = stride * h;
    /* calloc, not malloc: for depths below 8 a row is padded to a whole byte and
       libpng never writes those trailing bits, so malloc would feed heap garbage
       into the filter and compressor. The pixels decode correctly either way,
       but the encoded bytes would not be reproducible. */
    img.buffer.data = (unsigned char*)calloc(size ? size : 1, 1);
    if (!img.buffer.data) { png_longjmp(png, 1); }

    rows = (png_bytep*)malloc(sizeof(png_bytep) * h);
    if (!rows) { png_longjmp(png, 1); }
    for (y = 0; y < h; ++y) {
        rows[y] = img.buffer.data + (size_t)y * stride;
    }

    png_read_image(png, rows);
    png_read_end(png, NULL);

    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    free(file);

    img.buffer.size = size;
    img.width = w;
    img.height = h;
    img.channels = channels;
    img.bytes_per_channel = bpc;
    return img;

fail:
    free(rows);
    free(img.buffer.data);
    pxl_free(&img.metadata);
    if (png) {
        png_destroy_read_struct(&png, info ? &info : NULL, NULL);
    }
    free(file);
    memset(&img, 0, sizeof(img));
    return img;
}

/*----------------------------------------------------------------------------
  Save
----------------------------------------------------------------------------*/

int pxl_save_png(const char* path, const pxl_image* img)
{
    png_structp png = NULL;
    png_infop info = NULL;
    png_bytep* rows = NULL;
    pxl_mem_writer writer;
    int color_type, ok = 0;
    png_uint_32 y;
    size_t stride;
    int indexed, depth;
    unsigned pal_count;
    png_color pal[PXL_MAX_PALETTE];

    if (!img || !img->buffer.data) { return 0; }
    if (img->bytes_per_channel != 1 && img->bytes_per_channel != 2) { return 0; }

    indexed   = pxl_is_indexed(img);
    pal_count = pxl_palette_count(img);
    depth     = pxl_bit_depth(img);

    if (indexed) {
        /* Indices are one sub-byte-or-8-bit sample; the depth bounds the
           palette (a 4-bit index cannot address entry 17). */
        if (img->channels != 1 || img->bytes_per_channel != 1) { return 0; }
        if (depth != 1 && depth != 2 && depth != 4 && depth != 8) { return 0; }
        if (pal_count == 0 || pal_count > (1u << depth)) { return 0; }
        if (img->palette_alpha.size > pal_count) { return 0; }
        color_type = PNG_COLOR_TYPE_PALETTE;
    } else if (depth != 8 && depth != 16) {
        /* Sub-byte gray has no color_type of its own here; expand first. */
        return 0;
    } else switch (img->channels) {
        case 1: color_type = PNG_COLOR_TYPE_GRAY;       break;
        case 2: color_type = PNG_COLOR_TYPE_GRAY_ALPHA; break;
        case 3: color_type = PNG_COLOR_TYPE_RGB;        break;
        case 4: color_type = PNG_COLOR_TYPE_RGB_ALPHA;  break;
        default: return 0;
    }

    memset(&writer, 0, sizeof(writer));

    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { return 0; }
    info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); return 0; }

    if (setjmp(png_jmpbuf(png))) {
        free(rows);
        free(writer.data);
        png_destroy_write_struct(&png, &info);
        return 0;
    }

    png_set_write_fn(png, &writer, pxl_png_write_mem, pxl_png_flush_mem);
    png_set_IHDR(png, info, img->width, img->height,
                 depth, color_type,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    if (indexed) {
        unsigned i;
        for (i = 0; i < pal_count; ++i) {
            pal[i].red   = img->palette.data[i * 3 + 0];
            pal[i].green = img->palette.data[i * 3 + 1];
            pal[i].blue  = img->palette.data[i * 3 + 2];
        }
        png_set_PLTE(png, info, pal, (int)pal_count);
        if (img->palette_alpha.size > 0) {
            /* libpng copies the array; it does not retain the pointer. */
            png_set_tRNS(png, info, img->palette_alpha.data,
                         (int)img->palette_alpha.size, NULL);
        }
    }
    png_write_info(png, info);
    /* 16-bit samples already big-endian (PNG native): no swap. */

    /* Bit-packed rows for sub-byte indices; whole samples otherwise. */
    stride = pxl_row_bytes(img);
    if (stride == 0 || img->buffer.size < stride * (size_t)img->height) {
        png_longjmp(png, 1); /* buffer too small for the stated geometry */
    }
    rows = (png_bytep*)malloc(sizeof(png_bytep) * img->height);
    if (!rows) { png_longjmp(png, 1); }
    for (y = 0; y < img->height; ++y) {
        rows[y] = img->buffer.data + (size_t)y * stride;
    }
    png_write_image(png, rows);
    png_write_end(png, NULL);

    free(rows);
    rows = NULL;
    png_destroy_write_struct(&png, &info);

    /* Inject preserved metadata, if any, then write to disk. */
    if (img->metadata.data && img->metadata.size) {
        pxl_buffer merged = pxl_meta_inject(writer.data, writer.size,
                                            img->metadata.data,
                                            img->metadata.size);
        if (merged.data) {
            ok = pxl_spit(path, merged.data, merged.size);
            pxl_free(&merged);
        } else {
            /* Fall back to metadata-free PNG rather than failing. */
            ok = pxl_spit(path, writer.data, writer.size);
        }
    } else {
        ok = pxl_spit(path, writer.data, writer.size);
    }

    free(writer.data);
    return ok;
}

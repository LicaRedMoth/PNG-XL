/** \file pxl_png.c
    \brief PNG <-> raw pixel interop using libpng's low-level API.

    We deliberately avoid the simplified png_image_* API because it applies a
    gamma/linear transform to 16-bit images, which is lossy. The low-level API
    lets us read the raw samples unchanged.

    I/O goes through memory buffers (custom read/write callbacks) so we can
    extract ancillary chunks from the source bytes and inject preserved chunks
    into the output bytes -- see pxl_meta.c.

    Canonicalization on read:
      - palette images are expanded to RGB(A);
      - grayscale below 8 bits is expanded to 8;
      - a tRNS chunk is expanded to a real alpha channel.
    The result is always 1 (G), 2 (GA), 3 (RGB) or 4 (RGBA) channels at 8 or
    16 bits per channel.

    Byte order: 16-bit samples are kept in PNG's native big-endian order both
    in memory and in the .pxl payload (we never call png_set_swap). This makes
    the round-trip bit-exact and independent of host endianness.
*/
#include "pxl_png.h"
#include "pxl_meta.h"
#include "pxl_pngio.h"

#include <png.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------
  Load
----------------------------------------------------------------------------*/

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

    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
    }
    /* Keep 16-bit big-endian (PNG native): no png_set_swap. */
    png_read_update_info(png, info);

    bit_depth = png_get_bit_depth(png, info);
    channels  = (uint8_t)png_get_channels(png, info);
    bpc       = (uint8_t)(bit_depth / 8);

    if ((bpc != 1 && bpc != 2) || channels < 1 || channels > 4) {
        png_longjmp(png, 1);
    }

    stride = (size_t)w * channels * bpc;
    size = stride * h;
    img.buffer.data = (unsigned char*)malloc(size ? size : 1);
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

    if (!img || !img->buffer.data) { return 0; }
    if (img->bytes_per_channel != 1 && img->bytes_per_channel != 2) { return 0; }
    switch (img->channels) {
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
                 img->bytes_per_channel * 8, color_type,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    /* 16-bit samples already big-endian (PNG native): no swap. */

    stride = (size_t)img->width * img->channels * img->bytes_per_channel;
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

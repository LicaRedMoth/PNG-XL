/** \file pxl_io.c
    \brief Endianness-independent read/write of the .pxl header.
*/
#include "pxl_format.h"
#include "pxl_bytes.h"

#include <stddef.h>

void pxl_header_write(unsigned char* out, const pxl_header* h)
{
    out[0] = PXL_MAGIC0;
    out[1] = PXL_MAGIC1;
    out[2] = PXL_MAGIC2;
    out[3] = PXL_MAGIC3;
    out[4] = h->version;
    out[5] = h->channels;
    out[6] = h->bit_depth;
    out[7] = h->color_filter;
    pxl_put_le32(out + 8, h->width);
    pxl_put_le32(out + 12, h->height);
    pxl_put_le32(out + 16, h->raw_byte_count);
    pxl_put_le32(out + 20, h->meta_byte_count);
    pxl_put_le16(out + 24, h->palette_count);
    pxl_put_le16(out + 26, h->palette_alpha_count);
}

size_t pxl_header_palette_bytes(const pxl_header* h)
{
    return (size_t)h->palette_count * 3u + (size_t)h->palette_alpha_count;
}

size_t pxl_row_bytes_of(uint32_t width, uint8_t channels, uint8_t bit_depth)
{
    uint64_t bits;

    if (width == 0 || channels < 1 || channels > 4) {
        return 0;
    }
    switch (bit_depth) {
        case 1: case 2: case 4: case 8: case 16: break;
        default: return 0;
    }
    if (bit_depth < 8 && channels != 1) {
        return 0; /* PNG has no sub-byte multi-channel layout */
    }
    /* PNG's rule: pack MSB-first and pad the row up to a whole byte. */
    bits = (uint64_t)width * channels * bit_depth;
    if ((bits + 7u) / 8u > (uint64_t)(size_t)-1) {
        return 0;
    }
    return (size_t)((bits + 7u) / 8u);
}

int pxl_header_read(const unsigned char* in, size_t in_size, pxl_header* h)
{
    if (in_size < PXL_HEADER_BYTES) {
        return 0;
    }
    if (in[0] != PXL_MAGIC0 || in[1] != PXL_MAGIC1 ||
        in[2] != PXL_MAGIC2 || in[3] != PXL_MAGIC3) {
        return 0;
    }
    h->version             = in[4];
    h->channels            = in[5];
    h->bit_depth           = in[6];
    h->color_filter        = in[7];
    h->width               = pxl_get_le32(in + 8);
    h->height              = pxl_get_le32(in + 12);
    h->raw_byte_count      = pxl_get_le32(in + 16);
    h->meta_byte_count     = pxl_get_le32(in + 20);
    h->palette_count       = pxl_get_le16(in + 24);
    h->palette_alpha_count = pxl_get_le16(in + 26);

    if (h->version != PXL_VERSION) {
        return 0;
    }
    if (h->channels < 1 || h->channels > 4) {
        return 0;
    }
    switch (h->bit_depth) {
        case 1: case 2: case 4: case 8: case 16: break;
        default: return 0;
    }
    /* Sub-byte depths exist only for single-channel PNG (gray or indexed). */
    if (h->bit_depth < 8 && h->channels != 1) {
        return 0;
    }
    if (h->palette_count > PXL_MAX_PALETTE ||
        h->palette_alpha_count > h->palette_count) {
        return 0;
    }
    /* An indexed image is 1 channel of indices, at most 8 bits deep. */
    if (h->palette_count > 0 && (h->channels != 1 || h->bit_depth > 8)) {
        return 0;
    }
    return 1;
}

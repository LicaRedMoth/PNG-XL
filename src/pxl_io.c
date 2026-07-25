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
    out[6] = h->bytes_per_channel;
    out[7] = h->color_filter;
    pxl_put_le32(out + 8, h->width);
    pxl_put_le32(out + 12, h->height);
    pxl_put_le32(out + 16, h->raw_byte_count);
    pxl_put_le32(out + 20, h->meta_byte_count);
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
    h->version           = in[4];
    h->channels          = in[5];
    h->bytes_per_channel = in[6];
    h->color_filter      = in[7];
    h->width             = pxl_get_le32(in + 8);
    h->height            = pxl_get_le32(in + 12);
    h->raw_byte_count    = pxl_get_le32(in + 16);
    h->meta_byte_count   = pxl_get_le32(in + 20);

    if (h->version != PXL_VERSION) {
        return 0;
    }
    if (h->channels < 1 || h->channels > 4) {
        return 0;
    }
    if (h->bytes_per_channel != 1 && h->bytes_per_channel != 2) {
        return 0;
    }
    return 1;
}

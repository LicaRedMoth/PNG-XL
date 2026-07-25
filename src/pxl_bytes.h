/** \file pxl_bytes.h
    \brief Internal helpers for unaligned, endian-explicit integer access.

    Container headers are little-endian; PNG chunk fields are big-endian. Both
    are read and written byte-by-byte so the code is independent of host
    endianness and alignment. Header-only so it can be shared by libpxlcore
    (zstd only) and libpxl (PNG side) without adding a translation unit.
*/
#ifndef PXL_BYTES_H
#define PXL_BYTES_H

#include <stdint.h>

/*---- little-endian: .pxl / .apxl container fields --------------------------*/

static inline void pxl_put_le16(unsigned char* p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static inline uint16_t pxl_get_le16(const unsigned char* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline void pxl_put_le32(unsigned char* p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static inline uint32_t pxl_get_le32(const unsigned char* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*---- big-endian: PNG chunk fields -----------------------------------------*/

static inline void pxl_put_be32(unsigned char* p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static inline uint32_t pxl_get_be32(const unsigned char* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint16_t pxl_get_be16(const unsigned char* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

#endif /* PXL_BYTES_H */

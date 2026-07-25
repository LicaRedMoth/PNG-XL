/** \file pxl_format.h
    \brief On-disk layout of the .pxl (PNG XL) container.

    Internal header. The container is a fixed 24-byte little-endian header
    followed by a metadata block (possibly empty) and one zstd frame of
    filtered pixels.

      offset size  field
      0      4     Magic = "PXL1"
      4      1     Version (=1)
      5      1     Channels          (1..4)
      6      1     BytesPerChannel   (1 or 2)
      7      1     ColorFilter       (see PXL_FILTER_*)
      8      4     Width  (uint32 LE)
      12     4     Height (uint32 LE)
      16     4     RawByteCount (uint32 LE) -- filtered size before compression
      20     4     MetaByteCount (uint32 LE) -- size of the metadata block
      24            metadata block (MetaByteCount bytes; see pxl_meta.c)
      24+meta        zstd frame

    The Version byte lets future revisions change this layout; this build reads
    and writes exactly version 1. All multi-byte fields are read/written
    byte-by-byte (see pxl_io.c) so the format is independent of host endianness
    and struct padding.
*/
#ifndef PXL_FORMAT_H
#define PXL_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define PXL_MAGIC0 0x50 /* 'P' */
#define PXL_MAGIC1 0x58 /* 'X' */
#define PXL_MAGIC2 0x4C /* 'L' */
#define PXL_MAGIC3 0x31 /* '1' */

#define PXL_VERSION      1  /* the one and only current format version */
#define PXL_HEADER_BYTES 24

/* Color filter modes. */
#define PXL_FILTER_DELTA    0 /* generic per-channel left delta (any pixelBytes 1..8) */
#define PXL_FILTER_BCIF     1 /* BCIF YUV transform + plane split (8-bit RGB / RGBA)   */
#define PXL_FILTER_ADAPTIVE 2 /* PNG-style per-row None/Sub/Up/Avg/Paeth (any format)  */

/* Parsed header (in-memory representation). */
typedef struct {
    uint8_t  version;
    uint8_t  channels;
    uint8_t  bytes_per_channel;
    uint8_t  color_filter;
    uint32_t width;
    uint32_t height;
    uint32_t raw_byte_count;
    uint32_t meta_byte_count;
} pxl_header;

/* Serialize a header into out[PXL_HEADER_BYTES]. */
void pxl_header_write(unsigned char* out, const pxl_header* h);

/* Parse header from in (>= PXL_HEADER_BYTES). Returns 1 on success (valid
   magic and version), 0 otherwise. */
int pxl_header_read(const unsigned char* in, size_t in_size, pxl_header* h);

#endif /* PXL_FORMAT_H */

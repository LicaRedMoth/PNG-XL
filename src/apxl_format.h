/** \file apxl_format.h
    \brief On-disk layout of the .apxl (Animated PNG XL) container, version 1.

    An .apxl file stores all animation frames as full canvases concatenated
    into a single zstd frame (with long-distance matching), which lets zstd
    exploit the large frame-to-frame redundancy of real animations. Timing is
    kept in a compact per-frame table. There is no per-frame rectangle, no
    temporal delta, and no per-frame filtering -- measurements showed those
    hurt for animation because they break cross-frame byte matches.

    Header (32 bytes, little-endian, written byte-by-byte):
      offset size  field
      0      4     Magic = "APXL"
      4      1     Version (=1)
      5      1     Channels          (1..4)
      6      1     BytesPerChannel   (1 or 2)
      7      1     Flags             (reserved, 0)
      8      4     CanvasWidth  (uint32 LE)
      12     4     CanvasHeight (uint32 LE)
      16     4     FrameCount   (uint32 LE)
      20     4     LoopCount    (uint32 LE, 0 = infinite)
      24     4     MetaByteCount (uint32 LE)
      28     4     RawByteCount (uint32 LE) -- FrameCount * canvas_bytes
      32           metadata block (MetaByteCount bytes)
      32+meta      FrameTiming[FrameCount]  -- 4 bytes each: DelayNum(2) DelayDen(2)
      ...          zstd frame (all frames' pixels concatenated, LDM)

    canvas_bytes = CanvasWidth * CanvasHeight * Channels * BytesPerChannel.
    The zstd frame decompresses to exactly RawByteCount bytes = FrameCount
    full-canvas frames, in order.
*/
#ifndef APXL_FORMAT_H
#define APXL_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define APXL_MAGIC0 0x41 /* 'A' */
#define APXL_MAGIC1 0x50 /* 'P' */
#define APXL_MAGIC2 0x58 /* 'X' */
#define APXL_MAGIC3 0x4C /* 'L' */

#define APXL_VERSION      1
#define APXL_HEADER_BYTES 32
#define APXL_TIMING_BYTES 4  /* per frame: delay_num(2) + delay_den(2) */

typedef struct {
    uint8_t  version;
    uint8_t  channels;
    uint8_t  bytes_per_channel;
    uint8_t  flags;
    uint32_t canvas_w;
    uint32_t canvas_h;
    uint32_t frame_count;
    uint32_t loop_count;
    uint32_t meta_byte_count;
    uint32_t raw_byte_count;
} apxl_file_header;

/* Serialize/parse the file header. read returns 1 on success, 0 otherwise. */
void apxl_header_write(unsigned char* out, const apxl_file_header* h);
int  apxl_header_read(const unsigned char* in, size_t in_size, apxl_file_header* h);

#endif /* APXL_FORMAT_H */

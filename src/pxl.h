/** \file pxl.h
    \brief libpxlcore - PNG XL codec: raw pixels <-> .pxl bytes.

    This is the PNG-free core API. It depends only on zstd and turns raw pixel
    buffers (plus an opaque metadata blob) into the .pxl container and back. It
    knows nothing about PNG, so it can be reused by front ends that obtain
    pixels elsewhere (e.g. an ffmpeg codec receiving decoded frames).

    The .pxl format filters pixels with a reversible color transform +
    left-neighbor delta + color-plane split (ported from Zpng by Christopher A.
    Taylor), then compresses with zstd.

    PNG file I/O lives separately in pxl_png.h (libpxl, which adds libpng).

    See LICENSE for the combined libpng / zstd / Zpng license terms.
*/
#ifndef PXL_H
#define PXL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* zstd compression levels. Default matches Zpng's choice (higher levels gain
   little but cost speed). PXL_LEVEL_MAX is zstd's ceiling; values above it are
   clamped. Levels above ~19 require zstd's "ultra" mode and much more memory. */
#define PXL_LEVEL_DEFAULT 1
#define PXL_LEVEL_MAX     22

/* A heap-allocated byte buffer owned by the library. Free with pxl_free(). */
typedef struct {
    unsigned char* data;
    size_t         size;
} pxl_buffer;

/* Raw (uncompressed) image pixels plus geometry and preserved metadata.

   Pixels are stored row-major, tightly packed: stride == width * channels *
   bytes_per_channel. 16-bit channels (bytes_per_channel == 2) are stored in
   PNG-native big-endian byte order, so round-trips are bit-exact.

   metadata holds preserved PNG ancillary chunks (EXIF, ICC, cICP, text, etc.)
   serialized as a sequence of [4-byte type][4-byte length LE][data]. It may be
   empty (data == NULL, size == 0). Release it together with buffer via
   pxl_image_free(). */
typedef struct {
    pxl_buffer buffer;            /* pixel bytes */
    pxl_buffer metadata;          /* serialized ancillary chunks (may be empty) */
    uint32_t   width;
    uint32_t   height;
    uint8_t    channels;          /* 1=G, 2=GA, 3=RGB, 4=RGBA */
    uint8_t    bytes_per_channel; /* 1 or 2 */
} pxl_image;

/*----------------------------------------------------------------------------
  Codec: raw pixels <-> .pxl file bytes
----------------------------------------------------------------------------*/

/* Encode flags for pxl_encode_ex. */
#define PXL_ENCODE_PROGRESSIVE 1u /* pick only row-wise filters (never BCIF) so the
                                     result decodes top-to-bottom via the streaming
                                     API -- useful for progressive web loading */

/* Encode raw pixels into a complete .pxl byte stream (header + zstd frame).
   zstd_level <= 0 selects PXL_LEVEL_DEFAULT.
   On success returns buffer with non-NULL data; on failure data == NULL.
   The result must be released with pxl_free(). */
pxl_buffer pxl_encode(const pxl_image* img, int zstd_level);

/* As pxl_encode, with encode flags (see PXL_ENCODE_*). pxl_encode is exactly
   pxl_encode_ex(img, zstd_level, 0). */
pxl_buffer pxl_encode_ex(const pxl_image* img, int zstd_level, unsigned flags);

/* Decode a .pxl byte stream back into raw pixels.
   On success the returned image's buffer.data is non-NULL; on failure NULL.
   Release the result with pxl_free(&image.buffer). */
pxl_image pxl_decode(pxl_buffer file);

/* Release a buffer returned by the library and null it out. */
void pxl_free(pxl_buffer* buffer);

/* Release both the pixel and metadata buffers of an image. */
void pxl_image_free(pxl_image* img);

/*----------------------------------------------------------------------------
  Streaming (progressive) decode: emit rows top-to-bottom as bytes arrive
----------------------------------------------------------------------------*/

/* Called for each fully decoded row, in order from the top. `row` points into
   the decoder's internal image buffer and stays valid until pxl_stream_free.
   Rows are delivered progressively for DELTA/ADAPTIVE files; for BCIF files all
   rows arrive at pxl_stream_finish (the plane layout is not row-progressive). */
typedef void (*pxl_row_cb)(void* user, uint32_t row_index,
                           const unsigned char* row, size_t row_bytes);

typedef struct pxl_stream pxl_stream;

/* Create a streaming decoder. cb may be NULL (rows still accumulate in the
   buffer exposed by pxl_stream_image). Returns NULL on allocation failure. */
pxl_stream* pxl_stream_new(pxl_row_cb cb, void* user);

/* Feed the next chunk of .pxl bytes (any size, even one byte at a time).
   Returns the number of newly completed rows (>= 0), or -1 on a format error. */
int pxl_stream_feed(pxl_stream* s, const void* data, size_t len);

/* The progressively filled image. Geometry is valid once the 24-byte header has
   arrived (buffer.data != NULL then). *rows_ready (if non-NULL) receives how
   many top rows are fully decoded; rows below that are not yet valid. Returns
   NULL before the header is complete. */
const pxl_image* pxl_stream_image(const pxl_stream* s, uint32_t* rows_ready);

/* Finish decoding. Returns 1 if the image is complete and valid, else 0. */
int pxl_stream_finish(pxl_stream* s);

/* Free the streaming decoder and its buffers. */
void pxl_stream_free(pxl_stream* s);

/*----------------------------------------------------------------------------
  Version
----------------------------------------------------------------------------*/

/* Returns the libpxl version string, e.g. "1.0.0". */
const char* pxl_version(void);

#ifdef __cplusplus
}
#endif

#endif /* PXL_H */

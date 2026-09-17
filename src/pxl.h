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

   Pixels are stored row-major with stride == pxl_row_bytes(img), which for the
   common 8/16-bit cases is exactly width * channels * bytes_per_channel.
   16-bit channels (bytes_per_channel == 2) are stored in PNG-native big-endian
   byte order, so round-trips are bit-exact.

   metadata holds preserved PNG ancillary chunks (EXIF, ICC, cICP, text, etc.)
   serialized as a sequence of [4-byte type][4-byte length LE][data]. It may be
   empty (data == NULL, size == 0). Release it together with buffer via
   pxl_image_free().

   Indexed (palette) images set `palette` to 3 bytes per entry (R,G,B) and use
   channels == 1, where each sample is an index into it. `palette_alpha`, if
   non-empty, holds one alpha byte per entry (PNG's tRNS for color type 3).

   `bit_depth` carries PNG's sub-byte depths: 1, 2, 4, 8 or 16. Rows are then
   packed exactly as in PNG -- see pxl_row_bytes(). bit_depth == 0 means "derive
   from bytes_per_channel" (8 or 16), so code that predates these fields and
   only sets channels/bytes_per_channel keeps working unchanged. */
typedef struct {
    pxl_buffer buffer;            /* pixel bytes */
    pxl_buffer metadata;          /* serialized ancillary chunks (may be empty) */
    pxl_buffer palette;           /* indexed images: 3 bytes/entry RGB, else empty */
    pxl_buffer palette_alpha;     /* optional 1 byte/entry alpha, else empty */
    uint32_t   width;
    uint32_t   height;
    uint8_t    channels;          /* 1=G or index, 2=GA, 3=RGB, 4=RGBA */
    uint8_t    bytes_per_channel; /* 1 or 2 */
    uint8_t    bit_depth;         /* 1,2,4,8,16; 0 = bytes_per_channel * 8 */
} pxl_image;

/* Effective bit depth: bit_depth if set, else bytes_per_channel * 8. */
uint8_t pxl_bit_depth(const pxl_image* img);

/* Number of palette entries (palette.size / 3), 0 for non-indexed images. */
unsigned pxl_palette_count(const pxl_image* img);

/* 1 if the image is indexed (has a non-empty palette), else 0. */
int pxl_is_indexed(const pxl_image* img);

/* Bytes per pixel row, PNG's rule: for depths below 8 the row is bit-packed,
   most significant bits first, and padded to a whole byte --
   (width * channels * bit_depth + 7) / 8. Returns 0 for invalid geometry. */
size_t pxl_row_bytes(const pxl_image* img);

/*----------------------------------------------------------------------------
  Codec: raw pixels <-> .pxl file bytes
----------------------------------------------------------------------------*/

/* Encode flags for pxl_encode_ex. */
#define PXL_ENCODE_PROGRESSIVE 1u /* pick only row-wise filters (never BCIF) so the
                                     result decodes top-to-bottom via the streaming
                                     API -- useful for progressive web loading */
#define PXL_ENCODE_FAST_DECODE 2u /* restrict to {NONE, DELTA} -- the two filters
                                     measured to always decode faster than libpng.
                                     BCIF and ADAPTIVE are excluded: on a real PSP,
                                     BCIF loses to libpng outright at texture sizes
                                     despite winning at screen size (see
                                     docs/BENCHMARKS.md, 2026-09-17), and ADAPTIVE
                                     only ties libpng rather than beating it. Costs
                                     more size than the default (2.9% measured on a
                                     corpus of real UI screenshots) in exchange for
                                     a decode-speed guarantee the default does not
                                     make. Implies PXL_ENCODE_PROGRESSIVE's effect
                                     (BCIF is excluded either way) but not the
                                     reverse -- PXL_ENCODE_PROGRESSIVE alone still
                                     allows ADAPTIVE. */

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

/* Release the pixel, metadata and palette buffers of an image. */
void pxl_image_free(pxl_image* img);

/*----------------------------------------------------------------------------
  Palette expansion (convenience for front ends that want real color)
----------------------------------------------------------------------------*/

/* Expand an indexed or sub-8-bit image into a plain 8-bit image: indexed
   becomes RGB (3 channels) or RGBA (4, if palette_alpha is present), and
   1/2/4-bit gray becomes 8-bit gray scaled to the full range, as libpng's
   png_set_palette_to_rgb / png_set_expand_gray_1_2_4_to_8 do.

   Images that are already 8- or 16-bit non-indexed are returned as a plain deep
   copy, so callers can use this unconditionally. Metadata is not copied, since
   chunks such as sBIT and bKGD do not survive the expansion. On success
   buffer.data != NULL; release with pxl_image_free(). */
pxl_image pxl_image_expand(const pxl_image* img);

/*----------------------------------------------------------------------------
  Streaming (progressive) decode: emit rows top-to-bottom as bytes arrive
----------------------------------------------------------------------------*/

/* Called for each fully decoded row, in order from the top. Under
   PXL_OUTPUT_NATIVE (the default), `row` points into the decoder's internal
   image buffer and stays valid until pxl_stream_free; under any other output
   format, `row` points at a per-stream scratch buffer holding that row
   already converted, valid only until the next call. Rows are delivered
   progressively for DELTA/ADAPTIVE files; for BCIF files all rows arrive at
   pxl_stream_finish (the plane layout is not row-progressive). */
typedef void (*pxl_row_cb)(void* user, uint32_t row_index,
                           const unsigned char* row, size_t row_bytes);

/* Row format for the streaming callback. NATIVE hands back whatever the
   source image's own channels/bit_depth already are -- no conversion, no
   extra buffer, the long-standing default. The others convert each row to a
   fixed packed layout before the callback sees it: useful for handing rows
   straight to a GPU that reads its own native texture format (the PSP GE
   reads 5650/5551/4444/8888, none of which is what an 8-bit RGB(A) file
   decodes to) without every caller writing and maintaining that conversion
   itself. This never touches the file: the bytes on disk are exactly as
   lossless as they always were, and pxl_stream_image() still returns the
   true decoded (native) pixels regardless of what the row callback sees --
   only the callback's copy is converted, because the decoder's internal
   per-row predictors (ADAPTIVE's Paeth etc.) need the real 8-bit values to
   stay correct from row to row.

   Only defined for 8-bit, non-indexed RGB (3 channel) or RGBA (4 channel)
   source images -- pxl_stream_new_ex still returns a valid stream for
   anything else, but pxl_stream_feed fails once the header says otherwise,
   the same way any other geometry mismatch is reported. A 3-channel source
   asked for an alpha-carrying format is filled fully opaque. */
typedef enum {
    PXL_OUTPUT_NATIVE = 0,
    PXL_OUTPUT_RGBA8888,
    PXL_OUTPUT_RGB565,
    PXL_OUTPUT_RGBA5551,
    PXL_OUTPUT_RGBA4444
} pxl_output_format;

/* Converts an indexed image's palette into a packed CLUT in the same layout
   pxl_stream_new_ex's row conversion uses -- the PSP GE's texture formats
   (5650/5551/4444/8888) are also its only palette formats, and the index
   bytes an indexed .pxl decodes to already need no conversion at all (they
   are exactly what GU_PSM_T4/T8 read), so a palette this size is the entire
   remaining gap between an indexed .pxl and a texture the GE can sample
   directly. Unlike row conversion this is not part of the streaming API: a
   palette arrives once, in full, before any pixel rows, so there is nothing
   progressive about converting it -- call it once img->palette is populated
   (from pxl_decode(), or from pxl_stream_image() once its geometry is valid)
   and reuse the result for as many draws as the palette is used in.

   `fmt` must not be PXL_OUTPUT_NATIVE (there is no "native packed CLUT"
   layout to pass through, unlike a pixel row -- RGB/palette entries are
   always 3 or 4 plain bytes on disk). `out` must hold
   pxl_palette_count(img) * (fmt == PXL_OUTPUT_RGBA8888 ? 4 : 2) bytes.
   Entries beyond palette_alpha's length (or when it is empty) read fully
   opaque, the same default pxl_stream_new_ex's row conversion uses for a
   3-channel source. Returns the number of bytes written, or 0 if img is not
   indexed or fmt is PXL_OUTPUT_NATIVE. */
size_t pxl_convert_palette(const pxl_image* img, pxl_output_format fmt,
                           unsigned char* out);

typedef struct pxl_stream pxl_stream;

/* Create a streaming decoder. cb may be NULL (rows still accumulate in the
   buffer exposed by pxl_stream_image). Returns NULL on allocation failure. */
pxl_stream* pxl_stream_new(pxl_row_cb cb, void* user);

/* As pxl_stream_new, with row_cb's output converted to `fmt` (see
   pxl_output_format). pxl_stream_new(cb, user) is exactly
   pxl_stream_new_ex(cb, user, PXL_OUTPUT_NATIVE). */
pxl_stream* pxl_stream_new_ex(pxl_row_cb cb, void* user, pxl_output_format fmt);

/* Feed the next chunk of .pxl bytes (any size, even one byte at a time).
   Returns the number of newly completed rows (>= 0), or -1 on a format error. */
int pxl_stream_feed(pxl_stream* s, const void* data, size_t len);

/* The progressively filled image. Geometry is valid once the fixed header,
   palette and metadata have arrived (buffer.data != NULL then). *rows_ready (if non-NULL) receives how
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

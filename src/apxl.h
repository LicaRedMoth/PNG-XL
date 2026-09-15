/** \file apxl.h
    \brief libapxl - Animated PNG XL: lossless animation on top of the PXL core.

    An .apxl file stores an animation as a canvas plus a sequence of frames.
    Each frame is a full-canvas RGBA(-or-less) image with timing. All frames are
    concatenated and compressed as one zstd stream with long-distance matching,
    so the compressor exploits the redundancy between successive frames. This
    measurably beats per-frame streams, temporal deltas, and per-frame spatial
    filtering, all of which destroy the byte-level matches across frames.

    This is the PNG-free animation core: it depends only on the PXL core (pxl.h),
    not on libpng. APNG file interop lives in apng.h (libpxl).
*/
#ifndef APXL_H
#define APXL_H

#include "pxl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default zstd level for animation. Unlike still images, animation relies on
   long-distance matching across frames, which the codec only enables at level
   >= 10, so the still-image default (1) would compress poorly here. */
#define APXL_LEVEL_DEFAULT 12

/* One animation frame: a full-canvas image plus its display duration. The image
   dimensions always equal the canvas dimensions -- APNG's per-frame offsets and
   dispose/blend ops are resolved by the front end at load time, so playback
   needs nothing but the pixels and the delay. */
typedef struct {
    pxl_image image;      /* full canvas frame. Owns its buffer only when the
                             animation's `storage` is empty -- see apxl_anim. */
    uint16_t  delay_num;  /* display time = delay_num/delay_den seconds */
    uint16_t  delay_den;  /* 0 is treated as 100 per the APNG spec */
} apxl_frame;

/* A decoded animation. frames[0..frame_count-1] are full canvas frames. */
typedef struct {
    apxl_frame* frames;
    uint32_t    frame_count;
    uint32_t    loop_count;   /* 0 = infinite */
    uint32_t    canvas_w;
    uint32_t    canvas_h;
    uint8_t     channels;         /* 1..4 */
    uint8_t     bytes_per_channel;/* 1 or 2 */
    pxl_buffer  metadata;         /* preserved ancillary chunks (may be empty) */
    /* Frames decoded by apxl_decode all point into this one block rather than
       owning separate buffers: the decompressed stream is already the frame
       sequence, so copying each frame out of it doubled peak memory for no
       gain. apxl_free releases the block and leaves the frame buffers alone.
       Animations assembled by hand (apng_load, the encoder's callers) leave
       this empty and keep owning their frames individually, so both shapes
       free correctly. */
    pxl_buffer  storage;
} apxl_anim;

/* Encode an animation into a complete .apxl byte stream.
   zstd_level <= 0 selects PXL_LEVEL_DEFAULT; values above PXL_LEVEL_MAX clamp.
   On success returns buffer with non-NULL data; on failure data == NULL.
   Release the result with pxl_free(). */
pxl_buffer apxl_encode(const apxl_anim* anim, int zstd_level);

/* Decode a .apxl byte stream into a full-canvas frame sequence.
   On success frames != NULL and frame_count > 0; on failure frames == NULL.
   Release with apxl_free(). */
apxl_anim apxl_decode(pxl_buffer file);

/* Release all frames, their images, and metadata; zero the struct. */
void apxl_free(apxl_anim* anim);

#ifdef __cplusplus
}
#endif

#endif /* APXL_H */

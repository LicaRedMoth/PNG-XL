/** \file gif.h
    \brief GIF file interop for the animated PXL codec -- PNG-free, no external
           dependency (GIF's own LZW variant is decoded directly, unlike PNG's
           DEFLATE which comes from zlib).

    Added 2026-09-18 once indexed .apxl existed to receive it (see
    docs/RESEARCH.md's "Indexed .apxl" entry) -- a GIF is always <=256
    colours/frame by format definition, exactly the content class that entry
    measured. Every frame is composited to a full-canvas RGBA8 apxl_anim,
    mirroring apng_load(); apxl_anim_try_index() is what turns that back into
    an indexed .apxl when the composited result still fits one global
    palette, the same way an indexed APNG source already does. This module
    does not special-case "the source was already indexed" to skip that
    round trip -- see the file comment in gif.c for why that generalization
    was not worth its complexity for a first version.
*/
#ifndef GIF_H
#define GIF_H

#include "apxl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load a GIF (87a or 89a, static or animated) into a full-canvas RGBA8 frame
   sequence, compositing per-frame disposal/transparency the same way
   apng_load() composites APNG's dispose/blend. A static GIF yields a
   single-frame animation. On failure frames == NULL. Release with
   apxl_free(). */
apxl_anim gif_load(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* GIF_H */

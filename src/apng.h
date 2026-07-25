/** \file apng.h
    \brief APNG file interop for the animated PXL codec (libpng + zlib).

    libpng does not decode APNG natively, so we parse the acTL/fcTL/fdAT chunks
    ourselves, decode each frame's image data through libpng, and composite onto
    an RGBA canvas per the APNG dispose/blend rules. The result is a sequence of
    full-canvas frames (apxl_anim). Saving does the reverse: emit a valid APNG.

    All frames are canonicalized to RGBA 8-bit (channels=4, bpc=1), which is the
    common denominator for compositing with blend/dispose.
*/
#ifndef APNG_H
#define APNG_H

#include "apxl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load an APNG (or plain PNG) into a full-canvas frame sequence.
   A plain PNG yields a single-frame animation. On failure frames == NULL.
   Release with apxl_free(). */
apxl_anim apng_load(const char* path);

/* Write a full-canvas frame sequence as a valid APNG file.
   Returns 1 on success, 0 on failure. */
int apng_save(const char* path, const apxl_anim* anim);

#ifdef __cplusplus
}
#endif

#endif /* APNG_H */

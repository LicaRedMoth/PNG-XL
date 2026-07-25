/** \file pxl_png.h
    \brief libpxl - PNG file interop for the PXL codec (libpng + zlib).

    This is the PNG front end layered on top of the PNG-free core (pxl.h). It
    loads a PNG into raw pixels and preserved ancillary metadata, and writes
    raw pixels (plus metadata) back out as a PNG. Requires libpng and zlib.
*/
#ifndef PXL_PNG_H
#define PXL_PNG_H

#include "pxl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load a PNG file into raw pixels via libpng. Preserved ancillary chunks are
   returned in img.metadata. On success buffer.data is non-NULL; on failure
   NULL. Release with pxl_image_free(). */
pxl_image pxl_load_png(const char* path);

/* Save raw pixels (and any img.metadata chunks) to a PNG file via libpng.
   Returns 1 on success, 0 on failure. */
int pxl_save_png(const char* path, const pxl_image* img);

#ifdef __cplusplus
}
#endif

#endif /* PXL_PNG_H */

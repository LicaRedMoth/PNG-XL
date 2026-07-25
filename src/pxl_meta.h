/** \file pxl_meta.h
    \brief Preserve PNG ancillary chunks across a .pxl round-trip.

    The metadata block stored in a .pxl file is a flat sequence of records:
        [4 bytes chunk type][4 bytes data length, LE][length bytes of data]
    repeated. Chunk types are the raw PNG type codes (e.g. "eXIf", "iCCP").

    We preserve every ancillary chunk EXCEPT those tied to the original pixel
    layout, which our canonicalization (palette/tRNS/sub-8-bit expansion) makes
    invalid: PLTE, tRNS, sBIT, bKGD, hIST. IHDR/IDAT/IEND are structural and
    are never stored. Everything else -- eXIf, iCCP, cICP, gAMA, cHRM, sRGB,
    pHYs, tIME, tEXt, zTXt, iTXt, and any unknown ancillary chunk -- is copied
    byte-for-byte in original order.
*/
#ifndef PXL_META_H
#define PXL_META_H

#include "pxl.h"

#include <stddef.h>

/* Scan a PNG file's bytes and serialize preserved ancillary chunks into a
   freshly allocated buffer (see format above). On success returns a buffer
   that may be empty (data == NULL, size == 0 if nothing to preserve). On
   allocation failure returns {NULL, 0} as well; callers treat "no metadata"
   and "failure" identically (metadata is best-effort). */
pxl_buffer pxl_meta_extract(const unsigned char* png_data, size_t png_size);

/* Given a PNG file produced by libpng in `base`, return a new PNG byte stream
   with the preserved chunks from `meta` inserted just before IDAT (or, for
   chunks that must follow IDAT such as tIME/text, in a position PNG allows).
   Returns {NULL,0} on failure; caller should then fall back to `base`. */
pxl_buffer pxl_meta_inject(const unsigned char* base_png, size_t base_size,
                           const unsigned char* meta, size_t meta_size);

#endif /* PXL_META_H */

/** \file pattern.h
    \brief The one formula behind every embedded test image.

    Shared by bench/mkpsptest.c (host, generates the images) and main.c
    (target, re-derives the expected pixels to check a decode against)
    so the reference is never embedded as data at all -- only this formula
    is, and both sides compute it the same way. That drops the generated
    psp/testdata.h from ~9.6 MB to under 1.5 MB: the dominant cost was the
    raw reference pixels (1.57 MB per screen+texture pair, ~5x as hex text),
    not the compressed .pxl files actually being tested.

    Plain 8-bit wraparound integer arithmetic, so it is bit-identical on any
    architecture -- no float, no rounding, nothing for x86 and MIPS to
    disagree on.
*/
#ifndef PXL_PSP_PATTERN_H
#define PXL_PSP_PATTERN_H

#include <stdint.h>

/* r: diagonal ramp, g: coarse 16px block checkerboard (edges like a UI panel
   border, not per-pixel noise -- see bench/mkpsptest.c for why that matters
   for a throughput number), b: quadratic curve, a: constant. */
static void pxl_psp_pattern_pixel(uint32_t x, uint32_t y, uint8_t out[4])
{
    out[0] = (uint8_t)(x * 3 + y * 5);
    out[1] = (uint8_t)(((x / 16) ^ (y / 16)) * 16);
    out[2] = (uint8_t)(x * x + y * y);
    out[3] = 255;
}

#endif

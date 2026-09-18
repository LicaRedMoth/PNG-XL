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

/* Animated indexed test content (added for the "Animated .apxl texture
   playback on PSP" / "PSP/GE path" ROADMAP items): a fixed, hand-picked
   16-colour palette and a deterministic per-frame index formula, shared
   between bench/mkpsptest.c (which builds the embedded indexed .apxl from
   it) and psp/main.c (which re-derives the same expected index bytes and
   palette to check a decode against -- same "formula, not data" reasoning
   as pxl_psp_pattern_pixel above). Deliberately palette-friendly (16 flat
   colours, an 8px block pattern) rather than reusing the gradient pattern
   above, which has far more than 256 unique colours -- this is meant to
   read like a small scrolling UI/sprite animation, the content class the
   2026-09-18 indexed-animation corpus measurement found zero misses on. */
#define PXL_PSP_ANIM_PALETTE_COUNT 16
#define PXL_PSP_ANIM_W 64
#define PXL_PSP_ANIM_H 64
#define PXL_PSP_ANIM_FRAMES 8

static const uint8_t pxl_psp_anim_palette[PXL_PSP_ANIM_PALETTE_COUNT][4] = {
    { 220,  20,  60, 255 }, {  30, 144, 255, 255 }, {  50, 205,  50, 255 },
    { 255, 215,   0, 255 }, { 148,   0, 211, 255 }, { 255, 140,   0, 255 },
    {  64, 224, 208, 255 }, { 255,  20, 147, 255 }, { 139,  69,  19, 255 },
    { 105, 105, 105, 255 }, { 240, 230, 140, 255 }, {   0, 100,   0, 255 },
    { 176, 196, 222, 255 }, { 178,  34,  34, 255 }, {  70, 130, 180, 255 },
    { 245, 245, 245, 255 }
};

static uint8_t pxl_psp_anim_index(uint32_t x, uint32_t y, uint32_t frame)
{
    return (uint8_t)(((x / 8) + (y / 8) + frame) % PXL_PSP_ANIM_PALETTE_COUNT);
}

#endif

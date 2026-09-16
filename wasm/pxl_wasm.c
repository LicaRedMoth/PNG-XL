/** \file pxl_wasm.c
    \brief Flat entry points for the wasm32 build.

    pxl_decode() returns a struct by value and takes one by value, which is
    awkward across the WASM boundary. This flattens it: the host allocates an
    input buffer inside linear memory, hands back its offset, and reads the
    results through scalar getters.

    Two decode entry points on purpose. The native one hands back whatever the
    file holds -- 1/2/4-bit gray, indexed, 16-bit -- which is what a converter
    wants. The RGBA one expands to 8-bit RGBA, which is what a <canvas> needs
    and what the browser would otherwise have to do in JavaScript.
*/

#include "../src/pxl.h"

static pxl_image g_img;
static pxl_image g_rgba;

void pxl_wasm_reset(void);   /* shim.c */

/* Host-side: reserve room for the compressed file and write into it. */
unsigned char* pxl_wasm_alloc(int n)
{
    extern void* malloc(unsigned long);
    return (unsigned char*)malloc((unsigned long)(n > 0 ? n : 1));
}

static void clear(void)
{
    /* The arena is reclaimed wholesale, so the images only need forgetting. */
    g_img.buffer.data = 0;  g_img.buffer.size = 0;
    g_rgba.buffer.data = 0; g_rgba.buffer.size = 0;
}

/* Decode into the file's own pixel layout. Returns 1 on success. */
int pxl_wasm_decode(const unsigned char* data, int len)
{
    pxl_buffer file;
    if (!data || len <= 0) { return 0; }
    file.data = (unsigned char*)data;
    file.size = (unsigned long)len;
    g_img = pxl_decode(file);
    return g_img.buffer.data != 0;
}

/* Decode and expand to 8-bit RGBA, ready for ImageData. Returns 1 on success. */
int pxl_wasm_decode_rgba(const unsigned char* data, int len)
{
    pxl_image ex;
    if (!pxl_wasm_decode(data, len)) { return 0; }
    if (g_img.channels == 4 && g_img.bytes_per_channel == 1 &&
        pxl_bit_depth(&g_img) == 8 && !g_img.palette.data) {
        g_rgba = g_img;                 /* already exactly what canvas wants */
        return 1;
    }
    ex = pxl_image_expand(&g_img);
    if (!ex.buffer.data) { return 0; }
    g_rgba = ex;
    return 1;
}

int            pxl_wasm_width(void)     { return (int)g_img.width; }
int            pxl_wasm_height(void)    { return (int)g_img.height; }
int            pxl_wasm_channels(void)  { return (int)g_img.channels; }
int            pxl_wasm_depth(void)     { return (int)pxl_bit_depth(&g_img); }
unsigned char* pxl_wasm_pixels(void)    { return g_img.buffer.data; }
int            pxl_wasm_pixel_bytes(void){ return (int)g_img.buffer.size; }

/* The expanded buffer: channels are 1 (gray) or 3/4 after a palette. */
unsigned char* pxl_wasm_rgba(void)      { return g_rgba.buffer.data; }
int            pxl_wasm_rgba_bytes(void){ return (int)g_rgba.buffer.size; }
int            pxl_wasm_rgba_channels(void) { return (int)g_rgba.channels; }

/* Drop everything and reclaim the arena. Call between images. */
void pxl_wasm_free(void) { clear(); pxl_wasm_reset(); }

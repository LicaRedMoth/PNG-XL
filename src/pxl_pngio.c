/** \file pxl_pngio.c
    \brief Implementation of shared memory-backed libpng I/O helpers.
*/
#include "pxl_pngio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned char* pxl_slurp(const char* path, size_t* out_size)
{
    FILE* f = fopen(path, "rb");
    unsigned char* buf;
    long n;

    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (unsigned char*)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

int pxl_spit(const char* path, const unsigned char* data, size_t size)
{
    FILE* f = fopen(path, "wb");
    if (!f) { return 0; }
    if (fwrite(data, 1, size, f) != size) { fclose(f); return 0; }
    fclose(f);
    return 1;
}

void pxl_png_read_mem(png_structp png, png_bytep out, size_t n)
{
    pxl_mem_reader* r = (pxl_mem_reader*)png_get_io_ptr(png);
    if (r->pos + n > r->size) {
        png_error(png, "read past end of memory buffer");
        return;
    }
    memcpy(out, r->data + r->pos, n);
    r->pos += n;
}

void pxl_png_write_mem(png_structp png, png_bytep in, size_t n)
{
    pxl_mem_writer* w = (pxl_mem_writer*)png_get_io_ptr(png);
    if (w->size + n > w->cap) {
        size_t ncap = w->cap ? w->cap * 2 : 65536;
        unsigned char* nd;
        while (ncap < w->size + n) { ncap *= 2; }
        nd = (unsigned char*)realloc(w->data, ncap);
        if (!nd) { w->failed = 1; png_error(png, "oom"); return; }
        w->data = nd;
        w->cap = ncap;
    }
    memcpy(w->data + w->size, in, n);
    w->size += n;
}

void pxl_png_flush_mem(png_structp png) { (void)png; }

/** \file pxl_pngio.h
    \brief Shared internal helpers for memory-backed libpng I/O and whole-file
           read/write. Used by pxl_png.c (single images) and apng.c (animation).
*/
#ifndef PXL_PNGIO_H
#define PXL_PNGIO_H

#include <png.h>
#include <stddef.h>

/* Read an entire file into a freshly malloc'd buffer. Returns NULL on failure;
   on success *out_size holds the byte count. Caller frees. */
unsigned char* pxl_slurp(const char* path, size_t* out_size);

/* Write size bytes to path. Returns 1 on success, 0 on failure. */
int pxl_spit(const char* path, const unsigned char* data, size_t size);

/* libpng read callback over an in-memory buffer. Set as io_ptr a pxl_mem_reader. */
typedef struct {
    const unsigned char* data;
    size_t size;
    size_t pos;
} pxl_mem_reader;

void pxl_png_read_mem(png_structp png, png_bytep out, size_t n);

/* libpng write callback into a growable in-memory buffer. Set as io_ptr a
   pxl_mem_writer (zero-initialize before use; free .data afterwards). */
typedef struct {
    unsigned char* data;
    size_t size;
    size_t cap;
    int failed;
} pxl_mem_writer;

void pxl_png_write_mem(png_structp png, png_bytep in, size_t n);
void pxl_png_flush_mem(png_structp png);

#endif /* PXL_PNGIO_H */

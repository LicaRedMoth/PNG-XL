/* Minimal libpng decoder: file -> raw pixels, nothing else.
   The libpng-side counterpart of mindec_pxl.c, for comparing decoder weight. */
#include <stdio.h>
#include <stdlib.h>

#include <png.h>

int main(int argc, char** argv) {
    if (argc < 2) return 2;

    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(f); return 1; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(f); return 1; }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return 1;
    }

    png_init_io(png, f);
    png_read_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);

    png_uint_32 w = png_get_image_width(png, info);
    png_uint_32 h = png_get_image_height(png, info);
    size_t      rb = png_get_rowbytes(png, info);
    png_bytepp  rows = png_get_rows(png, info);

    unsigned long long sum = 0;
    for (png_uint_32 y = 0; y < h; y++)
        for (size_t x = 0; x < rb; x++) sum += rows[y][x];
    printf("%ux%u sum=%llu\n", w, h, sum);

    png_destroy_read_struct(&png, &info, NULL);
    fclose(f);
    return 0;
}

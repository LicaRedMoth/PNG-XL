/** \file dumpfiltered.c
    \brief Writes out the byte stream the encoder hands to zstd, so a zstd
           dictionary can be trained on it.

    Training a dictionary on .png or .pxl files would be useless: the first is a
    different container and the second is already compressed. What zstd actually
    sees is the *filtered* stream -- rows after the colour filter, before
    compression -- and that is what this dumps.

    apply_filter is static inside the codec, so the translation unit is included
    directly, the same way bench/stages.c and bench/encstages.c do it rather
    than widening the public API for a benchmark.

    Usage:
      bench/dumpfiltered <image.png> <out.bin> <filter>

    where filter is delta|bcif|adaptive|none, normally taken from what
    `pxltool info` reports for that file, so the training material matches what
    the encoder really produces.
*/

#include "../src/pxl_codec_encode.c"

#include "../src/pxl_png.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* `pxltool info` prints the filter name capitalised for BCIF and lowercase for
   the rest, so match without regard to case: taking the name verbatim silently
   dropped every BCIF file from a corpus once already. */
static int name_is(const char* a, const char* b)
{
    size_t i;
    for (i = 0; a[i] && b[i]; ++i) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) { return 0; }
    }
    return a[i] == b[i];
}

int main(int argc, char** argv)
{
    pxl_image img;
    pxl_geometry g;
    uint8_t* filtered;
    size_t fsize, want;
    uint8_t filter;
    FILE* out;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <image.png> <out.bin> <delta|bcif|adaptive|none>\n", argv[0]);
        return 2;
    }
    if      (name_is(argv[3], "delta"))    { filter = PXL_FILTER_DELTA; }
    else if (name_is(argv[3], "bcif"))     { filter = PXL_FILTER_BCIF; }
    else if (name_is(argv[3], "adaptive")) { filter = PXL_FILTER_ADAPTIVE; }
    else if (name_is(argv[3], "none"))     { filter = PXL_FILTER_NONE; }
    else { fprintf(stderr, "unknown filter '%s'\n", argv[3]); return 2; }

    img = pxl_load_png(argv[1]);
    if (!img.buffer.data) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }

    if (!pxl_geometry_of(img.width, img.height, img.channels,
                         img.bit_depth ? img.bit_depth : 8, &g)) {
        fprintf(stderr, "bad geometry\n"); pxl_image_free(&img); return 1;
    }
    /* BCIF is 8-bit RGB/RGBA only; asking for it elsewhere would read past the
       buffer in the plane split, exactly as the decoder guards against. */
    if (filter == PXL_FILTER_BCIF &&
        (img.bytes_per_channel != 1 || (img.channels != 3 && img.channels != 4))) {
        fprintf(stderr, "bcif not applicable here\n"); pxl_image_free(&img); return 1;
    }

    want = pxl_filtered_size(filter, g.filter_width, img.height, g.pixel_bytes);
    filtered = (uint8_t*)malloc(want ? want : 1);
    if (!filtered) { pxl_image_free(&img); return 1; }

    fsize = apply_filter(filter, g.pixel_bytes, img.buffer.data, filtered,
                         g.filter_width, img.height);
    if (fsize != want) {
        fprintf(stderr, "filtered size mismatch\n");
        free(filtered); pxl_image_free(&img); return 1;
    }

    out = fopen(argv[2], "wb");
    if (!out) { free(filtered); pxl_image_free(&img); return 1; }
    fwrite(filtered, 1, fsize, out);
    fclose(out);

    free(filtered);
    pxl_image_free(&img);
    return 0;
}

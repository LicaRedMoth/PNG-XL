/** \file pxltool.c
    \brief Command-line front end for the PNG XL (.pxl) codec.

    Usage:
      pxltool c    in.png  out.pxl [-l LEVEL]   compress PNG -> PXL
      pxltool d    in.pxl  out.png              decompress PXL -> PNG
      pxltool info in.pxl                       print .pxl header + stats
*/
#include "pxl.h"
#include "pxl_png.h"
#include "pxl_format.h"
#include "apxl.h"
#include "apxl_format.h"
#include "apng.h"
#include "gif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char* read_file(const char* path, size_t* out_size)
{
    FILE* f = fopen(path, "rb");
    unsigned char* buf;
    long n;

    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    buf = (unsigned char*)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

static int write_file(const char* path, const unsigned char* data, size_t size)
{
    FILE* f = fopen(path, "wb");
    if (!f) {
        return 0;
    }
    if (fwrite(data, 1, size, f) != size) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

static long file_size(const char* path)
{
    FILE* f = fopen(path, "rb");
    long n;
    if (!f) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

static int cmd_compress(const char* in, const char* out, int level, unsigned flags)
{
    pxl_image img;
    pxl_buffer enc;
    long src_size;

    img = pxl_load_png(in);
    if (!img.buffer.data) {
        fprintf(stderr, "error: could not load PNG '%s'\n", in);
        return 1;
    }

    enc = pxl_encode_ex(&img, level, flags);
    if (!enc.data) {
        fprintf(stderr, "error: encoding failed\n");
        pxl_image_free(&img);
        return 1;
    }

    if (!write_file(out, enc.data, enc.size)) {
        fprintf(stderr, "error: could not write '%s'\n", out);
        pxl_free(&enc);
        pxl_image_free(&img);
        return 1;
    }

    src_size = file_size(in);
    printf("%s -> %s\n", in, out);
    printf("  %ux%u, %u channel(s), %u-bit\n",
           img.width, img.height, img.channels, img.bytes_per_channel * 8);
    if (flags & PXL_ENCODE_FAST_DECODE) {
        printf("  fast decode    : yes ({none, delta} only -- also streams top-to-bottom)\n");
    } else if (flags & PXL_ENCODE_PROGRESSIVE) {
        printf("  progressive    : yes (row-wise filter only)\n");
    }
    if (img.metadata.size) {
        printf("  metadata       : %zu bytes preserved\n", img.metadata.size);
    }
    if (src_size > 0) {
        printf("  PNG %ld bytes -> PXL %zu bytes (%.1f%%)\n",
               src_size, enc.size, 100.0 * (double)enc.size / (double)src_size);
    } else {
        printf("  PXL %zu bytes\n", enc.size);
    }

    pxl_free(&enc);
    pxl_image_free(&img);
    return 0;
}

static int cmd_decompress(const char* in, const char* out)
{
    unsigned char* file;
    size_t file_sz;
    pxl_buffer fb;
    pxl_image img;

    file = read_file(in, &file_sz);
    if (!file) {
        fprintf(stderr, "error: could not read '%s'\n", in);
        return 1;
    }

    fb.data = file;
    fb.size = file_sz;
    img = pxl_decode(fb);
    free(file);

    if (!img.buffer.data) {
        fprintf(stderr, "error: not a valid .pxl file or decode failed\n");
        return 1;
    }

    if (!pxl_save_png(out, &img)) {
        fprintf(stderr, "error: could not write PNG '%s'\n", out);
        pxl_image_free(&img);
        return 1;
    }

    printf("%s -> %s\n", in, out);
    printf("  %ux%u, %u channel(s), %u-bit\n",
           img.width, img.height, img.channels, img.bytes_per_channel * 8);
    if (img.metadata.size) {
        printf("  metadata       : %zu bytes restored\n", img.metadata.size);
    }

    pxl_image_free(&img);
    return 0;
}

static int cmd_info(const char* in)
{
    unsigned char* file;
    size_t file_sz;
    pxl_header h;

    file = read_file(in, &file_sz);
    if (!file) {
        fprintf(stderr, "error: could not read '%s'\n", in);
        return 1;
    }
    if (!pxl_header_read(file, file_sz, &h)) {
        fprintf(stderr, "error: '%s' is not a valid .pxl file\n", in);
        free(file);
        return 1;
    }

    printf("%s\n", in);
    printf("  format version : %u\n", h.version);
    printf("  dimensions     : %u x %u\n", h.width, h.height);
    printf("  channels       : %u\n", h.channels);
    printf("  bit depth      : %u-bit\n", h.bit_depth);
    if (h.palette_count) {
        printf("  palette        : %u entries", h.palette_count);
        if (h.palette_alpha_count) {
            printf(" (+%u alpha)", h.palette_alpha_count);
        }
        printf("\n");
    } else {
        printf("  palette        : none (truecolor/grayscale)\n");
    }
    printf("  color filter   : %s\n",
           h.color_filter == PXL_FILTER_BCIF     ? "BCIF (YUV + planes)" :
           h.color_filter == PXL_FILTER_ADAPTIVE ? "adaptive (PNG-style per-row)" :
           h.color_filter == PXL_FILTER_NONE     ? "none (pixels stored verbatim)" :
                                                   "delta");
    printf("  progressive    : %s\n",
           h.color_filter == PXL_FILTER_BCIF
               ? "no (BCIF planes need the whole frame)"
               : "yes (decodes top-to-bottom)");
    printf("  metadata       : %u bytes\n", h.meta_byte_count);
    printf("  raw size       : %u bytes\n", h.raw_byte_count);
    printf("  file size      : %zu bytes\n", file_sz);
    if (h.raw_byte_count > 0) {
        printf("  ratio          : %.1f%% of raw\n",
               100.0 * (double)file_sz / (double)h.raw_byte_count);
    }

    free(file);
    return 0;
}

/*----------------------------------------------------------------------------
  Animation commands
----------------------------------------------------------------------------*/

static int cmd_anim_compress(const char* in, const char* out, int level, int try_indexed)
{
    apxl_anim anim;
    pxl_buffer enc;
    long src_size;

    anim = apng_load(in);
    if (!anim.frames) {
        fprintf(stderr, "error: could not load APNG '%s'\n", in);
        return 1;
    }

    if (try_indexed) {
        if (apxl_anim_try_index(&anim)) {
            printf("  indexed: %u-colour global palette fits, encoding as indexed\n",
                   pxl_palette_count(&anim.frames[0].image));
        } else {
            printf("  indexed: source doesn't fit one 256-colour palette, encoding as RGBA\n");
        }
    }

    enc = apxl_encode(&anim, level);
    if (!enc.data) {
        fprintf(stderr, "error: animation encoding failed\n");
        apxl_free(&anim);
        return 1;
    }

    if (!write_file(out, enc.data, enc.size)) {
        fprintf(stderr, "error: could not write '%s'\n", out);
        pxl_free(&enc);
        apxl_free(&anim);
        return 1;
    }

    src_size = file_size(in);
    printf("%s -> %s\n", in, out);
    printf("  %ux%u, %u frames, loop %u\n",
           anim.canvas_w, anim.canvas_h, anim.frame_count, anim.loop_count);
    if (src_size > 0) {
        printf("  APNG %ld bytes -> APXL %zu bytes (%.1f%%)\n",
               src_size, enc.size, 100.0 * (double)enc.size / (double)src_size);
    } else {
        printf("  APXL %zu bytes\n", enc.size);
    }

    pxl_free(&enc);
    apxl_free(&anim);
    return 0;
}

static int cmd_gif_compress(const char* in, const char* out, int level, int try_indexed)
{
    apxl_anim anim;
    pxl_buffer enc;
    long src_size;

    anim = gif_load(in);
    if (!anim.frames) {
        fprintf(stderr, "error: could not load GIF '%s'\n", in);
        return 1;
    }

    if (try_indexed) {
        if (apxl_anim_try_index(&anim)) {
            printf("  indexed: %u-colour global palette fits, encoding as indexed\n",
                   pxl_palette_count(&anim.frames[0].image));
        } else {
            printf("  indexed: source doesn't fit one 256-colour palette, encoding as RGBA\n");
        }
    }

    enc = apxl_encode(&anim, level);
    if (!enc.data) {
        fprintf(stderr, "error: animation encoding failed\n");
        apxl_free(&anim);
        return 1;
    }

    if (!write_file(out, enc.data, enc.size)) {
        fprintf(stderr, "error: could not write '%s'\n", out);
        pxl_free(&enc);
        apxl_free(&anim);
        return 1;
    }

    src_size = file_size(in);
    printf("%s -> %s\n", in, out);
    printf("  %ux%u, %u frames, loop %u\n",
           anim.canvas_w, anim.canvas_h, anim.frame_count, anim.loop_count);
    if (src_size > 0) {
        printf("  GIF %ld bytes -> APXL %zu bytes (%.1f%%)\n",
               src_size, enc.size, 100.0 * (double)enc.size / (double)src_size);
    } else {
        printf("  APXL %zu bytes\n", enc.size);
    }

    pxl_free(&enc);
    apxl_free(&anim);
    return 0;
}

static int cmd_anim_decompress(const char* in, const char* out)
{
    unsigned char* file;
    size_t file_sz;
    pxl_buffer fb;
    apxl_anim anim;

    file = read_file(in, &file_sz);
    if (!file) {
        fprintf(stderr, "error: could not read '%s'\n", in);
        return 1;
    }
    fb.data = file;
    fb.size = file_sz;
    anim = apxl_decode(fb);
    free(file);

    if (!anim.frames) {
        fprintf(stderr, "error: not a valid .apxl file or decode failed\n");
        return 1;
    }

    if (!apng_save(out, &anim)) {
        fprintf(stderr, "error: could not write APNG '%s'\n", out);
        apxl_free(&anim);
        return 1;
    }

    printf("%s -> %s\n", in, out);
    printf("  %ux%u, %u frames, loop %u\n",
           anim.canvas_w, anim.canvas_h, anim.frame_count, anim.loop_count);

    apxl_free(&anim);
    return 0;
}

static int cmd_anim_info(const char* in)
{
    unsigned char* file;
    size_t file_sz;
    apxl_file_header h;

    file = read_file(in, &file_sz);
    if (!file) {
        fprintf(stderr, "error: could not read '%s'\n", in);
        return 1;
    }
    if (!apxl_header_read(file, file_sz, &h)) {
        fprintf(stderr, "error: '%s' is not a valid .apxl file\n", in);
        free(file);
        return 1;
    }

    printf("%s\n", in);
    printf("  format version : %u\n", h.version);
    printf("  canvas         : %u x %u\n", h.canvas_w, h.canvas_h);
    printf("  channels       : %u (%u-bit)\n", h.channels, h.bytes_per_channel * 8);
    if (h.palette_count) {
        printf("  palette        : %u entries", h.palette_count);
        if (h.palette_alpha_count) {
            printf(" (+%u alpha)", h.palette_alpha_count);
        }
        printf("\n");
    } else {
        printf("  palette        : none (truecolor/grayscale)\n");
    }
    printf("  frames         : %u\n", h.frame_count);
    printf("  loop count     : %u%s\n", h.loop_count, h.loop_count ? "" : " (infinite)");
    printf("  metadata       : %u bytes\n", h.meta_byte_count);
    printf("  raw size       : %u bytes\n", h.raw_byte_count);
    printf("  file size      : %zu bytes\n", file_sz);
    if (h.raw_byte_count > 0) {
        printf("  ratio          : %.1f%% of raw\n",
               100.0 * (double)file_sz / (double)h.raw_byte_count);
    }

    free(file);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "pxltool (PNG XL) %s\n"
        "Usage:\n"
        "  pxltool c     in.png  out.pxl  [-l LEVEL] [-p] [-s]  compress PNG  -> PXL\n"
        "  pxltool d     in.pxl  out.png                        decompress PXL  -> PNG\n"
        "  pxltool info  in.pxl                                 print .pxl header\n"
        "  pxltool ca    in.apng out.apxl [-l LEVEL] [-i]        compress APNG -> APXL\n"
        "  pxltool cg    in.gif  out.apxl [-l LEVEL] [-i]        compress GIF  -> APXL\n"
        "  pxltool da    in.apxl out.apng                       decompress APXL -> APNG\n"
        "  pxltool ainfo in.apxl                                print .apxl header\n"
        "\n"
        "  -l LEVEL   zstd compression level, %d..%d (still default %d, anim %d).\n"
        "             Higher = smaller but slower. Values above %d are clamped.\n"
        "             Encoding is always lossless; still images try all filters,\n"
        "             animation uses one cross-frame stream with long-distance\n"
        "             matching (enabled at level >= 10).\n"
        "  -p         progressive: pick only row-wise filters (never BCIF) so the\n"
        "             file decodes top-to-bottom as it downloads. Usually a little\n"
        "             larger on photos, where BCIF would otherwise win.\n"
        "  -s         fast decode: pick only {none, delta}, the two filters measured\n"
        "             to always decode faster than libpng (BCIF loses to it outright\n"
        "             at texture sizes despite winning at screen size; adaptive only\n"
        "             ties it). Costs more size than the default -- 2.9%% measured on\n"
        "             real UI screenshots -- for a decode-speed guarantee. Implies -p.\n"
        "  -i         (ca only) try building one global <=256-colour palette across\n"
        "             every frame and encode indexed instead of RGBA if it fits --\n"
        "             ~33%% smaller at the same settings when it does (measured on 50\n"
        "             real animations; ~76%% of a general corpus fits, 100%% of\n"
        "             pixel-art/UI content sampled). Falls back to RGBA silently\n"
        "             when it doesn't, since not every source qualifies.\n",
        pxl_version(), 1, PXL_LEVEL_MAX, PXL_LEVEL_DEFAULT, APXL_LEVEL_DEFAULT,
        PXL_LEVEL_MAX);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }

    if (strcmp(argv[1], "c") == 0) {
        int level = PXL_LEVEL_DEFAULT;
        unsigned flags = 0;
        int i;
        if (argc < 4) {
            usage();
            return 2;
        }
        for (i = 4; i < argc; ++i) {
            if (strcmp(argv[i], "-p") == 0) {
                flags |= PXL_ENCODE_PROGRESSIVE;
            } else if (strcmp(argv[i], "-s") == 0) {
                flags |= PXL_ENCODE_FAST_DECODE;
            } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
                level = atoi(argv[++i]);
                if (level > PXL_LEVEL_MAX) {
                    fprintf(stderr,
                        "note: level %d exceeds zstd max %d; clamping to %d\n",
                        level, PXL_LEVEL_MAX, PXL_LEVEL_MAX);
                }
            } else {
                fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
                usage();
                return 2;
            }
        }
        return cmd_compress(argv[2], argv[3], level, flags);
    }
    if (strcmp(argv[1], "d") == 0) {
        if (argc < 4) {
            usage();
            return 2;
        }
        return cmd_decompress(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "info") == 0) {
        if (argc < 3) {
            usage();
            return 2;
        }
        return cmd_info(argv[2]);
    }
    if (strcmp(argv[1], "ca") == 0) {
        int level = APXL_LEVEL_DEFAULT;
        int try_indexed = 0;
        int i;
        if (argc < 4) {
            usage();
            return 2;
        }
        for (i = 4; i < argc; ++i) {
            if (strcmp(argv[i], "-i") == 0) {
                try_indexed = 1;
            } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
                level = atoi(argv[++i]);
                if (level > PXL_LEVEL_MAX) {
                    fprintf(stderr,
                        "note: level %d exceeds zstd max %d; clamping to %d\n",
                        level, PXL_LEVEL_MAX, PXL_LEVEL_MAX);
                }
            } else {
                fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
                usage();
                return 2;
            }
        }
        return cmd_anim_compress(argv[2], argv[3], level, try_indexed);
    }
    if (strcmp(argv[1], "cg") == 0) {
        int level = APXL_LEVEL_DEFAULT;
        int try_indexed = 0;
        int i;
        if (argc < 4) {
            usage();
            return 2;
        }
        for (i = 4; i < argc; ++i) {
            if (strcmp(argv[i], "-i") == 0) {
                try_indexed = 1;
            } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
                level = atoi(argv[++i]);
                if (level > PXL_LEVEL_MAX) {
                    fprintf(stderr,
                        "note: level %d exceeds zstd max %d; clamping to %d\n",
                        level, PXL_LEVEL_MAX, PXL_LEVEL_MAX);
                }
            } else {
                fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
                usage();
                return 2;
            }
        }
        return cmd_gif_compress(argv[2], argv[3], level, try_indexed);
    }
    if (strcmp(argv[1], "da") == 0) {
        if (argc < 4) {
            usage();
            return 2;
        }
        return cmd_anim_decompress(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "ainfo") == 0) {
        if (argc < 3) {
            usage();
            return 2;
        }
        return cmd_anim_info(argv[2]);
    }

    usage();
    return 2;
}

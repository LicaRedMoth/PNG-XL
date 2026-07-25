/** \file pxl_meta.c
    \brief Raw PNG chunk walking to preserve ancillary metadata.

    We parse PNG structure directly rather than through libpng: it is simpler,
    lets us treat unknown chunks uniformly, and keeps the exact original bytes.
*/
#include "pxl_meta.h"
#include "pxl_bytes.h"

#include <zlib.h> /* crc32 */

#include <stdlib.h>
#include <string.h>

/* PNG signature length. */
#define PNG_SIG_BYTES 8

/* Chunk types that must NOT be preserved: structural, or tied to the original
   pixel layout that we canonicalize away. */
static int is_dropped_type(const unsigned char* type)
{
    static const char* drop[] = {
        "IHDR", "IDAT", "IEND",           /* structural */
        "PLTE", "tRNS", "sBIT", "bKGD", "hIST", /* pixel-layout dependent */
        /* Animation control and data -- structural in exactly the way IDAT is.
           They carry the frames and their sequence numbers, which apng_save
           regenerates from scratch. Copying them forward would duplicate every
           frame's pixels into the metadata block (measured: a 62 KB APNG became
           a 104 KB .apxl) and emit sequence numbers contradicting the ones being
           written. Listed even though the still path never encounters them,
           because this extractor now also serves apng_load. */
        "acTL", "fcTL", "fdAT",
        NULL
    };
    int i;
    for (i = 0; drop[i]; ++i) {
        if (memcmp(type, drop[i], 4) == 0) {
            return 1;
        }
    }
    return 0;
}

/* A simple growable byte buffer. */
typedef struct {
    unsigned char* data;
    size_t size;
    size_t cap;
    int failed;
} bytebuf;

static void bb_append(bytebuf* b, const unsigned char* src, size_t n)
{
    if (b->failed) {
        return;
    }
    if (b->size + n > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 256;
        unsigned char* nd;
        while (ncap < b->size + n) {
            ncap *= 2;
        }
        nd = (unsigned char*)realloc(b->data, ncap);
        if (!nd) {
            b->failed = 1;
            return;
        }
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->size, src, n);
    b->size += n;
}

pxl_buffer pxl_meta_extract(const unsigned char* png, size_t size)
{
    pxl_buffer out;
    bytebuf bb;
    size_t pos = PNG_SIG_BYTES;

    out.data = NULL;
    out.size = 0;
    memset(&bb, 0, sizeof(bb));

    if (!png || size < PNG_SIG_BYTES) {
        return out;
    }

    while (pos + 8 <= size) {
        uint32_t len = pxl_get_be32(png + pos);
        const unsigned char* type = png + pos + 4;
        size_t data_off = pos + 8;

        /* Bounds: data + 4-byte CRC must fit. */
        if (data_off + (size_t)len + 4 > size) {
            break;
        }

        if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        if (!is_dropped_type(type)) {
            /* record: [type:4][len LE:4][data:len] */
            unsigned char hdr[8];
            memcpy(hdr, type, 4);
            pxl_put_le32(hdr + 4, len);
            bb_append(&bb, hdr, 8);
            bb_append(&bb, png + data_off, len);
        }

        pos = data_off + len + 4; /* advance past data + CRC */
    }

    if (bb.failed) {
        free(bb.data);
        return out;
    }
    out.data = bb.data;
    out.size = bb.size;
    return out;
}

pxl_buffer pxl_meta_inject(const unsigned char* base, size_t base_size,
                           const unsigned char* meta, size_t meta_size)
{
    pxl_buffer out;
    bytebuf bb;
    size_t pos = PNG_SIG_BYTES;
    size_t mpos = 0;
    int inserted = 0;

    out.data = NULL;
    out.size = 0;
    memset(&bb, 0, sizeof(bb));

    if (!base || base_size < PNG_SIG_BYTES) {
        return out;
    }

    /* Copy the signature. */
    bb_append(&bb, base, PNG_SIG_BYTES);

    while (pos + 8 <= base_size) {
        uint32_t len = pxl_get_be32(base + pos);
        const unsigned char* type = base + pos + 4;
        size_t chunk_len = 8 + (size_t)len + 4;

        if (pos + chunk_len > base_size) {
            break;
        }

        /* Insert all preserved chunks right before the first IDAT. This is a
           valid position for every ancillary chunk we keep. */
        if (!inserted && memcmp(type, "IDAT", 4) == 0) {
            while (mpos + 8 <= meta_size) {
                const unsigned char* mtype = meta + mpos;
                uint32_t mlen = pxl_get_le32(meta + mpos + 4);
                const unsigned char* mdata = meta + mpos + 8;
                unsigned char lenbe[4];
                uLong crc;

                if (mpos + 8 + (size_t)mlen > meta_size) {
                    break; /* corrupt metadata; stop */
                }
                pxl_put_be32(lenbe, mlen);
                bb_append(&bb, lenbe, 4);          /* length BE */
                bb_append(&bb, mtype, 4);          /* type */
                bb_append(&bb, mdata, mlen);       /* data */
                crc = crc32(0L, Z_NULL, 0);
                crc = crc32(crc, mtype, 4);
                if (mlen) {
                    crc = crc32(crc, mdata, mlen);
                }
                pxl_put_be32(lenbe, (uint32_t)crc);
                bb_append(&bb, lenbe, 4);          /* CRC BE */

                mpos += 8 + mlen;
            }
            inserted = 1;
        }

        bb_append(&bb, base + pos, chunk_len);
        pos += chunk_len;
    }

    if (bb.failed) {
        free(bb.data);
        return out;
    }
    out.data = bb.data;
    out.size = bb.size;
    return out;
}

/*
 * PNG XL (.pxl / .apxl) encoding via libpxl
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * PNG XL encoder using libpxl
 *
 * The still encoder is a straight one-packet-per-frame wrapper around
 * pxl_encode_ex.
 *
 * The animation encoder cannot be: .apxl compresses every frame into a single
 * zstd stream so that long-distance matching can exploit the redundancy between
 * frames, which is where its compression comes from. A frame is therefore not
 * independently encodable, so this encoder accumulates all frames and emits the
 * whole container as one packet at EOF.
 */

#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"
#include "exif_internal.h"

#include <pxl.h>
#include <apxl.h>

typedef struct LibPxlEncodeContext {
    const AVClass *class;
    int level;              /* zstd compression level */
    int progressive;        /* exclude BCIF so the result decodes top-to-bottom */

    /* animation only */
    apxl_anim anim;         /* frames accumulated until EOF */
    unsigned  frame_cap;    /* allocated size of anim.frames */
    unsigned  n_filled;     /* entries holding a pixel buffer; anim.frame_count is
                               reset after the packet is emitted, this is not, so
                               close() knows exactly what to free */
    int       eof;
} LibPxlEncodeContext;

/* Append one metadata record: ChunkType[4] · DataLength[4, LE] · Data. */
static int pxl_meta_append(unsigned char **buf, size_t *size, const char type[4],
                           const unsigned char *data, size_t len)
{
    unsigned char *p = av_realloc(*buf, *size + 8 + len);
    if (!p)
        return AVERROR(ENOMEM);
    memcpy(p + *size, type, 4);
    AV_WL32(p + *size + 4, (uint32_t)len);
    memcpy(p + *size + 8, data, len);
    *buf   = p;
    *size += 8 + len;
    return 0;
}

/* Build the .pxl metadata block from an AVFrame's side data, using the same
 * record format the still and animated containers share.
 *
 * cICP rather than iCCP for color: FFmpeg models color as CICP code points, and
 * cICP expresses exactly that in four bytes, whereas emitting an ICC profile
 * would mean synthesizing and deflating one -- and libpxlcore links no zlib on
 * purpose, since being zstd-only is what makes it usable from WASM and from
 * FFmpeg without dragging in an image library.
 *
 * Only written when the values are actually known: unspecified code points would
 * assert a color space the source never claimed.
 */
static int pxl_metadata_from_frame(AVCodecContext *avctx, const AVFrame *frame,
                                   pxl_buffer *meta)
{
    unsigned char *buf = NULL;
    size_t size = 0;
    AVBufferRef *exif = NULL;
    int ret;

    memset(meta, 0, sizeof(*meta));

    if (frame->color_primaries != AVCOL_PRI_UNSPECIFIED ||
        frame->color_trc       != AVCOL_TRC_UNSPECIFIED) {
        unsigned char cicp[4];
        cicp[0] = frame->color_primaries;
        cicp[1] = frame->color_trc;
        cicp[2] = 0; /* matrix coefficients: 0 = identity, i.e. RGB */
        cicp[3] = frame->color_range == AVCOL_RANGE_MPEG ? 0 : 1;
        ret = pxl_meta_append(&buf, &size, "cICP", cicp, sizeof(cicp));
        if (ret < 0)
            goto fail;
    }

    /* Collects AV_FRAME_DATA_EXIF plus anything folded into it, such as a
     * display matrix. Returns 0 with *exif NULL when there is nothing. */
    ret = ff_exif_get_buffer(avctx, frame, &exif, AV_EXIF_TIFF_HEADER);
    if (ret < 0)
        goto fail;
    if (exif) {
        ret = pxl_meta_append(&buf, &size, "eXIf", exif->data, exif->size);
        av_buffer_unref(&exif);
        if (ret < 0)
            goto fail;
    }

    meta->data = buf;
    meta->size = size;
    return 0;

fail:
    av_buffer_unref(&exif);
    av_freep(&buf);
    return ret;
}

static int pxl_geometry(enum AVPixelFormat pix_fmt, int *channels, int *bpc)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_GRAY8:    *channels = 1; *bpc = 1; return 0;
    case AV_PIX_FMT_YA8:      *channels = 2; *bpc = 1; return 0;
    case AV_PIX_FMT_RGB24:    *channels = 3; *bpc = 1; return 0;
    case AV_PIX_FMT_RGBA:     *channels = 4; *bpc = 1; return 0;
    case AV_PIX_FMT_GRAY16BE: *channels = 1; *bpc = 2; return 0;
    case AV_PIX_FMT_YA16BE:   *channels = 2; *bpc = 2; return 0;
    case AV_PIX_FMT_RGB48BE:  *channels = 3; *bpc = 2; return 0;
    case AV_PIX_FMT_RGBA64BE: *channels = 4; *bpc = 2; return 0;
    default:                  return AVERROR(EINVAL);
    }
}

/* libpxl wants tightly packed rows; AVFrames are usually strided. Fills img
   with a freshly allocated buffer the caller releases via pxl_image_free. */
static int pxl_image_from_frame(AVCodecContext *avctx, const AVFrame *frame,
                                pxl_image *img)
{
    int channels, bpc, ret;
    size_t stride, size;

    ret = pxl_geometry(frame->format, &channels, &bpc);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format for .pxl\n");
        return ret;
    }

    stride = (size_t)frame->width * channels * bpc;
    size   = stride * frame->height;

    memset(img, 0, sizeof(*img));
    img->buffer.data = av_malloc(size);
    if (!img->buffer.data)
        return AVERROR(ENOMEM);
    img->buffer.size      = size;
    img->width            = frame->width;
    img->height           = frame->height;
    img->channels         = channels;
    img->bytes_per_channel = bpc;

    av_image_copy_plane(img->buffer.data, stride,
                        frame->data[0], frame->linesize[0], stride, frame->height);

    ret = pxl_metadata_from_frame(avctx, frame, &img->metadata);
    if (ret < 0) {
        av_freep(&img->buffer.data);
        return ret;
    }
    return 0;
}

/* libpxl allocates with malloc and frees with free; our packed copies come from
   av_malloc. Release them the matching way instead of calling pxl_image_free. */
static void pxl_image_free_av(pxl_image *img)
{
    av_freep(&img->metadata.data);
    img->metadata.size = 0;
    av_freep(&img->buffer.data);
    img->buffer.size = 0;
}

/*----------------------------------------------------------------------------
  Still images (.pxl)
----------------------------------------------------------------------------*/

static int libpxl_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                               const AVFrame *frame, int *got_packet)
{
    LibPxlEncodeContext *ctx = avctx->priv_data;
    unsigned flags = ctx->progressive ? PXL_ENCODE_PROGRESSIVE : 0;
    pxl_image img;
    pxl_buffer out;
    int ret;

    ret = pxl_image_from_frame(avctx, frame, &img);
    if (ret < 0)
        return ret;

    out = pxl_encode_ex(&img, ctx->level, flags);
    pxl_image_free_av(&img);
    if (!out.data) {
        av_log(avctx, AV_LOG_ERROR, "pxl_encode failed\n");
        return AVERROR_EXTERNAL;
    }

    ret = ff_get_encode_buffer(avctx, pkt, out.size, 0);
    if (ret < 0) {
        pxl_free(&out);
        return ret;
    }
    memcpy(pkt->data, out.data, out.size);
    pxl_free(&out);

    *got_packet = 1;
    return 0;
}

/*----------------------------------------------------------------------------
  Animation (.apxl)
----------------------------------------------------------------------------*/

static int libpxl_anim_append(AVCodecContext *avctx, const AVFrame *frame)
{
    LibPxlEncodeContext *ctx = avctx->priv_data;
    apxl_frame *af;
    int ret;

    if (ctx->anim.frame_count == ctx->frame_cap) {
        unsigned cap = ctx->frame_cap ? ctx->frame_cap * 2 : 16;
        apxl_frame *grown = av_realloc_array(ctx->anim.frames, cap, sizeof(*grown));
        if (!grown)
            return AVERROR(ENOMEM);
        ctx->anim.frames = grown;
        ctx->frame_cap   = cap;
    }

    af = &ctx->anim.frames[ctx->anim.frame_count];
    memset(af, 0, sizeof(*af));
    ret = pxl_image_from_frame(avctx, frame, &af->image);
    if (ret < 0)
        return ret;

    /* Convert the frame duration to APNG-style delay_num/delay_den. Using the
       stream timebase directly keeps this exact for any frame rate. */
    {
        AVRational tb = avctx->time_base;
        int64_t dur = frame->duration > 0 ? frame->duration : 1;
        int64_t num = dur * tb.num;
        int64_t den = tb.den;
        AVRational r = av_make_q(1, 1);

        if (num > 0 && den > 0) {
            av_reduce(&r.num, &r.den, num, den, 65535);
        }
        af->delay_num = r.num > 0 ? (uint16_t)r.num : 1;
        af->delay_den = r.den > 0 ? (uint16_t)r.den : 100;
    }

    if (!ctx->anim.frame_count) {
        ctx->anim.canvas_w          = af->image.width;
        ctx->anim.canvas_h          = af->image.height;
        ctx->anim.channels          = af->image.channels;
        ctx->anim.bytes_per_channel = af->image.bytes_per_channel;
        /* .apxl carries one metadata block for the whole file, so frame 0's is
           promoted to the animation's and the per-frame copies are dropped --
           they would otherwise be encoded N times and ignored on decode. */
        ctx->anim.metadata   = af->image.metadata;
        af->image.metadata.data = NULL;
        af->image.metadata.size = 0;
    } else if (af->image.width != ctx->anim.canvas_w ||
               af->image.height != ctx->anim.canvas_h ||
               af->image.channels != ctx->anim.channels ||
               af->image.bytes_per_channel != ctx->anim.bytes_per_channel) {
        /* .apxl is a fixed canvas: every frame must match the first. */
        av_log(avctx, AV_LOG_ERROR,
               "Frame %u does not match the canvas established by frame 0\n",
               ctx->anim.frame_count);
        pxl_image_free_av(&af->image);
        return AVERROR(EINVAL);
    }

    ctx->anim.frame_count++;
    ctx->n_filled = ctx->anim.frame_count;
    return 0;
}

static int libpxl_anim_encode_frame(AVCodecContext *avctx, AVPacket *pkt)
{
    LibPxlEncodeContext *ctx = avctx->priv_data;
    AVFrame *frame;
    pxl_buffer out;
    int ret;

    /* Drain every input frame first: the container is one zstd stream, so
       nothing can be emitted until the last frame is known. */
    if (!ctx->eof) {
        frame = av_frame_alloc();
        if (!frame)
            return AVERROR(ENOMEM);

        for (;;) {
            ret = ff_encode_get_frame(avctx, frame);
            if (ret == AVERROR_EOF) {
                ctx->eof = 1;
                break;
            }
            if (ret < 0) {
                av_frame_free(&frame);
                return ret;          /* EAGAIN: ask again later */
            }
            ret = libpxl_anim_append(avctx, frame);
            av_frame_unref(frame);
            if (ret < 0) {
                av_frame_free(&frame);
                return ret;
            }
        }
        av_frame_free(&frame);
    }

    if (!ctx->anim.frame_count)
        return AVERROR_EOF;

    out = apxl_encode(&ctx->anim, ctx->level);
    if (!out.data) {
        av_log(avctx, AV_LOG_ERROR, "apxl_encode failed\n");
        return AVERROR_EXTERNAL;
    }

    ret = ff_get_encode_buffer(avctx, pkt, out.size, 0);
    if (ret < 0) {
        pxl_free(&out);
        return ret;
    }
    memcpy(pkt->data, out.data, out.size);
    pxl_free(&out);

    pkt->pts = pkt->dts = 0;
    pkt->flags |= AV_PKT_FLAG_KEY;

    /* One packet holds the whole animation; there is nothing after it. */
    ctx->anim.frame_count = 0;
    return 0;
}

static av_cold int libpxl_anim_encode_init(AVCodecContext *avctx)
{
    LibPxlEncodeContext *ctx = avctx->priv_data;

    /* Animation compression comes from long-distance matching across frames,
       which libpxl only enables at level >= 10. Warn instead of overriding, so
       an explicit -level stays honoured. */
    if (ctx->level > 0 && ctx->level < 10)
        av_log(avctx, AV_LOG_WARNING,
               "level %d disables long-distance matching; .apxl compresses "
               "much worse below level 10\n", ctx->level);
    if (ctx->level <= 0)
        ctx->level = APXL_LEVEL_DEFAULT;
    return 0;
}

static av_cold int libpxl_encode_close(AVCodecContext *avctx)
{
    LibPxlEncodeContext *ctx = avctx->priv_data;
    unsigned i;

    for (i = 0; i < ctx->n_filled && ctx->anim.frames; i++)
        pxl_image_free_av(&ctx->anim.frames[i].image);
    av_freep(&ctx->anim.frames);
    /* Promoted from frame 0 in libpxl_anim_append, so it is av_malloc'd like the
       frame buffers -- not apxl_free's business. */
    av_freep(&ctx->anim.metadata.data);
    ctx->anim.metadata.size = 0;
    ctx->frame_cap = ctx->n_filled = 0;
    ctx->anim.frame_count = 0;
    return 0;
}

#define OFFSET(x) offsetof(LibPxlEncodeContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

static const AVOption libpxl_options[] = {
    { "level", "zstd compression level (0 = codec default)",
        OFFSET(level), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, PXL_LEVEL_MAX, VE },
    { "progressive", "restrict to row-wise filters so the image decodes top-to-bottom",
        OFFSET(progressive), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },
    { NULL },
};

static const AVClass libpxl_encode_class = {
    .class_name = "libpxl",
    .item_name  = av_default_item_name,
    .option     = libpxl_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const enum AVPixelFormat libpxl_pix_fmts[] = {
    AV_PIX_FMT_GRAY8,    AV_PIX_FMT_YA8,
    AV_PIX_FMT_RGB24,    AV_PIX_FMT_RGBA,
    AV_PIX_FMT_GRAY16BE, AV_PIX_FMT_YA16BE,
    AV_PIX_FMT_RGB48BE,  AV_PIX_FMT_RGBA64BE,
    AV_PIX_FMT_NONE,
};

const FFCodec ff_libpxl_encoder = {
    .p.name         = "libpxl",
    CODEC_LONG_NAME("libpxl PNG XL"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_PXL,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_FRAME_THREADS,
    .priv_data_size = sizeof(LibPxlEncodeContext),
    FF_CODEC_ENCODE_CB(libpxl_encode_frame),
    CODEC_PIXFMTS_ARRAY(libpxl_pix_fmts),
    .p.priv_class   = &libpxl_encode_class,
    .p.wrapper_name = "libpxl",
};

const FFCodec ff_libpxl_anim_encoder = {
    .p.name         = "libpxl_anim",
    CODEC_LONG_NAME("libpxl PNG XL animated"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_PXL_ANIM,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    .priv_data_size = sizeof(LibPxlEncodeContext),
    .init           = libpxl_anim_encode_init,
    FF_CODEC_RECEIVE_PACKET_CB(libpxl_anim_encode_frame),
    .close          = libpxl_encode_close,
    CODEC_PIXFMTS_ARRAY(libpxl_pix_fmts),
    .p.priv_class   = &libpxl_encode_class,
    .p.wrapper_name = "libpxl",
};

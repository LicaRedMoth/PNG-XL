/*
 * PNG XL (.pxl / .apxl) decoding via libpxlcore
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
 * PNG XL decoder using libpxlcore
 *
 * .pxl is a lossless still image: a 24-byte header plus one zstd frame of
 * filtered pixels. .apxl is the animated container: all frames are stored as
 * full canvases inside a single zstd stream, so a frame cannot be extracted
 * without decompressing the ones before it. The animation decoder therefore
 * takes the whole file as one packet (the demuxer guarantees this) and hands
 * out the already-decoded frames one at a time.
 *
 * libpxlcore is the PNG-free half of libpxl: it depends only on zstd, so
 * linking it pulls in no image library of its own.
 */

#include <string.h>

#include "libavutil/csp.h"
#include "libavutil/error.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/pixfmt.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "exif_internal.h"

#include <pxl.h>
#include <apxl.h>

/* Ticks per second the apxl demuxer announces; kept in step with the
   avpriv_set_pts_info call in libavformat/apxldec.c. */
#define PXL_ANIM_TIMEBASE 100000

typedef struct LibPxlDecodeContext {
    apxl_anim anim;         /* animation only: decoded once, drained frame by frame */
    uint32_t  next_frame;   /* index of the next frame to hand out */
    int64_t   next_pts;     /* running sum of frame durations, in stream timebase */
    int       have_anim;
} LibPxlDecodeContext;

/* .pxl stores 1..4 channels of 8 or 16 bits. 16-bit samples are kept in
   PNG-native big-endian order, which is exactly what the *BE pixel formats
   expect, so no byte swapping is ever needed. */
static enum AVPixelFormat pxl_pix_fmt(int channels, int bytes_per_channel)
{
    if (bytes_per_channel == 1) {
        switch (channels) {
        case 1: return AV_PIX_FMT_GRAY8;
        case 2: return AV_PIX_FMT_YA8;
        case 3: return AV_PIX_FMT_RGB24;
        case 4: return AV_PIX_FMT_RGBA;
        }
    } else if (bytes_per_channel == 2) {
        switch (channels) {
        case 1: return AV_PIX_FMT_GRAY16BE;
        case 2: return AV_PIX_FMT_YA16BE;
        case 3: return AV_PIX_FMT_RGB48BE;
        case 4: return AV_PIX_FMT_RGBA64BE;
        }
    }
    return AV_PIX_FMT_NONE;
}

/* The metadata block is a flat sequence of PNG-ancillary records:
 *   ChunkType[4] · DataLength[4, little-endian] · Data[DataLength]
 * Walk it and map what FFmpeg models onto frame properties and side data. What
 * FFmpeg has no representation for (tEXt, tIME, pHYs, ...) stays in the .pxl and
 * is simply not surfaced here -- it is preserved for a pxltool round trip either
 * way.
 *
 * Precedence follows pngdec.c so that a .pxl converted from a PNG produces the
 * same color properties as decoding that PNG directly: cICP wins, then iCCP,
 * then sRGB, then cHRM. They are alternative descriptions of the same thing, so
 * honoring more than one would risk contradicting ourselves.
 */
static int pxl_apply_metadata(AVCodecContext *avctx, AVFrame *frame,
                              const unsigned char *meta, size_t meta_size)
{
    const unsigned char *cicp = NULL, *iccp = NULL, *chrm = NULL, *exif = NULL;
    size_t iccp_len = 0, exif_len = 0;
    int have_srgb = 0;
    size_t pos = 0;

    while (pos + 8 <= meta_size) {
        const unsigned char *type = meta + pos;
        uint32_t len = AV_RL32(meta + pos + 4);
        const unsigned char *data = meta + pos + 8;

        if (len > meta_size - pos - 8)
            break;  /* truncated record; ignore the remainder */

        if (!memcmp(type, "cICP", 4) && len >= 4) {
            cicp = data;
        } else if (!memcmp(type, "iCCP", 4)) {
            iccp = data; iccp_len = len;
        } else if (!memcmp(type, "sRGB", 4)) {
            have_srgb = 1;
        } else if (!memcmp(type, "cHRM", 4) && len >= 32) {
            chrm = data;
        } else if (!memcmp(type, "eXIf", 4)) {
            exif = data; exif_len = len;
        }
        pos += 8 + len;
    }

    if (cicp) {
        /* cICP is CICP code points: primaries, transfer, matrix, full-range. */
        if (cicp[0] < AVCOL_PRI_NB)
            avctx->color_primaries = frame->color_primaries = cicp[0];
        if (cicp[1] < AVCOL_TRC_NB)
            avctx->color_trc = frame->color_trc = cicp[1];
        if (cicp[3] == 0)
            avctx->color_range = frame->color_range = AVCOL_RANGE_MPEG;
        else if (cicp[3] == 1)
            avctx->color_range = frame->color_range = AVCOL_RANGE_JPEG;
    } else if (iccp) {
        /* iCCP payload is Name·0x00·CompressionMethod·zlib(profile). Attaching
         * the profile would mean inflating it, and libpxlcore deliberately links
         * no zlib -- that PNG-free property is what lets a WASM or ffmpeg build
         * pull in zstd alone. So the profile is preserved in the container but
         * not surfaced here; warn rather than silently imply none exists. */
        av_log(avctx, AV_LOG_VERBOSE,
               "iCCP present (%zu bytes, deflate-compressed); preserved in the "
               "container but not attached as side data\n", iccp_len);
    } else if (have_srgb) {
        avctx->color_primaries = frame->color_primaries = AVCOL_PRI_BT709;
        avctx->color_trc = frame->color_trc = AVCOL_TRC_IEC61966_2_1;
    } else if (chrm) {
        AVColorPrimariesDesc desc;
        enum AVColorPrimaries prim;
        desc.wp.x     = av_make_q(AV_RB32(chrm +  0), 100000);
        desc.wp.y     = av_make_q(AV_RB32(chrm +  4), 100000);
        desc.prim.r.x = av_make_q(AV_RB32(chrm +  8), 100000);
        desc.prim.r.y = av_make_q(AV_RB32(chrm + 12), 100000);
        desc.prim.g.x = av_make_q(AV_RB32(chrm + 16), 100000);
        desc.prim.g.y = av_make_q(AV_RB32(chrm + 20), 100000);
        desc.prim.b.x = av_make_q(AV_RB32(chrm + 24), 100000);
        desc.prim.b.y = av_make_q(AV_RB32(chrm + 28), 100000);
        prim = av_csp_primaries_id_from_desc(&desc);
        if (prim != AVCOL_PRI_UNSPECIFIED)
            avctx->color_primaries = frame->color_primaries = prim;
    }

    /* RGB pixel formats throughout; matching pngdec. */
    avctx->colorspace = frame->colorspace = AVCOL_SPC_RGB;

    if (exif && exif_len) {
        AVBufferRef *buf = av_buffer_alloc(exif_len);
        int ret;
        if (!buf)
            return AVERROR(ENOMEM);
        memcpy(buf->data, exif, exif_len);
        /* eXIf holds a bare TIFF header, no "Exif\0\0" prefix. buf is always
         * consumed, including on failure, so it must not be unref'd here. */
        ret = ff_decode_exif_attach_buffer(avctx, frame, &buf, AV_EXIF_TIFF_HEADER);
        if (ret < 0)
            av_log(avctx, AV_LOG_WARNING, "Could not attach EXIF metadata\n");
    }

    return 0;
}

/* Copy tightly packed libpxl pixels into an AVFrame, which may be strided. */
static int pxl_frame_from_image(AVCodecContext *avctx, AVFrame *frame,
                                const pxl_image *img)
{
    enum AVPixelFormat pix_fmt = pxl_pix_fmt(img->channels, img->bytes_per_channel);
    size_t stride = (size_t)img->width * img->channels * img->bytes_per_channel;
    int ret;

    if (pix_fmt == AV_PIX_FMT_NONE) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported pxl geometry: %d channels, %d bytes each\n",
               img->channels, img->bytes_per_channel);
        return AVERROR_INVALIDDATA;
    }

    ret = ff_set_dimensions(avctx, img->width, img->height);
    if (ret < 0)
        return ret;
    avctx->pix_fmt = pix_fmt;

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    av_image_copy_plane(frame->data[0], frame->linesize[0],
                        img->buffer.data, stride, stride, img->height);
    return 0;
}

/*----------------------------------------------------------------------------
  Still images (.pxl)
----------------------------------------------------------------------------*/

static int libpxl_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                               int *got_frame, AVPacket *avpkt)
{
    pxl_buffer file;
    pxl_image img;
    int ret;

    file.data = avpkt->data;
    file.size = avpkt->size;

    img = pxl_decode(file);
    if (!img.buffer.data) {
        av_log(avctx, AV_LOG_ERROR, "Not a valid .pxl file\n");
        return AVERROR_INVALIDDATA;
    }

    ret = pxl_frame_from_image(avctx, frame, &img);
    if (ret >= 0 && img.metadata.data && img.metadata.size)
        ret = pxl_apply_metadata(avctx, frame, img.metadata.data, img.metadata.size);
    pxl_image_free(&img);
    if (ret < 0)
        return ret;

    *got_frame = 1;
    return avpkt->size;
}

/*----------------------------------------------------------------------------
  Animation (.apxl)
----------------------------------------------------------------------------*/

/* The frames share one zstd stream, so the first call decodes the entire file
   and later calls only drain what was decoded. */
static int libpxl_anim_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    LibPxlDecodeContext *ctx = avctx->priv_data;
    const apxl_frame *af;
    int ret;

    if (!ctx->have_anim) {
        AVPacket *pkt = av_packet_alloc();
        pxl_buffer file;

        if (!pkt)
            return AVERROR(ENOMEM);

        ret = ff_decode_get_packet(avctx, pkt);
        if (ret < 0) {
            av_packet_free(&pkt);
            return ret;
        }

        file.data = pkt->data;
        file.size = pkt->size;
        ctx->anim = apxl_decode(file);
        av_packet_free(&pkt);

        if (!ctx->anim.frames || !ctx->anim.frame_count) {
            av_log(avctx, AV_LOG_ERROR, "Not a valid .apxl file\n");
            return AVERROR_INVALIDDATA;
        }
        ctx->have_anim  = 1;
        ctx->next_frame = 0;
    }

    if (ctx->next_frame >= ctx->anim.frame_count)
        return AVERROR_EOF;

    af  = &ctx->anim.frames[ctx->next_frame];
    ret = pxl_frame_from_image(avctx, frame, &af->image);
    if (ret < 0)
        return ret;

    /* Animation metadata describes the whole file, so it is applied to every
       frame -- FFmpeg carries color properties and side data per frame. */
    if (ctx->anim.metadata.data && ctx->anim.metadata.size) {
        ret = pxl_apply_metadata(avctx, frame, ctx->anim.metadata.data,
                                 ctx->anim.metadata.size);
        if (ret < 0)
            return ret;
    }

    /* APXL timing is a rational delay per frame (delay_den 0 means 100, per the
       APNG rule libpxl inherits). The demuxer sets the timebase to 1/100000 s,
       matching the APNG demuxer, so a delay converts exactly; pts is the running
       sum of the durations. */
    {
        unsigned den = af->delay_den ? af->delay_den : 100;
        frame->duration = (int64_t)af->delay_num * PXL_ANIM_TIMEBASE / den;
        frame->pts      = ctx->next_pts;
        ctx->next_pts  += frame->duration;
    }
    ctx->next_frame++;

    return 0;
}

static av_cold int libpxl_anim_decode_close(AVCodecContext *avctx)
{
    LibPxlDecodeContext *ctx = avctx->priv_data;
    apxl_free(&ctx->anim);
    return 0;
}

const FFCodec ff_libpxl_decoder = {
    .p.name         = "libpxl",
    CODEC_LONG_NAME("libpxl PNG XL"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_PXL,
    .p.capabilities = AV_CODEC_CAP_DR1,
    FF_CODEC_DECODE_CB(libpxl_decode_frame),
    .p.wrapper_name = "libpxl",
};

const FFCodec ff_libpxl_anim_decoder = {
    .p.name         = "libpxl_anim",
    CODEC_LONG_NAME("libpxl PNG XL animated"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_PXL_ANIM,
    .priv_data_size = sizeof(LibPxlDecodeContext),
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    FF_CODEC_RECEIVE_FRAME_CB(libpxl_anim_receive_frame),
    .close          = libpxl_anim_decode_close,
    .p.wrapper_name = "libpxl",
};

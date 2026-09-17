/*
 * Animated PNG XL (.apxl) demuxer
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
 * Animated PNG XL demuxer
 *
 * All frames of an .apxl file live in a single zstd stream so that
 * long-distance matching can exploit the redundancy between them. That makes
 * frames individually unextractable, so this demuxer emits the whole file as
 * one packet and lets the decoder split it into frames -- the same arrangement
 * the animated JPEG XL demuxer uses.
 */

#include <stdint.h>

#include "libavutil/intreadwrite.h"

#include "avformat.h"
#include "demux.h"
#include "internal.h"

/* Header layout, little-endian (see SPEC.md 10; version 2, 36 bytes total --
 * bytes 32/34 add PaletteCount/PaletteAlphaCount for indexed animation, added
 * 2026-09-18, unused by this probe since everything it checks fits in the
 * first 20 bytes):
 *   0 magic "APXL", 4 version, 5 channels, 6 bytesPerChannel, 7 flags,
 *   8 canvasW, 12 canvasH, 16 frameCount, 20 loopCount, 24 metaBytes,
 *   28 rawBytes, 32 paletteCount, 34 paletteAlphaCount. */
#define APXL_HEADER_BYTES 32

static int apxl_probe(const AVProbeData *p)
{
    if (p->buf_size < APXL_HEADER_BYTES)
        return 0;
    if (memcmp(p->buf, "APXL", 4))
        return 0;
    if (p->buf[4] != 2)                       /* version */
        return 0;
    if (p->buf[5] < 1 || p->buf[5] > 4)       /* channels */
        return 0;
    if (p->buf[6] != 1 && p->buf[6] != 2)     /* bytes per channel */
        return 0;
    if (!AV_RL32(p->buf + 8) || !AV_RL32(p->buf + 12))   /* canvas */
        return 0;
    if (!AV_RL32(p->buf + 16))                /* frame count */
        return 0;

    return AVPROBE_SCORE_MAX;
}

static int apxl_read_header(AVFormatContext *s)
{
    AVStream *st = avformat_new_stream(s, NULL);

    if (!st)
        return AVERROR(ENOMEM);

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_PXL_ANIM;

    /* Frame delays are rationals delay_num/delay_den seconds with 16-bit terms.
       1/100000 is what the APNG demuxer uses and it represents every such delay
       exactly for the denominators that occur in practice -- a coarser timebase
       (1/1000) rounds 1/24 s to 41 ms and drifts by 1.6% over a long animation. */
    avpriv_set_pts_info(st, 64, 1, 100000);

    return 0;
}

/* The decoder needs the complete file as a single packet. */
static int apxl_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVIOContext *pb = s->pb;
    int64_t size = avio_size(pb);
    int ret;

    if (avio_feof(pb))
        return AVERROR_EOF;

    if (size <= 0 || size > INT_MAX) {
        /* Non-seekable input: read until EOF instead of trusting avio_size. */
        ret = av_append_packet(pb, pkt, INT_MAX);
        if (ret < 0 && ret != AVERROR_EOF)
            return ret;
        return pkt->size > 0 ? 0 : AVERROR_EOF;
    }

    ret = av_get_packet(pb, pkt, size);
    if (ret < 0)
        return ret;

    pkt->flags |= AV_PKT_FLAG_KEY;
    return 0;
}

const FFInputFormat ff_apxl_demuxer = {
    .p.name         = "apxl",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Animated PNG XL"),
    .p.extensions   = "apxl",
    .p.mime_type    = "image/apxl",
    .p.flags        = AVFMT_GENERIC_INDEX | AVFMT_NOTIMESTAMPS,
    .read_probe     = apxl_probe,
    .read_header    = apxl_read_header,
    .read_packet    = apxl_read_packet,
};

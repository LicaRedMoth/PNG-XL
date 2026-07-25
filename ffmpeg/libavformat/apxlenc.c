/*
 * Animated PNG XL (.apxl) muxer
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
 * Animated PNG XL muxer
 *
 * The libpxl_anim encoder produces the entire container as one packet (all
 * frames share a single zstd stream), so muxing is just writing that packet
 * out. Anything else would be a second, conflicting container.
 */

#include "avformat.h"
#include "mux.h"

static int apxl_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    avio_write(s->pb, pkt->data, pkt->size);
    return 0;
}

const FFOutputFormat ff_apxl_muxer = {
    .p.name           = "apxl",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Animated PNG XL"),
    .p.mime_type      = "image/apxl",
    .p.extensions     = "apxl",
    .p.audio_codec    = AV_CODEC_ID_NONE,
    .p.video_codec    = AV_CODEC_ID_PXL_ANIM,
    .p.subtitle_codec = AV_CODEC_ID_NONE,
    .p.flags          = AVFMT_VARIABLE_FPS | AVFMT_NOTIMESTAMPS,
    .flags_internal   = FF_OFMT_FLAG_MAX_ONE_OF_EACH |
                        FF_OFMT_FLAG_ONLY_DEFAULT_CODECS,
    .write_packet     = apxl_write_packet,
};

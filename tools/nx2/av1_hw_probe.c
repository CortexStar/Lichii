// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Decodes an AV1 IVF file the way Lichii's nx2 video decoder does (FFmpeg's av1 decoder on a host
// device, or libdav1d), linked against the same FFmpeg package, with FFmpeg's log at debug level.
// Separates a driver or FFmpeg failure from Lichii's integration. No game files needed: use AV1
// conformance streams (aom-test-data) or a clip from any encoder.
//
// Build (Linux, llvm-mingw, the package from tools/windows/build-ffmpeg-mingw.sh):
//   x86_64-w64-mingw32-clang -O2 tools/nx2/av1_hw_probe.c -I<pkg>/include -L<pkg>/lib \
//     -lavcodec -lswresample -lavutil -lbcrypt -lole32 -luser32 -static -o av1_hw_probe.exe
// Run: av1_hw_probe.exe <file.ivf> <vulkan|d3d12va|d3d11va|cuda|none> [vulkan device index]
// PROBE_FRAME_HASHES=1 prints "frame <n> pts <pts> <hash>" per frame: the same hash as Lichii's
// EDEN_NX2_VDEC_TRACE "frame hash ... picture=" lines, so a stream dumped with
// EDEN_NX2_VDEC_STREAM_DUMP can be compared frame by frame across decoders and with Lichii.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>

static enum AVHWDeviceType g_type;

static enum AVPixelFormat get_format(AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
    for (int i = 0;; i++) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(ctx->codec, i);
        if (!config) {
            break;
        }
        if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) ||
            config->device_type != g_type) {
            continue;
        }
        for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == config->pix_fmt) {
                fprintf(stderr, "probe: get_format -> %s (sw %s, profile %d, level %d)\n",
                        av_get_pix_fmt_name(*p), av_get_pix_fmt_name(ctx->sw_pix_fmt),
                        ctx->profile, ctx->level);
                return *p;
            }
        }
    }
    fprintf(stderr, "probe: get_format: no hardware format offered\n");
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

static int receive(AVCodecContext* ctx, int* frames, uint64_t* sum) {
    AVFrame* frame = av_frame_alloc();
    AVFrame* sw = av_frame_alloc();
    int ret;
    while ((ret = avcodec_receive_frame(ctx, frame)) >= 0) {
        AVFrame* out = frame;
        if (frame->hw_frames_ctx) {
            const AVHWFramesContext* frames_ctx = (const AVHWFramesContext*)frame->hw_frames_ctx->data;
            sw->format = frames_ctx->sw_format;
            if ((ret = av_hwframe_transfer_data(sw, frame, 0)) < 0) {
                fprintf(stderr, "probe: av_hwframe_transfer_data: %s\n", av_err2str(ret));
                break;
            }
            out = sw;
        }
        for (int y = 0; y < out->height; y++) {
            for (int x = 0; x < out->width; x += 61) {
                *sum = *sum * 31 + out->data[0][y * out->linesize[0] + x];
            }
        }
        if (getenv("PROBE_FRAME_HASHES")) {
            // Every visible luma and chroma byte, one line per frame, to compare paths with diff.
            uint64_t hash = 1469598103934665603ULL;
            const int nv12 = out->format == AV_PIX_FMT_NV12;
            for (int plane = 0; plane < (nv12 ? 2 : 3); plane++) {
                const int h = plane ? (out->height + 1) / 2 : out->height;
                const int w = plane ? (nv12 ? (out->width + 1) / 2 * 2 : (out->width + 1) / 2)
                                    : out->width;
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        int v = out->data[plane][y * out->linesize[plane] + x];
                        if (!nv12 && plane) {
                            // Planar U/V to NV12 order: hash as interleaved pairs.
                            continue;
                        }
                        hash = (hash ^ (uint64_t)v) * 1099511628211ULL;
                    }
                }
            }
            if (!nv12) {
                for (int y = 0; y < (out->height + 1) / 2; y++) {
                    for (int x = 0; x < (out->width + 1) / 2; x++) {
                        hash = (hash ^ out->data[1][y * out->linesize[1] + x]) * 1099511628211ULL;
                        hash = (hash ^ out->data[2][y * out->linesize[2] + x]) * 1099511628211ULL;
                    }
                }
            }
            printf("frame %d pts %lld %016llx\n", *frames, (long long)frame->pts,
                   (unsigned long long)hash);
        }
        if (*frames < 3 || (*frames % 60) == 0) {
            fprintf(stderr, "probe: frame %d %dx%d %s pts %lld\n", *frames, out->width, out->height,
                    av_get_pix_fmt_name(out->format), (long long)frame->pts);
        }
        ++*frames;
        av_frame_unref(sw);
        av_frame_unref(frame);
    }
    av_frame_free(&sw);
    av_frame_free(&frame);
    return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : ret;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <file.ivf> <vulkan|d3d12va|d3d11va|cuda|none> [index]\n", argv[0]);
        return 2;
    }
    av_log_set_level(AV_LOG_DEBUG);
    FILE* file = fopen(argv[1], "rb");
    if (!file) {
        perror(argv[1]);
        return 2;
    }
    unsigned char header[32];
    if (fread(header, 1, 32, file) != 32 || memcmp(header, "DKIF", 4) != 0) {
        fprintf(stderr, "probe: not an IVF file\n");
        return 2;
    }

    int hardware = strcmp(argv[2], "none") != 0;
    const AVCodec* codec =
        hardware ? avcodec_find_decoder_by_name("av1") : avcodec_find_decoder(AV_CODEC_ID_AV1);
    if (!codec) {
        fprintf(stderr, "probe: no decoder\n");
        return 1;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    AVBufferRef* device = NULL;
    if (hardware) {
        g_type = av_hwdevice_find_type_by_name(argv[2]);
        const char* name = g_type == AV_HWDEVICE_TYPE_VULKAN ? (argc > 3 ? argv[3] : "0") : NULL;
        const int ret = av_hwdevice_ctx_create(&device, g_type, name, NULL, 0);
        if (ret < 0) {
            fprintf(stderr, "probe: av_hwdevice_ctx_create(%s): %s\n", argv[2], av_err2str(ret));
            return 1;
        }
        ctx->hw_device_ctx = av_buffer_ref(device);
        ctx->get_format = get_format;
        ctx->thread_count = 1;
    } else {
        ctx->thread_count = 2;
    }
    int ret = avcodec_open2(ctx, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "probe: avcodec_open2: %s\n", av_err2str(ret));
        return 1;
    }
    fprintf(stderr, "probe: decoder %s on %s\n", codec->name, argv[2]);

    int frames = 0, packets = 0;
    uint64_t sum = 0;
    unsigned char frame_header[12];
    while (fread(frame_header, 1, 12, file) == 12) {
        const uint32_t size = frame_header[0] | frame_header[1] << 8 | frame_header[2] << 16 |
                              (uint32_t)frame_header[3] << 24;
        AVPacket* packet = av_packet_alloc();
        if (av_new_packet(packet, (int)size) < 0 || fread(packet->data, 1, size, file) != size) {
            fprintf(stderr, "probe: short packet\n");
            return 1;
        }
        // The IVF timestamp (Lichii's EDEN_NX2_VDEC_STREAM_DUMP writes the game's, in us).
        packet->pts = (int64_t)((uint64_t)frame_header[4] | (uint64_t)frame_header[5] << 8 |
                                (uint64_t)frame_header[6] << 16 | (uint64_t)frame_header[7] << 24 |
                                (uint64_t)frame_header[8] << 32 | (uint64_t)frame_header[9] << 40 |
                                (uint64_t)frame_header[10] << 48 | (uint64_t)frame_header[11] << 56);
        packet->dts = packet->pts;
        ret = avcodec_send_packet(ctx, packet);
        av_packet_free(&packet);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            fprintf(stderr, "probe: avcodec_send_packet (packet %d): %s\n", packets, av_err2str(ret));
            // What Lichii does next: receive, then free the context to fall back.
            fprintf(stderr, "probe: receive after the error...\n");
            ret = receive(ctx, &frames, &sum);
            fprintf(stderr, "probe: receive returned %s\n", av_err2str(ret));
            fprintf(stderr, "probe: freeing the context...\n");
            avcodec_free_context(&ctx);
            fprintf(stderr, "probe: freed; falling back to libdav1d from the first packet\n");
            if (!hardware || frames > 0) {
                return 1;
            }
            hardware = 0;
            codec = avcodec_find_decoder(AV_CODEC_ID_AV1);
            ctx = avcodec_alloc_context3(codec);
            ctx->thread_count = 2;
            if ((ret = avcodec_open2(ctx, codec, NULL)) < 0) {
                fprintf(stderr, "probe: avcodec_open2 (fallback): %s\n", av_err2str(ret));
                return 1;
            }
            fseek(file, 32, SEEK_SET);
            packets = 0;
            continue;
        }
        ++packets;
        if ((ret = receive(ctx, &frames, &sum)) < 0) {
            fprintf(stderr, "probe: avcodec_receive_frame (packet %d): %s\n", packets, av_err2str(ret));
            return 1;
        }
    }
    avcodec_send_packet(ctx, NULL);
    if ((ret = receive(ctx, &frames, &sum)) < 0) {
        fprintf(stderr, "probe: drain: %s\n", av_err2str(ret));
        return 1;
    }
    printf("probe: OK %s %s: %d packets, %d frames, luma checksum %016llx\n", argv[1], argv[2],
           packets, frames, (unsigned long long)sum);
    avcodec_free_context(&ctx);
    av_buffer_unref(&device);
    fclose(file);
    return 0;
}

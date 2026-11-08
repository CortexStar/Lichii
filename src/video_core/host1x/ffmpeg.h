// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <queue>

#include "common/common_funcs.h"
#include "common/common_types.h"
#include "video_core/host1x/nvdec_common.h"

extern "C" {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif

#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

namespace Tegra {
class MemoryManager;
}

namespace FFmpeg {

class Packet;
class Frame;
class Decoder;
class HardwareContext;
class DecoderContext;
class DeinterlaceFilter;

// Wraps an AVPacket, a container for compressed bitstream data.
class Packet {
public:
    YUZU_NON_COPYABLE(Packet);
    YUZU_NON_MOVEABLE(Packet);

    explicit Packet(std::span<const u8> data);
    ~Packet();

    AVPacket* GetPacket() const {
        return m_packet;
    }

private:
    AVPacket* m_packet{};
};

// Wraps an AVFrame, a container for audio and video stream data.
class Frame {
public:
    YUZU_NON_COPYABLE(Frame);
    YUZU_NON_MOVEABLE(Frame);

    explicit Frame();
    ~Frame();

    int GetWidth() const {
        return m_frame->width;
    }

    int GetHeight() const {
        return m_frame->height;
    }

    AVPixelFormat GetPixelFormat() const {
        return static_cast<AVPixelFormat>(m_frame->format);
    }

    int GetStride(int plane) const {
        return m_frame->linesize[plane];
    }

    int* GetStrides() const {
        return m_frame->linesize;
    }

    u8* GetData(int plane) const {
        return m_frame->data[plane];
    }

    const u8* GetPlane(int plane) const {
        return m_frame->data[plane];
    }

    u8** GetPlanes() const {
        return m_frame->data;
    }

    void SetFormat(int format) {
        m_frame->format = format;
    }

    bool IsInterlaced() const {
#if defined(FF_API_INTERLACED_FRAME) || LIBAVUTIL_VERSION_MAJOR >= 59
        return m_frame->flags & AV_FRAME_FLAG_INTERLACED;
#else
        return m_frame->interlaced_frame;
#endif
    }

    bool IsHardwareDecoded() const {
        return m_frame->hw_frames_ctx != nullptr;
    }

    AVFrame* GetFrame() const {
        return m_frame;
    }

private:
    AVFrame* m_frame{};
};

// Wraps an AVCodec, a type containing information about a codec.
class Decoder {
public:
    YUZU_NON_COPYABLE(Decoder);
    YUZU_NON_MOVEABLE(Decoder);

    /// `hardware_only`: for AV1, FFmpeg's own decoder, which decodes only on a host GPU (FFmpeg
    /// picks libdav1d otherwise). Codec is null when FFmpeg was built without it.
    explicit Decoder(Tegra::Host1x::NvdecCommon::VideoCodec codec, bool hardware_only = false);
    ~Decoder() = default;

    bool SupportsDecodingOnDevice(AVPixelFormat* out_pix_fmt, AVHWDeviceType type) const;

    const AVCodec* GetCodec() const {
        return m_codec;
    }

private:
    const AVCodec* m_codec{};
};

// Wraps AVBufferRef for an accelerated decoder.
class HardwareContext {
public:
    YUZU_NON_COPYABLE(HardwareContext);
    YUZU_NON_MOVEABLE(HardwareContext);

    static std::vector<AVHWDeviceType> GetSupportedDeviceTypes();

    explicit HardwareContext() = default;
    ~HardwareContext();

    /// Attaches the first host device, in the codec's preference order, that FFmpeg can decode it
    /// on. Devices are created once per process.
    bool InitializeForDecoder(DecoderContext& decoder_context, const Decoder& decoder);

    /// The same from the `first`-th device type of the codec's order on; returns the index of
    /// the type attached.
    std::optional<size_t> InitializeForDecoder(DecoderContext& decoder_context,
                                               const Decoder& decoder, size_t first);

    AVHWDeviceType GetType() const {
        return m_type;
    }

    AVBufferRef* GetBufferRef() const {
        return m_gpu_decoder;
    }

private:
    AVBufferRef* m_gpu_decoder{};
    AVHWDeviceType m_type{AV_HWDEVICE_TYPE_NONE};
};

// Wraps an AVCodecContext.
class DecoderContext {
public:
    YUZU_NON_COPYABLE(DecoderContext);
    YUZU_NON_MOVEABLE(DecoderContext);

    explicit DecoderContext(const Decoder& decoder);
    ~DecoderContext();

    void InitializeHardwareDecoder(const HardwareContext& context);
    /// Threads FFmpeg (or libdav1d) decodes with; 0, the default, is one per logical CPU.
    void SetThreadCount(int count);
    bool OpenContext(const Decoder& decoder, std::span<const u8> extradata = {});
    bool SendPacket(const Packet& packet);
    std::shared_ptr<Frame> ReceiveFrame();

    AVCodecContext* GetCodecContext() const {
        return m_codec_context;
    }

    bool UsingDecodeOrder() const {
        return m_decode_order;
    }

    /// A host GPU decoder was attached and could not decode the stream (no hardware format for
    /// it, or a decode error).
    bool HardwareFailed() const {
        return m_hardware_failed;
    }

    void MarkHardwareFailed() {
        m_hardware_failed = m_hardware;
    }

private:
    const Decoder& m_decoder;
    AVCodecContext* m_codec_context{};
    std::shared_ptr<Frame> m_final_frame{};
    bool m_decode_order{};
    bool m_hardware{};
    bool m_hardware_failed{};
};

struct FrameOffsets {
    bool interlaced{};
    bool hidden{};
    u64 luma{};
    u64 luma_bottom{};
};

struct FrameDimensions {
    s32 width{};
    s32 height{};
};

class DecodeApi {
public:
    YUZU_NON_COPYABLE(DecodeApi);
    YUZU_NON_MOVEABLE(DecodeApi);

    DecodeApi() = default;
    ~DecodeApi() = default;

    bool Initialize(Tegra::Host1x::NvdecCommon::VideoCodec codec);
    void Reset();

    bool UsingDecodeOrder() const {
        return m_decoder_context->UsingDecodeOrder();
    }

    bool SendPacket(std::span<const u8> packet_data, const FrameOffsets& offsets,
                    std::optional<FrameDimensions> dimensions = std::nullopt);

    struct DecodedFrame {
        std::shared_ptr<Frame> frame;
        FrameOffsets offsets;
    };
    std::optional<DecodedFrame> ReceiveFrame();

private:
    std::optional<FFmpeg::Decoder> m_decoder;
    std::optional<FFmpeg::DecoderContext> m_decoder_context;
    std::optional<FFmpeg::HardwareContext> m_hardware_context;
    bool m_opened{};
    bool m_defer_android_mediacodec_open{};
    bool m_needs_h264_extradata{};
    s64 m_next_pts{};
    std::queue<FrameOffsets> m_pending_offsets;
};

} // namespace FFmpeg

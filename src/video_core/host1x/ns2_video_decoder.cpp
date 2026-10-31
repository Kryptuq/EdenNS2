// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>

#include "common/logging.h"
#include "video_core/host1x/ns2_video_decoder.h"

extern "C" {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

namespace Tegra::NS2 {

struct H264Decoder::Impl {
    AVCodecContext* context{};
    AVPacket* packet{};
    AVFrame* frame{};
    SwsContext* scaler{};
    u32 errors{};

    ~Impl() {
        Reset();
        if (packet) {
            av_packet_free(&packet);
        }
        if (frame) {
            av_frame_free(&frame);
        }
        if (scaler) {
            sws_freeContext(scaler);
        }
    }

    void Reset() {
        if (context) {
            avcodec_free_context(&context);
        }
    }

    bool Open(std::span<const u8> config) {
        Reset();
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            LOG_ERROR(HW_GPU, "NS2 video: FFmpeg has no H.264 decoder");
            return false;
        }
        context = avcodec_alloc_context3(codec);
        if (!context) {
            return false;
        }
        if (!config.empty()) {
            context->extradata = static_cast<u8*>(av_mallocz(config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (context->extradata) {
                std::memcpy(context->extradata, config.data(), config.size());
                context->extradata_size = static_cast<int>(config.size());
            }
        }
        // Pictures come out in presentation order (B-frame reordering), tagged with the
        // timestamp of the access unit they came from.
        context->pkt_timebase = AVRational{1, 1000000};
        context->thread_type = FF_THREAD_SLICE;
        context->thread_count = 0;
        if (avcodec_open2(context, codec, nullptr) < 0) {
            LOG_ERROR(HW_GPU, "NS2 video: could not open the H.264 decoder");
            Reset();
            return false;
        }
        if (!packet) {
            packet = av_packet_alloc();
        }
        if (!frame) {
            frame = av_frame_alloc();
        }
        return packet && frame;
    }
};

H264Decoder::H264Decoder() : impl{std::make_unique<Impl>()} {}
H264Decoder::~H264Decoder() = default;

bool H264Decoder::Configure(std::span<const u8> config) {
    const bool ok = impl->Open(config);
    LOG_INFO(HW_GPU, "NS2 video: H.264 decoder configured with {} bytes of codec data ({:02x} {:02x} {:02x} {:02x}), ok={}",
             config.size(), config.size() > 0 ? config[0] : 0, config.size() > 1 ? config[1] : 0,
             config.size() > 2 ? config[2] : 0, config.size() > 3 ? config[3] : 0, ok);
    return ok;
}

std::vector<H264Decoder::Frame> H264Decoder::Decode(std::span<const u8> data, s64 pts) {
    std::vector<Frame> frames;
    if (!impl->context && !impl->Open({})) {
        return frames;
    }
    if (data.empty()) {
        return frames;
    }
    if (av_new_packet(impl->packet, static_cast<int>(data.size())) < 0) {
        return frames;
    }
    std::memcpy(impl->packet->data, data.data(), data.size());
    impl->packet->pts = pts;
    impl->packet->dts = AV_NOPTS_VALUE;
    const int sent = avcodec_send_packet(impl->context, impl->packet);
    av_packet_unref(impl->packet);
    if (sent < 0 && sent != AVERROR(EAGAIN)) {
        if (impl->errors++ < 8) {
            LOG_WARNING(HW_GPU, "NS2 video: H.264 packet of {} bytes rejected ({})", data.size(), sent);
        }
        return frames;
    }
    Receive(frames);
    return frames;
}

std::vector<H264Decoder::Frame> H264Decoder::Drain() {
    std::vector<Frame> frames;
    if (!impl->context) {
        return frames;
    }
    if (avcodec_send_packet(impl->context, nullptr) >= 0) {
        Receive(frames);
    }
    avcodec_flush_buffers(impl->context);
    return frames;
}

void H264Decoder::Receive(std::vector<Frame>& frames) {
    while (avcodec_receive_frame(impl->context, impl->frame) == 0) {
        const int w = impl->frame->width, h = impl->frame->height;
        if (w <= 0 || h <= 0) {
            av_frame_unref(impl->frame);
            continue;
        }
        impl->scaler = sws_getCachedContext(impl->scaler, w, h,
                                            static_cast<AVPixelFormat>(impl->frame->format), w, h,
                                            AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!impl->scaler) {
            av_frame_unref(impl->frame);
            continue;
        }
        Frame& out = frames.emplace_back();
        // Packed NV12: the Y plane (pitch = width) followed by the interleaved UV plane.
        const size_t luma = static_cast<size_t>(w) * h;
        out.nv12.resize(luma + static_cast<size_t>(w) * ((h + 1) / 2) + 2);
        u8* dst[4] = {out.nv12.data(), out.nv12.data() + luma, nullptr, nullptr};
        int dst_stride[4] = {w, (w + 1) & ~1, 0, 0};
        sws_scale(impl->scaler, impl->frame->data, impl->frame->linesize, 0, h, dst, dst_stride);
        out.width = static_cast<u32>(w);
        out.height = static_cast<u32>(h);
        const s64 best = impl->frame->best_effort_timestamp;
        out.pts = best != AV_NOPTS_VALUE ? best : impl->frame->pts;
        av_frame_unref(impl->frame);
    }
}

} // namespace Tegra::NS2

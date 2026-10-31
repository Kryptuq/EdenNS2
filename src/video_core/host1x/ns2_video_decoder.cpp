// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstdarg>
#include <cstring>
#include <string_view>

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
namespace {
// Forwards FFmpeg's warnings (for example missing reference frames) to Eden's log, so picture
// corruption can be told apart from lost input.
void ForwardLog(void* avcl, int level, const char* format, va_list args) {
    if (level > AV_LOG_WARNING) {
        return;
    }
    static std::atomic<u32> logged{};
    const u32 n = logged.fetch_add(1, std::memory_order_relaxed);
    if (n >= 200 && n % 500 != 0) {
        return;
    }
    char line[512];
    int prefix = 1;
    av_log_format_line(avcl, level, format, args, line, sizeof(line), &prefix);
    LOG_WARNING(HW_GPU, "NS2 video: FFmpeg (#{}): {}", n, std::string_view{line});
}
} // namespace

struct H264Decoder::Impl {
    AVCodecContext* context{};
    AVCodecParserContext* parser{};
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
        if (parser) {
            av_parser_close(parser);
            parser = nullptr;
        }
        if (context) {
            avcodec_free_context(&context);
        }
    }

    bool Open(std::span<const u8> config) {
        Reset();
        static const bool log_installed = [] {
            av_log_set_callback(ForwardLog);
            return true;
        }();
        (void)log_installed;
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
        // Frame threading: several pictures decode in parallel. It adds a few frames of
        // latency (drained at end of stream) but keeps 1080p movies at full speed.
        context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        context->thread_count = 0;
        if (avcodec_open2(context, codec, nullptr) < 0) {
            LOG_ERROR(HW_GPU, "NS2 video: could not open the H.264 decoder");
            Reset();
            return false;
        }
        // The game's inputs are not guaranteed to be whole access units (frame threading needs
        // exactly one picture per packet): reassemble them with FFmpeg's H.264 parser.
        parser = av_parser_init(AV_CODEC_ID_H264);
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
    if (!impl->parser) {
        SendPacket(data.data(), data.size(), pts, frames);
        return frames;
    }
    const u8* in = data.data();
    int remaining = static_cast<int>(data.size());
    while (remaining > 0) {
        u8* out = nullptr;
        int out_size = 0;
        const int used = av_parser_parse2(impl->parser, impl->context, &out, &out_size, in, remaining,
                                          pts, AV_NOPTS_VALUE, 0);
        if (used < 0) {
            break;
        }
        in += used;
        remaining -= used;
        if (out_size > 0) {
            SendPacket(out, static_cast<size_t>(out_size), impl->parser->pts, frames);
        }
        if (used == 0 && out_size == 0) {
            break;
        }
    }
    return frames;
}

void H264Decoder::SendPacket(const u8* data, size_t size, s64 pts, std::vector<Frame>& frames) {
    if (av_new_packet(impl->packet, static_cast<int>(size)) < 0) {
        return;
    }
    std::memcpy(impl->packet->data, data, size);
    impl->packet->pts = pts;
    impl->packet->dts = AV_NOPTS_VALUE;
    int sent = avcodec_send_packet(impl->context, impl->packet);
    if (sent == AVERROR(EAGAIN)) {
        // Output must be collected before the decoder takes more input.
        Receive(frames);
        sent = avcodec_send_packet(impl->context, impl->packet);
    }
    av_packet_unref(impl->packet);
    if (sent < 0) {
        if (impl->errors++ < 8) {
            LOG_WARNING(HW_GPU, "NS2 video: H.264 packet of {} bytes rejected ({})", size, sent);
        }
        return;
    }
    Receive(frames);
}

void H264Decoder::Discard() {
    if (impl->context) {
        avcodec_flush_buffers(impl->context);
    }
    if (impl->parser) {
        av_parser_close(impl->parser);
        impl->parser = av_parser_init(AV_CODEC_ID_H264);
    }
}

std::vector<H264Decoder::Frame> H264Decoder::Drain() {
    std::vector<Frame> frames;
    if (!impl->context) {
        return frames;
    }
    if (impl->parser) {
        // The parser holds the last access unit until it sees the next one.
        u8* out = nullptr;
        int out_size = 0;
        av_parser_parse2(impl->parser, impl->context, &out, &out_size, nullptr, 0, AV_NOPTS_VALUE,
                         AV_NOPTS_VALUE, 0);
        if (out_size > 0) {
            SendPacket(out, static_cast<size_t>(out_size), impl->parser->pts, frames);
        }
        av_parser_close(impl->parser);
        impl->parser = av_parser_init(AV_CODEC_ID_H264);
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
        Frame& out = frames.emplace_back();
        const size_t luma = static_cast<size_t>(w) * h;
        out.nv12.resize(luma + static_cast<size_t>(w) * ((h + 1) / 2) + 2);
        out.width = static_cast<u32>(w);
        out.height = static_cast<u32>(h);
        const s64 best = impl->frame->best_effort_timestamp;
        out.pts = best != AV_NOPTS_VALUE ? best : impl->frame->pts;
        if (impl->frame->format == AV_PIX_FMT_YUV420P || impl->frame->format == AV_PIX_FMT_YUVJ420P) {
            // Common case: copy Y and interleave U/V directly (much cheaper than swscale).
            const AVFrame* f = impl->frame;
            for (int y = 0; y < h; ++y) {
                std::memcpy(out.nv12.data() + static_cast<size_t>(y) * w, f->data[0] + static_cast<ptrdiff_t>(y) * f->linesize[0], static_cast<size_t>(w));
            }
            const int cw = (w + 1) / 2, ch = (h + 1) / 2;
            for (int y = 0; y < ch; ++y) {
                const u8* u = f->data[1] + static_cast<ptrdiff_t>(y) * f->linesize[1];
                const u8* v = f->data[2] + static_cast<ptrdiff_t>(y) * f->linesize[2];
                u8* dst = out.nv12.data() + luma + static_cast<size_t>(y) * w;
                for (int x = 0; x < cw && 2 * x + 1 < w + 1; ++x) {
                    dst[2 * x] = u[x];
                    dst[2 * x + 1] = v[x];
                }
            }
            av_frame_unref(impl->frame);
            continue;
        }
        impl->scaler = sws_getCachedContext(impl->scaler, w, h,
                                            static_cast<AVPixelFormat>(impl->frame->format), w, h,
                                            AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!impl->scaler) {
            frames.pop_back();
            av_frame_unref(impl->frame);
            continue;
        }
        // Packed NV12: the Y plane (pitch = width) followed by the interleaved UV plane.
        u8* dst[4] = {out.nv12.data(), out.nv12.data() + luma, nullptr, nullptr};
        int dst_stride[4] = {w, (w + 1) & ~1, 0, 0};
        sws_scale(impl->scaler, impl->frame->data, impl->frame->linesize, 0, h, dst, dst_stride);
        av_frame_unref(impl->frame);
    }
}

} // namespace Tegra::NS2

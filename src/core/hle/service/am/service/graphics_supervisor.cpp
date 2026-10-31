// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <iterator>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <unordered_map>
#include <atomic>
#include <bit>
#include <array>
#include <mutex>
#include "common/ns2_watch.h"
#include "common/alignment.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/nvnflinger/buffer_queue_producer.h"
#include "core/hle/service/nvnflinger/ui/graphic_buffer.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_session.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/service/am/service/graphics_supervisor.h"
#include "core/hle/service/am/service/graphics_session.h"
#include "video_core/native_strict.h"
#include "video_core/host1x/ns2_video_decoder.h"
#include "video_core/textures/decoders.h"
#include "core/hle/service/am/service/gpu_session.h"
#include "core/hle/service/am/service/graphics_supervisor_protocol.h"
#include "core/hle/service/am/service/graphics_syncpoint_cache.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "core/hle/service/am/service/native_image_layout.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/vi/application_display_service.h"
#include "core/hle/service/vi/container.h"
#include "core/hle/service/vi/manager_root_service.h"
#include "core/hle/service/vi/vi_types.h"

namespace Service::AM {
struct NativeImageRegistry {
    struct Image {
        u32 id;
        u32 memory_id;
        std::array<u32, 16> preference;
        u64 size;
        bool window_pool{};
    };
    NativeImageRegistry(Kernel::KernelCore& kernel_, Kernel::KProcess* process_)
        : kernel{kernel_}, process{process_} {
        process->Open(kernel);
    }
    ~NativeImageRegistry() {
        if (module) {
            for (const auto& image : images) {
                ReleaseMemory(image.memory_id);
            }
        }
        if (memory_session) {
            module->GetContainer().CloseSession(*memory_session);
        }
        process->Close(kernel);
    }
    void ReleaseMemory(u32 id) {
        const auto info = module->GetContainer().GetNvMapFile().FreeHandle(id, true);
        if (info && info->can_unlock && info->address != 0) {
            process->GetPageTable().UnlockForDeviceAddressSpace(info->address, info->size);
        }
    }
    Kernel::KernelCore& kernel;
    Kernel::KProcess* process;
    std::mutex mutex;
    std::shared_ptr<Nvidia::Module> module;
    std::optional<Nvidia::NvCore::SessionId> memory_session;
    std::vector<Image> images;
};
using NativeImage = NativeImageRegistry::Image;

// Layout of a decoded picture inside a video decoder output image: pitch-linear NV12, the Y
// plane at offset 0 and the interleaved UV plane after it. The decoder writes this layout and
// GetCpuUserImageDescriptor (961) describes it to nn::gs::sdk::ReadUserImageNv12.
struct DecoderImageLayout {
    u32 width;
    u32 height;
    u32 pitch;
    u32 uv_offset;
};
static std::optional<DecoderImageLayout> GetDecoderImageLayout(const NativeImage& image) {
    const u32 width = (image.preference[2] & 0xffffU) + 1;
    const u32 height = (image.preference[2] >> 16) + 1;
    const u32 pitch = (width + 63) & ~63U;
    const u32 uv_offset = pitch * ((height + 1) & ~1U);
    if (u64{uv_offset} + u64{pitch} * ((height + 1) / 2) > image.size) {
        return std::nullopt;
    }
    return DecoderImageLayout{width, height, pitch, uv_offset};
}


namespace {

// TIPC payloads follow a 32-bit result, so these handlers push result.raw.

// Session returned by context commands 902/1002. The SDK fetches an event from it with
// command 1 and closes it on teardown; the event is never signaled for now.
// Session returned by nn::codec's video service manager (CreateVideoDecoderService /
// CreateVideoEncoderService). The real service drives the hardware video codec; this HLE
// version records the protocol and acknowledges every request so movie playback can proceed.
// Session returned by nn::codec's video service manager (CreateVideoDecoderService /
// CreateVideoEncoderService). Frames are not decoded: every submitted input frame completes at
// once into the next free registered output image, so movies run to their end (without a
// picture) instead of stalling. Protocol (from the SDK; matches Eglantine's nx2:vdecsvc):
// - 200/201: initialize (VP9 / H.264), 101: configure (X descriptor, then width, height, ...),
//   3: notification event (copy handle), 6/207/8: start/stop/flush, 115: finalize.
// - 311(image): register an output image, 314(image, u64 consumer fence): give one back,
//   212(image): unregister. The client keeps its own ring of queued images (nn::codec acquires
//   from it without IPC); 311/314 reply the NativeSyncHandle {~counter, mode, u64 value} that
//   is reached when a decoded frame is in that image. An all-zero handle counts as reached at
//   once, so it must not be used for images that have no frame yet.
// - 109(A: compressed frame, u64 pts, u8 end of stream): submit, reply u64 submission id.
// - 213(): next finished frame (image, width, height, width, height, colour space, u64 pts,
//   u8 end of stream) or 0x1c286f when none is ready (the client then waits on the event).
class IVideoCodecService final : public ServiceFramework<IVideoCodecService> {
public:
    IVideoCodecService(Core::System& system_, bool encoder, std::shared_ptr<NativeImageRegistry> images)
        : ServiceFramework{system_, encoder ? "IVideoEncService" : "IVideoDecService"},
          m_encoder{encoder}, m_context{system_, encoder ? "IVideoEncService" : "IVideoDecService"},
          m_images{std::move(images)} {}
    ~IVideoCodecService() override {
        {
            std::scoped_lock lk{m_ready->mutex};
            m_ready->event = nullptr;
        }
        if (m_ready_timer) {
            system.CoreTiming().UnscheduleEvent(m_ready_timer);
        }
        if (m_event) {
            m_context.CloseEvent(m_event);
        }
        if (m_syncpoint) {
            GraphicsSyncpointCache::Unpublish(*m_syncpoint);
            if (m_images && m_images->module) {
                m_images->module->GetContainer().GetSyncpointManager().FreeSyncpoint(*m_syncpoint);
            }
        }
    }

    Result HandleSyncRequest(Kernel::KServerSession& session, HLERequestContext& ctx) override {
        if (!ctx.IsTipc()) {
            return ServiceFrameworkBase::HandleSyncRequest(session, ctx);
        }
        const auto guard = LockService();
        const auto* packet = ctx.CommandBuffer();
        const u32 command = ctx.GetCommand();
        const u32 words = (std::min)(packet[1] & 0x3ffU, 24U);
        // Per-command sampling: polling commands (e.g. 213) would otherwise flood the log.
        const u32 seen = m_command_counts[command]++;
        const bool buffer_command = command == 311 || command == 314 || command == 212 || command == 213 ||
                                    command == 3 || command == 6 || command == 8 || command == 207;
        if (seen < 16 || seen % 1000 == 0 || (buffer_command && seen < 300)) {
            std::string data;
            for (u32 i = 0; i < words; ++i) {
                data += fmt::format("{:08x} ", packet[2 + i]);
            }
            LOG_INFO(Service_AM, "video {} service command={} (#{}) header={:08x}/{:08x} data={}",
                     m_encoder ? "encoder" : "decoder", command, seen, packet[0], packet[1], data);
        }

        if (m_encoder) {
            Reply(ctx, {0, 0, 0, 0, 0, 0, 0, 0});
        } else {
            switch (command) {
            case 3: GetEvent(ctx); break;
            case 101:
                m_width = packet[5];
                m_height = packet[6];
                {
                    // Words 2-4: A descriptor of the codec configuration (avcC / SPS+PPS).
                    const auto config = ReadDescriptor(packet + 2, 4096);
                    m_decoder.Configure(config);
                }
                Reply(ctx, {0, 0});
                break;
            case 109:
                SubmitInput(ctx);
                DumpWorkMemory();
                break;
            case 212:
                std::erase_if(m_free, [&](const FreeImage& free) { return free.image == packet[2]; });
                Reply(ctx, {});
                break;
            case 213: GetOutput(ctx); break;
            case 311:
            case 314: {
                // 311 registers an output image, 314 gives a consumed one back. Either way the
                // client queues the image as its next output and waits on the reply's fence
                // (NativeSyncHandle {~counter, mode, u64 value}) before reading it with 213, so
                // the fence must only be reached once a frame has been written into the image.
                EnsureSyncpoint();
                const auto existing = std::ranges::find(m_free, packet[2], &FreeImage::image);
                const u32 target = existing != m_free.end() ? existing->target : GiveBack(packet[2]);
                if (m_fences_logged++ < 32) {
                    LOG_INFO(Service_AM, "video decoder: image {} queued (command {}), fence counter {} value {}",
                             packet[2], command, m_syncpoint.value_or(~0U), target);
                }
                if (m_syncpoint) {
                    Reply(ctx, {~*m_syncpoint, 0, target, 0});
                } else {
                    Reply(ctx, {0, 0, 0, 0});
                }
                break;
            }
            case 200: case 201:
                EnsureSyncpoint();
                if (const Handle handle = ctx.GetCopyHandle(0); handle != 0) {
                    if (auto memory = ctx.GetObjectFromHandle<Kernel::KTransferMemory>(handle); memory.IsNotNull()) {
                        m_work_address = GetInteger(memory->GetSourceAddress());
                        m_work_size = memory->GetSize();
                        LOG_INFO(Service_AM, "video decoder: work memory {:#x} size {:#x}", m_work_address, m_work_size);
                    }
                }
                Reply(ctx, {});
                Signal();
                break;
            case 6: case 8: case 115: case 207:
                Reply(ctx, {});
                Signal();
                break;
            default:
                Reply(ctx, {0, 0, 0, 0, 0, 0, 0, 0});
                break;
            }
        }
        return system.IsPoweredOn() ? ctx.WriteToOutgoingCommandBuffer() : ResultSuccess;
    }

private:
    struct Picture {
        std::vector<u8> rgba; // Packed NV12 (see Tegra::NS2::H264Decoder).
        u32 width{};
        u32 height{};
    };
    struct Output {
        u32 image;
        u64 pts;
        bool end;
        std::shared_ptr<Picture> picture;
    };
    struct ReadyState {
        std::mutex mutex;
        std::atomic<bool> can_submit{true};
        Kernel::KEvent* event{};
    };

    void Reply(HLERequestContext& ctx, std::initializer_list<u32> data) {
        // Size counts the result word plus one (as the other TIPC replies here: {ctx, 2} = result).
        IPC::ResponseBuilder rb{ctx, static_cast<u32>(2 + data.size())};
        rb.Push(ResultSuccess.raw);
        for (const u32 word : data) {
            rb.Push<u32>(word);
        }
    }

    // The notification event is nn::codec's "submission ready" event: nn::movie waits on it
    // before it submits input. Keep it signalled only while the decoder has room, so the game
    // paces its input instead of having inputs refused (refused inputs got lost or came back
    // out of order, which corrupted the picture into blocks and made the movie skip).
    void Signal() {
        const bool room = m_waiting.size() < MaxWaitingFrames;
        m_ready->can_submit = room;
        if (!m_event) {
            return;
        }
        if (room) {
            m_event->Signal(kernel);
        } else {
            m_event->Clear(kernel);
        }
    }

    void GetEvent(HLERequestContext& ctx) {
        if (!m_event) {
            m_event = m_context.CreateEvent("VideoDecoderNotification");
            // The client attaches the event auto-clearing; re-signal it regularly so a consumed
            // signal never leaves the decoder thread waiting (a spurious wake only re-checks).
            {
                std::scoped_lock lk{m_ready->mutex};
                m_ready->event = m_event;
            }
            m_ready_timer = Core::Timing::CreateEvent(
                "VideoDecoderReady",
                [ready = m_ready, &kernel_ref = system.Kernel()](
                    s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                    std::scoped_lock lk{ready->mutex};
                    if (ready->event && ready->can_submit) {
                        ready->event->Signal(kernel_ref);
                    }
                    return std::nullopt;
                });
            constexpr std::chrono::nanoseconds interval{std::chrono::milliseconds{4}};
            system.CoreTiming().ScheduleLoopingEvent(interval, interval, m_ready_timer);
        }
        if (m_event) {
            LOG_INFO(Service_AM, "video decoder: notification event readable at {}",
                     fmt::ptr(&m_event->GetReadableEvent()));
        }
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push((m_event ? ResultSuccess : Kernel::ResultOutOfResource).raw);
        if (m_event) {
            ctx.AddCopyObject(&m_event->GetReadableEvent());
        }
        Signal();
    }

    // Reads an IPC A/B buffer descriptor (size, address low, packed high bits) from guest memory.
    std::vector<u8> ReadDescriptor(const u32* d, u64 max_size) {
        const u64 size = u64{d[0]} | (u64{(d[2] >> 24) & 0xf} << 32);
        const u64 address = u64{d[1]} | (u64{(d[2] >> 28) & 0xf} << 32) | (u64{(d[2] >> 2) & 0x7} << 36);
        std::vector<u8> data;
        if (!m_images || !address || !size || size > max_size) {
            return data;
        }
        data.resize(size);
        m_images->process->GetMemory().ReadBlock(address, data.data(), size);
        return data;
    }

    // Writes a decoded NV12 picture into a registered output image (see DecoderImageLayout).
    void WritePicture(u32 image_id, const std::shared_ptr<Picture>& picture) {
        if (!picture || picture->rgba.empty() || !m_images) {
            return;
        }
        const std::scoped_lock images_lock{m_images->mutex};
        const auto image = std::ranges::find(m_images->images, image_id, &NativeImage::id);
        if (image == m_images->images.end() || !m_images->module) {
            return;
        }
        const auto handle = m_images->module->GetContainer().GetNvMapFile().GetHandle(image->memory_id);
        if (!handle || handle->address == 0) {
            return;
        }
        const auto layout = GetDecoderImageLayout(*image);
        if (!layout) {
            return;
        }
        const u32 rows = (std::min)(picture->height, layout->height);
        const u32 row_bytes = (std::min)(picture->width, layout->width);
        const size_t luma = static_cast<size_t>(picture->width) * picture->height;
        auto& memory = m_images->process->GetMemory();
        if (picture->width == layout->pitch && picture->height == layout->height &&
            layout->uv_offset == luma && picture->rgba.size() >= luma + luma / 2) {
            // The decoder's packed NV12 already has the image layout: write it in one go.
            memory.WriteBlock(handle->address, picture->rgba.data(), luma + luma / 2);
        } else {
            // Y rows, then the UV rows (half height), clipped into the image.
            m_linear.assign(static_cast<size_t>(layout->uv_offset) + static_cast<size_t>(layout->pitch) * ((layout->height + 1) / 2), 0);
            for (u32 y = 0; y < rows; ++y) {
                std::memcpy(m_linear.data() + static_cast<size_t>(y) * layout->pitch,
                            picture->rgba.data() + static_cast<size_t>(y) * picture->width, row_bytes);
            }
            for (u32 y = 0; y < (rows + 1) / 2; ++y) {
                const size_t source = luma + static_cast<size_t>(y) * picture->width;
                if (source + row_bytes > picture->rgba.size()) break;
                std::memcpy(m_linear.data() + layout->uv_offset + static_cast<size_t>(y) * layout->pitch,
                            picture->rgba.data() + source, row_bytes);
            }
            memory.WriteBlock(handle->address, m_linear.data(), m_linear.size());
        }
        if (const u32 n = m_pictures_written++; n < 6 || n % 300 == 0) {
            // Average Y and U/V of the written picture, to tell a real frame from a blank one.
            u64 y_sum = 0, u_sum = 0, v_sum = 0, y_count = 0, uv_count = 0;
            for (size_t i = 0; i < luma; i += 97, ++y_count) y_sum += picture->rgba[i];
            for (size_t i = luma; i + 1 < picture->rgba.size(); i += 98, ++uv_count) {
                u_sum += picture->rgba[i];
                v_sum += picture->rgba[i + 1];
            }
            LOG_INFO(Service_AM, "video decoder: picture #{} {}x{} written to image {} at {:#x} (mean Y {} U {} V {})",
                     n, picture->width, picture->height, image_id, handle->address,
                     y_count ? y_sum / y_count : 0, uv_count ? u_sum / uv_count : 0,
                     uv_count ? v_sum / uv_count : 0);
        }
    }

    // The decoder's own counter: each queued output image gets the next value, and the counter
    // advances when a frame lands in that image (images fill in the order they were queued).
    void EnsureSyncpoint() {
        if (m_syncpoint || !m_images || !m_images->module) {
            return;
        }
        auto& syncpoints = m_images->module->GetContainer().GetSyncpointManager();
        const u32 id = syncpoints.AllocateSyncpoint(false);
        if (id == 0 || id >= GraphicsSyncpointCache::CounterCount) {
            LOG_WARNING(Service_AM, "video decoder: unusable syncpoint {}", id);
            if (id != 0) syncpoints.FreeSyncpoint(id);
            return;
        }
        m_syncpoint = id;
        m_fence_assigned = syncpoints.ReadAllocatedSyncpoint(id).value_or(0);
        GraphicsSyncpointCache::Publish(id);
        LOG_INFO(Service_AM, "video decoder: output fences use syncpoint {} (value {})", id, m_fence_assigned);
    }

    void Complete(u32 target) {
        if (!m_syncpoint) {
            return;
        }
        auto& host = system.Host1x().GetSyncpointManager();
        for (u32 i = 0; i < 64 && static_cast<s32>(host.GetHostSyncpointValue(*m_syncpoint) - target) < 0; ++i) {
            host.IncrementHost(*m_syncpoint);
        }
    }

    // An output image is free again: a frame waiting for one completes into it. Returns the
    // fence value that is reached once the image holds a frame.
    u32 GiveBack(u32 image) {
        const u32 target = ++m_fence_assigned;
        if (!m_waiting.empty()) {
            auto frame = std::move(m_waiting.front());
            m_waiting.pop_front();
            WritePicture(image, frame.picture);
            m_outputs.push_back({image, frame.pts, frame.end, nullptr});
            Complete(target);
            Signal();
            return target;
        }
        m_free.push_back({image, target});
        return target;
    }

    void SubmitInput(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        // Words 2-4 are the A descriptor of the compressed frame, then u64 pts (microseconds)
        // and the end-of-stream flag.
        const u64 pts = u64{w[5]} | (u64{w[6]} << 32);
        const bool end = (w[7] & 0xff) != 0;
        // Back-pressure: while decoded frames already wait for output images, refuse the input
        // with the "busy" result. nn::movie keeps the buffer, waits for the notification event
        // and submits it again. Accepting everything let the game push ~15000 inputs a second;
        // the waiting queue overflowed, frames lost their pixels and the images kept showing
        // the movie's first (white) frames.
        if (m_waiting.size() >= MaxWaitingFrames) {
            if (const u32 n = m_busy_replies++; n < 8 || n % 500 == 0) {
                LOG_INFO(Service_AM, "video decoder: input refused (busy, #{}), {} frames waiting", n, m_waiting.size());
            }
            Signal();
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push<u32>(DecoderBusy);
            return;
        }
        const auto data = ReadDescriptor(w + 2, 16ULL << 20);
        auto frames = m_decoder.Decode(data, static_cast<s64>(pts));
        if (end) {
            auto rest = m_decoder.Drain();
            std::move(rest.begin(), rest.end(), std::back_inserter(frames));
        }
        // Each picture becomes one output, in presentation order. An empty input without the
        // end flag produces nothing (the old code emitted a repeated picture for every input,
        // and the game then spammed empty inputs).
        for (size_t i = 0; i < frames.size(); ++i) {
            auto picture = std::make_shared<Picture>();
            picture->rgba = std::move(frames[i].nv12);
            picture->width = frames[i].width;
            picture->height = frames[i].height;
            m_last_picture = picture;
            Emit(std::move(picture), static_cast<u64>(frames[i].pts), end && i + 1 == frames.size());
        }
        if (end && frames.empty()) {
            Emit(m_last_picture, pts, true);
        }
        if (data.empty() && !end && m_empty_inputs++ < 4) {
            LOG_INFO(Service_AM, "video decoder: empty input without end of stream (pts {})", pts);
        }
        const u64 id = ++m_submitted;
        Reply(ctx, {static_cast<u32>(id), static_cast<u32>(id >> 32)});
        Signal();
    }

    void Emit(std::shared_ptr<Picture> picture, u64 pts, bool end) {
        if (!m_free.empty()) {
            const FreeImage free = m_free.front();
            m_free.pop_front();
            WritePicture(free.image, picture);
            m_outputs.push_back({free.image, pts, end, nullptr});
            Complete(free.target);
            return;
        }
        m_waiting.push_back({0, pts, end, std::move(picture)});
    }

    void GetOutput(HLERequestContext& ctx) {
        if (m_outputs.empty()) {
            // The code the client's own acquire uses for an empty ring.
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push<u32>(0x1c286f);
            return;
        }
        const Output output = m_outputs.front();
        m_outputs.pop_front();
        if (const u32 n = m_outputs_given++; n < 64 || n % 100 == 0) {
            LOG_INFO(Service_AM, "video decoder: output {} -> image {} pts {} (free {}, waiting {}, ready {})",
                     m_outputs_given, output.image, output.pts, m_free.size(), m_waiting.size(), m_outputs.size());
        }
        // Inputs carry microseconds (nn::codec converts the TimeSpan before sending), but the
        // client copies this value straight into the frame's nn::TimeSpan (nanoseconds). In
        // microseconds every frame looked far behind the playback clock.
        const u64 pts_ns = output.pts * 1000;
        Reply(ctx, {output.image, m_width, m_height, m_width, m_height, 0,
                    static_cast<u32>(pts_ns), static_cast<u32>(pts_ns >> 32),
                    output.end ? 1U : 0U, 0, 0, 0, 0, 0, 0, 0});
    }

    bool m_encoder;
    std::unordered_map<u32, u32> m_command_counts;
    KernelHelpers::ServiceContext m_context;
    Kernel::KEvent* m_event{};
    std::shared_ptr<ReadyState> m_ready = std::make_shared<ReadyState>();
    std::shared_ptr<Core::Timing::EventType> m_ready_timer;
    u32 m_width{};
    u32 m_height{};
    u64 m_submitted{};
    u64 m_work_address{};
    u64 m_work_size{};
    u32 m_work_dumps{};
    u32 m_outputs_given{};
    u32 m_empty_inputs{};
    u32 m_busy_replies{};
    // A decoded frame without a free output image waits here. One input can yield up to a few
    // pictures (end-of-stream drain), so the limit is checked before decoding.
    static constexpr size_t MaxWaitingFrames = 6;
    // nn::codec's "input queue full": nn::movie answers it by retrying later (result 0x79).
    static constexpr u32 DecoderBusy = 0x1c266f;

    // Logs the first words of the decoder's work memory at a few points, to see whether the
    // client keeps state (for example ring indices) there that the service must update.
    void DumpWorkMemory() {
        if (!m_work_address || !m_images || m_work_dumps >= 6) return;
        const u64 submitted = m_submitted;
        if (submitted != 1 && submitted != 2 && submitted != 30 && submitted != 300 &&
            submitted != 1000 && submitted != 3000)
            return;
        ++m_work_dumps;
        std::array<u32, 64> words{};
        m_images->process->GetMemory().ReadBlock(m_work_address, words.data(), sizeof(words));
        std::string text;
        for (u32 i = 0; i < words.size(); ++i) text += fmt::format(" {:08x}", words[i]);
        LOG_INFO(Service_AM, "video decoder: work memory after {} inputs:{}", submitted, text);
    }
    std::shared_ptr<NativeImageRegistry> m_images;
    Tegra::NS2::H264Decoder m_decoder;
    std::shared_ptr<Picture> m_last_picture;
    std::vector<u8> m_linear;
    u32 m_pictures_written{};
    struct FreeImage {
        u32 image;
        u32 target;
    };
    std::deque<FreeImage> m_free;
    std::optional<u32> m_syncpoint;
    u32 m_fence_assigned{};
    u32 m_fences_logged{};
    std::deque<Output> m_waiting;
    std::deque<Output> m_outputs;
};

// Video codec service manager opened through graphics supervisor command 902 (decoder) or
// 1002 (encoder). Command 1 creates a codec service session.
class IVideoCodecManager final : public ServiceFramework<IVideoCodecManager> {
public:
    IVideoCodecManager(Core::System& system_, bool encoder, std::shared_ptr<NativeImageRegistry> images)
        : ServiceFramework{system_, encoder ? "IVideoEncManager" : "IVideoDecManager"},
          m_encoder{encoder}, m_images{std::move(images)} {
        static const FunctionInfo functions[] = {
            {1, &IVideoCodecManager::CreateService, "CreateService"},
        };
        RegisterHandlersTipc(functions);
    }

    Result HandleSyncRequest(Kernel::KServerSession& session, HLERequestContext& ctx) override {
        if (!ctx.IsTipc() || handlers_tipc.contains(ctx.GetCommand())) {
            return ServiceFrameworkBase::HandleSyncRequest(session, ctx);
        }
        const auto guard = LockService();
        LOG_WARNING(Service_AM, "(STUBBED) video {} manager command={}", m_encoder ? "encoder" : "decoder",
                    ctx.GetCommand());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess.raw);
        return system.IsPoweredOn() ? ctx.WriteToOutgoingCommandBuffer() : ResultSuccess;
    }

private:
    void CreateService(HLERequestContext& ctx);

    bool m_encoder;
    std::shared_ptr<NativeImageRegistry> m_images;
};

class ISyncpointSupervisor final : public ServiceFramework<ISyncpointSupervisor> {
public:
    ISyncpointSupervisor(Core::System& system_, std::shared_ptr<Nvidia::Module> module)
        : ServiceFramework{system_, "ISyncpointSupervisor"}, m_module{std::move(module)},
          m_context{system_, "SyncpointSupervisor"} {
        // Events are signalled when the threshold requested with command 201 is reached. The
        // client trusts the signal without re-checking the counter (the video decoder aborts on
        // a spurious one), so events without a pending request are left alone.
        m_timer = Core::Timing::CreateEvent(
            "SyncpointSupervisor::Poll",
            [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                std::scoped_lock lock{m_events_mutex};
                for (size_t i = 0; i < m_events.size(); ++i) {
                    if (m_requests[i] && m_events[i] && Reached(*m_requests[i])) {
                        m_requests[i].reset();
                        m_events[i]->Signal(kernel);
                    }
                }
                return std::nullopt;
            });
        static const FunctionInfo functions[] = {
            {1, &ISyncpointSupervisor::GetMaximumEventCount, "GetMaximumEventCount"},
            {21, &ISyncpointSupervisor::SetEventCount, "SetEventCount"},
            {22, &ISyncpointSupervisor::GetEvent, "GetEvent"},
            {103, &ISyncpointSupervisor::QueryCounter, "QueryCounter"},
            {201, &ISyncpointSupervisor::RequestEvent, "RequestEvent"},
            {202, &ISyncpointSupervisor::CancelEvent, "CancelEvent"},
        };
        RegisterHandlersTipc(functions);
    }

    Result HandleSyncRequest(Kernel::KServerSession& session, HLERequestContext& ctx) override {
        if (!ctx.IsTipc() || handlers_tipc.contains(ctx.GetCommand())) {
            return ServiceFrameworkBase::HandleSyncRequest(session, ctx);
        }
        const auto guard = LockService();
        LOG_INFO(Service_AM, "syncpoint unsupported command={}, words={:08x}", ctx.GetCommand(),
                  fmt::join(std::span<const u32>(ctx.CommandBuffer() + 2, std::min<u32>(ctx.CommandBuffer()[1], 32)), ","));
        InvokeRequestTipc(ctx);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(Kernel::ResultNotImplemented.raw);
        return system.IsPoweredOn() ? ctx.WriteToOutgoingCommandBuffer() : ResultSuccess;
    }

private:
    static constexpr u32 MaximumEventCount = 63;

    void GetMaximumEventCount(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        const Result result = packet[0] == 0x11 && packet[1] == 0
            ? ResultSuccess : Kernel::ResultInvalidCombination;
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(result.raw);
        rb.Push(MaximumEventCount);
        LOG_INFO(Service_AM, "syncpoint maximum events={}", MaximumEventCount);
    }

    void SetEventCount(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        Result result = Kernel::ResultInvalidCombination;
        if (packet[0] == 0x25 && packet[1] == 1 && packet[2] <= MaximumEventCount) {
            m_event_count = packet[2];
            result = ResultSuccess;
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
        LOG_INFO(Service_AM, "syncpoint event count={}, result={:#x}", m_event_count, result.raw);
    }

    void QueryCounter(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        Result result = Kernel::ResultInvalidCombination;
        u64 value{};
        if (packet[0] == 0x77 && packet[1] == 1) {
            const u32 id = ~packet[2];
            auto current = m_module->GetContainer().GetSyncpointManager().ReadAllocatedSyncpoint(id);
            result = current ? ResultSuccess : Kernel::ResultNotFound;
            value = current.value_or(0);
            LOG_INFO(Service_AM, "query syncpoint id={}, value={}, result={:#x}", id, value, result.raw);
        }
        IPC::ResponseBuilder rb{ctx, 5};
        rb.Push(result.raw);
        rb.Push(value);
        rb.Push<u32>(0); // Existing NVDRV counters use 32-bit modular comparisons.
    }

    struct Request {
        u32 id;
        u64 value;
        bool wide;
    };

    bool Reached(const Request& request) {
        const auto current = m_module->GetContainer().GetSyncpointManager().ReadAllocatedSyncpoint(request.id);
        if (!current) {
            return true; // The client treats unallocated counters as reached as well.
        }
        return request.wide ? u64{*current} >= request.value
                            : static_cast<s32>(*current - static_cast<u32>(request.value)) >= 0;
    }

    // 201(event index, ~counter id, u64 threshold, u8 mode): signal the event once the counter
    // reaches the threshold (mode 0: 32-bit wrapping comparison, mode 1: 64-bit).
    void RequestEvent(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        Result result = Kernel::ResultInvalidCombination;
        const u32 index = packet[2];
        if (packet[1] == 5 && index < m_events.size()) {
            std::scoped_lock lock{m_events_mutex};
            if (auto* event = m_events[index]) {
                const Request request{~packet[3], u64{packet[4]} | (u64{packet[5]} << 32),
                                      (packet[6] & 0xff) == 1};
                event->Clear(kernel);
                if (Reached(request)) {
                    m_requests[index].reset();
                    event->Signal(kernel);
                } else {
                    m_requests[index] = request;
                }
                result = ResultSuccess;
                if (m_requests_logged++ < 64) {
                    LOG_INFO(Service_AM, "syncpoint event {} requested for counter {} value {} (mode {}, pending {})",
                             index, request.id, request.value, packet[6] & 0xff, m_requests[index].has_value());
                }
            }
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }

    void CancelEvent(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        Result result = Kernel::ResultInvalidCombination;
        if (packet[1] == 1 && packet[2] < m_events.size()) {
            std::scoped_lock lock{m_events_mutex};
            m_requests[packet[2]].reset();
            // nn::syncpt::sdk waits on the slot's event after a cancel, so a cancel signals it like
            // a completion (the client re-checks the counter itself). Otherwise each cancel of a
            // pending request stalled the caller until its wait timed out (behaviour confirmed
            // by Eglantine, whose movie player hung on this).
            if (auto* event = m_events[packet[2]]) {
                event->Signal(kernel);
            }
            result = ResultSuccess;
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }

    void GetEvent(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        Result result = Kernel::ResultInvalidCombination;
        Kernel::KReadableEvent* readable{};
        if (packet[0] == 0x26 && packet[1] == 1 && packet[2] < m_events.size()) {
            std::scoped_lock lock{m_events_mutex};
            auto& event = m_events[packet[2]];
            if (!event) {
                event = m_context.CreateEvent(fmt::format("SyncpointEvent{}", packet[2]));
                if (event) event->Signal(kernel);
            }
            result = event ? ResultSuccess : Kernel::ResultOutOfResource;
            if (event) {
                readable = &event->GetReadableEvent();
                readable->Open(kernel);
            }
            if (event && !m_timer_running) {
                m_timer_running = true;
                system.CoreTiming().ScheduleLoopingEvent(std::chrono::milliseconds{1},
                                                         std::chrono::milliseconds{1}, m_timer);
            }
        }
        LOG_INFO(Service_AM, "syncpoint event index={}, result={:#x}", packet[1] ? packet[2] : 0, result.raw);
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(result.raw);
        ctx.AddMoveObject(readable);
    }

public:
    ~ISyncpointSupervisor() override {
        if (m_timer_running) system.CoreTiming().UnscheduleEvent(m_timer);
        std::scoped_lock lock{m_events_mutex};
        for (auto* event : m_events) {
            if (event) m_context.CloseEvent(event);
        }
    }

private:
    std::shared_ptr<Nvidia::Module> m_module;
    u32 m_event_count{};
    KernelHelpers::ServiceContext m_context;
    std::mutex m_events_mutex;
    std::array<Kernel::KEvent*, 64> m_events{};
    std::array<std::optional<Request>, 64> m_requests{};
    u32 m_requests_logged{};
    std::shared_ptr<Core::Timing::EventType> m_timer;
    bool m_timer_running{};
};

class IVi2Session final : public ServiceFramework<IVi2Session> {
    struct ClientState {
        u64 token{};
        u32 slots{};
        std::optional<u64> display;
        std::optional<u64> layer;
        u64 available{0x7fff};
        u32 retire_notification{};
        u64 output_encoding{0x14};
        u32 available_notification{};
        std::shared_ptr<android::BufferQueueProducer> producer;
        std::array<std::optional<s32>, 15> queued{};
        std::optional<size_t> player_display;
    };

public:
    explicit IVi2Session(Core::System& system_, u64 aruid, std::shared_ptr<NativeImageRegistry> images) : ServiceFramework{system_, "IVi2Session"}, m_images{std::move(images)}, m_aruid{aruid}, m_context{system_, "Vi2Session"} {
        static const FunctionInfo functions[] = {
            {1, &IVi2Session::GetSessionLimit, "GetNotificationLimit"},
            {11, &IVi2Session::GetNotificationEvent, "GetNotificationEvent"},
            {13, &IVi2Session::ReleaseNotificationEvent, "ReleaseNotificationEvent"},
            {21, &IVi2Session::GetNotifications, "GetNotifications"},
            {31, &IVi2Session::GetRequestEvent, "GetRequestEvent"},
            {32, &IVi2Session::GetRequestEventWithHandle, "GetRequestEventWithHandle"},
            {103, &IVi2Session::StubSuccess, "Unknown103"},
            {106, &IVi2Session::StubSuccess, "Unknown106"},
            {107, &IVi2Session::StubSuccess, "Unknown107"},
            {113, &IVi2Session::StubSuccess, "Unknown113"},
            {261, &IVi2Session::StubSuccess, "Unknown261"},
            {262, &IVi2Session::StubSuccess, "Unknown262"},
            {263, &IVi2Session::StubSuccess, "Unknown263"},
            {264, &IVi2Session::StubSuccess, "Unknown264"},
            {265, &IVi2Session::StubSuccess, "Unknown265"},
            {266, &IVi2Session::StubSuccess, "Unknown266"},
            {267, &IVi2Session::StubSuccess, "Unknown267"},
            {268, &IVi2Session::StubSuccess, "Unknown268"},
            {269, &IVi2Session::StubSuccess, "Unknown269"},
            {270, &IVi2Session::StubSuccess, "Unknown270"},
            {271, &IVi2Session::StubSuccess, "Unknown271"},
            {272, &IVi2Session::StubSuccess, "Unknown272"},
            {301, &IVi2Session::StubSuccess, "Unknown301"},
            {302, &IVi2Session::StubSuccess, "Unknown302"},
            {303, &IVi2Session::StubSuccess, "Unknown303"},
            {315, &IVi2Session::StubSuccess, "Unknown315"},
            {352, &IVi2Session::StubSuccess, "Unknown352"},
            {353, &IVi2Session::StubSuccess, "Unknown353"},
            {361, &IVi2Session::StubSuccess, "Unknown361"},
            {104, &IVi2Session::StubZeroResult104, "Unknown104"},
            {312, &IVi2Session::StubSuccess, "Unknown312"},
            {314, &IVi2Session::StubSuccess, "Unknown314"},
            {391, &IVi2Session::StubSuccess, "Unknown391"},
            {105, &IVi2Session::StubZeroWord, "Unknown105"},
            {311, &IVi2Session::StubZeroWord, "Unknown311"},
            {321, &IVi2Session::StubZeroWord, "Unknown321"},
            {305, &IVi2Session::GetUnknownEvent305, "Unknown305"},
            {3, &IVi2Session::GetSessionLimit, "GetSessionLimit"},
            {5, &IVi2Session::GetSessionLimit, "GetRequestGroupLimit"},
            {51, &IVi2Session::ConfigureRequestGroups, "ConfigureRequestGroups"},
            {101, &IVi2Session::OpenDisplay, "OpenDisplay"},
            {102, &IVi2Session::CloseDisplay, "CloseDisplay"},
            {111, &IVi2Session::OpenEndpoint, "OpenEndpoint"},
            {112, &IVi2Session::CloseEndpoint, "CloseEndpoint"},
            {201, &IVi2Session::RegisterClientState, "RegisterClientState"},
            {202, &IVi2Session::UnregisterClientState, "UnregisterClientState"},
            {212, &IVi2Session::GetClientAvailability, "GetClientAvailability"},
            {229, &IVi2Session::PresentClientBuffer, "PresentClientBuffer"},
            {228, &IVi2Session::CancelClientBuffer, "CancelClientBuffer"},
            {351, &IVi2Session::SetClientOutputEncoding, "SetClientOutputEncoding"},
            {251, &IVi2Session::BindClientDisplay, "BindClientDisplay"},
            {252, &IVi2Session::UnbindClientDisplay, "UnbindClientDisplay"},
        };
        RegisterHandlersTipc(functions);
    }

    ~IVi2Session() override {
        if (m_release_poll) system.CoreTiming().UnscheduleEvent(m_release_poll);
        if (m_display_service) {
            for (auto& state : m_client_states) {
                if (state.layer) m_display_service->GetContainer()->DestroyManagedLayer(*state.layer);
            }
        }
        if (m_default_display) { m_display_service->CloseDisplay(*m_default_display); }
        m_context.CloseEvent(m_event);
        for (auto* event : m_request_events) {
            if (event) m_context.CloseEvent(event);
        }
        if (m_event_305) m_context.CloseEvent(m_event_305);
    }

    Result HandleSyncRequest(Kernel::KServerSession& session, HLERequestContext& ctx) override {
        if (!ctx.IsTipc() || handlers_tipc.contains(ctx.GetCommand())) {
            return ServiceFrameworkBase::HandleSyncRequest(session, ctx);
        }
        const auto guard = LockService();
        LOG_INFO(Service_AM, "VI2 unsupported command={}, words={:08x}", ctx.GetCommand(),
                  fmt::join(std::span<const u32>(ctx.CommandBuffer() + 2, std::min<u32>(ctx.CommandBuffer()[1], 32)), ","));
        InvokeRequestTipc(ctx);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(Kernel::ResultNotImplemented.raw);
        return system.IsPoweredOn() ? ctx.WriteToOutgoingCommandBuffer() : ResultSuccess;
    }
private:
    static std::optional<size_t> GetPlayerDisplayIndex(u64 name) {
        if ((name & 0xffffffffffffULL) != 0x726579616c70ULL || (name >> 56) != 0) {
            return std::nullopt;
        }
        const u32 player = (name >> 48) & 0xff;
        if (player < '1' || player > '3') return std::nullopt;
        return player - '1';
    }
    Result OpenDefaultDisplay(u64 name, u32 flags) {
        if (const auto player = GetPlayerDisplayIndex(name)) {
            R_UNLESS(flags < m_notifications.size() * 32, Kernel::ResultInvalidCombination);
            R_UNLESS(m_player_display_references[*player] < 1024, Kernel::ResultOutOfResource);
            // GameShare's detached player displays retain their own lifetime;
            // they do not target the host's default display compositor.
            m_player_display_notifications[*player] = flags;
            ++m_player_display_references[*player];
            R_SUCCEED();
        }
        // The native display token is an eight-byte name; command 101 returns
        // only a result. Retain the real VI display ID for later operations.
        R_UNLESS(name == 0x00746c7561666564ULL, Kernel::ResultNotFound);
        R_UNLESS(flags == 1, Kernel::ResultNotImplemented);
        R_UNLESS(m_display_references < 1024, Kernel::ResultOutOfResource);
        if (!m_display_service) {
            const auto root = system.ServiceManager().GetService<VI::IManagerRootService>("vi:m", true);
            R_UNLESS(root, Kernel::ResultInvalidState);
            R_TRY(root->GetDisplayService(&m_display_service, VI::Policy::Compositor));
        }
        if (!m_default_display) {
            u64 id{};
            R_TRY(m_display_service->OpenDefaultDisplay(&id));
            m_default_display = id;
        }
        m_display_notification = flags;
        ++m_display_references;
        R_SUCCEED();
    }
    void OpenDisplay(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        const u64 name = u64{packet[2]} | (u64{packet[3]} << 32);
        const auto result = packet[0] == 0x75 && packet[1] == 3
            ? OpenDefaultDisplay(name, packet[4]) : Kernel::ResultInvalidCombination;
        LOG_INFO(Service_AM, "VI2 open display name={:#x}, flags={:#x}, references={}, result={:#x}",
                  name, packet[4], m_display_references, result.raw);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }
    void CloseDisplay(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        Result result = Kernel::ResultInvalidCombination;
        const u64 name = u64{packet[2]} | (u64{packet[3]} << 32);
        if (packet[0] == 0x76 && packet[1] == 2 && GetPlayerDisplayIndex(name)) {
            const auto player = *GetPlayerDisplayIndex(name);
            result = Kernel::ResultNotFound;
            if (m_player_display_references[player]) {
                if (--m_player_display_references[player] == 0) {
                    const auto notification = m_player_display_notifications[player];
                    m_notifications[notification / 32] |= 1U << (notification % 32);
                    SignalNotification();
                }
                result = ResultSuccess;
            }
        } else if (packet[0] == 0x76 && packet[1] == 2 && name == 0x00746c7561666564ULL) {
            result = Kernel::ResultNotFound;
            if (m_default_display && m_display_references) {
                result = ResultSuccess;
                if (m_display_references == 1) {
                    result = m_display_service->CloseDisplay(*m_default_display);
                    if (result.IsSuccess()) { m_default_display.reset(); }
                }
                if (result.IsSuccess()) {
                    --m_display_references;
                    if (!m_display_references) {
                        m_notifications[m_display_notification / 32] |= 1U << (m_display_notification % 32);
                        SignalNotification();
                        LOG_INFO(Service_AM, "VI2 display retired notification={}", m_display_notification);
                    }
                }
            }
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }
    void RegisterClientState(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        const u64 token = u64{packet[2]} | (u64{packet[3]} << 32);
        const u32 first = packet[4];
        const u32 second = packet[5];
        Result result = Kernel::ResultInvalidCombination;
        if (packet[0] == 0xd9 && packet[1] == 4 && token != 0 &&
            first < 32 && second < 32 && first != second) {
            const u32 slots = (1U << first) | (1U << second);
            const auto existing = std::ranges::find(m_client_states, token, &ClientState::token);
            const auto free = std::ranges::find(m_client_states, u64{0}, &ClientState::token);
            result = Kernel::ResultInvalidState;
            if (existing == m_client_states.end() && !(m_client_slots & slots)) {
                result = Kernel::ResultOutOfResource;
                if (free != m_client_states.end()) {
                    // The SDK sends its object address as an opaque identity, not a
                    // service-accessible buffer. Keep the associated slots session-local.
                    *free = {token, slots};
                    free->retire_notification = first;
                    free->available_notification = second;
                    m_client_slots |= slots;
                    result = ResultSuccess;
                }
            }
        }
        LOG_INFO(Service_AM, "VI2 register client state token={:#x}, slots={}/{}, result={:#x}",
                  token, first, second, result.raw);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }
    void UnregisterClientState(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        const u64 token = u64{packet[2]} | (u64{packet[3]} << 32);
        Result result = Kernel::ResultInvalidCombination;
        if (packet[0] == 0xda && packet[1] == 2 && token != 0) {
            const auto state = std::ranges::find(m_client_states, token, &ClientState::token);
            result = Kernel::ResultNotFound;
            if (state != m_client_states.end()) {
                if (m_default_endpoint == token) { m_default_endpoint.reset(); }
                if (state->layer) m_display_service->GetContainer()->DestroyManagedLayer(*state->layer);
                m_notifications[state->retire_notification / 32] |= 1U << (state->retire_notification % 32);
                SignalNotification();
                LOG_INFO(Service_AM, "VI2 client retired token={:#x}, notification={}", token, state->retire_notification);
                m_client_slots &= ~state->slots;
                *state = {};
                result = ResultSuccess;
            }
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }
    void GetClientAvailability(HLERequestContext& ctx) {
        RefreshReleasedBuffers();
        const auto* packet = ctx.CommandBuffer();
        const u64 token = u64{packet[2]} | (u64{packet[3]} << 32);
        Result result = Kernel::ResultInvalidCombination;
        u64 id{};
        if (packet[0] == 0xe4 && packet[1] == 2 && token) {
            auto state = std::ranges::find(m_client_states, token, &ClientState::token);
            result = Kernel::ResultNotFound;
            if (state != m_client_states.end() && m_default_display) {
                result = ResultSuccess;
                if (state->player_display) {
                    id = 0;
                } else if (!state->layer) {
                    result = m_display_service->GetContainer()->CreateManagedLayer(&id, *m_default_display, m_aruid);
                    if (result.IsSuccess()) state->layer = id;
                } else {
                    id = *state->layer;
                }
            }
        }
        const auto state = std::ranges::find(m_client_states, token, &ClientState::token);
        const u64 available = result.IsSuccess() ? state->available : 0;
        LOG_INFO(Service_AM, "VI2 client availability token={:#x}, layer={}, mask={:#x}, result={:#x}", token, id, available, result.raw);
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(result.raw);
        rb.Push(available);
    }
    void CancelClientBuffer(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        const u64 token = u64{packet[2]} | (u64{packet[3]} << 32);
        const u32 slot = GraphicsSupervisorProtocol::DecodeBufferSlot(packet[4]);
        Result result = Kernel::ResultInvalidCombination;
        u64 available{};
        if (packet[0] == 0xf4 && packet[1] == 19 && slot < 15) {
            auto state = std::ranges::find(m_client_states, token, &ClientState::token);
            result = Kernel::ResultNotFound;
            if (token && state != m_client_states.end()) {
                // Unlike present (229), cancel has no image or input GPU fence.
                state->available |= u64{1} << slot;
                available = state->available;
                result = ResultSuccess;
            }
        }
        LOG_INFO(Service_AM, "VI2 cancel buffer token={:#x}, slot={}, mask={:#x}, result={:#x}", token, slot, available, result.raw);
        IPC::ResponseBuilder rb{ctx, 8};
        rb.Push(result.raw);
        rb.Push(available);
        for (u32 i = 0; i < 4; ++i) rb.Push<u32>(0);
    }
    void RefreshReleasedBuffers() {
        for (auto& state : m_client_states) {
            if (!state.producer || !m_images->module) continue;
            bool changed{};
            for (size_t slot = 0; slot < state.queued.size(); ++slot) {
                if (!state.queued[slot]) continue;
                const auto fence = state.producer->GetReleasedBufferFence(*state.queued[slot]);
                if (!fence) continue;
                bool complete = true;
                for (u32 i = 0; i < fence->num_fences; ++i) {
                    const auto& point = fence->fences[i];
                    if (static_cast<u32>(point.id) != ~u32{0}) {
                        const auto counter = m_images->module->GetContainer().GetSyncpointManager().ReadAllocatedSyncpoint(point.id);
                        if (!counter || static_cast<s32>(*counter - point.value) < 0) {
                            complete = false;
                        }
                    }
                }
                if (!complete) continue;
                state.queued[slot].reset();
                state.available |= u64{1} << slot;
                LOG_INFO(Service_AM, "Native display buffer retired: slot={}, mask={:#x}", slot, state.available);
                changed = true;
            }
            if (changed) {
                m_notifications[state.available_notification / 32] |= 1U << (state.available_notification % 32);
                SignalNotification();
            }
        }
    }
    Result QueueNativeImage(ClientState& state, const u32* packet) {
        const u32 slot = GraphicsSupervisorProtocol::DecodeBufferSlot(packet[4]);
        R_UNLESS(!state.player_display, Kernel::ResultNotImplemented);
        R_UNLESS(state.layer && state.display && slot < 15 && !state.queued[slot], Kernel::ResultInvalidState);
        const std::scoped_lock images_lock{m_images->mutex};
        const auto image = std::ranges::find(m_images->images, packet[5], &NativeImage::id);
        R_UNLESS(image != m_images->images.end() && image->window_pool && m_images->module, Kernel::ResultNotFound);
        // The observed presentation is a four-byte color image with the
        // backend's 16-GOB block height and no byte offset within its pool.
        R_UNLESS(packet[6] == 0 && packet[7] == image->preference[2] && packet[8] == 3 &&
                 image->preference[4] == 7 && state.output_encoding == 0x14,
                 Kernel::ResultNotImplemented);
        const u32 fence_id = ~packet[22];
        R_UNLESS(packet[23] == 0 && packet[25] == 0 &&
                 (fence_id == ~u32{0} || m_images->module->GetContainer().GetSyncpointManager().IsSyncpointAllocated(fence_id)),
                 Kernel::ResultInvalidCombination);
        if (!state.producer) {
            R_TRY(m_display_service->GetContainer()->GetLayerProducerHandle(&state.producer, *state.layer));
            android::QueueBufferOutput output{};
            R_UNLESS(state.producer->Connect({}, android::NativeWindowApi::Egl, false, &output) == android::Status::NoError,
                     Kernel::ResultInvalidState);
            R_UNLESS(state.producer->SetBufferCount(16) == android::Status::NoError, Kernel::ResultInvalidState);
            s32 binder{};
            R_TRY(m_display_service->GetContainer()->OpenLayer(&binder, *state.layer, m_aruid));
            R_TRY(m_display_service->GetContainer()->SetLayerVisibility(*state.layer, true));
        }
        auto native = std::make_shared<android::NvGraphicBuffer>();
        native->width = static_cast<s32>((image->preference[2] & 0xffffU) + 1);
        native->height = static_cast<s32>((image->preference[2] >> 16) + 1);
        native->stride = static_cast<s32>(image->preference[5] * 16);
        native->format = android::PixelFormat::Rgba8888;
        native->external_format = native->format;
        native->buffer_id = image->memory_id;
        auto buffer = std::make_shared<android::GraphicBuffer>(m_images->module->GetContainer().GetNvMapFile(), native);
        s32 queue_slot{};
        R_UNLESS(state.producer->AttachBuffer(&queue_slot, buffer) == android::Status::NoError, Kernel::ResultOutOfResource);
        android::QueueBufferInput input{};
        android::QueueBufferOutput output{};
        input.crop = {0, 0, native->width, native->height};
        input.swap_interval = 1;
        input.fence = android::Fence::NoFence();
        if (fence_id != ~u32{0}) {
            input.fence.num_fences = 1;
            input.fence.fences[0] = {static_cast<s32>(fence_id), packet[24]};
        }
        if (state.producer->QueueBuffer(queue_slot, input, &output) != android::Status::NoError) {
            state.producer->CancelBuffer(queue_slot, input.fence);
            return Kernel::ResultInvalidState;
        }
        state.queued[slot] = queue_slot;
        state.available &= ~(u64{1} << slot);
        if (!m_release_poll) {
            m_release_poll = Core::Timing::CreateEvent("NativeViBufferRelease", [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                const auto guard = LockService();
                RefreshReleasedBuffers();
                return std::nullopt;
            });
            system.CoreTiming().ScheduleLoopingEvent(std::chrono::milliseconds{1}, std::chrono::milliseconds{1}, m_release_poll);
        }
        Common::NS2Watch::g_present_count.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO(Service_AM, "Native image queued for display: image={}, slot={}, fence={}/{}, dimensions={}x{}",
                 image->id, slot, fence_id, packet[24], native->width, native->height);
        R_SUCCEED();
    }
    void PresentClientBuffer(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        const u64 token = u64{packet[2]} | (u64{packet[3]} << 32);
        RefreshReleasedBuffers();
        Result result = Kernel::ResultInvalidCombination;
        u64 available{};
        if (packet[0] == 0xf5 && packet[1] == 24) {
            const auto state = std::ranges::find(m_client_states, token, &ClientState::token);
            result = Kernel::ResultNotFound;
            if (token && state != m_client_states.end()) {
                result = QueueNativeImage(*state, packet);
                available = state->available;
            }
        }
        LOG_INFO(Service_AM, "VI2 present token={:#x}, image={}, slot={}, mask={:#x}, result={:#x}", token, packet[5], packet[4], available, result.raw);
        IPC::ResponseBuilder rb{ctx, 8};
        rb.Push(result.raw);
        rb.Push(available);
        for (u32 i = 0; i < 4; ++i) rb.Push<u32>(0);
    }
    void SetClientOutputEncoding(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        const u64 token = u64{packet[2]} | (u64{packet[3]} << 32);
        const u64 encoding = u64{packet[4]} | (u64{packet[5]} << 32);
        Result result = Kernel::ResultInvalidCombination;
        if (packet[0] == 0x16f && packet[1] == 4) {
            const auto state = std::ranges::find(m_client_states, token, &ClientState::token);
            result = Kernel::ResultNotFound;
            if (token && state != m_client_states.end()) {
                result = Kernel::ResultNotImplemented;
                // Only the SDK's observed default output encoding is supported.
                if (encoding == 0x14) {
                    state->output_encoding = encoding;
                    result = ResultSuccess;
                }
            }
        }
        LOG_INFO(Service_AM, "VI2 output encoding token={:#x}, encoding={:#x}, result={:#x}", token, encoding, result.raw);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }
    void BindClientDisplay(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        const u64 token = u64{packet[2]} | (u64{packet[3]} << 32);
        const u64 name = u64{packet[4]} | (u64{packet[5]} << 32);
        Result result = Kernel::ResultInvalidCombination;
        if (packet[0] == 0x10b && packet[1] == 4) {
            const auto state = std::ranges::find(m_client_states, token, &ClientState::token);
            result = Kernel::ResultNotFound;
            if (token && state != m_client_states.end() && GetPlayerDisplayIndex(name)) {
                const auto player = *GetPlayerDisplayIndex(name);
                result = Kernel::ResultInvalidState;
                if (m_player_display_references[player] && !state->display) {
                    result = ResultSuccess;
                    if (state->layer) {
                        result = m_display_service->GetContainer()->DestroyManagedLayer(*state->layer);
                    }
                    if (result.IsSuccess()) {
                        state->layer.reset();
                        state->display = name;
                        state->player_display = player;
                    }
                }
            } else if (token && state != m_client_states.end() && name == 0x00746c7561666564ULL &&
                m_default_display && m_display_references) {
                result = Kernel::ResultInvalidState;
                if (!state->display) {
                    state->display = m_default_display;
                    result = ResultSuccess;
                }
            }
        }
        LOG_INFO(Service_AM, "VI2 bind client token={:#x}, display={:#x}, result={:#x}",
                  token, name, result.raw);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }
    void UnbindClientDisplay(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        const u64 token = u64{packet[2]} | (u64{packet[3]} << 32);
        Result result = Kernel::ResultInvalidCombination;
        if (packet[0] == 0x10c && packet[1] == 2) {
            const auto state = std::ranges::find(m_client_states, token, &ClientState::token);
            result = Kernel::ResultNotFound;
            if (token && state != m_client_states.end() && state->display) {
                state->display.reset();
                state->player_display.reset();
                result = ResultSuccess;
            }
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }
    void OpenEndpoint(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        const u64 name = u64{packet[2]} | (u64{packet[3]} << 32);
        const u64 display_name = u64{packet[4]} | (u64{packet[5]} << 32);
        const u64 token = u64{packet[6]} | (u64{packet[7]} << 32);
        Result result = Kernel::ResultInvalidCombination;
        if (packet[0] == 0x7f && packet[1] == 6) {
            const auto state = std::ranges::find(m_client_states, token, &ClientState::token);
            result = Kernel::ResultNotFound;
            if (token && state != m_client_states.end() && state->display &&
                m_default_display == state->display && name == 0x00746c7561666564ULL &&
                display_name == name) {
                result = Kernel::ResultInvalidState;
                if (!m_default_endpoint) {
                    m_default_endpoint = token;
                    result = ResultSuccess;
                }
            }
        }
        LOG_INFO(Service_AM, "VI2 open endpoint name={:#x}, display={:#x}, client={:#x}, result={:#x}",
                  name, display_name, token, result.raw);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }
    void CloseEndpoint(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        const u64 name = u64{packet[2]} | (u64{packet[3]} << 32);
        const u64 display_name = u64{packet[4]} | (u64{packet[5]} << 32);
        Result result = Kernel::ResultInvalidCombination;
        if (packet[0] == 0x80 && packet[1] == 4 && name == 0x00746c7561666564ULL &&
            display_name == name) {
            result = Kernel::ResultNotFound;
            if (m_default_endpoint) {
                m_default_endpoint.reset();
                result = ResultSuccess;
            }
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }
    void ConfigureRequestGroups(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        Result result = Kernel::ResultInvalidCombination;
        if (packet[0] == 0x43 && packet[1] == 1 && packet[2] < 8) {
            m_request_group_limit = packet[2];
            m_request_groups_initialized = true;
            result = ResultSuccess;
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }
    void GetSessionLimit(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        const Result result = packet[0] == 16 + ctx.GetCommand() && packet[1] == 0
                                  ? ResultSuccess : Kernel::ResultInvalidCombination;
        // The SDK converts this inclusive upper group index into 32 client slots.
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(result.raw);
        rb.Push<u32>(ctx.GetCommand() == 5 ? 7 : 0);
    }

    void GetNotifications(HLERequestContext& ctx) {
        Result result = Kernel::ResultInvalidSize;
        if (ctx.GetWriteBufferSize() >= sizeof(m_notifications)) {
            ctx.WriteBuffer(m_notifications);
            LOG_INFO(Service_AM, "VI2 notifications={:08x}", fmt::join(m_notifications, ","));
            m_notifications.fill(0);
            if (m_event) m_event->Clear(kernel);
            result = ResultSuccess;
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }
    void GetRequestEvent(HLERequestContext& ctx) {
        // The SDK allocates request slots from a bitmap and fetches one event per slot,
        // which it later waits on. Request completion is not tracked yet, so the events
        // are signaled whenever the session raises a notification.
        const auto* packet = ctx.CommandBuffer();
        const bool valid = packet[0] == 0x2f && packet[1] == 1;
        ReplyRequestEvent(ctx, valid, packet[2]);
    }
    void GetRequestEventWithHandle(HLERequestContext& ctx) {
        // Same reply as command 31, but the client also passes one of its own handles
        // (copy handle) ahead of the slot index. The client handle is not used yet.
        const auto* packet = ctx.CommandBuffer();
        u32 offset = 2;
        if (packet[1] & 0x80000000) {
            const u32 special = packet[offset++];
            offset += ((special & 1) ? 2 : 0) + ((special >> 1) & 0xf) + ((special >> 5) & 0xf);
        }
        const bool valid = packet[0] == 0x30 && (packet[1] & 0x3ff) == 1;
        ReplyRequestEvent(ctx, valid, packet[offset]);
    }
    void ReplyRequestEvent(HLERequestContext& ctx, bool valid, u32 index) {
        Result result = Kernel::ResultInvalidCombination;
        Kernel::KReadableEvent* readable{};
        if (valid && index < m_request_events.size()) {
            auto& event = m_request_events[index];
            if (!event) {
                event = m_context.CreateEvent(fmt::format("Vi2Request{}", index));
                if (event) event->Signal(kernel);
            }
            result = event ? ResultSuccess : Kernel::ResultOutOfResource;
            if (event) {
                readable = &event->GetReadableEvent();
                readable->Open(kernel);
            }
        }
        LOG_INFO(Service_AM, "VI2 request event cmd={} index={} result={:#x}", ctx.GetCommand(), index, result.raw);
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(result.raw);
        ctx.AddMoveObject(readable);
    }
    void StubSuccess(HLERequestContext& ctx) {
        // Commands the SDK only checks for a result code. Their effects are not emulated.
        const auto* packet = ctx.CommandBuffer();
        const u32 words = packet[1] & 0x3ff;
        LOG_WARNING(Service_AM, "(STUBBED) VI2 command {} called, args={:08x}", ctx.GetCommand(),
                    fmt::join(std::span{packet + 2, std::min<u32>(words, 8)}, ","));
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
    void StubZeroResult104(HLERequestContext& ctx) {
        // Returns a u64 and a u16; both reported as zero until the meaning is known.
        LOG_WARNING(Service_AM, "(STUBBED) VI2 command 104 called, display={:#x}", ctx.CommandBuffer()[2]);
        IPC::ResponseBuilder rb{ctx, 5};
        rb.Push(ResultSuccess);
        rb.Push<u64>(0);
        rb.Push<u32>(0);
    }
    void StubZeroWord(HLERequestContext& ctx) {
        // Commands whose reply is a single small value; reported as zero.
        LOG_WARNING(Service_AM, "(STUBBED) VI2 command {} called", ctx.GetCommand());
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(0);
    }
    void GetUnknownEvent305(HLERequestContext& ctx) {
        // Replies with a move handle like command 31. Signaled together with notifications.
        LOG_WARNING(Service_AM, "(STUBBED) VI2 command 305 called");
        if (!m_event_305) {
            m_event_305 = m_context.CreateEvent("Vi2Event305");
            if (m_event_305) m_event_305->Signal(kernel);
        }
        Kernel::KReadableEvent* readable{};
        if (m_event_305) {
            readable = &m_event_305->GetReadableEvent();
            readable->Open(kernel);
        }
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(m_event_305 ? ResultSuccess : Kernel::ResultOutOfResource);
        ctx.AddMoveObject(readable);
    }
    void SignalNotification() {
        if (m_event_305) m_event_305->Signal(kernel);
        if (m_event) m_event->Signal(kernel);
        for (auto* event : m_request_events) {
            if (event) event->Signal(kernel);
        }
    }
    void ReleaseNotificationEvent(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        const auto result = packet[0] == 0x1d && packet[1] == 1 && packet[2] == 0
            ? ResultSuccess : Kernel::ResultInvalidCombination;
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }
    void GetNotificationEvent(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        Result result = Kernel::ResultInvalidCombination;
        Kernel::KReadableEvent* readable{};
        if (packet[0] == 0x1b && packet[1] == 1 && packet[2] == 0) {
            if (!m_event) {
                m_event = m_context.CreateEvent("Vi2Notification");
            }
            result = m_event ? ResultSuccess : Kernel::ResultOutOfResource;
            if (m_event) {
                if (std::ranges::any_of(m_notifications, [](u32 value) { return value != 0; })) m_event->Signal(kernel);
                readable = &m_event->GetReadableEvent();
                readable->Open(kernel);
            }
        }
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(result.raw);
        ctx.AddMoveObject(readable);
    }

    std::shared_ptr<NativeImageRegistry> m_images;
    std::shared_ptr<Core::Timing::EventType> m_release_poll;
    std::array<ClientState, 16> m_client_states{};
    u32 m_client_slots{};
    std::shared_ptr<VI::IApplicationDisplayService> m_display_service;
    std::optional<u64> m_default_display;
    u32 m_display_references{};
    u32 m_display_notification{};
    std::array<u32, 3> m_player_display_references{};
    std::array<u32, 3> m_player_display_notifications{};
    bool m_request_groups_initialized{};
    u32 m_request_group_limit{};
    std::optional<u64> m_default_endpoint;
    std::array<u32, 32> m_notifications{};
    const u64 m_aruid;
    KernelHelpers::ServiceContext m_context;
    Kernel::KEvent* m_event{};
    std::array<Kernel::KEvent*, 64> m_request_events{};
    Kernel::KEvent* m_event_305{};
};

} // namespace


IGraphicsSupervisor::IGraphicsSupervisor(Core::System& system_, Kernel::KProcess* process, u64 aruid)
    : ServiceFramework{system_, "IGraphicsSupervisor"}, m_context{system_, "GraphicsSupervisor"},
      m_syncpoint_cache_event{m_context}, m_process{process}, m_aruid{aruid} {
    m_process->Open(kernel);
    m_image_registry = std::make_shared<NativeImageRegistry>(kernel, m_process);
    static const FunctionInfo functions[] = {
        {4, &IGraphicsSupervisor::Initialize, "Initialize"},
        {11, &IGraphicsSupervisor::BindProcess, "BindProcess"},
        {101, &IGraphicsSupervisor::CompleteBootstrap, "CompleteBootstrap"},
        {115, &IGraphicsSupervisor::ConfigureNativeMemory, "ConfigureNativeMemory"},
        {111, &IGraphicsSupervisor::CreateNativeMemory, "CreateNativeMemory"},
        {112, &IGraphicsSupervisor::DestroyNativeMemory, "DestroyNativeMemory"},
        {114, &IGraphicsSupervisor::BindNativeMemory, "BindNativeMemory"},
        {201, &IGraphicsSupervisor::InitializeSyncpointContext, "InitializeSyncpointContext"},
        {202, &IGraphicsSupervisor::GetSyncpointSession, "GetSyncpointSession"},
        {204, &IGraphicsSupervisor::BindSyncpointMemory, "BindSyncpointMemory"},
        {301, &IGraphicsSupervisor::InitializeDisplayContext, "InitializeDisplayContext"},
        {305, &IGraphicsSupervisor::OpenDisplaySession, "OpenDisplaySession"},
        {314, &IGraphicsSupervisor::ConfigureNativeWindowImagePool, "DestroyNativeWindowAliasImagePool"},
        {315, &IGraphicsSupervisor::ConfigureNativeWindowImagePool, "CreateNativeWindowAliasImagePool"},
        {401, &IGraphicsSupervisor::InitializeGpuContext, "InitializeGpuContext"},
        {402, &IGraphicsSupervisor::InitializeGpuContext, "ActivateGpuContext"},
        {423, &IGraphicsSupervisor::BindNativeGpuMemory, "BindNativeGpuMemory"},
        {429, &IGraphicsSupervisor::CreateNativeImage, "CreateNativeImage"},
        {430, &IGraphicsSupervisor::DestroyNativeImage, "DestroyNativeImage"},
        {911, &IGraphicsSupervisor::CreateNativeImage, "InitializeDecoderImagePool"},
        {912, &IGraphicsSupervisor::DestroyNativeImage, "FinalizeDecoderImagePool"},
        {961, &IGraphicsSupervisor::GetCpuUserImageDescriptor, "GetCpuUserImageDescriptor"},
        {434, &IGraphicsSupervisor::RegisterGpuAddressSpace, "RegisterGpuAddressSpace"},
        {497, &IGraphicsSupervisor::OpenGpuSession, "OpenGpuSession"},
        {901, &IGraphicsSupervisor::InitializeEmptyContext, "InitializeContext901"},
        {902, &IGraphicsSupervisor::OpenContextNotifier, "OpenContextNotifier902"},
        {903, &IGraphicsSupervisor::InitializeEmptyContext, "FinalizeContext903"},
        {1001, &IGraphicsSupervisor::InitializeEmptyContext, "InitializeContext1001"},
        {1002, &IGraphicsSupervisor::OpenContextNotifier, "OpenContextNotifier1002"},
        {1003, &IGraphicsSupervisor::InitializeEmptyContext, "FinalizeContext1003"},
        {1101, &IGraphicsSupervisor::InitializeFrameContext, "InitializeFrameContext"},
        {1102, &IGraphicsSupervisor::InitializeFrameContext, "ActivateFrameContext"},
        {1402, &IGraphicsSupervisor::GetNvidiaDriver, "GetNvidiaDriver"},
        {1501, &IGraphicsSupervisor::InitializeImageContext, "InitializeImageContext1501"},
        {435, &IGraphicsSupervisor::CalculateImageRequirements, "CalculateNativeImageSize"},
        {1510, &IGraphicsSupervisor::CalculateImageRequirements, "CalculateImageRequirements"},
        {991, &IGraphicsSupervisor::CalculateImageRequirements, "CalculateImageRequirements991"},
        {1502, &IGraphicsSupervisor::InitializeImageContext, "InitializeImageContext1502"},
    };
    RegisterHandlersTipc(functions);
}

IGraphicsSupervisor::~IGraphicsSupervisor() {
    if (m_cache_poll) {
        system.CoreTiming().UnscheduleEvent(m_cache_poll);
    }
    if (m_syncpoint_rw) {
        m_syncpoint_rw->Close(kernel);
        m_syncpoint_ro->Close(kernel);
    }
    if (m_nv_module) {
        auto& nvmap = m_nv_module->GetContainer().GetNvMapFile();
        for (const auto id : m_native_memories) {
            if (const auto info = nvmap.FreeHandle(id, false); info && info->can_unlock && info->address != 0) {
                m_process->GetPageTable().UnlockForDeviceAddressSpace(info->address, info->size);
            }
        }
        if (m_native_memory_session) {
            m_nv_module->GetContainer().CloseSession(Nvidia::NvCore::SessionId{*m_native_memory_session});
        }
    }
    m_context.CloseEvent(m_syncpoint_request_event);
    m_process->Close(kernel);
}

Result IGraphicsSupervisor::HandleSyncRequest(Kernel::KServerSession& session,
                                               HLERequestContext& ctx) {
    if (!ctx.IsTipc() || handlers_tipc.contains(ctx.GetCommand())) {
        return ServiceFrameworkBase::HandleSyncRequest(session, ctx);
    }
    const auto guard = LockService();
    InvokeRequestTipc(ctx);
    // Unsupported TIPC must return an error instead of leaving request data as a reply.
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(Kernel::ResultNotImplemented.raw);
    return system.IsPoweredOn() ? ctx.WriteToOutgoingCommandBuffer() : ResultSuccess;
}

void IGraphicsSupervisor::Initialize(HLERequestContext& ctx) {
    const auto request = GraphicsSupervisorProtocol::DecodeBootstrap(
        std::span<const u32>{ctx.CommandBuffer(), IPC::COMMAND_BUFFER_LENGTH});
    Result result = Kernel::ResultInvalidCombination;
    if (request) {
        auto process = ctx.GetObjectFromHandle<Kernel::KProcess>(request->process_handle);
        const bool empty_remote_memory = std::ranges::all_of(
            request->parameters, [](u64 value) { return value == 0; });
        if (process.IsNull() || process.GetPointerUnsafe() != m_process) {
            result = Kernel::ResultInvalidHandle;
        } else if (m_phase != Phase::Created) {
            result = Kernel::ResultInvalidState;
        } else if (request->applet_resource_user_id != m_aruid) {
            result = Kernel::ResultInvalidArgument;
        } else if (request->flags != 0x8000000000000005ULL || !empty_remote_memory) {
            result = Kernel::ResultNotImplemented;
        } else {
            m_phase = Phase::Initialized;
            result = ResultSuccess;
        }
        LOG_INFO(Service_AM, "graphics bootstrap aruid={:#x}, flags={:#x}, result={:#x}",
                  request->applet_resource_user_id, request->flags, result.raw);
    }
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result.raw);
}

void IGraphicsSupervisor::BindProcess(HLERequestContext& ctx) {
    Result result = Kernel::ResultInvalidCombination;
    const auto* packet = ctx.CommandBuffer();
    if (packet[0] == 0x1b && packet[1] == 0x80000000 && packet[2] == 2) {
        auto process = ctx.GetObjectFromHandle<Kernel::KProcess>(ctx.GetCopyHandle(0));
        if (process.IsNull() || process.GetPointerUnsafe() != m_process) {
            result = Kernel::ResultInvalidHandle;
        } else if (m_phase != Phase::Initialized) {
            result = Kernel::ResultInvalidState;
        } else {
            m_phase = Phase::ProcessBound;
            result = ResultSuccess;
        }
    }
    LOG_INFO(Service_AM, "graphics process binding result={:#x}", result.raw);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result.raw);
}

void IGraphicsSupervisor::CompleteBootstrap(HLERequestContext& ctx) {
    Result result = Kernel::ResultInvalidCombination;
    const auto* packet = ctx.CommandBuffer();
    if (packet[0] == 0x75 && packet[1] == 0) {
        if (m_phase != Phase::ProcessBound) {
            result = Kernel::ResultInvalidState;
        } else {
            m_phase = Phase::Ready;
            result = ResultSuccess;
        }
    }
    LOG_INFO(Service_AM, "graphics bootstrap completion result={:#x}", result.raw);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result.raw);
}

void IGraphicsSupervisor::InitializeImageContext(HLERequestContext& ctx) {
    Result result = Kernel::ResultInvalidCombination;
    const auto* packet = ctx.CommandBuffer();
    if (packet[0] == 16 + ctx.GetCommand() && packet[1] == 0) {
        if (m_phase != Phase::Ready) {
            result = Kernel::ResultInvalidState;
        } else if (ctx.GetCommand() == 1501) {
            m_image_context_initialized = true;
            result = ResultSuccess;
        } else if (m_image_context_initialized) {
            m_image_formats_initialized = true;
            result = ResultSuccess;
        } else {
            result = Kernel::ResultInvalidState;
        }
    }
    LOG_INFO(Service_AM, "empty image context command={}, result={:#x}",
              ctx.GetCommand(), result.raw);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result.raw);
}

void IGraphicsSupervisor::OpenGpuSession(HLERequestContext& ctx) {
    const auto* packet = ctx.CommandBuffer();
    Result result = Kernel::ResultInvalidCombination;
    Kernel::KClientSession* client{};
    const u64 flags = u64{packet[2]} | (u64{packet[3]} << 32);
    if (packet[0] == 0x201 && packet[1] == 2 && flags == 4) {
        if (m_phase != Phase::Ready || !m_nv_module) {
            result = Kernel::ResultInvalidState;
        } else {
            auto handler = std::make_shared<IGpuSession>(system, m_nv_module, m_process);
            result = CreateGraphicsSession(system, ctx, handler, &client);
            if (result.IsSuccess()) {
                std::erase_if(m_gpu_sessions, [](const auto& item) { return item.expired(); });
                m_gpu_sessions.emplace_back(handler);
            }
        }
    }
    LOG_INFO(Service_AM, "GPU control session flags={:#x}, result={:#x}", flags, result.raw);
    // This SDK reads the move-handle slot before the result, including on failure.
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(result.raw);
    ctx.AddMoveObject(client);
}

void IGraphicsSupervisor::RegisterGpuAddressSpace(HLERequestContext& ctx) {
    const auto* packet = ctx.CommandBuffer();
    Result result = Kernel::ResultInvalidCombination;
    if (packet[0] == 0x1c2 && packet[1] == 1) {
        result = m_gpu_context_ready ? Kernel::ResultNotFound : Kernel::ResultInvalidState;
        if (m_gpu_context_ready) {
            for (const auto& item : m_gpu_sessions) {
                if (auto handler = item.lock()) {
                    result = handler->RegisterAddressSpace(packet[2]);
                    if (result.IsSuccess()) {
                        break;
                    }
                }
            }
        }
    }
    LOG_INFO(Service_AM, "register GPU address space id={}, result={:#x}", packet[2], result.raw);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result.raw);
}

void IGraphicsSupervisor::InitializeDisplayContext(HLERequestContext& ctx) {
    const auto* packet = ctx.CommandBuffer();
    Result result = Kernel::ResultInvalidCombination;
    if (packet[0] == 0x13d && packet[1] == 0) {
        result = m_phase == Phase::Ready ? ResultSuccess : Kernel::ResultInvalidState;
        if (result.IsSuccess()) {
            m_display_context_initialized = true;
        }
    }
    LOG_INFO(Service_AM, "display context initialization result={:#x}", result.raw);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result.raw);
}

void IGraphicsSupervisor::OpenDisplaySession(HLERequestContext& ctx) {
    const auto* packet = ctx.CommandBuffer();
    Kernel::KClientSession* client{};
    const u64 flags = u64{packet[2]} | (u64{packet[3]} << 32);
    Result result = Kernel::ResultInvalidCombination;
    if (packet[0] == 0x141 && packet[1] == 2 && flags == 0x300020000ULL) {
        result = m_display_context_initialized ? ResultSuccess : Kernel::ResultInvalidState;
        if (result.IsSuccess()) {
            result = CreateGraphicsSession(system, ctx, std::make_shared<IVi2Session>(system, m_aruid, m_image_registry),
                                           &client);
        }
    }
    LOG_INFO(Service_AM, "VI2 session flags={:#x}, result={:#x}", flags, result.raw);
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(result.raw);
    ctx.AddMoveObject(client);
}

void IGraphicsSupervisor::InitializeGpuContext(HLERequestContext& ctx) {
    const auto* packet = ctx.CommandBuffer();
    Result result = Kernel::ResultInvalidCombination;
    if (packet[0] == 16 + ctx.GetCommand() && packet[1] == 0) {
        if (m_phase != Phase::Ready) {
            result = Kernel::ResultInvalidState;
        } else if (ctx.GetCommand() == 401) {
            m_gpu_context_initialized = true;
            result = ResultSuccess;
        } else if (m_gpu_context_initialized) {
            m_gpu_context_ready = true;
            result = ResultSuccess;
        } else {
            result = Kernel::ResultInvalidState;
        }
    }
    LOG_INFO(Service_AM, "GPU context command={}, result={:#x}", ctx.GetCommand(), result.raw);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result.raw);
}

void IVideoCodecManager::CreateService(HLERequestContext& ctx) {
    Kernel::KClientSession* client{};
    const Result result =
        CreateGraphicsSession(system, ctx, std::make_shared<IVideoCodecService>(system, m_encoder, m_images), &client);
    LOG_INFO(Service_AM, "video {} service created, result={:#x}", m_encoder ? "encoder" : "decoder", result.raw);
    if (result.IsError()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
        return;
    }
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess.raw);
    rb.PushMoveObjects(ctx, client);
}

void IGraphicsSupervisor::OpenContextNotifier(HLERequestContext& ctx) {
    // Reply carries a move handle to a session (header 0x80000001, one move handle).
    const auto* packet = ctx.CommandBuffer();
    Result result = Kernel::ResultInvalidCombination;
    Kernel::KClientSession* client{};
    if (packet[0] == 16 + ctx.GetCommand() && packet[1] == 0) {
        result = CreateGraphicsSession(system, ctx,
            std::make_shared<IVideoCodecManager>(system, ctx.GetCommand() == 1002, m_image_registry), &client);
    }
    LOG_INFO(Service_AM, "video codec manager command={}, result={:#x}", ctx.GetCommand(), result.raw);
    if (result.IsError()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
        return;
    }
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess.raw);
    rb.PushMoveObjects(ctx, client);
}

void IGraphicsSupervisor::InitializeEmptyContext(HLERequestContext& ctx) {
    // Context setup commands (x01/x02 pairs) that carry no arguments and return only a
    // result in the title's SDK. Nothing is emulated behind them yet.
    const auto* packet = ctx.CommandBuffer();
    const bool valid = packet[0] == 16 + ctx.GetCommand() && packet[1] == 0;
    const Result result = valid ? ResultSuccess : Kernel::ResultInvalidCombination;
    LOG_INFO(Service_AM, "empty context command={}, result={:#x}", ctx.GetCommand(), result.raw);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result.raw);
}

void IGraphicsSupervisor::InitializeFrameContext(HLERequestContext& ctx) {
    const auto* packet = ctx.CommandBuffer();
    Result result = Kernel::ResultInvalidCombination;
    if (packet[0] == 16 + ctx.GetCommand() && packet[1] == 0) {
        if (m_phase != Phase::Ready) {
            result = Kernel::ResultInvalidState;
        } else if (ctx.GetCommand() == 1101) {
            m_frame_context_initialized = true;
            result = ResultSuccess;
        } else if (m_frame_context_initialized) {
            m_frame_context_ready = true;
            result = ResultSuccess;
        } else {
            result = Kernel::ResultInvalidState;
        }
    }
    LOG_INFO(Service_AM, "empty frame context command={}, result={:#x}", ctx.GetCommand(), result.raw);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result.raw);
}

void IGraphicsSupervisor::ConfigureNativeMemory(HLERequestContext& ctx) {
    const auto* packet = ctx.CommandBuffer();
    Result result = Kernel::ResultInvalidCombination;
    if (packet[0] == 0x83 && packet[1] == 2 && packet[3] == 0 &&
        (packet[2] == 0 || packet[2] == 2)) {
        result = m_phase == Phase::Ready ? ResultSuccess : Kernel::ResultInvalidState;
    }
    // Clear the optional Gpuk-memory mode: Eden currently provides the NVDRV path.
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(result.raw);
    rb.Push<u64>(0);
    LOG_INFO(Service_AM, "native memory uses NVDRV-compatible mode, result={:#x}", result.raw);
}

void IGraphicsSupervisor::InitializeSyncpointContext(HLERequestContext& ctx) {
    Result result = Kernel::ResultInvalidCombination;
    const auto* packet = ctx.CommandBuffer();
    if (packet[0] == 0xd9 && packet[1] == 0) {
        if (m_phase != Phase::Ready) {
            result = Kernel::ResultInvalidState;
        } else {
            m_syncpoint_context_initialized = true;
            result = ResultSuccess;
        }
    }
    LOG_INFO(Service_AM, "syncpoint context initialization result={:#x}", result.raw);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result.raw);
}

void IGraphicsSupervisor::GetSyncpointSession(HLERequestContext& ctx) {
    const auto* packet = ctx.CommandBuffer();
    Result result = Kernel::ResultInvalidState;
    Kernel::KClientSession* client{};
    if (packet[0] == 0xda && packet[1] == 0 && m_syncpoint_context_initialized && m_nv_module) {
        result = CreateGraphicsSession(system, ctx,
            std::make_shared<ISyncpointSupervisor>(system, m_nv_module), &client);
    }
    if (result.IsError()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
        return;
    }
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess.raw);
    rb.PushMoveObjects(ctx, client);
    LOG_INFO(Service_AM, "opened syncpoint-supervisor session");
}

void IGraphicsSupervisor::GetNvidiaDriver(HLERequestContext& ctx) {
    const auto Fail = [&](Result result) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    };
    const auto* packet = ctx.CommandBuffer();
    if (packet[0] != 0x58a || packet[1] != 0) {
        Fail(Kernel::ResultInvalidCombination);
        return;
    }
    if (m_phase != Phase::Ready || !m_image_formats_initialized) {
        Fail(Kernel::ResultInvalidState);
        return;
    }
    auto source = system.ServiceManager().GetService<Nvidia::NVDRV>("nvdrv:s", true);
    if (!source) {
        Fail(Kernel::ResultNotFound);
        return;
    }
    m_nv_module = source->GetModule();
    Kernel::KClientSession* client{};
    const auto result = CreateGraphicsSession(system, ctx,
        std::make_shared<Nvidia::NVDRV>(system, m_nv_module, "nvdrv:gsv", m_process, m_aruid),
        &client);
    if (result.IsError()) {
        Fail(result);
        return;
    }
    LOG_INFO(Service_AM, "opened process-bound NVDRV session for aruid={:#x}", m_aruid);
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess.raw);
    rb.PushMoveObjects(ctx, client);
}

void IGraphicsSupervisor::BindSyncpointMemory(HLERequestContext& ctx) {
    using namespace GraphicsSyncpointCache;
    const auto Fail = [&](Result result) {
        IPC::ResponseBuilder rb{ctx, 2, 0, 2};
        rb.Push(result.raw);
        ctx.AddMoveObject(nullptr);
        ctx.AddMoveObject(nullptr);
    };
    const auto* packet = ctx.CommandBuffer();
    if (packet[0] != 0xdc || packet[1] != 0x80000006 || packet[2] != 4 ||
        packet[5] != 1 || packet[6] != 0x1ff || packet[7] != 0x1000 || packet[8] != 0 ||
        packet[9] != 0x2000 || packet[10] != 0) {
        Fail(Kernel::ResultInvalidCombination);
        return;
    }
    if (!m_syncpoint_context_initialized || !m_nv_module || m_syncpoint_rw) {
        Fail(Kernel::ResultInvalidState);
        return;
    }
    auto rw = ctx.GetObjectFromHandle<Kernel::KTransferMemory>(ctx.GetCopyHandle(0));
    auto ro = ctx.GetObjectFromHandle<Kernel::KTransferMemory>(ctx.GetCopyHandle(1));
    if (rw.IsNull() || ro.IsNull() || rw->GetOwner() != m_process || ro->GetOwner() != m_process ||
        rw.GetPointerUnsafe() == ro.GetPointerUnsafe()) {
        Fail(Kernel::ResultInvalidHandle);
        return;
    }
    if (rw->GetSize() != 0x1000 || ro->GetSize() != 0x2000) {
        Fail(Kernel::ResultInvalidSize);
        return;
    }
    auto* request_event = m_context.CreateEvent("SyncpointCacheRequest");
    if (request_event == nullptr) {
        Fail(Kernel::ResultOutOfResource);
        return;
    }
    m_syncpoint_request_event = request_event;
    m_syncpoint_rw = rw.GetPointerUnsafe();
    m_syncpoint_ro = ro.GetPointerUnsafe();
    m_syncpoint_rw->Open(kernel);
    m_syncpoint_ro->Open(kernel);
    auto& memory = m_process->GetMemory();
    const auto rw_base = rw->GetSourceAddress();
    const auto ro_base = ro->GetSourceAddress();
    const std::array<u8, 0x2000> zeros{};
    memory.WriteBlock(rw_base, zeros.data(), 0x1000);
    memory.WriteBlock(ro_base, zeros.data(), 0x2000);
    memory.WriteBlock(ro_base, Header.data(), sizeof(Header));
    for (u32 id = 0; id < CounterCount; ++id) {
        memory.Write64(ro_base + ValuesOffset + id * sizeof(u64), Unallocated);
    }
    m_cache_poll = Core::Timing::CreateEvent("GraphicsSyncpointCache",
        [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
            const auto guard = LockService();
            this->RefreshSyncpointCache();
            return std::nullopt;
        });
    system.CoreTiming().ScheduleLoopingEvent(std::chrono::milliseconds{1},
                                             std::chrono::milliseconds{1}, m_cache_poll);
    request_event->Open(kernel);
    auto* completed = m_syncpoint_cache_event.GetHandle();
    completed->Open(kernel);
    IPC::ResponseBuilder rb{ctx, 2, 0, 2};
    rb.Push(ResultSuccess.raw);
    ctx.AddMoveObject(request_event);
    ctx.AddMoveObject(completed);
    LOG_INFO(Service_AM, "bound syncpoint cache: {} real counters, rw={:#x}, ro={:#x}",
              CounterCount, GetInteger(rw_base), GetInteger(ro_base));
}

void IGraphicsSupervisor::RefreshSyncpointCache() {
    using namespace GraphicsSyncpointCache;
    if (!m_syncpoint_rw || !system.IsPoweredOn()) {
        return;
    }
    auto& memory = m_process->GetMemory();
    const auto rw = m_syncpoint_rw->GetSourceAddress();
    const auto ro = m_syncpoint_ro->GetSourceAddress();
    auto& syncpoints = m_nv_module->GetContainer().GetSyncpointManager();
    for (const u32 id : GetPublished()) {
        if (id < CounterCount) {
            memory.Write64(ro + ValuesOffset + id * sizeof(u64),
                           m_counter_values[id].Observe(syncpoints.ReadAllocatedSyncpoint(id)));
        }
    }
    const u64 requests = memory.Read64(rw + GroupRequestOffset);
    const u64 acknowledgements = memory.Read64(ro + GroupAckOffset);
    u64 groups = (requests ^ acknowledgements) & ((1ULL << GroupCount) - 1);
    if (groups == 0) {
        return;
    }
    while (groups) {
        const u32 group = static_cast<u32>(std::countr_zero(groups));
        groups &= groups - 1;
        const u64 requested_slots = memory.Read64(rw + SlotRequestOffset + group * sizeof(u64));
        u64 slots = requested_slots ^ memory.Read64(ro + SlotAckOffset + group * sizeof(u64));
        while (slots) {
            const u32 bit = static_cast<u32>(std::countr_zero(slots));
            slots &= slots - 1;
            const u32 id = group * 64 + bit;
            const auto address = ro + ValuesOffset + id * sizeof(u64);
            const auto value = syncpoints.ReadAllocatedSyncpoint(id);
            const u64 updated = m_counter_values[id].Observe(value);
            memory.Write64(address, updated);
            {
                // Which counters the game polls through the cache (logged once per counter, and
                // again every 2000 refreshes), to find waits that never complete.
                static std::array<u32, CounterCount> refreshes{};
                const u32 count = refreshes[id]++;
                if (count == 0 || count % 2000 == 0) {
                    LOG_INFO(Service_AM, "NS2 trace: syncpoint cache refresh id={} value={} (allocated={}, cached={:#x}, refreshes={})",
                             id, value.value_or(0), value.has_value(), updated, count + 1);
                }
            }
        }
        std::atomic_thread_fence(std::memory_order_release);
        memory.Write64(ro + SlotAckOffset + group * sizeof(u64), requested_slots);
    }
    std::atomic_thread_fence(std::memory_order_release);
    memory.Write64(ro + GroupAckOffset, requests);
    m_syncpoint_request_event->Clear(kernel);
    m_syncpoint_cache_event.Signal(kernel);
}

void IGraphicsSupervisor::CalculateImageRequirements(HLERequestContext& ctx) {
    const auto* packet = ctx.CommandBuffer();
    Result result = Kernel::ResultInvalidCombination;
    std::optional<NativeImageRequirements> requirements;
    // 435 returns only a size; 1510 and 991 also return the alignment. 991 carries the
    // same 64-byte descriptor (type 1 or 2) as the other two.
    const bool gpu_image = ctx.GetCommand() == 435;
    const u32 expected_header = 16 + ctx.GetCommand();
    const bool type_ok = ctx.GetCommand() == 991 ? IsDecoderImageCode(packet[3])
                                                 : packet[3] == (gpu_image ? 1U : 2U);
    if (packet[0] == expected_header && packet[1] == 17 && type_ok) {
        result = Kernel::ResultInvalidState;
        if (m_phase == Phase::Ready && m_image_formats_initialized) {
            requirements = CalculateNativeImageRequirements(std::span{packet + 2, 16}, packet[18]);
            result = requirements ? ResultSuccess : Kernel::ResultNotImplemented;
        }
    }
    LOG_INFO(Service_AM, "native image requirements cmd={}, format={}, dimensions={:#x}, block={}, pitch={}, count={}, size={:#x}, alignment={:#x}, result={:#x}",
              ctx.GetCommand(), packet[3], packet[4], packet[6], packet[7], packet[18], requirements ? requirements->size : 0,
              requirements ? requirements->alignment : 0, result.raw);
    IPC::ResponseBuilder rb{ctx, gpu_image ? 4U : 6U};
    rb.Push(result.raw);
    rb.Push<u64>(requirements ? requirements->size : 0);
    if (!gpu_image) { rb.Push<u64>(requirements ? requirements->alignment : 0); }
}
void IGraphicsSupervisor::CreateNativeMemory(HLERequestContext& ctx) {
    const auto* packet = ctx.CommandBuffer();
    const u64 size = u64{packet[3]} | (u64{packet[4]} << 32);
    Result result = Kernel::ResultInvalidCombination;
    u32 id{};
    if (packet[0] == 0x7f && packet[1] == 3 && (packet[2] & 0xff) == 3) {
        result = Kernel::ResultInvalidState;
        if (m_phase == Phase::Ready && m_image_formats_initialized && m_nv_module) {
            result = Kernel::ResultInvalidSize;
            if (size && size <= (1ULL << 32) && Common::IsAligned(size, Kernel::PageSize)) {
                result = Kernel::ResultOutOfResource;
                if (m_native_memories.size() < 65536) {
                    auto& nvmap = m_nv_module->GetContainer().GetNvMapFile();
                    std::shared_ptr<Nvidia::NvCore::NvMap::Handle> handle;
                    if (nvmap.CreateHandle(size, handle) == Nvidia::NvResult::Success) {
                        id = handle->id;
                        m_native_memories.push_back(id);
                        result = ResultSuccess;
                    }
                }
            }
        }
    }
    LOG_INFO(Service_AM, "native memory create size={:#x}, id={}, result={:#x}", size, id, result.raw);
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(result.raw);
    rb.Push(id);
}

Result IGraphicsSupervisor::BindNativeMemoryBacking(u32 id, u64 address, u64 size, u64 flags, u64 offset) {
    R_UNLESS(m_nv_module && std::ranges::find(m_native_memories, id) != m_native_memories.end(),
             Kernel::ResultNotFound);
    R_UNLESS((flags == 0 || flags == 2) && offset == 0, Kernel::ResultNotImplemented);
    auto handle = m_nv_module->GetContainer().GetNvMapFile().GetHandle(id);
    R_UNLESS(handle && !handle->allocated, Kernel::ResultInvalidState);
    R_UNLESS(size && size == handle->size && address != 0 && address + size > address,
             Kernel::ResultInvalidSize);
    R_UNLESS(Common::IsAligned(address, Kernel::PageSize), Kernel::ResultInvalidAddress);
    auto& page_table = m_process->GetPageTable();
    R_UNLESS(page_table.Contains(address, size), Kernel::ResultInvalidCurrentMemory);
    if (flags == 2) {
        for (u64 current = address; current < address + size;) {
            Kernel::KMemoryInfo info{};
            Kernel::Svc::PageInfo page{};
            R_TRY(page_table.QueryInfo(&info, &page, current));
            R_UNLESS(True(info.m_attribute & Kernel::KMemoryAttribute::GpuAccessible),
                     Kernel::ResultInvalidCurrentMemory);
            const u64 next = info.m_address + info.m_size;
            R_UNLESS(next > current, Kernel::ResultInvalidCurrentMemory);
            current = next;
        }
    }
    bool is_io{};
    R_TRY(page_table.LockForMapDeviceAddressSpace(&is_io, address, size,
                                                 Kernel::KMemoryPermission::None, true, true));
    if (is_io) {
        page_table.UnlockForDeviceAddressSpace(address, size);
        R_RETURN(Kernel::ResultNotImplemented);
    }
    if (!m_native_memory_session) {
        m_native_memory_session = m_nv_module->GetContainer().OpenSession(m_process).id;
    }
    const auto result = handle->Alloc({}, Kernel::PageSize, 0, address,
                                      Nvidia::NvCore::SessionId{*m_native_memory_session});
    if (result != Nvidia::NvResult::Success) {
        page_table.UnlockForDeviceAddressSpace(address, size);
        R_RETURN(Kernel::ResultOutOfResource);
    }
    R_SUCCEED();
}

void IGraphicsSupervisor::BindNativeMemory(HLERequestContext& ctx) {
    const auto* packet = ctx.CommandBuffer();
    const auto pair = [&](size_t i) { return u64{packet[i]} | (u64{packet[i + 1]} << 32); };
    const auto result = packet[0] == 0x82 && packet[1] == 9
        ? BindNativeMemoryBacking(packet[2], pair(3), pair(5), pair(7), pair(9))
        : Kernel::ResultInvalidCombination;
    LOG_INFO(Service_AM, "native memory bind id={}, address={:#x}, size={:#x}, flags={:#x}, result={:#x}",
              packet[2], pair(3), pair(5), pair(7), result.raw);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result.raw);
}

Result IGraphicsSupervisor::ReleaseNativeMemory(u32 id) {
    const auto it = std::ranges::find(m_native_memories, id);
    R_UNLESS(m_nv_module && it != m_native_memories.end(), Kernel::ResultNotFound);
    auto& nvmap = m_nv_module->GetContainer().GetNvMapFile();
    auto handle = nvmap.GetHandle(id);
    R_UNLESS(handle, Kernel::ResultNotFound);
    {
        const std::scoped_lock lock{handle->mutex};
        if (handle->pins != 0 || handle->internal_dupes != 0 || handle->dupes != 1) {
            // Emulator-side references (presentation, texture cache) are not always
            // balanced. Freeing the last user duplicate force-unmaps the handle, so
            // proceed instead of failing the title's teardown.
            LOG_WARNING(Service_AM, "native memory {} released while referenced: pins={}, internal={}, dupes={}",
                        id, handle->pins, handle->internal_dupes, handle->dupes);
            R_UNLESS(!VideoCommon::NativeStrict(), Kernel::ResultBusy);
        }
    }
    const bool allocated = handle->allocated;
    handle.reset();
    const auto info = nvmap.FreeHandle(id, false);
    m_native_memories.erase(it);
    if (allocated) {
        if (!info || !info->can_unlock) {
            LOG_WARNING(Service_AM, "native memory {} still in use after release; keeping it locked", id);
            R_UNLESS(!VideoCommon::NativeStrict(), Kernel::ResultInvalidState);
            R_SUCCEED();
        }
        R_TRY(m_process->GetPageTable().UnlockForDeviceAddressSpace(info->address, info->size));
    }
    R_SUCCEED();
}

void IGraphicsSupervisor::DestroyNativeMemory(HLERequestContext& ctx) {
    const auto* packet = ctx.CommandBuffer();
    const auto result = packet[0] == 0x80 && packet[1] == 1
        ? ReleaseNativeMemory(packet[2]) : Kernel::ResultInvalidCombination;
    LOG_INFO(Service_AM, "native memory destroy id={}, result={:#x}", packet[2], result.raw);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result.raw);
}

void IGraphicsSupervisor::BindNativeGpuMemory(HLERequestContext& ctx) {
    const auto* packet = ctx.CommandBuffer();
    const auto pair = [&](size_t i) { return u64{packet[i]} | (u64{packet[i + 1]} << 32); };
    Result result = Kernel::ResultInvalidCombination;
    if (packet[0] == 0x1b7 && packet[1] == 12 && pair(10) == 0 && pair(12) == 0) {
        result = Kernel::ResultNotFound;
        if (m_nv_module) {
            auto& container = m_nv_module->GetContainer();
            const auto parent = container.GetNvMapFile().GetHandle(packet[7]);
            if (parent && parent->allocated) {
                const auto* session = container.GetSession(parent->session_id);
                const u64 offset = pair(8);
                const u64 address = pair(3);
                const u64 size = pair(5);
                if (session && session->process == m_process && offset <= parent->size &&
                    size <= parent->size - offset && parent->address + offset >= parent->address &&
                    address == parent->address + offset) {
                    result = BindNativeMemoryBacking(packet[2], address, size, 2, 0);
                }
            }
        }
    }
    LOG_INFO(Service_AM, "native GPU memory bind id={}, parent={}, address={:#x}, size={:#x}, offset={:#x}, result={:#x}",
              packet[2], packet[7], pair(3), pair(5), pair(8), result.raw);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result.raw);
}

void IGraphicsSupervisor::CreateNativeImage(HLERequestContext& ctx) {
    const std::scoped_lock images_lock{m_image_registry->mutex};
    const auto* packet = ctx.CommandBuffer();
    Result result = Kernel::ResultInvalidCombination;
    u32 id{};
    // 429 = CreateNativeImage; 911 = nn::codec InitializeDecoderImagePool (same descriptor).
    if (packet[0] == 16 + ctx.GetCommand() && packet[1] == 18 &&
        (packet[3] == 1 || packet[3] == 2 || (ctx.GetCommand() == 911 && IsDecoderImageCode(packet[3])))) {
        result = Kernel::ResultInvalidState;
        if (m_phase == Phase::Ready && m_gpu_context_ready && m_image_formats_initialized && m_nv_module) {
            const auto requirements = CalculateNativeImageRequirements(std::span{packet + 2, 16}, packet[18]);
            result = Kernel::ResultNotImplemented;
            if (requirements) {
                result = Kernel::ResultNotFound;
                auto& nvmap = m_nv_module->GetContainer().GetNvMapFile();
                const auto memory = nvmap.GetHandle(packet[19]);
                if (std::ranges::find(m_native_memories, packet[19]) != m_native_memories.end() &&
                    memory && memory->allocated) {
                    result = Kernel::ResultInvalidSize;
                    if (memory->size >= requirements->size) {
                        result = Kernel::ResultOutOfResource;
                        if (m_image_registry->images.size() < 65536 && m_next_native_image_id != 0 &&
                            memory->Duplicate(true) == Nvidia::NvResult::Success) {
                            NativeImage image{m_next_native_image_id++, packet[19], {}, requirements->size};
                            const auto preference = NormalizeNativeImagePreference(std::span{packet + 2, 16});
                            std::copy_n(preference.begin(), image.preference.size(), image.preference.begin());
                            id = image.id;
                            if (!m_image_registry->module) {
                                m_image_registry->module = m_nv_module;
                                m_image_registry->memory_session = m_nv_module->GetContainer().OpenSession(m_process);
                            }
                            m_image_registry->images.push_back(image);
                            result = ResultSuccess;
                        }
                    }
                }
            }
        }
    }
    LOG_INFO(Service_AM, "native image create memory={}, count={}, id={}, result={:#x}",
              packet[19], packet[18], id, result.raw);
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(result.raw);
    rb.Push(id);
}

// 961(native image handle): the 0x80-byte nn::gsv::CpuUserImageDescriptor that nn::codec's
// ReadDecoderOutputImage* passes to nn::gs::sdk::ReadUserImage*, relative to the image memory.
// Kind 0xa is pitch-linear NV12: u32 Y offset at 8, u64 Y pitch at 0x10, u32 UV offset at 0x18,
// u64 UV pitch at 0x20.
void IGraphicsSupervisor::GetCpuUserImageDescriptor(HLERequestContext& ctx) {
    const auto* packet = ctx.CommandBuffer();
    std::array<u32, 32> descriptor{};
    Result result = Kernel::ResultInvalidCombination;
    if (packet[0] == 0x3d1 && packet[1] == 1) {
        const std::scoped_lock images_lock{m_image_registry->mutex};
        const auto image = std::ranges::find(m_image_registry->images, packet[2], &NativeImage::id);
        result = Kernel::ResultNotFound;
        if (image != m_image_registry->images.end()) {
            if (const auto layout = GetDecoderImageLayout(*image)) {
                descriptor[0] = 0xa;
                descriptor[2] = 0;
                descriptor[4] = layout->pitch;
                descriptor[6] = layout->uv_offset;
                descriptor[8] = layout->pitch;
                result = ResultSuccess;
            }
        }
    }
    static u32 logged{};
    if (logged++ < 16) {
        LOG_INFO(Service_AM, "CPU user image descriptor image={}, pitch={}, uv offset={:#x}, result={:#x}",
                 packet[2], descriptor[4], descriptor[6], result.raw);
    }
    IPC::ResponseBuilder rb{ctx, 2 + static_cast<u32>(descriptor.size())};
    rb.Push(result.raw);
    for (const u32 word : descriptor) {
        rb.Push(word);
    }
}

void IGraphicsSupervisor::DestroyNativeImage(HLERequestContext& ctx) {
    const std::scoped_lock images_lock{m_image_registry->mutex};
    const auto* packet = ctx.CommandBuffer();
    Result result = Kernel::ResultInvalidCombination;
    // 430 = DestroyNativeImage; 912 = nn::codec FinalizeDecoderImagePool.
    if (packet[0] == 16 + ctx.GetCommand() && packet[1] == 1) {
        const auto image = std::ranges::find(m_image_registry->images, packet[2], &NativeImage::id);
        result = Kernel::ResultNotFound;
        if (m_nv_module && image != m_image_registry->images.end()) {
            result = Kernel::ResultBusy;
            if (!image->window_pool) {
                m_image_registry->ReleaseMemory(image->memory_id);
                m_image_registry->images.erase(image);
                result = ResultSuccess;
            }
        }
    }
    LOG_INFO(Service_AM, "native image destroy id={}, result={:#x}", packet[2], result.raw);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result.raw);
}

void IGraphicsSupervisor::ConfigureNativeWindowImagePool(HLERequestContext& ctx) {
    const std::scoped_lock images_lock{m_image_registry->mutex};
    const auto* packet = ctx.CommandBuffer();
    const bool create = ctx.GetCommand() == 315;
    Result result = Kernel::ResultInvalidCombination;
    if (packet[0] == (create ? 0x14bU : 0x14aU) && packet[1] == (create ? 2U : 1U)) {
        const auto image = std::ranges::find(m_image_registry->images, packet[2], &NativeImage::id);
        result = Kernel::ResultNotFound;
        if (image != m_image_registry->images.end()) {
            result = Kernel::ResultNotImplemented;
            if (!create || packet[3] == 1) {
                result = Kernel::ResultInvalidState;
                if (image->window_pool != create) {
                    image->window_pool = create;
                    result = ResultSuccess;
                }
            }
        }
    }
    LOG_INFO(Service_AM, "native window image pool id={}, create={}, result={:#x}",
              packet[2], create, result.raw);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result.raw);
}

} // namespace Service::AM

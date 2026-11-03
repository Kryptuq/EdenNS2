// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <thread>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include "common/cityhash.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/ns2_command_trace.h"
#include "core/core.h"
#include "video_core/dma_pusher.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/guest_memory.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace Tegra {

constexpr u32 MacroRegistersStart = 0xE00;
constexpr u32 ComputeInline = 0x6D;

DmaPusher::DmaPusher(Core::System& system_, MemoryManager& memory_manager_, Control::ChannelState& channel_state_)
    : system{system_}
    , memory_manager{memory_manager_}
    , channel_state{channel_state_}
    , signal_sync{false}
    , synced{false}
{}

DmaPusher::~DmaPusher() = default;

void DmaPusher::DispatchCalls() {
    dma_state.is_last_call = true;
    while (system.IsPoweredOn()) {
        if (!Step()) {
            break;
        }
    }
    system.GPU().FlushCommands();
    system.GPU().OnCommandListEnd();
}

bool DmaPusher::HasFailedNativeWork() const {
    const auto& context = channel_state.native_macro_context;
    return channel_state.native_abi && context &&
           (context->engine.native_execution_failed || context->compute_engine.native_execution_failed);
}

bool DmaPusher::Step() {
    if (HasFailedNativeWork()) return false;
    if (!ib_enable || dma_pushbuffer.empty()) {
        return false;
    }

    if (!puller.ResumeNativeAcquire(*this)) return false;

    CommandList& command_list = dma_pushbuffer.front();

    const size_t prefetch_size = command_list.prefetch_command_list.size();
    const size_t command_list_size = command_list.command_lists.size();

    if (prefetch_size == 0 && command_list_size == 0) {
        dma_pushbuffer.pop();
        dma_pushbuffer_subindex = 0;
        return true;
    }

    if (prefetch_size > 0) {
        const std::span<const CommandHeader> commands{command_list.prefetch_command_list};
        command_resume_offset += ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        if (puller.HasPendingNativeAcquire() || HasFailedNativeWork()) return false;
        command_resume_offset = 0;
        dma_pushbuffer.pop();
        return true;
    }

    auto& current_command = command_list.command_lists[dma_pushbuffer_subindex];
    const CommandListHeader& header = current_command;
    dma_state.dma_get = header.addr;

    if (signal_sync && !synced) {
        std::unique_lock lk(sync_mutex);
        sync_cv.wait(lk, [this]() { return synced; });
        signal_sync = false;
        synced = false;
    }

    if (header.size > 0) {
        if (subchannels[dma_state.subchannel] && dma_state.method_count) {
            const auto engine = subchannel_type[dma_state.subchannel];
            const bool kepler_payload = engine == Engines::EngineTypes::KeplerCompute && dma_state.method == ComputeInline && dma_state.non_incrementing;
            const bool macro_payload = engine == Engines::EngineTypes::Maxwell3D && dma_state.method >= MacroRegistersStart;
            if (kepler_payload || macro_payload) {
                const size_t words = std::min<size_t>(dma_state.method_count, header.size);
                subchannels[dma_state.subchannel]->current_dirty = memory_manager.IsMemoryDirty(dma_state.dma_get, words * sizeof(u32));
            }
        }
        const bool host_gpu_written = command_resume_offset == 0 && DownloadHostGpuWrittenList(
            header.addr, static_cast<u32>(header.size), command_list.Captured(dma_pushbuffer_subindex));
        if (host_gpu_written) {
            resume_from_capture = false;
            last_list_source = static_cast<u32>(VideoCommon::NS2CommandTrace::ListSource::HostGpuWritten);
        }
        // A list that starts inside a pending method holds that method's arguments, not commands:
        // an indirect dispatch's group counts, produced by earlier GPU work after the list was
        // submitted. The copy captured at submission predates that work (running it as submitted
        // launched garbage grids), so such a list always runs from memory.
        const bool argument_list = command_resume_offset == 0 && dma_state.method_count != 0 &&
                                   channel_state.native_abi;
        if (argument_list && !host_gpu_written) {
            const auto submitted = command_list.Captured(dma_pushbuffer_subindex);
            static std::atomic<u32> argument_logs{0};
            if (!submitted.empty() && header.size <= 8 && argument_logs.load(std::memory_order_relaxed) < 32) {
                std::array<u32, 8> live{};
                memory_manager.ReadBlockUnsafe(header.addr, live.data(), header.size * sizeof(u32));
                if (std::memcmp(live.data(), submitted.data(), header.size * sizeof(u32)) != 0) {
                    argument_logs.fetch_add(1, std::memory_order_relaxed);
                    std::array<u32, 8> words{};
                    for (size_t i = 0; i < header.size; ++i) words[i] = submitted[i].argument;
                    LOG_INFO(HW_GPU, "NS2: ch={} argument list {:#x} ({} words) changed since "
                             "submission: running {:08x} from memory instead of the captured {:08x}",
                             channel_state.bind_id, u64{header.addr}, u64{header.size},
                             fmt::join(std::span{live}.first(header.size), ","),
                             fmt::join(std::span{words}.first(header.size), ","));
                }
            }
            resume_from_capture = false;
            last_list_source = static_cast<u32>(VideoCommon::NS2CommandTrace::ListSource::Arguments);
        }
        const auto captured = host_gpu_written || argument_list
                                  ? std::span<const CommandHeader>{}
                                  : command_list.Captured(dma_pushbuffer_subindex);
        if (!captured.empty() && command_resume_offset == 0) {
            const u64 sequence = dma_pushbuffer_subindex < command_list.captured_sequences.size()
                ? command_list.captured_sequences[dma_pushbuffer_subindex] : 0;
            resume_from_capture = UseCapturedCommands(header.addr, captured, sequence);
        }
        if (command_resume_offset == 0) {
            using VideoCommon::NS2CommandTrace::ListSource;
            u64 age_us = 0;
            if (command_list.captured_time_us != 0) {
                const u64 now_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                age_us = now_us - command_list.captured_time_us;
            }
            VideoCommon::NS2CommandTrace::NoteList(channel_state.bind_id, header.addr,
                static_cast<u32>(header.size),
                captured.empty() && !host_gpu_written && !argument_list ? ListSource::Live : static_cast<ListSource>(last_list_source), age_us);
        }
        const bool use_safe = Settings::IsDMALevelDefault() ? Settings::IsGPULevelHigh() : Settings::IsDMALevelSafe();
        if (!captured.empty() && resume_from_capture) {
            command_resume_offset += ProcessCommands(captured.subspan(command_resume_offset), command_resume_offset);
        } else if (use_safe) {
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, Tegra::Memory::GuestMemoryFlags::SafeRead>headers(memory_manager, dma_state.dma_get, header.size, &command_headers);
            const std::span<const CommandHeader> commands{headers.data(), headers.size()};
            command_resume_offset += ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else {
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, Tegra::Memory::GuestMemoryFlags::UnsafeRead>headers(memory_manager, dma_state.dma_get, header.size, &command_headers);
            const std::span<const CommandHeader> commands{headers.data(), headers.size()};
            command_resume_offset += ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        }
    }

    if (puller.HasPendingNativeAcquire() || HasFailedNativeWork()) return false;
    command_resume_offset = 0;
    if (++dma_pushbuffer_subindex >= command_list_size) {
        dma_pushbuffer.pop();
        dma_pushbuffer_subindex = 0;
    } else {
        signal_sync = command_list.command_lists[dma_pushbuffer_subindex].sync && Settings::values.sync_memory_operations.GetValue();
    }

    if (signal_sync) {
        rasterizer->SignalFence([this]() {
            std::scoped_lock lk(sync_mutex);
            synced = true;
            sync_cv.notify_all();
        });
    }

    return true;
}

// The GPU can write commands or command data for itself: an indirect dispatch fetches its three
// group counts as the arguments of a compute macro straight from a buffer that an earlier compute
// shader filled. Eden runs that shader on the host GPU, so guest memory (and the copy captured at
// submission) still holds old data; running it launched garbage grids that never finished. The
// hardware reads the list only when it gets there, after the earlier work, so download what the
// host GPU wrote before running it. Returns whether the list was downloaded.
bool DmaPusher::DownloadHostGpuWrittenList(GPUVAddr address, u32 words,
                                           std::span<const CommandHeader> captured) {
    if (!channel_state.native_abi || rasterizer == nullptr || words == 0) {
        return false;
    }
    const u64 bytes = u64{words} * sizeof(u32);
    const auto device_address = memory_manager.GpuToCpuAddress(address);
    if (!device_address || !memory_manager.IsContinuousRange(address, bytes) ||
        !rasterizer->MustFlushRegion(*device_address, bytes)) {
        return false;
    }
    static std::atomic<u32> logged{0};
    const bool log = logged.fetch_add(1, std::memory_order_relaxed) < 32;
    std::array<u32, 8> before{};
    const u32 shown = std::min<u32>(words, static_cast<u32>(before.size()));
    if (log) memory_manager.ReadBlockUnsafe(address, before.data(), shown * sizeof(u32));
    rasterizer->FlushRegion(*device_address, bytes);
    if (log) {
        std::array<u32, 8> after{};
        memory_manager.ReadBlockUnsafe(address, after.data(), shown * sizeof(u32));
        std::array<u32, 8> submitted{};
        for (u32 i = 0; i < shown && i < captured.size(); ++i) submitted[i] = captured[i].argument;
        LOG_INFO(HW_GPU, "NS2: ch={} command list {:#x} ({} words) was written by the host GPU: "
                 "downloaded it before running it (first words: captured {:08x}, guest memory "
                 "{:08x}, host GPU {:08x})", channel_state.bind_id, address, words,
                 fmt::join(std::span{submitted}.first(captured.empty() ? 0 : shown), ","),
                 fmt::join(std::span{before}.first(shown), ","),
                 fmt::join(std::span{after}.first(shown), ","));
    }
    return true;
}

// The hardware fetches a command list right after it is submitted, but the GPU thread may reach it
// much later, and its memory may have changed by then. Two cases occur:
// - The guest reused the memory for a later submission (nn::movie records every video frame copy
//   into the same command buffer without waiting for the GPU): the current contents belong to
//   that later submission, so run this one as it was submitted. Reading it late executed the next
//   frame's copy instead and the cutscene flickered.
// - The guest completed the list in place after submitting it (channel 1 submits small lists with
//   placeholder values and fills them in before the GPU is meant to read them): run the current
//   contents, as the GPU would. Running the placeholders hung the GPU after the cutscene.
// The current contents belong to a later submission when that submission captured exactly them.
// Lists the GPU itself wrote since (GPU-generated commands) always run from memory.
bool DmaPusher::UseCapturedCommands(GPUVAddr address, std::span<const CommandHeader> captured, u64 sequence) {
    using VideoCommon::NS2CommandTrace::ListSource;
    const size_t bytes = captured.size_bytes();
    if (memory_manager.IsMemoryDirty(address, bytes)) {
        last_list_source = static_cast<u32>(ListSource::GpuWritten);
        return false;
    }
    live_headers.resize_destructive(captured.size());
    memory_manager.ReadBlockUnsafe(address, live_headers.data(), bytes);
    if (std::memcmp(live_headers.data(), captured.data(), bytes) == 0) {
        last_list_source = static_cast<u32>(ListSource::Captured);
        return true;
    }
    u64 live_hash = Common::CityHash64(reinterpret_cast<const char*>(live_headers.data()), bytes);
    bool reused = NativeSubmissions::ClaimedLater(channel_state.bind_id, address, sequence, live_hash);
    if (!reused && NativeSubmissions::UsuallyReused(channel_state.bind_id, address)) {
        // The guest may be recording the next list into this memory right now and submit it a
        // moment later (nn::movie: record, then submit). Memory with a history of reuse gets a
        // short grace period for that submission to arrive before the change counts as completed
        // in place; reading it now ran the next frame's copy (the remaining movie flicker).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{10};
        while (!reused && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
            memory_manager.ReadBlockUnsafe(address, live_headers.data(), bytes);
            live_hash = Common::CityHash64(reinterpret_cast<const char*>(live_headers.data()), bytes);
            reused = NativeSubmissions::ClaimedLater(channel_state.bind_id, address, sequence, live_hash);
        }
        ++waited_lists;
    }
    NativeSubmissions::NoteOutcome(channel_state.bind_id, address, reused);
    last_list_source = static_cast<u32>(reused ? ListSource::Reused : ListSource::Completed);
    const u64 count = reused ? ++reused_lists : ++completed_lists;
    if (count <= 16 || count % 1000 == 0) {
        LOG_INFO(HW_GPU, "NS2: ch={} command list {:#x} ({} words) changed after submission: {} (#{}; reused {}, completed in place {}, waited for a submission {})",
                 channel_state.bind_id, address, captured.size(),
                 reused ? "reused for a later submission, running it as submitted" : "completed in place, running the current contents",
                 count, reused_lists, completed_lists, waited_lists);
    }
    return reused;
}

namespace NativeSubmissions {
namespace {
struct Submission {
    u64 sequence;
    u64 hash;
    u32 reused;    ///< Changes of this list that a later submission claimed
    u32 completed; ///< Changes that no submission claimed (completed in place)
};
std::mutex mutex;
std::unordered_map<u64, Submission> latest;
u64 next_sequence{};
u64 Key(s32 channel, GPUVAddr address) {
    return (static_cast<u64>(static_cast<u32>(channel)) << 40) | (address & ((1ULL << 40) - 1));
}
} // namespace

u64 Record(s32 channel, GPUVAddr address, std::span<const CommandHeader> words) {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(words.data()), words.size_bytes());
    std::scoped_lock lock{mutex};
    if (latest.size() > (1U << 18)) {
        latest.clear();
    }
    const u64 sequence = ++next_sequence;
    Submission& entry = latest[Key(channel, address)];
    entry.sequence = sequence;
    entry.hash = hash;
    return sequence;
}

void NoteOutcome(s32 channel, GPUVAddr address, bool reused) {
    std::scoped_lock lock{mutex};
    const auto it = latest.find(Key(channel, address));
    if (it == latest.end()) return;
    u32& counter = reused ? it->second.reused : it->second.completed;
    counter = (std::min)(counter + 1, 1U << 30);
}

bool UsuallyReused(s32 channel, GPUVAddr address) {
    std::scoped_lock lock{mutex};
    const auto it = latest.find(Key(channel, address));
    return it != latest.end() && it->second.reused >= 8 && it->second.reused > 4 * it->second.completed;
}

bool ClaimedLater(s32 channel, GPUVAddr address, u64 sequence, u64 hash) {
    std::scoped_lock lock{mutex};
    const auto it = latest.find(Key(channel, address));
    return it != latest.end() && it->second.sequence > sequence && it->second.hash == hash;
}
} // namespace NativeSubmissions

size_t DmaPusher::ProcessCommands(std::span<const CommandHeader> commands, size_t word_offset) {
    for (size_t index = 0; index < commands.size();) {
        // Data word of methods command
        if (dma_state.method_count && dma_state.non_incrementing) {
            auto const& command_header = commands[index]; //must ref (MUltiMethod re)
            dma_state.dma_word_offset = u64((word_offset + index) * sizeof(u32));
            u32 max_write = u32(std::min<std::size_t>(index + dma_state.method_count, commands.size()) - index);
            if (channel_state.native_abi && dma_state.method < non_puller_methods) max_write = 1;
            CallMultiMethod(&command_header.argument, max_write);
            dma_state.method_count -= max_write;
            dma_state.is_last_call = true;
            index += max_write;
        } else if (dma_state.method_count) {
            auto const command_header = commands[index]; //can copy
            dma_state.dma_word_offset = u64((word_offset + index) * sizeof(u32));
            dma_state.is_last_call = dma_state.method_count <= 1;
            CallMethod(command_header.argument);
            dma_state.method += !dma_state.non_incrementing ? 1 : 0;
            dma_state.non_incrementing |= dma_increment_once;
            dma_state.method_count--;
            index++;
        } else {
            auto const command_header = commands[index]; //can copy
            // No command active - this is the first word of a new one
            switch (command_header.mode) {
            case SubmissionMode::Increasing:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = false;
                break;
            case SubmissionMode::NonIncreasing:
                SetState(command_header);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::Inline:
                dma_state.method = command_header.method;
                dma_state.subchannel = command_header.subchannel;
                dma_state.dma_word_offset = u64(-s64(dma_state.dma_get)); // negate to set address as 0
                trace_inline_source = dma_state.dma_get + u64(word_offset + index) * sizeof(u32);
                CallMethod(command_header.arg_count);
                trace_inline_source = 0;
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::IncreaseOnce:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = true;
                break;
            default:
                break;
            }
            index++;
        }
        if (puller.HasPendingNativeAcquire() || HasFailedNativeWork()) return index;
    }
    return commands.size();
}

void DmaPusher::SetState(const CommandHeader& command_header) {
    dma_state.method = command_header.method;
    dma_state.subchannel = command_header.subchannel;
    dma_state.method_count = command_header.method_count;
}

void DmaPusher::CallMethod(u32 argument) {
    VideoCommon::NS2CommandTrace::NoteMethod(dma_state.subchannel, dma_state.method, argument,
        trace_inline_source != 0 ? trace_inline_source : dma_state.dma_get + dma_state.dma_word_offset);
    if (dma_state.method < non_puller_methods) {
        puller.CallPullerMethod(*this, Engines::Puller::MethodCall{
            dma_state.method,
            argument,
            dma_state.subchannel,
            dma_state.method_count,
        });
    } else {
        auto subchannel = subchannels[dma_state.subchannel];
        if (!subchannel->execution_mask[dma_state.method]) {
            subchannel->method_sink.emplace_back(dma_state.method, argument);
        } else {
            subchannel->ConsumeSink(system);
            subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
            subchannel->CallMethod(system, dma_state.method, argument, dma_state.is_last_call);
        }
    }
}

void DmaPusher::CallMultiMethod(const u32* base_start, u32 num_methods) {
    // Every argument of a non-incrementing method goes to the same method; record up to 64.
    for (u32 i = 0; i < std::min<u32>(num_methods, 64); ++i) {
        VideoCommon::NS2CommandTrace::NoteMethod(dma_state.subchannel, dma_state.method, base_start[i],
            dma_state.dma_get + dma_state.dma_word_offset + u64{i} * sizeof(u32));
    }
    if (dma_state.method < non_puller_methods) {
        puller.CallMultiMethod(*this, dma_state.method, dma_state.subchannel, base_start, num_methods, dma_state.method_count);
    } else {
        auto subchannel = subchannels[dma_state.subchannel];
        subchannel->ConsumeSink(system);
        subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
        subchannel->CallMultiMethod(system, dma_state.method, base_start, num_methods, dma_state.method_count);
    }
}

void DmaPusher::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
}

} // namespace Tegra

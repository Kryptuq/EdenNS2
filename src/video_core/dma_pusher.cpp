// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <fmt/format.h>
#include "common/cityhash.h"
#include "common/logging.h"
#include "common/settings.h"
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
        const auto captured = command_list.Captured(dma_pushbuffer_subindex);
        if (!captured.empty() && command_resume_offset == 0) {
            const u64 sequence = dma_pushbuffer_subindex < command_list.captured_sequences.size()
                ? command_list.captured_sequences[dma_pushbuffer_subindex] : 0;
            resume_from_capture = UseCapturedCommands(header.addr, captured, sequence);
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
    const size_t bytes = captured.size_bytes();
    if (memory_manager.IsMemoryDirty(address, bytes)) {
        return false;
    }
    live_headers.resize_destructive(captured.size());
    memory_manager.ReadBlockUnsafe(address, live_headers.data(), bytes);
    if (std::memcmp(live_headers.data(), captured.data(), bytes) == 0) {
        return true;
    }
    const u64 live_hash = Common::CityHash64(reinterpret_cast<const char*>(live_headers.data()), bytes);
    const bool reused = NativeSubmissions::ClaimedLater(channel_state.bind_id, address, sequence, live_hash);
    const u64 count = reused ? ++reused_lists : ++completed_lists;
    if (count <= 16 || count % 1000 == 0) {
        LOG_INFO(HW_GPU, "NS2: ch={} command list {:#x} ({} words) changed after submission: {} (#{}; reused {}, completed in place {})",
                 channel_state.bind_id, address, captured.size(),
                 reused ? "reused for a later submission, running it as submitted" : "completed in place, running the current contents",
                 count, reused_lists, completed_lists);
    }
    return reused;
}

namespace NativeSubmissions {
namespace {
struct Submission {
    u64 sequence;
    u64 hash;
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
    latest[Key(channel, address)] = {sequence, hash};
    return sequence;
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
                CallMethod(command_header.arg_count);
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

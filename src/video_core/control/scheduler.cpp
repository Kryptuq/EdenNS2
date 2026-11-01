// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <memory>
#include <vector>

#include "common/assert.h"
#include "video_core/control/channel_state.h"
#include "video_core/control/scheduler.h"
#include "video_core/ns2_order_trace.h"
#include "common/logging.h"
#include "video_core/gpu.h"

namespace Tegra::Control {

void Scheduler::Push(GPU& gpu, s32 channel, CommandList&& entries) {
    std::shared_ptr<ChannelState> channel_state;
    {
        std::unique_lock lk(scheduling_guard);
        auto it = channels.find(channel);
        ASSERT(it != channels.end());
        channel_state = it->second;
        gpu.BindChannel(channel_state->bind_id);
    }
    VideoCommon::NS2Order::g_channel.store(channel_state->bind_id, std::memory_order_relaxed);
    if (channel_state->bind_id != 1 && VideoCommon::NS2Order::Take()) {
        LOG_INFO(HW_GPU, "NS2 order: submit ch={} lists={} suspended={}", channel_state->bind_id,
                 entries.command_lists.size(), channel_state->payload->dma_pusher.HasPendingNativeAcquire());
    }
    // Process commands outside the lock to reduce contention.
    // Multiple channels can prepare their commands in parallel.
    channel_state->payload->dma_pusher.Push(std::move(entries));
    channel_state->payload->dma_pusher.DispatchCalls();
    if (channel_state->payload->dma_pusher.HasPendingNativeAcquire() &&
        std::find(pending_native_channels.begin(), pending_native_channels.end(), channel_state) == pending_native_channels.end()) {
        pending_native_channels.push_back(channel_state);
    }
    if (!pending_native_channels.empty()) ResumePending(gpu);
}

void Scheduler::ResumePending(GPU& gpu) {
    ResumeReadyNativeChannels(pending_native_channels,
        [](const auto& state) { return state->payload->dma_pusher.HasPendingNativeAcquire() &&
                                     state->payload->dma_pusher.CanResumeNativeAcquire(); },
        [&](const auto& state) {
            {
                std::scoped_lock lock{scheduling_guard};
                gpu.BindChannel(state->bind_id);
            }
            VideoCommon::NS2Order::g_channel.store(state->bind_id, std::memory_order_relaxed);
            if (VideoCommon::NS2Order::Take()) {
                LOG_INFO(HW_GPU, "NS2 order: resume ch={}", state->bind_id);
            }
            state->payload->dma_pusher.DispatchCalls();
        });
    std::erase_if(pending_native_channels,
        [](const auto& state) { return !state->payload->dma_pusher.HasPendingNativeAcquire(); });
}

void Scheduler::DeclareChannel(std::shared_ptr<ChannelState> new_channel) {
    s32 channel = new_channel->bind_id;
    std::unique_lock lk(scheduling_guard);
    channels.emplace(channel, new_channel);
}

} // namespace Tegra::Control

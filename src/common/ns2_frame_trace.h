// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include "common/common_types.h"

namespace Common::NS2FrameTrace {

/// Yoshi movie diagnostics: set by the texture cache while it traces whole frames, so the
/// services log their side (presents, decoder outputs) for the same frames.
inline std::atomic<bool>& Active() {
    static std::atomic<bool> active{};
    return active;
}

/// The texture cache's frame number while tracing (for log correlation).
inline std::atomic<u64>& Frame() {
    static std::atomic<u64> frame{};
    return frame;
}

/// A constant buffer whose texture handle resolved to no image (its GPU address): inline
/// constant buffer uploads into it are logged.
inline std::atomic<u64>& WatchedConstBuffer() {
    static std::atomic<u64> address{};
    return address;
}

/// Handle blocks a traced draw found no image in: GPU address, device address, the GPU frame of
/// the draw. Their contents are logged again at the end of the following frames, and CPU writes
/// into them are logged, to tell data written too late from data overwritten too early.
struct WatchedBlock {
    u64 gpu_addr;
    u64 device_addr;
    u64 frame;
};
inline std::mutex& WatchedBlocksMutex() {
    static std::mutex mutex;
    return mutex;
}
inline std::vector<WatchedBlock>& WatchedBlocks() {
    static std::vector<WatchedBlock> blocks;
    return blocks;
}
inline void WatchBlock(u64 gpu_addr, u64 device_addr, u64 frame) {
    std::scoped_lock lock{WatchedBlocksMutex()};
    auto& blocks = WatchedBlocks();
    for (const auto& block : blocks) {
        if (block.gpu_addr == gpu_addr && block.frame == frame) {
            return;
        }
    }
    if (blocks.size() < 64) {
        blocks.push_back({gpu_addr, device_addr, frame});
    }
}

/// The bound graphics texture header table (GPU address), for logging GPU-side writes into the
/// headers the movie draw uses.
inline std::atomic<u64>& HeaderTable() {
    static std::atomic<u64> address{};
    return address;
}

} // namespace Common::NS2FrameTrace

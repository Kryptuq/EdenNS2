// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
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

/// Writer trace: the first page of every region a movie draw's handle block lies in. Guest CPU
/// writes into these pages that reach the JIT's memory callbacks are logged with the writing
/// thread's registers and frame-pointer chain (to find the code that writes, and overwrites, the
/// block), and draws reading a constant buffer in these pages are logged with the data they read.
struct WriterPage {
    std::atomic<uintptr_t> host{}; ///< Host pointer of the page (what CPU and GPU both map to)
    std::atomic<u64> gpu{};        ///< GPU address of the page
};
inline constexpr size_t MaxWriterPages = 8;
inline std::array<WriterPage, MaxWriterPages>& WriterPages() {
    static std::array<WriterPage, MaxWriterPages> pages{};
    return pages;
}
inline std::atomic<u32>& WriterPageCount() {
    static std::atomic<u32> count{};
    return count;
}
inline std::atomic<int>& WriterCpuBudget() {
    static std::atomic<int> budget{1500};
    return budget;
}
inline std::atomic<int>& WriterGpuBudget() {
    static std::atomic<int> budget{1500};
    return budget;
}
/// Returns true if the page was added now.
inline bool AddWriterPage(uintptr_t host, u64 gpu) {
    static std::mutex mutex;
    std::scoped_lock lock{mutex};
    const u32 count = WriterPageCount().load(std::memory_order_relaxed);
    for (u32 i = 0; i < count; ++i) {
        if (WriterPages()[i].host.load(std::memory_order_relaxed) == host) {
            return false;
        }
    }
    if (count >= MaxWriterPages) {
        return false;
    }
    WriterPages()[count].host.store(host, std::memory_order_relaxed);
    WriterPages()[count].gpu.store(gpu, std::memory_order_relaxed);
    WriterPageCount().store(count + 1, std::memory_order_release);
    return true;
}
/// Index of the watched page a host range touches, or -1.
inline int FindWriterPageHost(uintptr_t host, size_t size) {
    const u32 count = WriterPageCount().load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        const uintptr_t page = WriterPages()[i].host.load(std::memory_order_relaxed);
        if (host < page + 0x1000 && host + size > page) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
inline int FindWriterPageGpu(u64 gpu, u64 size) {
    const u32 count = WriterPageCount().load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        const u64 page = WriterPages()[i].gpu.load(std::memory_order_relaxed);
        if (gpu < page + 0x1000 && gpu + size > page) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
/// Microseconds since the first call, for ordering CPU and GPU events in the log.
inline u64 TraceMicros() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
}

} // namespace Common::NS2FrameTrace

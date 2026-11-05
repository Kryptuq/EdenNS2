// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"

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
/// Presents handed back to the game (nx2:vi2 229 replies). A write logged with present N belongs
/// to the frame the game presents as N + 1; it places traced writes and reads in the game's frames.
inline std::atomic<u64>& PresentsReturned() {
    static std::atomic<u64> count{};
    return count;
}
inline std::atomic<int>& PresentTraceBudget() {
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

/// Emulator-side writes into the writer-trace pages (buffer/texture cache downloads, engine
/// writes): which path writes (thread-local, set by the caller), and the movie block at +0x100 of
/// the page before and after the copy. Tells an emulator write-back apart from a game rewrite.
inline const char*& EmulatorWriteTag() {
    thread_local const char* tag = nullptr;
    return tag;
}
struct EmulatorWriteTagScope {
    const char* previous;
    explicit EmulatorWriteTagScope(const char* tag) : previous{EmulatorWriteTag()} {
        EmulatorWriteTag() = tag;
    }
    ~EmulatorWriteTagScope() {
        EmulatorWriteTag() = previous;
    }
    EmulatorWriteTagScope(const EmulatorWriteTagScope&) = delete;
    EmulatorWriteTagScope& operator=(const EmulatorWriteTagScope&) = delete;
};
inline std::atomic<int>& EmulatorWriteBudget() {
    static std::atomic<int> budget{3000};
    return budget;
}
inline std::atomic<int>& GpuWrittenMarkBudget() {
    static std::atomic<int> budget{600};
    return budget;
}
/// Put around an emulator-side copy into host memory [dst, dst + size).
class EmulatorWriteNote {
public:
    EmulatorWriteNote(const void* dst, size_t size, u64 device_addr)
        : dst_{reinterpret_cast<uintptr_t>(dst)}, size_{size}, device_addr_{device_addr} {
        if (WriterPageCount().load(std::memory_order_relaxed) == 0) [[likely]] {
            return;
        }
        page_ = FindWriterPageHost(dst_, size);
        if (page_ < 0) {
            return;
        }
        block_ = WriterPages()[page_].host.load(std::memory_order_relaxed) + 0x100;
        std::memcpy(before_.data(), reinterpret_cast<const void*>(block_), sizeof(before_));
    }
    ~EmulatorWriteNote() {
        if (page_ < 0 || EmulatorWriteBudget().fetch_sub(1, std::memory_order_relaxed) <= 0) {
            return;
        }
        std::array<u32, 4> after{};
        std::memcpy(after.data(), reinterpret_cast<const void*>(block_), sizeof(after));
        const uintptr_t page_host = WriterPages()[page_].host.load(std::memory_order_relaxed);
        const u64 page_gpu = WriterPages()[page_].gpu.load(std::memory_order_relaxed);
        const char* tag = EmulatorWriteTag();
        LOG_INFO(HW_Memory,
                 "NS2 writer trace: t={}us GPU frame {} present {} emulator write by {} into GPU "
                 "{:#x} (page {} +{:#x}) {} bytes (device {:#x}): block +0x100 "
                 "{:08x} {:08x} {:08x} {:08x} -> {:08x} {:08x} {:08x} {:08x}{}",
                 TraceMicros(), Frame().load(std::memory_order_relaxed),
                 PresentsReturned().load(std::memory_order_relaxed),
                 tag != nullptr ? tag : "an unattributed path", page_gpu + (dst_ - page_host),
                 page_, dst_ - page_host, size_, device_addr_, before_[0], before_[1], before_[2],
                 before_[3], after[0], after[1], after[2], after[3],
                 before_ != after ? " (block changed)" : "");
    }
    EmulatorWriteNote(const EmulatorWriteNote&) = delete;
    EmulatorWriteNote& operator=(const EmulatorWriteNote&) = delete;

private:
    uintptr_t dst_;
    size_t size_;
    u64 device_addr_;
    int page_{-1};
    uintptr_t block_{};
    std::array<u32, 4> before_{};
};

} // namespace Common::NS2FrameTrace

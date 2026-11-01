// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include "common/common_types.h"

// Bounded trace of the order in which Switch 2 GPU work executes across channels (submissions,
// 2D blits, semaphores, syncpoints), started by the first 2D blit (the in-game movie).
namespace VideoCommon::NS2Order {
inline std::atomic<s32> g_channel{-1};
inline std::atomic<u32> g_remaining{0};
inline std::atomic<bool> g_started{false};

inline void Start(u32 events) {
    if (!g_started.exchange(true)) g_remaining.store(events);
}
inline bool Take() {
    u32 remaining = g_remaining.load(std::memory_order_relaxed);
    while (remaining != 0 &&
           !g_remaining.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed)) {
    }
    return remaining != 0;
}
inline s32 Channel() { return g_channel.load(std::memory_order_relaxed); }
} // namespace VideoCommon::NS2Order

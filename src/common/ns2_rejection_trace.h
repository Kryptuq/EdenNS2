// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Diagnostics only (yoshi-integration, yint-28): the life of a native program that was rejected
// because its memory was not valid when it was bound. Answers: is its memory written later, by
// whom (CPU thread or GPU engine + method), how long after the rejecting bind, is the rejection
// lifted by that write, and does the next bind then lower. Nothing here changes behaviour: it
// only records and logs.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/ns2_frame_trace.h"

namespace Common::NS2RejectionTrace {

using Clock = std::chrono::steady_clock;

/// The GPU method being executed on this thread (set by the puller; engine 0xffff = puller).
struct MethodContext {
    bool active = false;
    u32 engine = 0;
    u32 method = 0;
    s32 channel = -1;
};
inline MethodContext& CurrentMethod() {
    thread_local MethodContext context;
    return context;
}
struct MethodScope {
    MethodContext previous;
    MethodScope(u32 engine, u32 method, s32 channel) : previous{CurrentMethod()} {
        CurrentMethod() = MethodContext{true, engine, method, channel};
    }
    ~MethodScope() {
        CurrentMethod() = previous;
    }
    MethodScope(const MethodScope&) = delete;
    MethodScope& operator=(const MethodScope&) = delete;
};

/// Which path is invalidating caches on this thread (a static string; nullptr = none set).
inline const char*& InvalidationSource() {
    thread_local const char* source = nullptr;
    return source;
}
struct SourceScope {
    const char* previous;
    explicit SourceScope(const char* source) : previous{InvalidationSource()} {
        InvalidationSource() = source;
    }
    ~SourceScope() {
        InvalidationSource() = previous;
    }
    SourceScope(const SourceScope&) = delete;
    SourceScope& operator=(const SourceScope&) = delete;
};
inline const char* CpuCoreSource(size_t core) {
    static constexpr std::array<const char*, 4> names{
        "CPU write (core 0)", "CPU write (core 1)", "CPU write (core 2)",
        "CPU write (core 3 or a host/HLE thread)"};
    return names[(std::min)(core, names.size() - 1)];
}

inline std::string DescribeWriter(const char* path) {
    std::string text;
    if (const char* source = InvalidationSource()) {
        text = source;
    }
    if (path != nullptr) {
        text += text.empty() ? "" : ", ";
        text += path;
    }
    if (const MethodContext& method = CurrentMethod(); method.active) {
        text += text.empty() ? "" : ", during ";
        text += method.engine == 0xffff
                    ? fmt::format("puller method {:#x} (channel {})", method.method, method.channel)
                    : fmt::format("GPU engine {:04X} method {:#x} (channel {})", method.engine,
                                  method.method, method.channel);
    }
    return text.empty() ? std::string{"an unattributed thread"} : text;
}

struct Entry {
    u64 program{};
    u64 begin{};
    u64 size{};
    Clock::time_point rejected{};
    Clock::time_point lifted{};
    u32 skipped{};
    u32 unsafe_writes{};
    bool is_lifted{};
    u8 lift_logs{};
    u8 result_logs{};
    u8 reject_logs{};
    u8 unsafe_logs{};
};

struct State {
    std::mutex mutex;
    std::vector<Entry> entries;
    std::atomic<u64> lowest{std::numeric_limits<u64>::max()};
    std::atomic<u64> highest{0};
    std::atomic<u32> count{0};
    int budget = 400;
    Clock::time_point last_summary{Clock::now()};
    u32 skipped_since_summary = 0;
    int summaries = 60;
};
inline State& GetState() {
    static State state;
    return state;
}

inline double Ms(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

inline bool TakeLine(State& state, u8& per_entry) {
    if (state.budget <= 0 || per_entry >= 3) {
        return false;
    }
    --state.budget;
    ++per_entry;
    return true;
}

inline void Recompute(State& state) {
    u64 lowest = std::numeric_limits<u64>::max();
    u64 highest = 0;
    for (const Entry& entry : state.entries) {
        lowest = (std::min)(lowest, entry.begin);
        highest = (std::max)(highest, entry.begin + entry.size);
    }
    state.lowest.store(lowest, std::memory_order_relaxed);
    state.highest.store(highest, std::memory_order_relaxed);
    state.count.store(static_cast<u32>(state.entries.size()), std::memory_order_relaxed);
}

inline Entry* Find(State& state, u64 program) {
    const auto it = std::find_if(state.entries.begin(), state.entries.end(),
                                 [program](const Entry& entry) { return entry.program == program; });
    return it == state.entries.end() ? nullptr : &*it;
}

/// A program was rejected (or rejected again after a lift). [begin, begin + size) = what the
/// capture read (device addresses).
inline void OnRejected(u64 program, u64 begin, u64 size, const std::string& what) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    const auto now = Clock::now();
    if (Entry* entry = Find(state, program)) {
        if (entry->is_lifted && TakeLine(state, entry->reject_logs)) {
            LOG_INFO(HW_GPU,
                     "Rejection trace: program {:#x} REJECTED AGAIN on the bind after its lift "
                     "({:.1f} ms after the lift): {}",
                     program, Ms(entry->lifted, now), what);
        }
        entry->begin = begin;
        entry->size = size;
        entry->rejected = now;
        entry->is_lifted = false;
        entry->skipped = 0;
        entry->unsafe_writes = 0;
    } else {
        state.entries.push_back(Entry{.program = program, .begin = begin, .size = size, .rejected = now});
        u8 first{};
        if (TakeLine(state, first)) {
            LOG_INFO(HW_GPU, "Rejection trace: tracking program {:#x}, captured range {:#x}+{:#x}",
                     program, begin, size);
        }
    }
    Recompute(state);
}

inline void MaybeSummary(State& state, Clock::time_point now) {
    if (state.summaries <= 0 || state.skipped_since_summary == 0 ||
        now - state.last_summary < std::chrono::seconds{10}) {
        return;
    }
    u32 lifted = 0, never_written = 0, unsafe_only = 0;
    const Entry* oldest = nullptr;
    for (const Entry& entry : state.entries) {
        if (entry.is_lifted) {
            ++lifted;
            continue;
        }
        entry.unsafe_writes != 0 ? ++unsafe_only : ++never_written;
        if (oldest == nullptr || entry.rejected < oldest->rejected) {
            oldest = &entry;
        }
    }
    --state.summaries;
    LOG_INFO(HW_GPU,
             "Rejection trace summary: {} rejected programs ({} lifted, awaiting a rebuild; {} still "
             "rejected and never written since; {} written only by unsafe paths), {} rejected binds "
             "in the last {:.1f} s{}",
             state.entries.size(), lifted, never_written, unsafe_only, state.skipped_since_summary,
             Ms(state.last_summary, now) / 1000.0,
             oldest != nullptr ? fmt::format(", oldest still rejected {:#x} for {:.1f} s",
                                             oldest->program, Ms(oldest->rejected, now) / 1000.0)
                               : std::string{});
    state.last_summary = now;
    state.skipped_since_summary = 0;
}

/// A draw/dispatch was skipped because its program is rejected.
inline void NoteSkip(u64 program) {
    State& state = GetState();
    if (state.count.load(std::memory_order_relaxed) == 0) {
        return;
    }
    std::scoped_lock lock{state.mutex};
    if (Entry* entry = Find(state, program)) {
        ++entry->skipped;
    }
    ++state.skipped_since_summary;
    MaybeSummary(state, Clock::now());
}

/// The shader cache dropped the rejection because [addr, addr + size) was invalidated.
inline void OnLifted(u64 program, u64 addr, u64 size) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    Entry* entry = Find(state, program);
    if (entry == nullptr || entry->is_lifted) {
        return;
    }
    const auto now = Clock::now();
    entry->is_lifted = true;
    entry->lifted = now;
    if (TakeLine(state, entry->lift_logs)) {
        LOG_INFO(HW_GPU,
                 "Rejection trace: program {:#x} LIFTED by {}: write {:#x}+{:#x}, {:.1f} ms after "
                 "the rejecting bind; {} binds skipped meanwhile, {} earlier unsafe writes",
                 program, DescribeWriter(nullptr), addr, size, Ms(entry->rejected, now),
                 entry->skipped, entry->unsafe_writes);
    }
}

/// The program at this address was lowered successfully.
inline void OnBuilt(u64 program) {
    State& state = GetState();
    if (state.count.load(std::memory_order_relaxed) == 0) {
        return;
    }
    std::scoped_lock lock{state.mutex};
    Entry* entry = Find(state, program);
    if (entry == nullptr) {
        return;
    }
    if (TakeLine(state, entry->result_logs)) {
        LOG_INFO(HW_GPU,
                 "Rejection trace: program {:#x} LOWERED OK on the next bind ({:.1f} ms after the "
                 "lift, {:.1f} ms after the rejecting bind)",
                 program, Ms(entry->lifted, Clock::now()), Ms(entry->rejected, Clock::now()));
    }
    state.entries.erase(state.entries.begin() + (entry - state.entries.data()));
    Recompute(state);
}

/// An emulator-side write that does NOT notify the shader cache by itself (device/GPU
/// WriteBlockUnsafe, Write<T>, cached writes). Logged once per rejected program it reaches.
inline void NoteUnsafeWrite(u64 addr, u64 size, const char* path) {
    State& state = GetState();
    if (addr + size <= state.lowest.load(std::memory_order_relaxed) ||
        addr >= state.highest.load(std::memory_order_relaxed)) [[likely]] {
        return;
    }
    std::scoped_lock lock{state.mutex};
    const auto now = Clock::now();
    for (Entry& entry : state.entries) {
        if (entry.is_lifted || addr >= entry.begin + entry.size || addr + size <= entry.begin) {
            continue;
        }
        ++entry.unsafe_writes;
        if (entry.unsafe_writes == 1 && TakeLine(state, entry.unsafe_logs)) {
            const char* tag = Common::NS2FrameTrace::EmulatorWriteTag();
            LOG_INFO(HW_GPU,
                     "Rejection trace: program {:#x} written by an unsafe emulator write ({}): "
                     "{:#x}+{:#x}, {:.1f} ms after the rejecting bind (no shader-cache "
                     "notification by this write itself)",
                     entry.program, DescribeWriter(tag != nullptr ? tag : path), addr, size,
                     Ms(entry.rejected, now));
        }
    }
}

} // namespace Common::NS2RejectionTrace

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <map>
#include <mutex>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/ns2_draw_stats.h"

namespace VideoCommon::NS2Draw {
namespace {
struct Entry {
    std::array<u32, static_cast<size_t>(Result::Count)> results{};
    u64 elements{};
    u64 zeta{};
    u32 indexed{};
};
std::mutex mutex;
std::map<u64, Entry> entries;
std::atomic<u32> skipped_logs{};
std::atomic<s32> method_trace_budget{};
std::atomic<bool> trace_macro_execute{};
constexpr const char* Name(Result result) {
    switch (result) {
    case Result::Drawn: return "drawn";
    case Result::NoPipeline: return "no pipeline";
    case Result::ConfigureFailed: return "configure failed";
    default: return "?";
    }
}
} // namespace

void Note(Result result, u64 rt0, u64 zeta, u32 elements, bool indexed) {
    if (result != Result::Drawn && skipped_logs.fetch_add(1, std::memory_order_relaxed) < 40) {
        LOG_WARNING(HW_GPU, "NS2 trace: draw skipped ({}): rt0={:#x} zeta={:#x} elements={} indexed={}",
                    Name(result), rt0, zeta, elements, indexed);
    }
    std::scoped_lock lock{mutex};
    if (entries.size() > 512) return;
    Entry& entry = entries[rt0];
    ++entry.results[static_cast<size_t>(result)];
    entry.elements += elements;
    entry.zeta = zeta;
    entry.indexed += indexed ? 1 : 0;
}

void Reset() {
    std::scoped_lock lock{mutex};
    entries.clear();
}

std::string Summary() {
    std::scoped_lock lock{mutex};
    std::string text;
    for (const auto& [rt0, entry] : entries) {
        text += fmt::format("  draws rt0={:#x} zeta={:#x}: drawn={} no_pipeline={} configure_failed={} "
                            "elements={} indexed={}\n",
                            rt0, entry.zeta, entry.results[0], entry.results[1], entry.results[2],
                            entry.elements, entry.indexed);
    }
    return text;
}

void ArmMethodTrace(s32 lines) {
    method_trace_budget.store(lines, std::memory_order_relaxed);
}

bool TakeMethodTrace() {
    if (method_trace_budget.load(std::memory_order_relaxed) <= 0) return false;
    return method_trace_budget.fetch_sub(1, std::memory_order_relaxed) > 0;
}

std::atomic<bool> frame_trace{};

void SetFrameTrace(bool active) {
    frame_trace.store(active, std::memory_order_relaxed);
}

bool FrameTraceActive() {
    return frame_trace.load(std::memory_order_relaxed);
}

void SetTraceMacroExecute(bool value) {
    trace_macro_execute.store(value, std::memory_order_relaxed);
}

bool TakeTraceMacroExecute() {
    return trace_macro_execute.exchange(false, std::memory_order_relaxed);
}

} // namespace VideoCommon::NS2Draw

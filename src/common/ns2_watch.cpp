// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cstring>

#include "common/ns2_watch.h"
#include "common/thread.h"

namespace Common::NS2Watch {

std::atomic<u64> g_present_count{0};
std::atomic<u64> g_composite_count{0};

namespace {

struct Slot {
    std::atomic<bool> used{false};
    std::atomic<const char*> label{nullptr};
    std::atomic<u64> since{0};
    std::atomic<u64> value{0};
    char detail[96]{};
    char thread[48]{};
};

constexpr size_t MaxSlots = 128;
std::array<Slot, MaxSlots> g_slots;

Slot* CurrentSlot() {
    thread_local Slot* slot = [] () -> Slot* {
        for (auto& candidate : g_slots) {
            bool expected = false;
            if (candidate.used.compare_exchange_strong(expected, true)) {
                const std::string name = Common::GetCurrentThreadName();
                std::strncpy(candidate.thread, name.c_str(), sizeof(candidate.thread) - 1);
                return &candidate;
            }
        }
        return nullptr;
    }();
    return slot;
}

void CopyDetail(char* out, const char* detail) {
    if (detail) {
        std::strncpy(out, detail, 95);
        out[95] = '\0';
    } else {
        out[0] = '\0';
    }
}

} // namespace

u64 NowMs() {
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void SetPhase(const char* label, const char* detail, u64 value) {
    Slot* slot = CurrentSlot();
    if (!slot) {
        return;
    }
    // Refresh the thread name lazily; threads are often named after their first phase.
    if (slot->thread[0] == '\0') {
        const std::string name = Common::GetCurrentThreadName();
        std::strncpy(slot->thread, name.c_str(), sizeof(slot->thread) - 1);
    }
    slot->since.store(0, std::memory_order_release);
    CopyDetail(slot->detail, detail);
    slot->value.store(value, std::memory_order_relaxed);
    slot->label.store(label, std::memory_order_relaxed);
    slot->since.store(label ? NowMs() : 0, std::memory_order_release);
}

void ClearPhase() {
    SetPhase(nullptr);
}

std::vector<PhaseInfo> CollectPhases(u64 min_age_ms) {
    std::vector<PhaseInfo> out;
    const u64 now = NowMs();
    for (auto& slot : g_slots) {
        if (!slot.used.load(std::memory_order_acquire)) {
            continue;
        }
        const u64 since = slot.since.load(std::memory_order_acquire);
        const char* label = slot.label.load(std::memory_order_relaxed);
        if (since == 0 || !label || now < since || now - since < min_age_ms) {
            continue;
        }
        char detail[96];
        std::memcpy(detail, slot.detail, sizeof(detail));
        detail[95] = '\0';
        out.push_back(PhaseInfo{slot.thread, label, detail, slot.value.load(std::memory_order_relaxed), now - since});
    }
    return out;
}

PhaseScope::PhaseScope(const char* label, const char* detail, u64 value) {
    Slot* slot = CurrentSlot();
    if (slot) {
        m_prev_label = slot->label.load(std::memory_order_relaxed);
        m_prev_value = slot->value.load(std::memory_order_relaxed);
        m_prev_since = slot->since.load(std::memory_order_relaxed);
        std::memcpy(m_prev_detail, slot->detail, sizeof(m_prev_detail));
    } else {
        m_prev_label = nullptr;
        m_prev_value = 0;
        m_prev_since = 0;
        m_prev_detail[0] = '\0';
    }
    SetPhase(label, detail, value);
}

PhaseScope::~PhaseScope() {
    Slot* slot = CurrentSlot();
    if (!slot) {
        return;
    }
    slot->since.store(0, std::memory_order_release);
    std::memcpy(slot->detail, m_prev_detail, sizeof(m_prev_detail));
    slot->value.store(m_prev_value, std::memory_order_relaxed);
    slot->label.store(m_prev_label, std::memory_order_relaxed);
    slot->since.store(m_prev_label ? m_prev_since : 0, std::memory_order_release);
}

} // namespace Common::NS2Watch

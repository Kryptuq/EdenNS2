// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

#include "common/common_types.h"

// Lightweight progress tracking used by the NS2 hang watchdog. Host threads publish what they are
// currently doing ("phases"); the watchdog reports phases that have been active for a long time
// when the guest stops presenting frames.
namespace Common::NS2Watch {

/// Number of frames the guest queued for display (bumped by the presentation services).
extern std::atomic<u64> g_present_count;
/// Number of frames actually composited by the renderer.
extern std::atomic<u64> g_composite_count;

/// Milliseconds since an arbitrary steady epoch.
u64 NowMs();

/// Sets the current thread's phase. `label` must be a string literal (or otherwise outlive the
/// phase); `detail` is copied.
void SetPhase(const char* label, const char* detail = nullptr, u64 value = 0);
/// Marks the current thread as idle.
void ClearPhase();

struct PhaseInfo {
    std::string thread;
    std::string label;
    std::string detail;
    u64 value;
    u64 age_ms;
};
/// Returns phases active for at least `min_age_ms`.
std::vector<PhaseInfo> CollectPhases(u64 min_age_ms);

/// Recent GPU work recorded by the renderer: draws (kind 0) and dispatches (kind 1) with the host
/// tick of the submission they are recorded into and their shader addresses. Repeats of the same
/// work in the same tick are counted instead of stored again.
struct RecentWork {
    u64 tick;
    u32 kind;
    u32 count;
    u64 shaders[6];
};
void NoteWork(u64 tick, u32 kind, const u64 (&shaders)[6]);
std::vector<RecentWork> CollectWork();

/// Installs a last-chance crash handler that writes the exception (code, address, module offset,
/// a stack of return addresses) and every thread's current phase to `path` before the process
/// dies. Eden's own log is written asynchronously and loses its last lines in a crash.
void InstallCrashReport(const std::string& path);

/// RAII helper that restores the previous phase on scope exit.
class PhaseScope {
public:
    explicit PhaseScope(const char* label, const char* detail = nullptr, u64 value = 0);
    ~PhaseScope();
    PhaseScope(const PhaseScope&) = delete;
    PhaseScope& operator=(const PhaseScope&) = delete;

private:
    const char* m_prev_label;
    u64 m_prev_value;
    u64 m_prev_since;
    char m_prev_detail[96];
};

} // namespace Common::NS2Watch

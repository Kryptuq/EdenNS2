// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

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

} // namespace Common::NS2FrameTrace

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

namespace Shader::Ampere {

/// Whether HMMA is lowered to NS2_HMMA (warp-shuffle emulation) without EDEN_NX2_TENSOR. Set for
/// the titles that need it (Yoshi and the Mysterious Book, whose upscaling passes use HMMA);
/// other titles keep rejecting HMMA by default (title 4's 372-HMMA network takes the host driver
/// minutes to compile).
inline std::atomic<bool>& NativeTensorByDefault() {
    static std::atomic<bool> enabled{false};
    return enabled;
}

} // namespace Shader::Ampere

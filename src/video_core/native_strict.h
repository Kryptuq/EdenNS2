// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

namespace VideoCommon {

/// Native (Switch 2) GPU work that cannot be translated is skipped by default so the
/// title keeps running and every unsupported shader can be collected in one session.
/// EDEN_NATIVE_STRICT restores the original behavior of blocking submission completion.
inline bool NativeStrict() {
    static const bool strict = std::getenv("EDEN_NATIVE_STRICT") != nullptr;
    return strict;
}

} // namespace VideoCommon

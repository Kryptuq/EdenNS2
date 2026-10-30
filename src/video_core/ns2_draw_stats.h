// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include "common/common_types.h"

namespace VideoCommon::NS2Draw {

enum class Result : u32 { Drawn, NoPipeline, ConfigureFailed, Count };

/// Records a draw attempt into render target 0 at the given GPU address.
void Note(Result result, u64 rt0, u64 zeta, u32 elements, bool indexed);

/// Clears the per-target statistics.
void Reset();

/// Per-target statistics since the last reset, one line per target.
std::string Summary();

} // namespace VideoCommon::NS2Draw

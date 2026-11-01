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

/// Arms logging of the next `lines` 3D engine method calls (bounded tracing of one frame).
void ArmMethodTrace(s32 lines);

/// True while method tracing is armed; consumes one line of the budget.
bool TakeMethodTrace();

/// Marks that the next native macro execution should log its refreshed parameters.
void SetTraceMacroExecute(bool value);

/// Frame trace: while active, every draw of the frame is logged with its render targets, shaders
/// and sampled images (black screen diagnostics).
void SetFrameTrace(bool active);
bool FrameTraceActive();
bool TakeTraceMacroExecute();

} // namespace VideoCommon::NS2Draw

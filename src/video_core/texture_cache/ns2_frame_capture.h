// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <string>

#include "common/common_types.h"
#include "video_core/surface.h"

namespace VideoCommon::NS2Capture {

/// Writes level 0 of a downloaded (linear, host layout) image as a JPEG into the log folder's
/// ns2_capture directory. Returns false for formats it cannot convert.
bool WriteImage(std::span<const u8> data, u32 width, u32 height, u32 row_pixels,
                VideoCore::Surface::PixelFormat format, const std::string& name);

/// Appends a line to ns2_capture/index.txt.
void AppendIndex(const std::string& line);

} // namespace VideoCommon::NS2Capture

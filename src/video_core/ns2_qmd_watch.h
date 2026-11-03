// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <string>

#include "common/common_types.h"

// NS2 diagnostics for QMDs whose grid is garbage when they run: a timeline of every read of a
// QMD (with its grid words) and of every GPU-engine write (copy engine, inline upload, semaphore
// or report write) that touches a QMD's grid words. When the grid of a read differs from the
// previous read and no engine write was seen in between, the guest CPU wrote it.
namespace VideoCommon::NS2QmdWatch {

/// A QMD at `address` was read; `grid` are its words 12-14.
void NoteRead(u64 address, const std::array<u32, 3>& grid, bool dispatched);

/// A GPU engine wrote [address, address + size). `source` names the engine, `extra` is engine
/// specific (copy source address, payload). `source_gpu_modified`: the copy source held data
/// the host GPU wrote that was not downloaded. `guest_grid` returns the grid words now in guest
/// memory for every QMD the write touched (filled by the callback).
template <typename ReadGrid>
void NoteWrite(const char* source, u64 address, u64 size, u64 extra, bool source_gpu_modified,
               ReadGrid&& read_grid);

/// Timeline of the QMD at `address` (empty if none was recorded).
std::string History(u64 address);

// Implementation detail for the template above.
bool TouchesGrid(u64 address, u64 size, u64* qmd_out, u32 max_qmds);
void RecordWrite(u64 qmd, const char* source, u64 address, u64 size, u64 extra,
                 bool source_gpu_modified, const std::array<u32, 3>& guest_grid);

template <typename ReadGrid>
void NoteWrite(const char* source, u64 address, u64 size, u64 extra, bool source_gpu_modified,
               ReadGrid&& read_grid) {
    std::array<u64, 4> qmds{};
    if (!TouchesGrid(address, size, qmds.data(), static_cast<u32>(qmds.size()))) {
        return;
    }
    for (const u64 qmd : qmds) {
        if (qmd == 0) break;
        RecordWrite(qmd, source, address, size, extra, source_gpu_modified, read_grid(qmd));
    }
}

} // namespace VideoCommon::NS2QmdWatch

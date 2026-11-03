// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include "common/common_types.h"

// NS2 diagnostics: the most recent command-list fetches and methods the GPU thread processed,
// with the guest address every argument was read from. Printed when a native QMD has an
// implausible grid, to show which commands wrote its grid words and where their data came from.
namespace VideoCommon::NS2CommandTrace {

/// How the GPU thread read a command list.
enum class ListSource : u32 {
    Live,          ///< Not captured at submission; read from guest memory now.
    Captured,      ///< Captured at submission and unchanged since.
    Reused,        ///< Changed, a later submission claimed the new contents: ran as submitted.
    Completed,     ///< Changed, nobody claimed the new contents: ran the current contents.
    GpuWritten,    ///< A GPU engine wrote the list memory: ran from memory.
    HostGpuWritten, ///< Host GPU (shader) data in the list memory: downloaded, ran it.
};

/// The GPU thread starts a command list of `words` words at `address` of `channel`.
/// `age_us`: time since submission (0 if unknown).
void NoteList(s32 channel, u64 address, u32 words, ListSource source, u64 age_us);

/// A method was called. `source` is the guest address of the argument (0 if none).
void NoteMethod(u32 subchannel, u32 method, u32 argument, u64 source);

/// A macro emitted a method (no guest address).
void NoteMacroEmit(u32 method, u32 argument);

/// The last `count` entries of the current channel, oldest first.
std::string Dump(u32 count);

/// The most recent write of `method` on the subchannel of the most recent `anchor_method` write
/// (or by a macro) before that anchor: guest address its argument was read from (0 for a macro
/// emit) and the argument.
bool LastWriteBefore(u32 anchor_method, u32 method, u64* source, u32* argument);

/// Channel of the list being processed.
s32 CurrentChannel();

} // namespace VideoCommon::NS2CommandTrace

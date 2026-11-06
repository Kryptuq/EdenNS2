// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Diagnostics only (yoshi-integration, yint-29): where the program address of a rejected native
// program came from. Every write of a 3D pipeline's program address (pipelines[i].reserved[0],
// the high byte, and reserved[1], the low word) is remembered twice: as parsed from the command
// buffer (GPU address of the word, and whether the entry ran from the submit-time copy or from
// memory) and as stored into the register (the macro that stored it, if any). Nothing here
// changes behaviour.

#include <array>
#include <chrono>
#include <mutex>
#include <optional>

#include "common/common_types.h"

namespace VideoCommon::NS2ProgramAddressTrace {

using Clock = std::chrono::steady_clock;

/// MAXWELL3D_REG_INDEX(pipelines) (checked by a static_assert in maxwell_3d.cpp).
inline constexpr u32 PipelineBase = 0x800;
inline constexpr u32 PipelineStride = 0x10; // 0x40 bytes per pipeline
inline constexpr u32 NumStages = 6;
inline constexpr u32 HighWord = 5;          // reserved[0]
inline constexpr u32 LowWord = 6;           // reserved[1]

inline bool IsProgramAddressMethod(u32 method) {
    if (method < PipelineBase || method >= PipelineBase + NumStages * PipelineStride) [[likely]] {
        return false;
    }
    const u32 word = (method - PipelineBase) % PipelineStride;
    return word == HighWord || word == LowWord;
}

/// Slot of a program address register: stage * 2 + (0 high, 1 low).
inline size_t Slot(u32 method) {
    const u32 relative = method - PipelineBase;
    return (relative / PipelineStride) * 2 + (relative % PipelineStride - HighWord);
}

struct Write {
    bool valid = false;
    u32 value = 0;
    u64 address = 0;              ///< command word GPU address (parsed) / engine segment (stored)
    const char* source = nullptr; ///< which copy of the entry was parsed (parsed writes)
    s32 channel = -1;
    u32 macro = 0;                ///< macro method that stored it (stored writes), 0 = none
    Clock::time_point time{};
};

struct Ring {
    std::array<Write, 16> writes{};
    u32 next = 0;
    void Add(const Write& write) {
        writes[next++ % writes.size()] = write;
    }
    /// Newest write of this value.
    std::optional<Write> Find(u32 value) const {
        for (u32 i = 0; i < writes.size(); ++i) {
            const Write& write = writes[(next + writes.size() - 1 - i) % writes.size()];
            if (write.valid && write.value == value) {
                return write;
            }
        }
        return std::nullopt;
    }
    u32 Count() const {
        u32 count = 0;
        for (const Write& write : writes) {
            count += write.valid ? 1 : 0;
        }
        return count;
    }
};

struct State {
    std::mutex mutex;
    std::array<Ring, NumStages * 2> parsed;
    std::array<Ring, NumStages * 2> stored;
};
inline State& GetState() {
    static State state;
    return state;
}

/// Which copy of the current command buffer entry is being parsed on this thread.
inline const char*& EntrySource() {
    thread_local const char* source = "unknown copy";
    return source;
}
/// The macro executing on this thread (its method), 0 = none.
inline u32& CurrentMacro() {
    thread_local u32 macro = 0;
    return macro;
}
struct MacroScope {
    u32 previous;
    explicit MacroScope(u32 macro) : previous{CurrentMacro()} {
        CurrentMacro() = macro;
    }
    ~MacroScope() {
        CurrentMacro() = previous;
    }
    MacroScope(const MacroScope&) = delete;
    MacroScope& operator=(const MacroScope&) = delete;
};

/// A 3D method parsed from a command buffer word (address 0: an inline header).
inline void NoteParsed(u32 method, u32 value, u64 word_address, s32 channel) {
    if (!IsProgramAddressMethod(method)) [[likely]] {
        return;
    }
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.parsed[Slot(method)].Add(
        Write{true, value, word_address, EntrySource(), channel, 0, Clock::now()});
}

/// A 3D register store.
inline void NoteStored(u32 method, u32 value, u64 segment) {
    if (!IsProgramAddressMethod(method)) [[likely]] {
        return;
    }
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.stored[Slot(method)].Add(
        Write{true, value, segment, nullptr, -1, CurrentMacro(), Clock::now()});
}

struct Provenance {
    std::optional<Write> parsed;
    std::optional<Write> stored;
    u32 parsed_count = 0;
};
inline Provenance Find(size_t stage, bool high, u32 value) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    const size_t slot = stage * 2 + (high ? 0 : 1);
    return Provenance{state.parsed[slot].Find(value), state.stored[slot].Find(value),
                      state.parsed[slot].Count()};
}

} // namespace VideoCommon::NS2ProgramAddressTrace

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>

#include <fmt/format.h>

#include "video_core/ns2_command_trace.h"

namespace VideoCommon::NS2CommandTrace {
namespace {
enum class Kind : u8 { Method, MacroEmit, List };

struct Entry {
    u64 source;
    u32 method;
    u32 argument;
    u32 extra;
    s32 channel;
    u8 subchannel;
    Kind kind;
};

constexpr size_t Size = 1024;
// Written by the GPU thread only; read on the same thread when an implausible grid is seen.
std::array<Entry, Size> g_entries{};
u64 g_next{};
s32 g_channel{-1};

void Push(const Entry& entry) {
    g_entries[g_next % Size] = entry;
    ++g_next;
}

const char* SourceName(u32 source) {
    switch (static_cast<ListSource>(source)) {
    case ListSource::Live: return "live (not captured)";
    case ListSource::Captured: return "captured, unchanged";
    case ListSource::Reused: return "changed, ran as submitted";
    case ListSource::Completed: return "changed, ran current contents";
    case ListSource::GpuWritten: return "GPU-written, ran from memory";
    case ListSource::HostGpuWritten: return "written by the host GPU, downloaded";
    case ListSource::Arguments: return "method arguments, ran from memory";
    }
    return "?";
}
} // namespace

void NoteList(s32 channel, u64 address, u32 words, ListSource source, u64 age_us) {
    g_channel = channel;
    Push({address, words, static_cast<u32>(source),
          static_cast<u32>(std::min<u64>(age_us, 0xffffffffULL)), channel, 0, Kind::List});
}

void NoteMethod(u32 subchannel, u32 method, u32 argument, u64 source) {
    Push({source, method, argument, 0, g_channel, static_cast<u8>(subchannel), Kind::Method});
}

void NoteMacroEmit(u32 method, u32 argument) {
    Push({0, method, argument, 0, g_channel, 0xff, Kind::MacroEmit});
}

s32 CurrentChannel() {
    return g_channel;
}

std::string Dump(u32 count) {
    std::string out;
    const u64 available = std::min<u64>(g_next, Size);
    u64 printed = 0;
    std::array<u64, Size> picked{};
    for (u64 i = 0; i < available && printed < count; ++i) {
        const Entry& entry = g_entries[(g_next - 1 - i) % Size];
        if (entry.channel != g_channel) continue;
        picked[printed++] = g_next - 1 - i;
    }
    for (u64 i = printed; i-- > 0;) {
        const Entry& entry = g_entries[picked[i] % Size];
        switch (entry.kind) {
        case Kind::List:
            out += fmt::format("\n  list {:#x} ({} words, {}, {} us after submission)", entry.source,
                               entry.method, SourceName(entry.argument), entry.extra);
            break;
        case Kind::Method:
            out += fmt::format("\n    sub{} m{:#05x} = {:08x} @ {:#x}", entry.subchannel,
                               entry.method, entry.argument, entry.source);
            break;
        case Kind::MacroEmit:
            out += fmt::format("\n    macro m{:#05x} = {:08x}", entry.method, entry.argument);
            break;
        }
    }
    return out;
}

bool LastWriteBefore(u32 anchor_method, u32 method, u64* source, u32* argument) {
    const u64 available = std::min<u64>(g_next, Size);
    u64 i = 0;
    u32 subchannel = 0xff;
    for (; i < available; ++i) {
        const Entry& entry = g_entries[(g_next - 1 - i) % Size];
        if (entry.channel == g_channel && entry.kind != Kind::List && entry.method == anchor_method) {
            subchannel = entry.subchannel;
            break;
        }
    }
    for (++i; i < available; ++i) {
        const Entry& entry = g_entries[(g_next - 1 - i) % Size];
        if (entry.channel != g_channel || entry.kind == Kind::List || entry.method != method) continue;
        if (entry.kind == Kind::MacroEmit || entry.subchannel == subchannel || subchannel == 0xff) {
            *source = entry.kind == Kind::Method ? entry.source : 0;
            *argument = entry.argument;
            return true;
        }
    }
    return false;
}

} // namespace VideoCommon::NS2CommandTrace

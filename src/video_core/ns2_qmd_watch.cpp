// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <deque>
#include <map>
#include <mutex>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/ns2_qmd_watch.h"

namespace VideoCommon::NS2QmdWatch {
namespace {
constexpr u64 GridOffset = 12 * 4;
constexpr u64 GridSize = 3 * 4;
constexpr size_t HistoryDepth = 24;

struct Event {
    u64 time_us;
    bool write;
    bool dispatched;
    const char* source;
    u64 address;
    u64 size;
    u64 extra;
    bool source_gpu_modified;
    std::array<u32, 3> grid;
};

struct Qmd {
    std::deque<Event> events;
    std::array<u32, 3> last_read_grid{};
    bool has_read = false;
    bool write_since_read = false;
};

std::mutex g_mutex;
std::map<u64, Qmd> g_qmds;
u32 g_logged_writes = 0;
u32 g_logged_cpu_changes = 0;

u64 NowUs() {
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

void Push(Qmd& qmd, const Event& event) {
    qmd.events.push_back(event);
    if (qmd.events.size() > HistoryDepth) qmd.events.pop_front();
}
} // Anonymous namespace

void NoteRead(u64 address, const std::array<u32, 3>& grid, bool dispatched) {
    std::scoped_lock lock{g_mutex};
    if (g_qmds.size() >= 16384 && !g_qmds.contains(address)) {
        g_qmds.clear();
    }
    Qmd& qmd = g_qmds[address];
    if (qmd.has_read && qmd.last_read_grid != grid && !qmd.write_since_read && g_logged_cpu_changes < 16) {
        ++g_logged_cpu_changes;
        LOG_INFO(HW_GPU,
                 "NS2 QMD watch: QMD {:#x} grid {}x{}x{} -> {}x{}x{} without a GPU engine write in between "
                 "(written by the guest CPU)",
                 address, qmd.last_read_grid[0], qmd.last_read_grid[1] & 0xffff, qmd.last_read_grid[2] & 0xffff,
                 grid[0], grid[1] & 0xffff, grid[2] & 0xffff);
    }
    qmd.last_read_grid = grid;
    qmd.has_read = true;
    qmd.write_since_read = false;
    Push(qmd, Event{NowUs(), false, dispatched, dispatched ? "run" : "read", address, 0, 0, false, grid});
}

bool TouchesGrid(u64 address, u64 size, u64* qmd_out, u32 max_qmds) {
    std::scoped_lock lock{g_mutex};
    if (g_qmds.empty() || size == 0) return false;
    // QMDs whose grid [q + 0x30, q + 0x3c) intersects [address, address + size).
    auto it = g_qmds.lower_bound(address >= GridOffset + GridSize ? address - GridOffset - GridSize + 1 : 0);
    u32 found = 0;
    for (; it != g_qmds.end() && it->first + GridOffset < address + size && found < max_qmds; ++it) {
        if (it->first + GridOffset + GridSize > address) qmd_out[found++] = it->first;
    }
    return found != 0;
}

void RecordWrite(u64 qmd_address, const char* source, u64 address, u64 size, u64 extra,
                 bool source_gpu_modified, const std::array<u32, 3>& guest_grid) {
    std::scoped_lock lock{g_mutex};
    const auto it = g_qmds.find(qmd_address);
    if (it == g_qmds.end()) return;
    it->second.write_since_read = true;
    Push(it->second, Event{NowUs(), true, false, source, address, size, extra, source_gpu_modified, guest_grid});
    if (g_logged_writes < 16) {
        ++g_logged_writes;
        LOG_INFO(HW_GPU,
                 "NS2 QMD watch: {} wrote [{:#x}, +{:#x}) over the grid of QMD {:#x} (extra {:#x}{}); grid in "
                 "guest memory now {:08x},{:08x},{:08x}",
                 source, address, size, qmd_address, extra,
                 source_gpu_modified ? ", source written by the host GPU and not downloaded" : "",
                 guest_grid[0], guest_grid[1], guest_grid[2]);
    }
}

std::string History(u64 address) {
    std::scoped_lock lock{g_mutex};
    const auto it = g_qmds.find(address);
    if (it == g_qmds.end()) return {};
    std::string text;
    const u64 last = it->second.events.empty() ? 0 : it->second.events.back().time_us;
    for (const Event& event : it->second.events) {
        if (event.write) {
            text += fmt::format("\n  -{} us: {} wrote [{:#x}, +{:#x}) extra {:#x}{}, guest grid {:08x},{:08x},{:08x}",
                                last - event.time_us, event.source, event.address, event.size, event.extra,
                                event.source_gpu_modified ? " (source GPU-written)" : "", event.grid[0],
                                event.grid[1], event.grid[2]);
        } else {
            text += fmt::format("\n  -{} us: {} grid {:08x},{:08x},{:08x}", last - event.time_us, event.source,
                                event.grid[0], event.grid[1], event.grid[2]);
        }
    }
    return text;
}

} // namespace VideoCommon::NS2QmdWatch

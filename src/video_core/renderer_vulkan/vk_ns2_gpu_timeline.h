// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// Diagnostics for Switch 2 titles: host GPU timestamps around every native draw and dispatch.
/// The NS2 watchdog thread polls them (Common::NS2Watch::RunGpuPoll) to report
///  - native work the host GPU started or queued but has not finished for a long time (hangs),
///    before Windows resets the driver, and
///  - how long dispatches take on the host GPU (slow emulated shaders).
/// It only adds timestamp writes; rendering is unchanged. EDEN_NS2_GPU_TIMING=0 turns it off.
class NS2GpuTimeline {
public:
    enum class Kind : u32 { Draw = 0, Dispatch = 1 };

    /// Returns nullptr when disabled or unsupported by the host device.
    static std::unique_ptr<NS2GpuTimeline> Create(const Device& device, Scheduler& scheduler);

    NS2GpuTimeline(const Device& device, Scheduler& scheduler, vk::QueryPool pool);
    ~NS2GpuTimeline();

    NS2GpuTimeline(const NS2GpuTimeline&) = delete;
    NS2GpuTimeline& operator=(const NS2GpuTimeline&) = delete;

    /// Records a timestamp before the work. Returns an id for End, 0 when not recorded.
    u64 Begin(Kind kind, const u64 (&shaders)[6], std::array<u32, 3> grid, bool indirect);
    /// Records a timestamp after the work started with Begin.
    void End(u64 id);

    /// Watchdog thread: reads finished timestamps and reports hangs and slow work.
    void Poll();

private:
    static constexpr u32 NumRecords = 65536;
    // Queries are reset on the host in chunks, once every slot of a chunk is done on the GPU.
    static constexpr u32 ChunkSize = 1024;
    static constexpr u32 NumChunks = NumRecords / ChunkSize;

    struct Record {
        u64 id{};
        u64 tick{};
        Kind kind{};
        bool indirect{};
        bool ended_recorded{};
        bool reported{};
        std::array<u32, 3> grid{};
        u64 shaders[6]{};
    };

    struct Totals {
        double total_ms{};
        double max_ms{};
        u64 count{};
        Kind kind{};
    };

    void Account(const Record& record, double ms);
    void ReportStuck(const Record& record, bool begun, u64 unfinished_ms, const char* what);
    void LogSummary(u64 now_ms);

    const Device& device;
    Scheduler& scheduler;
    vk::QueryPool pool;
    double period_ns;

    std::mutex mutex;
    std::vector<Record> records;
    std::array<u64, NumChunks> chunk_max_tick{};
    std::array<bool, NumChunks> chunk_usable{};
    std::vector<u64> results;
    u64 next_id = 1;
    u64 poll_id = 1;

    u64 stuck_id = 0;
    u64 stuck_since_ms = 0;
    bool device_lost = false;
    bool stuck_reported = false;
    bool fault_logged = false;
    u32 slow_logged = 0;

    std::unordered_map<u64, Totals> totals;
    u64 summary_start_ms = 0;
    u64 last_finished_id = 0;
    Record last_finished{};
};

} // namespace Vulkan

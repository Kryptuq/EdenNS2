// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/ns2_watch.h"
#include "video_core/renderer_vulkan/vk_ns2_gpu_timeline.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {
// Native work unfinished for this long is reported (Windows resets the driver after ~2 s).
constexpr u64 StuckReportMs = 750;
// Single draws/dispatches at least this long are logged individually (capped).
constexpr double SlowMs = 50.0;
constexpr u32 MaxSlowLines = 400;
constexpr u64 SummaryIntervalMs = 10'000;

std::string Describe(NS2GpuTimeline::Kind kind, const u64 (&shaders)[6], std::array<u32, 3> grid,
                     bool indirect) {
    if (kind == NS2GpuTimeline::Kind::Dispatch) {
        if (indirect) {
            return fmt::format("dispatch shader={:#x} (indirect)", shaders[0]);
        }
        return fmt::format("dispatch shader={:#x} grid={}x{}x{}", shaders[0], grid[0], grid[1],
                           grid[2]);
    }
    return fmt::format("draw shaders={:#x},{:#x},{:#x},{:#x},{:#x},{:#x}", shaders[0], shaders[1],
                       shaders[2], shaders[3], shaders[4], shaders[5]);
}

u64 MainShader(NS2GpuTimeline::Kind kind, const u64 (&shaders)[6]) {
    if (kind == NS2GpuTimeline::Kind::Dispatch) {
        return shaders[0];
    }
    // Fragment stage first (index 4), then the last vertex stage present.
    if (shaders[4] != 0) {
        return shaders[4];
    }
    for (size_t i = 6; i-- > 0;) {
        if (shaders[i] != 0) {
            return shaders[i];
        }
    }
    return 0;
}
} // namespace

std::unique_ptr<NS2GpuTimeline> NS2GpuTimeline::Create(const Device& device, Scheduler& scheduler) {
    if (const char* value = std::getenv("EDEN_NS2_GPU_TIMING"); value && std::strcmp(value, "0") == 0) {
        LOG_INFO(Render_Vulkan, "NS2 GPU timing: disabled by EDEN_NS2_GPU_TIMING=0");
        return nullptr;
    }
    if (!device.IsHostQueryResetSupported() || !device.SupportsGraphicsComputeTimestamps() ||
        device.GetTimestampPeriod() <= 0.0f) {
        LOG_WARNING(Render_Vulkan, "NS2 GPU timing: unavailable (host query reset {}, timestamps {})",
                    device.IsHostQueryResetSupported(), device.SupportsGraphicsComputeTimestamps());
        return nullptr;
    }
    vk::QueryPool pool = device.GetLogical().CreateQueryPool({
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = NumRecords * 2,
        .pipelineStatistics = 0,
    });
    device.GetLogical().ResetQueryPool(*pool, 0, NumRecords * 2);
    LOG_INFO(Render_Vulkan,
             "NS2 GPU timing: enabled ({} records, {} ns per tick); reports native work unfinished "
             "for {} ms and dispatches/draws of {} ms or more",
             NumRecords, device.GetTimestampPeriod(), StuckReportMs, SlowMs);
    return std::make_unique<NS2GpuTimeline>(device, scheduler, std::move(pool));
}

NS2GpuTimeline::NS2GpuTimeline(const Device& device_, Scheduler& scheduler_, vk::QueryPool pool_)
    : device{device_}, scheduler{scheduler_}, pool{std::move(pool_)},
      period_ns{static_cast<double>(device_.GetTimestampPeriod())}, records(NumRecords),
      summary_start_ms{Common::NS2Watch::NowMs()} {
    // The whole pool was reset at creation, so every chunk starts out usable.
    chunk_usable.fill(true);
    Common::NS2Watch::SetGpuPoll([this] { Poll(); });
}

NS2GpuTimeline::~NS2GpuTimeline() {
    // Waits for a poll in progress, so the watchdog never touches a destroyed timeline.
    Common::NS2Watch::SetGpuPoll({});
}

u64 NS2GpuTimeline::Begin(Kind kind, const u64 (&shaders)[6], std::array<u32, 3> grid,
                          bool indirect) {
    u32 query{};
    u64 id{};
    {
        std::scoped_lock lock{mutex};
        id = next_id++;
        const u32 slot = static_cast<u32>(id % NumRecords);
        const u32 chunk = slot / ChunkSize;
        if (slot % ChunkSize == 0) {
            // Entering a chunk: its queries may only be reset once the host GPU is done with
            // every timestamp written into it during the previous pass.
            chunk_usable[chunk] = scheduler.IsFree(chunk_max_tick[chunk]);
            if (chunk_usable[chunk]) {
                device.GetLogical().ResetQueryPool(*pool, slot * 2, ChunkSize * 2);
                chunk_max_tick[chunk] = 0;
            }
        }
        Record& record = records[slot];
        record = Record{};
        if (!chunk_usable[chunk]) {
            return 0;
        }
        query = slot * 2;
        record.id = id;
        record.tick = scheduler.CurrentTick();
        record.kind = kind;
        record.indirect = indirect;
        record.grid = grid;
        std::memcpy(record.shaders, shaders, sizeof(record.shaders));
        chunk_max_tick[chunk] = (std::max)(chunk_max_tick[chunk], record.tick);
    }
    scheduler.Record([query_pool = *pool, query](vk::CommandBuffer cmdbuf) {
        cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_pool, query);
    });
    return id;
}

void NS2GpuTimeline::End(u64 id) {
    if (id == 0) {
        return;
    }
    const u32 slot = static_cast<u32>(id % NumRecords);
    {
        std::scoped_lock lock{mutex};
        Record& record = records[slot];
        if (record.id != id) {
            return;
        }
        // The end timestamp may land in a later submission than the begin timestamp.
        record.tick = scheduler.CurrentTick();
        record.ended_recorded = true;
        u64& chunk_tick = chunk_max_tick[slot / ChunkSize];
        chunk_tick = (std::max)(chunk_tick, record.tick);
    }
    scheduler.Record([query_pool = *pool, query = slot * 2 + 1](vk::CommandBuffer cmdbuf) {
        cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool, query);
    });
}

void NS2GpuTimeline::Poll() {
    std::scoped_lock lock{mutex};
    if (device_lost) {
        return;
    }
    const u64 now = Common::NS2Watch::NowMs();
    if (next_id > NumRecords && poll_id < next_id - NumRecords) {
        poll_id = next_id - NumRecords;
    }
    // Read the timestamps of every pending record at once (at most two ranges of the ring).
    const u64 pending = next_id - poll_id;
    if (pending == 0) {
        if (now - summary_start_ms >= SummaryIntervalMs) {
            LogSummary(now);
        }
        return;
    }
    results.assign(pending * 4, 0);
    const u32 first_slot = static_cast<u32>(poll_id % NumRecords);
    const u64 first_count = (std::min<u64>)(pending, NumRecords - first_slot);
    const VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
    VkResult result = device.GetLogical().GetQueryResults(
        *pool, first_slot * 2, static_cast<u32>(first_count * 2), first_count * 4 * sizeof(u64),
        results.data(), sizeof(u64) * 2, flags);
    if (result != VK_ERROR_DEVICE_LOST && pending > first_count) {
        const u64 rest = pending - first_count;
        result = device.GetLogical().GetQueryResults(*pool, 0, static_cast<u32>(rest * 2),
                                                     rest * 4 * sizeof(u64),
                                                     results.data() + first_count * 4,
                                                     sizeof(u64) * 2, flags);
    }
    if (result == VK_ERROR_DEVICE_LOST) {
        device_lost = true;
        for (u64 id = poll_id; id < next_id; ++id) {
            const Record& record = records[id % NumRecords];
            if (record.id == id) {
                ReportStuck(record, false, stuck_id == id ? now - stuck_since_ms : 0,
                            "host GPU device lost; oldest native work not seen finished");
                break;
            }
        }
        return;
    }
    while (poll_id < next_id) {
        Record& record = records[poll_id % NumRecords];
        if (record.id != poll_id) {
            ++poll_id;
            continue;
        }
        const u64* data = results.data() + (pending - (next_id - poll_id)) * 4;
        const bool begun = data[1] != 0;
        const bool ended = data[3] != 0;
        if (ended) {
            if (begun && data[2] >= data[0]) {
                Account(record, static_cast<double>(data[2] - data[0]) * period_ns / 1'000'000.0);
            }
            if (record.reported) {
                LOG_ERROR(Render_Vulkan, "NS2 GPU: previously reported work #{} finished after {} ms: {}",
                          record.id, now - stuck_since_ms,
                          Describe(record.kind, record.shaders, record.grid, record.indirect));
            }
            last_finished_id = record.id;
            last_finished = record;
            ++poll_id;
            continue;
        }
        if (scheduler.IsFree(record.tick)) {
            // Its submission completed without an end timestamp to read (End was never
            // recorded because the work was abandoned); nothing to measure.
            ++poll_id;
            continue;
        }
        if (!record.ended_recorded || record.tick >= scheduler.CurrentTick()) {
            // Still being recorded, or its command buffer has not been submitted yet: the host
            // GPU cannot be blamed for it.
            stuck_id = 0;
            break;
        }
        // Oldest native work the host GPU has not finished.
        if (stuck_id != record.id) {
            stuck_id = record.id;
            stuck_since_ms = now;
        } else if (!record.reported && now - stuck_since_ms >= StuckReportMs) {
            record.reported = true;
            ReportStuck(record, begun, now - stuck_since_ms, "host GPU has not finished native work");
        }
        break;
    }
    if (now - summary_start_ms >= SummaryIntervalMs) {
        LogSummary(now);
    }
}

void NS2GpuTimeline::Account(const Record& record, double ms) {
    Totals& entry = totals[MainShader(record.kind, record.shaders)];
    entry.total_ms += ms;
    entry.max_ms = (std::max)(entry.max_ms, ms);
    entry.kind = record.kind;
    ++entry.count;
    if (ms >= SlowMs && slow_logged < MaxSlowLines) {
        ++slow_logged;
        LOG_WARNING(Render_Vulkan, "NS2 GPU time: {} took {:.1f} ms (work #{})",
                    Describe(record.kind, record.shaders, record.grid, record.indirect), ms, record.id);
    }
}

void NS2GpuTimeline::ReportStuck(const Record& record, bool begun, u64 unfinished_ms,
                                 const char* what) {
    // Errors flush the log file immediately, so these lines survive a driver reset or crash.
    LOG_ERROR(Render_Vulkan, "NS2 GPU: {}: work #{} {} (GPU {} it, unfinished for {} ms, host tick {:#x})",
              what, record.id, Describe(record.kind, record.shaders, record.grid, record.indirect),
              begun ? "started" : "has not reached", unfinished_ms, record.tick);
    if (last_finished_id != 0) {
        LOG_ERROR(Render_Vulkan, "NS2 GPU:   last finished native work #{}: {}", last_finished_id,
                  Describe(last_finished.kind, last_finished.shaders, last_finished.grid,
                           last_finished.indirect));
    }
    // The next native work queued behind it, for context.
    u32 listed = 0;
    for (u64 id = record.id + 1; id < next_id && listed < 6; ++id) {
        const Record& next = records[id % NumRecords];
        if (next.id != id) {
            continue;
        }
        ++listed;
        LOG_ERROR(Render_Vulkan, "NS2 GPU:   queued after it #{}: {}", id,
                  Describe(next.kind, next.shaders, next.grid, next.indirect));
    }
}

void NS2GpuTimeline::LogSummary(u64 now) {
    if (!totals.empty()) {
        std::vector<std::pair<u64, Totals>> sorted(totals.begin(), totals.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second.total_ms > b.second.total_ms; });
        double all{};
        for (const auto& [shader, entry] : sorted) {
            all += entry.total_ms;
        }
        LOG_INFO(Render_Vulkan, "NS2 GPU time, last {} s: {:.0f} ms of native work in total; top shaders:",
                 (now - summary_start_ms) / 1000, all);
        for (size_t i = 0; i < (std::min<size_t>)(sorted.size(), 6); ++i) {
            const auto& [shader, entry] = sorted[i];
            LOG_INFO(Render_Vulkan, "NS2 GPU time:   {} {:#x}: {:.0f} ms in {} runs (max {:.1f} ms)",
                     entry.kind == Kind::Dispatch ? "dispatch" : "draw", shader, entry.total_ms,
                     entry.count, entry.max_ms);
        }
    }
    totals.clear();
    summary_start_ms = now;
}

} // namespace Vulkan

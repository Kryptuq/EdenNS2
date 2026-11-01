// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "common/logging.h"
#include "common/ns2_watch.h"
#include "common/thread.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/global_scheduler_context.h"
#include "core/hle/kernel/k_handle_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "core/ns2_watchdog.h"

namespace Core {

namespace {
// Seconds without a new frame before each report (per stall).
constexpr u64 ReportAfterMs[] = {15'000, 45'000, 120'000};
} // namespace

NS2Watchdog::NS2Watchdog(System& system) : m_system{system} {
    m_thread = std::jthread([this](std::stop_token token) { Run(token); });
}

NS2Watchdog::~NS2Watchdog() {
    m_thread.request_stop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void NS2Watchdog::Run(std::stop_token stop_token) {
    Common::SetCurrentThreadName("NS2Watchdog");
    std::mutex mutex;
    std::condition_variable_any cv;

    u64 last_presented = Common::NS2Watch::g_present_count.load();
    u64 stall_start = Common::NS2Watch::NowMs();
    size_t reports = 0;
    // Periodic snapshots of the guest threads even while frames are presented, to see what
    // worker threads (loading, media) are waiting on.
    const u64 started = stall_start;
    u64 next_snapshot = started + 50'000;
    u32 snapshots = 0;

    while (!stop_token.stop_requested()) {
        {
            std::unique_lock lk{mutex};
            cv.wait_for(lk, stop_token, std::chrono::milliseconds{500}, [] { return false; });
        }
        if (stop_token.stop_requested() || !m_system.IsPoweredOn()) {
            break;
        }
        const u64 presented = Common::NS2Watch::g_present_count.load();
        const u64 now = Common::NS2Watch::NowMs();
        if (snapshots < 5 && now >= next_snapshot) {
            ++snapshots;
            next_snapshot = now + 20'000;
            LOG_WARNING(Core, "NS2 watchdog: periodic thread snapshot {} at {} s", snapshots, (now - started) / 1000);
            Dump(0);
        }
        if (presented != last_presented) {
            if (reports != 0) {
                LOG_WARNING(Core, "NS2 watchdog: frames resumed after {} ms (presented={})",
                            now - stall_start, presented);
            }
            last_presented = presented;
            stall_start = now;
            reports = 0;
            continue;
        }
        if (reports < std::size(ReportAfterMs) && now - stall_start >= ReportAfterMs[reports]) {
            ++reports;
            Dump(now - stall_start);
        }
    }
}

void NS2Watchdog::Dump(u64 stalled_ms) {
    LOG_WARNING(Core, "NS2 watchdog: no new frame for {} s (queued={}, composited={})",
                stalled_ms / 1000, Common::NS2Watch::g_present_count.load(),
                Common::NS2Watch::g_composite_count.load());

    u64 oldest_waited_tick = ~0ULL;
    for (const auto& phase : Common::NS2Watch::CollectPhases(1000)) {
        LOG_WARNING(Core, "NS2 watchdog: host thread '{}' busy for {} ms: {} {} value={:#x}",
                    phase.thread, phase.age_ms, phase.label, phase.detail, phase.value);
        if (std::string_view{phase.label} == "waiting for host GPU tick") {
            oldest_waited_tick = (std::min)(oldest_waited_tick, phase.value);
        }
    }
    if (oldest_waited_tick != ~0ULL) {
        // The host GPU never finished this submission: list the work recorded into it (and the
        // one before, for context).
        for (const auto& work : Common::NS2Watch::CollectWork()) {
            if (work.tick + 1 < oldest_waited_tick) continue;
            LOG_WARNING(Core, "NS2 watchdog: tick {:#x} {} x{} shaders {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}",
                        work.tick, work.kind == 0 ? "draw" : "dispatch", work.count, work.shaders[0],
                        work.shaders[1], work.shaders[2], work.shaders[3], work.shaders[4], work.shaders[5]);
        }
    }

    Kernel::KProcess* process = m_system.ApplicationProcess();
    if (!process) {
        LOG_WARNING(Core, "NS2 watchdog: no application process");
        return;
    }

    // Copy the list first; this is diagnostic code running while the guest is presumed stuck.
    const auto& live_list = m_system.Kernel().GlobalSchedulerContext().GetThreadList();
    std::vector<Kernel::KThread*> threads(live_list.begin(), live_list.end());

    auto& memory = process->GetMemory();
    const bool dump_code = stalled_ms == 0 && !m_code_dumped;
    for (Kernel::KThread* thread : threads) {
        if (!thread || thread->GetOwnerProcess() != process) {
            continue;
        }
        const auto& ctx = thread->GetContext();
        const std::string name = GetThreadName(thread).value_or("?");
        LOG_WARNING(Core,
                    "NS2 watchdog: guest thread id={} name='{}' state={} wait={} prio={} core={} "
                    "pc={:#x} lr={:#x} sp={:#x} x0={:#x} x1={:#x} x2={:#x}",
                    thread->GetThreadId(), name, GetThreadState(thread),
                    GetThreadWaitReason(thread), thread->GetPriority(), thread->GetActiveCore(),
                    ctx.pc, ctx.lr, ctx.sp, ctx.r[0], ctx.r[1], ctx.r[2]);

        // Objects behind handle-like register values and a possible handle array at x1 (as for
        // WaitSynchronization), to tell what a waiting thread waits on.
        if (thread->GetState() == Kernel::ThreadState::Waiting || name.find("Video") != std::string::npos ||
            name.find("Movie") != std::string::npos) {
            std::string objects;
            const auto describe = [&](Kernel::Handle handle) {
                if (handle == 0 || handle > 0xffffff) return;
                auto object = process->GetHandleTable().GetObjectWithoutPseudoHandle<Kernel::KAutoObject>(
                    m_system.Kernel(), handle);
                if (object.IsNull()) return;
                objects += fmt::format(" {:#x}={}@{}", handle, object->GetTypeName(),
                                       fmt::ptr(object.GetPointerUnsafe()));
                if (auto* readable = object->DynamicCast<Kernel::KReadableEvent*>()) {
                    objects += fmt::format("('{}', signaled={})",
                                           Service::KernelHelpers::EventNameForDebugging(readable->GetParent()),
                                           readable->IsSignaled(m_system.Kernel()));
                }
            };
            for (int r = 0; r < 4; ++r) describe(static_cast<Kernel::Handle>(ctx.r[r]));
            if (ctx.r[1] && memory.IsValidVirtualAddressRange(ctx.r[1], 16)) {
                for (int i = 0; i < 4; ++i) describe(memory.Read32(ctx.r[1] + i * 4));
            }
            if (!objects.empty()) LOG_WARNING(Core, "NS2 watchdog:   objects{}", objects);
        }

        // Frame-pointer walk (raw addresses; module bases are logged at load time).
        std::string chain;
        u64 fp = ctx.fp;
        for (int i = 0; i < 24; ++i) {
            if (!fp || (fp % 8) != 0 || !memory.IsValidVirtualAddressRange(fp, 16)) {
                break;
            }
            const u64 ret = memory.Read64(fp + 8);
            const u64 next = memory.Read64(fp);
            chain += fmt::format("{:#x},", ret);
            if (next <= fp) {
                break;
            }
            fp = next;
        }
        if (!chain.empty()) {
            chain.pop_back();
            LOG_WARNING(Core, "NS2 watchdog:   call chain {}", chain);
        }
        // Once, for the media threads: the code around each return address and the top of the
        // stack, so the wait loops can be disassembled from the log.
        if (stalled_ms == 0 && !m_code_dumped &&
            (name.find("Video") != std::string::npos || name.find("Movie") != std::string::npos)) {
            const auto dump = [&](const char* what, u64 address, u64 size) {
                if (!memory.IsValidVirtualAddressRange(address, size)) return;
                for (u64 offset = 0; offset < size; offset += 64) {
                    std::string line;
                    for (u64 k = 0; k < 64; k += 4) line += fmt::format(" {:08x}", memory.Read32(address + offset + k));
                    LOG_WARNING(Core, "NS2 code {} {} {:#x}:{}", name, what, address + offset, line);
                }
            };
            dump("pc", (ctx.pc & ~0x3fULL) - 0x100, 0x200);
            u64 frame = ctx.fp;
            for (int i = 0; i < 8; ++i) {
                if (!frame || (frame % 8) != 0 || !memory.IsValidVirtualAddressRange(frame, 16)) break;
                const u64 ret = memory.Read64(frame + 8);
                dump("ret", (ret & ~0x3fULL) - 0x600, 0x800);
                const u64 next = memory.Read64(frame);
                if (next <= frame) break;
                frame = next;
            }
            dump("stack", ctx.sp & ~0x3fULL, 0x400);
            std::string regs;
            for (int r = 0; r < 31; ++r) regs += fmt::format(" x{}={:#x}", r, ctx.r[r]);
            LOG_WARNING(Core, "NS2 code {} regs:{}", name, regs);
        }
    }
    if (dump_code) m_code_dumped = true;
}

} // namespace Core

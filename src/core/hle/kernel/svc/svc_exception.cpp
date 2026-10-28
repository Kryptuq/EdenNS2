// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cctype>
#include <string>
#include <vector>
#include "core/core.h"
#include "core/debugger/debugger.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/svc_types.h"
#include "core/memory.h"
#include "core/reporter.h"

namespace Kernel::Svc {

/// Break program execution
namespace {
// Diagnostic: titles built without reliable frame records leave the regular backtrace
// incomplete, so log every stack word that looks like a code address.
void LogStackCodePointers(Core::System& system) {
    auto* process = GetCurrentProcessPointer(system.Kernel());
    if (!process) {
        return;
    }
    auto* arm = process->GetArmInterface(system.CurrentPhysicalCore().CoreIndex());
    if (!arm) {
        return;
    }
    Svc::ThreadContext ctx{};
    arm->GetContext(ctx);
    constexpr size_t Words = 2048;
    std::vector<u64> stack(Words);
    if (!process->GetMemory().ReadBlock(ctx.sp, stack.data(), Words * sizeof(u64))) {
        LOG_ERROR(Debug_Emulated, "Stack dump: could not read sp={:#x}", ctx.sp);
        return;
    }
    // Loaded modules start at the entry point; 512 MiB covers all of them.
    const u64 code_start = GetInteger(process->GetEntryPoint());
    const u64 code_end = code_start + 0x20000000;
    LOG_ERROR(Debug_Emulated, "Stack dump: sp={:#x} code={:#x}-{:#x}", ctx.sp, code_start, code_end);
    std::string line;
    size_t count = 0;
    for (size_t i = 0; i < Words; ++i) {
        if (stack[i] >= code_start && stack[i] < code_end) {
            line += fmt::format(" +{:x}={:x}", i * 8, stack[i]);
            if (++count % 8 == 0) {
                LOG_ERROR(Debug_Emulated, "Stack code pointers:{}", line);
                line.clear();
            }
        }
    }
    if (!line.empty()) {
        LOG_ERROR(Debug_Emulated, "Stack code pointers:{}", line);
    }
}
} // namespace

void Break(Core::System& system, BreakReason reason, u64 info1, u64 info2) {
    BreakReason break_reason =
        reason & static_cast<BreakReason>(~BreakReason::NotificationOnlyFlag);
    bool notification_only = True(reason & BreakReason::NotificationOnlyFlag);

    bool has_dumped_buffer{};
    std::vector<u8> debug_buffer;

    const auto handle_debug_buffer = [&](u64 addr, u64 sz) {
        if (sz == 0 || addr == 0 || has_dumped_buffer) {
            return;
        }

        auto& memory = GetCurrentMemory(system.Kernel());

        // This typically is an error code so we're going to assume this is the case
        if (sz == sizeof(u32)) {
            LOG_CRITICAL(Debug_Emulated, "debug_buffer_err_code={:X}", memory.Read32(addr));
        } else {
            // We don't know what's in here so we'll hexdump it
            debug_buffer.resize(sz);
            memory.ReadBlock(addr, debug_buffer.data(), sz);
            std::string hexdump;
            for (std::size_t i = 0; i < debug_buffer.size(); i++) {
                hexdump += fmt::format("{:02X} ", debug_buffer[i]);
                if ((i + 1) % 32 == 0) {
                    hexdump += '\n';
                }
            }
            LOG_CRITICAL(Debug_Emulated, "debug_buffer=\n{}", hexdump);
        }
        has_dumped_buffer = true;
    };
    switch (break_reason) {
    case BreakReason::Panic:
        LOG_CRITICAL(Debug_Emulated, "Userspace PANIC! info1={:#016x}, info2={:#016x}", info1,
                     info2);
        handle_debug_buffer(info1, info2);
        break;
    case BreakReason::Assert:
        LOG_CRITICAL(Debug_Emulated, "Userspace Assertion failed! info1={:#016x}, info2={:#016x}",
                     info1, info2);
        handle_debug_buffer(info1, info2);
        break;
    case BreakReason::User:
        LOG_WARNING(Debug_Emulated, "Userspace Break! {:#016x} with size {:#016x}", info1, info2);
        handle_debug_buffer(info1, info2);
        break;
    case BreakReason::PreLoadDll:
        LOG_INFO(Debug_Emulated,
                 "Userspace Attempting to load an NRO at {:#016x} with size {:#016x}", info1,
                 info2);
        break;
    case BreakReason::PostLoadDll:
        LOG_INFO(Debug_Emulated, "Userspace Loaded an NRO at {:#016x} with size {:#016x}", info1,
                 info2);
        break;
    case BreakReason::PreUnloadDll:
        LOG_INFO(Debug_Emulated,
                 "Userspace Attempting to unload an NRO at {:#016x} with size {:#016x}", info1,
                 info2);
        break;
    case BreakReason::PostUnloadDll:
        LOG_INFO(Debug_Emulated, "Userspace Unloaded an NRO at {:#016x} with size {:#016x}",
                 info1, info2);
        break;
    case BreakReason::CppException:
        LOG_CRITICAL(Debug_Emulated, "Signalling debugger. Uncaught C++ exception encountered.");
        break;
    default:
        LOG_WARNING(
            Debug_Emulated,
            "Signalling debugger, Unknown break reason {:#x}, info1={:#016x}, info2={:#016x}",
            reason, info1, info2);
        handle_debug_buffer(info1, info2);
        break;
    }

    system.GetReporter().SaveSvcBreakReport(
        static_cast<u32>(reason), notification_only, info1, info2,
        has_dumped_buffer ? std::make_optional(debug_buffer) : std::nullopt);

    if (!notification_only) {
        LOG_CRITICAL(
            Debug_Emulated,
            "Emulated program broke execution! reason={:#016x}, info1={:#016x}, info2={:#016x}",
            reason, info1, info2);

        handle_debug_buffer(info1, info2);

        system.CurrentPhysicalCore().LogBacktrace(system.Kernel());
        LogStackCodePointers(system);
    }

    const bool should_break = !notification_only;
    if (system.DebuggerEnabled() && should_break) {
        auto* thread = system.Kernel().GetCurrentEmuThread();
        system.GetDebugger().NotifyThreadStopped(thread);
        thread->RequestSuspend(system.Kernel(), Kernel::SuspendType::Debug);
    }
}

void ReturnFromException(Core::System& system, Result result) {
    UNIMPLEMENTED();
}

void Break64(Core::System& system, BreakReason break_reason, uint64_t arg, uint64_t size) {
    Break(system, break_reason, arg, size);
}

void Break64From32(Core::System& system, BreakReason break_reason, uint32_t arg, uint32_t size) {
    Break(system, break_reason, arg, size);
}

void ReturnFromException64(Core::System& system, Result result) {
    ReturnFromException(system, result);
}

void ReturnFromException64From32(Core::System& system, Result result) {
    ReturnFromException(system, result);
}

} // namespace Kernel::Svc

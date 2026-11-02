// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "common/logging.h"
#include "common/ns2_watch.h"
#include "common/thread.h"

namespace Common::NS2Watch {

std::atomic<u64> g_present_count{0};
std::atomic<u64> g_composite_count{0};

namespace {

struct Slot {
    std::atomic<bool> used{false};
    std::atomic<const char*> label{nullptr};
    std::atomic<u64> since{0};
    std::atomic<u64> value{0};
    char detail[96]{};
    char thread[48]{};
};

constexpr size_t MaxSlots = 128;
std::array<Slot, MaxSlots> g_slots;

Slot* CurrentSlot() {
    thread_local Slot* slot = [] () -> Slot* {
        for (auto& candidate : g_slots) {
            bool expected = false;
            if (candidate.used.compare_exchange_strong(expected, true)) {
                const std::string name = Common::GetCurrentThreadName();
                std::strncpy(candidate.thread, name.c_str(), sizeof(candidate.thread) - 1);
                return &candidate;
            }
        }
        return nullptr;
    }();
    return slot;
}

void CopyDetail(char* out, const char* detail) {
    if (detail) {
        std::strncpy(out, detail, 95);
        out[95] = '\0';
    } else {
        out[0] = '\0';
    }
}

} // namespace

namespace {
std::mutex g_work_mutex;
std::array<RecentWork, 256> g_work{};
size_t g_work_next{};
size_t g_work_size{};
} // namespace

void NoteWork(u64 tick, u32 kind, const u64 (&shaders)[6]) {
    std::scoped_lock lock{g_work_mutex};
    if (g_work_size != 0) {
        RecentWork& last = g_work[(g_work_next + g_work.size() - 1) % g_work.size()];
        if (last.tick == tick && last.kind == kind && std::memcmp(last.shaders, shaders, sizeof(shaders)) == 0) {
            ++last.count;
            return;
        }
    }
    RecentWork& entry = g_work[g_work_next];
    entry.tick = tick;
    entry.kind = kind;
    entry.count = 1;
    std::memcpy(entry.shaders, shaders, sizeof(shaders));
    g_work_next = (g_work_next + 1) % g_work.size();
    g_work_size = (std::min)(g_work_size + 1, g_work.size());
}

std::vector<RecentWork> CollectWork() {
    std::scoped_lock lock{g_work_mutex};
    std::vector<RecentWork> out;
    for (size_t i = 0; i < g_work_size; ++i) {
        out.push_back(g_work[(g_work_next + g_work.size() - g_work_size + i) % g_work.size()]);
    }
    return out;
}

u64 NowMs() {
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void SetPhase(const char* label, const char* detail, u64 value) {
    Slot* slot = CurrentSlot();
    if (!slot) {
        return;
    }
    // Refresh the thread name lazily; threads are often named after their first phase.
    if (slot->thread[0] == '\0') {
        const std::string name = Common::GetCurrentThreadName();
        std::strncpy(slot->thread, name.c_str(), sizeof(slot->thread) - 1);
    }
    slot->since.store(0, std::memory_order_release);
    CopyDetail(slot->detail, detail);
    slot->value.store(value, std::memory_order_relaxed);
    slot->label.store(label, std::memory_order_relaxed);
    slot->since.store(label ? NowMs() : 0, std::memory_order_release);
}

void ClearPhase() {
    SetPhase(nullptr);
}

std::vector<PhaseInfo> CollectPhases(u64 min_age_ms) {
    std::vector<PhaseInfo> out;
    const u64 now = NowMs();
    for (auto& slot : g_slots) {
        if (!slot.used.load(std::memory_order_acquire)) {
            continue;
        }
        const u64 since = slot.since.load(std::memory_order_acquire);
        const char* label = slot.label.load(std::memory_order_relaxed);
        if (since == 0 || !label || now < since || now - since < min_age_ms) {
            continue;
        }
        char detail[96];
        std::memcpy(detail, slot.detail, sizeof(detail));
        detail[95] = '\0';
        out.push_back(PhaseInfo{slot.thread, label, detail, slot.value.load(std::memory_order_relaxed), now - since});
    }
    return out;
}

PhaseScope::PhaseScope(const char* label, const char* detail, u64 value) {
    Slot* slot = CurrentSlot();
    if (slot) {
        m_prev_label = slot->label.load(std::memory_order_relaxed);
        m_prev_value = slot->value.load(std::memory_order_relaxed);
        m_prev_since = slot->since.load(std::memory_order_relaxed);
        std::memcpy(m_prev_detail, slot->detail, sizeof(m_prev_detail));
    } else {
        m_prev_label = nullptr;
        m_prev_value = 0;
        m_prev_since = 0;
        m_prev_detail[0] = '\0';
    }
    SetPhase(label, detail, value);
}

PhaseScope::~PhaseScope() {
    Slot* slot = CurrentSlot();
    if (!slot) {
        return;
    }
    slot->since.store(0, std::memory_order_release);
    std::memcpy(slot->detail, m_prev_detail, sizeof(m_prev_detail));
    slot->value.store(m_prev_value, std::memory_order_relaxed);
    slot->label.store(m_prev_label, std::memory_order_relaxed);
    slot->since.store(m_prev_label ? m_prev_since : 0, std::memory_order_release);
}

namespace {
std::string g_crash_path;

#ifdef _WIN32
void WritePhases(FILE* file) {
    std::fprintf(file, "crashing thread: %s\n", Common::GetCurrentThreadName().c_str());
    for (const auto& phase : CollectPhases(0)) {
        std::fprintf(file, "thread '%s': %s %s value=%#llx (for %llu ms)\n", phase.thread.c_str(),
                     phase.label.c_str(), phase.detail.c_str(),
                     static_cast<unsigned long long>(phase.value),
                     static_cast<unsigned long long>(phase.age_ms));
    }
}

void WriteAddress(FILE* file, const void* address) {
    HMODULE module{};
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) &&
        module != nullptr) {
        GetModuleFileNameA(module, name, sizeof(name));
    }
    const char* base = std::strrchr(name, '\\');
    base = base ? base + 1 : name;
    std::fprintf(file, "  %p %s+%#llx\n", address, base,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                 reinterpret_cast<uintptr_t>(module)));
}

LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter{};

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    static std::atomic<bool> written{false};
    if (written.exchange(true) || g_crash_path.empty()) {
        return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
    }
    FILE* file = std::fopen(g_crash_path.c_str(), "w");
    if (!file) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    std::fprintf(file, "exception %#lx at:\n", static_cast<unsigned long>(record->ExceptionCode));
    WriteAddress(file, record->ExceptionAddress);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        std::fprintf(file, "access violation (%s) at %#llx\n",
                     record->ExceptionInformation[0] == 0 ? "read"
                     : record->ExceptionInformation[0] == 1 ? "write" : "execute",
                     static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }
    std::fprintf(file, "stack:\n");
    void* frames[64]{};
    const USHORT count = RtlCaptureStackBackTrace(0, 64, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        WriteAddress(file, frames[i]);
    }
    WritePhases(file);
    std::fclose(file);
    LOG_CRITICAL(Common, "NS2: unhandled exception {:#x} at {}, see ns2_crash.txt",
                 static_cast<unsigned long>(record->ExceptionCode), record->ExceptionAddress);
    return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

std::string ModuleOf(const void* address, uintptr_t* offset = nullptr) {
    HMODULE module{};
    char name[MAX_PATH] = "";
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(address), &module) ||
        module == nullptr) {
        return {};
    }
    GetModuleFileNameA(module, name, sizeof(name));
    if (offset) {
        *offset = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module);
    }
    const char* base = std::strrchr(name, '\\');
    return base ? base + 1 : name;
}

// First-chance exceptions that are not part of normal operation, logged before any handler runs
// (a later handler may end the process without reaching the crash filter). Access violations in
// code without a module are fastmem faults of the JIT and are skipped, as are C++ exceptions,
// debugger messages and thread naming.
LONG CALLBACK FirstChanceHandler(EXCEPTION_POINTERS* info) {
    static std::atomic<u32> logged{0};
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    const DWORD code = record->ExceptionCode;
    if (code == 0xE06D7363 /* C++ */ || code == 0x406D1388 /* thread name */ ||
        code == DBG_PRINTEXCEPTION_C || code == 0x4001000A /* DBG_PRINTEXCEPTION_WIDE_C */ ||
        code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP ||
        code == 0x000006BA /* RPC_S_SERVER_UNAVAILABLE */ || code == 0x40080201 /* WinRT originate */) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t offset{};
    const std::string module = ModuleOf(record->ExceptionAddress, &offset);
    if (code == EXCEPTION_ACCESS_VIOLATION && module.empty()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (logged.fetch_add(1) >= 16) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    unsigned long long target{};
    const char* kind = "";
    if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        kind = record->ExceptionInformation[0] == 0 ? " (read" : record->ExceptionInformation[0] == 1 ? " (write" : " (execute";
        target = static_cast<unsigned long long>(record->ExceptionInformation[1]);
    }
    LOG_CRITICAL(Common, "NS2: first-chance exception {:#x} in thread '{}' at {}+{:#x}{}{}", static_cast<unsigned long>(code),
                 Common::GetCurrentThreadName(), module.empty() ? std::string{"?"} : module,
                 static_cast<unsigned long long>(offset), kind,
                 *kind ? fmt::format(" of {:#x})", target) : std::string{});
    return EXCEPTION_CONTINUE_SEARCH;
}

void WriteCrashNote(const char* what) {
    const std::string thread = Common::GetCurrentThreadName();
    if (!g_crash_path.empty()) {
        if (FILE* file = std::fopen(g_crash_path.c_str(), "w")) {
            std::fprintf(file, "%s in thread '%s'\nstack:\n", what, thread.c_str());
            void* frames[64]{};
            const USHORT count = RtlCaptureStackBackTrace(1, 64, frames, nullptr);
            for (USHORT i = 0; i < count; ++i) {
                WriteAddress(file, frames[i]);
            }
            WritePhases(file);
            std::fclose(file);
        }
    }
    LOG_CRITICAL(Common, "NS2: {} in thread '{}' (stack in ns2_crash.txt)", what, thread);
}

void AbortSignalHandler(int) {
    WriteCrashNote("abort() called");
}

void InvalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    WriteCrashNote("invalid parameter passed to a C runtime function");
    std::abort();
}

void PureCallHandler() {
    WriteCrashNote("pure virtual function call");
    std::abort();
}
#endif

std::atomic<bool> g_exit_logged{false};

// exit() from any thread (also from a driver or library) ends Eden without a crash. Logged with
// the calling stack; at a normal shutdown this line is expected.
void ExitNote() {
    if (g_exit_logged.exchange(true)) {
        return;
    }
    std::string stack;
#ifdef _WIN32
    void* frames[24]{};
    const USHORT count = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        uintptr_t offset{};
        const std::string module = ModuleOf(frames[i], &offset);
        stack += fmt::format(" {}+{:#x}", module.empty() ? std::string{"?"} : module,
                             static_cast<unsigned long long>(offset));
    }
#endif
    LOG_CRITICAL(Common, "NS2: process exit() in thread '{}', stack:{}", Common::GetCurrentThreadName(),
                 stack.empty() ? std::string{" -"} : stack);
}
} // namespace

namespace {
std::mutex g_gpu_poll_mutex;
std::function<void()> g_gpu_poll;
} // namespace

void SetGpuPoll(std::function<void()> poll) {
    std::scoped_lock lock{g_gpu_poll_mutex};
    g_gpu_poll = std::move(poll);
}

void RunGpuPoll() {
    std::scoped_lock lock{g_gpu_poll_mutex};
    if (g_gpu_poll) {
        g_gpu_poll();
    }
}

namespace {
// An uncaught C++ exception (for example a Vulkan call failing with VK_ERROR_DEVICE_LOST) ends the
// process through std::terminate, which the crash filter above does not see.
[[noreturn]] void TerminateHandler() {
    std::string message = "std::terminate without an active exception";
    if (const std::exception_ptr exception = std::current_exception()) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& e) {
            message = e.what();
        } catch (...) {
            message = "non-standard exception";
        }
    }
    const std::string thread = Common::GetCurrentThreadName();
    // Critical messages are flushed to the log file immediately.
    LOG_CRITICAL(Common, "NS2: uncaught exception in thread '{}': {}", thread, message);
    if (!g_crash_path.empty()) {
        if (FILE* file = std::fopen(g_crash_path.c_str(), "w")) {
            std::fprintf(file, "uncaught C++ exception in thread '%s': %s\n", thread.c_str(),
                         message.c_str());
#ifdef _WIN32
            WritePhases(file);
#endif
            std::fclose(file);
        }
    }
    std::abort();
}
} // namespace

void CheckCrashReport() {
#ifdef _WIN32
    if (g_crash_path.empty()) {
        return;
    }
    // Another component replacing the filter would bypass the crash report: put it back and say who.
    const LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(CrashFilter);
    if (current != CrashFilter) {
        static std::atomic<u32> reported{0};
        if (reported.fetch_add(1) < 4) {
            const std::string module = current ? ModuleOf(reinterpret_cast<const void*>(current)) : std::string{"none"};
            LOG_WARNING(Common, "NS2: the unhandled exception filter was replaced (by {}); restored, the other one is chained",
                        module.empty() ? std::string{"?"} : module);
        }
        if (current) {
            g_previous_filter = current;
        }
    }
#endif
}

void InstallCrashReport(const std::string& path) {
    static std::atomic<bool> installed{false};
    g_crash_path = path;
    if (installed.exchange(true)) {
        return;
    }
    std::set_terminate(TerminateHandler);
    std::atexit(ExitNote);
    std::at_quick_exit(ExitNote);
#ifdef _WIN32
    g_previous_filter = SetUnhandledExceptionFilter(CrashFilter);
    AddVectoredExceptionHandler(1, FirstChanceHandler);
    std::signal(SIGABRT, AbortSignalHandler);
    _set_invalid_parameter_handler(InvalidParameterHandler);
    _set_purecall_handler(PureCallHandler);
#endif
    // Elsewhere SIGSEGV belongs to fastmem, so nothing is installed.
}

} // namespace Common::NS2Watch

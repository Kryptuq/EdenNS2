// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <thread>

#include "common/common_types.h"

namespace Core {

class System;

/// Reports what the guest and host threads are doing when a Switch 2 title stops presenting
/// frames, so hangs can be diagnosed from a log.
class NS2Watchdog {
public:
    explicit NS2Watchdog(System& system);
    ~NS2Watchdog();

    NS2Watchdog(const NS2Watchdog&) = delete;
    NS2Watchdog& operator=(const NS2Watchdog&) = delete;

private:
    void Run(std::stop_token stop_token);
    void Dump(u64 stalled_ms);

    System& m_system;
    std::jthread m_thread;
};

} // namespace Core

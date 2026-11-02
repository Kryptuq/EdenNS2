// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "common/cityhash.h"
#include "video_core/dma_pusher.h"

namespace {
using Tegra::CommandHeader;
using namespace Tegra::NativeSubmissions;

u64 Hash(const std::array<CommandHeader, 3>& words) {
    return Common::CityHash64(reinterpret_cast<const char*>(words.data()), sizeof(words));
}
} // namespace

TEST_CASE("NativeSubmissions: memory reused for a later submission", "[ns2-submissions]") {
    // nn::movie: list A submitted, then re-recorded with B and submitted again.
    const std::array<CommandHeader, 3> a{{{1}, {2}, {3}}};
    const std::array<CommandHeader, 3> b{{{4}, {5}, {6}}};
    const u64 first = Record(3, 0x1000, a);
    const u64 second = Record(3, 0x1000, b);
    REQUIRE(ClaimedLater(3, 0x1000, first, Hash(b)));
    REQUIRE_FALSE(ClaimedLater(3, 0x1000, second, Hash(b)));
}

TEST_CASE("NativeSubmissions: list completed in place", "[ns2-submissions]") {
    // Channel 1: placeholders submitted (twice), filled in afterwards.
    const std::array<CommandHeader, 3> placeholder{{{0}, {0}, {0}}};
    const std::array<CommandHeader, 3> filled{{{0x28}, {0x17}, {1}}};
    const u64 first = Record(1, 0x2000, placeholder);
    const u64 second = Record(1, 0x2000, placeholder);
    REQUIRE_FALSE(ClaimedLater(1, 0x2000, first, Hash(filled)));
    REQUIRE_FALSE(ClaimedLater(1, 0x2000, second, Hash(filled)));
}

TEST_CASE("NativeSubmissions: channels are separate", "[ns2-submissions]") {
    const std::array<CommandHeader, 3> a{{{7}, {8}, {9}}};
    const std::array<CommandHeader, 3> b{{{10}, {11}, {12}}};
    const u64 first = Record(5, 0x3000, a);
    Record(6, 0x3000, b);
    REQUIRE_FALSE(ClaimedLater(5, 0x3000, first, Hash(b)));
}

TEST_CASE("NativeSubmissions: reuse history", "[ns2-submissions]") {
    const std::array<CommandHeader, 3> a{{{13}, {14}, {15}}};
    Record(7, 0x4000, a);
    Record(8, 0x4000, a);
    REQUIRE_FALSE(UsuallyReused(7, 0x4000));
    for (int i = 0; i < 8; ++i) NoteOutcome(7, 0x4000, true);
    REQUIRE(UsuallyReused(7, 0x4000));
    REQUIRE_FALSE(UsuallyReused(8, 0x4000));
    // Mostly completed in place: no reuse history.
    for (int i = 0; i < 8; ++i) NoteOutcome(8, 0x4000, true);
    for (int i = 0; i < 40; ++i) NoteOutcome(8, 0x4000, false);
    REQUIRE_FALSE(UsuallyReused(8, 0x4000));
    // A new submission keeps the history.
    Record(7, 0x4000, a);
    REQUIRE(UsuallyReused(7, 0x4000));
}

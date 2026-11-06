// SPDX-License-Identifier: GPL-3.0-or-later

// Instruction forms ported from EdenNS2 main (yoshi-int-build-29). The words are the ones Yoshi's
// red/yellow area programs were rejected on in yoshi-int-build-28.

#include <array>
#include <cstdlib>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/decode.h"
#include "shader_recompiler/frontend/maxwell/opcodes.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

bool Emits(const Shader::Ampere::LoweredProgram& lowered, u64 mask, u64 value) {
    for (const u64 instruction : lowered.instructions) {
        if ((instruction & mask) == value) {
            return true;
        }
    }
    return false;
}

TEST_CASE("Ampere SHFL keeps its mode (DOWN)", "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{{{0x00077589, 0x08001f05, 0x000e0000, 0x000e2200}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    // Maxwell SHFL with register lane: mode 2 (DOWN) at bits 30-31, Rd = R7, Ra = R0, lane R5.
    REQUIRE(Emits(lowered, 0xfff8000000000000ULL | (u64{3} << 30) | (u64{0xff} << 20) | 0xffffULL,
                  0xef10000000000000ULL | (u64{2} << 30) | (u64{5} << 20) | 0x0007ULL));
}

TEST_CASE("Ampere SHFL.IDX still lowers as before", "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{{{0x00077589, 0x00001f05, 0x000e0000, 0x000e2200}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(Emits(lowered, 0xfff8000000000000ULL | (u64{3} << 30), 0xef10000000000000ULL));
}

TEST_CASE("Ampere F2I from F16 rounds to nearest", "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{{{0x00197305, 0x00000013, 0x00103100, 0x0000a200}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    // Maxwell F2I (reg) with rounding 0 (RN) at bits 39-40.
    REQUIRE(Emits(lowered, 0xfff8000000000000ULL | (u64{3} << 39), 0x5cb0000000000000ULL));
}

TEST_CASE("Ampere F2I from F16 truncating is unchanged", "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{{{0x00197305, 0x00000013, 0x0010f100, 0x0000a200}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(Emits(lowered, 0xfff8000000000000ULL | (u64{3} << 39),
                  0x5cb0000000000000ULL | (u64{3} << 39)));
}

TEST_CASE("Ampere HMMA becomes NS2_HMMA only when enabled for the title", "[video_core][ampere]") {
    REQUIRE(std::getenv("EDEN_NX2_TENSOR") == nullptr);
    const std::array<Instruction, 2> code{{{{0x1402723c, 0x0000000e, 0x000008ff, 0x002fe600}}, Exit}};
    Shader::Ampere::NativeTensorByDefault() = false;
    const auto rejected = LowerToMaxwell(code);
    REQUIRE(!static_cast<bool>(rejected));
    REQUIRE(rejected.error == "native matrix multiply (set EDEN_NX2_TENSOR to emulate)");

    Shader::Ampere::NativeTensorByDefault() = true;
    const auto lowered = LowerToMaxwell(code);
    Shader::Ampere::NativeTensorByDefault() = false;
    REQUIRE(static_cast<bool>(lowered));
    // NS2_HMMA: dest R2 (0-7), A R20 (8-15), B R14 (20-27), C RZ (28-35), F16 result (36).
    const u64 expected = 0xe100000000000000ULL | 0x0007'0000ULL | 0x02ULL | (u64{0x14} << 8) |
                         (u64{0x0e} << 20) | (u64{0xff} << 28);
    REQUIRE(Emits(lowered, ~0ULL, expected));
    REQUIRE(Shader::Maxwell::Decode(expected) == Shader::Maxwell::Opcode::NS2_HMMA);
}

TEST_CASE("Ampere HMMA with F32 results is accepted", "[video_core][ampere]") {
    // Same instruction with bit 76 (F32 results): C, D are four registers.
    const std::array<Instruction, 2> code{{{{0x1404723c, 0x0000000e, 0x000018ff, 0x002fe600}}, Exit}};
    Shader::Ampere::NativeTensorByDefault() = true;
    const auto lowered = LowerToMaxwell(code);
    Shader::Ampere::NativeTensorByDefault() = false;
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(Emits(lowered, 0xff00'0010'0000'0000ULL, 0xe100'0010'0000'0000ULL));
}

} // namespace

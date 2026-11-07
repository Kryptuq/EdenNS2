// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// LDSM.16.M88 (ldmatrix) lowering (yoshi-int-build-32). The lowered Maxwell code is executed for a
// whole warp (32 lanes in lockstep, shared memory, SHFL.IDX) by a small interpreter of the
// instructions this lowering emits (opcode patterns and fields as in maxwell.inc and the Maxwell
// translator) and compared with ldmatrix: lanes 8m..8m+7 give the row addresses of matrix m, lane
// t receives the 32 bits at row t/4, byte 4*(t%4) of each matrix in Rd+m.

#include <array>
#include <cstring>
#include <random>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

struct Warp {
    std::array<std::array<u32, 256>, 32> r{};
    std::vector<u8> shared = std::vector<u8>(0x4000);

    u32 R(u32 lane, u64 index) const { return index == 255 ? 0U : r[lane][index]; }

    bool Run(const std::vector<u64>& code) {
        for (const u64 insn : code) {
            const auto bits = [insn](u32 offset, u32 count) { return (insn >> offset) & ((1ULL << count) - 1); };
            const u64 top = insn >> 48;
            if ((top & 0xfff0) == 0xe300) return true;                 // EXIT
            if (bits(16, 4) != 7) return false;                        // the lowering emits @PT only
            const u64 d = bits(0, 8), a = bits(8, 8);
            std::array<u32, 32> result{};
            for (u32 lane = 0; lane < 32; ++lane) {
                if ((top & 0xfff8) == 0xf0c8) {                       // S2R
                    if (bits(20, 8) != 0) return false;
                    result[lane] = lane;                               // SR_LANEID
                } else if ((top & 0xfef8) == 0x3828) {                // SHR.U32 imm
                    result[lane] = R(lane, a) >> bits(20, 5);
                } else if ((top & 0xfef8) == 0x3848) {                // SHL imm
                    result[lane] = R(lane, a) << bits(20, 5);
                } else if ((top & 0xfe00) == 0x1c00) {                // IADD32I
                    result[lane] = R(lane, a) + static_cast<u32>(bits(20, 32));
                } else if ((top & 0xfc00) == 0x0400) {                // LOP32I
                    if (bits(53, 2) != 0) return false;
                    result[lane] = R(lane, a) & static_cast<u32>(bits(20, 32));
                } else if ((top & 0xfff8) == 0x5c10) {                // IADD reg
                    if (bits(43, 7)) return false;
                    result[lane] = R(lane, a) + R(lane, bits(20, 8));
                } else if ((top & 0xfff8) == 0xef10) {                // SHFL.IDX, lane in a register
                    if (bits(28, 1) || !bits(29, 1) || bits(30, 2) || bits(34, 13) != 31) return false;
                    result[lane] = R(static_cast<u32>(R(lane, bits(20, 8)) & 31), a);
                } else if ((top & 0xfff8) == 0xef48) {                // LDS
                    const u32 address = R(lane, a) + static_cast<u32>(bits(20, 24));
                    if (address + 4 > shared.size()) return false;
                    if (bits(48, 3) == 4) {
                        std::memcpy(&result[lane], &shared[address], 4);
                    } else if (bits(48, 3) == 2) {
                        u16 half;
                        std::memcpy(&half, &shared[address], 2);
                        result[lane] = half;
                    } else {
                        return false;
                    }
                } else {
                    return false;
                }
            }
            if (d != 255)
                for (u32 lane = 0; lane < 32; ++lane) r[lane][d] = result[lane];
        }
        return true;
    }
};

// LDSM.16.M88.4 Rd..Rd+3, [Ra + offset] (words of 557f7bdab63f0cb8 +0x25e0: R28..R31, [R28]).
constexpr Instruction Ldsm4(u32 dest, u32 address, u32 offset) {
    return {{0x0000783bU | (dest << 16) | (address << 24), offset << 8, 0x00000200U, 0x000fe800U}};
}

void Check(u32 dest, u32 address, u32 offset) {
    const std::array<Instruction, 2> code{{Ldsm4(dest, address, offset), Exit}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    std::mt19937 rng{dest * 256 + address};
    for (int iteration = 0; iteration < 50; ++iteration) {
        Warp warp;
        for (auto& byte : warp.shared) byte = static_cast<u8>(rng());
        for (u32 lane = 0; lane < 32; ++lane) {
            for (auto& value : warp.r[lane]) value = static_cast<u32>(rng());
            // Row addresses: 16-byte rows anywhere in shared memory (lanes 8m..8m+7: matrix m).
            warp.r[lane][address] = (rng() % 0x3e0) * 16;
        }
        const Warp before = warp;
        REQUIRE(warp.Run(lowered.instructions));
        for (u32 lane = 0; lane < 32; ++lane) {
            for (u32 matrix = 0; matrix < 4; ++matrix) {
                const u32 row_address = before.r[8 * matrix + lane / 4][address] + offset + 4 * (lane % 4);
                u32 expected;
                std::memcpy(&expected, &before.shared[row_address], 4);
                REQUIRE(warp.r[lane][dest + matrix] == expected);
            }
            // Guest registers outside the destination keep their values.
            for (u32 reg = 0; reg < 246; ++reg) {
                if (reg >= dest && reg < dest + 4) continue;
                REQUIRE(warp.r[lane][reg] == before.r[lane][reg]);
            }
        }
    }
}

TEST_CASE("LDSM.x4 loads four matrices for every lane", "[video_core][ampere][ldsm]") {
    Check(8, 20, 0);
    Check(8, 20, 0x40);
}

TEST_CASE("LDSM.x4 whose address register is inside its destination", "[video_core][ampere][ldsm]") {
    // The game's form: the address is the first destination register (557f7bdab63f0cb8 +0x25e0).
    Check(28, 28, 0);
    // ... and the other positions inside the destination range.
    Check(28, 29, 0);
    Check(28, 31, 0x20);
}

} // namespace

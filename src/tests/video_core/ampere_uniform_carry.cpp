// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// UIADD3 carry forms (yoshi-int-build-31). The words are the ones Yoshi's red/yellow area compute
// programs were rejected on (yoshi-int-build-30 dump, programs 445c38e434e886a7 and
// 557f7bdab63f0cb8): 64-bit address adds whose carries live in uniform predicates. The lowered
// Maxwell code is executed by a small interpreter of the instructions these lowerings emit (field
// layout and opcode patterns as in maxwell.inc) and compared with the UIADD3 result on random inputs.

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

struct Machine {
    std::array<u32, 256> r{};
    std::array<bool, 8> p{};
    bool carry{};

    u32 R(u64 index) const { return index == 255 ? 0U : r[index]; }
    void SetR(u64 index, u32 value) { if (index != 255) r[index] = value; }
    bool P(u64 index) const { return index == 7 ? true : p[index]; }
    void SetP(u64 index, bool value) { if (index != 7) p[index] = value; }

    // Runs the lowered program; returns false on an instruction the interpreter does not know.
    bool Run(const std::vector<u64>& code) {
        for (const u64 insn : code) {
            const auto bits = [insn](u32 offset, u32 count) { return (insn >> offset) & ((1ULL << count) - 1); };
            const bool guard = P(bits(16, 3)) != (bits(19, 1) != 0);
            const u64 top = insn >> 48;
            const u64 dst = bits(0, 8), a = bits(8, 8), b = bits(20, 8);
            if ((insn >> 52) == 0xe30) return true;                    // EXIT
            if (!guard) continue;
            if ((top & 0xfff8) == 0x5c10) {                           // IADD reg
                const u32 x = bits(49, 1) ? ~R(a) + 1 : R(a), y = bits(48, 1) ? ~R(b) + 1 : R(b);
                const u64 sum = u64{x} + y + (bits(43, 1) && carry ? 1 : 0);
                if (bits(47, 1)) carry = (sum >> 32) != 0;
                SetR(dst, static_cast<u32>(sum));
            } else if ((top & 0xfff8) == 0x5c98) {                    // MOV reg
                SetR(dst, R(b));
            } else if ((top & 0xfff0) == 0x0100) {                   // MOV32I
                SetR(dst, static_cast<u32>(bits(20, 32)));
            } else if ((top & 0xfff0) == 0x5b60) {                    // ISETP reg, unsigned, AND PT
                if (bits(48, 1) || bits(39, 3) != 7 || bits(42, 1) || bits(45, 2) || bits(0, 3) != 7) return false;
                const u32 x = R(a), y = R(b);
                bool result{};
                switch (bits(49, 3)) {
                case 1: result = x < y; break;
                case 2: result = x == y; break;
                case 3: result = x <= y; break;
                case 4: result = x > y; break;
                case 5: result = x != y; break;
                case 6: result = x >= y; break;
                default: return false;
                }
                SetP(bits(3, 3), result);
            } else if ((top & 0xfff8) == 0x5ca0) {                    // SEL
                SetR(dst, (P(bits(39, 3)) != (bits(42, 1) != 0)) ? R(a) : R(b));
            } else if ((top & 0xfef8) == 0x3800) {                    // BFE.U32 imm
                if (bits(48, 1)) return false;
                const u32 position = static_cast<u32>(bits(20, 8)), length = static_cast<u32>(bits(28, 8));
                SetR(dst, (R(a) >> position) & ((1U << length) - 1));
            } else if ((top & 0xfc00) == 0x0400) {                    // LOP32I
                const u32 imm = static_cast<u32>(bits(20, 32));
                switch (bits(53, 2)) {
                case 0: SetR(dst, R(a) & imm); break;
                case 1: SetR(dst, R(a) | imm); break;
                case 2: SetR(dst, R(a) ^ imm); break;
                default: return false;
                }
            } else if ((top & 0xfff8) == 0x38e8) {                    // P2R PR: Rd = Ra & ~mask | P & mask
                const u32 mask = static_cast<u32>(bits(20, 7));
                u32 inserted = 0;
                for (u32 i = 0; i < 7; ++i) inserted |= (P(i) ? 1U : 0U) << i;
                SetR(dst, (R(a) & ~mask) | (inserted & mask));
            } else if ((top & 0xfef8) == 0x38f0) {                    // R2P PR
                const u32 mask = static_cast<u32>(bits(20, 7));
                for (u32 i = 0; i < 7; ++i)
                    if (mask & (1U << i)) SetP(i, (R(a) >> i) & 1);
            } else {
                return false;
            }
        }
        return true;
    }
};

// UR n lives in R(216 + n) for n < 22; UPn is R239 bit 6-n.
constexpr u32 Home(u32 ur) { return 216 + ur; }
bool UP(const Machine& m, u32 n) { return (m.r[239] >> (6 - n)) & 1; }

Machine RandomMachine(std::mt19937& rng) {
    Machine m;
    for (auto& value : m.r) value = static_cast<u32>(rng());
    for (u32 i = 0; i < 7; ++i) m.p[i] = rng() & 1;
    m.r[239] &= 0x7f;
    return m;
}

// Values that exercise the carries: random, all ones, zero and near-overflow.
u32 Interesting(std::mt19937& rng) {
    switch (rng() % 4) {
    case 0: return 0xffffffffU - (rng() % 3);
    case 1: return rng() % 3;
    default: return static_cast<u32>(rng());
    }
}

std::vector<u64> Lower(const Instruction& insn) {
    const std::array<Instruction, 2> code{{insn, Exit}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    return lowered.instructions;
}

// Every register and predicate except the expected outputs (and lowering scratch R246+) is kept.
void RequireOnlyChanged(const Machine& before, const Machine& after, std::optional<u32> dest_ur,
                        u32 up_mask) {
    for (u32 i = 0; i < 246; ++i) {
        if (i == 239 || (dest_ur && i == Home(*dest_ur))) continue;
        REQUIRE(after.r[i] == before.r[i]);
    }
    for (u32 i = 0; i < 7; ++i) REQUIRE(after.p[i] == before.p[i]);
    const u32 kept = 0x7f & ~up_mask;
    REQUIRE((after.r[239] & kept) == (before.r[239] & kept));
}

TEST_CASE("UIADD3 two-source add with carry out to UP2", "[video_core][ampere][uniform-carry]") {
    // UIADD3 UR9, UP2, UR9, UR6, URZ (445c38e434e886a7 +0x2220)
    const auto code = Lower({{0x09097290, 0x00000006, 0x0ff5e03f, 0x000fe200}});
    std::mt19937 rng{1};
    for (int i = 0; i < 2000; ++i) {
        Machine m = RandomMachine(rng);
        m.r[Home(9)] = Interesting(rng);
        m.r[Home(6)] = Interesting(rng);
        const u64 sum = u64{m.r[Home(9)]} + m.r[Home(6)];
        const Machine before = m;
        REQUIRE(m.Run(code));
        REQUIRE(m.r[Home(9)] == static_cast<u32>(sum));
        REQUIRE(UP(m, 2) == (sum >> 32 != 0));
        RequireOnlyChanged(before, m, 9, 1U << 4);
    }
}

TEST_CASE("UIADD3 three-source add with carries out to UP1 and UP0", "[video_core][ampere][uniform-carry]") {
    // UIADD3 UR11, UP1, UP0, UR11, UR4, UR13 (445c +0x2240) and UR9 = UR9 + UR4 + UR6 (557f +0x2890)
    struct Case { Instruction insn; u32 d, a, b, c; };
    for (const Case& test : {Case{{{0x0b0b7290, 0x00000004, 0x0f83e00d, 0x000fe200}}, 11, 11, 4, 13},
                             Case{{{0x09097290, 0x00000004, 0x0f83e006, 0x000fe400}}, 9, 9, 4, 6}}) {
        const auto code = Lower(test.insn);
        std::mt19937 rng{2};
        for (int i = 0; i < 2000; ++i) {
            Machine m = RandomMachine(rng);
            m.r[Home(test.a)] = Interesting(rng);
            m.r[Home(test.b)] = Interesting(rng);
            m.r[Home(test.c)] = Interesting(rng);
            const u64 sum = u64{m.r[Home(test.a)]} + m.r[Home(test.b)] + m.r[Home(test.c)];
            const Machine before = m;
            REQUIRE(m.Run(code));
            REQUIRE(m.r[Home(test.d)] == static_cast<u32>(sum));
            REQUIRE(UP(m, 1) == (sum >= (1ULL << 32)));  // first output: sum >= 2^32
            REQUIRE(UP(m, 0) == (sum >= (1ULL << 33)));  // second output: sum >= 2^33
            RequireOnlyChanged(before, m, test.d, (1U << 5) | (1U << 6));
        }
    }
}

TEST_CASE("UIADD3.X with carry in from UP2", "[video_core][ampere][uniform-carry]") {
    // UIADD3.X UR8, UR8, UR7, URZ, UP2, !UPT (445c +0x2260)
    const auto code = Lower({{0x08087290, 0x00000007, 0x097fe43f, 0x000fea00}});
    std::mt19937 rng{3};
    for (int i = 0; i < 2000; ++i) {
        Machine m = RandomMachine(rng);
        m.r[Home(8)] = Interesting(rng);
        m.r[Home(7)] = Interesting(rng);
        const u32 expected = m.r[Home(8)] + m.r[Home(7)] + (UP(m, 2) ? 1U : 0U);
        const Machine before = m;
        REQUIRE(m.Run(code));
        REQUIRE(m.r[Home(8)] == expected);
        RequireOnlyChanged(before, m, 8, 0);
    }
}

TEST_CASE("UIADD3.X with two carries in from UP1 and UP0", "[video_core][ampere][uniform-carry]") {
    // UIADD3.X UR10, UR10, UR5, UR16, UP1, UP0 (445c +0x23a0) and UR8, UR8, UR5, UR7 (557f +0x28d0)
    struct Case { Instruction insn; u32 d, a, b, c; };
    for (const Case& test : {Case{{{0x0a0a7290, 0x00000005, 0x08fe0410, 0x000fe400}}, 10, 10, 5, 16},
                             Case{{{0x08087290, 0x00000005, 0x08fe0407, 0x000fe200}}, 8, 8, 5, 7}}) {
        const auto code = Lower(test.insn);
        std::mt19937 rng{4};
        for (int i = 0; i < 2000; ++i) {
            Machine m = RandomMachine(rng);
            m.r[Home(test.a)] = Interesting(rng);
            m.r[Home(test.b)] = Interesting(rng);
            m.r[Home(test.c)] = Interesting(rng);
            const u32 expected = m.r[Home(test.a)] + m.r[Home(test.b)] + m.r[Home(test.c)] +
                                 (UP(m, 1) ? 1U : 0U) + (UP(m, 0) ? 1U : 0U);
            const Machine before = m;
            REQUIRE(m.Run(code));
            REQUIRE(m.r[Home(test.d)] == expected);
            RequireOnlyChanged(before, m, test.d, 0);
        }
    }
}

TEST_CASE("Full 64-bit uniform add chain from the red/yellow program", "[video_core][ampere][uniform-carry]") {
    // 557f7bdab63f0cb8 +0x2890 / +0x28d0 together: (UR8:UR9) += (UR5:UR4) + (UR7:UR6). The low
    // words add with carries into UP1/UP0, the high words add them back in.
    const auto low = Lower({{0x09097290, 0x00000004, 0x0f83e006, 0x000fe400}});
    const auto high = Lower({{0x08087290, 0x00000005, 0x08fe0407, 0x000fe200}});
    std::mt19937 rng{5};
    for (int i = 0; i < 2000; ++i) {
        Machine m = RandomMachine(rng);
        for (const u32 ur : {4U, 5U, 6U, 7U, 8U, 9U}) m.r[Home(ur)] = Interesting(rng);
        const u64 x = (u64{m.r[Home(8)]} << 32) | m.r[Home(9)];
        const u64 y = (u64{m.r[Home(5)]} << 32) | m.r[Home(4)];
        const u64 z = (u64{m.r[Home(7)]} << 32) | m.r[Home(6)];
        REQUIRE(m.Run(low));
        REQUIRE(m.Run(high));
        REQUIRE(((u64{m.r[Home(8)]} << 32) | m.r[Home(9)]) == x + y + z);
    }
}

TEST_CASE("UIADD3 carries to UP2 and up stay rejected under a uniform guard", "[video_core][ampere][uniform-carry]") {
    // @UP3 UIADD3 UR9, UP2, UR9, UR6, URZ: the guard swap also uses R253, keep rejecting.
    const std::array<Instruction, 2> code{{{{0x09093290, 0x00000006, 0x0ff5e03f, 0x000fe200}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(!static_cast<bool>(lowered));
    REQUIRE(lowered.error == "native integer carry mode");
}

} // namespace

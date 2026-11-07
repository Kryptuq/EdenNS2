// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Register remap (EdenNS2 main, ported for yoshi-int-build-31): guest registers in the range the
// native lowering reserves for itself are renamed per instruction and redirected to extended IR
// registers. Yoshi's red/yellow area programs be594fabcdb19f53 and fb88b1bef8a89c12 use R200-R250.

#include <array>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/reg.h"
#include "shader_recompiler/object_pool.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

// IADD3 Rd, Ra, Rb, RZ (register form, no carries), as in the game's programs.
constexpr Instruction Iadd3(u32 d, u32 a, u32 b) {
    return {{0x00007210U | (d << 16) | (a << 24), b, 0x07ffe0ffU, 0x000fe200U}};
}

// Executes the lowered code: IADD (reg) and EXIT, with each instruction's register remaps applied
// the way the translator applies them (the remapped low register reads/writes extended register
// 256 + to).
struct Machine {
    std::array<u32, Shader::IR::NUM_REGS> r{};

    bool Run(const Shader::Ampere::LoweredProgram& lowered) {
        for (u32 index = 0; index < lowered.instructions.size(); ++index) {
            const u64 insn = lowered.instructions[index];
            const auto reg = [&](u64 field) -> size_t {
                for (const auto& range : lowered.register_remaps)
                    if (range.first <= index && index < range.last && range.from == field) return 256 + range.to;
                return field;
            };
            const auto read = [&](u64 field) { return field == 255 ? 0U : r[reg(field)]; };
            if ((insn >> 52) == 0xe30) return true;
            if (((insn >> 48) & 0xfff8) != 0x5c10 || ((insn >> 16) & 15) != 7 || ((insn >> 43) & 0x7f)) return false;
            const u64 d = insn & 0xff, a = (insn >> 8) & 0xff, b = (insn >> 20) & 0xff;
            if (d != 255) r[reg(d)] = read(a) + read(b);
        }
        return true;
    }
};

TEST_CASE("Guest registers in the reserved range are renamed and redirected", "[video_core][ampere][remap]") {
    // R248 is lowering scratch: R248 = R1 + R2; R3 = R248 + R1; R250 = R248 + R3; R4 = R250 + RZ.
    const std::array<Instruction, 5> code{{Iadd3(248, 1, 2), Iadd3(3, 248, 1), Iadd3(250, 248, 3),
                                           Iadd3(4, 250, 255), Exit}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(!lowered.register_remaps.empty());
    for (const auto& range : lowered.register_remaps) {
        REQUIRE(range.first <= range.last);
        REQUIRE(range.last <= lowered.instructions.size());
        REQUIRE(range.from < 246);                     // a free low register of that instruction
        REQUIRE(range.to < Shader::IR::NUM_EXTENDED_REGS);
    }
    // The lowered instructions name no reserved register: they all became placeholders.
    for (const u64 insn : lowered.instructions) {
        if ((insn >> 52) == 0xe30) continue;
        for (const u32 shift : {0U, 8U, 20U}) {
            const u32 field = static_cast<u32>((insn >> shift) & 0xff);
            REQUIRE((field < 246 || field == 255));
        }
    }
    std::mt19937 rng{7};
    for (int i = 0; i < 500; ++i) {
        Machine m;
        for (auto& value : m.r) value = static_cast<u32>(rng());
        const Machine before = m;
        REQUIRE(m.Run(lowered));
        const u32 r248 = before.r[1] + before.r[2];
        const u32 r3 = r248 + before.r[1];
        const u32 r250 = r248 + r3;
        REQUIRE(m.r[3] == r3);
        REQUIRE(m.r[4] == r250);
        REQUIRE(m.r[256 + Shader::Ampere::ExtendedIndexOfGuestRegister(248)] == r248);
        REQUIRE(m.r[256 + Shader::Ampere::ExtendedIndexOfGuestRegister(250)] == r250);
        // Placeholders only redirect: every other guest register below the reserved range keeps
        // its value.
        for (u32 reg = 0; reg < 246; ++reg) {
            if (reg == 3 || reg == 4) continue;
            REQUIRE(m.r[reg] == before.r[reg]);
        }
    }
}

TEST_CASE("Programs outside the reserved range are lowered without remaps", "[video_core][ampere][remap]") {
    // R245 is below the threshold (246) of a program without uniform registers: unchanged.
    const std::array<Instruction, 3> code{{Iadd3(245, 1, 2), Iadd3(3, 245, 1), Exit}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.register_remaps.empty());
    bool names_r245 = false;
    for (const u64 insn : lowered.instructions) names_r245 |= (insn & 0xff) == 245;
    REQUIRE(names_r245);
}

TEST_CASE("Register remaps survive the lowered-code storage next to the address map", "[video_core][ampere][remap]") {
    std::vector<u64> code(10, 0);
    code.push_back(0x5c10000000070000ULL);
    code.push_back(0);
    const std::vector<std::pair<u32, Shader::IR::RegisterRemap>> remaps{
        {0x50, {4, 56}}, {0x50, {5, 57}}, {0x68, {8, 58}}};
    Shader::Ampere::AppendNativeRegisterRemaps(code, remaps);
    const std::array<u32, 2> offsets{0, 1};
    Shader::Ampere::AppendNativeAddressMap(code, offsets);
    REQUIRE(Shader::Ampere::HasNativeAddressMap(code));
    const auto parsed = Shader::Ampere::NativeRegisterRemaps(code);
    REQUIRE(parsed.size() == 2);
    REQUIRE(parsed.at(0x50).size() == 2);
    REQUIRE(parsed.at(0x50)[0].from == 4);
    REQUIRE(parsed.at(0x50)[1].to == 57);
    REQUIRE(parsed.at(0x68)[0].from == 8);
    REQUIRE(parsed.at(0x68)[0].to == 58);
    // The address map still reads correctly behind the remap words.
    REQUIRE(Shader::Ampere::NativeProgramAddress(code, 16) == 0x58U);
    // Code without remaps parses to none (programs lowered before this change).
    std::vector<u64> plain(12, 0);
    Shader::Ampere::AppendNativeAddressMap(plain, offsets);
    REQUIRE(Shader::Ampere::NativeRegisterRemaps(plain).empty());
}

TEST_CASE("The IR emitter redirects remapped registers to extended registers", "[video_core][ampere][remap]") {
    Shader::ObjectPool<Shader::IR::Inst> inst_pool;
    Shader::ObjectPool<Shader::IR::Block> block_pool;
    Shader::IR::Block* const block = block_pool.Create(inst_pool);
    Shader::IR::IREmitter ir{*block};
    const std::array<Shader::IR::RegisterRemap, 1> remap{{{4, 56}}};
    ir.register_remap = remap;
    const auto value = ir.GetReg(Shader::IR::Reg::R4);
    ir.SetReg(Shader::IR::Reg::R4, value);
    (void)ir.GetReg(Shader::IR::Reg::R5);
    std::vector<Shader::IR::Reg> regs;
    for (const Shader::IR::Inst& inst : block->Instructions()) regs.push_back(inst.Arg(0).Reg());
    REQUIRE(regs.size() == 3);
    REQUIRE(regs[0] == Shader::IR::ExtendedReg(56));
    REQUIRE(regs[1] == Shader::IR::ExtendedReg(56));
    REQUIRE(regs[2] == Shader::IR::Reg::R5);
}

} // namespace

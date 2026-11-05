// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <span>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
using Shader::Ampere::ProgramCaptureState;

constexpr Instruction Nop{{0x00007918, 0, 0, 0}};
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};
// A data word that followed a rejected Yoshi program (loading area, yint-25 run 2): its opcode
// bits read as BSSY with a target far outside any program.
constexpr Instruction FollowingData{{0x2a7d8945, 0x3f800001, 0x2f457943, 0}};

struct Capture {
    ProgramCaptureState::Result status{};
    u32 index{};
};

Capture Run(std::span<const Instruction> code) {
    ProgramCaptureState capture;
    for (u32 index = 0; index < code.size(); ++index) {
        const auto status = capture.Observe(code[index], index);
        if (status != ProgramCaptureState::Result::Continue) return {status, index};
    }
    return {ProgramCaptureState::Result::Continue, static_cast<u32>(code.size())};
}

// Yoshi's subroutine form: the link (R55) is set a few instructions before CALL.REL.NOINC, the
// callee re-synchronises the warp with a register mask and returns through the link after the
// main program's EXIT; the program closes with a self-branch and is followed by other data.
std::vector<Instruction> YoshiCallProgram() {
    return {
        {{0x06007947, 0x00000013, 0x0b800000, 0}}, // 0: BRA.CONV UR6 -> 2 (mode bits 3)
        Nop,                                       // 1
        {{0x00377802, 0x00000050, 0x00000f00, 0}}, // 2: MOV R55, 0x50 (return address)
        Nop,                                       // 3
        {{0x00007944, 0x00000020, 0x03c00000, 0}}, // 4: CALL.REL.NOINC -> 7
        Nop,                                       // 5
        Exit,                                      // 6
        Nop,                                       // 7: callee
        {{0x00007348, 0x00000007, 0x03800000, 0}}, // 8: WARPSYNC R7
        {{0x37007950, 0xffffff60, 0x03c3ffff, 0}}, // 9: RET R55
        {{0x00007947, 0xfffffff0, 0x0383ffff, 0}}, // 10: BRA self
        Instruction{}, Instruction{},              // padding
        FollowingData,
    };
}
} // namespace

TEST_CASE("Ampere capture ends a program with linked NOINC calls at its return", "[video_core][ampere]") {
    const auto code = YoshiCallProgram();
    const Capture capture = Run(code);
    REQUIRE(capture.status == ProgramCaptureState::Result::Complete);
    REQUIRE(capture.index == 9);

    const std::span<const Instruction> captured{code.data(), capture.index + 1};
    const auto lowered = LowerToMaxwell(captured);
    INFO("offset=" << lowered.error_offset << " error=" << lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.source_size == captured.size() * sizeof(Instruction));
}

TEST_CASE("Ampere call links may be set early but not overwritten", "[video_core][ampere]") {
    auto code = YoshiCallProgram();
    code[3] = {{0x00377802, 0x00000000, 0x00000f00, 0}}; // MOV R55, 0 between link and call
    const std::span<const Instruction> captured{code.data(), 10};
    const auto lowered = LowerToMaxwell(captured);
    REQUIRE(!static_cast<bool>(lowered));
    REQUIRE(lowered.error == "native call linkage mode");
}

TEST_CASE("Ampere capture keeps NOINC calls without a link as jumps", "[video_core][ampere]") {
    // Title 4's form: no return address is set up, so the target is part of the main program.
    const std::vector<Instruction> unlinked{
        {{0x00007944, 0x00000010, 0x03c00000, 0}}, // 0: CALL.REL.NOINC -> 2
        Nop,
        Exit,
        FollowingData,
    };
    const Capture jump = Run(unlinked);
    REQUIRE(jump.status == ProgramCaptureState::Result::Complete);
    REQUIRE(jump.index == 2);

    // A predicated NOINC call is a conditional jump even with a matching MOV before it.
    const std::vector<Instruction> predicated{
        {{0x00377802, 0x00000020, 0x00000f00, 0}}, // 0: MOV R55, 0x20
        {{0x00000944, 0x00000010, 0x03c00000, 0}}, // 1: @P0 CALL.REL.NOINC -> 3
        Nop,
        Exit,
        FollowingData,
    };
    const Capture conditional = Run(predicated);
    REQUIRE(conditional.status == ProgramCaptureState::Result::Complete);
    REQUIRE(conditional.index == 3);
}

TEST_CASE("Ampere capture and lowering read BRA convergence modes", "[video_core][ampere]") {
    // .DIV (mode 2) to +0x10: never taken on Eden; the capture must not see an unaligned target.
    const std::vector<Instruction> code{
        {{0x00007947, 0x00000012, 0x03800000, 0}},
        Nop,
        Exit,
    };
    const Capture capture = Run(code);
    REQUIRE(capture.status == ProgramCaptureState::Result::Complete);
    REQUIRE(capture.index == 2);
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));

    auto reserved = code;
    reserved[0].words[1] = 0x00000011; // mode 1 is not a known form
    REQUIRE(!static_cast<bool>(LowerToMaxwell(reserved)));
}

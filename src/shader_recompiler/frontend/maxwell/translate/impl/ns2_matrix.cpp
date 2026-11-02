// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Warp-level matrix operations of native Switch 2 programs (Ampere HMMA / LDSM). The native
// lowering encodes them as NS2_HMMA / NS2_LDSM in an opcode range unused by Maxwell; they are
// emulated here with subgroup shuffles, using the fragment layouts of PTX mma.m16n8k16 and
// ldmatrix. All 32 lanes of the warp must execute them together, as on hardware.

#include <tuple>

#include "common/bit_field.h"
#include "common/common_types.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {
namespace {
IR::U32 FromLane(IR::IREmitter& ir, const IR::U32& value, const IR::U32& lane) {
    // Clamp 31, no segmentation: an index into the whole 32-lane warp.
    return ir.ShuffleIndex(value, lane, ir.Imm32(31), ir.Imm32(0));
}

std::pair<IR::F32, IR::F32> Halves(IR::IREmitter& ir, const IR::U32& value) {
    const IR::Value vector{ir.UnpackFloat2x16(value)};
    return {IR::F32{ir.FPConvert(32, IR::F16{ir.CompositeExtract(vector, 0)})},
            IR::F32{ir.FPConvert(32, IR::F16{ir.CompositeExtract(vector, 1)})}};
}

IR::U32 PackHalves(IR::IREmitter& ir, const IR::F32& low, const IR::F32& high) {
    return ir.PackFloat2x16(ir.CompositeConstruct(IR::F16{ir.FPConvert(16, low)},
                                                  IR::F16{ir.FPConvert(16, high)}));
}
} // Anonymous namespace

void TranslatorVisitor::NS2_HMMA(u64 insn) {
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest;
        BitField<8, 8, IR::Reg> a;
        BitField<20, 8, IR::Reg> b;
        BitField<28, 8, IR::Reg> c;
        BitField<36, 1, u64> f32_result;
    } const hmma{insn};
    // m16n8k16, F16 inputs. Lane t: group g = t / 4, thread q = t % 4.
    //   A (16x16): a0 = A[g][2q..], a1 = A[g+8][2q..], a2 = A[g][2q+8..], a3 = A[g+8][2q+8..]
    //   B (16x8):  b0 = B[2q..][g], b1 = B[2q+8..][g]
    //   C, D (16x8): c0 = {C[g][2q], C[g][2q+1]}, c1 = {C[g+8][2q], C[g+8][2q+1]}
    //   (F32 results: four registers c0..c3 in the same element order).
    const IR::U32 lane{ir.LaneId()};
    const IR::U32 group{ir.ShiftRightLogical(lane, ir.Imm32(2))};
    const IR::U32 thread{ir.BitwiseAnd(lane, ir.Imm32(3))};
    const bool f32_result{hmma.f32_result != 0};
    const IR::FpControl control{
        .no_contraction = false,
        .rounding = IR::FpRounding::RN,
        .fmz_mode = IR::FmzMode::None,
    };
    // Accumulators in F32: D[g][2q], D[g][2q+1], D[g+8][2q], D[g+8][2q+1].
    std::array<IR::F32, 4> acc;
    if (hmma.c == IR::Reg::RZ) {
        acc.fill(ir.Imm32(0.0f));
    } else if (f32_result) {
        for (int i = 0; i < 4; ++i) acc[i] = ir.BitCast<IR::F32>(X(hmma.c + i));
    } else {
        std::tie(acc[0], acc[1]) = Halves(ir, X(hmma.c));
        std::tie(acc[2], acc[3]) = Halves(ir, X(hmma.c + 1));
    }
    const std::array<IR::U32, 4> a{X(hmma.a), X(hmma.a + 1), X(hmma.a + 2), X(hmma.a + 3)};
    const std::array<IR::U32, 2> b{X(hmma.b), X(hmma.b + 1)};
    const IR::U32 row_lanes{ir.ShiftLeftLogical(group, ir.Imm32(2))};       // 4g
    const IR::U32 column_lanes{ir.ShiftLeftLogical(thread, ir.Imm32(3))};   // 4 * (2q)
    for (u32 j = 0; j < 4; ++j) {
        const IR::U32 row_lane{ir.IAdd(row_lanes, ir.Imm32(j))};
        const IR::U32 column0_lane{ir.IAdd(column_lanes, ir.Imm32(j))};
        const IR::U32 column1_lane{ir.IAdd(column_lanes, ir.Imm32(4 + j))};
        // k = 2j, 2j+1 (registers a0/a1, b0) and k = 2j+8, 2j+9 (a2/a3, b1).
        for (u32 half = 0; half < 2; ++half) {
            const auto [row_g0, row_g1] = Halves(ir, FromLane(ir, a[half * 2], row_lane));
            const auto [row_h0, row_h1] = Halves(ir, FromLane(ir, a[half * 2 + 1], row_lane));
            const auto [col0_0, col0_1] = Halves(ir, FromLane(ir, b[half], column0_lane));
            const auto [col1_0, col1_1] = Halves(ir, FromLane(ir, b[half], column1_lane));
            acc[0] = IR::F32{ir.FPFma(row_g0, col0_0, acc[0], control)};
            acc[0] = IR::F32{ir.FPFma(row_g1, col0_1, acc[0], control)};
            acc[1] = IR::F32{ir.FPFma(row_g0, col1_0, acc[1], control)};
            acc[1] = IR::F32{ir.FPFma(row_g1, col1_1, acc[1], control)};
            acc[2] = IR::F32{ir.FPFma(row_h0, col0_0, acc[2], control)};
            acc[2] = IR::F32{ir.FPFma(row_h1, col0_1, acc[2], control)};
            acc[3] = IR::F32{ir.FPFma(row_h0, col1_0, acc[3], control)};
            acc[3] = IR::F32{ir.FPFma(row_h1, col1_1, acc[3], control)};
        }
    }
    if (f32_result) {
        for (int i = 0; i < 4; ++i) X(hmma.dest + i, ir.BitCast<IR::U32>(acc[i]));
    } else {
        X(hmma.dest, PackHalves(ir, acc[0], acc[1]));
        X(hmma.dest + 1, PackHalves(ir, acc[2], acc[3]));
    }
}

void TranslatorVisitor::NS2_LDSM(u64 insn) {
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest;
        BitField<8, 8, IR::Reg> address;
        BitField<20, 24, s64> offset;
        BitField<44, 2, u64> count;
        BitField<46, 1, u64> transpose;
    } const ldsm{insn};
    // ldmatrix .b16 m8n8: lanes 8i..8i+7 hold the row addresses of matrix i; lane t receives
    // row t / 4, columns 2 * (t % 4) and + 1 of each matrix (transposed: rows 2 * (t % 4) and
    // + 1, column t / 4).
    const u32 matrices{1U << ldsm.count};
    const IR::U32 lane{ir.LaneId()};
    const IR::U32 row{ir.ShiftRightLogical(lane, ir.Imm32(2))};
    const IR::U32 pair{ir.BitwiseAnd(lane, ir.Imm32(3))};
    const IR::U32 address{ir.IAdd(X(ldsm.address), ir.Imm32(static_cast<s32>(ldsm.offset)))};
    for (u32 i = 0; i < matrices; ++i) {
        IR::U32 value;
        if (ldsm.transpose == 0) {
            const IR::U32 row_address{FromLane(ir, address, ir.IAdd(row, ir.Imm32(8 * i)))};
            value = IR::U32{ir.LoadShared(32, false, ir.IAdd(row_address, ir.ShiftLeftLogical(pair, ir.Imm32(2))))};
        } else {
            const IR::U32 first_row{ir.IAdd(ir.ShiftLeftLogical(pair, ir.Imm32(1)), ir.Imm32(8 * i))};
            const IR::U32 column_offset{ir.ShiftLeftLogical(row, ir.Imm32(1))};
            const IR::U32 low_address{FromLane(ir, address, first_row)};
            const IR::U32 high_address{FromLane(ir, address, ir.IAdd(first_row, ir.Imm32(1)))};
            const IR::U32 low{ir.LoadShared(16, false, ir.IAdd(low_address, column_offset))};
            const IR::U32 high{ir.LoadShared(16, false, ir.IAdd(high_address, column_offset))};
            value = ir.BitwiseOr(low, ir.ShiftLeftLogical(high, ir.Imm32(16)));
        }
        X(ldsm.dest + i, value);
    }
}

} // namespace Shader::Maxwell

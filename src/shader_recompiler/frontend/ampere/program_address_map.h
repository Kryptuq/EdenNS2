// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <unordered_map>
#include <utility>
#include <span>
#include <vector>
#include "common/common_types.h"
#include "shader_recompiler/frontend/ir/reg.h"

namespace Shader::Ampere {
constexpr u64 NativeAddressMapMagic = 0x50414d4e4544454eULL;
inline bool HasNativeAddressMap(std::span<const u64> code) {
    return code.size() >= 2 && code.back() == NativeAddressMapMagic;
}
inline void AppendNativeAddressMap(std::vector<u64>& code, std::span<const u32> offsets) {
    if (offsets.empty()) return;
    for (u32 index : offsets) code.push_back(0x50U + index * 8 + ((index + 1) / 3) * 8);
    code.push_back(offsets.size());
    code.push_back(NativeAddressMapMagic);
}
inline std::optional<u32> NativeProgramAddress(std::span<const u64> code, u32 address) {
    if (!HasNativeAddressMap(code) || address % 16) return {};
    const u64 count = code[code.size() - 2];
    if (!count || count > 65536 || count + 2 > code.size() || address / 16 >= count) return {};
    const size_t start = code.size() - static_cast<size_t>(count) - 2;
    const u64 target = code[start + address / 16];
    if (target < 0x50 || target >= start * 8 || target % 8 || target % 32 == 0) return {};
    return static_cast<u32>(target);
}

// Register redirections of lowered instructions, stored as unreachable data in front of the
// address map: one word per redirected register, (address << 32) | (from << 8) | to.
constexpr u64 NativeRegisterRemapMagic = 0x50414d4552474552ULL;
inline void AppendNativeRegisterRemaps(std::vector<u64>& code,
                                       std::span<const std::pair<u32, IR::RegisterRemap>> remaps) {
    if (remaps.empty()) return;
    for (const auto& [address, remap] : remaps)
        code.push_back((u64{address} << 32) | (u64{remap.from} << 8) | remap.to);
    code.push_back(remaps.size());
    code.push_back(NativeRegisterRemapMagic);
}
inline std::unordered_map<u32, std::vector<IR::RegisterRemap>> NativeRegisterRemaps(std::span<const u64> code) {
    std::unordered_map<u32, std::vector<IR::RegisterRemap>> result;
    size_t end = code.size();
    if (HasNativeAddressMap(code)) {
        const u64 count = code[code.size() - 2];
        if (count + 2 > code.size()) return result;
        end -= static_cast<size_t>(count) + 2;
    }
    if (end < 2 || code[end - 1] != NativeRegisterRemapMagic) return result;
    const u64 count = code[end - 2];
    if (count + 2 > end) return result;
    for (size_t i = end - 2 - static_cast<size_t>(count); i < end - 2; ++i) {
        const u64 word = code[i];
        result[static_cast<u32>(word >> 32)].push_back(
            IR::RegisterRemap{static_cast<u8>(word >> 8), static_cast<u8>(word)});
    }
    return result;
}
} // namespace Shader::Ampere

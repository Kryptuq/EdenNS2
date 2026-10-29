// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include "common/common_types.h"

namespace Service::AM {

struct NativeImageRequirements {
    u64 size;
    u64 alignment;
};

/// Output layout codes nn::codec puts in decoder image preferences (see below).
inline bool IsDecoderImageCode(u32 code) {
    return (code >= 1 && code <= 6) || (code >= 0xa && code <= 0xf);
}

// Fills in the layout of a descriptor that leaves it to the system (block height, pitch,
// depth and mip count all zero), as the video decoder does through
// nn::codec::MakeDecodeImagePreference. Uses the same block-linear layout games pick for
// four-byte color images: 128-row blocks (smaller for short images) and a pitch rounded up
// to whole 64-byte GOBs.
inline std::array<u32, 16> NormalizeNativeImagePreference(std::span<const u32> preference) {
    std::array<u32, 16> out{};
    std::copy_n(preference.begin(), (std::min)(preference.size(), out.size()), out.begin());
    // Video decoder output codes (nn::codec MakeDecodeImagePreference): 1-3 / 4-6 for the two
    // codec modes, 0xa-0xf for HDR outputs. Nothing is decoded into these images yet, so they
    // are sized generously as 4 bytes per pixel (8 for the 16-bit float codes 0xe/0xf) and
    // described as the four-byte color format the rest of the image code understands.
    const bool decoder_code = IsDecoderImageCode(out[1]);
    if (out[0] == 0 && decoder_code && out[3] == 0 && out[4] == 0 && out[5] == 0 &&
        out[6] == 0 && out[7] == 0) {
        const u32 bytes_per_pixel = (out[1] == 0xe || out[1] == 0xf) ? 8 : 4;
        const u32 width = (out[2] & 0xffffU) + 1;
        const u32 height = (out[2] >> 16) + 1;
        u32 block_log2 = 7;
        while (block_log2 > 3 && (1U << (block_log2 - 1)) >= height) {
            --block_log2;
        }
        if (out[1] != 1 && out[1] != 2) {
            out[1] = 1;
        }
        out[4] = block_log2;
        out[5] = (width * bytes_per_pixel + 63) / 64;
        out[6] = 1;
        out[7] = 1;
    }
    return out;
}

// Experimental inter-module block image layout, restricted to the observed
// four-byte color format. Row pitch is in 64-byte GOBs; block height is in pixels.
inline std::optional<NativeImageRequirements> CalculateNativeImageRequirements(
    std::span<const u32> raw_preference, u32 count) {
    if (raw_preference.size() != 16) {
        return std::nullopt;
    }
    const auto normalized = NormalizeNativeImagePreference(raw_preference);
    const std::span<const u32> preference{normalized};
    if (count != 1 || preference[0] != 0 ||
        (preference[1] != 1 && preference[1] != 2) || preference[3] != 0 || preference[6] != 1 ||
        preference[7] != 1 || preference[4] < 3 || preference[4] > 8) {
        return std::nullopt;
    }
    for (size_t i = 8; i < preference.size(); ++i) {
        if (preference[i] != 0) { return std::nullopt; }
    }
    const u64 width = (preference[2] & 0xffffU) + 1;
    const u64 height = (preference[2] >> 16) + 1;
    const u64 pitch_gobs = preference[5];
    if (pitch_gobs < (width * 4 + 63) / 64 || pitch_gobs > 4096) {
        return std::nullopt;
    }
    const u64 block_height = 1ULL << preference[4];
    const u64 aligned_height = (height + block_height - 1) & ~(block_height - 1);
    return NativeImageRequirements{pitch_gobs * 64 * aligned_height, 64 * block_height};
}

} // namespace Service::AM

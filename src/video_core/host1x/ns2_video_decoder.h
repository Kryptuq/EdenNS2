// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace Tegra::NS2 {

/// Software H.264 decoder for Switch 2 movie playback (nn::codec video decoder service).
/// Decodes one access unit at a time and returns the picture as packed NV12 (Y plane with
/// pitch = width, then the interleaved UV plane), the layout nn::codec clients read back.
class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();

    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    /// (Re)creates the decoder with codec configuration data (avcC record or Annex B SPS/PPS).
    bool Configure(std::span<const u8> config);

    /// Decodes one compressed frame. Returns true and fills `nv12` when a picture came out.
    bool Decode(std::span<const u8> data, std::vector<u8>& nv12, u32& width, u32& height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Tegra::NS2

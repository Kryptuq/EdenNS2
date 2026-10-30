// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/stb.h"
#include "video_core/texture_cache/ns2_frame_capture.h"

namespace VideoCommon::NS2Capture {
namespace {
using VideoCore::Surface::PixelFormat;

std::filesystem::path Directory() {
    auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "ns2_capture";
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return path;
}

float Half(u16 value) {
    const u32 sign = (value >> 15) & 1, exponent = (value >> 10) & 0x1f, mantissa = value & 0x3ff;
    float result;
    if (exponent == 0) result = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31) result = mantissa ? 0.0f : 65504.0f;
    else result = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    return sign ? -result : result;
}

// Unsigned small floats (5-bit exponent) of B10G11R11.
float SmallFloat(u32 value, u32 mantissa_bits) {
    const u32 exponent = value >> mantissa_bits, mantissa = value & ((1u << mantissa_bits) - 1);
    if (exponent == 0) return std::ldexp(static_cast<float>(mantissa), -14 - static_cast<int>(mantissa_bits));
    if (exponent == 31) return 65504.0f;
    return std::ldexp(static_cast<float>(mantissa | (1u << mantissa_bits)),
                      static_cast<int>(exponent) - 15 - static_cast<int>(mantissa_bits));
}

u8 Unorm(float value) {
    return static_cast<u8>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// HDR values: Reinhard and a display gamma, so dark and bright content both stay visible.
u8 Tonemap(float value) {
    value = std::max(value, 0.0f);
    return Unorm(std::pow(value / (1.0f + value), 1.0f / 2.2f));
}
} // namespace

bool WriteImage(std::span<const u8> data, u32 width, u32 height, u32 row_pixels, PixelFormat format,
                const std::string& name) {
    if (format == PixelFormat::D24_UNORM_S8_UINT || format == PixelFormat::D32_FLOAT ||
        format == PixelFormat::S8_UINT_D24_UNORM || format == PixelFormat::D32_FLOAT_S8_UINT) {
        // Depth: normalized between the smallest and largest value so geometry stands out.
        if (row_pixels < width || data.size() < static_cast<size_t>(row_pixels) * height * 4) return false;
        std::vector<float> depth(static_cast<size_t>(width) * height);
        float low = 1e30f, high = -1e30f;
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                u32 word;
                std::memcpy(&word, data.data() + (static_cast<size_t>(y) * row_pixels + x) * 4, 4);
                float value;
                if (format == PixelFormat::D24_UNORM_S8_UINT) value = static_cast<float>(word & 0xffffff) / 16777215.0f;
                else if (format == PixelFormat::S8_UINT_D24_UNORM) value = static_cast<float>(word >> 8) / 16777215.0f;
                else std::memcpy(&value, &word, 4);
                depth[static_cast<size_t>(y) * width + x] = value;
                low = std::min(low, value);
                high = std::max(high, value);
            }
        }
        std::vector<u8> gray(depth.size() * 3);
        for (size_t i = 0; i < depth.size(); ++i) {
            const u8 v = high > low ? Unorm((depth[i] - low) / (high - low)) : 0;
            gray[i * 3] = gray[i * 3 + 1] = gray[i * 3 + 2] = v;
        }
        AppendIndex(fmt::format("  depth {} range {} .. {}", name, low, high));
        std::vector<u8> jpeg;
        stbi_write_jpg_to_func(
            [](void* context, void* chunk, int size) {
                auto* buffer = static_cast<std::vector<u8>*>(context);
                const auto* bytes_in = static_cast<const u8*>(chunk);
                buffer->insert(buffer->end(), bytes_in, bytes_in + size);
            },
            &jpeg, static_cast<int>(width), static_cast<int>(height), 3, gray.data(), 90);
        std::ofstream file(Directory() / (name + ".jpg"), std::ios::binary);
        file.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
        return static_cast<bool>(file);
    }
    u32 bytes = 0;
    switch (format) {
    case PixelFormat::A8B8G8R8_UNORM: case PixelFormat::A8B8G8R8_SRGB:
    case PixelFormat::B8G8R8A8_UNORM: case PixelFormat::B8G8R8A8_SRGB:
    case PixelFormat::A2B10G10R10_UNORM: case PixelFormat::B10G11R11_FLOAT:
    case PixelFormat::R32_FLOAT: case PixelFormat::R16G16_FLOAT:
        bytes = 4; break;
    case PixelFormat::R16G16B16A16_FLOAT: bytes = 8; break;
    case PixelFormat::R32G32B32A32_FLOAT: bytes = 16; break;
    case PixelFormat::R16_FLOAT: case PixelFormat::R16_UNORM: case PixelFormat::R8G8_UNORM: bytes = 2; break;
    case PixelFormat::R8_UNORM: bytes = 1; break;
    default: return false;
    }
    if (row_pixels < width || data.size() < static_cast<size_t>(row_pixels) * (height - 1) * bytes + width * bytes)
        return false;
    std::vector<u8> rgb(static_cast<size_t>(width) * height * 3);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u8* p = data.data() + (static_cast<size_t>(y) * row_pixels + x) * bytes;
            u8* out = rgb.data() + (static_cast<size_t>(y) * width + x) * 3;
            u32 word{};
            std::memcpy(&word, p, std::min<u32>(bytes, 4));
            switch (format) {
            case PixelFormat::A8B8G8R8_UNORM: case PixelFormat::A8B8G8R8_SRGB:
                out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; break;
            case PixelFormat::B8G8R8A8_UNORM: case PixelFormat::B8G8R8A8_SRGB:
                out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; break;
            case PixelFormat::A2B10G10R10_UNORM:
                out[0] = static_cast<u8>((word & 0x3ff) >> 2);
                out[1] = static_cast<u8>(((word >> 10) & 0x3ff) >> 2);
                out[2] = static_cast<u8>(((word >> 20) & 0x3ff) >> 2);
                break;
            case PixelFormat::B10G11R11_FLOAT:
                out[0] = Tonemap(SmallFloat(word & 0x7ff, 6));
                out[1] = Tonemap(SmallFloat((word >> 11) & 0x7ff, 6));
                out[2] = Tonemap(SmallFloat(word >> 22, 5));
                break;
            case PixelFormat::R16G16B16A16_FLOAT: {
                u16 h[3];
                std::memcpy(h, p, sizeof(h));
                for (int i = 0; i < 3; ++i) out[i] = Tonemap(Half(h[i]));
                break;
            }
            case PixelFormat::R32G32B32A32_FLOAT: {
                float f[3];
                std::memcpy(f, p, sizeof(f));
                for (int i = 0; i < 3; ++i) out[i] = Tonemap(f[i]);
                break;
            }
            case PixelFormat::R16G16_FLOAT: {
                u16 h[2];
                std::memcpy(h, p, sizeof(h));
                out[0] = Tonemap(Half(h[0])); out[1] = Tonemap(Half(h[1])); out[2] = 0;
                break;
            }
            case PixelFormat::R32_FLOAT: {
                float f;
                std::memcpy(&f, p, sizeof(f));
                out[0] = out[1] = out[2] = Tonemap(f);
                break;
            }
            case PixelFormat::R16_FLOAT: out[0] = out[1] = out[2] = Tonemap(Half(static_cast<u16>(word))); break;
            case PixelFormat::R16_UNORM: out[0] = out[1] = out[2] = static_cast<u8>((word & 0xffff) >> 8); break;
            case PixelFormat::R8G8_UNORM: out[0] = p[0]; out[1] = p[1]; out[2] = 0; break;
            default: out[0] = out[1] = out[2] = p[0]; break;
            }
        }
    }
    std::vector<u8> jpeg;
    stbi_write_jpg_to_func(
        [](void* context, void* chunk, int size) {
            auto* buffer = static_cast<std::vector<u8>*>(context);
            const auto* bytes_in = static_cast<const u8*>(chunk);
            buffer->insert(buffer->end(), bytes_in, bytes_in + size);
        },
        &jpeg, static_cast<int>(width), static_cast<int>(height), 3, rgb.data(), 90);
    std::ofstream file(Directory() / (name + ".jpg"), std::ios::binary);
    file.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
    return static_cast<bool>(file);
}

void AppendIndex(const std::string& line) {
    std::ofstream file(Directory() / "index.txt", std::ios::app);
    file << line << '\n';
}

} // namespace VideoCommon::NS2Capture

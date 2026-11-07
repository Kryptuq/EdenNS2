// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Raw surface accesses wider than a texel of the bound image (EdenNS2 main b108, ported for
// yoshi-int-build-31): translated end to end (Maxwell -> IR -> SPIR-V) and the module inspected.

#include <array>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"

namespace {
Shader::Profile MakeProfile() {
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.unified_descriptor_binding = true;
    profile.support_descriptor_aliasing = true;
    profile.support_int8 = true;
    profile.support_uniform_and_storage_buffer_8bit = true;
    profile.support_storage_buffer_8bit = true;
    profile.support_int16 = true;
    profile.support_uniform_and_storage_buffer_16bit = true;
    profile.support_storage_buffer_16bit = true;
    profile.support_int64 = true;
    profile.support_float_controls = true;
    profile.support_separate_denorm_behavior = true;
    profile.support_separate_rounding_mode = true;
    profile.support_fp16_denorm_preserve = true;
    profile.support_fp32_denorm_preserve = true;
    profile.support_fp16_denorm_flush = true;
    profile.support_fp32_denorm_flush = true;
    profile.support_fp16_signed_zero_nan_preserve = true;
    profile.support_fp32_signed_zero_nan_preserve = true;
    profile.support_fp64_signed_zero_nan_preserve = true;
    profile.support_explicit_workgroup_layout = true;
    profile.support_workgroup_layout_8bit_access = true;
    profile.support_workgroup_layout_16bit_access = true;
    profile.support_quad_shuffles = true;
    profile.support_vote = true;
    profile.support_shuffle_relative = true;
    profile.supported_subgroup_stages = 0x7fffffff;
    profile.support_viewport_index_layer_non_geometry = true;
    profile.support_viewport_mask = true;
    profile.support_typeless_image_loads = true;
    profile.support_demote_to_helper_invocation = true;
    profile.support_int64_atomics = true;
    profile.support_shared_int64_atomics = true;
    profile.support_derivative_control = true;
    profile.support_geometry_shader_passthrough = true;
    profile.support_native_ndc = true;
    profile.support_scaled_attributes = true;
    profile.support_multi_viewport = true;
    profile.support_geometry_streams = true;
    profile.support_sampled_image_array_nonuniform_indexing = true;
    profile.support_storage_image_array_nonuniform_indexing = true;
    profile.support_uniform_texel_buffer_array_nonuniform_indexing = true;
    profile.support_storage_texel_buffer_array_nonuniform_indexing = true;
    profile.has_broken_fp16_float_controls = true;
    profile.max_subgroup_size = 32;
    profile.min_ssbo_alignment = 16;
    profile.max_user_clip_distances = 8;
    return profile;
}

Shader::HostTranslateInfo MakeHostInfo() {
    Shader::HostTranslateInfo info{};
    info.min_ssbo_alignment = 16;
    info.max_per_stage_descriptor_sampled_images = 1048576;
    info.max_per_stage_resources = 4294967295U;
    info.max_descriptor_set_samplers = 1048576;
    info.max_descriptor_set_uniform_buffers = 1048576;
    info.max_descriptor_set_uniform_buffers_dynamic = 15;
    info.max_descriptor_set_storage_buffers = 1048576;
    info.max_descriptor_set_storage_buffers_dynamic = 16;
    info.max_descriptor_set_sampled_images = 1048576;
    info.max_descriptor_set_storage_images = 1048576;
    info.max_descriptor_set_input_attachements = 1048576;
    info.support_float64 = true;
    info.support_float16 = true;
    info.support_int64 = true;
    info.support_snorm_render_buffer = true;
    info.support_viewport_index_layer = true;
    info.support_viewport_mask = true;
    return info;
}
} // namespace

namespace {
// Maxwell compute program: S2R R1/R2/R3 = TID.X/Y/Z; SULD.D.B32.2D R4, [R1], bound 0x99;
// SUST.D.B32.2D [R1], R4, bound 0x99; EXIT. The bound image's texel format is chosen by the test.
class RawSurfaceEnvironment final : public Shader::Environment {
public:
    explicit RawSurfaceEnvironment(Shader::TexturePixelFormat format_) : format{format_} {
        constexpr u64 bound = 0x99ULL << 36;
        constexpr u64 raw_2d_b32 = (1ULL << 51) | (1ULL << 52) | (3ULL << 33) | (4ULL << 20);
        const std::array<u64, 6> instructions{
            0xf0c8000000000000ULL | (0x21ULL << 20) | 1,                       // S2R R1, SR_TID.X
            0xf0c8000000000000ULL | (0x22ULL << 20) | 2,                       // S2R R2, SR_TID.Y
            0xf0c8000000000000ULL | (0x23ULL << 20) | 3,                       // S2R R3, SR_TID.Z
            0xeb00000000000000ULL | raw_2d_b32 | bound | (1ULL << 8) | 4,      // SULD.D.B32
            0xeb20000000000000ULL | raw_2d_b32 | bound | (1ULL << 8) | 4,      // SUST.D.B32
            0xe30000000007000fULL,                                             // EXIT
        };
        for (const u64 instruction : instructions) {
            if (code.size() % 4 == 0) code.push_back(0);
            code.push_back(instruction);
        }
        code.push_back(0xe30000000007000fULL);
        stage = Shader::Stage::Compute;
        start_address = 0;
    }
    u64 ReadInstruction(u32 address) override {
        return address / 8 < code.size() ? code[address / 8] : 0;
    }
    u32 ReadCbufValue(u32, u32 offset) override { return offset; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return format; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {8, 8, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}

private:
    std::vector<u64> code;
    Shader::TexturePixelFormat format;
};

struct RawSurfaceModule {
    u32 reads{};
    u32 writes{};
    std::vector<u32> image_formats; // spv::ImageFormat of storage images
    std::vector<u32> sampled_types; // result ids of their sampled types
    u32 u32_type{};
};

RawSurfaceModule InspectRawSurfaceModule(const std::vector<u32>& code) {
    RawSurfaceModule module;
    for (size_t i = 5; i < code.size();) {
        const u32 count = code[i] >> 16;
        const u32 opcode = code[i] & 0xffff;
        if (count == 0) break;
        if (opcode == 21 && code[i + 2] == 32 && code[i + 3] == 0) module.u32_type = code[i + 1]; // OpTypeInt 32 0
        if (opcode == 25 && code[i + 7] == 2) { // OpTypeImage, Sampled = 2 (storage)
            module.sampled_types.push_back(code[i + 2]);
            module.image_formats.push_back(code[i + 8]);
        }
        module.reads += opcode == 98;  // OpImageRead
        module.writes += opcode == 99; // OpImageWrite
        i += count;
    }
    return module;
}

std::vector<u32> TranslateRawSurface(Shader::TexturePixelFormat format) {
    RawSurfaceEnvironment env{format};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block;
    Shader::ObjectPool<Shader::IR::Inst> inst;
    Shader::ObjectPool<Shader::IR::Block> block;
    Shader::Maxwell::Flow::CFG cfg{env, flow_block, env.StartAddress()};
    auto program = Shader::Maxwell::TranslateProgram(inst, block, env, cfg, MakeHostInfo());
    Shader::Backend::Bindings binding;
    auto profile = MakeProfile();
    profile.support_typeless_image_loads = true;
    return Shader::Backend::SPIRV::EmitSPIRV(profile, {}, program, binding);
}
} // namespace

TEST_CASE("Raw surface accesses wider than a texel are split into texel-sized accesses", "[video_core][ns2-surface]") {
    constexpr u32 SpvR32ui = 33, SpvR16ui = 38, SpvR8ui = 39;
    SECTION("32-bit access to a 16-bit image: two texels through an R16ui view") {
        const auto code = TranslateRawSurface(Shader::TexturePixelFormat::R16_FLOAT);
        const auto module = InspectRawSurfaceModule(code);
        REQUIRE(module.image_formats == std::vector<u32>{SpvR16ui});
        REQUIRE(module.sampled_types == std::vector<u32>{module.u32_type});
        REQUIRE(module.reads == 2);
        REQUIRE(module.writes == 2);
    }
    SECTION("32-bit access to a 32-bit image is unchanged") {
        const auto module = InspectRawSurfaceModule(TranslateRawSurface(Shader::TexturePixelFormat::R32_FLOAT));
        REQUIRE(module.image_formats == std::vector<u32>{SpvR32ui});
        REQUIRE(module.reads == 1);
        REQUIRE(module.writes == 1);
    }
    SECTION("32-bit access to an 8-bit image: four texels") {
        const auto module = InspectRawSurfaceModule(TranslateRawSurface(Shader::TexturePixelFormat::R8_UNORM));
        REQUIRE(module.image_formats == std::vector<u32>{SpvR8ui});
        REQUIRE(module.reads == 4);
        REQUIRE(module.writes == 4);
    }
}


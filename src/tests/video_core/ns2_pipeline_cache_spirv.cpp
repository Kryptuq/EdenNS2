// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Offline check of a Vulkan pipeline cache (vulkan.bin): translates every cached pipeline to
// SPIR-V the way the Vulkan pipeline cache does and writes the modules to a directory, so they
// can be validated (spirv-val) without the game or a GPU. Set NS2_PIPELINE_CACHE to a copy of the
// cache file and NS2_SPIRV_OUT to the output directory; skipped otherwise.

#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stop_token>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/shader_environment.h"

namespace {
using Maxwell = Tegra::Engines::Maxwell3D::Regs;

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

// Native (Switch 2) program lowered to Maxwell code laid out as InitializeNativeCode does:
// a zeroed SPH, then a scheduling word before every three instructions.
class LoweredEnvironment final : public Shader::Environment {
public:
    explicit LoweredEnvironment(const Shader::Ampere::LoweredProgram& lowered, Shader::Stage stage_) {
        code.resize(sizeof(Shader::ProgramHeader) / sizeof(u64));
        for (const u64 instruction : lowered.instructions) {
            if ((code.size() * sizeof(u64)) % 32 == 0) code.push_back(0);
            code.push_back(instruction);
        }
        code.push_back(0);
        code.push_back(0xe30000000007000fULL); // EXIT guard
        bound = lowered.required_texture_buffer == ~0U ? 2 : lowered.required_texture_buffer;
        stage = stage_;
        start_address = 0;
    }
    u64 ReadInstruction(u32 address) override {
        return address / 8 < code.size() ? code[address / 8] : 0;
    }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return bound; }
    u32 LocalMemorySize() const override { return 0x1000; }
    u32 SharedMemorySize() const override { return 0x8000; }
    std::array<u32, 3> WorkgroupSize() const override { return {64, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}
    void SetHeader(const void* header) {
        std::memcpy(code.data(), header, sizeof(Shader::ProgramHeader));
        std::memcpy(&sph, header, sizeof(sph));
    }

private:
    std::vector<u64> code;
    u32 bound{};
};

void Write(const std::filesystem::path& path, const std::vector<u32>& code) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(code.data()), static_cast<std::streamsize>(code.size() * 4));
}
} // namespace

TEST_CASE("NS2 pipeline cache to SPIR-V", "[video_core][ns2-cache]") {
    const char* cache = std::getenv("NS2_PIPELINE_CACHE");
    const char* out_dir = std::getenv("NS2_SPIRV_OUT");
    if (!cache || !out_dir) {
        SKIP("NS2_PIPELINE_CACHE/NS2_SPIRV_OUT not set");
    }
    std::filesystem::create_directories(out_dir);
    const auto profile = MakeProfile();
    const auto host_info = MakeHostInfo();
    size_t graphics{}, compute{}, failed{};
    std::stop_source stop;
    VideoCommon::LoadPipelines(
        stop.get_token(), cache, 18,
        [&](std::ifstream& file, VideoCommon::FileEnvironment env) {
            Vulkan::ComputePipelineCacheKey key;
            file.read(reinterpret_cast<char*>(&key), sizeof(key));
            ++compute;
            try {
                Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block;
                Shader::ObjectPool<Shader::IR::Inst> inst;
                Shader::ObjectPool<Shader::IR::Block> block;
                Shader::Maxwell::Flow::CFG cfg{env, flow_block, env.StartAddress()};
                auto program = Shader::Maxwell::TranslateProgram(inst, block, env, cfg, host_info);
                Shader::Backend::Bindings binding;
                const auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, {}, program, binding);
                Write(std::filesystem::path{out_dir} / fmt::format("c_{:016x}.spv", key.Hash()), code);
            } catch (const std::exception& e) {
                ++failed;
                fmt::print("compute {:016x}: {}\n", key.Hash(), e.what());
            }
        },
        [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment> envs) {
            Vulkan::GraphicsPipelineCacheKey key;
            file.read(reinterpret_cast<char*>(&key), sizeof(key));
            ++graphics;
            try {
                Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block;
                Shader::ObjectPool<Shader::IR::Inst> inst;
                Shader::ObjectPool<Shader::IR::Block> block;
                std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;
                const bool uses_vertex_a{key.unique_hashes[0] != 0};
                const bool uses_vertex_b{key.unique_hashes[1] != 0};
                size_t env_index{};
                for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
                    if (key.unique_hashes[index] == 0) continue;
                    auto& env = envs[env_index++];
                    const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
                    Shader::Maxwell::Flow::CFG cfg(env, flow_block, cfg_offset, index == 0);
                    if (!uses_vertex_a || index != 1) {
                        programs[index] = Shader::Maxwell::TranslateProgram(inst, block, env, cfg, host_info);
                    } else {
                        auto vb = Shader::Maxwell::TranslateProgram(inst, block, env, cfg, host_info);
                        programs[index] = Shader::Maxwell::MergeDualVertexPrograms(programs[0], vb, env);
                    }
                }
                const Shader::IR::Program* previous{};
                Shader::Backend::Bindings binding;
                for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0; index < Maxwell::MaxShaderProgram; ++index) {
                    if (key.unique_hashes[index] == 0) continue;
                    auto& program = programs[index];
                    Shader::RuntimeInfo info;
                    if (previous) {
                        info.previous_stage_stores = previous->info.stores;
                        info.previous_stage_legacy_stores_mapping = previous->info.legacy_stores_mapping;
                    } else {
                        info.previous_stage_stores.mask.set();
                    }
                    if (program.stage == Shader::Stage::VertexB) {
                        info.generic_input_types.fill(Shader::AttributeType::Float);
                    }
                    Shader::Maxwell::ConvertLegacyToGeneric(program, info);
                    if (const char* dump = std::getenv("NS2_DUMP_HASH");
                        dump && std::strtoull(dump, nullptr, 16) == key.unique_hashes[index]) {
                        fmt::print("{}\n", Shader::IR::DumpProgram(program));
                    }
                    const auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, info, program, binding);
                    Write(std::filesystem::path{out_dir} /
                              fmt::format("g_{:016x}_{}_{:016x}.spv", key.Hash(), index, key.unique_hashes[index]),
                          code);
                    previous = &program;
                }
            } catch (const std::exception& e) {
                ++failed;
                fmt::print("graphics {:016x}: {}\n", key.Hash(), e.what());
            }
        });
    fmt::print("pipelines: {} graphics, {} compute, {} failed to translate\n", graphics, compute, failed);
}

// Lowers native programs (raw 16-byte instructions, one program per file in NS2_NATIVE_DIR, as
// dumped from LogRejectedNativeShader) and runs them through the Maxwell frontend and the SPIR-V
// backend as compute shaders. Graphics-only operations (attributes) may fail here legitimately.
TEST_CASE("NS2 native lowering", "[video_core][ns2-lower]") {
    const char* dir = std::getenv("NS2_NATIVE_DIR");
    if (!dir) {
        SKIP("NS2_NATIVE_DIR not set");
    }
    const auto profile = MakeProfile();
    const auto host_info = MakeHostInfo();
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        std::ifstream file(entry.path(), std::ios::binary);
        std::vector<Shader::Ampere::Instruction> native;
        Shader::Ampere::Instruction insn;
        while (file.read(reinterpret_cast<char*>(&insn), sizeof(insn))) native.push_back(insn);
        const auto lowered = Shader::Ampere::LowerToMaxwell(native);
        const auto name = entry.path().filename().string();
        if (!lowered) {
            fmt::print("{}: lowering failed at {:#x}: {}\n", name, lowered.error_offset, lowered.error);
            continue;
        }
        try {
            LoweredEnvironment env{lowered, Shader::Stage::Compute};
            Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block;
            Shader::ObjectPool<Shader::IR::Inst> inst;
            Shader::ObjectPool<Shader::IR::Block> block;
            Shader::Maxwell::Flow::CFG cfg{env, flow_block, sizeof(Shader::ProgramHeader)};
            auto program = Shader::Maxwell::TranslateProgram(inst, block, env, cfg, host_info);
            Shader::Backend::Bindings binding;
            const auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, {}, program, binding);
            if (const char* out = std::getenv("NS2_SPIRV_OUT")) {
                Write(std::filesystem::path{out} / (entry.path().stem().string() + ".spv"), code);
            }
            fmt::print("{}: ok ({} lowered instructions)\n", name, lowered.instructions.size());
        } catch (const std::exception& e) {
            fmt::print("{}: translation failed: {}\n", name, e.what());
        }
    }
}

// Translates one dumped native geometry shader (native header followed by code, as logged by
// "NS2 native dump") through the lowering and the Maxwell frontend into SPIR-V.
TEST_CASE("NS2 native geometry shader", "[video_core][ns2-lower-gs]") {
    const char* path = std::getenv("NS2_NATIVE_SHADER");
    const char* out = std::getenv("NS2_SPIRV_OUT");
    if (!path || !out) {
        SKIP("NS2_NATIVE_SHADER/NS2_SPIRV_OUT not set");
    }
    std::ifstream file(path, std::ios::binary);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    constexpr size_t NativeHeaderSize = 0x80;
    REQUIRE(bytes.size() > NativeHeaderSize);
    std::vector<Shader::Ampere::Instruction> native((bytes.size() - NativeHeaderSize) / sizeof(Shader::Ampere::Instruction));
    std::memcpy(native.data(), bytes.data() + NativeHeaderSize, native.size() * sizeof(Shader::Ampere::Instruction));
    // The program ends at its trailing self-branch.
    for (size_t i = 0; i < native.size(); ++i) {
        if ((native[i].words[0] & 0xfff) == 0x947 && native[i].words[1] == 0xfffffff0) {
            native.resize(i + 1);
            break;
        }
    }
    const auto lowered = Shader::Ampere::LowerToMaxwell(native);
    INFO(lowered.error);
    REQUIRE(lowered);
    LoweredEnvironment env{lowered, Shader::Stage::Geometry};
    env.SetHeader(bytes.data());
    const auto profile = MakeProfile();
    const auto host_info = MakeHostInfo();
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block;
    Shader::ObjectPool<Shader::IR::Inst> inst;
    Shader::ObjectPool<Shader::IR::Block> block;
    Shader::Maxwell::Flow::CFG cfg{env, flow_block, sizeof(Shader::ProgramHeader)};
    auto program = Shader::Maxwell::TranslateProgram(inst, block, env, cfg, host_info);
    Shader::RuntimeInfo info;
    info.input_topology = Shader::InputTopology::Triangles;
    // As written by the vertex shader that feeds it: position, generic 0 (xy) and 1 (x).
    for (const auto attribute : {Shader::IR::Attribute::PositionX, Shader::IR::Attribute::PositionY,
                                 Shader::IR::Attribute::PositionZ, Shader::IR::Attribute::PositionW,
                                 Shader::IR::Attribute::Generic0X, Shader::IR::Attribute::Generic0Y,
                                 Shader::IR::Attribute::Generic1X}) {
        info.previous_stage_stores.Set(attribute);
    }
    Shader::Backend::Bindings binding;
    const auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, info, program, binding);
    Write(std::filesystem::path{out} / "geometry.spv", code);
    fmt::print("geometry shader: {} lowered instructions, {} SPIR-V words, output topology {}, max vertices {}\n",
               lowered.instructions.size(), code.size(), static_cast<u32>(program.output_topology),
               program.output_vertices);
}

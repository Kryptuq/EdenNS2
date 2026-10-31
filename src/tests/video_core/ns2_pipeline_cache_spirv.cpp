// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Offline check of a Vulkan pipeline cache (vulkan.bin): translates every cached pipeline to
// SPIR-V the way the Vulkan pipeline cache does and writes the modules to a directory, so they
// can be validated (spirv-val) without the game or a GPU. Set NS2_PIPELINE_CACHE to a copy of the
// cache file and NS2_SPIRV_OUT to the output directory; skipped otherwise.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stop_token>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
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

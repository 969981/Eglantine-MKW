// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Offline shader translation benchmark.
//
// Loads every pipeline of a Vulkan disk shader cache (vulkan.bin) and runs, single-threaded, what
// the Vulkan pipeline cache runs before the driver compile: CFG, TranslateProgram (and the dual
// vertex merge), ConvertLegacyToGeneric and EmitSPIRV. No Vulkan device is needed: the profile is
// a fixed one, close to an AMD RDNA2 GPU on the proprietary Windows driver.
//
// It reports the total time, the time per pass (Shader::PassTimer) and the slowest pipelines, and
// can write a hash of every emitted SPIR-V module, to check that an optimization does not change
// the output.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/pass_timer.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/shader_environment.h"
#include "video_core/transform_feedback.h"

namespace {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;
using Clock = std::chrono::steady_clock;
using Shader::PassTimer::Pass;
using Vulkan::FixedPipelineState;

// Must match CACHE_VERSION in vk_pipeline_cache.cpp: LoadPipelines deletes files of another version.
constexpr u32 CACHE_VERSION = 18;

struct Pipeline {
    bool is_compute{};
    Vulkan::ComputePipelineCacheKey compute_key{};
    Vulkan::GraphicsPipelineCacheKey graphics_key{};
    std::vector<VideoCommon::FileEnvironment> envs;
};

struct BuildResult {
    // One hash per emitted SPIR-V module, in emission order, or an error message.
    std::vector<u64> module_hashes;
    std::string error;
    u64 nanoseconds{};
};

// ---- Copied from vk_pipeline_cache.cpp (anonymous namespace there), with the device queries
// replaced by what an RDNA2 GPU answers: transform feedback supported, not MoltenVK.

Shader::OutputTopology MaxwellToOutputTopology(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return Shader::OutputTopology::PointList;
    case Maxwell::PrimitiveTopology::LineStrip:
        return Shader::OutputTopology::LineStrip;
    default:
        return Shader::OutputTopology::TriangleStrip;
    }
}

Shader::CompareFunction MaxwellToCompareFunction(Maxwell::ComparisonOp comparison) {
    switch (comparison) {
    case Maxwell::ComparisonOp::Never_D3D:
    case Maxwell::ComparisonOp::Never_GL:
        return Shader::CompareFunction::Never;
    case Maxwell::ComparisonOp::Less_D3D:
    case Maxwell::ComparisonOp::Less_GL:
        return Shader::CompareFunction::Less;
    case Maxwell::ComparisonOp::Equal_D3D:
    case Maxwell::ComparisonOp::Equal_GL:
        return Shader::CompareFunction::Equal;
    case Maxwell::ComparisonOp::LessEqual_D3D:
    case Maxwell::ComparisonOp::LessEqual_GL:
        return Shader::CompareFunction::LessThanEqual;
    case Maxwell::ComparisonOp::Greater_D3D:
    case Maxwell::ComparisonOp::Greater_GL:
        return Shader::CompareFunction::Greater;
    case Maxwell::ComparisonOp::NotEqual_D3D:
    case Maxwell::ComparisonOp::NotEqual_GL:
        return Shader::CompareFunction::NotEqual;
    case Maxwell::ComparisonOp::GreaterEqual_D3D:
    case Maxwell::ComparisonOp::GreaterEqual_GL:
        return Shader::CompareFunction::GreaterThanEqual;
    case Maxwell::ComparisonOp::Always_D3D:
    case Maxwell::ComparisonOp::Always_GL:
        return Shader::CompareFunction::Always;
    }
    return {};
}

Shader::AttributeType CastAttributeType(const FixedPipelineState::VertexAttribute& attr) {
    if (attr.enabled == 0) {
        return Shader::AttributeType::Disabled;
    }
    switch (attr.Type()) {
    case Maxwell::VertexAttribute::Type::UnusedEnumDoNotUseBecauseItWillGoAway:
        return Shader::AttributeType::Disabled;
    case Maxwell::VertexAttribute::Type::SNorm:
    case Maxwell::VertexAttribute::Type::UNorm:
    case Maxwell::VertexAttribute::Type::Float:
        return Shader::AttributeType::Float;
    case Maxwell::VertexAttribute::Type::SInt:
        return Shader::AttributeType::SignedInt;
    case Maxwell::VertexAttribute::Type::UInt:
        return Shader::AttributeType::UnsignedInt;
    case Maxwell::VertexAttribute::Type::UScaled:
        return Shader::AttributeType::UnsignedScaled;
    case Maxwell::VertexAttribute::Type::SScaled:
        return Shader::AttributeType::SignedScaled;
    }
    return Shader::AttributeType::Float;
}

Shader::AttributeType AttributeType(const FixedPipelineState& state, size_t index) {
    switch (state.DynamicAttributeType(index)) {
    case 0:
        return Shader::AttributeType::Disabled;
    case 1:
        return Shader::AttributeType::Float;
    case 2:
        return Shader::AttributeType::SignedInt;
    case 3:
        return Shader::AttributeType::UnsignedInt;
    }
    return Shader::AttributeType::Disabled;
}

Shader::RuntimeInfo MakeRuntimeInfo(std::span<const Shader::IR::Program> programs,
                                    const Vulkan::GraphicsPipelineCacheKey& key,
                                    const Shader::IR::Program& program,
                                    const Shader::IR::Program* previous_program) {
    Shader::RuntimeInfo info;
    if (previous_program) {
        info.previous_stage_stores = previous_program->info.stores;
        info.previous_stage_legacy_stores_mapping = previous_program->info.legacy_stores_mapping;
        if (previous_program->is_geometry_passthrough) {
            info.previous_stage_stores.mask |= previous_program->info.passthrough.mask;
        }
    } else {
        info.previous_stage_stores.mask.set();
    }
    const Shader::Stage stage{program.stage};
    const bool has_geometry{key.unique_hashes[4] != 0 && !programs[4].is_geometry_passthrough};
    const bool gl_ndc{key.state.ndc_minus_one_to_one != 0};
    const float point_size{std::bit_cast<float>(key.state.point_size)};
    switch (stage) {
    case Shader::Stage::VertexB:
        if (!has_geometry) {
            if (key.state.topology == Maxwell::PrimitiveTopology::Points) {
                info.fixed_state_point_size = point_size;
            }
            if (key.state.xfb_enabled) {
                auto [varyings, count] =
                    VideoCommon::MakeTransformFeedbackVaryings(key.state.xfb_state);
                info.xfb_varyings = varyings;
                info.xfb_count = count;
            }
            info.convert_depth_mode = gl_ndc;
        }
        if (key.state.dynamic_vertex_input) {
            for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
                info.generic_input_types[index] = AttributeType(key.state, index);
            }
        } else {
            std::ranges::transform(key.state.attributes, info.generic_input_types.begin(),
                                   &CastAttributeType);
        }
        break;
    case Shader::Stage::TessellationEval:
        info.tess_clockwise = key.state.tessellation_clockwise != 0;
        info.tess_primitive = [&key] {
            switch (static_cast<Maxwell::Tessellation::DomainType>(
                key.state.tessellation_primitive.Value())) {
            case Maxwell::Tessellation::DomainType::Isolines:
                return Shader::TessPrimitive::Isolines;
            case Maxwell::Tessellation::DomainType::Triangles:
                return Shader::TessPrimitive::Triangles;
            case Maxwell::Tessellation::DomainType::Quads:
                return Shader::TessPrimitive::Quads;
            }
            return Shader::TessPrimitive::Triangles;
        }();
        info.tess_spacing = [&] {
            const u32 raw{key.state.tessellation_spacing};
            switch (static_cast<Maxwell::Tessellation::Spacing>(raw)) {
            case Maxwell::Tessellation::Spacing::Integer:
                return Shader::TessSpacing::Equal;
            case Maxwell::Tessellation::Spacing::FractionalOdd:
                return Shader::TessSpacing::FractionalOdd;
            case Maxwell::Tessellation::Spacing::FractionalEven:
                return Shader::TessSpacing::FractionalEven;
            }
            return Shader::TessSpacing::Equal;
        }();
        break;
    case Shader::Stage::Geometry:
        if (program.output_topology == Shader::OutputTopology::PointList) {
            info.fixed_state_point_size = point_size;
        }
        if (key.state.xfb_enabled != 0) {
            auto [varyings, count] =
                VideoCommon::MakeTransformFeedbackVaryings(key.state.xfb_state);
            info.xfb_varyings = varyings;
            info.xfb_count = count;
        }
        info.convert_depth_mode = gl_ndc;
        break;
    case Shader::Stage::Fragment:
        info.alpha_test_func = MaxwellToCompareFunction(
            key.state.UnpackComparisonOp(key.state.alpha_test_func.Value()));
        info.alpha_test_reference = std::bit_cast<float>(key.state.alpha_test_ref);
        info.dual_source_blend = key.state.attachment0_dual_source_blend != 0;
        break;
    default:
        break;
    }
    switch (key.state.topology) {
    case Maxwell::PrimitiveTopology::Points:
        info.input_topology = Shader::InputTopology::Points;
        break;
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineLoop:
    case Maxwell::PrimitiveTopology::LineStrip:
        info.input_topology = Shader::InputTopology::Lines;
        break;
    case Maxwell::PrimitiveTopology::Triangles:
    case Maxwell::PrimitiveTopology::TriangleStrip:
    case Maxwell::PrimitiveTopology::TriangleFan:
    case Maxwell::PrimitiveTopology::Quads:
    case Maxwell::PrimitiveTopology::QuadStrip:
    case Maxwell::PrimitiveTopology::Polygon:
    case Maxwell::PrimitiveTopology::Patches:
        info.input_topology = Shader::InputTopology::Triangles;
        break;
    case Maxwell::PrimitiveTopology::LinesAdjacency:
    case Maxwell::PrimitiveTopology::LineStripAdjacency:
        info.input_topology = Shader::InputTopology::LinesAdjacency;
        break;
    case Maxwell::PrimitiveTopology::TrianglesAdjacency:
    case Maxwell::PrimitiveTopology::TriangleStripAdjacency:
        info.input_topology = Shader::InputTopology::TrianglesAdjacency;
        break;
    }
    info.force_early_z = key.state.early_z != 0;
    info.y_negate = key.state.y_negate != 0;
    return info;
}

// ---- Fixed host description: Radeon RX 6000 series, AMD proprietary driver, Vulkan 1.3.

Shader::Profile MakeProfile() {
    return Shader::Profile{
        .supported_spirv = 0x00010600U,
        .unified_descriptor_binding = true,
        .support_descriptor_aliasing = true,
        .support_int8 = true,
        .support_uniform_and_storage_buffer_8bit = true,
        .support_storage_buffer_8bit = true,
        .support_int16 = true,
        .support_uniform_and_storage_buffer_16bit = true,
        .support_storage_buffer_16bit = true,
        .support_int64 = true,
        .support_vertex_instance_id = false,
        .support_float_controls = true,
        .support_separate_denorm_behavior = true,
        .support_separate_rounding_mode = true,
        .support_fp16_denorm_preserve = true,
        .support_fp32_denorm_preserve = true,
        .support_fp16_denorm_flush = false,
        .support_fp32_denorm_flush = true,
        .support_fp16_signed_zero_nan_preserve = true,
        .support_fp32_signed_zero_nan_preserve = true,
        .support_fp64_signed_zero_nan_preserve = true,
        .support_explicit_workgroup_layout = true,
        .support_workgroup_layout_8bit_access = true,
        .support_workgroup_layout_16bit_access = true,
        .support_shader_quad_control = false,
        .support_quad_shuffles = true,
        .support_vote = true,
        .support_shuffle_relative = true,
        .supported_subgroup_stages = 0x7F,
        .support_viewport_index_layer_non_geometry = true,
        .support_viewport_mask = false,
        .support_typeless_image_loads = true,
        .support_demote_to_helper_invocation = true,
        .support_int64_atomics = true,
        .support_shared_int64_atomics = true,
        .support_derivative_control = true,
        .support_geometry_shader_passthrough = false,
        .support_native_ndc = true,
        .support_scaled_attributes = true,
        .support_multi_viewport = true,
        .support_geometry_streams = true,
        .support_sampled_image_array_nonuniform_indexing = true,
        .support_storage_image_array_nonuniform_indexing = true,
        .support_uniform_texel_buffer_array_nonuniform_indexing = true,
        .support_storage_texel_buffer_array_nonuniform_indexing = true,
        .warp_size_potentially_larger_than_guest = true,
        .lower_left_origin_mode = false,
        .need_declared_frag_colors = false,
        .need_gather_subpixel_offset = true,
        .has_broken_spirv_clamp = false,
        .has_broken_spirv_position_input = false,
        .has_broken_unsigned_image_offsets = false,
        .has_broken_signed_operations = false,
        .has_broken_fp16_float_controls = false,
        .has_broken_fp32_denorm_flush = false,
        .ignore_nan_fp_comparisons = false,
        .has_broken_spirv_subgroup_mask_vector_extract_dynamic = false,
        .has_broken_spirv_subgroup_shuffle = false,
        .max_subgroup_size = 64,
        .has_broken_robust = false,
        .min_ssbo_alignment = 4,
        .max_user_clip_distances = 8,
    };
}

Shader::HostTranslateInfo MakeHostInfo() {
    Shader::HostTranslateInfo info{
        .min_ssbo_alignment = 4,
        .max_per_stage_descriptor_sampled_images = 1048576,
        .max_per_stage_resources = 1048576,
        .max_descriptor_set_samplers = 1048576,
        .max_descriptor_set_uniform_buffers = 1048576,
        .max_descriptor_set_uniform_buffers_dynamic = 8,
        .max_descriptor_set_storage_buffers = 1048576,
        .max_descriptor_set_storage_buffers_dynamic = 8,
        .max_descriptor_set_sampled_images = 1048576,
        .max_descriptor_set_storage_images = 1048576,
        .max_descriptor_set_input_attachements = 1048576,
        .support_float64 = true,
        .support_float16 = true,
        .support_int64 = true,
        .needs_demote_reorder = true,
        .support_snorm_render_buffer = true,
        .support_viewport_index_layer = true,
        .support_viewport_mask = false,
        .support_geometry_shader_passthrough = false,
        .support_conditional_barrier = true,
        .single_lane_geometry_subgroups = false,
    };
    info.ApplyDescriptorLimitPolicy();
    return info;
}

struct Pools {
    void ReleaseContents() {
        flow_block.ReleaseContents();
        block.ReleaseContents();
        inst.ReleaseContents();
    }

    // Same initial sizes as Vulkan::ShaderPools.
    Shader::ObjectPool<Shader::IR::Inst> inst{8192};
    Shader::ObjectPool<Shader::IR::Block> block{32};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block{32};
};

u64 HashCode(const std::vector<u32>& code) {
    return Common::CityHash64(reinterpret_cast<const char*>(code.data()),
                              code.size() * sizeof(u32));
}

// Mirrors PipelineCache::CreateGraphicsPipeline up to the shader modules.
void BuildGraphics(Pools& pools, const Shader::Profile& profile,
                   const Shader::HostTranslateInfo& host_info, Pipeline& pipeline,
                   BuildResult& result) {
    const auto& key{pipeline.graphics_key};
    std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;
    const bool uses_vertex_a{key.unique_hashes[0] != 0};
    const bool uses_vertex_b{key.unique_hashes[1] != 0};
    Shader::IR::Program* layer_source_program{};
    size_t env_index{0};
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && is_emulated_stage) {
            programs[index] = Shader::Maxwell::GenerateGeometryPassthrough(
                pools.inst, pools.block, host_info, *layer_source_program,
                MaxwellToOutputTopology(key.state.topology));
            continue;
        }
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{pipeline.envs.at(env_index)};
        ++env_index;
        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        if (!uses_vertex_a || index != 1) {
            programs[index] =
                Shader::Maxwell::TranslateProgram(pools.inst, pools.block, env, cfg, host_info);
        } else {
            auto program_vb{
                Shader::Maxwell::TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
            programs[index] =
                Shader::Maxwell::MergeDualVertexPrograms(programs[0], program_vb, env);
        }
        if (Shader::Optimization::HasBrokenPattern(programs[index])) {
            result.error = "skipped: known broken pattern";
            return;
        }
        if (programs[index].info.requires_layer_emulation) {
            layer_source_program = &programs[index];
        }
    }
    const Shader::IR::Program* previous_stage{};
    Shader::Backend::Bindings binding;
    for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0;
         index < Maxwell::MaxShaderProgram; ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && !is_emulated_stage) {
            continue;
        }
        Shader::IR::Program& program{programs[index]};
        const auto runtime_info{MakeRuntimeInfo(programs, key, program, previous_stage)};
        Shader::Maxwell::ConvertLegacyToGeneric(program, runtime_info);
        const std::vector<u32> code{
            Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, binding)};
        result.module_hashes.push_back(HashCode(code));
        previous_stage = &program;
    }
}

// Mirrors PipelineCache::CreateComputePipeline up to the shader module.
void BuildCompute(Pools& pools, const Shader::Profile& profile,
                  const Shader::HostTranslateInfo& host_info, Pipeline& pipeline,
                  BuildResult& result) {
    Shader::Environment& env{pipeline.envs.front()};
    Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};
    auto program{Shader::Maxwell::TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
    const std::vector<u32> code{Shader::Backend::SPIRV::EmitSPIRV(profile, program)};
    result.module_hashes.push_back(HashCode(code));
}

BuildResult Build(Pools& pools, const Shader::Profile& profile,
             const Shader::HostTranslateInfo& host_info, Pipeline& pipeline) {
    BuildResult result;
    // The pipeline cache releases its pools before each runtime build.
    pools.ReleaseContents();
    const auto start{Clock::now()};
    try {
        if (pipeline.is_compute) {
            BuildCompute(pools, profile, host_info, pipeline, result);
        } else {
            BuildGraphics(pools, profile, host_info, pipeline, result);
        }
    } catch (const std::exception& e) {
        result.module_hashes.clear();
        result.error = fmt::format("exception: {}", e.what());
    }
    result.nanoseconds = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    return result;
}

u64 PipelineId(const Pipeline& pipeline) {
    if (pipeline.is_compute) {
        return pipeline.compute_key.unique_hash;
    }
    return Common::CityHash64(reinterpret_cast<const char*>(pipeline.graphics_key.unique_hashes.data()),
                              sizeof(pipeline.graphics_key.unique_hashes));
}

void PrintUsage() {
    std::fputs("usage: eden-shader-bench <vulkan.bin> [--repeat N] [--hashes FILE] [--scale 1|2|3]"
               " [--top N] [--only INDEX]\n"
               "  The cache file is copied to a temporary file first; it is never modified.\n",
               stderr);
}

} // Anonymous namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }
    std::filesystem::path input{argv[1]};
    int repeat{3};
    int top{10};
    int scale{1};
    std::optional<size_t> only;
    std::optional<std::filesystem::path> hashes_path;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (i + 1 >= argc) {
            PrintUsage();
            return 1;
        }
        if (arg == "--repeat") {
            repeat = (std::max)(1, std::atoi(argv[++i]));
        } else if (arg == "--hashes") {
            hashes_path = argv[++i];
        } else if (arg == "--scale") {
            scale = std::atoi(argv[++i]);
        } else if (arg == "--top") {
            top = std::atoi(argv[++i]);
        } else if (arg == "--only") {
            only = static_cast<size_t>(std::atoll(argv[++i]));
        } else {
            PrintUsage();
            return 1;
        }
    }
    switch (scale) {
    case 1:
        Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res1X);
        break;
    case 2:
        Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res2X);
        break;
    case 3:
        Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res3X);
        break;
    default:
        PrintUsage();
        return 1;
    }
    Settings::UpdateRescalingInfo();

    // LoadPipelines deletes a file it cannot read: give it a copy.
    const std::filesystem::path copy{std::filesystem::temp_directory_path() /
                                     fmt::format("eden-shader-bench-{}.bin", std::rand())};
    std::error_code ec;
    if (!std::filesystem::copy_file(input, copy, std::filesystem::copy_options::overwrite_existing,
                                    ec)) {
        std::fprintf(stderr, "cannot copy %s: %s\n", input.string().c_str(),
                     ec.message().c_str());
        return 1;
    }
    std::vector<Pipeline> pipelines;
    VideoCommon::LoadPipelines(
        std::stop_token{}, copy, CACHE_VERSION,
        [&](std::ifstream& file, VideoCommon::FileEnvironment env) {
            Pipeline& pipeline{pipelines.emplace_back()};
            pipeline.is_compute = true;
            file.read(reinterpret_cast<char*>(&pipeline.compute_key),
                      sizeof(pipeline.compute_key));
            pipeline.envs.push_back(std::move(env));
        },
        [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment> envs) {
            Pipeline& pipeline{pipelines.emplace_back()};
            file.read(reinterpret_cast<char*>(&pipeline.graphics_key),
                      sizeof(pipeline.graphics_key));
            pipeline.envs = std::move(envs);
        });
    std::filesystem::remove(copy, ec);
    if (pipelines.empty()) {
        std::fputs("no pipeline loaded (wrong file or cache version?)\n", stderr);
        return 1;
    }
    if (only) {
        // Profile a single pipeline, numbered as in the full run
        if (*only >= pipelines.size()) {
            std::fputs("--only: no such pipeline\n", stderr);
            return 1;
        }
        Pipeline selected{std::move(pipelines[*only])};
        pipelines.clear();
        pipelines.push_back(std::move(selected));
    }
    const size_t num_compute{static_cast<size_t>(
        std::ranges::count_if(pipelines, [](const Pipeline& p) { return p.is_compute; }))};
    size_t num_programs{};
    for (const Pipeline& pipeline : pipelines) {
        num_programs += pipeline.envs.size();
    }
    fmt::print("{} pipelines ({} graphics, {} compute), {} programs, scale {}x, {} runs\n",
               pipelines.size(), pipelines.size() - num_compute, num_compute, num_programs, scale,
               repeat);

    const Shader::Profile profile{MakeProfile()};
    const Shader::HostTranslateInfo host_info{MakeHostInfo()};
    Pools pools;

    // Run 0 warms up the pools and caches and gives the reference output; runs 1..N are timed.
    // A pipeline's best time over the timed runs is its time, which filters out preemption.
    std::vector<BuildResult> reference(pipelines.size());
    std::vector<u64> best(pipelines.size(), ~0ULL);
    std::vector<Shader::PassTimer::Totals> run_totals(static_cast<size_t>(repeat));
    std::vector<u64> run_wall(static_cast<size_t>(repeat));
    size_t nondeterministic{};
    for (int run = 0; run <= repeat; ++run) {
        Shader::PassTimer::Totals* const totals{run == 0 ? nullptr : &run_totals[run - 1]};
        Shader::PassTimer::thread_totals = totals;
        u64 wall{};
        for (size_t i = 0; i < pipelines.size(); ++i) {
            BuildResult result{Build(pools, profile, host_info, pipelines[i])};
            wall += result.nanoseconds;
            if (run == 0) {
                reference[i] = std::move(result);
                continue;
            }
            best[i] = (std::min)(best[i], result.nanoseconds);
            if (result.module_hashes != reference[i].module_hashes ||
                result.error != reference[i].error) {
                ++nondeterministic;
            }
        }
        if (run > 0) {
            run_wall[run - 1] = wall;
        }
    }
    Shader::PassTimer::thread_totals = nullptr;
    pools.ReleaseContents();

    // The run with the lowest total is reported per pass, so the pass times add up to it.
    const size_t best_run{static_cast<size_t>(
        std::ranges::min_element(run_wall) - run_wall.begin())};
    const auto& totals{run_totals[best_run]};
    const double total_ms{static_cast<double>(run_wall[best_run]) / 1e6};
    u64 sum_best{};
    for (const u64 value : best) {
        sum_best += value;
    }
    fmt::print("total: {:.1f} ms (best run; runs:", total_ms);
    for (const u64 wall : run_wall) {
        fmt::print(" {:.1f}", static_cast<double>(wall) / 1e6);
    }
    fmt::print("), sum of per-pipeline best: {:.1f} ms\n", static_cast<double>(sum_best) / 1e6);

    std::vector<size_t> order(static_cast<size_t>(Pass::Count));
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::ranges::sort(order, [&](size_t a, size_t b) {
        return totals.nanoseconds[a] > totals.nanoseconds[b];
    });
    u64 accounted{};
    fmt::print("{:<30} {:>10} {:>7} {:>8}\n", "pass", "ms", "%", "calls");
    for (const size_t index : order) {
        if (totals.calls[index] == 0) {
            continue;
        }
        accounted += totals.nanoseconds[index];
        const double ms{static_cast<double>(totals.nanoseconds[index]) / 1e6};
        fmt::print("{:<30} {:>10.2f} {:>6.1f}% {:>8}\n",
                   Shader::PassTimer::Name(static_cast<Pass>(index)), ms, 100.0 * ms / total_ms,
                   totals.calls[index]);
    }
    const double other_ms{static_cast<double>(run_wall[best_run] - accounted) / 1e6};
    fmt::print("{:<30} {:>10.2f} {:>6.1f}%\n", "(other)", other_ms, 100.0 * other_ms / total_ms);

    std::vector<size_t> slowest(pipelines.size());
    for (size_t i = 0; i < slowest.size(); ++i) {
        slowest[i] = i;
    }
    std::ranges::sort(slowest, [&](size_t a, size_t b) { return best[a] > best[b]; });
    fmt::print("slowest pipelines (best time):\n");
    for (int i = 0; i < top && i < static_cast<int>(slowest.size()); ++i) {
        const size_t index{slowest[i]};
        fmt::print("  #{:<5} {:>9.2f} ms  {} {:016x}\n", index,
                   static_cast<double>(best[index]) / 1e6,
                   pipelines[index].is_compute ? "compute " : "graphics", PipelineId(pipelines[index]));
    }

    size_t num_errors{};
    size_t num_modules{};
    u64 combined{};
    std::FILE* hashes{};
    if (hashes_path) {
        hashes = std::fopen(hashes_path->string().c_str(), "w");
    }
    for (size_t i = 0; i < reference.size(); ++i) {
        const BuildResult& result{reference[i]};
        std::string line{fmt::format("{} {:016x}", i, PipelineId(pipelines[i]))};
        if (!result.error.empty()) {
            ++num_errors;
            line += " " + result.error;
        }
        for (const u64 hash : result.module_hashes) {
            ++num_modules;
            line += fmt::format(" {:016x}", hash);
            combined = Common::CityHash64(reinterpret_cast<const char*>(&hash), sizeof(hash)) ^
                       (combined * 0x9E3779B97F4A7C15ULL);
        }
        if (hashes) {
            std::fprintf(hashes, "%s\n", line.c_str());
        }
    }
    if (hashes) {
        std::fclose(hashes);
    }
    fmt::print("{} SPIR-V modules, {} pipelines failed or skipped, output hash {:016x}\n",
               num_modules, num_errors, combined);
    if (nondeterministic != 0) {
        fmt::print("WARNING: {} builds differed from the first run\n", nondeterministic);
    }
    return 0;
}

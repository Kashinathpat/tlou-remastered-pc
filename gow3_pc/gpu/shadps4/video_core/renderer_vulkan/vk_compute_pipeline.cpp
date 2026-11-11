// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>

#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

ComputePipeline::ComputePipeline(const Instance& instance, Scheduler& scheduler,
                                 DescriptorHeap& desc_heap, const Shader::Profile& profile,
                                 vk::PipelineCache pipeline_cache, ComputePipelineKey compute_key_,
                                 const Shader::Info& info_, vk::ShaderModule module,
                                 SerializationSupport& sdata, bool preloading /*=false*/)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache, true},
      compute_key{compute_key_}, host_local_size{ShaderModuleLocalSize(module)} {
    auto& info = stages[int(Shader::SwStage::Compute)];
    info = &info_;
    const auto debug_str = GetDebugString();

    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
        .requiredSubgroupSize = 64,
    };
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .pNext = instance.IsSubgroupSize64Supported() ? &subgroup_size_ci : nullptr,
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };

    u32 binding{};
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    for (const auto& buffer : info->buffers) {
        // During deserialization, we don't have access to the UD to fetch sharp data. To address
        // this properly we need to track shaprs or portion of them in `sdata`, but since we're
        // interested only in "is storage" flag (which is not even effective atm), we can take a
        // shortcut there.
        const auto sharp = preloading ? AmdGpu::Buffer{} : buffer.GetSharp(*info);
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    for (const auto& image : info->images) {
        const u32 num_bindings = image.NumBindings(*info);
        bindings.push_back({
            .binding = binding,
            .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                               : vk::DescriptorType::eSampledImage,
            .descriptorCount = num_bindings,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
        binding += num_bindings;
    }
    for (const auto& sampler : info->samplers) {
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }

    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    const auto device = instance.GetDevice();
    auto [descriptor_set_result, descriptor_set] =
        device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(descriptor_set_result == vk::Result::eSuccess,
               "Failed to create compute descriptor set layout: {}",
               vk::to_string(descriptor_set_result));
    desc_layout = std::move(descriptor_set);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create compute pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Compute PipelineLayout {}", debug_str);

    // gow3 10 Nov 2026 diagnostics (GOW3_PIPELINE_STATS=1): workgroups above 256 threads.
    const bool statistics = instance.HasPipelineStatistics() && HostWorkgroupThreads() > 256;
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .flags = statistics ? vk::PipelineCreateFlagBits::eCaptureStatisticsKHR
                            : vk::PipelineCreateFlags{},
        .stage = shader_ci,
        .layout = *pipeline_layout,
    };
    auto [pipeline_result, pipe] =
        instance.GetDevice().createComputePipelineUnique(pipeline_cache, compute_pipeline_ci);
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create compute pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Compute Pipeline {}", debug_str);
    if (statistics) {
        const auto& device = instance.GetDevice();
        const vk::PipelineInfoKHR pipeline_info{.pipeline = *pipeline};
        const auto [exec_result, executables] =
            device.getPipelineExecutablePropertiesKHR(pipeline_info);
        std::string line = fmt::format("GPU pipeline statistics: cs {:#x}, {}x{}x{} threads:",
                                       info->pgm_hash, host_local_size[0], host_local_size[1],
                                       host_local_size[2]);
        for (u32 i = 0; exec_result == vk::Result::eSuccess && i < executables.size(); ++i) {
            const vk::PipelineExecutableInfoKHR exec_info{.pipeline = *pipeline,
                                                          .executableIndex = i};
            const auto [stat_result, stats] = device.getPipelineExecutableStatisticsKHR(exec_info);
            if (stat_result != vk::Result::eSuccess) {
                continue;
            }
            for (const auto& stat : stats) {
                line += fmt::format(" {}=", std::string_view{stat.name.data()});
                switch (stat.format) {
                case vk::PipelineExecutableStatisticFormatKHR::eBool32:
                    line += fmt::format("{}", stat.value.b32);
                    break;
                case vk::PipelineExecutableStatisticFormatKHR::eInt64:
                    line += fmt::format("{}", stat.value.i64);
                    break;
                case vk::PipelineExecutableStatisticFormatKHR::eUint64:
                    line += fmt::format("{}", stat.value.u64);
                    break;
                case vk::PipelineExecutableStatisticFormatKHR::eFloat64:
                    line += fmt::format("{:.2f}", stat.value.f64);
                    break;
                }
            }
        }
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }
}

ComputePipeline::~ComputePipeline() = default;

} // namespace Vulkan

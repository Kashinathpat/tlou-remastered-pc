// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
} // namespace VideoCore

namespace Serialization {
struct Archive;
}

namespace Vulkan {

class Instance;
class Scheduler;
class DescriptorHeap;

struct ComputePipelineKey {
    size_t value;

    friend bool operator==(const ComputePipelineKey& lhs, const ComputePipelineKey& rhs) {
        return lhs.value == rhs.value;
    }
    friend bool operator!=(const ComputePipelineKey& lhs, const ComputePipelineKey& rhs) {
        return !(lhs == rhs);
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

class ComputePipeline : public Pipeline {
public:
    struct SerializationSupport {
        u32 dummy{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar);
    };

    ComputePipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
                    const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
                    ComputePipelineKey compute_key, const Shader::Info& info,
                    vk::ShaderModule module, SerializationSupport& sdata, bool preloading);
    ~ComputePipeline();

    /// gow3 10 Nov 2026 (WorkgroupSplitFactors): how many host workgroups each guest workgroup of
    /// `guest` threads runs as in each dimension: the guest size over the LocalSize its SPIR-V
    /// declares.
    std::array<u32, 3> WorkgroupSplit(const std::array<u32, 3>& guest) const noexcept {
        std::array<u32, 3> factors{1, 1, 1};
        for (u32 i = 0; i < 3; ++i) {
            const u32 host = host_local_size[i];
            if (host && guest[i] > host && guest[i] % host == 0) {
                factors[i] = guest[i] / host;
            }
        }
        return factors;
    }

    /// gow3 10 Nov 2026 (GOW3_PIPELINE_STATS): threads in one host workgroup (0: unknown).
    u32 HostWorkgroupThreads() const noexcept {
        return host_local_size[0] * host_local_size[1] * host_local_size[2];
    }

private:
    ComputePipelineKey compute_key;
    std::array<u32, 3> host_local_size{};
};

} // namespace Vulkan

template <>
struct std::hash<Vulkan::ComputePipelineKey> {
    std::size_t operator()(const Vulkan::ComputePipelineKey& key) const noexcept {
        return std::hash<size_t>{}(key.value);
    }
};

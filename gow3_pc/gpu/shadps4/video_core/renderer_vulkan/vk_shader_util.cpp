// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_movie_aspect.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {

// gow3 10 Nov 2026 (ShaderModuleLocalSize): OpExecutionMode %entry LocalSize x y z.
/// gow3 11 Nov 2026: a module emitted with lanes (LaneTraversal, emit_spirv.cpp) names its entry
/// point "gow3_lanes_<n>": each host invocation runs n guest ones, so the guest workgroup is n
/// times its LocalSize in X, and no split is read from it.
std::array<u32, 3> SpirvLocalSize(std::span<const u32> code) {
    constexpr u32 OpName = 5;
    constexpr u32 OpExecutionMode = 16;
    constexpr u32 ExecutionModeLocalSize = 17;
    std::array<u32, 3> size{};
    u32 lanes = 1;
    for (size_t i = 5; i < code.size();) {
        const u32 words = code[i] >> 16;
        const u32 opcode = code[i] & 0xffff;
        if (words == 0 || i + words > code.size()) {
            break;
        }
        if (opcode == OpExecutionMode && words >= 6 && code[i + 2] == ExecutionModeLocalSize) {
            size = {code[i + 3], code[i + 4], code[i + 5]};
        }
        if (opcode == OpName && words >= 3) {
            const char* name = reinterpret_cast<const char*>(&code[i + 2]);
            const size_t max = (words - 2) * sizeof(u32);
            constexpr std::string_view prefix = "gow3_lanes_";
            const std::string_view text{name, strnlen(name, max)};
            if (text.starts_with(prefix)) {
                lanes = std::max(1u, static_cast<u32>(std::atoi(text.data() + prefix.size())));
            }
        }
        if (opcode == 54) { // OpFunction: no more debug names or execution modes
            break;
        }
        i += words;
    }
    size[0] *= lanes;
    return size;
}

std::mutex g_local_size_mutex;
std::unordered_map<VkShaderModule, std::array<u32, 3>> g_local_size;

// gow3 21 Oct 2026, diagnostics (GOW3_INVARIANT_POSITION=1, off by default): the games draw
// depth first and then shade with depth test EQUAL, which needs every vertex shader to compute
// the same position bit for bit. SPIR-V promises that only for outputs decorated Invariant (DXVK
// decorates all D3D positions). Adds the decoration to every output variable that is the Position
// built-in, also in SPIR-V cached by earlier builds. It did not change a
// red floor (a cached binary paired with another permutation's description, see
// vk_pipeline_serialization.cpp BinaryMatchesMeta), so it stays opt-in.
constexpr u32 OpVariable = 59;
constexpr u32 OpDecorate = 71;
constexpr u32 DecorationBuiltIn = 11;
constexpr u32 DecorationInvariant = 18;
constexpr u32 BuiltInPosition = 0;
constexpr u32 StorageClassOutput = 3;

bool InvariantPositionEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_INVARIANT_POSITION");
        const bool on = env && env[0] == '1';
        if (on) {
            std::printf("GPU: vertex positions computed invariantly (GOW3_INVARIANT_POSITION=1)\n");
            std::fflush(stdout);
        }
        return on;
    }();
    return enabled;
}

// Returns the code with the decoration added, or an empty vector when nothing needs it.
std::vector<u32> WithInvariantPosition(std::span<const u32> code) {
    constexpr size_t HeaderWords = 5;
    if (code.size() <= HeaderWords || code[0] != 0x07230203u) {
        return {};
    }
    std::unordered_set<u32> outputs, invariant, positions;
    for (size_t at = HeaderWords; at < code.size();) {
        const u32 words = code[at] >> 16;
        const u32 opcode = code[at] & 0xffffu;
        if (words == 0 || at + words > code.size()) {
            return {};
        }
        if (opcode == OpVariable && words >= 4 && code[at + 3] == StorageClassOutput) {
            outputs.insert(code[at + 2]);
        } else if (opcode == OpDecorate && words >= 3) {
            if (code[at + 2] == DecorationInvariant) {
                invariant.insert(code[at + 1]);
            } else if (words >= 4 && code[at + 2] == DecorationBuiltIn &&
                       code[at + 3] == BuiltInPosition) {
                positions.insert(code[at + 1]);
            }
        }
        at += words;
    }
    std::vector<u32> patched;
    for (size_t at = HeaderWords; at < code.size(); at += code[at] >> 16) {
        const u32 words = code[at] >> 16;
        if ((code[at] & 0xffffu) != OpDecorate || words < 4 || code[at + 2] != DecorationBuiltIn ||
            code[at + 3] != BuiltInPosition) {
            continue;
        }
        const u32 id = code[at + 1];
        if (!outputs.contains(id) || invariant.contains(id)) {
            continue;
        }
        if (patched.empty()) {
            patched.assign(code.begin(), code.end());
        }
        const size_t insert_at = at + words + (patched.size() - code.size());
        const u32 decoration[] = {(3u << 16) | OpDecorate, id, DecorationInvariant};
        patched.insert(patched.begin() + insert_at, std::begin(decoration), std::end(decoration));
    }
    return patched;
}

// gow3 11 Nov 2026: GOW3_DISPLAY_ASPECT=<w>x<h>, the display's aspect ratio when it is not 16:9
// (tools/probe_tlou.py): the movies keep 16:9 in it (vk_movie_aspect.h).
float DisplayAspect() {
    static const float aspect = [] {
        const char* env = std::getenv("GOW3_DISPLAY_ASPECT");
        int width = 0, height = 0;
        if (!env || std::sscanf(env, "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0) {
            return 0.0f;
        }
        return static_cast<float>(width) / static_cast<float>(height);
    }();
    return aspect;
}

} // Anonymous namespace

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {
    std::vector<u32> patched;
    if (InvariantPositionEnabled()) {
        patched = WithInvariantPosition(code);
        if (!patched.empty()) {
            code = patched;
        }
    }
    std::vector<u32> movie;
    if (DisplayAspect() > 0.0f) {
        movie = WithMovieAspect(code, DisplayAspect());
        if (!movie.empty()) {
            code = movie;
        }
    }
    const vk::ShaderModuleCreateInfo shader_info = {
        .codeSize = code.size() * sizeof(u32),
        .pCode = code.data(),
    };

    auto [module_result, module] = device.createShaderModule(shader_info);
    ASSERT_MSG(module_result == vk::Result::eSuccess, "Failed to compile SPIR-V shader: {}",
               vk::to_string(module_result));
    // Every module is recorded (or its entry dropped): a handle the driver reuses after a module
    // was destroyed never keeps the old module's size.
    const auto local_size = SpirvLocalSize(code);
    {
        std::scoped_lock lock{g_local_size_mutex};
        if (local_size[0]) {
            g_local_size[static_cast<VkShaderModule>(module)] = local_size;
        } else {
            g_local_size.erase(static_cast<VkShaderModule>(module));
        }
    }
    return module;
}

std::array<u32, 3> ShaderModuleLocalSize(vk::ShaderModule module) {
    std::scoped_lock lock{g_local_size_mutex};
    const auto it = g_local_size.find(static_cast<VkShaderModule>(module));
    return it != g_local_size.end() ? it->second : std::array<u32, 3>{};
}

} // namespace Vulkan

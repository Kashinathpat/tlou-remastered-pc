// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/**
 * @brief Creates a vulkan shader module from SPIR-V bytecode.
 * @param code The SPIR-V bytecode data.
 * @param device The vulkan device handle
 */
vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device);

/// gow3 10 Nov 2026 (WorkgroupSplitFactors, shader_recompiler/runtime_info.h): the LocalSize a
/// module's SPIR-V declares (zeros: none, not a compute shader). The dispatch compares it with the
/// guest workgroup size, so the split always matches the binary in use, cached or new.
std::array<u32, 3> ShaderModuleLocalSize(vk::ShaderModule module);

} // namespace Vulkan

// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/ir/program.h"

namespace Shader {
struct Profile;
void InjectClipDistanceAttributes(IR::Program& program, RuntimeInfo& runtime_info);
} // namespace Shader

namespace Shader::Optimization {

void SsaRewritePass(IR::Program& program);
void SsaRepairPass(IR::Program& program);
void PhiSimplificationPass(IR::Program& program);
void InverseBallotEliminationPass(IR::Program& program);
void LowerWave64BallotPass(IR::Program& program, const RuntimeInfo& runtime_info,
                           const Profile& profile);
void LowerHardwareIntrinsics(IR::Program& program);
void LowerPhisToRegsPass(IR::Program& program);
void DeadCodeEliminationPass(IR::Program& program);
/// gow3: exact integers from interpolated float varyings on GPUs without manual interpolation.
void InterpolatedIntegerPass(IR::Program& program);
void ConstantPropagationPass(IR::BlockList& program);
void FlattenExtendedUserdataPass(IR::Program& program);
void ReadLaneEliminationPass(IR::Program& program);
ResourceDiscoveryList ResourceDiscoverPass(IR::Program& program, const Profile& profile);
void ResourcePatchingPass(Shader::Info& info, const ResourceDiscoveryList& sharp_usages,
                          const Profile& profile);
void CollectShaderInfoPass(IR::Program& program, const Profile& profile);
void LowerBufferFormatToRaw(IR::Program& program);
void LowerUserClipPlanes(IR::Program& program, const RuntimeInfo& runtime_info);
void LowerFp64ToFp32(IR::Program& program);
void RingAccessElimination(const IR::Program& program, const RuntimeInfo& runtime_info);
void TessellationPreprocess(IR::Program& program, RuntimeInfo& runtime_info);
void HullShaderTransform(IR::Program& program, const RuntimeInfo& runtime_info);
void DomainShaderTransform(const IR::Program& program, const RuntimeInfo& runtime_info);
void SharedMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                             const Profile& profile);
/// gow3: whether the shader gets LDS barriers for groups of several waves on 32-lane subgroups.
bool MultiwaveLdsBarriers(u64 pgm_hash);
/// gow3 12 Nov 2026 (quarter_depth_pass.cpp, The Last of Us #5/#66): Cs_DownsampleQuarterDepth
/// writes the depth the ambient shadows were computed at instead of the farthest of each block
/// (GOW3_QUARTER_DEPTH=0: off). QuarterDepthPointSample: whether this shader is changed (its
/// cached binary has its own name).
bool QuarterDepthPointSample(u64 pgm_hash);
void QuarterDepthPass(IR::Program& program);
/// gow3 11 Nov 2026: workgroup barriers inside an if whose condition differs between threads run
/// for every thread, the if's LDS writes kept to the threads it selects (shared_memory_barrier_pass
/// .cpp). On while UniformBarriersOn() (the pipeline cache sets it).
void UniformBarrierPass(IR::Program& program);
inline bool& UniformBarriersOn() {
    static bool on = false;
    return on;
}
/// gow3 10 Nov 2026 (from BilalReloaded's port 0.8.4): LDS accesses bounds-checked like the PS4
/// (shared_memory_bounds_pass.cpp). GOW3_LDS_BOUNDS=0: off.
void SharedMemoryBoundsPass(IR::Program& program, const RuntimeInfo& runtime_info,
                            const Profile& profile);
bool SharedMemoryBounds(u64 pgm_hash);
/// gow3 11 Nov 2026 (lane_coarsening.cpp): how many guest invocations each host invocation of
/// this compute shader runs (1: one). The GPU it runs on is set once by the renderer.
u32 LaneCoarsening(u64 pgm_hash);
void SetLaneCoarseningGpu(u32 vendor_id, u32 device_id);
/// gow3 11 Nov 2026: the largest cloth solver is skipped at the dispatch (vk_rasterizer.cpp).
bool SkipLargeClothSolver(u64 pgm_hash);
void SharedMemorySimplifyPass(IR::Program& program, const Profile& profile);
void SharedMemoryToStoragePass(IR::Program& program, const RuntimeInfo& runtime_info,
                               const Profile& profile);

} // namespace Shader::Optimization

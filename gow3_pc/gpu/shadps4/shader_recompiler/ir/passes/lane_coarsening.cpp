// SPDX-FileCopyrightText: Copyright 2026 gow3_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 11 Nov 2026: which compute shaders run several guest invocations per host invocation
// (the SPIR-V backend's lane coarsening, TraverseLanes in emit_spirv.cpp).
//
// The Last of Us Remastered's cloth solver for 257-512 vertices (Cs_ClothSimulate512: cs e44070db
// in update 1.11, a9b60b18 in 1.00) is one 512-thread workgroup that needs 128 registers per
// thread, all 64K registers of an SM. On NVIDIA GPUs before Turing it lost the device
// intermittently (Xid 13 "SKEDCHECK19_CTA_REGISTER_CONSUMPTION failed"), though it had run
// before in the same session, and nothing changed in the shader avoided it. Each of its threads
// keeps one vertex in registers from barrier to barrier, so it cannot be split into workgroups;
// run with 2 lanes, it is a 256-thread workgroup, like the 256-vertex solver (12c24522) that
// never lost the device there.
//
// GOW3_PASCAL_CLOTH512: unset or "auto" = 2 lanes on NVIDIA GPUs before Turing, "1" = the
// solver as it is everywhere, "skip" = not run on those GPUs, "0" = not run anywhere,
// "2" = 2 lanes everywhere. GOW3_LANE_COARSEN=<hex hash>[:<lanes>],... adds other shaders
// (diagnostics: the 256-vertex solver, 12c24522, checks the transformation on any GPU).

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include "shader_recompiler/ir/passes/ir_passes.h"

namespace Shader::Optimization {

namespace {

std::atomic<u32> g_vendor{0};
std::atomic<u32> g_device{0};

bool IsLargeClothSolver(u64 pgm_hash) {
    return pgm_hash == 0xe44070dbULL || pgm_hash == 0xa9b60b18ULL;
}

bool PreTuringNvidia() {
    return g_vendor.load(std::memory_order_relaxed) == 0x10DE &&
           g_device.load(std::memory_order_relaxed) < 0x1E00;
}

enum class ClothMode { Run, Skip, Lanes };

ClothMode LargeClothMode() {
    const char* env = std::getenv("GOW3_PASCAL_CLOTH512");
    const bool affected = PreTuringNvidia();
    if (env && std::strcmp(env, "1") == 0) {
        return ClothMode::Run;
    }
    if (env && std::strcmp(env, "0") == 0) {
        return ClothMode::Skip;
    }
    if (env && std::strcmp(env, "2") == 0) {
        return ClothMode::Lanes;
    }
    if (env && std::strcmp(env, "skip") == 0) {
        return affected ? ClothMode::Skip : ClothMode::Run;
    }
    return affected ? ClothMode::Lanes : ClothMode::Run;
}

const std::vector<std::pair<u64, u32>>& ExtraShaders() {
    static const std::vector<std::pair<u64, u32>> list = [] {
        std::vector<std::pair<u64, u32>> out;
        const char* env = std::getenv("GOW3_LANE_COARSEN");
        for (const char* s = env; s && *s;) {
            char* next{};
            const u64 hash = std::strtoull(s, &next, 16);
            u32 lanes = 2;
            if (next && *next == ':') {
                lanes = static_cast<u32>(std::strtoul(next + 1, &next, 10));
            }
            if (hash != 0 && (lanes == 2 || lanes == 4)) {
                out.emplace_back(hash, lanes);
            }
            s = next && *next == ',' ? next + 1 : nullptr;
        }
        return out;
    }();
    return list;
}

} // Anonymous namespace

void SetLaneCoarseningGpu(u32 vendor_id, u32 device_id) {
    g_vendor.store(vendor_id, std::memory_order_relaxed);
    g_device.store(device_id, std::memory_order_relaxed);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        const ClothMode mode = LargeClothMode();
        std::printf("GPU: the cloth solver for 257-512 vertices (Cs_ClothSimulate512) %s "
                    "(GOW3_PASCAL_CLOTH512)\n",
                    mode == ClothMode::Run    ? "runs as it is"
                    : mode == ClothMode::Skip ? "is not run"
                                              : "runs as 256 threads with 2 vertices each");
        std::fflush(stdout);
    }
}

u32 LaneCoarsening(u64 pgm_hash) {
    for (const auto& [hash, lanes] : ExtraShaders()) {
        if (hash == pgm_hash) {
            return lanes;
        }
    }
    return IsLargeClothSolver(pgm_hash) && LargeClothMode() == ClothMode::Lanes ? 2 : 1;
}

bool SkipLargeClothSolver(u64 pgm_hash) {
    return IsLargeClothSolver(pgm_hash) && LargeClothMode() == ClothMode::Skip;
}

} // namespace Shader::Optimization

// SPDX-FileCopyrightText: Copyright 2026 gow3_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 12 Nov 2026 (The Last of Us Remastered, issues #5 and #66): a thin light or dark outline
// where the soft shadows of characters and vehicles meet the ground, also at 1080p.
//
// The game draws those "ambient shadows" at a quarter of the render size: Cs_AmbientShadowsNonMsaa
// (cs 71859bbb) and the occluder draws (ps c019bb7e) compute each quarter-size texel from the
// depth and normal of the top-left pixel of its 4x4 block. Every lit material's pixel shader then
// upsamples them with a depth-aware filter that weighs the four nearest texels by how close their
// depth is to the pixel's, taking that depth from a quarter-size depth buffer written by
// Cs_DownsampleQuarterDepth (cs 42cb16a5): the farthest of the 16 depths of each block, not the
// depth the shadow was computed at. Along a silhouette the two disagree: a block whose top-left
// pixel is on a character but whose farthest pixel is the ground stores the character's (light)
// shadow under the ground's depth, so the ground next to the character takes it (a light band),
// and the character's own edge pixels match no texel and take the ground's shadow (a dark one).
//
// This pass makes Cs_DownsampleQuarterDepth write the depth of the pixel each texel's shadow was
// computed at: of its four gathers, the one at the block's top-left 2x2 holds that pixel in its
// fourth component (gather order: (i,j+1), (i+1,j+1), (i+1,j), (i,j)). The texels then carry a
// matching depth and shadow and the filter keeps each side of an edge to its own shadow. The
// quarter-size depth is read by nothing else (frame captures of 17 scenes). GOW3_QUARTER_DEPTH=0:
// the game's farthest depth.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {

namespace {

constexpr u64 DownsampleQuarterDepth = 0x42cb16a5ULL; // update 1.11

bool QuarterDepthOn() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_QUARTER_DEPTH");
        return !(env && std::strcmp(env, "0") == 0);
    }();
    return on;
}

void Report(u64 pgm_hash, const char* what) {
    std::printf("Shader: cs %#llx (quarter-size depth): %s\n",
                static_cast<unsigned long long>(pgm_hash), what);
    std::fflush(stdout);
}

} // namespace

bool QuarterDepthPointSample(u64 pgm_hash) {
    return pgm_hash == DownsampleQuarterDepth && QuarterDepthOn();
}

void QuarterDepthPass(IR::Program& program) {
    if (!QuarterDepthPointSample(program.info.pgm_hash)) {
        return;
    }
    // The shader as update 1.11 has it: four gathers of the full-size depth, the farthest of
    // their 16 values (FPMax32/FPMaxTri32) written as the first component of one ImageWrite.
    std::vector<IR::Inst*> gathers;
    IR::Inst* write = nullptr;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::ImageSampleRaw) {
                gathers.push_back(&inst);
            } else if (inst.GetOpcode() == IR::Opcode::ImageWrite) {
                write = write ? nullptr : &inst;
            }
        }
    }
    IR::Inst* const value = write ? write->Arg(4).TryInst() : nullptr;
    IR::Inst* const farthest = value && value->GetOpcode() == IR::Opcode::CompositeConstructF32x4
                                   ? value->Arg(0).TryInst()
                                   : nullptr;
    if (gathers.size() != 4 || !farthest ||
        (farthest->GetOpcode() != IR::Opcode::FPMaxTri32 &&
         farthest->GetOpcode() != IR::Opcode::FPMax32)) {
        Report(program.info.pgm_hash, "not the expected code, left as it is");
        return;
    }
    // The gathers in program order: (+u, +v), (+u, -v), (-u, +v), (-u, -v) around the block's
    // center; the last one covers its top-left 2x2 pixels. Its fourth component is read by a
    // CompositeExtractF32x4 that feeds the maximum.
    IR::Inst* const top_left = gathers[3];
    IR::Inst* pixel = nullptr;
    for (const IR::Use& use : top_left->Uses()) {
        if (use.user->GetOpcode() == IR::Opcode::CompositeExtractF32x4 &&
            use.user->Arg(1).IsImmediate() && use.user->Arg(1).U32() == 3) {
            pixel = use.user;
        }
    }
    if (!pixel) {
        Report(program.info.pgm_hash, "no top-left texel among the gathers, left as it is");
        return;
    }
    value->SetArg(0, IR::Value{pixel});
    static bool reported = false;
    if (!reported) {
        reported = true;
        Report(program.info.pgm_hash, "the depth of each block's top-left pixel, where the "
                                      "ambient shadows are computed (not the farthest)");
    }
}

} // namespace Shader::Optimization

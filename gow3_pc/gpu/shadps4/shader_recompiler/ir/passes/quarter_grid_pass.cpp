// SPDX-FileCopyrightText: Copyright 2026 gow3_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 12 Nov 2026 (The Last of Us Remastered, issues #5 and #66).
//
// Every lit material's pixel shader upsamples the ambient shadows (drawn at a quarter of the render
// size) with a depth-aware filter whose texel grid is written into the shader as four float
// literals, which the game rewrites for its render size when it loads the shader: width / 4,
// height / 4, 4 / width, 4 / height (480, 270, 1/480, 1/270 at 1080p; render_size_literals.h).
// tools/probe_tlou.py makes the game draw those shadows and their depth at half the render size
// (AMBIENT_SHADOWS_HALF: a light ring where feet and wheels touch the ground, half as wide), and the
// game knows nothing of it in the materials.
//
// This pass gives those shaders the texel grid of the texture they sample: in a pixel shader that
// holds all four literals of the current render size, their uses become the width, height and
// their inverses read from the image descriptor (T#) of the texture the filter samples with them.
// Whatever size the game or the patch gives that texture, the grid matches it; at the game's own
// sizes the values are the literals'. GOW3_QUARTER_GRID=0 turns the pass off.

#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_set>
#include <vector>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/dominance.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/render_size_literals.h"

namespace Shader::Optimization {

namespace {

constexpr size_t NumGrid = 4; // width, height, 1 / width, 1 / height

std::optional<size_t> Classify(u32 bits, const std::array<u32, NumGrid>& words) {
    for (size_t i = 0; i < NumGrid; ++i) {
        if (bits == words[i]) {
            return i;
        }
    }
    return std::nullopt;
}

struct GridUse {
    IR::Inst* inst;
    size_t arg;
    size_t grid;
    bool raw; ///< a 32-bit integer immediate made a float by the instruction (BitCastF32U32)
};

/// Whether `value` is computed from one of `sources` (instructions, through their arguments).
bool DependsOn(const IR::Value& value, const std::unordered_set<const IR::Inst*>& sources,
               std::unordered_set<const IR::Inst*>& visited) {
    std::vector<const IR::Inst*> stack;
    if (IR::Inst* inst = value.TryInst()) {
        stack.push_back(inst);
    }
    while (!stack.empty()) {
        const IR::Inst* inst = stack.back();
        stack.pop_back();
        if (!visited.insert(inst).second) {
            continue;
        }
        if (sources.contains(inst)) {
            return true;
        }
        for (size_t i = 0; i < inst->NumArgs(); ++i) {
            if (IR::Inst* arg = inst->Arg(i).TryInst()) {
                stack.push_back(arg);
            }
        }
    }
    return false;
}

/// The third dword of the image descriptor an ImageSampleRaw samples (width - 1 in bits 0-13,
/// height - 1 in bits 14-27), when the descriptor is built from its words.
IR::Inst* DescriptorSizeWord(const IR::Inst& sample) {
    IR::Inst* handle = sample.Arg(0).TryInst();
    if (!handle || handle->GetOpcode() != IR::Opcode::ImageHandle) {
        return nullptr;
    }
    IR::Inst* words = handle->Arg(0).TryInst();
    if (!words || words->GetOpcode() != IR::Opcode::CompositeConstructU32x4) {
        return nullptr;
    }
    return words->Arg(2).TryInst();
}

/// Whether the value `def` defines is available at `use` (`def` dominates it).
bool Available(IR::Inst* def, IR::Inst* use) {
    IR::Block* const def_block = def->GetParent();
    IR::Block* const use_block = use->GetParent();
    if (def_block != use_block) {
        return IR::Dominates(def_block, use_block);
    }
    for (const IR::Inst& inst : def_block->Instructions()) {
        if (&inst == def) {
            return true;
        }
        if (&inst == use) {
            return false;
        }
    }
    return false;
}

} // namespace

bool QuarterGridOn() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_QUARTER_GRID");
        return !(env && std::strcmp(env, "0") == 0);
    }();
    return on;
}

bool QuarterGridPass(IR::Program& program) {
    if (!QuarterGridOn() || program.info.sw_stage != SwStage::Fragment) {
        return false;
    }
    const auto words = RenderSizeLiteralWords();
    std::vector<GridUse> uses;
    std::array<bool, NumGrid> seen{};
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            const bool raw_cast = inst.GetOpcode() == IR::Opcode::BitCastF32U32;
            for (size_t arg = 0; arg < inst.NumArgs(); ++arg) {
                const IR::Value value = inst.Arg(arg);
                if (!value.IsImmediate()) {
                    continue;
                }
                std::optional<size_t> grid;
                if (value.Type() == IR::Type::F32) {
                    grid = Classify(std::bit_cast<u32>(value.F32()), words);
                } else if (raw_cast && value.Type() == IR::Type::U32) {
                    grid = Classify(value.U32(), words);
                }
                if (grid) {
                    uses.push_back({&inst, arg, *grid, raw_cast});
                    seen[*grid] = true;
                }
            }
        }
    }
    // The filter's signature: all four literals.
    for (const bool found : seen) {
        if (!found) {
            return false;
        }
    }
    const auto report = [&](const char* what) {
        static std::atomic<u32> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
            std::printf("Shader: ps %#llx has the ambient shadows' grid literals but %s; left as "
                        "it is\n",
                        static_cast<unsigned long long>(program.info.pgm_hash), what);
            std::fflush(stdout);
        }
    };
    for (const GridUse& use : uses) {
        if (use.inst->GetOpcode() == IR::Opcode::Phi) {
            report("a literal goes through a phi");
            return false;
        }
    }

    for (IR::Block* const block : program.post_order_blocks) {
        block->immediate_dominator = nullptr;
    }
    IR::ComputeDominators(program.post_order_blocks);

    // The texture the filter samples: an ImageSampleRaw whose coordinates come from the literals
    // and whose descriptor is available wherever a literal is used.
    std::unordered_set<const IR::Inst*> sources;
    for (const GridUse& use : uses) {
        sources.insert(use.inst);
    }
    IR::Inst* size_word = nullptr;
    bool sampled = false;
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::ImageSampleRaw) {
                continue;
            }
            std::unordered_set<const IR::Inst*> visited;
            bool from_grid = false;
            for (size_t arg = 2; arg <= 4 && !from_grid; ++arg) {
                from_grid = DependsOn(inst.Arg(arg), sources, visited);
            }
            if (!from_grid) {
                continue;
            }
            sampled = true;
            IR::Inst* const word = DescriptorSizeWord(inst);
            if (!word || word->GetOpcode() == IR::Opcode::Phi) {
                continue;
            }
            bool everywhere = true;
            for (const GridUse& use : uses) {
                everywhere = everywhere && Available(word, use.inst);
            }
            if (everywhere) {
                size_word = word;
                break;
            }
        }
        if (size_word) {
            break;
        }
    }
    if (!size_word) {
        report(sampled ? "its texture's descriptor is read after a use"
                       : "no texture is sampled with it");
        return false;
    }

    // Width, height and their inverses, right after the descriptor word is read.
    IR::Block* const block = size_word->GetParent();
    auto after = IR::Block::InstructionList::s_iterator_to(*size_word);
    ++after;
    IR::IREmitter ir{*block, after};
    const IR::U32 word{IR::Value{size_word}};
    const IR::U32 width{ir.IAdd(ir.BitFieldExtract(word, ir.Imm32(0u), ir.Imm32(14u)), ir.Imm32(1u))};
    const IR::U32 height{
        ir.IAdd(ir.BitFieldExtract(word, ir.Imm32(14u), ir.Imm32(14u)), ir.Imm32(1u))};
    std::array<IR::F32, NumGrid> values;
    values[0] = IR::F32{ir.ConvertUToF(32, 32, width)};
    values[1] = IR::F32{ir.ConvertUToF(32, 32, height)};
    values[2] = IR::F32{ir.FPRecip(values[0])};
    values[3] = IR::F32{ir.FPRecip(values[1])};
    std::array<IR::U32, NumGrid> raw_values;
    for (size_t i = 0; i < NumGrid; ++i) {
        raw_values[i] = ir.BitCast<IR::U32, IR::F32>(values[i]);
    }
    for (const GridUse& use : uses) {
        use.inst->SetArg(use.arg, use.raw ? IR::Value{raw_values[use.grid]}
                                          : IR::Value{values[use.grid]});
    }
    static std::atomic<u32> patched{0};
    if (patched.fetch_add(1, std::memory_order_relaxed) == 0) {
        std::printf("Shader: ps %#llx and the other pixel shaders that upsample the ambient "
                    "shadows take their grid from the texture they sample\n",
                    static_cast<unsigned long long>(program.info.pgm_hash));
        std::fflush(stdout);
    }
    return true;
}

} // namespace Shader::Optimization

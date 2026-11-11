// SPDX-License-Identifier: GPL-2.0-or-later
// Lane coarsening (LaneTraversal in emit_spirv.cpp, lane_coarsening.cpp) on a small compute
// program shaped like the cloth solvers: a loop with barriers whose body holds an if (a region
// with its own control flow), phis in the loop header (entered from the skeleton) and in the if's
// merge (inside a region). Writes the SPIR-V with 1, 2 and 4 lanes; validate them with
//   spirv-val --target-env vulkan1.3 <dir>/lanes-*.spv
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"

using namespace Shader;

static IR::Inst* NewPhi(IR::Block& block) {
    const auto it = block.PrependNewInst(block.begin(), IR::Opcode::Phi);
    it->SetFlags(IR::Type::U32);
    return &*it;
}

static std::vector<u32> Emit(u64 hash) {
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    info.pgm_hash = hash;
    info.shared_types = IR::Type::U32;
    info.loads.Set(IR::Attribute::LocalInvocationId, 0);
    RuntimeInfo runtime{};
    runtime.Initialize(info.hw_stage, info.sw_stage);
    runtime.hw.cs.workgroup_size = {256, 1, 1};
    runtime.hw.cs.shared_memory_size = 1024;

    Common::ObjectPool<IR::Inst> pool;
    IR::Block entry(pool), header(pool), body(pool), then(pool), join(pool), cont(pool),
        merge(pool);
    // entry: lds[tid] = tid; barrier
    IR::IREmitter e(entry);
    const IR::U32 tid = e.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
    const IR::U32 slot = e.IMul(tid, e.Imm32(4u));
    e.WriteShared(32, tid, slot);
    e.Barrier();
    // header: i = phi(0, i + 1), acc = phi(tid, joined)
    IR::Inst* const i_phi = NewPhi(header);
    IR::Inst* const acc_phi = NewPhi(header);
    const IR::U32 i{IR::Value{i_phi}};
    const IR::U32 acc{IR::Value{acc_phi}};
    // body: v = lds[(tid + 1) & 255]; if (v < 128) acc + v
    IR::IREmitter b(body);
    const IR::U32 next = b.BitwiseAnd(b.IAdd(tid, b.Imm32(1u)), b.Imm32(255u));
    const IR::U32 v{b.LoadShared(32, false, b.IMul(next, b.Imm32(4u)))};
    const IR::U1 small = b.ILessThan(v, b.Imm32(128u), false);
    IR::IREmitter t(then);
    const IR::U32 sum = t.IAdd(acc, v);
    // join: joined = phi(acc, sum); barrier; lds[tid] = joined; barrier
    IR::Inst* const joined_phi = NewPhi(join);
    const IR::U32 joined{IR::Value{joined_phi}};
    IR::IREmitter j(join);
    j.Barrier();
    j.WriteShared(32, joined, slot);
    j.Barrier();
    // continue: i + 1 < 4
    IR::IREmitter c(cont);
    const IR::U32 i_next = c.IAdd(i, c.Imm32(1u));
    const IR::U1 again = c.ILessThan(i_next, c.Imm32(4u), false);
    // merge: lds[tid] = acc
    IR::IREmitter m(merge);
    m.WriteShared(32, joined, slot);
    m.Epilogue();

    i_phi->AddPhiOperand(&entry, e.Imm32(0u));
    i_phi->AddPhiOperand(&cont, i_next);
    acc_phi->AddPhiOperand(&entry, tid);
    acc_phi->AddPhiOperand(&cont, joined);
    joined_phi->AddPhiOperand(&body, acc);
    joined_phi->AddPhiOperand(&then, sum);

    IR::Program program(info);
    for (IR::Block* block : {&entry, &header, &body, &then, &join, &cont, &merge}) {
        program.blocks.push_back(block);
    }
    using Type = IR::AbstractSyntaxNode::Type;
    auto& list = program.syntax_list;
    const auto block_node = [](IR::Block* block) {
        return IR::AbstractSyntaxNode{.data = {.block = block}, .type = Type::Block};
    };
    list.push_back(block_node(&entry));
    list.push_back(block_node(&header));
    IR::AbstractSyntaxNode loop{.type = Type::Loop};
    loop.data.loop = {&body, &cont, &merge};
    list.push_back(loop);
    list.push_back(block_node(&body));
    IR::AbstractSyntaxNode if_node{.type = Type::If};
    if_node.data.if_node = {small, &then, &join};
    list.push_back(if_node);
    list.push_back(block_node(&then));
    IR::AbstractSyntaxNode end_if{.type = Type::EndIf};
    end_if.data.end_if = {&join};
    list.push_back(end_if);
    list.push_back(block_node(&join));
    list.push_back(block_node(&cont));
    IR::AbstractSyntaxNode repeat{.type = Type::Repeat};
    repeat.data.repeat = {again, &header, &merge};
    list.push_back(repeat);
    list.push_back(block_node(&merge));
    list.push_back(IR::AbstractSyntaxNode{.type = Type::Return});

    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    Backend::Bindings bindings{};
    return Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
}

int main(int argc, char** argv) {
    const std::filesystem::path dir = argc > 1 ? argv[1] : ".";
    std::filesystem::create_directories(dir);
    // Hashes 1001 (1 lane), 1002 (2), 1004 (4).
#ifdef _WIN32
    _putenv("GOW3_LANE_COARSEN=1002:2,1004:4");
#else
    setenv("GOW3_LANE_COARSEN", "1002:2,1004:4", 1);
#endif
    for (u32 lanes : {1u, 2u, 4u}) {
        const auto code = Emit(0x1000 + lanes);
        std::ofstream out(dir / ("lanes-" + std::to_string(lanes) + ".spv"), std::ios::binary);
        out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
        if (!out) {
            return 1;
        }
    }
    std::printf("wrote lanes-1.spv, lanes-2.spv, lanes-4.spv to %s\n", dir.string().c_str());
    return 0;
}

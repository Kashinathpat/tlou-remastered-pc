// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <span>
#include <cstdlib>
#include <cstdio>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <boost/container/small_vector.hpp>
#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/func_traits.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {
namespace {

static constexpr spv::ExecutionMode GetInputPrimitiveType(AmdGpu::PrimitiveType type) {
    switch (type) {
    case AmdGpu::PrimitiveType::PointList:
        return spv::ExecutionMode::InputPoints;
    case AmdGpu::PrimitiveType::LineList:
    case AmdGpu::PrimitiveType::LineStrip:
        return spv::ExecutionMode::InputLines;
    case AmdGpu::PrimitiveType::TriangleList:
    case AmdGpu::PrimitiveType::TriangleStrip:
    case AmdGpu::PrimitiveType::RectList:
        return spv::ExecutionMode::Triangles;
    case AmdGpu::PrimitiveType::AdjTriangleList:
        return spv::ExecutionMode::InputTrianglesAdjacency;
    case AmdGpu::PrimitiveType::AdjLineList:
        return spv::ExecutionMode::InputLinesAdjacency;
    default:
        UNREACHABLE_MSG("Unknown input primitive type {}", u32(type));
    }
}

static constexpr spv::ExecutionMode GetOutputPrimitiveType(AmdGpu::GsOutputPrimitiveType type) {
    switch (type) {
    case AmdGpu::GsOutputPrimitiveType::PointList:
        return spv::ExecutionMode::OutputVertices;
    case AmdGpu::GsOutputPrimitiveType::LineStrip:
        return spv::ExecutionMode::OutputLineStrip;
    case AmdGpu::GsOutputPrimitiveType::TriangleStrip:
        return spv::ExecutionMode::OutputTriangleStrip;
    default:
        UNREACHABLE_MSG("Unknown output primitive type {}", u32(type));
    }
}

template <auto func, typename... Args>
void SetDefinition(EmitContext& ctx, IR::Inst* inst, Args... args) {
    inst->SetDefinition<Id>(func(ctx, std::forward<Args>(args)...));
}

template <typename ArgType>
ArgType Arg(EmitContext& ctx, const IR::Value& arg) {
    if constexpr (std::is_same_v<ArgType, Id>) {
        return ctx.Def(arg);
    } else if constexpr (std::is_same_v<ArgType, const IR::Value&>) {
        return arg;
    } else if constexpr (std::is_same_v<ArgType, u32>) {
        return arg.U32();
    } else if constexpr (std::is_same_v<ArgType, u64>) {
        return arg.U64();
    } else if constexpr (std::is_same_v<ArgType, bool>) {
        return arg.U1();
    } else if constexpr (std::is_same_v<ArgType, IR::Attribute>) {
        return arg.Attribute();
    } else if constexpr (std::is_same_v<ArgType, IR::ScalarReg>) {
        return arg.ScalarReg();
    } else if constexpr (std::is_same_v<ArgType, IR::VectorReg>) {
        return arg.VectorReg();
    } else if constexpr (std::is_same_v<ArgType, const char*>) {
        return arg.StringLiteral();
    } else if constexpr (std::is_same_v<ArgType, IR::Patch>) {
        return arg.Patch();
    }
    UNREACHABLE();
}

template <auto func, bool is_first_arg_inst, size_t... I>
void Invoke(EmitContext& ctx, IR::Inst* inst, std::index_sequence<I...>) {
    using Traits = Common::FuncTraits<decltype(func)>;
    if constexpr (std::is_same_v<typename Traits::ReturnType, Id>) {
        if constexpr (is_first_arg_inst) {
            SetDefinition<func>(
                ctx, inst, inst,
                Arg<typename Traits::template ArgType<I + 2>>(ctx, inst->Arg(I))...);
        } else {
            SetDefinition<func>(
                ctx, inst, Arg<typename Traits::template ArgType<I + 1>>(ctx, inst->Arg(I))...);
        }
    } else {
        if constexpr (is_first_arg_inst) {
            func(ctx, inst, Arg<typename Traits::template ArgType<I + 2>>(ctx, inst->Arg(I))...);
        } else {
            func(ctx, Arg<typename Traits::template ArgType<I + 1>>(ctx, inst->Arg(I))...);
        }
    }
}

template <auto func>
void Invoke(EmitContext& ctx, IR::Inst* inst) {
    using Traits = Common::FuncTraits<decltype(func)>;
    static_assert(Traits::NUM_ARGS >= 1, "Insufficient arguments");
    if constexpr (Traits::NUM_ARGS == 1) {
        Invoke<func, false>(ctx, inst, std::make_index_sequence<0>{});
    } else {
        using FirstArgType = typename Traits::template ArgType<1>;
        static constexpr bool is_first_arg_inst = std::is_same_v<FirstArgType, IR::Inst*>;
        using Indices = std::make_index_sequence<Traits::NUM_ARGS - (is_first_arg_inst ? 2 : 1)>;
        Invoke<func, is_first_arg_inst>(ctx, inst, Indices{});
    }
}

void EmitInst(EmitContext& ctx, IR::Inst* inst) {
    switch (inst->GetOpcode()) {
#define OPCODE(name, result_type, ...)                                                             \
    case IR::Opcode::name:                                                                         \
        return Invoke<&Emit##name>(ctx, inst);
#include "shader_recompiler/ir/opcodes.inc"
#undef OPCODE
    }
    UNREACHABLE_MSG("Invalid opcode {}", inst->GetOpcode());
}

Id TypeId(const EmitContext& ctx, IR::Type type) {
    switch (type) {
    case IR::Type::U1:
        return ctx.U1[1];
    case IR::Type::U32:
        return ctx.U32[1];
    case IR::Type::U32x2:
        return ctx.U32[2];
    case IR::Type::U32x3:
        return ctx.U32[3];
    case IR::Type::U32x4:
        return ctx.U32[4];
    case IR::Type::F32:
        return ctx.F32[1];
    case IR::Type::U64:
        return ctx.U64;
    default:
        UNREACHABLE_MSG("Phi node type {}", type);
    }
}

void Traverse(EmitContext& ctx, const IR::Program& program) {
    const char* guard_env=std::getenv("GOW3_SHADER_LOOP_LIMIT");
    const u32 loop_limit=guard_env ? std::strtoul(guard_env,nullptr,10) : 0;
    Id loop_counter{};
    IR::Block* current_block{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::Block: {
            const Id label{node.data.block->Definition<Id>()};
            if (current_block) {
                ctx.OpBranch(label);
            }
            current_block = node.data.block;
            ctx.AddLabel(label);
            if (loop_limit && node.data.block==program.blocks.front()) {
                loop_counter=ctx.DefineVar<false>(ctx.U32[1],spv::StorageClass::Function,ctx.u32_zero_value);
                ctx.Name(loop_counter,"diagnostic_loop_counter");
            }
            for (IR::Inst& inst : node.data.block->Instructions()) {
                EmitInst(ctx, &inst);
            }
            ctx.first_to_last_label_map[label.value] = ctx.last_label;
            break;
        }
        case IR::AbstractSyntaxNode::Type::If: {
            const Id if_label{node.data.if_node.body->Definition<Id>()};
            const Id endif_label{node.data.if_node.merge->Definition<Id>()};
            ctx.OpSelectionMerge(endif_label, spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(ctx.Def(node.data.if_node.cond), if_label, endif_label);
            break;
        }
        case IR::AbstractSyntaxNode::Type::Loop: {
            const Id body_label{node.data.loop.body->Definition<Id>()};
            const Id continue_label{node.data.loop.continue_block->Definition<Id>()};
            const Id endloop_label{node.data.loop.merge->Definition<Id>()};

            ctx.OpLoopMerge(endloop_label, continue_label, spv::LoopControlMask::MaskNone);
            ctx.OpBranch(body_label);
            break;
        }
        case IR::AbstractSyntaxNode::Type::Break: {
            const Id break_label{node.data.break_node.merge->Definition<Id>()};
            const Id skip_label{node.data.break_node.skip->Definition<Id>()};
            ctx.OpBranchConditional(ctx.Def(node.data.break_node.cond), break_label, skip_label);
            break;
        }
        case IR::AbstractSyntaxNode::Type::EndIf:
            if (current_block) {
                ctx.OpBranch(node.data.end_if.merge->Definition<Id>());
            }
            break;
        case IR::AbstractSyntaxNode::Type::Repeat: {
            Id cond{ctx.Def(node.data.repeat.cond)};
            if (loop_limit) {
                const Id iterations=ctx.OpIAdd(ctx.U32[1],ctx.OpLoad(ctx.U32[1],loop_counter),ctx.ConstU32(1U));
                ctx.OpStore(loop_counter,iterations);
                cond=ctx.OpLogicalAnd(ctx.U1[1],cond,ctx.OpULessThan(ctx.U1[1],iterations,ctx.ConstU32(loop_limit)));
            }
            const Id loop_header_label{node.data.repeat.loop_header->Definition<Id>()};
            const Id merge_label{node.data.repeat.merge->Definition<Id>()};
            ctx.OpBranchConditional(cond, loop_header_label, merge_label);
            break;
        }
        case IR::AbstractSyntaxNode::Type::Return:
            ctx.OpReturn();
            break;
        case IR::AbstractSyntaxNode::Type::Unreachable:
            ctx.OpUnreachable();
            break;
        }
        if (node.type != IR::AbstractSyntaxNode::Type::Block) {
            current_block = nullptr;
        }
    }
}

// gow3 11 Nov 2026: lane coarsening (lane_coarsening.cpp). Each host invocation runs
// `ctx.lanes` guest invocations: guest X = host X + lane * host size. The program is cut at its
// barriers. What lies between two of them (a region) is emitted once per lane, one copy after the
// other, each copy with its own SPIR-V values and labels; the barriers and the ifs and loops that
// contain one (the skeleton) are emitted once, their conditions taken from lane 0 (a GCN
// program's barriers are reached by every wave alike, so those conditions are uniform). The
// blocks the skeleton enters start once, with a phi per lane. Lane L's arithmetic is guest
// invocation X + L * host X's, in the same order, so the results are the same bit for bit;
// shared memory written before a barrier is read after it, by any lane.
class LaneTraversal {
public:
    LaneTraversal(EmitContext& ctx_, const IR::Program& program_)
        : ctx{ctx_}, program{program_}, lanes{ctx_.lanes} {}

    /// Whether the program can be emitted this way: no cross-invocation operations (each lane
    /// would see only its own host invocation's lanes), and barriers only in structured code.
    static bool Eligible(const IR::Program& program, std::string& why) {
        for (const IR::Block* block : program.blocks) {
            for (const IR::Inst& inst : *block) {
                switch (inst.GetOpcode()) {
                case IR::Opcode::LaneId:
                case IR::Opcode::WarpId:
                case IR::Opcode::QuadBroadcast:
                case IR::Opcode::ReadFirstLane:
                case IR::Opcode::ReadLane:
                case IR::Opcode::WriteLane:
                case IR::Opcode::Ballot:
                case IR::Opcode::BallotFindLsb:
                case IR::Opcode::InverseBallot:
                case IR::Opcode::GroupAny:
                case IR::Opcode::Discard:
                case IR::Opcode::DiscardCond:
                case IR::Opcode::GetExec:
                case IR::Opcode::SetExec:
                    why = fmt::format("{}", inst.GetOpcode());
                    return false;
                default:
                    break;
                }
            }
        }
        return true;
    }

    /// Finds the skeleton; false (with the reason) when a region would leave into a skeleton
    /// block that has phis (the copy of a lane before the last one goes on to the next copy
    /// instead, so that block could not tell the lanes' paths apart).
    bool Analyze(std::string& why);

    void Emit() {
        Walk();
    }

    void PatchPhis() {
        size_t record = 0;
        bool first = true;
        ctx.PatchDeferredPhi([&](size_t arg, Id) {
            if (arg == 0) {
                record = first ? 0 : record + 1;
                first = false;
            }
            ASSERT(record < phis.size());
            const PhiRecord& phi = phis[record];
            const IR::Value value = phi.inst->Arg(arg);
            Id id;
            if (value.IsImmediate()) {
                id = ctx.Def(value);
            } else {
                const auto it = defs.find(value.Inst());
                ASSERT_MSG(it != defs.end() && Sirit::ValidId(it->second[phi.lane]),
                           "Lane phi argument without a definition");
                id = it->second[phi.lane];
            }
            const IR::Block* parent = phi.inst->PhiBlock(arg);
            Id label{};
            if (!phi.shared) {
                const auto it = exits[phi.lane].find(parent);
                if (it != exits[phi.lane].end()) {
                    label = it->second;
                }
            }
            if (!Sirit::ValidId(label)) {
                const auto it = final_exits.find(parent);
                ASSERT_MSG(it != final_exits.end(), "Lane phi predecessor without an exit");
                label = it->second;
            }
            return std::make_pair(id, label);
        });
    }

private:
    using Type = IR::AbstractSyntaxNode::Type;
    struct Item {
        enum class Kind : u8 { Label, Inst, Node };
        Kind kind;
        bool skeleton{};
        IR::Block* block{};
        IR::Inst* inst{};
        const IR::AbstractSyntaxNode* node{};
    };
    struct PhiRecord {
        IR::Inst* inst;
        u32 lane;
        bool shared;
    };

    static bool EmittedOnce(const IR::Inst& inst) {
        switch (inst.GetOpcode()) {
        case IR::Opcode::Barrier:
        case IR::Opcode::Prologue:
        case IR::Opcode::Epilogue:
            return true;
        default:
            return false;
        }
    }

    void FindSkeleton() {
        IR::Block* current{};
        for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
            if (node.type == Type::Block) {
                current = node.data.block;
                items.push_back({Item::Kind::Label, false, current, nullptr, nullptr});
                for (IR::Inst& inst : current->Instructions()) {
                    items.push_back({Item::Kind::Inst, false, current, &inst, nullptr});
                }
            } else {
                items.push_back({Item::Kind::Node, false, current, nullptr, &node});
            }
        }
        // Constructs: [open, close] item ranges; a loop's header is the block open at its node.
        struct Construct {
            size_t open, close;
            bool loop;
            bool skeleton;
        };
        std::vector<Construct> constructs;
        std::vector<size_t> stack;
        std::vector<size_t> loops; // construct indices
        std::vector<size_t> break_loop(items.size(), SIZE_MAX);
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].kind != Item::Kind::Node) {
                continue;
            }
            switch (items[i].node->type) {
            case Type::If:
            case Type::Loop:
                stack.push_back(constructs.size());
                if (items[i].node->type == Type::Loop) {
                    loops.push_back(constructs.size());
                }
                constructs.push_back({i, 0, items[i].node->type == Type::Loop, false});
                break;
            case Type::EndIf:
            case Type::Repeat:
                ASSERT(!stack.empty());
                constructs[stack.back()].close = i;
                if (constructs[stack.back()].loop) {
                    loops.pop_back();
                }
                stack.pop_back();
                break;
            case Type::Break:
                ASSERT(!loops.empty());
                break_loop[i] = loops.back();
                break;
            default:
                break;
            }
        }
        ASSERT(stack.empty());
        // The skeleton: barriers, the ends of the program, the constructs containing them, the
        // breaks out of skeleton loops and the constructs containing those.
        for (Item& item : items) {
            if (item.kind == Item::Kind::Inst && EmittedOnce(*item.inst)) {
                item.skeleton = true;
            }
            if (item.kind == Item::Kind::Node &&
                (item.node->type == Type::Return || item.node->type == Type::Unreachable)) {
                item.skeleton = true;
            }
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t i = 0; i < items.size(); ++i) {
                if (items[i].kind == Item::Kind::Node && items[i].node->type == Type::Break &&
                    !items[i].skeleton && constructs[break_loop[i]].skeleton) {
                    items[i].skeleton = changed = true;
                }
            }
            for (Construct& c : constructs) {
                if (c.skeleton) {
                    continue;
                }
                for (size_t i = c.open + 1; i < c.close; ++i) {
                    const Item& item = items[i];
                    const bool barrier = item.kind == Item::Kind::Inst &&
                                         item.inst->GetOpcode() == IR::Opcode::Barrier;
                    if (barrier || (item.skeleton && item.kind == Item::Kind::Node)) {
                        c.skeleton = changed = true;
                        break;
                    }
                }
            }
        }
        // Blocks entered from the skeleton: they start once.
        for (const Construct& c : constructs) {
            if (!c.skeleton) {
                continue;
            }
            items[c.open].skeleton = items[c.close].skeleton = true;
            const auto* node = items[c.open].node;
            if (c.loop) {
                skeleton_blocks.insert(items[c.open].block); // header
                skeleton_blocks.insert(node->data.loop.body);
                skeleton_blocks.insert(node->data.loop.continue_block);
                skeleton_blocks.insert(node->data.loop.merge);
            } else {
                skeleton_blocks.insert(node->data.if_node.body);
                skeleton_blocks.insert(node->data.if_node.merge);
            }
        }
        for (const Item& item : items) {
            if (item.skeleton && item.kind == Item::Kind::Node && item.node->type == Type::Break) {
                skeleton_blocks.insert(item.node->data.break_node.merge);
                skeleton_blocks.insert(item.node->data.break_node.skip);
            }
        }
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].kind == Item::Kind::Label &&
                (i == 0 || items[i - 1].skeleton || skeleton_blocks.contains(items[i].block))) {
                items[i].skeleton = true;
                skeleton_blocks.insert(items[i].block);
            }
        }
    }

    void SwitchLane(u32 lane) {
        ctx.lane = lane;
        for (auto& [inst, ids] : defs) {
            if (Sirit::ValidId(ids[lane])) {
                const_cast<IR::Inst*>(inst)->SetDefinition<Id>(ids[lane]);
            }
        }
        for (auto& [block, ids] : labels) {
            const_cast<IR::Block*>(block)->SetDefinition<Id>(ids[lane]);
        }
    }

    void Record(IR::Inst* inst, u32 lane) {
        auto& ids = defs[inst];
        ids[lane] = inst->Definition<Id>();
    }

    /// A branch target: inside a lane copy, a skeleton block is left for the next lane's copy
    /// (the last lane's copy goes on to it).
    Id Target(const IR::Block* block) {
        if (in_copy && skeleton_blocks.contains(block)) {
            leaving = true;
            if (ctx.lane + 1 < lanes) {
                return copy_labels[ctx.lane + 1];
            }
        }
        return block->Definition<Id>();
    }

    void CloseBlock() {
        if (open) {
            if (in_copy) {
                exits[ctx.lane][open] = ctx.last_label;
                if (leaving && ctx.lane + 1 == lanes) {
                    final_exits[open] = ctx.last_label;
                }
            } else {
                final_exits[open] = ctx.last_label;
            }
        }
        open = nullptr;
        leaving = false;
    }

    void EmitPhi(IR::Inst* inst, u32 lane, bool shared) {
        const size_t num_args{inst->NumArgs()};
        boost::container::small_vector<Id, 32> blocks(num_args, ctx.void_id);
        const Id id = ctx.DeferredOpPhi(TypeId(ctx, inst->Flags<IR::Type>()),
                                        std::span(blocks.data(), blocks.size()));
        inst->SetDefinition<Id>(id);
        phis.push_back({inst, lane, shared});
        Record(inst, lane);
    }

    void EmitLabel(IR::Block* block) {
        const Id label = block->Definition<Id>();
        if (open) {
            ctx.OpBranch(label);
            CloseBlock();
        }
        ctx.AddLabel(label);
        open = block;
    }

    /// A structured node, emitted in the current lane (conditions from its definitions).
    void EmitNode(const IR::AbstractSyntaxNode& node) {
        switch (node.type) {
        case Type::If:
            ctx.OpSelectionMerge(Target(node.data.if_node.merge),
                                 spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(ctx.Def(node.data.if_node.cond),
                                    Target(node.data.if_node.body),
                                    Target(node.data.if_node.merge));
            CloseBlock();
            break;
        case Type::Loop:
            ctx.OpLoopMerge(Target(node.data.loop.merge),
                            Target(node.data.loop.continue_block),
                            spv::LoopControlMask::MaskNone);
            ctx.OpBranch(Target(node.data.loop.body));
            CloseBlock();
            break;
        case Type::Break:
            ctx.OpBranchConditional(ctx.Def(node.data.break_node.cond),
                                    Target(node.data.break_node.merge),
                                    Target(node.data.break_node.skip));
            CloseBlock();
            break;
        case Type::EndIf:
            if (open) {
                ctx.OpBranch(Target(node.data.end_if.merge));
                CloseBlock();
            }
            break;
        case Type::Repeat:
            ctx.OpBranchConditional(ctx.Def(node.data.repeat.cond),
                                    Target(node.data.repeat.loop_header),
                                    Target(node.data.repeat.merge));
            CloseBlock();
            break;
        case Type::Return:
            ctx.OpReturn();
            open = nullptr;
            break;
        case Type::Unreachable:
            ctx.OpUnreachable();
            open = nullptr;
            break;
        case Type::Block:
            UNREACHABLE();
        }
    }

    /// A region: once per lane, each copy after the previous one.
    void EmitRegion(size_t begin, size_t end) {
        if (begin == end) {
            return;
        }
        bool straight = true;
        for (size_t i = begin; i < end; ++i) {
            straight &= items[i].kind == Item::Kind::Inst;
        }
        IR::Block* const start = open;
        for (size_t i = begin; i < end; ++i) {
            if (items[i].kind == Item::Kind::Label) {
                auto& ids = labels[items[i].block];
                ids[0] = items[i].block->Definition<Id>();
                for (u32 lane = 1; lane < lanes; ++lane) {
                    ids[lane] = ctx.OpLabel();
                }
            }
        }
        for (u32 lane = 1; lane < lanes; ++lane) {
            copy_labels[lane] = straight ? Id{} : ctx.OpLabel();
        }
        in_copy = true;
        for (u32 lane = 0; lane < lanes; ++lane) {
            SwitchLane(lane);
            if (lane > 0 && !straight) {
                // The previous copy falls through, or left for this one (Target).
                if (open) {
                    ctx.OpBranch(copy_labels[lane]);
                }
                ctx.AddLabel(copy_labels[lane]);
                open = start;
                leaving = false;
            }
            for (size_t i = begin; i < end; ++i) {
                const Item& item = items[i];
                switch (item.kind) {
                case Item::Kind::Label:
                    EmitLabel(item.block);
                    break;
                case Item::Kind::Inst:
                    if (item.inst->GetOpcode() == IR::Opcode::Phi) {
                        EmitPhi(item.inst, lane, false);
                    } else {
                        EmitInst(ctx, item.inst);
                        Record(item.inst, lane);
                    }
                    break;
                case Item::Kind::Node:
                    EmitNode(*item.node);
                    break;
                }
            }
        }
        in_copy = false;
        SwitchLane(0);
    }

    void Walk() {
        size_t region = 0;
        for (size_t i = 0; i < items.size(); ++i) {
            const Item& item = items[i];
            if (!item.skeleton) {
                continue;
            }
            EmitRegion(region, i);
            SwitchLane(0);
            switch (item.kind) {
            case Item::Kind::Label: {
                EmitLabel(item.block);
                // Its phis: one per lane, entered from the skeleton.
                size_t j = i + 1;
                for (; j < items.size() && items[j].kind == Item::Kind::Inst &&
                       items[j].inst->GetOpcode() == IR::Opcode::Phi;
                     ++j) {
                    for (u32 lane = 0; lane < lanes; ++lane) {
                        EmitPhi(items[j].inst, lane, true);
                    }
                }
                SwitchLane(0);
                i = j - 1;
                break;
            }
            case Item::Kind::Inst:
                EmitInst(ctx, item.inst);
                break;
            case Item::Kind::Node:
                EmitNode(*item.node);
                break;
            }
            region = i + 1;
        }
        EmitRegion(region, items.size());
    }

    EmitContext& ctx;
    const IR::Program& program;
    const u32 lanes;
    std::vector<Item> items;
    std::unordered_set<const IR::Block*> skeleton_blocks;
    std::unordered_map<const IR::Inst*, std::array<Id, 4>> defs;
    std::unordered_map<const IR::Block*, std::array<Id, 4>> labels;
    std::array<std::unordered_map<const IR::Block*, Id>, 4> exits;
    std::unordered_map<const IR::Block*, Id> final_exits;
    std::vector<PhiRecord> phis;
    std::array<Id, 4> copy_labels{};
    IR::Block* open{};
    bool in_copy{};
    bool leaving{};
};

bool LaneTraversal::Analyze(std::string& why) {
    FindSkeleton();
    const auto leaves_into_phis = [&](const IR::Block* target) {
        return skeleton_blocks.contains(target) && !target->empty() &&
               target->front().GetOpcode() == IR::Opcode::Phi;
    };
    for (const Item& item : items) {
        if (item.kind != Item::Kind::Node || item.skeleton) {
            continue;
        }
        const auto& d = item.node->data;
        bool bad = false;
        switch (item.node->type) {
        case Type::If:
            bad = leaves_into_phis(d.if_node.merge) || leaves_into_phis(d.if_node.body);
            break;
        case Type::EndIf:
            bad = leaves_into_phis(d.end_if.merge);
            break;
        case Type::Loop:
            bad = leaves_into_phis(d.loop.merge) || leaves_into_phis(d.loop.body) ||
                  leaves_into_phis(d.loop.continue_block);
            break;
        case Type::Break:
            bad = leaves_into_phis(d.break_node.merge) || leaves_into_phis(d.break_node.skip);
            break;
        case Type::Repeat:
            bad = leaves_into_phis(d.repeat.merge) || leaves_into_phis(d.repeat.loop_header);
            break;
        default:
            break;
        }
        if (bad) {
            why = "a region leaves into a skeleton block with phis";
            return false;
        }
    }
    return true;
}

Id DefineMain(EmitContext& ctx, const IR::Program& program, LaneTraversal* lanes) {
    const Id void_function{ctx.TypeFunction(ctx.void_id)};
    const Id main{ctx.OpFunction(ctx.void_id, spv::FunctionControlMask::MaskNone, void_function)};
    for (IR::Block* const block : program.blocks) {
        block->SetDefinition(ctx.OpLabel());
    }
    if (lanes) {
        lanes->Emit();
    } else {
        Traverse(ctx, program);
    }
    ctx.OpFunctionEnd();
    return main;
}

spv::ExecutionMode ExecutionMode(AmdGpu::TessellationType primitive) {
    switch (primitive) {
    case AmdGpu::TessellationType::Isoline:
        return spv::ExecutionMode::Isolines;
    case AmdGpu::TessellationType::Triangle:
        return spv::ExecutionMode::Triangles;
    case AmdGpu::TessellationType::Quad:
        return spv::ExecutionMode::Quads;
    }
    UNREACHABLE_MSG("Tessellation primitive {}", magic_enum::enum_name(primitive));
}

spv::ExecutionMode ExecutionMode(AmdGpu::TessellationPartitioning spacing) {
    switch (spacing) {
    case AmdGpu::TessellationPartitioning::Integer:
        return spv::ExecutionMode::SpacingEqual;
    case AmdGpu::TessellationPartitioning::FracOdd:
        return spv::ExecutionMode::SpacingFractionalOdd;
    case AmdGpu::TessellationPartitioning::FracEven:
        return spv::ExecutionMode::SpacingFractionalEven;
    case AmdGpu::TessellationPartitioning::Pow2:
        // Pow2 rounds tessellation factors to the nearest power of 2, which has no
        // direct Vulkan equivalent. SpacingEqual (integer) is the closest match.
        LOG_WARNING(Render_Vulkan, "Tessellation partitioning Pow2 has no Vulkan equivalent, "
                                   "falling back to SpacingEqual");
        return spv::ExecutionMode::SpacingEqual;
    default:
        break;
    }
    UNREACHABLE_MSG("Tessellation spacing {}", magic_enum::enum_name(spacing));
}

void SetupCapabilities(const Info& info, const Profile& profile, const RuntimeInfo& runtime_info,
                       EmitContext& ctx) {
    ctx.AddCapability(spv::Capability::Image1D);
    ctx.AddCapability(spv::Capability::Sampled1D);
    ctx.AddCapability(spv::Capability::ImageQuery);
    ctx.AddCapability(spv::Capability::Int8);
    ctx.AddCapability(spv::Capability::Int16);
    ctx.AddCapability(spv::Capability::Int64);
    ctx.AddCapability(spv::Capability::StorageBuffer8BitAccess);
    ctx.AddCapability(spv::Capability::StorageBuffer16BitAccess);
    if (info.uses_fp16) {
        ctx.AddCapability(spv::Capability::Float16);
    }
    if (info.uses_fp64) {
        ctx.AddCapability(spv::Capability::Float64);
    }
    if (info.has_storage_images) {
        ctx.AddCapability(spv::Capability::StorageImageExtendedFormats);
        ctx.AddCapability(spv::Capability::StorageImageReadWithoutFormat);
        ctx.AddCapability(spv::Capability::StorageImageWriteWithoutFormat);
        if (profile.supports_image_load_store_lod) {
            ctx.AddExtension("SPV_AMD_shader_image_load_store_lod");
            ctx.AddCapability(spv::Capability::ImageReadWriteLodAMD);
        }
    }
    if (info.has_image_gather) {
        ctx.AddCapability(spv::Capability::ImageGatherExtended);
    }
    if (info.has_image_query) {
        ctx.AddCapability(spv::Capability::ImageQuery);
    }
    if ((info.uses_image_atomic_float_min_max && profile.supports_image_fp32_atomic_min_max) ||
        (info.uses_buffer_atomic_float_min_max && profile.supports_buffer_fp32_atomic_min_max)) {
        ctx.AddExtension("SPV_EXT_shader_atomic_float_min_max");
        ctx.AddCapability(spv::Capability::AtomicFloat32MinMaxEXT);
    }
    if (info.uses_lane_id) {
        ctx.AddCapability(spv::Capability::GroupNonUniform);
    }
    if (info.uses_group_quad) {
        ctx.AddCapability(spv::Capability::GroupNonUniformQuad);
    }
    if (info.uses_group_shuffle) {
        ctx.AddCapability(spv::Capability::GroupNonUniformShuffle);
    }
    if (info.uses_group_ballot || info.loads.Get(IR::Attribute::SubgroupLtMask)) {
        ctx.AddCapability(spv::Capability::GroupNonUniformBallot);
    }
    if (info.uses_shader_clock) {
        if (ctx.profile.supports_shader_subgroup_clock) {
            ctx.AddExtension("SPV_KHR_shader_clock");
            ctx.AddCapability(spv::Capability::ShaderClockKHR);
        } else {
            LOG_WARNING(Render_Recompiler,
                        "Shader requires support for ShaderClockKHR capability"
                        " that your Vulkan instance does not advertise. Results may vary");
        }
    }
    const auto stage = info.sw_stage;
    if (stage == SwStage::Vertex) {
        ctx.AddExtension("SPV_KHR_shader_draw_parameters");
        ctx.AddCapability(spv::Capability::DrawParameters);
    }
    if (stage == SwStage::Geometry) {
        ctx.AddCapability(spv::Capability::Geometry);
    }
    if (info.hw_stage == HwStage::Fragment) {
        if (profile.supports_amd_shader_explicit_vertex_parameter) {
            ctx.AddExtension("SPV_AMD_shader_explicit_vertex_parameter");
        } else if (profile.supports_fragment_shader_barycentric) {
            ctx.AddExtension("SPV_KHR_fragment_shader_barycentric");
            ctx.AddCapability(spv::Capability::FragmentBarycentricKHR);
            ctx.AddCapability(spv::Capability::InterpolationFunction);
        }
        if (info.loads.Get(IR::Attribute::SampleIndex) ||
            runtime_info.hw.fs.addr_flags.linear_sample_ena ||
            runtime_info.hw.fs.addr_flags.persp_sample_ena ||
            (!profile.supports_amd_shader_explicit_vertex_parameter &&
             profile.supports_fragment_shader_barycentric &&
             info.loads.Get(IR::Attribute::BaryCoordSmoothSample))) {
            ctx.AddCapability(spv::Capability::SampleRateShading);
        }
        if (info.loads.GetAny(IR::Attribute::RenderTargetIndex)) {
            ctx.AddCapability(spv::Capability::Geometry);
        }
        if (info.stores.Get(IR::Attribute::StencilRef) && profile.supports_shader_stencil_export) {
            ctx.AddExtension("SPV_EXT_shader_stencil_export");
            ctx.AddCapability(spv::Capability::StencilExportEXT);
        }
    }
    if (stage == SwStage::TessellationControl || stage == SwStage::TessellationEval) {
        ctx.AddCapability(spv::Capability::Tessellation);
    }
    if (stage == SwStage::Vertex || stage == SwStage::TessellationControl ||
        stage == SwStage::TessellationEval) {
        if (info.stores.GetAny(IR::Attribute::RenderTargetIndex)) {
            ctx.AddCapability(spv::Capability::ShaderLayer);
        }
        if (info.stores.GetAny(IR::Attribute::ViewportIndex)) {
            ctx.AddCapability(spv::Capability::ShaderViewportIndex);
        }
    } else if (stage == SwStage::Geometry && info.stores.GetAny(IR::Attribute::ViewportIndex)) {
        ctx.AddCapability(spv::Capability::MultiViewport);
    }
    if (info.uses_dma || ctx.VertexMotion()) {
        ctx.AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
        ctx.AddExtension("SPV_KHR_physical_storage_buffer");
    }
    const auto shared_type_count = std::popcount(static_cast<u32>(info.shared_types));
    if (shared_type_count > 1 && profile.supports_workgroup_explicit_memory_layout) {
        ctx.AddExtension("SPV_KHR_workgroup_memory_explicit_layout");
        ctx.AddCapability(spv::Capability::WorkgroupMemoryExplicitLayoutKHR);
        ctx.AddCapability(spv::Capability::WorkgroupMemoryExplicitLayout16BitAccessKHR);
    }
    if (info.uses_buffer_int64_atomics || info.uses_shared_int64_atomics) {
        if (info.uses_buffer_int64_atomics) {
            ASSERT_MSG(ctx.profile.supports_buffer_int64_atomics,
                       "Shader requires support for atomic Int64 buffer operations that your "
                       "Vulkan instance does not advertise");
        }
        if (info.uses_shared_int64_atomics) {
            ASSERT_MSG(ctx.profile.supports_shared_int64_atomics,
                       "Shader requires support for atomic Int64 shared memory operations that "
                       "your Vulkan instance does not advertise");
        }
        ctx.AddCapability(spv::Capability::Int64Atomics);
    }
}

void DefineEntryPoint(const Info& info, EmitContext& ctx, Id main) {
    const std::span interfaces(ctx.interfaces.data(), ctx.interfaces.size());
    spv::ExecutionModel execution_model{};
    switch (info.sw_stage) {
    case SwStage::Compute: {
        const std::array<u32, 3> workgroup_size{ctx.runtime_info.hw.cs.workgroup_size};
        execution_model = spv::ExecutionModel::GLCompute;
        // gow3 10 Nov 2026: split workgroups (WorkgroupSplitFactors, runtime_info.h). The dispatch
        // reads the split back from this LocalSize (ShaderModuleLocalSize).
        ctx.AddExecutionMode(main, spv::ExecutionMode::LocalSize,
                             workgroup_size[0] / ctx.workgroup_split[0] / ctx.lanes,
                             workgroup_size[1] / ctx.workgroup_split[1],
                             workgroup_size[2] / ctx.workgroup_split[2]);
        break;
    }
    case SwStage::Vertex:
        execution_model = spv::ExecutionModel::Vertex;
        break;
    case SwStage::TessellationControl:
        execution_model = spv::ExecutionModel::TessellationControl;
        ctx.AddCapability(spv::Capability::Tessellation);
        ctx.AddExecutionMode(main, spv::ExecutionMode::OutputVertices,
                             ctx.runtime_info.sw.tcs.NumOutputControlPoints());
        break;
    case SwStage::TessellationEval: {
        execution_model = spv::ExecutionModel::TessellationEvaluation;
        ctx.AddExecutionMode(main, ExecutionMode(ctx.runtime_info.sw.tes.tess_type));
        ctx.AddExecutionMode(main, ExecutionMode(ctx.runtime_info.sw.tes.tess_partitioning));
        ctx.AddExecutionMode(main, ctx.runtime_info.sw.tes.tess_topology ==
                                           AmdGpu::TessellationTopology::TriangleCcw
                                       ? spv::ExecutionMode::VertexOrderCcw
                                       : spv::ExecutionMode::VertexOrderCw);
        break;
    }
    case SwStage::Fragment:
        execution_model = spv::ExecutionModel::Fragment;
        if (ctx.profile.lower_left_origin_mode) {
            ctx.AddExecutionMode(main, spv::ExecutionMode::OriginLowerLeft);
        } else {
            ctx.AddExecutionMode(main, spv::ExecutionMode::OriginUpperLeft);
        }
        if (info.has_discard) {
            ctx.AddCapability(spv::Capability::DemoteToHelperInvocation);
        }
        if (info.stores.GetAny(IR::Attribute::Depth)) {
            ctx.AddExecutionMode(main, spv::ExecutionMode::DepthReplacing);
        }
        break;
    case SwStage::Geometry:
        execution_model = spv::ExecutionModel::Geometry;
        ctx.AddExecutionMode(main, GetInputPrimitiveType(ctx.runtime_info.hw.gs.in_primitive));
        ctx.AddExecutionMode(main, GetOutputPrimitiveType(ctx.runtime_info.hw.gs.out_primitive[0]));
        ctx.AddExecutionMode(main, spv::ExecutionMode::OutputVertices,
                             ctx.runtime_info.hw.gs.output_vertices);
        ctx.AddExecutionMode(main, spv::ExecutionMode::Invocations,
                             ctx.runtime_info.hw.gs.num_invocations);
        break;
    default:
        UNREACHABLE_MSG("Stage {}", u32(info.hw_stage));
    }
    ctx.AddEntryPoint(execution_model, main, "main", interfaces);
}

void SetupDenormFlushMode(EmitContext& ctx, const Profile& profile, const RuntimeInfo& runtime_info,
                          Id main_func) {
    const auto fp32_denorm_mode = runtime_info.props.fp_denorm_mode32;
    if (fp32_denorm_mode == AmdGpu::FpDenormMode::InOutFlush) {
        if (profile.support_fp32_denorm_flush) {
            ctx.AddCapability(spv::Capability::DenormFlushToZero);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormFlushToZero, 32U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float32 denorm flushing is not supported by the GPU");
            });
        }
    } else {
        LOG_WARNING(Render_Vulkan, "Unknown FP denorm mode {}", u32(fp32_denorm_mode));
    }

    if (!ctx.info.uses_fp16 && !ctx.info.uses_fp64) {
        return;
    }

    const auto fp16_64_denorm_mode = runtime_info.props.fp_denorm_mode16_64;
    const auto same_denorm_mode = fp16_64_denorm_mode == fp32_denorm_mode;
    if (!same_denorm_mode && !profile.supports_denorm_behavior_independence) {
        static std::once_flag logged;
        std::call_once(logged, [] {
            LOG_WARNING(Render_Vulkan, "Float32 and Float16/64 denorm modes are different but the "
                                       "GPU does not support that");
        });
        return;
    }

    if (fp16_64_denorm_mode == AmdGpu::FpDenormMode::InOutFlush) {
        if (profile.support_fp16_denorm_flush) {
            ctx.AddCapability(spv::Capability::DenormFlushToZero);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormFlushToZero, 16U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float16 denorm flushing is not supported by the GPU");
            });
        }
        if (profile.support_fp64_denorm_flush) {
            ctx.AddCapability(spv::Capability::DenormFlushToZero);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormFlushToZero, 64U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float64 denorm flushing is not supported by the GPU");
            });
        }
    } else if (fp16_64_denorm_mode == AmdGpu::FpDenormMode::InOutAllow) {
        if (profile.support_fp16_denorm_preserve) {
            ctx.AddCapability(spv::Capability::DenormPreserve);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormPreserve, 16U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float16 denorm preserving is not supported by the GPU");
            });
        }
        if (profile.support_fp64_denorm_preserve) {
            ctx.AddCapability(spv::Capability::DenormPreserve);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormPreserve, 64U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float64 denorm preserving is not supported by the GPU");
            });
        }
    } else {
        LOG_WARNING(Render_Vulkan, "Unknown Float16/64 denorm mode {}", u32(fp16_64_denorm_mode));
    }
}

void SetupRoundingMode(EmitContext& ctx, const Profile& profile, const RuntimeInfo& runtime_info,
                       Id main_func) {
    const auto fp_round_mode = runtime_info.props.fp_round_mode32;
    if (fp_round_mode == AmdGpu::FpRoundMode::ToZero) {
        if (profile.support_fp32_round_to_zero) {
            ctx.AddCapability(spv::Capability::RoundingModeRTZ);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::RoundingModeRTZ, 32U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float32 rounding to zero is not supported by the GPU");
            });
        }
    } else if (fp_round_mode != AmdGpu::FpRoundMode::NearestEven) {
        LOG_WARNING(Render_Vulkan, "Unimplemented FP rounding mode {}",
                    magic_enum::enum_name(fp_round_mode));
    }

    if (ctx.info.uses_fp16 || ctx.info.uses_fp64) {
        const auto fp16_64_round_mode = runtime_info.props.fp_round_mode16_64;
        const auto same_round_mode = fp16_64_round_mode == fp_round_mode;
        if (!same_round_mode && !profile.supports_rounding_mode_independence) {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float32 and Float16/64 rounding modes are different "
                                           "but the GPU does not support that");
            });
            return;
        }

        if (fp16_64_round_mode == AmdGpu::FpRoundMode::ToZero) {
            if (ctx.info.uses_fp16) {
                if (profile.support_fp16_round_to_zero) {
                    ctx.AddCapability(spv::Capability::RoundingModeRTZ);
                    ctx.AddExecutionMode(main_func, spv::ExecutionMode::RoundingModeRTZ, 16U);
                } else {
                    static std::once_flag logged;
                    std::call_once(logged, [] {
                        LOG_WARNING(Render_Vulkan,
                                    "Float16 rounding to zero is not supported by the GPU");
                    });
                }
            }
            if (ctx.info.uses_fp64) {
                if (profile.support_fp64_round_to_zero) {
                    ctx.AddCapability(spv::Capability::RoundingModeRTZ);
                    ctx.AddExecutionMode(main_func, spv::ExecutionMode::RoundingModeRTZ, 64U);
                } else {
                    static std::once_flag logged;
                    std::call_once(logged, [] {
                        LOG_WARNING(Render_Vulkan,
                                    "Float64 rounding to zero is not supported by the GPU");
                    });
                }
            }
        } else if (fp16_64_round_mode != AmdGpu::FpRoundMode::NearestEven) {
            LOG_WARNING(Render_Vulkan, "Unknown Float16/64 rounding mode {}",
                        u32(fp16_64_round_mode));
        }
    }
}

void SetupInfNanPreserveMode(EmitContext& ctx, const Profile& profile,
                             const RuntimeInfo& runtime_info, Id main_func) {
    if (profile.support_fp16_signed_zero_inf_nan_preserve ||
        profile.support_fp32_signed_zero_inf_nan_preserve ||
        profile.support_fp64_signed_zero_inf_nan_preserve) {
        ctx.AddCapability(spv::Capability::SignedZeroInfNanPreserve);
    }

    if (profile.support_fp32_signed_zero_inf_nan_preserve) {
        ctx.AddExecutionMode(main_func, spv::ExecutionMode::SignedZeroInfNanPreserve, 32U);
    } else {
        static std::once_flag logged;
        std::call_once(logged, [] {
            LOG_WARNING(Render_Vulkan,
                        "Float32 signed zero/inf/nan preserve mode is not supported by the GPU");
        });
    }
    if (ctx.info.uses_fp16) {
        if (profile.support_fp16_signed_zero_inf_nan_preserve) {
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::SignedZeroInfNanPreserve, 16U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(
                    Render_Vulkan,
                    "Float16 signed zero/inf/nan preserve mode is not supported by the GPU");
            });
        }
    }
    if (ctx.info.uses_fp64) {
        if (profile.support_fp64_signed_zero_inf_nan_preserve) {
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::SignedZeroInfNanPreserve, 64U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(
                    Render_Vulkan,
                    "Float64 signed zero/inf/nan preserve mode is not supported by the GPU");
            });
        }
    }
}

void SetupFloatMode(EmitContext& ctx, const Profile& profile, const RuntimeInfo& runtime_info,
                    Id main_func) {
    ctx.AddExtension("SPV_KHR_float_controls");
    SetupDenormFlushMode(ctx, profile, runtime_info, main_func);
    SetupRoundingMode(ctx, profile, runtime_info, main_func);
    SetupInfNanPreserveMode(ctx, profile, runtime_info, main_func);
}

void PatchPhiNodes(const IR::Program& program, EmitContext& ctx) {
    auto inst{program.blocks.front()->begin()};
    size_t block_index{0};
    ctx.PatchDeferredPhi([&](u32 phi_arg, Id first_parent) {
        if (phi_arg == 0) {
            ++inst;
            while (inst == program.blocks[block_index]->end() ||
                   inst->GetOpcode() != IR::Opcode::Phi) {
                ++block_index;
                inst = program.blocks[block_index]->begin();
            }
        }
        ASSERT(inst != program.blocks[block_index]->end());
        const Id arg = ctx.Def(inst->Arg(phi_arg));
        const Id parent = ctx.first_to_last_label_map[first_parent.value];
        return std::make_pair(arg, parent);
    });
}
} // Anonymous namespace

std::vector<u32> EmitSPIRV(const Profile& profile, const RuntimeInfo& runtime_info,
                           const IR::Program& program, Bindings& binding) {
    EmitContext ctx{profile, runtime_info, program.info, binding};
    // gow3 11 Nov 2026: lane coarsening (LaneTraversal).
    std::optional<LaneTraversal> lanes;
    if (ctx.lanes > 1) {
        std::string why;
        lanes.emplace(ctx, program);
        if (!LaneTraversal::Eligible(program, why) || !lanes->Analyze(why)) {
            std::printf("GPU: compute shader %#llx runs one invocation per thread: %s\n",
                        static_cast<unsigned long long>(program.info.pgm_hash), why.c_str());
            std::fflush(stdout);
            lanes.reset();
            ctx.lanes = 1;
        } else {
            std::printf("GPU: compute shader %#llx runs %u invocations per thread (%u threads)\n",
                        static_cast<unsigned long long>(program.info.pgm_hash), ctx.lanes,
                        runtime_info.hw.cs.workgroup_size[0] / ctx.lanes);
            std::fflush(stdout);
        }
    }
    const Id main{DefineMain(ctx, program, lanes ? &*lanes : nullptr)};
    DefineEntryPoint(program.info, ctx, main);
    SetupCapabilities(program.info, profile, runtime_info, ctx);
    SetupFloatMode(ctx, profile, runtime_info, main);
    if (lanes) {
        lanes->PatchPhis();
        ctx.Name(main, fmt::format("gow3_lanes_{}", ctx.lanes));
    } else {
        PatchPhiNodes(program, ctx);
    }
    binding.user_data += program.info.ud_mask.NumRegs();
    return ctx.Assemble();
}

Id EmitPhi(EmitContext& ctx, IR::Inst* inst) {
    const size_t num_args{inst->NumArgs()};
    boost::container::small_vector<Id, 32> blocks;
    blocks.reserve(num_args);
    for (size_t index = 0; index < num_args; ++index) {
        blocks.push_back(inst->PhiBlock(index)->Definition<Id>());
    }
    // The type of a phi instruction is stored in its flags
    const Id result_type{TypeId(ctx, inst->Flags<IR::Type>())};
    return ctx.DeferredOpPhi(result_type, std::span(blocks.data(), blocks.size()));
}

void EmitVoid(EmitContext&) {}

Id EmitConditionRef(EmitContext& ctx, const IR::Value& value) {
    const Id id{ctx.Def(value)};
    ASSERT_MSG(Sirit::ValidId(id), "Forward identity declaration");
    return id;
}

Id EmitGetPcLo(EmitContext& ctx, const IR::Value& value) {
    const Id id{ctx.Def(value)};
    ASSERT_MSG(Sirit::ValidId(id), "Forward identity declaration");
    return id;
}

void EmitGetScc(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetExec(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetVcc(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetVccLo(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetVccHi(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetM0(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetScc(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetExec(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetVcc(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetVccLo(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetVccHi(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetM0(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

} // namespace Shader::Backend::SPIRV

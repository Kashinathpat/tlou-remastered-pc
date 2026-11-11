// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include "common/types.h"

namespace Vulkan {

// gow3 11 Nov 2026, The Last of Us Remastered on a display of another aspect ratio than 16:9
// (GOW3_DISPLAY_ASPECT=<w>x<h>, read in vk_shader_util.cpp; tools/probe_tlou.py sets it with its
// aspect ratio patches): the movies (the cutscenes) are 16:9 NV12 frames that one compute shader
// (cs 0x562c4394, 1.11) converts to RGB across its whole target, so they came out stretched. When
// the target has the display's aspect ratio, the sampled coordinate is scaled about the middle to
// keep 16:9 (u' = (u - 0.5) * max(R, 1) + 0.5, v' likewise with max(1/R, 1); R = target aspect /
// (16/9)) and the texels left outside the frame are written black (pillarbox; letterbox below
// 16:9). The idea is github.com/SanyaGames227/tlou_r_shadps4_ultrawide's (a SPIR-V patch with a
// fixed R); here R comes from the target's size, so other targets of the shader stay as they are.
// Applied when the module is created, so also to SPIR-V from the shader cache.
inline constexpr std::string_view kMovieShaderName = "0x562c4394";

// Returns the movie shader with the bars added, or an empty vector (another shader, a 16:9
// display, or a module of an unexpected shape).
inline std::vector<u32> WithMovieAspect(std::span<const u32> code, float display) {
    constexpr size_t HeaderWords = 5;
    constexpr u32 OpString = 7, OpCapability = 17, OpTypeBool = 20, OpTypeInt = 21,
                  OpTypeFloat = 22, OpTypeVector = 23, OpConstant = 43,
                  OpConstantComposite = 44, OpFunction = 54, OpLabel = 248, OpLoad = 61,
                  OpCompositeConstruct = 80, OpCompositeExtract = 81,
                  OpImageSampleImplicitLod = 87, OpImageSampleExplicitLod = 88,
                  OpImageWrite = 99, OpImageQuerySize = 104, OpConvertUToF = 112, OpFAdd = 129,
                  OpFSub = 131, OpFMul = 133, OpFDiv = 136, OpLogicalAnd = 167, OpSelect = 169,
                  OpFOrdGreaterThan = 186, OpFOrdLessThanEqual = 188,
                  OpFOrdGreaterThanEqual = 190, CapabilityImageQuery = 50;
    if (display <= 0.0f || std::abs(display / (16.0f / 9.0f) - 1.0f) < 0.003f ||
        code.size() <= HeaderWords || code[0] != 0x07230203u) {
        return {};
    }
    struct Op {
        size_t at;
        u32 opcode, words;
    };
    std::vector<Op> ops;
    std::unordered_map<u32, size_t> defined; // result id -> index in ops
    bool named = false, image_query = false;
    u32 float_type = 0, u32_type = 0, boolean = 0, f32vec2 = 0, f32vec4 = 0, u32vec2 = 0;
    std::unordered_map<u32, std::pair<u32, u32>> vectors; // id -> (component, count)
    for (size_t at = HeaderWords; at < code.size();) {
        const u32 words = code[at] >> 16;
        const u32 opcode = code[at] & 0xffffu;
        if (words == 0 || at + words > code.size()) {
            return {};
        }
        ops.push_back({at, opcode, words});
        if (opcode == OpString && words >= 3) {
            const char* text = reinterpret_cast<const char*>(&code[at + 2]);
            named |= std::string_view{text, strnlen(text, (words - 2) * sizeof(u32))} ==
                     kMovieShaderName;
        } else if (opcode == OpCapability && words == 2 && code[at + 1] == CapabilityImageQuery) {
            image_query = true;
        } else if (opcode == OpTypeBool && words == 2) {
            boolean = code[at + 1];
        } else if (opcode == OpTypeFloat && words >= 3 && code[at + 2] == 32) {
            float_type = code[at + 1];
        } else if (opcode == OpTypeInt && words == 4 && code[at + 2] == 32 && code[at + 3] == 0) {
            u32_type = code[at + 1];
        } else if (opcode == OpTypeVector && words == 4) {
            vectors[code[at + 1]] = {code[at + 2], code[at + 3]};
        } else if (opcode == OpLoad || opcode == OpCompositeConstruct) {
            defined[code[at + 2]] = ops.size() - 1;
        }
        at += words;
    }
    if (!named) {
        return {};
    }
    for (const auto& [id, vector] : vectors) {
        f32vec2 = vector == std::pair{float_type, 2u} ? id : f32vec2;
        f32vec4 = vector == std::pair{float_type, 4u} ? id : f32vec4;
        u32vec2 = vector == std::pair{u32_type, 2u} ? id : u32vec2;
    }
    // The coordinates the frame's planes are sampled at (one vec2 (u, v), maybe built twice) and
    // the one write of the result.
    std::vector<size_t> coordinates;
    size_t write = 0, function = 0;
    for (size_t i = 0; i < ops.size(); ++i) {
        const auto [at, opcode, words] = ops[i];
        if (opcode == OpFunction && !function) {
            function = i;
        } else if ((opcode == OpImageSampleImplicitLod || opcode == OpImageSampleExplicitLod) &&
                   words >= 5) {
            const auto it = defined.find(code[at + 4]);
            if (it == defined.end() || ops[it->second].opcode != OpCompositeConstruct ||
                ops[it->second].words != 5) {
                return {};
            }
            coordinates.push_back(it->second);
        } else if (opcode == OpImageWrite) {
            if (write) {
                return {};
            }
            write = i;
        }
    }
    const auto failed = [] {
        std::printf("GPU: the movie shader %s has an unexpected shape; movies stay stretched\n",
                    kMovieShaderName.data());
        std::fflush(stdout);
        return std::vector<u32>{};
    };
    if (!float_type || !boolean || !f32vec2 || !f32vec4 || !u32vec2 || !function || !write ||
        coordinates.empty()) {
        return failed();
    }
    const size_t first = *std::min_element(coordinates.begin(), coordinates.end());
    const u32 u = code[ops[first].at + 3], v = code[ops[first].at + 4];
    const auto target_load = defined.find(code[ops[write].at + 1]);
    if (target_load == defined.end() || ops[target_load->second].opcode != OpLoad || first > write) {
        return failed();
    }
    for (const size_t c : coordinates) {
        if (code[ops[c].at + 3] != u || code[ops[c].at + 4] != v) {
            return failed();
        }
    }
    for (size_t i = first; i < write; ++i) {
        if (ops[i].opcode == OpLabel) { // the write must be in the coordinates' block
            return failed();
        }
    }
    const u32 image_type = code[ops[target_load->second].at + 1];
    const u32 image_variable = code[ops[target_load->second].at + 3];
    const u32 texel = code[ops[write].at + 3];

    u32 bound = code[3];
    const auto id = [&bound] { return bound++; };
    const auto bits = [](float value) { return std::bit_cast<u32>(value); };
    std::vector<u32> constants;
    const auto constant = [&](float value) {
        const u32 result = id();
        constants.insert(constants.end(), {(4u << 16) | OpConstant, float_type, result, bits(value)});
        return result;
    };
    const u32 zero = constant(0.0f), one = constant(1.0f), half = constant(0.5f),
              nine_sixteenths = constant(9.0f / 16.0f), low = constant(display * 0.99f),
              high = constant(display * 1.01f), black = id();
    constants.insert(constants.end(),
                     {(7u << 16) | OpConstantComposite, f32vec4, black, zero, zero, zero, zero});
    std::vector<u32> scale;
    const auto op = [](std::vector<u32>& out, u32 opcode, std::initializer_list<u32> operands) {
        out.push_back((static_cast<u32>(operands.size() + 1) << 16) | opcode);
        out.insert(out.end(), operands);
    };
    // R from the target's size; the scale only when the target has the display's aspect ratio.
    const u32 image = id(), size = id(), sizef = id(), width = id(), height = id(), aspect = id(),
              above = id(), below = id(), matches = id(), r = id(), inverse = id(), wide = id(),
              tall = id(), sx_any = id(), sy_any = id(), sx = id(), sy = id();
    op(scale, OpLoad, {image_type, image, image_variable});
    op(scale, OpImageQuerySize, {u32vec2, size, image});
    op(scale, OpConvertUToF, {f32vec2, sizef, size});
    op(scale, OpCompositeExtract, {float_type, width, sizef, 0});
    op(scale, OpCompositeExtract, {float_type, height, sizef, 1});
    op(scale, OpFDiv, {float_type, aspect, width, height});
    op(scale, OpFOrdGreaterThanEqual, {boolean, above, aspect, low});
    op(scale, OpFOrdLessThanEqual, {boolean, below, aspect, high});
    op(scale, OpLogicalAnd, {boolean, matches, above, below});
    op(scale, OpFMul, {float_type, r, aspect, nine_sixteenths});
    op(scale, OpFDiv, {float_type, inverse, one, r});
    op(scale, OpFOrdGreaterThan, {boolean, wide, r, one});
    op(scale, OpFOrdGreaterThan, {boolean, tall, inverse, one});
    op(scale, OpSelect, {float_type, sx_any, wide, r, one});
    op(scale, OpSelect, {float_type, sy_any, tall, inverse, one});
    op(scale, OpSelect, {float_type, sx, matches, sx_any, one});
    op(scale, OpSelect, {float_type, sy, matches, sy_any, one});
    const auto scaled = [&](u32 coordinate, u32 factor) {
        const u32 centered = id(), stretched = id(), result = id();
        op(scale, OpFSub, {float_type, centered, coordinate, half});
        op(scale, OpFMul, {float_type, stretched, centered, factor});
        op(scale, OpFAdd, {float_type, result, stretched, half});
        return result;
    };
    const u32 u2 = scaled(u, sx), v2 = scaled(v, sy);
    // Black outside the frame.
    std::vector<u32> mask;
    const u32 u_low = id(), u_high = id(), v_low = id(), v_high = id(), in_u = id(), in_v = id(),
              inside = id(), shown = id();
    op(mask, OpFOrdGreaterThanEqual, {boolean, u_low, u2, zero});
    op(mask, OpFOrdLessThanEqual, {boolean, u_high, u2, one});
    op(mask, OpFOrdGreaterThanEqual, {boolean, v_low, v2, zero});
    op(mask, OpFOrdLessThanEqual, {boolean, v_high, v2, one});
    op(mask, OpLogicalAnd, {boolean, in_u, u_low, u_high});
    op(mask, OpLogicalAnd, {boolean, in_v, v_low, v_high});
    op(mask, OpLogicalAnd, {boolean, inside, in_u, in_v});
    op(mask, OpSelect, {f32vec4, shown, inside, texel, black});

    std::vector<u32> patched(code.begin(), code.begin() + HeaderWords);
    patched[3] = bound;
    if (!image_query) {
        patched.insert(patched.end(), {(2u << 16) | OpCapability, CapabilityImageQuery});
    }
    for (size_t i = 0; i < ops.size(); ++i) {
        const auto [at, opcode, words] = ops[i];
        if (i == function) {
            patched.insert(patched.end(), constants.begin(), constants.end());
        }
        if (i == first) {
            patched.insert(patched.end(), scale.begin(), scale.end());
        }
        if (i == write) {
            patched.insert(patched.end(), mask.begin(), mask.end());
        }
        const size_t start = patched.size();
        patched.insert(patched.end(), code.begin() + at, code.begin() + at + words);
        if (std::find(coordinates.begin(), coordinates.end(), i) != coordinates.end()) {
            patched[start + 3] = u2;
            patched[start + 4] = v2;
        } else if (i == write) {
            patched[start + 3] = shown;
        }
    }
    static std::once_flag logged;
    std::call_once(logged, [display] {
        std::printf("GPU: movies keep 16:9 in a display of aspect %.3f (black bars)\n", display);
        std::fflush(stdout);
    });
    return patched;
}

} // namespace Vulkan

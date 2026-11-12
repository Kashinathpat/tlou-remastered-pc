// SPDX-FileCopyrightText: Copyright 2026 gow3_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "shader_recompiler/render_size_literals.h"

namespace Shader {

namespace {

constexpr u32 Pack(u32 width, u32 height) {
    return width | height << 16;
}

constexpr u32 Default = Pack(1920, 1080);

/// The literals the game writes for a render size, as it computes them (width * 0.25,
/// height * 0.25, 4 / width, 4 / height in single precision): 480, 270, 1/480 and 1/270 at 1080p.
std::array<u32, 4> LiteralWords(u32 size) {
    const float width = static_cast<float>(size & 0xFFFF);
    const float height = static_cast<float>(size >> 16);
    return {std::bit_cast<u32>(width * 0.25f), std::bit_cast<u32>(height * 0.25f),
            std::bit_cast<u32>(4.0f / width), std::bit_cast<u32>(4.0f / height)};
}

bool Holds(std::span<const u32> words, const std::array<u32, 4>& literals) {
    return std::ranges::any_of(words, [&](u32 word) {
        return std::ranges::find(literals, word) != literals.end();
    });
}

} // namespace

u32 GameRenderSize() {
    static const u32 size = [] {
        const char* game = std::getenv("GOW3_GAME_RESOLUTION");
        unsigned width = 0, height = 0;
        if (game && std::sscanf(game, "%ux%u", &width, &height) == 2 && width >= 64 &&
            width <= 0xFFFF && height >= 64 && height <= 0xFFFF) {
            return Pack(width, height);
        }
        return Default;
    }();
    return size;
}

std::array<u32, 4> RenderSizeLiteralWords() {
    return LiteralWords(GameRenderSize());
}

u32 RenderSizeLiterals(std::span<const u32> code) {
    const u32 size = GameRenderSize();
    return Holds(code, LiteralWords(size)) ? size : 0;
}

bool HoldsOtherRenderSizeLiterals(std::span<const u32> spv) {
    const u32 size = GameRenderSize();
    if (size != Default && Holds(spv, LiteralWords(Default))) {
        return true;
    }
    // Another size's grid: a quarter width or height q (a whole number of pixels times 4) whose
    // inverse 1/q is in the binary too. Powers of two are left out (texture sizes and their
    // inverses, common in shaders; no render size has a power of two for a quarter of a side).
    const auto current = LiteralWords(size);
    std::vector<float> quarters;
    for (const u32 word : spv) {
        const float q = std::bit_cast<float>(word);
        if (!(q >= 120.0f && q <= 2048.0f) || word == current[0] || word == current[1]) {
            continue;
        }
        const float side = q * 4.0f;
        if (side != std::floor(side) || std::has_single_bit(static_cast<u32>(side))) {
            continue;
        }
        if (std::ranges::find(quarters, q) == quarters.end()) {
            quarters.push_back(q);
        }
    }
    for (const float q : quarters) {
        if (std::ranges::find(spv, std::bit_cast<u32>(1.0f / q)) != spv.end()) {
            return true;
        }
    }
    return false;
}

} // namespace Shader

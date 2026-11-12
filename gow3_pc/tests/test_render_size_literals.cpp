// SPDX-License-Identifier: GPL-2.0-or-later
// RenderSizeLiterals / HoldsOtherRenderSizeLiterals (shader_recompiler/render_size_literals.h):
// which shader code holds The Last of Us' render-size literals (the ambient shadows' quarter-size
// grid, 480, 270, 1/480 and 1/270 at 1080p, which the game rewrites for its render size), and
// which cached SPIR-V of an older build was translated for another size. Runs at 2560x1440.
#include <bit>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "shader_recompiler/render_size_literals.h"

static u32 F(float value) {
    return std::bit_cast<u32>(value);
}

int main() {
#ifdef _WIN32
    _putenv("GOW3_GAME_RESOLUTION=2560x1440");
#else
    setenv("GOW3_GAME_RESOLUTION", "2560x1440", 1);
#endif
    using namespace Shader;
    assert(GameRenderSize() == (2560u | 1440u << 16));

    // The game computes width * 0.25 and 4 / width in single precision: at 1080p that is the
    // shaders' own 480 and 1/480 (0x3b088889) and 270 and 1/270 (0x3b72b9d6).
    assert(F(1920.0f * 0.25f) == 0x43f00000u && F(4.0f / 1920.0f) == 0x3b088889u);
    assert(F(1080.0f * 0.25f) == 0x43870000u && F(4.0f / 1080.0f) == 0x3b72b9d6u);

    // Translation: code holding any literal of the current size is recorded with that size.
    const std::vector<u32> plain{0xbeeb03ffu, 0x7e000280u, F(2.0f), F(0.5f)};
    const std::vector<u32> grid_1440{0xbeeb03ffu, 0x7e0802ffu, F(640.0f), 0x7e0a02ffu,
                                     F(1.0f / 360.0f)};
    const std::vector<u32> grid_1080{0xbeeb03ffu, 0x7e0802ffu, F(480.0f)};
    assert(RenderSizeLiterals(plain) == 0);
    assert(RenderSizeLiterals(grid_1440) == (2560u | 1440u << 16));
    assert(RenderSizeLiterals(grid_1080) == 0); // the game would have written 640 there

    // Cached SPIR-V of earlier builds (no record in the meta).
    assert(!HoldsOtherRenderSizeLiterals(plain));
    assert(!HoldsOtherRenderSizeLiterals(grid_1440));
    assert(HoldsOtherRenderSizeLiterals(grid_1080)); // any 1080p literal, at another size
    const std::vector<u32> grid_4k{F(960.0f), F(540.0f), F(1.0f / 960.0f), F(1.0f / 540.0f)};
    assert(HoldsOtherRenderSizeLiterals(grid_4k)); // a q and 1/q pair of another size
    const std::vector<u32> half_height{F(262.5f), F(4.0f / 1050.0f)}; // 1680x1050
    assert(HoldsOtherRenderSizeLiterals(half_height));
    const std::vector<u32> textures{F(256.0f), F(1.0f / 256.0f), F(512.0f), F(1.0f / 512.0f)};
    assert(!HoldsOtherRenderSizeLiterals(textures)); // powers of two: texture sizes
    const std::vector<u32> lone{F(960.0f), F(100.0f)};
    assert(!HoldsOtherRenderSizeLiterals(lone)); // no inverse: not a grid

    std::printf("render size literals: ok\n");
    return 0;
}

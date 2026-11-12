// SPDX-FileCopyrightText: Copyright 2026 gow3_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 12 Nov 2026 (The Last of Us Remastered, issues #5 and #66: a light or dark outline around
// characters and vehicles, soft shadows that look detached from them).
//
// The game draws its characters' soft "ambient shadows" at a quarter of the render size and every
// lit material's pixel shader upsamples them with a depth-aware filter whose texel grid is
// written into the shader as four float literals: 480, 270, 1/480 and 1/270 for a 1920x1080
// render (about 20,000 shaders in update 1.11, none for another size). When it loads a shader
// the game rewrites those literals for its render size (eboot 1.11 0xC3BA30: every instruction
// literal equal to one of them becomes width / 4, height / 4, 4 / width or 4 / height; the PS4
// Pro's 3200x1800 needed it), so the shader binary in memory depends on the render size, and its
// hash (the one in the binary's footer) does not.
//
// The pipeline cache is named by that hash: shaders translated at one render size were loaded at
// another, and the filter then read the quarter-size textures (640x360 at 2560x1440, 960x540 at
// 3840x2160) on the grid of another size, shifting the shadows off the objects with a band along
// their edges. Every cache built before the launcher's Resolution changed the render size (11 Nov
// 2026) was a 1080p cache. Shader metas now record the render size whose literals the shader holds
// (RenderSizeLiterals) and a cached shader translated for another size compiles again.

#pragma once

#include <array>
#include <span>
#include "common/types.h"

namespace Shader {

/// The render size the game draws at, width | height << 16 (GOW3_GAME_RESOLUTION, set by
/// tools/probe_tlou.py; 1920x1080 without it).
u32 GameRenderSize();

/// The four literals as the game writes them for its render size: width / 4, height / 4,
/// 4 / width, 4 / height (single precision float words).
std::array<u32, 4> RenderSizeLiteralWords();

/// GameRenderSize() when the guest code holds one of the four literals for that size, else 0.
u32 RenderSizeLiterals(std::span<const u32> code);

/// For a cached SPIR-V binary whose meta predates the render size record: whether it holds the
/// literals of another render size (the 1080p ones, or a pair of q and 1/q that are not
/// width / 4 or height / 4 of the current size).
bool HoldsOtherRenderSizeLiterals(std::span<const u32> spv);

} // namespace Shader

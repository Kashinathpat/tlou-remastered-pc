// SPDX-License-Identifier: GPL-2.0-or-later
// WorkgroupSplitFactors (shader_recompiler/runtime_info.h): the host workgroups a split guest
// workgroup runs as, and the guest IDs the shader rebuilds from them (the same arithmetic as
// SplitLocalInvocation and the WorkgroupId case in emit_spirv_context_get_set.cpp).
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
#include "shader_recompiler/runtime_info.h"

using Size = std::array<u32, 3>;

static Shader::HwComputeRuntimeInfo Cs(Size size, bool indirect = false) {
    Shader::HwComputeRuntimeInfo cs{};
    cs.workgroup_size = size;
    cs.indirect = indirect;
    return cs;
}

static u32 Total(Size s) {
    return s[0] * s[1] * s[2];
}

// Every (guest workgroup, guest invocation) of a dispatch of `groups` guest workgroups is reached
// exactly once, every host workgroup holds consecutive guest invocation indices, and the guest
// invocation index matches x + y * X + z * X * Y.
static void CheckCover(Size size, Size groups) {
    const auto f = Shader::WorkgroupSplitFactors(true, Cs(size));
    const u32 parts = f[0] * f[1] * f[2];
    const Size host{size[0] / f[0], size[1] / f[1], size[2] / f[2]};
    assert(Total(host) <= 256);
    if (parts > 1) {
        assert(Total(host) <= Shader::WorkgroupSplitHostSize());
    }
    std::vector<u8> seen(size_t(Total(size)) * Total(groups));
    for (u32 hz = 0; hz < groups[2]; ++hz) {
        for (u32 hy = 0; hy < groups[1]; ++hy) {
            for (u32 hx = 0; hx < groups[0] * parts; ++hx) {
                const u32 part = hx % parts;
                const Size guest_group{hx / parts, hy, hz};
                u32 first_index = ~0u, last_index = 0;
                for (u32 lz = 0; lz < host[2]; ++lz) {
                    for (u32 ly = 0; ly < host[1]; ++ly) {
                        for (u32 lx = 0; lx < host[0]; ++lx) {
                            const Size local{lx, ly, lz};
                            Size guest{};
                            u32 inner = 1;
                            for (u32 d = 0; d < 3; ++d) {
                                const u32 p = (part / inner) % f[d];
                                inner *= f[d];
                                guest[d] = local[d] + p * host[d];
                            }
                            const u32 index =
                                guest[0] + (guest[1] + guest[2] * size[1]) * size[0];
                            first_index = std::min(first_index, index);
                            last_index = std::max(last_index, index);
                            const u32 group = guest_group[0] +
                                              (guest_group[1] + guest_group[2] * groups[1]) *
                                                  groups[0];
                            auto& slot = seen[size_t(group) * Total(size) + index];
                            assert(slot == 0);
                            slot = 1;
                        }
                    }
                }
                // Consecutive indices: whole waves, in the same order as before.
                assert(last_index - first_index + 1 == Total(host));
            }
        }
    }
    for (const u8 slot : seen) {
        assert(slot == 1);
    }
}

int main() {
    using Shader::WorkgroupSplitFactors;
    Shader::WorkgroupSplitOn() = true;
    assert(Shader::WorkgroupSplitHostSize() == 64);

    // The cases in The Last of Us' shader cache.
    assert((WorkgroupSplitFactors(true, Cs({1024, 1, 1})) == Size{16, 1, 1}));
    assert((WorkgroupSplitFactors(true, Cs({512, 1, 1})) == Size{8, 1, 1}));
    assert((WorkgroupSplitFactors(true, Cs({32, 32, 1})) == Size{1, 16, 1}));
    assert((WorkgroupSplitFactors(true, Cs({8, 8, 8})) == Size{1, 1, 8}));
    assert((WorkgroupSplitFactors(true, Cs({320, 1, 1})) == Size{8, 1, 1}));
    assert((WorkgroupSplitFactors(true, Cs({64, 16, 1})) == Size{1, 16, 1}));
    // Not split: small, shared data, indirect, switched off.
    assert((WorkgroupSplitFactors(true, Cs({256, 1, 1})) == Size{1, 1, 1}));
    assert((WorkgroupSplitFactors(true, Cs({16, 16, 1})) == Size{1, 1, 1}));
    assert((WorkgroupSplitFactors(false, Cs({1024, 1, 1})) == Size{1, 1, 1}));
    assert((WorkgroupSplitFactors(true, Cs({1024, 1, 1}, true)) == Size{1, 1, 1}));
    // An odd outer extent stops the split there; 3 x 100 can only leave 300 (> 256): whole.
    assert((WorkgroupSplitFactors(true, Cs({100, 3, 1})) == Size{1, 1, 1}));
    assert(Shader::WorkgroupSplit(true, Cs({1024, 1, 1})) == 16);

    for (const Size size : {Size{1024, 1, 1}, Size{512, 1, 1}, Size{32, 32, 1}, Size{8, 8, 8},
                            Size{320, 1, 1}, Size{64, 16, 1}, Size{16, 4, 8}, Size{256, 1, 1}}) {
        CheckCover(size, {3, 2, 2});
    }

    Shader::WorkgroupSplitHostSize() = 256;
    assert((WorkgroupSplitFactors(true, Cs({1024, 1, 1})) == Size{4, 1, 1}));
    CheckCover({32, 32, 1}, {2, 3, 1});
    Shader::WorkgroupSplitOn() = false;
    assert((WorkgroupSplitFactors(true, Cs({1024, 1, 1})) == Size{1, 1, 1}));

    std::puts("Workgroup split: factors, guest IDs and wave order passed");
}

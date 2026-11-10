// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

// The Last of Us Remastered: a shader-independent safety net for small GPU-written compute
// outputs, beside the end-of-pipe readbacks of the known physics results (TlouEopReadbackList
// in vk_rasterizer.cpp). From erickmendes22's Android fork (tlou-remastered-android,
// tlou_auto_readback.h, its "Full" policy), where it took the character deformation away.
//
// A binding qualifies by its descriptor and its writer, not by its shader hash: a compute
// write of 256 B to 8 KiB with a 4-64 byte stride whose V# covers the bytes copied. It is read
// back at the next end-of-pipe event like the known results, and only while fewer than half of
// the event's 256 ranges are taken, so the known results always have room. Not Precise
// readbacks (GOW3_READBACKS=2).
namespace TlouAutoReadback {
constexpr bool Eligible(bool compute, bool gpu_written, bool known_result, std::uint32_t stride,
                        std::uint64_t records, std::uint64_t bytes,
                        std::uint32_t event_slots) noexcept {
    if (!compute || !gpu_written || known_result || event_slots >= 128 || bytes < 256 ||
        bytes > 8192 || stride < 4 || stride > 64 || (stride & 3) != 0 || records == 0) {
        return false;
    }
    // The V# must cover the bytes copied (a format's element size may be past its stride).
    return records <= UINT64_MAX / stride && records * stride >= bytes;
}
} // namespace TlouAutoReadback

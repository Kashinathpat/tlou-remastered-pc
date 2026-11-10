// SPDX-License-Identifier: GPL-2.0-or-later
// TlouAutoReadback::Eligible (GOW3_AUTO_EOP_READBACK=1): which compute outputs join the
// end-of-pipe readbacks whatever their shader. Adapted from the Android fork's test.
#include "video_core/renderer_vulkan/tlou_auto_readback.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

using TlouAutoReadback::Eligible;

// The physics result sizes seen on PC and Android (4272/6480), and 4368 seen on Android.
static_assert(Eligible(true, true, false, 4, 1068, 4272, 0));
static_assert(Eligible(true, true, false, 4, 1092, 4368, 0));
static_assert(Eligible(true, true, false, 4, 1620, 6480, 1));
static_assert(Eligible(true, true, false, 48, 89, 4272, 0)); // the PC's 48-byte records
static_assert(Eligible(true, true, false, 16, 256, 4096, 127));
static_assert(!Eligible(true, true, false, 4, 1620, 6480, 128)); // half the event's ranges taken
static_assert(!Eligible(false, true, false, 4, 1620, 6480, 0));  // graphics writes
static_assert(!Eligible(true, false, false, 4, 1620, 6480, 0));  // read-only
static_assert(!Eligible(true, true, true, 4, 1620, 6480, 0));    // a known result has its own path
static_assert(!Eligible(true, true, false, 4, 100, 255, 0));     // too small
static_assert(!Eligible(true, true, false, 4, 3000, 12000, 0));  // too large
static_assert(!Eligible(true, true, false, 3, 1068, 3204, 0));   // stride not a dword multiple
static_assert(!Eligible(true, true, false, 64, 1, 8192, 0));     // V# shorter than the copy
static_assert(!Eligible(true, true, false, 4, 0, 8192, 0));      // no records

int main() {
    for (unsigned stride = 1; stride <= 72; ++stride) {
        for (unsigned bytes = 0; bytes <= 9000; bytes += 16) {
            for (unsigned slots : {0u, 127u, 128u, 255u}) {
                const bool expect = slots < 128 && stride >= 4 && stride <= 64 &&
                                    stride % 4 == 0 && bytes >= 256 && bytes <= 8192;
                const unsigned records = (bytes + stride - 1) / stride;
                assert(Eligible(true, true, false, stride, records, bytes, slots) == expect);
                // A V# one record short of the copy never qualifies.
                if (records) {
                    assert(!Eligible(true, true, false, stride, records - 1,
                                     std::uint64_t(records) * stride, slots));
                }
            }
        }
    }
    std::puts("tlou-auto-readback-test: ok");
}

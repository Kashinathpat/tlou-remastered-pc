/* gow3 12 Nov 2026 diagnostics (GOW3_RAYCAST_DIAG=1, The Last of Us Remastered 1.11): what the
 * game's mesh ray caster (shared/src/ndlib/render/ngen/meshraycaster.cpp) reads when a batch of
 * GPU ray casts is reported done. A hook at the entry of its HandleResults (eboot 0xb5b1e0, the
 * MeshRayCasterImplGpuResultsReady job, (manager, batch)) counts the rays and the hits it sees
 * (t > 0, the game's own test) by requester (the ray's callback; 0x6b8390 = a bullet's impact
 * effect, projectile-base.cpp) and whether each hit lies on its ray: a hit copied from another
 * batch (results read before the GPU wrote them) does not.
 *
 * Batch layout (from HandleResults and the batch setup at 0xb5c3b4):
 *   +0x10                ray count (at most 16)
 *   +0x20 + i * 0xd0     request: start xyz w, end xyzw, direction (normalised) xyzw, ...,
 *                        +0x38 callback, +0x40 user data
 *   +0xdd0 + i * 0x40    result (Cs_RayTraceGatherHits record, copied by Cs_MemCpy): +0x00 ray,
 *                        +0x04 level, +0x08 object, +0x0c sub-mesh, +0x10 triangle, +0x14 t,
 *                        +0x18 u v, +0x28 position xyz, +0x34 normal xyz */
#include "runtime_mouselook.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int runtime_raycast_diag_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("GOW3_RAYCAST_DIAG");
        enabled = env && env[0] == '1';
    }
    return enabled;
}

enum { KINDS = 8 };
static struct { uint64_t callback; long long rays, hits, off_ray; } kinds[KINDS];
static long long batches, described;
static uint64_t last_report_ms;
static volatile long lock;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

__attribute__((sysv_abi)) void *runtime_raycast_diag_results(void *rdi, void *rsi, void *rdx) {
    (void)rdi;
    (void)rdx;
    const unsigned char *batch = rsi;
    if (!batch) return NULL;
    int32_t n;
    memcpy(&n, batch + 0x10, 4);
    if (n <= 0 || n > 16) return NULL;
    const uint64_t base = (uint64_t)(uintptr_t)runtime_mouselook_image();
    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {
    }
    ++batches;
    for (int i = 0; i < n; ++i) {
        float start[4], dir[4], pos[3], t;
        uint64_t callback;
        memcpy(start, batch + 0x20 + i * 0xd0, 16);
        memcpy(dir, batch + 0x40 + i * 0xd0, 16);
        memcpy(&callback, batch + 0x58 + i * 0xd0, 8);
        callback -= base;
        const unsigned char *rec = batch + 0xdd0 + i * 0x40;
        memcpy(&t, rec + 0x14, 4);
        memcpy(pos, rec + 0x28, 12);
        const int hit = t > 0.0f;
        float err = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float d = pos[k] - (start[k] + t * dir[k]);
            err += d * d;
        }
        err = sqrtf(err);
        const int good = !hit || err < 0.05f + 0.01f * t;
        int slot = 0;
        while (slot < KINDS - 1 && kinds[slot].callback && kinds[slot].callback != callback) ++slot;
        kinds[slot].callback = callback;
        ++kinds[slot].rays;
        kinds[slot].hits += hit;
        kinds[slot].off_ray += !good;
        /* Rays that are not a character's foot probe (straight down), and every miss or
         * misplaced hit: in detail, up to 300 lines. */
        if ((fabsf(dir[1]) < 0.95f || !hit || !good) && described < 300) {
            ++described;
            int32_t ids[5];
            memcpy(ids, rec, sizeof(ids));
            printf("Ray-cast diag: callback %#llx batch %p ray %d/%d %s t %.3f (off by %.3f) level %d object %d "
                   "sub-mesh %d triangle %d; start %.2f %.2f %.2f dir %.2f %.2f %.2f length %.2f pos %.2f %.2f "
                   "%.2f\n",
                   (unsigned long long)callback, (const void *)batch, i, n,
                   !hit ? "MISS" : good ? "hit" : "hit NOT on the ray", t, err, ids[1], ids[2], ids[3], ids[4],
                   start[0], start[1], start[2], dir[0], dir[1], dir[2], dir[3], pos[0], pos[1], pos[2]);
        }
    }
    const uint64_t now = now_ms();
    if (now - last_report_ms >= 5000) {
        last_report_ms = now;
        printf("Ray-cast diag (5 s, CPU side): %lld batches; by callback (rays/hits/hits off their ray):", batches);
        for (int k = 0; k < KINDS && kinds[k].callback; ++k)
            printf(" %#llx %lld/%lld/%lld", (unsigned long long)kinds[k].callback, kinds[k].rays, kinds[k].hits,
                   kinds[k].off_ray);
        printf("\n");
        memset(kinds, 0, sizeof(kinds));
        batches = 0;
    }
    fflush(stdout);
    __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
    return NULL;
}

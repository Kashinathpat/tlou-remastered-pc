/* Mouse look handlers (see runtime_mouselook.h): hooks in the game's camera input functions that
 * turn the camera by the mouse movement since the last frame, on the game's thread, once per
 * frame. A site's handler runs in the game's job fibers (64 KiB stacks): keep its frame small.
 *
 * The Last of Us Remastered 1.11 (10 Nov 2026, found by following the right stick from the pad
 * conversion 0xa4d360 with data watchpoints, then the camera manager's virtual calls with
 * instruction breakpoints; the same scheme as the Uncharted ports' camera hooks):
 *  - Third-person camera: the player camera's input step 0x297440(camera, stick), called from its
 *    update 0x2961a0 with the stick block (right stick at +8/+0xc, the raw one at +0x18/+0x1c).
 *    Yaw: 0x298bc0(camera + 0x1d0, ...) turns the wanted look direction at camera + 0x1e0 (sin h,
 *    0, cos h, 0; h in degrees, the stick's positive x lowers it; the shown direction at +0x1d0
 *    eases towards it) by speed * dt * stick x * scale,
 *    with a speed ramp (+0x200 now, up to the tuning's +0x4dc and the cap +0x208) and the scale at
 *    +0x510. Pitch: a 0..1 position on the camera's vertical curve at +0x380 (copy at +0x340),
 *    stepped by the game's 0x754610(camera + 0x220, pitch, delta) with delta = smoothed stick y *
 *    +0x4f0 * dt * +0x514. +0x390 holds the frame of the last stick input (the camera recentres
 *    itself some seconds after it; +0x370 is the recentring flag the stick clears).
 *  - Aim (strafe) camera: its controller's update 0x56b6e0(out, controller, stick, reference,
 *    flag, direction, dt) builds the new aim direction from the current one (*direction, x y z w)
 *    and the stick (0x56d630, curve, aim speeds at +0x138/+0x13c). The mouse turns *direction
 *    before the controller reads it.
 * The game's frame counter (clock at *(image + 0x156a480): +0x1c0 frame, +0x1c8 frame time) lets
 * both cameras turn by the same mouse movement in a frame (the hidden one too, so neither jumps
 * when the game switches between them), each camera object once per frame.
 * GOW3_MOUSE_LOOK_DEBUG=1 logs, once a second, the cameras' headings and how much of the asked
 * turn each kept from one call to the next. */
#include "runtime_mouselook.h"
#include "runtime.h"
#include "gpu/gow3gpu.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned char *image;

void runtime_mouselook_set_image(unsigned char *base) { image = base; }
unsigned char *runtime_mouselook_image(void) { return image; }

int runtime_mouselook_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        /* Also with the mouse off: the hooks give the controller camera speed too. Without a
         * captured mouse (off, or no focus) they take no mouse movement. */
        const char *look = getenv("GOW3_MOUSE_LOOK");
        enabled = !(look && look[0] == '0');
    }
    return enabled;
}

static int look_debug(void) {
    static int debug = -1;
    if (debug < 0) {
        const char *env = getenv("GOW3_MOUSE_LOOK_DEBUG");
        debug = env && env[0] == '1';
    }
    return debug;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Radians per mouse count at sensitivity 1 (the settings' mouse sensitivity scales it, live;
 * 0.0011 rad is about 0.063 degrees: a 2,000-count swipe turns about 126 degrees). */
static float look_scale(int aiming) {
    const float s = gow3gpu_controls_mouse_sens();
    const float aim = aiming ? gow3gpu_controls_mouse_aim_sens() : 1.0f;
    return 0.0011f * (s > 0.0f ? s : 1.0f) * (aim > 0.0f ? aim : 1.0f);
}
static float look_invert_x(void) { return gow3gpu_controls_mouse_invert_x() ? -1.0f : 1.0f; }
/* Up/down: the invert switch and the vertical speed (settings, Controls tab). */
static float look_vertical(void) {
    const float v = gow3gpu_controls_mouse_vertical();
    return (gow3gpu_controls_mouse_invert() ? -1.0f : 1.0f) * (v > 0.0f ? v : 1.0f);
}

/* ---- The Last of Us Remastered 1.11 ---------------------------------------------------------- */
enum {
    TLOU_CLOCK = 0x156a480,       /* pointer to the game clock */
    TLOU_PLAYER_OF = 0x49c630,    /* player object for a camera's player id (camera + 0xf0), or NULL */
    TLOU_PITCH_STEP = 0x754610,   /* float step(curve, pitch, delta) */
};
enum {
    TLOU_PITCH_DEGREES_PER_STEP = 57, /* camera pitch per unit of the step's delta (half the curve) */
};
typedef ABI void *(*TlouPlayerOf)(uint32_t id);
typedef ABI float (*TlouPitchStep)(void *curve, float pitch, float delta);

static uint32_t tlou_frame(void) {
    const unsigned char *clock = *(unsigned char *const *)(image + TLOU_CLOCK);
    return clock ? *(const uint32_t *)(clock + 0x1c0) : 0;
}

/* One mouse movement per game frame, shared by the cameras that run in it; each camera object
 * turns by it at most once per frame (a frame that does not advance, as in a pause, turns
 * nothing again). The cameras may run on different job threads: a spin lock guards this. */
typedef struct {
    const void *object;
    uint32_t frame;
} Applied;
static volatile int frame_lock;
static uint32_t taken_frame;
static int taken_any, taken_valid;
static float taken_dx, taken_dy;
static Applied applied[8];
static unsigned applied_next;

static void lock(void) {
    while (__atomic_exchange_n(&frame_lock, 1, __ATOMIC_ACQUIRE))
        __builtin_ia32_pause();
}
static void unlock(void) { __atomic_store_n(&frame_lock, 0, __ATOMIC_RELEASE); }

/* The mouse movement for this frame for the camera object; 0 when it already turned this frame,
 * or the mouse is not captured or belongs to the stick now (runtime_pad.c). */
static int frame_look(const void *object, uint32_t frame, float *dx, float *dy) {
    lock();
    if (!taken_any || frame != taken_frame) {
        taken_valid = gow3gpu_mouse_take_look(&taken_dx, &taken_dy);
        taken_frame = frame;
        taken_any = 1;
    }
    int first = 1;
    for (unsigned i = 0; i < sizeof(applied) / sizeof(applied[0]); ++i) {
        if (applied[i].object == object) {
            first = applied[i].frame != frame;
            applied[i].frame = frame;
            goto found;
        }
    }
    applied[applied_next].object = object;
    applied[applied_next].frame = frame;
    applied_next = (applied_next + 1) % (sizeof(applied) / sizeof(applied[0]));
found:
    *dx = taken_dx;
    *dy = taken_dy;
    const int valid = taken_valid && first;
    unlock();
    return valid;
}

/* Yaw about +Y by angle (radians; positive raises the heading atan2(x, z), as a negative stick x
 * does). */
static void turn_xz(float *v, float angle) {
    const float c = cosf(angle), s = sinf(angle);
    const float x = v[0], z = v[2];
    v[0] = x * c + z * s;
    v[2] = z * c - x * s;
}
static float heading_of(const float *v) { return atan2f(v[0], v[2]) * 57.29578f; }
static float wrap_degrees(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

/* Debug: how much of the last turn a camera still had at its next call (1 = all kept; the stick
 * turns in between count too). */
typedef struct {
    const char *name;
    uint64_t last;
    unsigned calls, turns;
    float expected, before_next, kept_sum;
    int pending;
} TurnDebug;
static void turn_debug(TurnDebug *d, float heading_now, float asked_degrees, const char *extra) {
    if (d->pending) {
        /* heading_now includes this call's turn: take it out */
        const float got = wrap_degrees(heading_now - asked_degrees - d->before_next);
        if (fabsf(d->expected) > 0.05f) {
            d->kept_sum += got / d->expected;
            ++d->turns;
        }
        d->pending = 0;
    }
    ++d->calls;
    const uint64_t now = now_ms();
    if (now - d->last >= 1000) {
        printf("Mouse look debug: %s heading %.1f, %u calls, %u turns, kept %.2f%s\n", d->name, heading_now,
               d->calls, d->turns, d->turns ? d->kept_sum / (float)d->turns : 0.0f, extra ? extra : "");
        fflush(stdout);
        d->last = now;
        d->calls = d->turns = 0;
        d->kept_sum = 0.0f;
    }
    if (asked_degrees != 0.0f) {
        /* heading_now is after the turn: the next call compares with the heading before it */
        d->expected = asked_degrees;
        d->before_next = wrap_degrees(heading_now - asked_degrees);
        d->pending = 1;
    }
}

/* Third-person camera input 0x297440(camera, stick). */
static TurnDebug follow_debug = {.name = "camera"};
static volatile uint64_t camera_called_ms, aim_called_ms;
static ABI void *tlou_camera_input(unsigned char *camera, const float *stick, void *unused) {
    (void)stick;
    (void)unused;
    if (!camera || !image) return NULL;
    float *direction = (float *)(camera + 0x1e0);
    if (!((TlouPlayerOf)(image + TLOU_PLAYER_OF))(*(const uint32_t *)(camera + 0xf0))) return NULL;
    /* with its yaw locked the camera does not take the mouse (the stick path does the same) */
    if (!camera[0x1d0 + 0x28]) __atomic_store_n(&camera_called_ms, now_ms(), __ATOMIC_RELAXED);
    const uint32_t frame = tlou_frame();
    float dx = 0, dy = 0;
    const int took = frame_look(camera, frame, &dx, &dy);
    float asked = 0.0f;
    if (took && (dx != 0.0f || dy != 0.0f)) {
        const float per_count = look_scale(0);
        /* the yaw lock the stick path respects (scripted moments) */
        if (dx != 0.0f && !camera[0x1d0 + 0x28]) {
            const float angle = -dx * per_count * look_invert_x();
            turn_xz(direction, angle);
            /* 13 Nov 2026 (GitHub #138, #150: the camera kept moving after the mouse stopped):
             * the yaw controller holds the shown direction at +0x00 (camera + 0x1d0) and eases it
             * towards the wanted one at +0x10 (+0x1e0), each frame closing about a third of the
             * gap; a fast mouse left the shown camera 25-45 degrees behind (traces in Pittsburgh).
             * The mouse turns both, so the camera follows it at once; the game's own camera moves
             * keep their easing. */
            turn_xz((float *)(camera + 0x1d0), angle);
            asked = angle * 57.29578f;
        }
        if (dy != 0.0f) {
            /* 12 Nov 2026 (GitHub #123, looking up and down was slow): the step's delta is in
             * half-ranges of the vertical curve (it works on 2 * (pitch - 0.5)), and the curve
             * spans about 114 degrees (measured in Pittsburgh: +52.5 at 0, -61.3 at 1, steady
             * 12 degrees/s for 18.9 asked through the old stick-rate ratio 2.7 / 240). So one
             * delta unit is about 57 degrees: the mouse now pitches as many degrees per count as
             * it turns. */
            const float delta = dy * per_count * 57.29578f * look_vertical() / TLOU_PITCH_DEGREES_PER_STEP;
            float *pitch = (float *)(camera + 0x380);
            const float next = ((TlouPitchStep)(image + TLOU_PITCH_STEP))(camera + 0x220, *pitch, delta);
            if (isfinite(next)) {
                *pitch = next;
                *(float *)(camera + 0x340) = next;
            }
        }
        /* as the stick does: no recentring for a while after input */
        *(uint32_t *)(camera + 0x390) = frame;
        camera[0x370] = 0;
    }
    if (look_debug()) {
        /* the vertical curve position and the camera's pitch from its last locator (+0x150, a
         * rotation quaternion x y z w; forward is +z) */
        const float *q = (const float *)(camera + 0x150);
        const float forward_y = 2.0f * (q[1] * q[2] - q[3] * q[0]);
        char extra[96];
        snprintf(extra, sizeof(extra), ", curve %.3f, pitch %.1f", *(const float *)(camera + 0x380),
                 asinf(forward_y < -1.0f ? -1.0f : forward_y > 1.0f ? 1.0f : forward_y) * 57.29578f);
        turn_debug(&follow_debug, heading_of(direction), asked, extra);
        if (getenv("GOW3_MOUSE_LOOK_TRACE")) {
            /* every call: the turned direction's heading against the shown camera's (its last
             * locator), to see how far the shown camera lags */
            const float fx = 2.0f * (q[0] * q[2] + q[3] * q[1]), fz = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
            const float shown = atan2f(fx, fz) * 57.29578f, target = heading_of(direction);
            printf("Mouse look trace: frame %u dx %.1f dy %.1f target %.2f shown %.2f curve %.4f pitch %.2f\n", frame,
                   dx, dy, target, shown, *(const float *)(camera + 0x380),
                   asinf(forward_y < -1.0f ? -1.0f : forward_y > 1.0f ? 1.0f : forward_y) * 57.29578f);
            /* while the two differ, the unit xz vectors in the camera whose heading is the shown
             * one (or opposite): the smoothed direction */
            const float lag = wrap_degrees(target + 180.0f - shown);
            if (fabsf(lag) > 8.0f) {
                char found[512] = "";
                size_t n = 0;
                for (unsigned off = 0; off + 12 <= 0x2000 && n < sizeof(found) - 32; off += 4) {
                    const float *v = (const float *)(camera + off);
                    const float len = v[0] * v[0] + v[2] * v[2];
                    if (!(fabsf(len - 1.0f) < 0.02f) || !(fabsf(v[1]) < 0.3f)) continue;
                    const float h = heading_of(v);
                    if (fabsf(wrap_degrees(h - shown)) < 1.5f || fabsf(wrap_degrees(h - shown - 180.0f)) < 1.5f)
                        n += (size_t)snprintf(found + n, sizeof(found) - n, " %#x(%.1f)", off, h);
                }
                printf("Mouse look trace: lag %.1f, shown-heading vectors:%s\n", lag, found);
            }
        }
    }
    return NULL;
}

/* Aim camera controller 0x56b6e0(out, controller, stick, direction, flag, up): the mouse turns
 * the current direction the controller starts from (11 Nov 2026: the sixth argument is the up
 * vector, 0 1 0; the controller measures the pitch between the two). It runs every frame, also
 * while not aiming. Pitch stays within 80 degrees. */
static TurnDebug aim_debug = {.name = "aim"};
static ABI void *tlou_aim_input(void *out, unsigned char *controller, void *stick, float *direction, uint64_t flag,
                                const float *up) {
    (void)out;
    (void)stick;
    (void)flag;
    (void)up;
    if (!controller || !direction || !image) return NULL;
    __atomic_store_n(&aim_called_ms, now_ms(), __ATOMIC_RELAXED);
    float dx = 0, dy = 0;
    const int took = frame_look(controller, tlou_frame(), &dx, &dy);
    float asked = 0.0f;
    char extra[64] = "";
    if (took && (dx != 0.0f || dy != 0.0f) && isfinite(direction[0]) && isfinite(direction[1]) &&
        isfinite(direction[2])) {
        const float per_count = look_scale(1);
        if (dx != 0.0f) {
            const float angle = -dx * per_count * look_invert_x();
            turn_xz(direction, angle);
            asked = angle * 57.29578f;
        }
        if (dy != 0.0f) {
            const float x = direction[0], y = direction[1], z = direction[2];
            const float flat = sqrtf(x * x + z * z), length = sqrtf(x * x + y * y + z * z);
            if (flat > 1e-4f && length > 1e-4f) {
                float elevation = atan2f(y, flat) - dy * per_count * look_vertical();
                const float limit = 80.0f * 0.017453292f;
                elevation = elevation > limit ? limit : elevation < -limit ? -limit : elevation;
                const float scale = length * cosf(elevation) / flat;
                direction[0] = x * scale;
                direction[2] = z * scale;
                direction[1] = length * sinf(elevation);
            }
        }
    }
    if (look_debug()) {
        snprintf(extra, sizeof(extra), ", elevation %.1f",
                 atan2f(direction[1], sqrtf(direction[0] * direction[0] + direction[2] * direction[2])) * 57.29578f);
        turn_debug(&aim_debug, heading_of(direction), asked, extra);
    }
    return NULL;
}

/* A camera hook ran in the last 60 ms (a frame at 20 FPS): the mouse turns the camera directly;
 * otherwise (cutscenes, cameras without a hook) it is the right stick. 12 Nov 2026 (GitHub #123,
 * a pushed crate's camera did not turn with the mouse): the aim controller is updated every frame,
 * aiming or not, so it counts only while the aim button is held; the third-person camera's input
 * step runs only while that camera is in use. */
int runtime_mouselook_camera_recent(void) {
    const uint64_t now = now_ms();
    return now - __atomic_load_n(&camera_called_ms, __ATOMIC_RELAXED) < 60 ||
           (runtime_pad_aim_held() && now - __atomic_load_n(&aim_called_ms, __ATOMIC_RELAXED) < 60);
}

int runtime_mouselook_aim_recent(void) {
    return now_ms() - __atomic_load_n(&aim_called_ms, __ATOMIC_RELAXED) < 200;
}

static const MouseLookSite sites[] = {
    {"The Last of Us Remastered (camera)",
     0x297440,
     6, /* push rbp; mov rbp, rsp; push r15 */
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
      0x08, 0x01, 0x00, 0x00, 0x49, 0x89, 0xfd, 0x49},
     24,
     (void *)tlou_camera_input},
    {"The Last of Us Remastered (aim camera)",
     0x56b6e0,
     6,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
      0x48, 0x03, 0x00, 0x00, 0x49, 0x89, 0xf4, 0x48},
     24,
     (void *)tlou_aim_input},
    /* Diagnostics only (GOW3_RAYCAST_DIAG=1, runtime_raycast_diag.c). */
    {"The Last of Us Remastered (ray-cast results, diagnostics)",
     0xb5b1e0,
     6,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xe4,
      0xe0, 0x48, 0x81, 0xec, 0xc0, 0x00, 0x00, 0x00},
     24,
     (void *)runtime_raycast_diag_results},
    /* The interface's quad builder (runtime_hud.c). */
    {"The Last of Us Remastered (interface quads)",
     0xaf8d40,
     6,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
      0x48, 0x49, 0x89, 0xfe, 0x48, 0x8d, 0x3d, 0x15},
     24,
     (void *)runtime_hud_quad},
};
enum { CAMERA_SITES = 2, RAYCAST_SITE = 2, HUD_SITE = 3, SITE_COUNT = sizeof(sites) / sizeof(sites[0]) };

const MouseLookSite *runtime_mouselook_sites(unsigned *count) {
    static MouseLookSite chosen[SITE_COUNT];
    const char *env = getenv("GOW3_MOUSE_LOOK_SITES"); /* debugging: a bit mask of the sites */
    const unsigned long mask = env && *env ? strtoul(env, NULL, 0) : ~0ul;
    unsigned n = 0;
    for (unsigned i = 0; i < SITE_COUNT; ++i) {
        if (i == RAYCAST_SITE && !runtime_raycast_diag_enabled()) continue;
        if (i == HUD_SITE && !runtime_hud_enabled()) continue;
        if (i < CAMERA_SITES && !runtime_mouselook_enabled()) continue;
        if (mask & (1ul << i)) chosen[n++] = sites[i];
    }
    *count = n;
    return chosen;
}

/* The right stick's x and y clamps, each followed by its store (vmovss [rdi + 0x18 / 0x1c], xmm4). */
static const StickBoostSite boost_sites[] = {
    {"The Last of Us Remastered (right stick x)", 0xa4d48d,
     {0xc4, 0xc1, 0x58, 0x5d, 0xe0, 0xc5, 0xd8, 0x5f, 0xe0, 0xc5, 0xfa, 0x11, 0x67, 0x18}},
    {"The Last of Us Remastered (right stick y)", 0xa4d4d6,
     {0xc4, 0xc1, 0x58, 0x5d, 0xe0, 0xc5, 0xd8, 0x5f, 0xe0, 0xc5, 0xfa, 0x11, 0x67, 0x1c}},
};
static float *boost_slot;

const StickBoostSite *runtime_mouselook_boost_sites(unsigned *count) {
    const char *env = getenv("GOW3_STICK_BOOST");
    *count = env && env[0] == '0' ? 0 : sizeof(boost_sites) / sizeof(boost_sites[0]);
    return boost_sites;
}

void runtime_mouselook_set_boost_slot(float *slot) { __atomic_store_n(&boost_slot, slot, __ATOMIC_RELEASE); }

void runtime_mouselook_set_stick_boost(float boost) {
    float *slot = __atomic_load_n(&boost_slot, __ATOMIC_ACQUIRE);
    if (slot) __atomic_store(slot, &boost, __ATOMIC_RELAXED);
}

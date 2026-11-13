/* Mouse look: the mouse turns the game's camera directly, like a PC game (no virtual stick, no
 * stick acceleration or turn-speed cap); the controller's camera speed setting scales the stick
 * the camera reads. The loader (probe.c, install_mouse_look) puts a jump at the entry of each
 * game's camera input function; the stub saves the argument registers, calls the site's handler
 * with the function's first three arguments, restores them (the third replaced by the handler's
 * result when that is not NULL), replays the replaced bytes and jumps back. Sites only install
 * when the original bytes match (that game and version). */
#pragma once
#include <stdint.h>

typedef struct {
    const char *game;
    uint64_t offset;                /* image offset of the camera input function's entry */
    unsigned length;                /* bytes replaced: whole instructions, >= 5, replayed */
    unsigned char expected[24];     /* original bytes at the entry */
    unsigned expected_length;
    void *handler;                  /* ABI void *handler(void *rdi, void *rsi, void *rdx): NULL, or
                                       the third argument the function gets instead */
} MouseLookSite;

/* GOW3_MOUSE_LOOK=0 turns the hooks off: the mouse then acts as the right stick (and the
 * controller camera speed does nothing). */
int runtime_mouselook_enabled(void);
const MouseLookSite *runtime_mouselook_sites(unsigned *count);
void runtime_mouselook_set_image(unsigned char *image);
unsigned char *runtime_mouselook_image(void);
/* Right stick boost (3 Nov 2026, The Last of Us Remastered 1.11): the game turns the camera no
 * faster than the right stick at full deflection, so a mouse moved fast hit that cap whatever its
 * sensitivity. The pad conversion (0xa4d360: byte / 127 - 1, dead zone 0.08, / 0.84) clamps each
 * right stick axis to -1..1 (vminps, vmaxps) at these sites; the loader (probe.c,
 * install_stick_boost) moves each clamp into a stub that then multiplies the axis by the boost
 * runtime_pad.c sets per pad read: 1 for controllers and a still mouse, more when the mouse asks
 * for more than the full stick. GOW3_STICK_BOOST=0 leaves the game's clamp alone. */
typedef struct {
    const char *game;
    uint64_t offset;               /* image offset of the clamp: vminps xmm4, xmm8; vmaxps xmm4, xmm0 */
    unsigned char expected[14];    /* the clamp (9 bytes, replayed) and the axis's store after it */
} StickBoostSite;
const StickBoostSite *runtime_mouselook_boost_sites(unsigned *count);
void runtime_mouselook_set_boost_slot(float *slot);
void runtime_mouselook_set_stick_boost(float boost);
/* A camera hook ran in the last 60 ms: the mouse turns the camera, not the stick. */
int runtime_mouselook_camera_recent(void);
/* The aim camera's hook ran in the last 200 ms: the mouse turns the aim directly too. */
int runtime_mouselook_aim_recent(void);
/* runtime_pad.c: the aim button (L2) was held at the last pad read. */
int runtime_pad_aim_held(void);
/* runtime_raycast_diag.c (GOW3_RAYCAST_DIAG=1): a site at the mesh ray caster's HandleResults. */
int runtime_raycast_diag_enabled(void);
__attribute__((sysv_abi)) void *runtime_raycast_diag_results(void *rdi, void *rsi, void *rdx);
/* runtime_hud.c: a site at the interface's quad builder (another aspect ratio than 16:9, or
 * GOW3_HUD_DIAG=1). */
int runtime_hud_enabled(void);
__attribute__((sysv_abi)) void *runtime_hud_quad(void *rdi, void *rsi, void *rdx);

/* The Last of Us Remastered 1.11 on a display wider than 16:9: full-screen interface layers.
 *
 * The interface is laid out on a 1280x720 canvas. tools/probe_tlou.py's aspect ratio patches map
 * that canvas to the middle 16:9 of the screen (the quad builder's x scale 2/1280R and offset
 * -1/R), so the menus and the HUD keep their shape. Layers meant to cover the whole screen then
 * stopped at the 16:9 edges (GitHub #139):
 *  - quads over the whole canvas (0..1280 x 0..720): the menus' and pause screen's dimming
 *    (black, alpha 0.85), the fades, the damage vignette;
 *  - the rifle scope's black frame: four black quads from past the canvas' sides (-20..280 and
 *    1000..1300, each half the height) to the circle. The scope's glass (96..1184, the zoomed view
 *    with its dirt) is left as it is.
 * The game's quad builder 0xaf8d40(context, vertices, xmm0) turns one quad (4 vertices of 0x40
 * bytes: +0 x, +4 y in canvas units, +0xc +0x10 texture coordinates, +0x20 colour) into screen
 * space. A hook at its entry (runtime_mouselook.c's site table) sends, for each such quad, one more
 * quad through it first on each side it reaches: a bar from the screen's edge to the quad's edge
 * that repeats the quad's edge column of texels. The quad itself stays as it was, so its texture
 * keeps its shape.
 *
 * GOW3_DISPLAY_ASPECT=<w>x<h> (set with the aspect ratio patches) gives R; GOW3_HUD_BARS=0 turns
 * the bars off. GOW3_HUD_DIAG=1 logs the quads over the whole canvas, and every few seconds all
 * quads of a 40 ms window (=2: every half second), with the caller (0 when the hook's frame is not
 * as expected). */
#include "runtime_mouselook.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QUAD_BUILDER 0xaf8d40
#define VERTEX 0x40

typedef __attribute__((sysv_abi)) void (*QuadBuilder)(void *context, void *vertices, float xmm0);

static uint64_t hud_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int hud_diag(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("GOW3_HUD_DIAG");
        enabled = env && env[0] >= '1' && env[0] <= '9' ? env[0] - '0' : 0;
    }
    return enabled;
}

/* The display's width over 16:9's, or 0 when the bars are not needed. */
static float hud_ratio(void) {
    static float ratio = -1.0f;
    if (ratio < 0.0f) {
        float found = 0.0f;
        const char *env = getenv("GOW3_DISPLAY_ASPECT"), *bars = getenv("GOW3_HUD_BARS");
        int w = 0, h = 0;
        if (env && sscanf(env, "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && !(bars && bars[0] == '0')) {
            const float r = (float)w / (float)h / (16.0f / 9.0f);
            if (r > 1.003f) found = r;
        }
        if (found > 0.0f) {
            printf("Interface: full-screen layers reach the screen's edges (display %.3f times as wide as 16:9)\n",
                   found);
            fflush(stdout);
        }
        ratio = found;
    }
    return ratio;
}

int runtime_hud_enabled(void) { return hud_diag() || hud_ratio() > 0.0f; }

static volatile long hud_lock;
static uint64_t window_start, window_end, last_wide_ms;
static unsigned wide_logged;

static void log_quad(const unsigned char *v, int wide, float x0, float x1, float y0, float y1, uint64_t caller) {
    const uint64_t now = hud_now_ms();
    while (__atomic_exchange_n(&hud_lock, 1, __ATOMIC_ACQUIRE)) {
    }
    if (now >= window_end + (hud_diag() >= 2 ? 500 : 4000)) {
        window_start = now;
        window_end = now + 40;
        printf("HUD diag: quads of the next 40 ms\n");
    }
    const int in_window = now >= window_start && now < window_end;
    if (in_window || (wide && (now - last_wide_ms >= 1000 || wide_logged < 4))) {
        const float *a = (const float *)v;
        const float *c = (const float *)(v + 2 * VERTEX);
        printf("HUD diag:%s caller %#llx x %.1f..%.1f y %.1f..%.1f uv %.3f,%.3f..%.3f,%.3f colour %.2f %.2f %.2f "
               "%.2f\n",
               wide ? " wide" : "", (unsigned long long)caller, x0, x1, y0, y1, a[3], a[4], c[3], c[4], a[8], a[9],
               a[10], a[11]);
        if (wide) {
            if (now - last_wide_ms >= 1000) wide_logged = 0;
            last_wide_ms = now;
            ++wide_logged;
        }
        fflush(stdout);
    }
    __atomic_store_n(&hud_lock, 0, __ATOMIC_RELEASE);
}

/* One bar: the quad's vertices on its `edge` side keep their texture coordinates, nudged 2% of
 * the quad's texture width inwards (away from a texture's border texels and from the far side a
 * repeating sampler would blend in). The bar spans from `outer` (the screen's edge) to the quad's
 * edge. */
static int build_bar(unsigned char *bar, const unsigned char *quad, float edge, float outer) {
    float u_min = INFINITY, u_max = -INFINITY;
    for (int i = 0; i < 4; ++i) {
        const float u = ((const float *)(quad + i * VERTEX))[3];
        u_min = fminf(u_min, u);
        u_max = fmaxf(u_max, u);
    }
    const float inward = (u_max - u_min) * 0.02f;
    for (int i = 0; i < 4; ++i) {
        const float *v = (const float *)(quad + i * VERTEX);
        /* the vertex on the edge side at this vertex's height */
        int source = -1;
        for (int j = 0; j < 4; ++j) {
            const float *w = (const float *)(quad + j * VERTEX);
            if (w[0] == edge && w[1] == v[1]) source = j;
        }
        if (source < 0) return 0; /* not an axis-aligned rectangle */
        memcpy(bar + i * VERTEX, quad + source * VERTEX, VERTEX);
        float *b = (float *)(bar + i * VERTEX);
        b[0] = v[0] == edge ? edge : outer;
        b[3] += b[3] <= u_min ? inward : -inward;
    }
    return 1;
}

__attribute__((sysv_abi)) void *runtime_hud_quad(void *context, void *vertices, void *rdx) {
    (void)rdx;
    if (!vertices) return NULL;
    const unsigned char *v = vertices;
    float x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY;
    for (int i = 0; i < 4; ++i) {
        const float *f = (const float *)(v + i * VERTEX);
        x0 = fminf(x0, f[0]);
        x1 = fmaxf(x1, f[0]);
        y0 = fminf(y0, f[1]);
        y1 = fmaxf(y1, f[1]);
    }
    const int wide = x0 <= 0.5f && x1 >= 1279.5f && y0 <= 0.5f && y1 >= 719.5f;
    const float *colour = (const float *)(v + 0x20);
    const int frame = !wide && y1 - y0 >= 300.0f && colour[0] == 0.0f && colour[1] == 0.0f && colour[2] == 0.0f &&
                      (x0 <= 0.5f || x1 >= 1279.5f);
    /* The stub (probe.c, build_mouse_look_stub) saved xmm0-7 (0x80 bytes), then r11 r10 r9 r8 rdi
     * rsi rdx rcx rax, then the game's return address. A Windows build sets its frame pointer
     * anywhere in the frame, so the save area is found above this function's locals by the saved
     * rdi and rsi (this call's first two arguments). */
    volatile uint64_t anchor = 0;
    const char *saved = NULL;
    for (const char *p = (const char *)&anchor; p < (const char *)&anchor + 1024; p += 8) {
        uint64_t rdi_rsi[2];
        memcpy(rdi_rsi, p, sizeof(rdi_rsi));
        if (rdi_rsi[0] == (uint64_t)(uintptr_t)context && rdi_rsi[1] == (uint64_t)(uintptr_t)vertices) {
            saved = p - 0x80 - 4 * 8;
            break;
        }
    }
    unsigned char *image = runtime_mouselook_image();
    if (hud_diag()) {
        uint64_t caller = 0;
        if (saved) memcpy(&caller, saved + 0x80 + 9 * 8, sizeof(caller));
        if (image && caller > (uint64_t)(uintptr_t)image) caller -= (uint64_t)(uintptr_t)image;
        log_quad(v, wide, x0, x1, y0, y1, caller);
    }
    const float r = hud_ratio();
    /* The bars go through this builder too (they reach the screen's edges): not again. */
    static __thread int building;
    if ((!wide && !frame) || r <= 0.0f || !image || building) return NULL;
    if (!saved) {
        static int warned;
        if (!warned++) printf("Interface: unexpected hook frame; full-screen layers left as they are\n");
        return NULL;
    }
    float xmm0;
    memcpy(&xmm0, saved, sizeof(xmm0));
    const float half = 640.0f * r;
    unsigned char bar[4 * VERTEX];
    const QuadBuilder builder = (QuadBuilder)(void *)(image + QUAD_BUILDER);
    building = 1;
    if (x0 <= 0.5f && build_bar(bar, v, x0, 640.0f - half)) builder(context, bar, xmm0);
    if (x1 >= 1279.5f && build_bar(bar, v, x1, 640.0f + half)) builder(context, bar, xmm0);
    building = 0;
    return NULL;
}

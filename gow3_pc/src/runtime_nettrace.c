/* Network call trace (GOW3_NET_TRACE=1): every NP (PSN), Net, NetCtl, Http, Ssl and Voice call
 * the game makes is logged with its first six integer arguments and its result, so the path the
 * multiplayer menus take (PSN rooms, Naughty Dog's own servers, LAN) can be read from a log.
 * Arguments known to be strings (URLs, host names, API paths, object names) are printed as text,
 * a few request structures as hex. Busy calls (polls, epoll waits, byte-order helpers) are
 * logged for their first calls only, then every 500th.
 *
 * Each traced import gets its own thunk that forwards six integer registers. The functions it
 * wraps take no floating point or stack arguments the stubs read, so forwarding six is enough
 * for a diagnostic; GOW3_NET_TRACE is off by default. */
#include "runtime.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

typedef uint64_t (ABI *TracedFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
#define TRACE_SLOTS 320
static struct { const char *name; TracedFn fn; uint32_t calls; } traced[TRACE_SLOTS];
static int traced_count;
static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;

static int trace_enabled(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("GOW3_NET_TRACE"); v = e && e[0] == '1'; }
    return v;
}

static int traced_symbol(const char *s) {
    static const char *const prefixes[] = {"sceNp", "sceNet", "sceHttp", "sceSsl", "sceVoice",
                                           "sceInvitationDialog", "sceRudp", "sceAppContent"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(*prefixes); ++i)
        if (!strncmp(s, prefixes[i], strlen(prefixes[i]))) return 1;
    /* POSIX sockets (libScePosix / libkernel) */
    static const char *const posix[] = {"socket", "bind", "connect", "listen", "accept", "send", "recv",
                                        "sendto", "recvfrom", "setsockopt", "getsockopt", "select", "inet_pton"};
    for (size_t i = 0; i < sizeof(posix) / sizeof(*posix); ++i)
        if (!strcmp(s, posix[i])) return 1;
    return 0;
}

/* Calls made every frame or per packet: only the first few, then a sample. */
static int busy_symbol(const char *s) {
    static const char *const busy[] = {"sceNetHton", "sceNetNtoh", "sceNpCheckCallback", "sceNetCtlCheckCallback",
                                       "PollAsync", "sceNetEpollWait", "sceHttpWaitRequest", "sceVoiceReadFromOPort",
                                       "sceVoiceWriteToIPort", "sceVoiceGetPortInfo", "sceNetErrnoLoc", "sceNpPollAsync",
                                       "sceNpGetState", "sceNetCtlGetState", "sceVoiceGetMuteFlag", "sceVoiceGetVolume",
                                       "sceVoiceSetVolume", "sceVoiceSetMuteFlag", "sceNpCmp"};
    for (size_t i = 0; i < sizeof(busy) / sizeof(*busy); ++i)
        if (strstr(s, busy[i])) return 1;
    return 0;
}

/* Which arguments are strings (bit per argument) or a structure worth a hex dump. */
typedef struct { const char *name; unsigned strings; int hex_arg, hex_len; } ArgInfo;
static const ArgInfo arg_info[] = {
    {"sceHttpCreateTemplate", 1u << 1, -1, 0},
    {"sceHttpCreateConnectionWithURL", 1u << 1, -1, 0},
    {"sceHttpCreateRequestWithURL", 1u << 2, -1, 0},
    {"sceHttpAddRequestHeader", (1u << 1) | (1u << 2), -1, 0},
    {"sceHttpRemoveRequestHeader", 1u << 1, -1, 0},
    {"sceHttpUriParse", 1u << 1, -1, 0},
    {"sceNetResolverStartNtoa", 1u << 1, -1, 0},
    {"sceNetResolverCreate", 1u << 0, -1, 0},
    {"sceNetPoolCreate", 1u << 0, -1, 0},
    {"sceNetEpollCreate", 1u << 0, -1, 0},
    {"sceNetSocket", 1u << 0, -1, 0},
    {"sceAppContentAddcontMount", 1u << 1, -1, 0},
    {"sceAppContentAddcontEnqueueDownload", 1u << 1, -1, 0},
    {"sceNetInetPton", 1u << 1, -1, 0},
    {"sceNetBind", 0, 1, 16},
    {"sceNetConnect", 0, 1, 16},
    {"sceNetSendto", 0, 4, 16},
    {"sceNetSetsockopt", 0, 3, 8},
    {"sceNpWebApiCreateRequest", (1u << 1) | (1u << 2), -1, 0},
    {"sceNpSetNpTitleId", 1u << 0, -1, 0},
    {"sceNpMatching2CreateContext", 0, 0, 64},
    {"sceNpMatching2ContextStart", 0, -1, 0},
    {"sceNpMatching2SearchRoom", 0, 1, 64},
    {"sceNpMatching2CreateJoinRoom", 0, 1, 96},
    {"sceNpMatching2JoinRoom", 0, 1, 64},
    {"sceNpMatching2GetWorldInfoList", 0, 1, 16},
    {"sceNpMatching2JoinLobby", 0, 1, 32},
    {"sceNpSignalingCreateContext", 0, 0, 36},
    {"sceNpSignalingActivateConnection", 0, 1, 36},
    {"sceNpAuthGetAuthorizationCode", 0, 1, 48},
    {"sceNpAuthCreateRequest", 0, -1, 0},
    {"sceNpTusCreateNpTitleCtx", 0, 0, 16},
    {"sceNpScoreCreateNpTitleCtx", 0, 0, 16},
};

/* A guest pointer that can be read: the guest runs in this process, so a bad pointer is only
 * possible if the game passes one; it is checked before reading. */
static int readable(uint64_t p, size_t len) {
    if (p < 0x10000 || p > 0x00007fffffffffffull) return 0;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((const void *)(uintptr_t)p, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize >= p + len;
#else
    (void)len;
    return 1;
#endif
}

static void print_arg(char *out, size_t size, const ArgInfo *info, int i, uint64_t v) {
    if (info && (info->strings >> i & 1) && readable(v, 1)) {
        char text[200]; size_t n = 0;
        const char *s = (const char *)(uintptr_t)v;
        while (n + 1 < sizeof(text) && readable(v + n, 1) && s[n]) { text[n] = (s[n] >= 32 && s[n] < 127) ? s[n] : '.'; ++n; }
        text[n] = 0;
        snprintf(out, size, "\"%s\"", text);
        return;
    }
    snprintf(out, size, "0x%" PRIx64, v);
}

static ABI uint64_t trace_call(int slot, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    const char *name = traced[slot].name;
    const ArgInfo *info = NULL;
    for (size_t i = 0; i < sizeof(arg_info) / sizeof(*arg_info); ++i)
        if (!strcmp(arg_info[i].name, name)) { info = &arg_info[i]; break; }
    uint64_t args[6] = {a, b, c, d, e, f};
    char before[1024] = "";
    /* Strings and structures are printed before the call (outputs may overwrite them). */
    uint32_t n = __atomic_add_fetch(&traced[slot].calls, 1, __ATOMIC_RELAXED);
    int log = !busy_symbol(name) || n <= 8 || n % 500 == 0;
    if (log) {
        size_t used = 0;
        for (int i = 0; i < 6 && used < sizeof(before); ++i) {
            char one[260]; print_arg(one, sizeof(one), info, i, args[i]);
            used += (size_t)snprintf(before + used, sizeof(before) - used, "%s%s", i ? ", " : "", one);
        }
        if (info && info->hex_arg >= 0 && readable(args[info->hex_arg], (size_t)info->hex_len) && used < sizeof(before)) {
            const unsigned char *p = (const unsigned char *)(uintptr_t)args[info->hex_arg];
            used += (size_t)snprintf(before + used, sizeof(before) - used, " | arg%d:", info->hex_arg);
            for (int k = 0; k < info->hex_len && used + 3 < sizeof(before); ++k)
                used += (size_t)snprintf(before + used, sizeof(before) - used, "%s%02x", k % 4 ? "" : " ", p[k]);
        }
    }
    uint64_t r = traced[slot].fn(a, b, c, d, e, f);
    if (log) {
        pthread_mutex_lock(&trace_lock);
        printf("NetTrace: %s(%s) = 0x%x%s [#%u]\n", name, before, (uint32_t)r,
               (int32_t)r < 0 ? " (error)" : "", n);
        fflush(stdout);
        pthread_mutex_unlock(&trace_lock);
    }
    return r;
}

#define T(i) static ABI uint64_t trace_##i(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) \
    { return trace_call(i, a, b, c, d, e, f); }
#define T10(i) T(i##0) T(i##1) T(i##2) T(i##3) T(i##4) T(i##5) T(i##6) T(i##7) T(i##8) T(i##9)
T(0) T(1) T(2) T(3) T(4) T(5) T(6) T(7) T(8) T(9)
T10(1) T10(2) T10(3) T10(4) T10(5) T10(6) T10(7) T10(8) T10(9) T10(10) T10(11) T10(12) T10(13) T10(14)
T10(15) T10(16) T10(17) T10(18) T10(19) T10(20) T10(21) T10(22) T10(23) T10(24) T10(25) T10(26) T10(27)
T10(28) T10(29) T10(30) T10(31)
#define P(i) trace_##i
#define P10(i) P(i##0), P(i##1), P(i##2), P(i##3), P(i##4), P(i##5), P(i##6), P(i##7), P(i##8), P(i##9)
static const TracedFn thunks[TRACE_SLOTS] = {
    P(0), P(1), P(2), P(3), P(4), P(5), P(6), P(7), P(8), P(9),
    P10(1), P10(2), P10(3), P10(4), P10(5), P10(6), P10(7), P10(8), P10(9), P10(10), P10(11), P10(12), P10(13),
    P10(14), P10(15), P10(16), P10(17), P10(18), P10(19), P10(20), P10(21), P10(22), P10(23), P10(24), P10(25),
    P10(26), P10(27), P10(28), P10(29), P10(30), P10(31),
};

uintptr_t runtime_nettrace_wrap(const char *nid, uintptr_t function) {
    if (!function || !trace_enabled()) return function;
    const char *symbol = runtime_symbol(nid);
    if (!symbol || !traced_symbol(symbol)) return function;
    pthread_mutex_lock(&trace_lock);
    int slot = -1;
    for (int i = 0; i < traced_count; ++i)
        if (traced[i].fn == (TracedFn)function && !strcmp(traced[i].name, symbol)) slot = i;
    if (slot < 0 && traced_count < TRACE_SLOTS) {
        slot = traced_count++;
        traced[slot].name = symbol;
        traced[slot].fn = (TracedFn)function;
    }
    pthread_mutex_unlock(&trace_lock);
    if (slot < 0) { printf("NetTrace: no slot left for %s (not traced)\n", symbol); return function; }
    return (uintptr_t)thunks[slot];
}

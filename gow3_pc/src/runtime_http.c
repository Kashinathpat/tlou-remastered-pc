/* libSceHttp requests (multiplayer research). By default every transfer fails as unplugged, as
 * before. GOW3_HTTP_FILES=<folder> answers GET requests from files instead of the network:
 * http://<host>[:port]/<path> is <folder>/<host>/<path> ("./" parts dropped, the query ignored).
 * A file that exists is a 200 response with Content-Length and Last-Modified; any other URL
 * still fails. Nothing is ever sent to a server.
 *
 * Non-blocking requests complete at once: sceHttpWaitRequest reports a served request as
 * readable (SCE_HTTP_NB_EVENT_IN) until its body has been read. */
#define _GNU_SOURCE
#include "runtime.h"
#include <ctype.h>
#include <strings.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define HTTP_NETWORK ((int32_t)0x80431063)
#define HTTP_INVALID_ID ((int32_t)0x80431100)
#define HTTP_INVALID_VALUE ((int32_t)0x80431077)
#define HTTP_BEFORE_SEND ((int32_t)0x80431071)
#define HTTP_NB_EVENT_IN 0x1

#define HTTP_OBJECTS 64
typedef struct {
    int32_t id;          /* 0: free slot */
    int kind;            /* 1 template, 2 connection, 3 request */
    char url[1024];
    unsigned char *body; /* served file */
    uint64_t size, offset;
    int sent, served;
    int64_t mtime;
    uintptr_t epoll;     /* handle it is registered with, 0 = none */
    void *user_arg;
    char headers[256];
} HttpObject;
static HttpObject objects[HTTP_OBJECTS];
static pthread_mutex_t http_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t next_object = 0x200;

static const char *files_root(void) {
    const char *v = getenv("GOW3_HTTP_FILES");
    return v && *v ? v : NULL;
}

static HttpObject *object_new(int kind, const char *url) {
    pthread_mutex_lock(&http_lock);
    HttpObject *o = NULL;
    for (int i = 0; i < HTTP_OBJECTS; ++i)
        if (!objects[i].id) { o = &objects[i]; break; }
    if (o) {
        memset(o, 0, sizeof(*o));
        o->id = next_object++;
        o->kind = kind;
        if (url) snprintf(o->url, sizeof(o->url), "%s", url);
    }
    pthread_mutex_unlock(&http_lock);
    return o;
}

static HttpObject *object_get(int32_t id) {
    for (int i = 0; i < HTTP_OBJECTS; ++i)
        if (objects[i].id == id && id) return &objects[i];
    return NULL;
}

static void object_free(HttpObject *o) {
    free(o->body);
    memset(o, 0, sizeof(*o));
}

/* <root>/<host>/<path> for an http(s) URL; 0 if the URL cannot name a file. */
int runtime_http_url_file(const char *root, const char *url, char *out, size_t size) {
    const char *p = strstr(url, "://");
    if (!p) return 0;
    p += 3;
    size_t host_len = strcspn(p, ":/?#");
    if (!host_len || host_len > 255) return 0;
    char host[256];
    memcpy(host, p, host_len); host[host_len] = 0;
    p += host_len;
    if (*p == ':') p += strcspn(p, "/?#");
    size_t n = (size_t)snprintf(out, size, "%s/%s", root, host);
    while (*p == '/') {
        ++p;
        size_t len = strcspn(p, "/?#");
        if (len == 2 && p[0] == '.' && p[1] == '.') return 0; /* never outside the folder */
        if (len && !(len == 1 && p[0] == '.')) {
            if (n + 1 + len >= size) return 0;
            out[n++] = '/';
            memcpy(out + n, p, len); n += len; out[n] = 0;
        }
        p += len;
    }
    for (size_t i = 0; host[i]; ++i)
        if (!isalnum((unsigned char)host[i]) && host[i] != '.' && host[i] != '-') return 0;
    return 1;
}

static void serve(HttpObject *o) {
    const char *root = files_root();
    char path[1400];
    o->served = 0;
    if (!root || !runtime_http_url_file(root, o->url, path, sizeof(path))) {
        printf("Runtime: HTTP %s: no network (GOW3_HTTP_FILES %s)\n", o->url, root ? "has no such path" : "not set");
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { printf("Runtime: HTTP %s: not served (no %s)\n", o->url, path); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    o->body = malloc(size > 0 ? (size_t)size : 1);
    o->size = size > 0 && fread(o->body, 1, (size_t)size, f) == (size_t)size ? (uint64_t)size : 0;
    fclose(f);
    struct stat st;
    o->mtime = stat(path, &st) ? 0 : (int64_t)st.st_mtime;
    time_t t = (time_t)o->mtime;
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char date[64];
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    snprintf(o->headers, sizeof(o->headers),
             "Content-Length: %llu\r\nContent-Type: application/octet-stream\r\nLast-Modified: %s\r\n\r\n",
             (unsigned long long)o->size, date);
    o->served = 1;
    printf("Runtime: HTTP %s: served from %s (%llu bytes)\n", o->url, path, (unsigned long long)o->size);
}

static ABI int32_t http_template(int32_t ctx, const char *agent, int32_t version, int32_t proxy) {
    (void)ctx; (void)agent; (void)version; (void)proxy;
    HttpObject *o = object_new(1, NULL);
    return o ? o->id : HTTP_INVALID_VALUE;
}
static ABI int32_t http_connection(int32_t tmpl, const char *url, int32_t keepalive) {
    (void)tmpl; (void)keepalive;
    HttpObject *o = object_new(2, url);
    return o ? o->id : HTTP_INVALID_VALUE;
}
static ABI int32_t http_request(int32_t conn, int32_t method, const char *url, uint64_t length) {
    (void)conn; (void)method; (void)length;
    if (!url) return HTTP_INVALID_VALUE;
    HttpObject *o = object_new(3, url);
    return o ? o->id : HTTP_INVALID_VALUE;
}
static ABI int32_t http_delete(int32_t id) {
    pthread_mutex_lock(&http_lock);
    HttpObject *o = object_get(id);
    if (o) object_free(o);
    pthread_mutex_unlock(&http_lock);
    return o ? 0 : HTTP_INVALID_ID;
}
static ABI int32_t http_send(int32_t id, const void *data, uint64_t size) {
    (void)data; (void)size;
    pthread_mutex_lock(&http_lock);
    HttpObject *o = object_get(id);
    int32_t r = HTTP_INVALID_ID;
    if (o) { o->sent = 1; serve(o); r = o->served ? 0 : HTTP_NETWORK; }
    pthread_mutex_unlock(&http_lock);
    fflush(stdout);
    return r;
}
static ABI int32_t http_status(int32_t id, int32_t *code) {
    pthread_mutex_lock(&http_lock);
    HttpObject *o = object_get(id);
    int32_t r = !o ? HTTP_INVALID_ID : !o->sent ? HTTP_BEFORE_SEND : !o->served ? HTTP_NETWORK : 0;
    if (!r && code) *code = 200;
    pthread_mutex_unlock(&http_lock);
    return r;
}
/* result: 0 = the length is known (SCE_HTTP_CONTENTLEN_EXIST). */
static ABI int32_t http_length(int32_t id, int32_t *result, uint64_t *length) {
    pthread_mutex_lock(&http_lock);
    HttpObject *o = object_get(id);
    int32_t r = !o ? HTTP_INVALID_ID : !o->sent ? HTTP_BEFORE_SEND : !o->served ? HTTP_NETWORK : 0;
    if (!r) { if (result) *result = 0; if (length) *length = o->size; }
    pthread_mutex_unlock(&http_lock);
    return r;
}
static ABI int32_t http_read(int32_t id, void *data, uint64_t size) {
    pthread_mutex_lock(&http_lock);
    HttpObject *o = object_get(id);
    int32_t r;
    if (!o) r = HTTP_INVALID_ID;
    else if (!o->sent) r = HTTP_BEFORE_SEND;
    else if (!o->served) r = HTTP_NETWORK;
    else {
        uint64_t n = o->size - o->offset;
        if (n > size) n = size;
        if (n > 0x7fffffff) n = 0x7fffffff;
        if (n && data) memcpy(data, o->body + o->offset, (size_t)n);
        o->offset += n;
        r = (int32_t)n;
    }
    pthread_mutex_unlock(&http_lock);
    return r;
}
static ABI int32_t http_headers(int32_t id, char **header, uint64_t *size) {
    pthread_mutex_lock(&http_lock);
    HttpObject *o = object_get(id);
    int32_t r = !o ? HTTP_INVALID_ID : !o->sent ? HTTP_BEFORE_SEND : !o->served ? HTTP_NETWORK : 0;
    if (!r) { if (header) *header = o->headers; if (size) *size = strlen(o->headers); }
    pthread_mutex_unlock(&http_lock);
    return r;
}
/* sceHttpParseResponseHeader(header, len, field, &value, &value_len): the field's value. */
#define HTTP_PARSE_NOT_FOUND ((int32_t)0x80432025)
int32_t runtime_http_parse_header(const char *header, uint64_t len, const char *field, const char **value, uint64_t *value_len) {
    if (!header || !field) return HTTP_INVALID_VALUE;
    size_t flen = strlen(field);
    const char *end = header + len, *line = header;
    while (line < end) {
        const char *eol = line;
        while (eol < end && *eol != '\r' && *eol != '\n') ++eol;
        if ((size_t)(eol - line) > flen && line[flen] == ':' && !strncasecmp(line, field, flen)) {
            const char *v = line + flen + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) ++v;
            if (value) *value = v;
            if (value_len) *value_len = (uint64_t)(eol - v);
            return 0;
        }
        line = eol;
        while (line < end && (*line == '\r' || *line == '\n')) ++line;
    }
    return HTTP_PARSE_NOT_FOUND;
}
static ABI int32_t http_parse_header(const char *header, uint64_t len, const char *field, const char **value, uint64_t *value_len) {
    return runtime_http_parse_header(header, len, field, value, value_len);
}
static ABI int32_t http_epoll_create(int32_t ctx, void **handle) {
    (void)ctx;
    if (!handle) return HTTP_INVALID_VALUE;
    static int32_t next_handle = 0x100;
    pthread_mutex_lock(&http_lock);
    *handle = (void *)(uintptr_t)(next_handle++);
    pthread_mutex_unlock(&http_lock);
    return 0;
}
static ABI int32_t http_set_epoll(int32_t id, void *handle, void *arg) {
    pthread_mutex_lock(&http_lock);
    HttpObject *o = object_get(id);
    if (o) { o->epoll = (uintptr_t)handle; o->user_arg = arg; }
    pthread_mutex_unlock(&http_lock);
    return o ? 0 : HTTP_INVALID_ID;
}
static ABI int32_t http_unset_epoll(int32_t id) {
    pthread_mutex_lock(&http_lock);
    HttpObject *o = object_get(id);
    if (o) { o->epoll = 0; o->user_arg = NULL; }
    pthread_mutex_unlock(&http_lock);
    return o ? 0 : HTTP_INVALID_ID;
}
/* SceHttpNBEvent: uint32 events, uint32 detail, int32 id, (pad), void *userArg: 24 bytes. */
static ABI int32_t http_wait(void *handle, unsigned char *events, int32_t max, int64_t timeout) {
    int32_t n = 0;
    pthread_mutex_lock(&http_lock);
    for (int i = 0; i < HTTP_OBJECTS && events && n < max; ++i) {
        HttpObject *o = &objects[i];
        if (!o->id || o->epoll != (uintptr_t)handle || !o->sent || !o->served || o->offset >= o->size) continue;
        unsigned char *e = events + 24 * n++;
        uint32_t flags = HTTP_NB_EVENT_IN, detail = 0;
        memcpy(e, &flags, 4); memcpy(e + 4, &detail, 4); memcpy(e + 8, &o->id, 4);
        memset(e + 12, 0, 4); memcpy(e + 16, &o->user_arg, 8);
    }
    pthread_mutex_unlock(&http_lock);
    if (!n && timeout > 0) {
#ifdef _WIN32
        compat_sleep_ns((uint64_t)timeout * 1000);
#else
        struct timespec t = {timeout / 1000000, (timeout % 1000000) * 1000};
        nanosleep(&t, NULL);
#endif
    }
    return n;
}
static ABI int32_t http_ok(void) { return 0; }

static const RuntimeExport exports[] = {
    {"sceHttpCreateTemplate", http_template}, {"sceHttpDeleteTemplate", http_delete},
    {"sceHttpCreateConnectionWithURL", http_connection}, {"sceHttpDeleteConnection", http_delete},
    {"sceHttpCreateRequestWithURL", http_request}, {"sceHttpDeleteRequest", http_delete},
    {"sceHttpSendRequest", http_send}, {"sceHttpGetStatusCode", http_status},
    {"sceHttpGetResponseContentLength", http_length}, {"sceHttpReadData", http_read},
    {"sceHttpGetAllResponseHeaders", http_headers}, {"sceHttpParseResponseHeader", http_parse_header},
    {"sceHttpCreateEpoll", http_epoll_create}, {"sceHttpDestroyEpoll", http_ok},
    {"sceHttpSetEpoll", http_set_epoll}, {"sceHttpUnsetEpoll", http_unset_epoll},
    {"sceHttpWaitRequest", http_wait}, {"sceHttpAbortWaitRequest", http_ok},
};
uintptr_t runtime_http_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }

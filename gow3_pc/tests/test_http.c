/* libSceHttp requests: unplugged by default; GOW3_HTTP_FILES serves GETs from files. */
#undef NDEBUG
#include "../src/runtime_http.c"
#include <assert.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }
#ifdef _WIN32
void compat_sleep_ns(uint64_t ns) { (void)ns; }
#endif

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

int main(void) {
    char path[512];
    /* URL -> file: host folder, "./" dropped, query ignored, ".." refused. */
    assert(runtime_http_url_file("root", "http://t1ps4.final.prod.s3.amazonaws.com/./net10.bin.psarc.crypt", path, sizeof(path)));
    assert(!strcmp(path, "root/t1ps4.final.prod.s3.amazonaws.com/net10.bin.psarc.crypt"));
    assert(runtime_http_url_file("root", "https://host:8080/a/b.txt?x=1", path, sizeof(path)) && !strcmp(path, "root/host/a/b.txt"));
    assert(!runtime_http_url_file("root", "http://host/../secret", path, sizeof(path)));
    assert(!runtime_http_url_file("root", "http://bad\\host/x", path, sizeof(path)));
    assert(!runtime_http_url_file("root", "no-scheme", path, sizeof(path)));

    /* Unplugged: objects exist, the transfer fails. */
    set_env("GOW3_HTTP_FILES", NULL);
    int32_t tmpl = http_template(1, "agent", 2, 1);
    int32_t conn = http_connection(tmpl, "http://example.test/file.bin", 1);
    int32_t req = http_request(conn, 0, "http://example.test/file.bin", 0);
    assert(tmpl > 0 && conn > 0 && req > 0);
    int32_t code = 0, result = -1; uint64_t length = 0;
    assert(http_status(req, &code) == HTTP_BEFORE_SEND);
    assert(http_send(req, NULL, 0) == HTTP_NETWORK);
    assert(http_status(req, &code) == HTTP_NETWORK && http_read(req, path, 8) == HTTP_NETWORK);
    assert(http_delete(req) == 0 && http_delete(req) == HTTP_INVALID_ID);

    /* Served from a file. */
    mkdir("http-test", 0755); mkdir("http-test/example.test", 0755);
    FILE *f = fopen("http-test/example.test/file.bin", "wb");
    assert(f); fputs("0123456789", f); fclose(f);
    set_env("GOW3_HTTP_FILES", "http-test");
    req = http_request(conn, 0, "http://example.test/./file.bin", 0);
    void *eh = NULL; assert(http_epoll_create(1, &eh) == 0 && eh);
    assert(http_set_epoll(req, eh, (void *)(uintptr_t)0x1234) == 0);
    unsigned char events[48];
    assert(http_wait(eh, events, 2, 0) == 0); /* nothing sent yet */
    assert(http_send(req, NULL, 0) == 0);
    assert(http_status(req, &code) == 0 && code == 200);
    assert(http_length(req, &result, &length) == 0 && result == 0 && length == 10);
    assert(http_wait(eh, events, 2, 0) == 1);
    uint32_t flags; int32_t id; void *arg;
    memcpy(&flags, events, 4); memcpy(&id, events + 8, 4); memcpy(&arg, events + 16, 8);
    assert(flags == HTTP_NB_EVENT_IN && id == req && arg == (void *)(uintptr_t)0x1234);
    char *headers = NULL; uint64_t header_size = 0;
    assert(http_headers(req, &headers, &header_size) == 0 && header_size == strlen(headers));
    const char *value; uint64_t value_len;
    assert(runtime_http_parse_header(headers, header_size, "content-length", &value, &value_len) == 0);
    assert(value_len == 2 && !strncmp(value, "10", 2));
    assert(runtime_http_parse_header(headers, header_size, "Last-Modified", &value, &value_len) == 0 && value_len > 20);
    assert(runtime_http_parse_header(headers, header_size, "Etag", &value, &value_len) == HTTP_PARSE_NOT_FOUND);
    char body[16] = {0};
    assert(http_read(req, body, 4) == 4 && !memcmp(body, "0123", 4));
    assert(http_read(req, body, 16) == 6 && !memcmp(body, "456789", 6));
    assert(http_read(req, body, 16) == 0);
    assert(http_wait(eh, events, 2, 0) == 0); /* all read */
    assert(http_unset_epoll(req) == 0 && http_delete(req) == 0);

    /* A path the folder lacks still fails. */
    req = http_request(conn, 0, "http://example.test/other.bin", 0);
    assert(http_send(req, NULL, 0) == HTTP_NETWORK);
    http_delete(req); http_delete(conn); http_delete(tmpl);
    remove("http-test/example.test/file.bin"); rmdir("http-test/example.test"); rmdir("http-test");
    puts("http test: ok");
    return 0;
}

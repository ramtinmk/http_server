#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "test_utils.h"
#include "tls_test.h"

/*
 * TLS E2E suite.
 *
 * The server is launched by this suite rather than assumed to be running: it
 * generates a self-signed certificate, forks ./bin/http_server with a plaintext
 * and a TLS listener on two free ports, and drives the TLS path with a real
 * OpenSSL client. The document root is a temporary directory containing one
 * large file, so the TLS buffered pread+SSL_write body path (used for files
 * that exceed the cache threshold) is exercised as well as the cached small
 * bodies. The fixed startup assets (root/home.html, root/hello.html) are read
 * relative to the repository root, which is the test runner's working
 * directory.
 */

#define TLS_TEST_HOST "127.0.0.1"
#define TLS_TEST_TIMEOUT_MS 5000
#define TLS_LARGE_BYTES (2 * 1024 * 1024)
#define TLS_ALPN_WIRE "\x08http/1.1"

typedef struct {
    int status;
    char *body;
    size_t body_len;
} TlsResponse;

typedef struct {
    pid_t server_pid;
    int plain_port;
    int tls_port;
    char tmpdir[PATH_MAX];
    char docroot[PATH_MAX];
    char cert[PATH_MAX];
    char key[PATH_MAX];
    SSL_CTX *client_ctx;
} TlsSuite;

static TlsSuite g_tls;

/* ------------------------------------------------------------------ */
/* Socket and process helpers                                          */
/* ------------------------------------------------------------------ */

static int reserve_free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        close(fd);
        return -1;
    }
    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

static int connect_to_port(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, TLS_TEST_HOST, &addr.sin_addr) <= 0 ||
        connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int wait_for_port(int port, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        int fd = connect_to_port(port);
        if (fd >= 0) {
            close(fd);
            return 0;
        }
        usleep(50000);
    }
    return -1;
}

static int generate_self_signed_cert(const char *dir) {
    snprintf(g_tls.cert, sizeof(g_tls.cert), "%s/cert.pem", dir);
    snprintf(g_tls.key, sizeof(g_tls.key), "%s/key.pem", dir);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execlp("openssl", "openssl", "req", "-x509", "-newkey", "rsa:2048",
               "-keyout", g_tls.key, "-out", g_tls.cert, "-days", "1",
               "-nodes", "-subj", "/CN=localhost", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr,
                "TLS test: `openssl req` failed; is the openssl CLI installed?\n");
        return -1;
    }
    chmod(g_tls.key, 0600);
    return 0;
}

static pid_t spawn_tls_server(void) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        char plain[16];
        char tls[16];
        snprintf(plain, sizeof(plain), "%d", g_tls.plain_port);
        snprintf(tls, sizeof(tls), "%d", g_tls.tls_port);
        setenv("HTTP_SERVER_PORT", plain, 1);
        setenv("HTTP_SERVER_TLS", "1", 1);
        setenv("HTTP_SERVER_TLS_PORT", tls, 1);
        setenv("HTTP_SERVER_TLS_CERT", g_tls.cert, 1);
        setenv("HTTP_SERVER_TLS_KEY", g_tls.key, 1);
        setenv("HTTP_SERVER_DOCUMENT_ROOT", g_tls.docroot, 1);
        setenv("HTTP_SERVER_ACCESS_LOG", "0", 1);
        setenv("HTTP_SERVER_LOG_LEVEL", "error", 1);
        execl("./bin/http_server", "http_server", (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* ------------------------------------------------------------------ */
/* Document-root fixtures                                              */
/* ------------------------------------------------------------------ */

static unsigned char pattern_byte(size_t index) {
    return (unsigned char)(index % 251);
}

static int write_pattern_file(const char *path, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return -1;

    unsigned char buffer[8192];
    size_t written = 0;
    while (written < size) {
        size_t chunk = size - written < sizeof(buffer)
                           ? size - written : sizeof(buffer);
        for (size_t i = 0; i < chunk; i++) buffer[i] = pattern_byte(written + i);
        if (fwrite(buffer, 1, chunk, file) != chunk) {
            fclose(file);
            return -1;
        }
        written += chunk;
    }
    return fclose(file) == 0 ? 0 : -1;
}

static int pattern_matches(const char *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if ((unsigned char)data[i] != pattern_byte(i)) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* OpenSSL client                                                      */
/* ------------------------------------------------------------------ */

static SSL_CTX *build_client_context(void) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    static const unsigned char alpn[] = TLS_ALPN_WIRE;
    SSL_CTX_set_alpn_protos(ctx, alpn, sizeof(alpn) - 1);
    return ctx;
}

static SSL *tls_connect_current(void) {
    int fd = connect_to_port(g_tls.tls_port);
    if (fd < 0) return NULL;
    SSL *ssl = SSL_new(g_tls.client_ctx);
    if (!ssl) {
        close(fd);
        return NULL;
    }
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, "localhost");
    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl);
        close(fd);
        return NULL;
    }
    return ssl;
}

static void tls_close(SSL *ssl) {
    if (!ssl) return;
    int fd = SSL_get_fd(ssl);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    if (fd >= 0) close(fd);
}

static void tls_response_free(TlsResponse *response) {
    if (!response) return;
    free(response->body);
    response->body = NULL;
    response->body_len = 0;
}

static int ssl_read_exact(SSL *ssl, void *buffer, size_t size) {
    size_t received = 0;
    while (received < size) {
        int count = SSL_read(ssl, (char *)buffer + received,
                             (int)(size - received));
        if (count <= 0) return -1;
        received += (size_t)count;
    }
    return 0;
}

static int ssl_read_headers(SSL *ssl, char **headers) {
    *headers = NULL;
    size_t length = 0;
    while (length < 65536) {
        char byte;
        if (SSL_read(ssl, &byte, 1) != 1) {
            free(*headers);
            *headers = NULL;
            return -1;
        }
        char *grown = realloc(*headers, length + 2);
        if (!grown) {
            free(*headers);
            *headers = NULL;
            return -1;
        }
        *headers = grown;
        (*headers)[length++] = byte;
        (*headers)[length] = '\0';
        if (length >= 4 && memcmp(*headers + length - 4, "\r\n\r\n", 4) == 0) {
            return 0;
        }
    }
    free(*headers);
    *headers = NULL;
    return -1;
}

/* Send one request and read one Content-Length-framed response. The connection
 * is left open so a keep-alive test can issue a second request. */
static int tls_request(SSL *ssl, const char *request, TlsResponse *response) {
    memset(response, 0, sizeof(*response));
    if (SSL_write(ssl, request, (int)strlen(request)) <= 0) return -1;

    char *headers = NULL;
    if (ssl_read_headers(ssl, &headers) != 0) return -1;

    if (sscanf(headers, "HTTP/%*s %d", &response->status) != 1) {
        free(headers);
        return -1;
    }
    const char *length_field = strcasestr(headers, "content-length:");
    if (!length_field) {
        free(headers);
        return -1;
    }
    long long length = strtoll(length_field + strlen("content-length:"),
                               NULL, 10);
    free(headers);
    if (length < 0) return -1;

    char *body = malloc((size_t)length + 1);
    if (!body) return -1;
    if (ssl_read_exact(ssl, body, (size_t)length) != 0) {
        free(body);
        return -1;
    }
    body[length] = '\0';
    response->body = body;
    response->body_len = (size_t)length;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Suite lifecycle                                                     */
/* ------------------------------------------------------------------ */

static int tls_suite_setup(void) {
    memset(&g_tls, 0, sizeof(g_tls));
    g_tls.server_pid = -1;

    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(g_tls.tmpdir, sizeof(g_tls.tmpdir), "%s/http_tls_test_XXXXXX", tmp);
    if (!mkdtemp(g_tls.tmpdir)) {
        perror("TLS test: mkdtemp");
        return -1;
    }
    if (snprintf(g_tls.docroot, sizeof(g_tls.docroot), "%s/docroot",
                 g_tls.tmpdir) >= (int)sizeof(g_tls.docroot)) {
        return -1;
    }
    if (mkdir(g_tls.docroot, 0700) != 0) {
        perror("TLS test: mkdir docroot");
        return -1;
    }

    char large[PATH_MAX + 16];
    snprintf(large, sizeof(large), "%s/large.bin", g_tls.docroot);
    if (write_pattern_file(large, TLS_LARGE_BYTES) != 0) {
        fprintf(stderr, "TLS test: cannot write large fixture\n");
        return -1;
    }

    if (generate_self_signed_cert(g_tls.tmpdir) != 0) return -1;

    g_tls.plain_port = reserve_free_port();
    do {
        g_tls.tls_port = reserve_free_port();
    } while (g_tls.tls_port == g_tls.plain_port);
    if (g_tls.plain_port <= 0 || g_tls.tls_port <= 0) {
        fprintf(stderr, "TLS test: cannot reserve ports\n");
        return -1;
    }

    g_tls.client_ctx = build_client_context();
    if (!g_tls.client_ctx) return -1;

    g_tls.server_pid = spawn_tls_server();
    if (g_tls.server_pid < 0) {
        fprintf(stderr, "TLS test: cannot fork the server\n");
        return -1;
    }
    if (wait_for_port(g_tls.plain_port, TLS_TEST_TIMEOUT_MS) != 0 ||
        wait_for_port(g_tls.tls_port, TLS_TEST_TIMEOUT_MS) != 0) {
        fprintf(stderr, "TLS test: server did not start on ports %d/%d\n",
                g_tls.plain_port, g_tls.tls_port);
        return -1;
    }
    return 0;
}

static void tls_suite_teardown(void) {
    if (g_tls.server_pid > 0) {
        kill(g_tls.server_pid, SIGTERM);
        int status = 0;
        waitpid(g_tls.server_pid, &status, 0);
        g_tls.server_pid = -1;
    }
    if (g_tls.client_ctx) {
        SSL_CTX_free(g_tls.client_ctx);
        g_tls.client_ctx = NULL;
    }
    if (g_tls.tmpdir[0]) {
        char large[PATH_MAX + 16];
        snprintf(large, sizeof(large), "%s/large.bin", g_tls.docroot);
        unlink(large);
        unlink(g_tls.cert);
        unlink(g_tls.key);
        rmdir(g_tls.docroot);
        rmdir(g_tls.tmpdir);
    }
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

void test_tls_handshake_and_alpn(void) {
    SSL *ssl = tls_connect_current();
    TEST_ASSERT(ssl != NULL);

    const unsigned char *protocol = NULL;
    unsigned int protocol_length = 0;
    SSL_get0_alpn_selected(ssl, &protocol, &protocol_length);
    TEST_ASSERT(protocol != NULL);
    TEST_ASSERT(protocol_length == strlen("http/1.1"));
    TEST_ASSERT(memcmp(protocol, "http/1.1", protocol_length) == 0);

    const char *version = SSL_get_version(ssl);
    TEST_ASSERT(version != NULL);
    TEST_ASSERT(strcmp(version, "TLSv1.3") == 0 ||
                strcmp(version, "TLSv1.2") == 0);

    const SSL_CIPHER *cipher = SSL_get_current_cipher(ssl);
    TEST_ASSERT(cipher != NULL);
    const char *name = SSL_CIPHER_get_name(cipher);
    TEST_ASSERT(name != NULL);
    TEST_ASSERT(strstr(name, "RC4") == NULL);
    TEST_ASSERT(strstr(name, "3DES") == NULL);
    TEST_ASSERT(strstr(name, "DES") == NULL);
    TEST_ASSERT(strstr(name, "MD5") == NULL);
    TEST_ASSERT(strstr(name, "NULL") == NULL);

    tls_close(ssl);
}

void test_tls_get_cached_body(void) {
    SSL *ssl = tls_connect_current();
    TEST_ASSERT(ssl != NULL);

    TlsResponse response;
    TEST_ASSERT(tls_request(ssl,
        "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        &response) == 0);
    TEST_ASSERT(response.status == 200);
    TEST_ASSERT(response.body != NULL);
    TEST_ASSERT(strstr(response.body, "Greetings!") != NULL);

    tls_response_free(&response);
    tls_close(ssl);
}

void test_tls_keepalive_two_requests(void) {
    SSL *ssl = tls_connect_current();
    TEST_ASSERT(ssl != NULL);

    TlsResponse first;
    TEST_ASSERT(tls_request(ssl,
        "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n",
        &first) == 0);
    TEST_ASSERT(first.status == 200);
    TEST_ASSERT(first.body != NULL);
    TEST_ASSERT(strstr(first.body, "<title>Home Page</title>") != NULL);
    tls_response_free(&first);

    TlsResponse second;
    TEST_ASSERT(tls_request(ssl,
        "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        &second) == 0);
    TEST_ASSERT(second.status == 200);
    TEST_ASSERT(second.body != NULL);
    TEST_ASSERT(strstr(second.body, "Greetings!") != NULL);
    tls_response_free(&second);

    tls_close(ssl);
}

void test_tls_streamed_large_body(void) {
    SSL *ssl = tls_connect_current();
    TEST_ASSERT(ssl != NULL);

    TlsResponse response;
    TEST_ASSERT(tls_request(ssl,
        "GET /large.bin HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        &response) == 0);
    TEST_ASSERT(response.status == 200);
    TEST_ASSERT(response.body_len == TLS_LARGE_BYTES);
    TEST_ASSERT(pattern_matches(response.body, response.body_len));

    tls_response_free(&response);
    tls_close(ssl);
}

void test_plaintext_listener_serves(void) {
    int fd = connect_to_port(g_tls.plain_port);
    TEST_ASSERT(fd != -1);

    const char request[] =
        "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    TEST_ASSERT(send(fd, request, sizeof(request) - 1, 0) ==
                (ssize_t)(sizeof(request) - 1));

    char buffer[4096];
    size_t received = 0;
    for (;;) {
        ssize_t count = recv(fd, buffer + received, sizeof(buffer) - 1 - received, 0);
        if (count <= 0) break;
        received += (size_t)count;
        buffer[received] = '\0';
        if (strstr(buffer, "Greetings!") != NULL) break;
        if (received >= sizeof(buffer) - 1) break;
    }
    TEST_ASSERT(strstr(buffer, "HTTP/1.1 200 OK") != NULL);
    TEST_ASSERT(strstr(buffer, "Greetings!") != NULL);
    close(fd);
}

void test_tls_rejects_garbage_then_serves(void) {
    int fd = connect_to_port(g_tls.tls_port);
    TEST_ASSERT(fd != -1);
    const char garbage[] = "this is not a TLS ClientHello\r\n\r\n";
    (void)send(fd, garbage, sizeof(garbage) - 1, 0);
    close(fd);
    usleep(50000);

    /* A bad handshake must not wedge the listener: a fresh valid handshake and
     * request must still complete. */
    SSL *ssl = tls_connect_current();
    TEST_ASSERT(ssl != NULL);
    TlsResponse response;
    TEST_ASSERT(tls_request(ssl,
        "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        &response) == 0);
    TEST_ASSERT(response.status == 200);
    tls_response_free(&response);
    tls_close(ssl);
}

void run_tls_tests(void) {
    printf("=== TLS E2E Suite (self-contained) ===\n\n");
    signal(SIGPIPE, SIG_IGN);

    if (tls_suite_setup() != 0) {
        printf(ANSI_COLOR_RED
               "    [FAIL] TLS suite setup failed (see diagnostics above)"
               ANSI_COLOR_RESET "\n\n");
        tests_failed++;
        tests_run++;
        tls_suite_teardown();
        return;
    }

    RUN_TEST(test_tls_handshake_and_alpn,
             "TLS 1.2+/ALPN http/1.1 and no weak cipher");
    RUN_TEST(test_tls_get_cached_body, "GET a cached asset over TLS");
    RUN_TEST(test_tls_keepalive_two_requests, "Two requests on one TLS session");
    RUN_TEST(test_tls_streamed_large_body, "Stream a 2 MiB file over TLS");
    RUN_TEST(test_plaintext_listener_serves, "Plaintext listener coexists");
    RUN_TEST(test_tls_rejects_garbage_then_serves,
             "Bad handshake does not wedge the listener");

    tls_suite_teardown();
}

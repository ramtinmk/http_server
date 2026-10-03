#define _GNU_SOURCE

#include "tls.h"
#include "log.h"
#include "server_config.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

/* ------------------------------------------------------------------ */
/* Shared context and reload state                                      */
/* ------------------------------------------------------------------ */

static SSL_CTX *g_ctx;
static char     g_cert_path[4096];
static char     g_key_path[4096];

/* Serializes reload against itself. New handshakes take the context's current
 * certificate; OpenSSL's own locking covers the in-use internal state. */
static pthread_mutex_t g_reload_lock = PTHREAD_MUTEX_INITIALIZER;

/* Set from the SIGHUP handler (async-signal-safe), consumed by an event loop. */
static volatile sig_atomic_t g_reload_requested;

int tls_enabled(void)
{
    return g_ctx != NULL;
}

SSL_CTX *tls_context(void)
{
    return g_ctx;
}

const char *tls_last_error(void)
{
    static char buf[512];
    unsigned long e = ERR_peek_last_error();
    if (e == 0) {
        buf[0] = '\0';
        return buf;
    }
    ERR_error_string_n(e, buf, sizeof(buf));
    ERR_clear_error();
    return buf;
}

/* ------------------------------------------------------------------ */
/* Policy helpers                                                       */
/* ------------------------------------------------------------------ */

/*
 * ALPN select callback: choose "http/1.1" when the client offers it. A client
 * that offers ALPN without http/1.1 still completes the handshake (noack): the
 * record layer is TLS, only the application protocol is unnegotiated.
 */
static int alpn_select_cb(SSL *ssl, const unsigned char **out,
                          unsigned char *outlen, const unsigned char *in,
                          unsigned int inlen, void *arg)
{
    (void)ssl;
    (void)arg;
    /* Server list is in wire format: a 1-byte length followed by the protocol
     * name, so "\x08http/1.1". */
    static const unsigned char http11[] = "\x08http/1.1";
    if (SSL_select_next_proto((unsigned char **)out, outlen, http11,
                              sizeof(http11) - 1, in, inlen) ==
        OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_NOACK;
}

/*
 * Validate the private key file's mode. A world-readable or world-writable
 * private key is a local disclosure risk and is fatal; group-readable is only
 * a warning so common root:ssl-cert 0640 deployments keep working.
 */
static int check_key_permissions(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0; /* unreadable key is reported by the OpenSSL load below */

    if ((st.st_mode & 0007) != 0) {
        fprintf(stderr,
                "FATAL: TLS private key '%s' is accessible to other users "
                "(mode 0%o); restrict it to the server account "
                "(chmod 600)\n", path, (unsigned)(st.st_mode & 0777));
        return -1;
    }
    if ((st.st_mode & 0070) != 0) {
        fprintf(stderr,
                "WARNING: TLS private key '%s' is group-accessible "
                "(mode 0%o); prefer 0600\n",
                path, (unsigned)(st.st_mode & 0777));
    }
    return 0;
}

/* Load the configured certificate chain and key into the shared context.
 * Caller holds g_reload_lock or is single-threaded during startup. */
static int load_cert_key(void)
{
    ERR_clear_error();
    if (SSL_CTX_use_certificate_chain_file(g_ctx, g_cert_path) != 1) {
        fprintf(stderr, "FATAL: cannot load TLS certificate '%s': %s\n",
                g_cert_path, tls_last_error());
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(g_ctx, g_key_path, SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "FATAL: cannot load TLS private key '%s': %s\n",
                g_key_path, tls_last_error());
        return -1;
    }
    if (SSL_CTX_check_private_key(g_ctx) != 1) {
        fprintf(stderr,
                "FATAL: TLS certificate '%s' and key '%s' do not match: %s\n",
                g_cert_path, g_key_path, tls_last_error());
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int tls_init(const ServerConfig *cfg)
{
    if (!cfg->tls_enabled)
        return 0;

    if (cfg->tls_cert_file[0] == '\0') {
        fprintf(stderr, "FATAL: tls=1 requires the 'tls_cert_file' key\n");
        return -1;
    }
    if (cfg->tls_key_file[0] == '\0') {
        fprintf(stderr, "FATAL: tls=1 requires the 'tls_key_file' key\n");
        return -1;
    }
    if (cfg->tls_port == cfg->port) {
        fprintf(stderr,
                "FATAL: tls_port=%d must differ from port=%d\n",
                cfg->tls_port, cfg->port);
        return -1;
    }
    if (check_key_permissions(cfg->tls_key_file) != 0)
        return -1;

    snprintf(g_cert_path, sizeof(g_cert_path), "%s", cfg->tls_cert_file);
    snprintf(g_key_path, sizeof(g_key_path), "%s", cfg->tls_key_file);

    g_ctx = SSL_CTX_new(TLS_server_method());
    if (!g_ctx) {
        fprintf(stderr, "FATAL: SSL_CTX_new failed: %s\n", tls_last_error());
        return -1;
    }

    /* Protocol floor: TLS 1.2+, prefer 1.3. Disable compression and
     * renegotiation; prefer the server's cipher order. 0-RTT is off by default
     * (no SSL_CTX_set_max_early_data call). */
    if (SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION) != 1) {
        fprintf(stderr, "FATAL: cannot set TLS 1.2 minimum: %s\n",
                tls_last_error());
        tls_shutdown();
        return -1;
    }
    long opts = SSL_OP_NO_COMPRESSION | SSL_OP_CIPHER_SERVER_PREFERENCE;
#ifdef SSL_OP_NO_RENEGOTIATION
    opts |= SSL_OP_NO_RENEGOTIATION;
#endif
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    /* A peer that closes without sending close_notify is a clean EOF, not a
     * fatal protocol error. Without this, OpenSSL 3 reports the FIN as
     * SSL_ERROR_SSL (SSL_R_UNEXPECTED_EOF_WHILE_READING) and the event loop
     * drops a keep-alive connection the client only intended to reuse. */
    opts |= SSL_OP_IGNORE_UNEXPECTED_EOF;
#endif
    SSL_CTX_set_options(g_ctx, opts);
    SSL_CTX_set_mode(g_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                            SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                            SSL_MODE_RELEASE_BUFFERS);

    /* A modern ECDHE-only suite list; TLS 1.3 suites use OpenSSL's safe
     * defaults. ECDSA and RSA leaf certificates are both covered. */
    if (SSL_CTX_set_cipher_list(
            g_ctx,
            "ECDHE-ECDSA-AES128-GCM-SHA256:"
            "ECDHE-RSA-AES128-GCM-SHA256:"
            "ECDHE-ECDSA-AES256-GCM-SHA384:"
            "ECDHE-RSA-AES256-GCM-SHA384:"
            "ECDHE-ECDSA-CHACHA20-POLY1305:"
            "ECDHE-RSA-CHACHA20-POLY1305") != 1) {
        fprintf(stderr, "FATAL: cannot set TLS cipher list: %s\n",
                tls_last_error());
        tls_shutdown();
        return -1;
    }

    SSL_CTX_set_alpn_select_cb(g_ctx, alpn_select_cb, NULL);

    /* Enable session resumption (both session-id and TLS 1.3 tickets). */
    SSL_CTX_set_session_cache_mode(g_ctx, SSL_SESS_CACHE_SERVER);

    if (load_cert_key() != 0) {
        tls_shutdown();
        return -1;
    }

    log_msg(LOG_LEVEL_INFO, "tls_init ok port=%d cert=%s key=%s",
            cfg->tls_port, g_cert_path, g_key_path);
    return 0;
}

void tls_shutdown(void)
{
    if (g_ctx) {
        SSL_CTX_free(g_ctx);
        g_ctx = NULL;
    }
}

SSL *tls_new_conn(int fd)
{
    if (!g_ctx)
        return NULL;
    SSL *ssl = SSL_new(g_ctx);
    if (!ssl)
        return NULL;
    if (SSL_set_fd(ssl, fd) != 1) {
        SSL_free(ssl);
        return NULL;
    }
    SSL_set_accept_state(ssl);
    return ssl;
}

int tls_handshake_step(SSL *ssl, int *want_write)
{
    *want_write = 0;
    /* SSL_get_error() attributes whatever is on the (thread-local) OpenSSL
     * error queue, so it must be empty before each I/O call. Otherwise a
     * stale error from a previous connection makes a benign WANT_READ look
     * like a fatal SSL_ERROR_SSL. */
    ERR_clear_error();
    int r = SSL_accept(ssl);
    if (r == 1)
        return 1;

    int err = SSL_get_error(ssl, r);
    if (err == SSL_ERROR_WANT_READ)
        return 0;
    if (err == SSL_ERROR_WANT_WRITE) {
        *want_write = 1;
        return 0;
    }
    return -1;
}

void tls_request_reload(void)
{
    g_reload_requested = 1;
}

int tls_reload_if_requested(void)
{
    if (!g_reload_requested)
        return 0;
    g_reload_requested = 0;
    if (!g_ctx)
        return 0;

    pthread_mutex_lock(&g_reload_lock);
    int rc = load_cert_key();
    pthread_mutex_unlock(&g_reload_lock);

    if (rc != 0) {
        log_msg(LOG_LEVEL_ERROR, "tls_cert_reload failed; keeping previous");
        return -1;
    }
    log_msg(LOG_LEVEL_INFO, "tls_cert_reload ok cert=%s", g_cert_path);
    return 0;
}

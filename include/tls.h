#ifndef TLS_H
#define TLS_H

#include "config.h"

/*
 * OpenSSL-backed TLS termination.
 *
 * One process-wide SSL_CTX is built at startup from the resolved configuration
 * and shared read-only by every event loop; each accepted TLS connection gets
 * its own SSL object. The event loop drives the handshake and record I/O
 * nonblocking (see event_loop.c); this module owns the policy (TLS 1.2
 * minimum, TLS 1.3 preferred, ALPN http/1.1) and certificate lifecycle.
 *
 * The context is opaque here; callers that need the OpenSSL types include
 * <openssl/ssl.h> themselves.
 */

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st     SSL;

/*
 * Build the shared context from `cfg` and load the certificate and key.
 * Returns 0 on success, including when TLS is disabled (a no-op). Returns -1
 * on a missing path, an unreadable/invalid certificate or key, a key/cert
 * mismatch, or an insecure key-file mode. Fatal at startup.
 */
int tls_init(const ServerConfig *cfg);

/* Free the shared context. Safe to call once; a no-op when TLS is disabled. */
void tls_shutdown(void);

/* Nonzero when TLS is enabled and the context is ready. */
int tls_enabled(void);

/* The shared context, or NULL when TLS is disabled. */
SSL_CTX *tls_context(void);

/* Create a server-side SSL bound to `fd` (which must be nonblocking), or NULL
 * on allocation failure. */
SSL *tls_new_conn(int fd);

/*
 * Drive one step of the server handshake. Returns 1 when the handshake is
 * complete, 0 when more I/O is needed (`*want_write` selects the epoll
 * direction to await), and -1 on a fatal handshake error.
 */
int tls_handshake_step(SSL *ssl, int *want_write);

/*
 * Async-signal-safe: request a certificate/key reload on the next event-loop
 * tick. The signal handler only sets a flag.
 */
void tls_request_reload(void);

/*
 * Perform a requested reload from an event-loop thread. Returns 0 when nothing
 * was requested or the reload succeeded, and -1 when a reload was requested but
 * failed (the previous certificate remains in use). Reloads never close an
 * in-flight connection.
 */
int tls_reload_if_requested(void);

/* Human-readable OpenSSL error stack (single line) for logging, or "" when the
 * stack is empty. */
const char *tls_last_error(void);

#endif /* TLS_H */

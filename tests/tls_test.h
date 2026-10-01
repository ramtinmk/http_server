#ifndef TEST_TLS_H
#define TEST_TLS_H

/* Runs the self-contained TLS E2E suite. It generates a throwaway self-signed
 * certificate, forks its own server instance with both a plaintext and a TLS
 * listener, and exercises the TLS record path through the OpenSSL client API.
 * No externally started server is required. */
void run_tls_tests(void);

#endif // TEST_TLS_H

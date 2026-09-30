---
plan_id: production-http-server-phase3
title: Phase 3 TLS termination
category: implementation
status: done
owner: agent
created: 2026-09-30
updated: 2026-09-30
related: [production-http-server, production-http-server-phase2]
---

# Phase 3 TLS termination

## Purpose

Serve the Phase 2 document root over HTTPS: a nonblocking TLS handshake and
record path inside the existing `epoll` event loop, hardened protocol/cipher
defaults, ALPN advertising `http/1.1`, fail-fast certificate loading with
`SIGHUP` hot reload, and TLS-specific metrics. This is
`production-http-server` phase 3; plaintext stays the reference path and the
fallback (the program plan's rollback for TLS regression).

**Library choice (recorded before implementation):** **OpenSSL 1.1.1f**
(`libssl-dev`, `pkg-config --modversion openssl` = 1.1.1f, the Ubuntu 20.04
system package). It is already available on the target host, supports TLS 1.3,
ALPN, secure renegotiation, and session resumption, and is the vetted library the
program plan names. mbedTLS was the alternative; OpenSSL wins on ALPN/1.3
maturity and host availability.

## Scope

In: a new `src/tls.c` + `include/tls.h` (OpenSSL context, cert/key load and
reload, handshake step, ALPN callback, protocol/cipher policy), TLS state in
`ELConnection` and the state machine in `src/event_loop.c` (handshake, TLS
`recv`/`send`, a buffered file-body path replacing `sendfile` for TLS), a second
per-loop listener, the `ServerConfig` TLS surface (`include/config.h`,
`src/config.c`, `include/server_config.h`), TLS counters in `src/metrics.c`,
startup wiring and `SIGHUP` in `src/main.c`, a `scripts/phase3_tls_test.py` E2E
harness that writes `benchmarks/production_phase3_tls.json`, and the
architecture/env-var/gotcha/readme/AGENTS docs plus the program-plan checklist.

Out: HTTP/2 and HTTP/3 (ALPN is left ready), 0-RTT / early data, client
certificate (mTLS) verification, OCSP stapling, SNI multi-certificate, and
per-IP controls (phase 4).

## Baseline

- No TLS anywhere: the server binds one plaintext listener per loop and calls
  `recv()`/`send()`/`sendfile()` directly on the accepted fd
  (`src/event_loop.c`).
- `SIGHUP` only asks the log writer to reopen its file (`src/main.c:504`).
- No certificate handling, no ALPN, no TLS configuration keys.
- Evidence: `readme.md` Roadmap, `docs/architecture.md`.

## Steps

1. [ ] Configuration surface: `tls` (bool), `tls_port`, `tls_cert_file`,
   `tls_key_file` as runtime keys (`CFG_KEYS` + `config_defaults`) with
   compile-time defaults in `server_config.h`; `--help` and startup print list
   them. An enabled TLS listener with a missing cert/key is fatal and names the
   key.
2. [ ] `include/tls.h` / `src/tls.c`: `tls_init` builds one shared `SSL_CTX`
   (TLS 1.2+ minimum, TLS 1.3 preferred, `SSL_OP_NO_COMPRESSION` /
   `SSL_OP_NO_RENEGOTIATION`, a modern ECDHE cipher list, ALPN select callback
   advertising `http/1.1`), loads and validates the certificate chain and
   private key, checks the key file's permissions, and enables resumption
   tickets. `tls_new_conn` creates a server `SSL` per accepted fd with
   `SSL_MODE_ENABLE_PARTIAL_WRITE` and `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER`.
   `tls_reload` re-reads cert/key into the live context.
3. [ ] Event-loop TLS state machine: a `CONN_TLS_HANDSHAKE` state driven by
   `SSL_accept()` handling `SSL_ERROR_WANT_READ`/`WANT_WRITE`; the
   header-read deadline bounds the handshake; TLS `SSL_read`/`SSL_write`
   wrappers with explicit `WANT_*` retry; file-backed bodies streamed through a
   bounded per-connection buffer (`pread` + `SSL_write`) instead of `sendfile`;
   `SSL_free` on close. No 0-RTT (early data left at the 0 default).
4. [ ] Dual listeners: each loop optionally binds a second `SO_REUSEPORT`
   listener on `tls_port`; the accept path tags accepted sockets as TLS and
   starts the handshake. Plaintext and TLS are independently toggled by config;
   `tls_port` must differ from `port`.
5. [ ] Certificate hot reload: `SIGHUP` sets a flag from the signal handler;
   the event-loop deadline tick calls `tls_reload_if_requested()` to reload the
   certificate into the live context without dropping connections. A failed
   reload keeps the previous certificate and logs a warning.
6. [ ] TLS metrics: successful handshakes, resumed sessions, and handshake
   failures as cumulative counters, emitted in the JSON snapshot.
7. [ ] Tests: `scripts/phase3_tls_test.py` generates a throwaway self-signed
   cert, starts the server with plaintext + TLS, drives the existing HTTP corpus
   over `openssl s_client`/`ssl` and a raw request over TLS, asserts a TLS
   request for a doc-root file succeeds with the right body, exercises a
   large-file (buffered) TLS body, checks ALPN negotiates `http/1.1`, verifies
   `SIGHUP` cert reload drops no connections, and records the TLS/plaintext
   throughput ratio in `benchmarks/production_phase3_tls.json`.
8. [ ] Update `docs/architecture.md`, `docs/env-vars.md`, `docs/gotchas.md`,
   `readme.md`, the `AGENTS.md` files, and the `production-http-server` program
   plan phase 3 checklist.

## Validation

- [x] `cmake -S . -B . && make` with no new warnings under `-Wall -Wextra
      -pedantic`.
- [x] `./bin/run_tests ring`.
- [x] `./bin/http_server & ... ./bin/run_tests server` per `AGENTS.md`
      (plaintext regression path unchanged).
- [x] `python3 scripts/phase3_tls_test.py` exits 0 and writes the artifact.
- [x] `openssl s_client -alpn http/1.1 -connect 127.0.0.1:<tls_port>` completes
      a `GET /` and reports a TLS 1.2+/1.3 cipher with no weak finding.
- [x] `make lint` completes with no compiler diagnostics; the only new
      clang-tidy records are the repo-wide `readability-braces-around-statements`
      style notes that every existing source already produces (no
      correctness/analyzer findings in `src/tls.c`, `src/event_loop.c`, or
      `src/metrics.c`).

## Exit criteria

- [x] A local protocol/cipher scan (`openssl s_client`, checking the negotiated
      protocol and cipher and the ALPN result) reports no weak protocol, cipher,
      or certificate finding; evidence: the `tls` block of
      `benchmarks/production_phase3_tls.json`.
- [x] Handshakes under load never stall an event loop; `openssl s_client` and a
      scripted client complete a request; evidence: the concurrency handshake
      case and zero `tls_handshake_failures` in the artifact, plus server-side
      `header_timeout` staying 0 during the TLS load.
- [x] TLS `hardware_agnostic_rps` is recorded and the TLS/plaintext ratio is
      documented; cert reload drops zero connections; evidence:
      `benchmarks/production_phase3_tls.json` `ratio` and
      `reload_connections_before/after`.

## Risks and rollback

- **TLS integration stalls loops or regresses throughput** → TLS is behind the
  `tls` toggle and a separate listener; plaintext remains the default path and
  is untouched for non-TLS connections. Rollback: set `tls = 0`.
- **A TLS write/read `WANT_*` state machine bug desynchronizes a response** →
  the header/body send uses an explicit offset cursor and `SSL_MODE_*` partial
  writes; a mismatch is caught by the TLS body-corpus test against the plaintext
  sha256 of the same file. Rollback: disable the TLS listener.
- **`sendfile` is unavailable over TLS** → TLS file bodies use a bounded
  per-connection `pread`+`SSL_write` buffer; the buffer is fixed-size
  (`TLS_FILE_BUF_SIZE`) so memory stays bounded. Rollback: disable TLS.
- **Certificate reload races an in-flight handshake** → reload only mutates the
  context's certificate under a mutex; existing `SSL` objects and handshakes
  keep the certificate they started with. Rollback: restart to rotate certs.
- **Weak default policy** → min protocol is TLS 1.2, TLS 1.3 preferred, CBC/RC4/
  3DES disabled, compression and renegotiation off, and the scan case asserts
  the negotiated suite. Rollback: none required; policy is in `src/tls.c`.

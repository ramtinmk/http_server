# tests/ Notes

Repo-wide rules and the test policy live in `../AGENTS.md`. Read those first.

## Layout

- `main_test.c` — the suite router. It calls `run_ring_buffer_tests()` and
  `run_server_tests()` based on `argv[1]` (`ring`, `server`, or both). A new
  `tests/*.c` file must be added to the explicit `TEST_SOURCES` list in
  `../CMakeLists.txt` (the build does not glob) and then wired into
  `main_test.c` — otherwise it **runs nothing**.
- `server_test.c` — the server E2E suite (`RUN_TEST(...)` list at the bottom).
  Requires `./bin/http_server` on `127.0.0.1:8081`. Tests speak raw sockets to
  assert framing, keep-alive, pipelining, timeouts, and status codes.
- `tls_test.c` — the self-contained TLS E2E suite. It generates a throwaway
  self-signed certificate, forks its own server with plaintext + TLS listeners
  on ephemeral ports, and drives the TLS record path through the OpenSSL client
  API: handshake, ALPN `http/1.1`, protocol/cipher floor, cached and streamed
  (2 MiB) bodies, one-session keep-alive, plaintext coexistence, and a garbage
  handshake that must not wedge the listener. Needs no external server.
- `test_ring_buffer.c` — server-free unit suite.
- `test_http_benchmark.py` — Python unit tests for the harness CSV/metrics
  mapping; part of CTest as `benchmark_python_tests`.
- `test_benchmark_corpus.py` — E2E tests for the deterministic corpus generator:
  manifest vs on-disk bytes, reproducibility, tamper detection, and the
  cached-vs-streamed class boundary; part of CTest as `benchmark_corpus_tests`.
- `test_utils.h` — shared assertions/helpers.

## Running

- `./bin/run_tests ring` — no server needed.
- `./bin/run_tests tls` — self-contained; starts and stops its own server.
- `./bin/run_tests server` — start the server from the repo root first; see the
  recipe in `../AGENTS.md`.
- `ctest --output-on-failure` — `unit_tests` needs the server; start it first.

## Writing tests

Follow the policy in `../AGENTS.md`: prefer E2E, never write unit tests after the
code, avoid tautological and change-detector tests, and only add regression
tests when there is a genuine behavior gap. Prefer extending `server_test.c` with
a real request/response scenario over asserting on internal tables.

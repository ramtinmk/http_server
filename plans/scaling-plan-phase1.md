# Phase 1 Implementation Plan

This plan supports Phase 1 of `plans/scaling-plan.md`: remove avoidable
per-request work without changing the server's concurrency model.

## Scope

Work in the existing static-response path, primarily in `src/http_server.c`
and related declarations in `include/`. Keep the implementation small and use
the existing `/home` and `/hello` assets. Do not redesign the worker pool,
queue, or socket I/O in this phase.

## Implementation steps

1. Make `BACKLOG` configurable and raise its default from 10 to a reasonable
   burst-tolerant value such as 1024. Note that the kernel `somaxconn` limit
   may cap the effective value; this is not expected to improve sustained
   throughput by itself.
2. Load `home.html` and `hello.html` once during startup. Retain their body
   bytes and lengths in server-lifetime storage, and fail startup clearly if an
   asset cannot be loaded.
3. Precompute the plain response metadata and headers needed by the existing
   response path. Select a complete cached representation in the hot path
   instead of reopening, stat'ing, or reformatting the asset for every request.
4. Generate and retain one gzip representation per asset at startup. The
   request path should select cached plain or gzip bytes rather than invoking
   zlib for every request.
5. Preserve existing behavior for `GET`, `HEAD`, 404, unsupported methods,
   keep-alive, and `Accept-Encoding`. In particular, `HEAD` must send the
   selected headers and `Content-Length` without a body, and gzip negotiation
   must continue to handle `gzip;q=0`, `identity`, missing, or malformed
   headers correctly.
6. Reuse request buffers and remove only clearly safe repeated work, such as
   redundant path formatting or string-length calculation. Avoid introducing a
   new cache subsystem or synchronization scheme.

The existing access-log gate is already complete. Keep throughput runs with
`HTTP_SERVER_ACCESS_LOG=0`.

## Validation

- Add or update focused tests for plain and gzip responses, decompression,
  `HEAD`, encoding negotiation, 404, and keep-alive behavior.
- Run `make` and the applicable unit and server test suites from the repository
  root.
- Run the benchmark matrix before and after the change. Compare offered load,
  completed throughput, errors, and steady-state p99, and verify that response
  bytes and status codes are unchanged.
- Treat the phase as complete when the existing p99 and 5,000 req/s criteria
  are met, or when profiling shows that the remaining limit is the blocking
  connection architecture.

Cached assets are fixed for the server lifetime in this phase. If reloadable
files are needed later, add an explicit reload or development mode rather than
silently changing production behavior.

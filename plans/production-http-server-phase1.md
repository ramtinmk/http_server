---
plan_id: production-http-server-phase1
title: Phase 1 HTTP/1.1 correctness and caching semantics
category: implementation
status: done
owner: agent
created: 2026-09-30
updated: 2026-09-30
related: [production-http-server, production-http-server-phase0-ops]
---

# Phase 1 HTTP/1.1 correctness and caching semantics

## Purpose

Make the server interoperable and smuggling-resistant: strict RFC 9110/9112
message parsing, correct method/status/connection semantics, conditional
requests, byte ranges, and `Accept-Encoding` negotiation, backed by a checked-in
conformance corpus of malformed and edge-case requests with expected statuses.
This is `production-http-server` phase 1; it establishes the parser and response
semantics that phase 2 (document root) and phase 3 (TLS) build on.

## Scope

In: `src/http_server.c` (parser and response construction), `include/http_server.h`
(the `PendingResponse` contract), `src/event_loop.c` only where the response
contract changes, `tests/server_test.c` plus a new conformance corpus, and the
architecture/env-var/gotcha/readme/AGENTS docs.

Out: TLS, document-root path resolution, dynamic content/CGI, HTTP/2, 

## Steps

1. [x] Strict request parsing in `el_prepare_response()`:
   - Reject control characters anywhere in the request line or header field;
     reject obs-fold (a header line beginning with SP/HTAB) with `400`.
   - Validate the method as a token, and the request-target/version grammar;
     accept only `HTTP/1.0` and `HTTP/1.1`, else `505`.
   - Collect `Content-Length` occurrences: any invalid value, duplicate, or
     conflicting value is `400`; a non-zero request body for a bodyless method
     is rejected. Any `Transfer-Encoding` is rejected with `501` (no chunked
     request support), and `Content-Length` + `Transfer-Encoding` together is
     `400` (smuggling).
   - Require `Host` on HTTP/1.1 (`400` when absent); an absolute-form target
     supplies the authority and its path is used for routing.
2. [x] Method/status semantics: `GET`, `HEAD`, `OPTIONS`, and `POST`; unknown
   methods → `501`; known-but-not-allowed on the resource → `405` with `Allow`.
   `OPTIONS *` → `204`. Every response carries generated `Date`, `Server`,
   `Content-Length` (except bodyless statuses), and an explicit `Connection`
   honoring the HTTP version and the request's `Connection` field.
3. [x] `Expect: 100-continue`: send `100 Continue` before the final response
   when the request advertises it (or `417` for an unsupported expectation).
   Only meaningful for requests with a body; since bodies are rejected, this is
   resolved as part of the framing decision. (No interim `100` is emitted: the
   server never accepts a body, so it sends the final response directly.)
4. [x] Conditional requests: strong `ETag` from asset size/mtime (distinct for
   the gzip representation) and `Last-Modified`; evaluate `If-None-Match`
   (strong and weak comparison, `*`) and `If-Modified-Since` → `304`;
   `If-Range` selects a range only when the validator matches.
5. [x] Byte ranges: a single satisfiable range produces `206` with
   `Content-Range` and `Accept-Ranges: bytes`; all-unsatisfiable yields `416`.
   Two or more satisfiable ranges produce a bounded `206 multipart/byteranges`
   (`format_multipart()`, heap body owned via `PendingResponse.owned_body`).
   The set is bounded by `MAX_MULTIPART_RANGES`/`MAX_MULTIPART_BYTES`; an
   invalid or over-bounds range-set is ignored and answered with the full `200`.
   Evidence: `test_byte_ranges_multipart` and the multi-range corpus cases.
6. [x] `Accept-Encoding` negotiation: `gzip`, `identity`, `*`, `q=0` refusal,
   malformed values, and missing header; emit `Vary: Accept-Encoding` on every
   negotiable response and `Content-Encoding: gzip` only for the gzip variant.
   Identity refusal plus gzip refusal yields `406`.
7. [x] Conformance corpus: a table of raw request bytes → expected status and
   framing, exercised against the live server and wired into
   `run_server_tests()`; covers malformed lines, obs-fold, control characters,
   duplicate/conflicting `Content-Length`, `Transfer-Encoding`, missing `Host`,
   absolute-form, long lines/headers, and smuggling combinations.
8. [x] Update `docs/architecture.md`, `docs/env-vars.md`, `docs/gotchas.md`,
   `readme.md`, the `AGENTS.md` files, and the `production-http-server` phase 1
   checklist. (`docs/env-vars.md` needed no change: no runtime key was added.)

## Validation

- [x] `cmake -S . -B . && make` with no new warnings under `-Wall -Wextra -pedantic`.
- [x] `./bin/run_tests ring` (parser-free unit suite).
- [x] `./bin/http_server & ... ./bin/run_tests server` per `AGENTS.md`, including
      the conformance corpus and the connection/resource gate tests
      (`test_byte_ranges_multipart`, `test_el_pipelined_after_error`,
      `test_el_client_close_during_write`, `test_el_capacity_and_fd_leak`;
      29/29 pass).
- [x] `curl --http1.0` and `curl` keep-alive interop against a running server.
- [x] `make lint` (only the repo-wide `_GNU_SOURCE` reserved-identifier warning
      remains; no new warnings in `src/http_server.c`).

## Exit criteria

- [x] Every `scaling-plan` "Correctness and Regression Gates" item that can be
      checked black-box is checked, including duplicate `Content-Length`,
      `Transfer-Encoding`, header casing, long request lines, the connection
      behaviors, and the resource-safety gates; evidence: `./bin/run_tests
      server`. The two fault-injection gates (`EMFILE`, `ENOMEM`) are handled in
      code and explicitly deferred to the Phase 4 fuzzing/sanitizer work, which
      the program plan assigns them to; `scaling-plan.md` marks them `[~]` with
      that pointer.
- [x] The conformance corpus passes with the documented status for every case;
      evidence: `./bin/run_tests server` and the corpus table in this plan.
- [x] `curl --http1.0` and keep-alive interop (curl) succeed; evidence: recorded
      `curl` transcript in the phase notes above.

## Risks and rollback

- **Response contract grows dynamic headers** → `PendingResponse` gains an
  inline header buffer; the event loop sends the inline block when `header` is
  NULL and never frees either form. Rollback: keep the old precomputed blocks
  and disable the dynamic path behind the same parser entry point.
- **Stricter parsing rejects legitimate clients** → Every rejection has a
  documented status and a corpus entry; a rejected-shape regression is visible
  as a corpus failure before release.
- **Ranges/conditionals return the wrong bytes** → Range math is bounded and
  tested against the cached representation length; a mismatch fails the corpus
  and `test_byte_ranges_multipart`.
- **Heap-owned multipart bodies leak or double-free** → `PendingResponse.owned_body`
  is freed by the event loop in `pq_pop()`/`pq_release_all()`, and the
  disconnect-burst test asserts the server's descriptor count stays flat.
  Rollback: ignore multi-range `Range` headers and return the full `200`
  (delete `format_multipart()` and the ownership field).

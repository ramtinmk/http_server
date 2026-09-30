---
plan_id: production-http-server-phase2
title: Phase 2 Secure static file serving
category: implementation
status: done
owner: agent
created: 2026-09-30
updated: 2026-09-30
related: [production-http-server, production-http-server-phase1]
---

# Phase 2 Secure static file serving

## Purpose

Serve an operator-configured document root safely and efficiently on top of the
corrected Phase 1 parser: resolve request paths without ever escaping the root,
map files to MIME types, reuse the Phase 1 validators (`ETag`, `Last-Modified`,
conditional requests, ranges), stream large files without blocking an event
loop, and keep file descriptors and cached bytes inside their configured
budgets. This is `production-http-server` phase 2; phase 3 (TLS) and phase 4
(sandbox) build on the resolved-path boundary established here.

## Scope

In: a new `src/path_resolver.c` (decode + normalize + `openat2`/`O_NOFOLLOW`),
a new `src/file_cache.c` (bounded, ref-counted representation cache), MIME
resolution, directory-index handling, the `ServerConfig` document-root surface
(`include/config.h`, `src/config.c`, `include/server_config.h`), file-backed
streaming responses in `include/http_server.h`/`src/http_server.c` and
`src/event_loop.c`, a Python E2E acceptance harness plus additions to
`tests/server_test.c`, and the architecture/env-var/gotcha/readme/AGENTS docs.

Out: TLS, dynamic content/CGI, HTTP/2, directory listing, `sendfile` for gzip
representations, and per-IP resource controls (phase 4).

## Baseline

- Only three fixed paths are served: `/` and `/home` -> `home.html`, `/hello` ->
  `hello.html`; every other path is `404` (`src/http_server.c:1528`).
- `home.html`/`hello.html` are read and gzip-precompressed once at startup
  (`initialize_static_responses()`); there is no document root, no MIME map, no
  path resolution, and no cache eviction.
- Response bodies always point into cached/static memory or one heap-owned
  multipart buffer; there is no file-descriptor-backed streaming path
  (`PendingResponse` in `include/http_server.h`).
- Evidence: `readme.md` routes table, `docs/architecture.md` "Static assets and
  response semantics".

## Steps

1. [x] Configuration surface: `document_root`, `index_files`, `mime_types`,
   `hidden_files`, `symlinks`, `cache_budget_bytes` as runtime keys
   (`CFG_KEYS` + `config_defaults`) with compile-time defaults in
   `server_config.h`; startup validates the root is an accessible directory and
   prints the resolved values.
2. [x] Path resolution (`src/path_resolver.c`): percent-decode exactly once;
   reject `%00`, backslashes, and control bytes; split and normalize `.`/`..`
   with a `..` that would escape the root rejected; enforce the hidden-file and
   symlink policies; open with `openat2(RESOLVE_BENEATH[|RESOLVE_NO_SYMLINKS])`
   and fall back to an `O_NOFOLLOW` dirfd walk; resolve directories to an index
   file; require a regular file.
3. [x] MIME map: builtin extension table plus an optional operator-supplied
   `mime.types`-style file; `Content-Type` for every doc-root response.
4. [x] Bounded representation cache (`src/file_cache.c`): LRU with a byte budget
   and entry cap, per-entry reference count so an in-flight response pins its
   bytes, gzip representation built on insert; eviction only frees unreferenced
   entries.
5. [x] File-backed responses: extend `PendingResponse` with a `body_fd` +
   `body_file_off` streaming form; `event_loop.c` sends it with nonblocking
   `sendfile()` on the same partial-write/`EAGAIN` path as `send_bytes()` and
   closes the descriptor on send-complete or connection close. Files larger than
   the per-file cache threshold stream identity-only; small files are cached and
   reuse the Phase 1 conditional/range/negotiation logic unchanged.
6. [x] Error semantics: `403` for hidden/symlink/`..`-escape policy refusals,
   `404` for missing files, `400` for a malformed encoding, `414` for an
   over-long target; directories without an index file are `404` (no listing).
7. [x] Tests: a `scripts/phase2_static_test.py` E2E harness with a temporary
   document root (traversal/symlink/`%00`/backslash corpus, hidden files,
   directory index, large-file partial write, slow reader, cache/fd budget
   check) that writes `benchmarks/production_phase2_static.json`; extend
   `tests/server_test.c` with doc-root and hidden-file E2E cases.
8. [x] Update `docs/architecture.md`, `docs/env-vars.md`, `docs/gotchas.md`,
   `readme.md`, the `AGENTS.md` files, and the `production-http-server`
   program-plan phase 2 checklist.

## Validation

- [x] `cmake -S . -B . && make` with no new warnings under `-Wall -Wextra
      -pedantic`.
- [x] `./bin/run_tests ring`.
- [x] `./bin/http_server & ... ./bin/run_tests server` per `AGENTS.md` (existing
      Phase 1 corpus plus the new doc-root cases).
- [x] `python3 scripts/phase2_static_test.py` (or `make phase2-static`) exits 0
      and writes the artifact.
- [x] `make lint` shows no new warnings in the changed sources.

## Exit criteria

- [x] The traversal corpus cannot read outside the document root; evidence:
      `benchmarks/production_phase2_static.json` `traversal_cases` all pass and
      the harness asserts no response body matches any sentinel outside the root.
- [x] Doc-root serving throughput is within 10% of the cached fixed-path
      baseline on the same host; evidence: the harness records both in
      `benchmarks/production_phase2_static.json`.
- [x] Cache bytes and open file descriptors stay within their configured budgets
      under sustained mixed-file load; evidence: the harness's `cache_bytes` <=
      `cache_budget_bytes` and `open_fds` <= configured bound.

## Risks and rollback

- **Path-resolution vulnerability** -> all resolution lives in one function,
  `path_resolver_open()`, uses `openat2(RESOLVE_BENEATH)`/`O_NOFOLLOW`, and is
  the sole merge gate via the traversal corpus. Rollback: stop serving doc-root
  paths (answer `404`) and keep the fixed-path aliases.
- **Cache use-after-free with in-flight responses** -> every response that
  borrows cache bytes pins the entry; eviction skips pinned entries and
  `file_cache_release()` runs in `pq_pop()`/`pq_release_all()`. Rollback: set
  the cache budget to zero (stream every file from an fd).
- **Heap-fd leak across disconnect** -> `body_fd` is closed in the same event
  loop paths as `owned_body`; the disconnect-burst test asserts a flat
  descriptor count. Rollback: never enable the streaming path (cache
  everything up to the budget).
- **Stricter resolution rejects legitimate files** -> every refusal has a
  documented status and a corpus entry; a regression is visible before merge.

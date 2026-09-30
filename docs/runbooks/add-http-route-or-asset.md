# Runbook: add an HTTP asset / route

Since Phase 2, most "routes" are just files under the document root. There is
no directory listing and no dynamic content.

## Add a static file (no code change)

1. Drop the file anywhere beneath `HTTP_SERVER_DOCUMENT_ROOT` (default `.`, the
   repository root).
2. Request it by its path, e.g. `GET /assets/app.css`. `Content-Type` comes
   from the builtin MIME table plus the optional `HTTP_SERVER_MIME_TYPES` file;
   add an extension there if it maps incorrectly.
3. Hidden (leading-dot) paths are `403` unless `HTTP_SERVER_HIDDEN_FILES=1`;
   directories serve an `index_files` entry, else `404`.

Files up to `CACHE_MAX_FILE_BYTES` are read into the bounded representation
cache on first request (plain + gzip). Larger files stream from the descriptor
with `sendfile` and are served identity-only.

## Add a fixed alias or change method/status semantics

Edit `el_prepare_response()` in `src/http_server.c`. The `/home` and `/hello`
aliases are examples: they select a `StaticAsset` and call
`select_asset_response(req, asset, content_type, force_close, pr)`. Keep the
response pointed at cached memory (or use `owned_body`/`body_fd` for owned
resources); the event loop must never `free()` a borrowed pointer (see
`docs/architecture.md` invariants).

## Change a status code or header

- Error statuses are produced by `emit_status()` / `format_simple()` and the
  `reason_phrase()` table in `src/http_server.c`; add a phrase next to the
  existing ones and return the status from the parser/selection path.
- `404`/`403` preserve keep-alive (`force_close = 0`); protocol errors and
  desynchronizing requests force close (`force_close = 1`). Match the
  existing convention and set `body_fd = -1` on every hand-built response.

## Security-sensitive paths

Never `open()` a request-derived path directly. All document-root resolution
goes through `path_resolver_open()` so decoding, normalization, and the
hidden/symlink policy are applied in one place.

## Verify

Extend `tests/server_test.c` with an E2E case that sends a real request and
asserts the status line / headers / body framing (see
`test_docroot_static_serving`). For traversal or policy changes, add a case to
`scripts/phase2_static_test.py` and run `make phase2-static`. Per `AGENTS.md`,
prefer E2E; do not add a unit test that just re-asserts a table.

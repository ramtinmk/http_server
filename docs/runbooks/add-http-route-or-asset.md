# Runbook: add or change an HTTP route / static asset

Routes are hard-coded in `el_prepare_response()` and served from **startup-cached
assets**. There is no directory listing and no dynamic content.

## Add a new static asset

1. Drop the file at the repository root (assets are opened by relative path),
   e.g. `about.html`.
2. In `src/http_server.c`:
   - Add a `static StaticAsset about_asset;` alongside `home_asset` /
     `hello_asset`.
   - In `initialize_static_responses()`, call
     `load_static_asset("about.html", &about_asset)` and, on failure, free what
     was already loaded and return `-1` (see the existing `hello.html` block).
   - In the route selection in `el_prepare_response()` add
     `strcmp(req.path, "/about") == 0` → `req.accepts_gzip ? &about_asset.gzip
     : &about_asset.plain`.
3. Regenerate and rebuild (`cmake -S . -B . && make`), then add an E2E case in
   `tests/server_test.c` and wire it into `run_server_tests()` with
   `RUN_TEST(...)`.

`load_static_asset()` reads, gzip-precompresses, and builds both keep-alive and
close header blocks automatically. Do not hand-roll a response.

## Change behavior of an existing route

Edit the selection block at `src/http_server.c:493`. Keep the response pointed at
cached memory — `PendingResponse` pointers must never be `free()`d by the event
loop (see `docs/architecture.md` invariants).

## Add a status code or header

- Parser-level errors use the `ERROR_TEMPLATE` macro at
  `src/http_server.c:24`; add a literal next to the existing 400/404/501/414/431
  constants and return it via `fill_static_response()`.
- `404` intentionally preserves keep-alive (`force_close = 0`); protocol errors
  force close (`force_close = 1`). Match the existing convention.

## Verify

Extend `tests/server_test.c` with an E2E case that sends a real request and
asserts the status line / headers / body framing. Per `AGENTS.md`, prefer E2E;
do not add a unit test that just re-asserts the route table.

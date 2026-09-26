# Runbook: change a configuration limit

All server limits live in `include/server_config.h` (plus `PORT`/`BACKLOG` in
`include/http_server.h`). Every one is `#ifndef`-guarded, so a compile-time
`-D` override wins over the header default.

## 1. Pick the default

Edit the `#define` in `include/server_config.h`. Keep the comment above it
accurate — these comments are the design rationale. If the limit has a validated
range or interacts with another (for example `MAX_ACTIVE_CONNECTIONS` vs
`REQUIRED_NOFILE_HEADROOM`), document that.

## 2. Override per build (without editing the header)

```bash
cmake -S . -B . -DCMAKE_C_FLAGS="-DEL_THREAD_COUNT=4 -DMAX_ACTIVE_CONNECTIONS=256"
make
```

## 3. Update the docs

- `docs/env-vars.md` — the compile-time table and the "overriding" section.
- `readme.md` — only if the user-visible behavior or benchmark envelope changes.

## 4. Rebuild and verify startup

```bash
cmake -S . -B . && make
./bin/http_server        # prints every resolved limit under "Server Configuration"
```

Confirm the printed value and the `Connection capacity:` line
(`operator_max`, `descriptor_cap`, `effective`, limiting factor). A capacity of
zero, or an effective `RLIMIT_NOFILE` below the requirement, is fatal by design.

## 5. Test the affected behavior

- Capacity/admission changes: `make saturation` (below/equal/above capacity).
- FD/preflight changes: `make startup-failfast` and start on a host with a low
  `ulimit -n` to confirm the clear failure.
- Timeout changes: the corresponding `tests/server_test.c` case
  (`*_timeout`, `test_keepalive_request_limit`).

Mark the relevant phase checkboxes in `plans/` if the limit came from a plan.

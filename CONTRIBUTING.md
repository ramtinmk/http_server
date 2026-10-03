# Contributing

Thanks for improving the C HTTP Server. This guide covers the workflow for
humans and agents; the authoritative project rules are in [`AGENTS.md`](AGENTS.md)
and [`docs/`](docs/).

## Before you start

- Read [`AGENTS.md`](AGENTS.md) (the router) and the relevant file under
  [`docs/`](docs/): `architecture.md` for module boundaries, `env-vars.md` for
  every limit, `gotchas.md` for sharp edges, and `docs/runbooks/` for recipes.
- Plans live in [`plans/`](plans/). Read [`plan-spec.md`](plan-spec.md) before
  editing a plan. Program plans list phases; each phase gets an
  `implementation` plan before work starts.

## Branches and commits

- Start each standalone feature, fix, refactor, or experiment on a topic branch
  off `main` (`feature/...`, `fix/...`, `refactor/...`, `experiment/...`).
- Small localized changes and documentation can go directly on the current
  branch.
- Never switch branches with unrelated uncommitted changes; never stash,
  discard, commit, or push without being asked.
- Keep commits focused and describe the behavior change, not just the files.

## Build

The project uses C11, POSIX threads, `zlib`, and OpenSSL (`libssl-dev`). Run all
commands from the repository root.

```bash
make                             # configures build/, then builds (Release, hardened)
# or, explicitly out-of-source:
cmake -S . -B build && cmake --build build
```

- Executables land in `bin/` (`bin/http_server`, `bin/run_tests`).
- After adding or removing a C file, add/remove it in the explicit source lists
  in [`CMakeLists.txt`](CMakeLists.txt) and re-run `cmake -S . -B build`. Removing a
  file that is still listed fails loudly at configure/link time.
- Use `-DCMAKE_BUILD_TYPE=Debug` for symbols; do not benchmark a Debug build.
- The build writes a `build-manifest.json` beside the build recording the
  toolchain and dependency versions.

## Test

Prefer end-to-end tests. The focused suites need no running server; the server
suite needs `./bin/http_server` running from the repository root.

```bash
./bin/run_tests ring
./bin/run_tests tls
ctest --output-on-failure            # with the server running for the server suite

./bin/http_server --document-root . &
SERVER_PID=$!
sleep 0.5
./bin/run_tests server
kill "$SERVER_PID" && wait "$SERVER_PID" 2>/dev/null || true
```

Acceptance harnesses for each phase write an artifact under `benchmarks/`; run
them with the matching `make` target (for example `make phase5-observability`,
`make phase4-hardening`, `make memory-soak`). See [`docs/runbooks/verify-changes.md`](docs/runbooks/verify-changes.md).

## Changing code

- Keep changes consistent with the surrounding structure, naming, and style.
- Update documentation in the same change: a new limit or environment variable
  goes in `docs/env-vars.md`; a new module responsibility or invariant goes in
  `docs/architecture.md` and the nearest `AGENTS.md`; a new sharp edge goes in
  `docs/gotchas.md`; a new repeatable task becomes a `docs/runbooks/` file.
- Tick the relevant phase checklist in `plans/` as work lands.
- Avoid new compiler warnings; the project builds with `-Wall -Wextra
  -pedantic` and lint uses `clang-tidy` (`make lint`).

## Reporting a bug

Include the exact command, the effective configuration, the server log, and the
artifact or minimal request that reproduces it. The `build-manifest.json` and
the startup log identify the exact build and resolved limits.

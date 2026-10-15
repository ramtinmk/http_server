# Cyclomatic complexity baseline

The pre-commit gate uses Lizard with a maximum cyclomatic complexity (CCN) of
15. The gate applies to C files changed by a commit or pull request. It is
deliberately stricter than the current repository-wide baseline, so touching a
complex function requires reducing its complexity or explicitly addressing the
design in review.

## Current findings

The existing full-tree audit reports 27 functions above CCN 15. The highest
findings are:

| Function | File | CCN | Main concern |
| --- | --- | ---: | --- |
| `apply_seccomp` | `src/sandbox.c` | 93 | One function encodes many syscall/filter cases; split policy construction from installation. |
| `read_http_response` | `tests/server_test.c` | 45 | Response framing, parsing, and validation are coupled; extract header/body parsing helpers. |
| `el_prepare_response` | `src/http_server.c` | 36 | Request/result policy and response assembly are combined; separate selection from serialization. |
| `privilege_drop` | `src/privilege.c` | 31 | Multiple platform and failure branches should become small capability-specific helpers. |
| `event_loop_main` | `src/event_loop.c` | 29 | Event polling, dispatch, timeout handling, and shutdown are in one control loop. |
| `event_loop_run` | `src/event_loop.c` | 28 | Startup, listener setup, and loop lifecycle should be separated. |
| `parse_request_line` | `src/http_server.c` | 28 | Token validation and semantic request checks are interleaved. |
| `parse_byte_ranges` | `src/http_server.c` | 32 | Range grammar, normalization, and bounds checks should be separate stages. |

Other findings are concentrated in TLS/event-loop I/O (`src/event_loop.c`),
request parsing and response formatting (`src/http_server.c`), startup
hardening (`src/main.c`, `src/sandbox.c`), path handling (`src/path_resolver.c`),
and the server E2E harness (`tests/server_test.c`).

## Recommended order

1. Refactor `src/sandbox.c:apply_seccomp`; it has the largest maintenance and
   review risk.
2. Split `src/http_server.c` parsing/response functions while preserving the
   existing E2E behavior.
3. Separate event-loop lifecycle, readiness, and TLS helpers without changing
   connection ownership or shutdown ordering.
4. Refactor test helpers independently; their complexity does not directly
   affect the server runtime but makes failures harder to diagnose.

Run the full audit with:

```bash
make complexity
```

The command is expected to report the baseline until these refactors land.
